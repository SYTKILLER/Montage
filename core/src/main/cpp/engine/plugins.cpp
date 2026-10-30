// M26 插件内核（P8）：纯逻辑半——manifest 校验/参数解析/band 规划/清洗/发布/注册表。
// 头文件注释见 plugins.h；JSVM 宿主在 plugins_jsvm.cpp。方案：docs/40-方案设计/14。

#include "engine/plugins.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include <hilog/log.h>

namespace montage {
namespace plugins {

namespace {

constexpr uint32_t kTile = kTileSize;
constexpr uint32_t kDomain = 0xD0026;
constexpr const char* kTag = "Montage.M26";

// ---- 迷你 JSON（用户参数串解析；manifest 走 JSVM 直读不经 JSON——方案 §6）----
struct JV {
    enum T { Null, Bool, Num, Str, Arr, Obj };
    T t = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JV> arr;
    std::vector<std::pair<std::string, JV>> obj;

    const JV* find(const char* key) const {
        for (const auto& kv : obj) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }
};

struct JsonLite {
    const char* p = nullptr;
    const char* end = nullptr;
    std::string err;
    int depth = 0;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            ++p;
        }
    }

    bool parse(JV& out) {
        ws();
        if (p >= end) {
            err = "empty json";
            return false;
        }
        if (++depth > 32) {
            err = "json too deep";
            return false;
        }
        const char c = *p;
        bool ok = false;
        if (c == '{') {
            ok = parseObj(out);
        } else if (c == '[') {
            ok = parseArr(out);
        } else if (c == '"') {
            ok = parseStr(out);
        } else if (c == 't' && end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
            out.t = JV::Bool;
            out.b = true;
            p += 4;
            ok = true;
        } else if (c == 'f' && end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
            out.t = JV::Bool;
            out.b = false;
            p += 5;
            ok = true;
        } else if (c == 'n' && end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
            out.t = JV::Null;
            p += 4;
            ok = true;
        } else {
            ok = parseNum(out);
        }
        --depth;
        return ok;
    }

    bool parseObj(JV& out) {
        out.t = JV::Obj;
        ++p;
        ws();
        if (p < end && *p == '}') {
            ++p;
            return true;
        }
        while (p < end) {
            JV key;
            ws();
            if (!parseStr(key)) {
                return false;
            }
            ws();
            if (p >= end || *p != ':') {
                err = "expected : in object";
                return false;
            }
            ++p;
            JV val;
            if (!parse(val)) {
                return false;
            }
            out.obj.emplace_back(key.str, std::move(val));
            ws();
            if (p < end && *p == ',') {
                ++p;
                continue;
            }
            if (p < end && *p == '}') {
                ++p;
                return true;
            }
            err = "expected , or } in object";
            return false;
        }
        err = "unterminated object";
        return false;
    }

    bool parseArr(JV& out) {
        out.t = JV::Arr;
        ++p;
        ws();
        if (p < end && *p == ']') {
            ++p;
            return true;
        }
        while (p < end) {
            JV val;
            if (!parse(val)) {
                return false;
            }
            out.arr.push_back(std::move(val));
            ws();
            if (p < end && *p == ',') {
                ++p;
                continue;
            }
            if (p < end && *p == ']') {
                ++p;
                return true;
            }
            err = "expected , or ] in array";
            return false;
        }
        err = "unterminated array";
        return false;
    }

    bool parseStr(JV& out) {
        if (p >= end || *p != '"') {
            err = "expected string";
            return false;
        }
        ++p;
        out.t = JV::Str;
        while (p < end) {
            const char c = *p;
            if (c == '"') {
                ++p;
                return true;
            }
            if (c == '\\') {
                ++p;
                if (p >= end) {
                    break;
                }
                switch (*p) {
                    case '"': out.str.push_back('"'); break;
                    case '\\': out.str.push_back('\\'); break;
                    case '/': out.str.push_back('/'); break;
                    case 'n': out.str.push_back('\n'); break;
                    case 't': out.str.push_back('\t'); break;
                    case 'r': out.str.push_back('\r'); break;
                    case 'b': out.str.push_back('\b'); break;
                    case 'f': out.str.push_back('\f'); break;
                    case 'u': {
                        // \uXXXX → UTF-8（BMP 内；代理对 v1 不合并，登记）
                        if (end - p < 5) {
                            err = "bad \\u escape";
                            return false;
                        }
                        unsigned cp = 0;
                        for (int i = 1; i <= 4; ++i) {
                            const char h = p[i];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') {
                                cp |= static_cast<unsigned>(h - '0');
                            } else if (h >= 'a' && h <= 'f') {
                                cp |= static_cast<unsigned>(h - 'a' + 10);
                            } else if (h >= 'A' && h <= 'F') {
                                cp |= static_cast<unsigned>(h - 'A' + 10);
                            } else {
                                err = "bad \\u escape";
                                return false;
                            }
                        }
                        p += 4;
                        if (cp < 0x80) {
                            out.str.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            out.str.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.str.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.str.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.str.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.str.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default:
                        err = "bad escape";
                        return false;
                }
                ++p;
                continue;
            }
            out.str.push_back(c);
            ++p;
        }
        err = "unterminated string";
        return false;
    }

    bool parseNum(JV& out) {
        char* e = nullptr;
        const double v = std::strtod(p, &e);
        if (e == p || e > end) {
            err = "bad number";
            return false;
        }
        out.t = JV::Num;
        out.num = v;
        p = e;
        return true;
    }
};

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool validKey(const std::string& k) {
    if (k.empty() || k.size() > 32) {
        return false;
    }
    for (const char c : k) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool printableName(const std::string& s) {
    if (s.empty() || s.size() > 192) {  // 字节上限 ≈ 64 个 3 字节 CJK 字符
        return false;
    }
    int chars = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
        if (c < 0x80) {
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            i += 4;
        } else {
            return false;
        }
        ++chars;
    }
    return chars <= 64;
}

}  // namespace

// ---- manifest 校验（photocraft Manifest::validate 同规则）----
bool validateManifest(const RawManifest& raw, Manifest& out, std::string& err) {
    out = Manifest{};
    if (!raw.complete) {
        err = "manifest: missing id/name/kind";
        return false;
    }
    if (raw.kind != "filter") {
        err = "manifest: kind must be \"filter\"";
        return false;
    }
    if (!validId(raw.id)) {
        err = "manifest: invalid id (use 1-64 of A-Z a-z 0-9 . _ -)";
        return false;
    }
    if (!printableName(raw.name)) {
        err = "manifest: name must be 1-64 printable characters";
        return false;
    }
    if (raw.version.size() > 32 || raw.description.size() > 1024 || raw.author.size() > 128) {
        err = "manifest: version/description/author too long";
        return false;
    }
    if (raw.area != "content" && raw.area != "canvas" && !raw.area.empty()) {
        err = "manifest: area must be \"content\" or \"canvas\"";
        return false;
    }
    if (raw.hasOverlap && (!std::isfinite(raw.overlap) || raw.overlap < 0 ||
                           raw.overlap > kMaxOverlap)) {
        err = "manifest: overlap exceeds " + std::to_string(kMaxOverlap);
        return false;
    }
    out.id = raw.id;
    out.name = raw.name;
    out.version = raw.version;
    out.description = raw.description;
    out.author = raw.author;
    out.canvasArea = raw.area == "canvas";
    out.overlap = raw.hasOverlap ? static_cast<uint32_t>(raw.overlap) : 0;
    if (raw.params.size() > kMaxParams) {
        err = "manifest: too many parameters (max " + std::to_string(kMaxParams) + ")";
        return false;
    }
    for (const auto& kv : raw.params) {
        const std::string& k = kv.first;
        const ParamSpec& s = kv.second;
        if (!validKey(k)) {
            err = "manifest: invalid parameter name \"" + k + "\"";
            return false;
        }
        if (s.type == ParamSpec::Number || s.type == ParamSpec::Int) {
            if (!std::isfinite(s.mn) || !std::isfinite(s.mx) || !std::isfinite(s.def) ||
                s.mn > s.mx) {
                err = "manifest: parameter \"" + k + "\": bad range";
                return false;
            }
        }
        if (s.type == ParamSpec::Choice) {
            if (s.options.empty() || s.options.size() > 64) {
                err = "manifest: parameter \"" + k + "\": options must be 1-64 entries";
                return false;
            }
            for (const std::string& o : s.options) {
                if (o.empty() || o.size() > 256) {
                    err = "manifest: parameter \"" + k + "\": bad option";
                    return false;
                }
            }
            if (!s.cdef.empty() &&
                std::find(s.options.begin(), s.options.end(), s.cdef) == s.options.end()) {
                err = "manifest: parameter \"" + k + "\": default not in options";
                return false;
            }
        }
        out.params.emplace_back(k, s);
    }
    return true;
}

// ---- 用户参数解析（photocraft resolve_params 同式）----
bool resolveParams(const Manifest& m, const std::string& userJson,
                   std::vector<std::pair<std::string, PValue>>& out, std::string& err) {
    out.clear();
    JV root;
    if (!userJson.empty()) {
        JsonLite jp;
        jp.p = userJson.data();
        jp.end = userJson.data() + userJson.size();
        if (!jp.parse(root)) {
            err = "params: " + (jp.err.empty() ? std::string("invalid json") : jp.err);
            return false;
        }
        if (root.t == JV::Null) {
            root.t = JV::Obj;
        } else if (root.t != JV::Obj) {
            err = "params: must be a JSON object";
            return false;
        }
    }
    for (const auto& kv : m.params) {
        const std::string& k = kv.first;
        const ParamSpec& spec = kv.second;
        const JV* given = root.find(k.c_str());
        PValue v;
        switch (spec.type) {
            case ParamSpec::Number: {
                double d = spec.def;
                if (given != nullptr) {
                    if (given->t != JV::Num || !std::isfinite(given->num)) {
                        err = "params: \"" + k + "\" must be a number";
                        return false;
                    }
                    d = given->num;
                }
                d = std::clamp(d, spec.mn, spec.mx);
                v.t = PValue::Num;
                v.num = d;
                break;
            }
            case ParamSpec::Int: {
                double d = spec.def;
                if (given != nullptr) {
                    if (given->t != JV::Num || !std::isfinite(given->num)) {
                        err = "params: \"" + k + "\" must be a number";
                        return false;
                    }
                    d = given->num;
                }
                d = std::clamp(std::round(d), spec.mn, spec.mx);
                v.t = PValue::Num;
                v.num = d;
                break;
            }
            case ParamSpec::Bool: {
                bool b = spec.bdef;
                if (given != nullptr) {
                    if (given->t != JV::Bool) {
                        err = "params: \"" + k + "\" must be true or false";
                        return false;
                    }
                    b = given->b;
                }
                v.t = PValue::Bool;
                v.b = b;
                break;
            }
            case ParamSpec::Choice: {
                std::string s = spec.cdef.empty() ? spec.options.front() : spec.cdef;
                if (given != nullptr) {
                    if (given->t != JV::Str) {
                        err = "params: \"" + k + "\" must be one of the options";
                        return false;
                    }
                    if (std::find(spec.options.begin(), spec.options.end(), given->str) ==
                        spec.options.end()) {
                        err = "params: \"" + k + "\" must be one of the options";
                        return false;
                    }
                    s = given->str;
                }
                v.t = PValue::Str;
                v.s = s;
                break;
            }
        }
        out.emplace_back(k, v);
    }
    return true;
}

// ---- band 几何 ----
IRect iIntersect(const IRect& a, const IRect& b) {
    return IRect{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1),
                 std::min(a.y1, b.y1)};
}

IRect iInflate(const IRect& r, int32_t m) {
    return IRect{r.x0 - m, r.y0 - m, r.x1 + m, r.y1 + m};
}

IRect iUnion(const IRect& a, const IRect& b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    return IRect{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1),
                 std::max(a.y1, b.y1)};
}

void planBands(const IRect& area, std::vector<IRect>& bands) {
    bands.clear();
    if (area.empty()) {
        return;
    }
    const int32_t t = static_cast<int32_t>(kTile);
    // tile 对齐块划分（photocraft TileCoord::containing 同式），行高 1 tile、宽 across tile
    const int32_t tx0 = area.x0 / t;
    const int32_t ty0 = area.y0 / t;
    const int32_t tx1 = (area.x1 - 1) / t;
    const int32_t ty1 = (area.y1 - 1) / t;
    for (int32_t ty = ty0; ty <= ty1; ++ty) {
        for (int32_t tx = tx0; tx <= tx1; tx += kBandTileAcross) {
            IRect band{tx * t, ty * t, std::min((tx + kBandTileAcross) * t, (tx1 + 1) * t),
                       (ty + 1) * t};
            bands.push_back(iIntersect(band, area));
        }
    }
}

IRect bandInput(const IRect& band, uint32_t overlap) {
    return iInflate(band, static_cast<int32_t>(overlap));
}

// ---- 读端（稀疏网格 + 边缘复制）----
namespace {

// 网格像素读（网格局部坐标；缺瓦片 = 透明；DeepEditGuard 之后的网格恒 U8）。
void gridPixel(const TileGrid& grid, int gx, int gy, uint8_t out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    if (gx < 0 || gy < 0) {
        return;
    }
    const uint32_t tx = static_cast<uint32_t>(gx) / kTile;
    const uint32_t ty = static_cast<uint32_t>(gy) / kTile;
    if (tx >= grid.cols || ty >= grid.rows) {
        return;
    }
    const auto it = grid.tiles.find(ty * grid.cols + tx);
    if (it == grid.tiles.end()) {
        return;
    }
    const uint32_t lx = static_cast<uint32_t>(gx) % kTile;
    const uint32_t ly = static_cast<uint32_t>(gy) % kTile;
    const uint8_t* base = it->second->pixels->mapCpu();
    const size_t rowBytes = it->second->pixels->rowBytes();
    std::memcpy(out, base + static_cast<size_t>(ly) * rowBytes + lx * 4u, 4);
}

}  // namespace

void readGridClamped(const TileGrid& grid, const IRect& inRect, const IRect& extent,
                     std::vector<uint8_t>& out) {
    const int w = inRect.width();
    const int h = inRect.height();
    if (w <= 0 || h <= 0) {
        out.clear();
        return;
    }
    out.assign(static_cast<size_t>(w) * h * 4u, 0);
    const IRect inner = iIntersect(inRect, extent);
    if (inner.empty()) {
        return;  // 全域外 = 全透明（photocraft read_clamped 同）
    }
    // inner 逐像素拷贝（网格局部坐标 = 文档坐标；网格原点恒 0——层网格像素空间即文档域）
    for (int y = inRect.y0; y < inRect.y1; ++y) {
        const int sy = std::clamp(y, inner.y0, inner.y1 - 1);
        uint8_t* dst = out.data() + static_cast<size_t>(y - inRect.y0) * w * 4u;
        for (int x = inRect.x0; x < inRect.x1; ++x) {
            const int sx = std::clamp(x, inner.x0, inner.x1 - 1);
            gridPixel(grid, sx, sy, dst + static_cast<size_t>(x - inRect.x0) * 4u);
        }
    }
}

float selGrayAt(const TileGrid* sel, int docX, int docY) {
    if (sel == nullptr) {
        return 1.0f;
    }
    return static_cast<float>(selectionGrayAt(*sel, docX, docY)) / 255.0f;
}

void finishBand(const float* px, const float* orig, int w, int h, const TileGrid* sel,
                int32_t selOx, int32_t selOy, const IRect& band, uint8_t* dst, int32_t dstW,
                int32_t dstOriginX, int32_t dstOriginY) {
    for (int y = 0; y < h; ++y) {
        const int docY = band.y0 + y;  // 网格局部 y
        uint8_t* row = dst + static_cast<size_t>(docY - dstOriginY) * dstW * 4u;
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
            // 选区掩码在文档域：层网格局部 → 文档坐标加层 origin（filter_ops SelSampler 同式）
            const float g = selGrayAt(sel, selOx + band.x0 + x, selOy + docY);
            uint8_t* o = row + static_cast<size_t>(band.x0 + x - dstOriginX) * 4u;
            for (int c = 0; c < 4; ++c) {
                float v = px[i + c];
                const float base = orig[i + c];
                if (!std::isfinite(v)) {
                    v = base;
                }
                v = std::clamp(v, 0.0f, 1.0f);
                const float outc = g >= 0.9995f ? v : (base + (v - base) * g);
                o[c] = static_cast<uint8_t>(std::lround(outc * 255.0f));
            }
        }
    }
}

std::shared_ptr<const TileGrid> publishAreaStrip(const TileGrid& orig, const IRect& area,
                                                 const uint8_t* strip, uint64_t revision) {
    auto out = std::make_shared<TileGrid>();
    out->cols = orig.cols;
    out->rows = orig.rows;
    out->depth = PixelDepth::U8;
    out->tiles = orig.tiles;  // COW：未触碰瓦片共享
    const int t = static_cast<int32_t>(kTile);
    const int tx0 = std::max(0, area.x0 / t);
    const int ty0 = std::max(0, area.y0 / t);
    const int tx1 = std::min<int32_t>(static_cast<int32_t>(orig.cols) - 1, (area.x1 - 1) / t);
    const int ty1 = std::min<int32_t>(static_cast<int32_t>(orig.rows) - 1, (area.y1 - 1) / t);
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            const uint32_t key = static_cast<uint32_t>(ty) * orig.cols + static_cast<uint32_t>(tx);
            const int px0 = tx * t;
            const int py0 = ty * t;
            // 该瓦片在 area 内的行带（strip 域坐标）
            const int ry0 = std::max(area.y0, py0) - area.y0;
            const int ry1 = std::min(area.y1, py0 + t) - area.y0;
            auto tile = std::make_shared<Tile>();
            auto ebuf = std::make_unique<EngineBuffer>(kTile, kTile);
            uint8_t* bytes = ebuf->mapCpuWrite();
            const auto it = orig.tiles.find(key);
            if (it != orig.tiles.end()) {
                const uint8_t* src = it->second->pixels->mapCpu();
                std::memcpy(bytes, src, static_cast<size_t>(kTile) * kTile * 4u);
            } else {
                std::memset(bytes, 0, static_cast<size_t>(kTile) * kTile * 4u);
            }
            for (int ry = ry0; ry < ry1; ++ry) {
                const int dy = py0 + ry + area.y0;  // 文档 y
                const int lx0 = std::max(area.x0, px0);
                const int lx1 = std::min(area.x1, px0 + t);
                if (lx1 <= lx0) {
                    continue;
                }
                // strip 行 = (ry, 文档 x − area.x0)
                std::memcpy(bytes + static_cast<size_t>(dy - py0) * kTile * 4u +
                                static_cast<size_t>(lx0 - px0) * 4u,
                            strip + (static_cast<size_t>(ry) * area.width() + (lx0 - area.x0)) * 4u,
                            static_cast<size_t>(lx1 - lx0) * 4u);
            }
            tile->pixels = std::move(ebuf);
            out->tiles[key] = std::move(tile);
        }
    }
    out->revision = revision;
    return out;
}

// ---- 注册表 ----
PluginRegistry& PluginRegistry::get() {
    static PluginRegistry inst;
    return inst;
}

size_t PluginRegistry::upsert(const PluginEntry& e) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& ex : entries_) {
        if (ex.man.id == e.man.id) {
            PluginEntry next = e;
            next.poisoned = false;  // 重注册（更新）重置毒标
            ex = std::move(next);
            return static_cast<size_t>(&ex - entries_.data());
        }
    }
    entries_.push_back(e);
    return entries_.size() - 1;
}

bool PluginRegistry::remove(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].man.id == id) {
            entries_.erase(entries_.begin() + static_cast<long>(i));
            return true;
        }
    }
    return false;
}

PluginEntry* PluginRegistry::find(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& e : entries_) {
        if (e.man.id == id) {
            return &e;
        }
    }
    return nullptr;
}

void PluginRegistry::poison(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& e : entries_) {
        if (e.man.id == id) {
            e.poisoned = true;
            OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag,
                         "plugin poisoned (deadline): %{public}s", e.man.id.c_str());
        }
    }
}

std::vector<PluginEntry> PluginRegistry::list() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return entries_;
}

size_t PluginRegistry::count() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return entries_.size();
}

}  // namespace plugins
}  // namespace montage
