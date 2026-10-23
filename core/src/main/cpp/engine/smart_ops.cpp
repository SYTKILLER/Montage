#include "engine/smart_ops.h"

// M21 智能对象渲染管线（photocraft engine/src/smart_cmds.rs C++ 翻译，MIT OR Apache-2.0）。
// 重活全部锁外：Engine 级源解码/placed 缓存独立互斥，不碰 docMutex（CaOp 异步范式配套）。

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "engine/blend_cpu.h"
#include "engine/filter_ops.h"
#include "engine/style_ops.h"
#include "engine/warp_ops.h"
#include "io/image_export.h"
#include "io/smart_source.h"
#include "io/png_decode.h"
#include "io/project_open.h"
#include "io/project_save.h"
#include "io/psd/psd_import.h"
#include "io/psd/psd_reader.h"

namespace montage {
namespace smart {

namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
// 单源/单放置 20MP 预算（引擎网格预算同族；超限报错防 OOM）
constexpr uint64_t kMaxSmartPixels = 20000000ull;
// 嵌套源解码深度护栏（SO 内 SO 的 .montage 递归；对抗自引用包）
constexpr int kMaxDecodeDepth = 8;
// 会话缓存字节预算（源解码 + placed 两级合计；LRU 逐出）
constexpr size_t kCacheBudget = 512ull * 1024 * 1024;

inline void fnvMix(uint64_t& h, uint64_t v) {
    h ^= v;
    h *= kFnvPrime;
}

inline uint64_t fnvBytes(const uint8_t* p, size_t n) {
    uint64_t h = kFnvOffset;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
    return h;
}

inline uint64_t fnvDoubles(const double* v, size_t n) {
    uint64_t h = kFnvOffset;
    for (size_t i = 0; i < n; ++i) {
        uint64_t bits = 0;
        std::memcpy(&bits, &v[i], sizeof(bits));
        fnvMix(h, bits);
    }
    return h;
}

inline uint64_t fnvFloats(const float* v, size_t n) {
    uint64_t h = kFnvOffset;
    for (size_t i = 0; i < n; ++i) {
        uint64_t bits = 0;
        std::memcpy(&bits, &v[i], sizeof(bits));
        fnvMix(h, bits);
    }
    return h;
}

// ---------- 会话缓存（源解码 + placed；contentsId 键 + hash 校验防撞号错拿） ----------

struct SourceEntry {
    uint64_t hash = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint8_t> rgba;  // w*h*4 直 alpha
    uint64_t lastUse = 0;
    size_t bytes() const { return rgba.size(); }
};

struct PlacedEntry {
    uint64_t sourceHash = 0;
    uint64_t transformHash = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    double originX = 0.0;
    double originY = 0.0;
    std::vector<uint8_t> rgba;
    uint64_t lastUse = 0;
    size_t bytes() const { return rgba.size(); }
};

std::mutex g_cacheMtx;
std::unordered_map<uint64_t, SourceEntry> g_sourceCache;
std::unordered_map<uint64_t, PlacedEntry> g_placedCache;
uint64_t g_useTick = 0;
size_t g_cacheTotal = 0;

void touchAndEvict() {
    while (g_cacheTotal > kCacheBudget) {
        uint64_t bestKey = 0;
        uint64_t bestUse = UINT64_MAX;
        int which = -1;
        for (const auto& [k, e] : g_sourceCache) {
            if (e.lastUse < bestUse) {
                bestUse = e.lastUse;
                bestKey = k;
                which = 0;
            }
        }
        for (const auto& [k, e] : g_placedCache) {
            if (e.lastUse < bestUse) {
                bestUse = e.lastUse;
                bestKey = k;
                which = 1;
            }
        }
        if (which < 0) {
            return;
        }
        if (which == 0) {
            g_cacheTotal -= g_sourceCache[bestKey].bytes();
            g_sourceCache.erase(bestKey);
        } else {
            g_cacheTotal -= g_placedCache[bestKey].bytes();
            g_placedCache.erase(bestKey);
        }
    }
}

std::string baseName(const std::string& path) {
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string lowerExt(const std::string& fileName) {
    const size_t dot = fileName.rfind('.');
    if (dot == std::string::npos || dot + 1 >= fileName.size()) {
        return "";
    }
    std::string ext = fileName.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

thread_local int g_decodeDepth = 0;

// 子文档 → 扁平合成（源域全幅 strip；restyle 后 BuildFlattenJob/FlattenVisible）
bool flattenSubDocument(Document& sub, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba,
                        std::string& err) {
    restyleDocument(sub);
    w = sub.width;
    h = sub.height;
    if (w == 0 || h == 0) {
        err = "smart source document is empty";
        return false;
    }
    if (static_cast<uint64_t>(w) * h > kMaxSmartPixels) {
        err = "smart source too large";
        return false;
    }
    io::FlattenJob job = io::BuildFlattenJob(sub, 0, 0, 0, 0);
    io::FlattenResult res = io::FlattenVisible(job, 0, 0, static_cast<int>(w), static_cast<int>(h));
    if (!res.ok) {
        err = res.error.empty() ? "smart source flatten failed" : res.error;
        return false;
    }
    rgba = std::move(res.rgba);
    return true;
}

// 子文档层平移（shiftLayerOrigins 逐层；组子层同在平铺序内）
void shiftStack(std::vector<Layer>& layers, double dx, double dy) {
    for (Layer& l : layers) {
        shiftLayerOrigins(l, dx, dy);
    }
}

// strip 墨迹精确包围盒（alpha > 0）
bool stripInkBBox(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, uint32_t& x,
                  uint32_t& y, uint32_t& bw, uint32_t& bh) {
    uint32_t x0 = UINT32_MAX, y0 = UINT32_MAX, x1 = 0, y1 = 0;
    for (uint32_t ry = 0; ry < h; ++ry) {
        const uint8_t* row = rgba.data() + static_cast<size_t>(ry) * w * 4u;
        for (uint32_t rx = 0; rx < w; ++rx) {
            if (row[rx * 4u + 3u] != 0) {
                x0 = std::min(x0, rx);
                y0 = std::min(y0, ry);
                x1 = std::max(x1, rx);
                y1 = std::max(y1, ry);
            }
        }
    }
    if (x0 == UINT32_MAX) {
        return false;
    }
    x = x0;
    y = y0;
    bw = x1 - x0 + 1u;
    bh = y1 - y0 + 1u;
    return true;
}

// 源缓存取数（contentsId 键 + sourceHash 校验；出参 sourceHash 供 placed 缓存键）
bool getSourceFlat(const SmartLayerState& sm, uint64_t& sourceHashOut, uint32_t& w, uint32_t& h,
                   std::vector<uint8_t>& rgba, std::string& err) {
    std::vector<uint8_t> bytes;
    std::string name;
    if (sm.embedded) {
        bytes = sm.sourceBytes;
        name = sm.fileName;
    } else {
        // 链接源：读文件（鸿蒙沙箱限制 → 仅沙箱可见路径可用，登记）
        if (sm.linkedPath.empty()) {
            err = "linked smart object has no path";
            return false;
        }
        FILE* f = std::fopen(sm.linkedPath.c_str(), "rb");
        if (f == nullptr) {
            err = "linked file unavailable: " + sm.linkedPath;
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz <= 0 || static_cast<uint64_t>(sz) > 512ull * 1024 * 1024) {
            std::fclose(f);
            err = "linked file unreadable";
            return false;
        }
        bytes.resize(static_cast<size_t>(sz));
        const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        if (got != bytes.size()) {
            err = "linked file read failed";
            return false;
        }
        name = baseName(sm.linkedPath);
    }
    if (bytes.empty()) {
        err = "smart object has no source bytes";
        return false;
    }
    const uint64_t hash = fnvBytes(bytes.data(), bytes.size());
    sourceHashOut = hash;
    {
        std::lock_guard<std::mutex> lk(g_cacheMtx);
        auto it = g_sourceCache.find(sm.contentsId);
        if (it != g_sourceCache.end() && it->second.hash == hash) {
            it->second.lastUse = ++g_useTick;
            w = it->second.w;
            h = it->second.h;
            rgba = it->second.rgba;
            return true;
        }
    }
    if (g_decodeDepth >= kMaxDecodeDepth) {
        err = "smart source nesting too deep";
        return false;
    }
    ++g_decodeDepth;
    uint32_t dw = 0;
    uint32_t dh = 0;
    std::vector<uint8_t> flat;
    const bool ok = decodeSourceFlat(name, bytes, dw, dh, flat, err);
    --g_decodeDepth;
    if (!ok) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    SourceEntry& e = g_sourceCache[sm.contentsId];
    e.hash = hash;
    e.w = dw;
    e.h = dh;
    e.rgba = flat;  // 保留一份进缓存
    e.lastUse = ++g_useTick;
    g_cacheTotal += e.bytes();
    touchAndEvict();
    w = dw;
    h = dh;
    rgba = e.rgba;
    return true;
}

// placed 缓存查取（源 hash + transform 指纹）
bool getCachedPlaced(const SmartLayerState& sm, uint64_t sourceHash, uint32_t& w, uint32_t& h,
                     double& ox, double& oy, std::vector<uint8_t>& rgba) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    auto it = g_placedCache.find(sm.contentsId);
    if (it == g_placedCache.end() || it->second.sourceHash != sourceHash ||
        it->second.transformHash != fnvDoubles(sm.m, 6)) {
        return false;
    }
    it->second.lastUse = ++g_useTick;
    w = it->second.w;
    h = it->second.h;
    ox = it->second.originX;
    oy = it->second.originY;
    rgba = it->second.rgba;
    return true;
}

void putCachedPlaced(const SmartLayerState& sm, uint64_t sourceHash, uint32_t w, uint32_t h,
                     double ox, double oy, const std::vector<uint8_t>& rgba) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    PlacedEntry& e = g_placedCache[sm.contentsId];
    g_cacheTotal -= e.bytes();
    e.sourceHash = sourceHash;
    e.transformHash = fnvDoubles(sm.m, 6);
    e.w = w;
    e.h = h;
    e.originX = ox;
    e.originY = oy;
    e.rgba = rgba;
    e.lastUse = ++g_useTick;
    g_cacheTotal += e.bytes();
    touchAndEvict();
}

// 放置：源 strip → 文档域（纯整数平移快路径；一般仿射走 warpMeshImg 双三次+收缩预滤波）
bool placeSource(const SmartLayerState& sm, const uint8_t* src, uint32_t sw, uint32_t sh,
                 uint32_t& w, uint32_t& h, double& ox, double& oy, std::vector<uint8_t>& out,
                 std::string& err) {
    const double a = sm.m[0];
    const double b = sm.m[1];
    const double c = sm.m[2];
    const double d = sm.m[3];
    const double e = sm.m[4];
    const double f = sm.m[5];
    const bool identityScale = std::fabs(a - 1.0) < 1e-9 && std::fabs(b) < 1e-9 &&
                               std::fabs(d) < 1e-9 && std::fabs(e - 1.0) < 1e-9;
    if (identityScale && std::fabs(c - std::lround(c)) < 1e-9 &&
        std::fabs(f - std::lround(f)) < 1e-9) {
        // 平移快路径：整 strip 直拷（尺寸不变）
        w = sw;
        h = sh;
        ox = static_cast<double>(std::lround(c));
        oy = static_cast<double>(std::lround(f));
        out.assign(static_cast<size_t>(sw) * sh * 4u, 0);
        std::memcpy(out.data(), src, static_cast<size_t>(sw) * sh * 4u);
        return true;
    }
    // 一般仿射：float Img → warpMeshImg（M20 渲染器，Bicubic + Jacobian 收缩子采样）
    warp::Img img;
    img.w = static_cast<int>(sw);
    img.h = static_cast<int>(sh);
    img.px.resize(static_cast<size_t>(sw) * sh * 4u);
    for (size_t i = 0; i < img.px.size(); ++i) {
        img.px[i] = static_cast<float>(src[i]) / 255.0f;
    }
    const inpaint::RectI srcRect{0, 0, static_cast<int>(sw), static_cast<int>(sh)};
    auto map = [a, b, c, d, e, f](double x, double y) {
        return std::array<double, 2>{a * x + b * y + c, d * x + e * y + f};
    };
    const warp::TriRender r = warp::warpMeshImg(img, srcRect, map, warp::Interp::Bicubic);
    if (r.out.w <= 0 || r.out.h <= 0) {
        w = 0;
        h = 0;
        ox = r.originX;
        oy = r.originY;
        out.clear();
        return true;
    }
    if (static_cast<uint64_t>(r.out.w) * static_cast<uint64_t>(r.out.h) > kMaxSmartPixels) {
        err = "smart placement too large";
        return false;
    }
    w = static_cast<uint32_t>(r.out.w);
    h = static_cast<uint32_t>(r.out.h);
    ox = r.originX;
    oy = r.originY;
    out.resize(static_cast<size_t>(w) * h * 4u);
    for (size_t i = 0; i + 3 < r.out.px.size(); i += 4) {
        for (int ch = 0; ch < 4; ++ch) {
            float v = r.out.px[i + ch];
            v = std::min(1.0f, std::max(0.0f, v));
            out[i + ch] = static_cast<uint8_t>(std::lround(v * 255.0f));
        }
    }
    return true;
}

// 单智能滤镜：applyFilter → blend/opacity 直混（W3C composite）
bool applyOneFilter(std::vector<uint8_t>& cur, uint32_t w, uint32_t h, const SmartFilter& sf,
                    std::string& err) {
    filters::FilterParams p = sf.params;
    p.kind = sf.kind;
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4u, 0);
    if (!filters::applyFilter(cur.data(), out.data(), w, h, p, err)) {
        return false;
    }
    if (sf.blend != BlendMode::Normal || sf.opacity < 1.0) {
        const float op = static_cast<float>(std::min(1.0, std::max(0.0, sf.opacity)));
        const size_t n = static_cast<size_t>(w) * h * 4u;
        for (size_t i = 0; i + 3 < n; i += 4) {
            const float cb[4] = {cur[i] / 255.0f, cur[i + 1] / 255.0f, cur[i + 2] / 255.0f,
                                 cur[i + 3] / 255.0f};
            const float cs[4] = {out[i] / 255.0f, out[i + 1] / 255.0f, out[i + 2] / 255.0f,
                                 out[i + 3] / 255.0f};
            float o[4];
            blend_cpu::composite(sf.blend, cb, cs, op, o);
            cur[i] = static_cast<uint8_t>(std::lround(std::min(1.0f, std::max(0.0f, o[0])) * 255.0f));
            cur[i + 1] = static_cast<uint8_t>(std::lround(std::min(1.0f, std::max(0.0f, o[1])) * 255.0f));
            cur[i + 2] = static_cast<uint8_t>(std::lround(std::min(1.0f, std::max(0.0f, o[2])) * 255.0f));
            cur[i + 3] = static_cast<uint8_t>(std::lround(std::min(1.0f, std::max(0.0f, o[3])) * 255.0f));
        }
    } else {
        cur = std::move(out);
    }
    return true;
}

// 滤镜蒙版 mix（premul 域凸组合 → 直 alpha；photocraft mask_mix 同构）
void maskMix(const std::vector<uint8_t>& placed, const std::vector<uint8_t>& filtered, uint32_t w,
             uint32_t h, const LayerMask& mask, std::vector<uint8_t>& out) {
    const size_t n = static_cast<size_t>(w) * h * 4u;
    out.resize(n);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
            const float g =
                static_cast<float>(layerMaskGrayAt(mask, static_cast<int>(x), static_cast<int>(y))) /
                255.0f;
            const float ab = placed[i + 3] / 255.0f;
            const float af = filtered[i + 3] / 255.0f;
            const float oa = ab + (af - ab) * g;
            out[i + 3] = static_cast<uint8_t>(std::lround(std::min(1.0f, oa) * 255.0f));
            for (int ch = 0; ch < 3; ++ch) {
                const float cb = placed[i + ch] / 255.0f;
                const float cf = filtered[i + ch] / 255.0f;
                const float pm = cb * ab + (cf * af - cb * ab) * g;
                out[i + ch] =
                    oa > 0.0f
                        ? static_cast<uint8_t>(std::lround(
                              std::min(1.0f, std::max(0.0f, pm / oa)) * 255.0f))
                        : 0;
            }
        }
    }
}

void refreshMaskComposite(Layer& l) {
    if (l.smart == nullptr) {
        return;
    }
    if (l.mask != nullptr && l.mask->enabled && l.smartCache != nullptr) {
        l.render = composeMasked(l.smartCache, *l.mask);
    } else {
        l.render = nullptr;
    }
}

}  // namespace

namespace {

// contentsId 进程内单调（nextContentsId / bumpContentsSeqAbove 共享）
std::atomic<uint64_t> g_contentsSeq{1};

}  // namespace

uint64_t nextContentsId() {
    return g_contentsSeq.fetch_add(1);
}

void bumpContentsSeqAbove(uint64_t v) {
    uint64_t cur = g_contentsSeq.load(std::memory_order_relaxed);
    while (cur <= v && !g_contentsSeq.compare_exchange_weak(cur, v + 1, std::memory_order_relaxed)) {
    }
}

uint64_t fingerprint(const SmartLayerState& sm) {
    uint64_t h = kFnvOffset;
    fnvMix(h, sm.sourceHash != 0 ? sm.sourceHash
                                 : fnvBytes(sm.sourceBytes.data(), sm.sourceBytes.size()));
    fnvMix(h, fnvDoubles(sm.m, 6));
    fnvMix(h, sm.filtersEnabled ? 0x5B : 0x2C);
    for (const SmartFilter& f : sm.filters) {
        fnvMix(h, static_cast<uint64_t>(f.kind));
        fnvMix(h, fnvBytes(reinterpret_cast<const uint8_t*>(f.psdClass.data()), f.psdClass.size()));
        fnvMix(h, static_cast<uint64_t>(f.blend));
        double op = f.opacity;
        fnvMix(h, fnvDoubles(&op, 1));
        fnvMix(h, f.visible ? 0x1F : 0x63);
        const double pv[6] = {f.params.p0, f.params.p1, f.params.p2, f.params.p3, f.params.p4,
                              f.params.p5};
        fnvMix(h, fnvDoubles(pv, 6));
        fnvMix(h, f.params.seed);
        fnvMix(h, fnvFloats(f.params.kernel, 25));
        fnvMix(h, fnvFloats(f.params.colorFg, 3));
        fnvMix(h, fnvFloats(f.params.colorBg, 3));
        double sc = f.params.scale;
        double of = f.params.offset;
        fnvMix(h, fnvDoubles(&sc, 1));
        fnvMix(h, fnvDoubles(&of, 1));
        fnvMix(h, fnvBytes(reinterpret_cast<const uint8_t*>(f.psdHex.data()), f.psdHex.size()));
    }
    fnvMix(h, sm.filters.size());
    if (sm.filterMask != nullptr) {
        fnvMix(h, 0xFACADEull);
        fnvMix(h, sm.filterMask->pixels != nullptr ? sm.filterMask->pixels->revision : 0);
        fnvMix(h, sm.filterMask->enabled ? 1 : 0);
        fnvMix(h, static_cast<uint64_t>(sm.filterMask->offsetX));
        fnvMix(h, static_cast<uint64_t>(sm.filterMask->offsetY));
        fnvMix(h, sm.filterMask->outside);
    }
    return h;
}

bool decodeSourceFlat(const std::string& fileName, const std::vector<uint8_t>& bytes,
                      uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba, std::string& err) {
    const std::string ext = lowerExt(fileName);
    if (ext == "montage") {
        Document sub;
        size_t layerCount = 0;
        if (!io::openProjectBytes(bytes, sub, &layerCount, err)) {
            return false;
        }
        return flattenSubDocument(sub, w, h, rgba, err);
    }
    if (ext == "psd" || ext == "psb") {
        psd::PsdDocument pd;
        if (!psd::readPsd(bytes.data(), bytes.size(), pd, err)) {
            return false;
        }
        Document sub;
        sub.width = static_cast<uint32_t>(pd.width);
        sub.height = static_cast<uint32_t>(pd.height);
        sub.name = baseName(fileName);
        uint64_t subId = 0;
        std::vector<Layer> layers;
        if (!io::assemblePsdLayers(pd, [&subId]() { return ++subId; }, nullptr, layers, nullptr,
                                   err)) {
            return false;
        }
        sub.layers = std::move(layers);
        return flattenSubDocument(sub, w, h, rgba, err);
    }
    if (ext == "png") {
        std::shared_ptr<const TileGrid> grid = io::decodePngToGrid(bytes.data(), bytes.size(), err);
        if (grid == nullptr) {
            return false;
        }
        // 源域 = 网格全域（256 对齐；透明 padding 由墨迹裁剪/放置域自然消化）
        w = grid->cols * kTileSize;
        h = grid->rows * kTileSize;
        if (static_cast<uint64_t>(w) * h > kMaxSmartPixels) {
            err = "smart source too large";
            return false;
        }
        rgba.assign(static_cast<size_t>(w) * h * 4u, 0);
        for (const auto& [key, tile] : grid->tiles) {
            const uint32_t tx = key % grid->cols;
            const uint32_t ty = key / grid->cols;
            const uint8_t* src = tile->pixels->mapCpu();
            const uint32_t stride = tile->pixels->rowBytes();
            for (uint32_t r = 0; r < kTileSize; ++r) {
                std::memcpy(rgba.data() +
                                (static_cast<size_t>(ty) * kTileSize + r) * w * 4u +
                                static_cast<size_t>(tx) * kTileSize * 4u,
                            src + static_cast<size_t>(r) * stride,
                            static_cast<size_t>(kTileSize) * 4u);
            }
            tile->pixels->unmap();
        }
        return true;
    }
    // 其余栅格格式（JPEG/BMP/WebP/HEIC…）：系统解码器单趟（mimeType 驱动 swapRB——M16a 纪律）
    return io::decodeRasterSource(fileName, bytes, w, h, rgba, err);
}

bool renderLayer(Layer& l, std::string& err) {
    if (l.smart == nullptr) {
        return false;
    }
    const SmartLayerState& sm = *l.smart;
    const uint64_t fp = fingerprint(sm);
    if (fp == l.smartCacheHash && l.smartCache != nullptr) {
        refreshMaskComposite(l);
        return true;
    }
    uint64_t srcHash = 0;
    uint32_t sw = 0;
    uint32_t sh = 0;
    std::vector<uint8_t> src;
    if (!getSourceFlat(sm, srcHash, sw, sh, src, err)) {
        return false;
    }
    // placed（缓存查取 → placeSource）
    uint32_t pw = 0;
    uint32_t ph = 0;
    double ox = 0.0;
    double oy = 0.0;
    std::vector<uint8_t> placed;
    if (!getCachedPlaced(sm, srcHash, pw, ph, ox, oy, placed)) {
        if (!placeSource(sm, src.data(), sw, sh, pw, ph, ox, oy, placed, err)) {
            return false;
        }
        putCachedPlaced(sm, srcHash, pw, ph, ox, oy, placed);
    }
    if (pw == 0 || ph == 0) {
        l.smartCache = nullptr;
        l.smartCacheHash = fp;
        refreshMaskComposite(l);
        return true;
    }
    // 滤镜蒙版 mix 的未滤镜基准（滤镜链前拷贝；无蒙版零拷贝）
    std::vector<uint8_t> unfiltered;
    const bool needMix = sm.filterMask != nullptr && sm.filterMask->enabled;
    if (needMix) {
        unfiltered = placed;
    }
    // 滤镜链
    std::vector<uint8_t> cur = std::move(placed);
    if (sm.filtersEnabled) {
        for (const SmartFilter& f : sm.filters) {
            if (!f.visible || f.kind < 0) {
                continue;  // 未实现滤镜（PSD 保 descriptor）直通
            }
            if (!applyOneFilter(cur, pw, ph, f, err)) {
                return false;
            }
        }
    }
    if (needMix) {
        std::vector<uint8_t> mixed;
        maskMix(unfiltered, cur, pw, ph, *sm.filterMask, mixed);
        cur = std::move(mixed);
    }
    // 发布（origin 回填 = 放置域原点；蒙版合成跟随）
    l.smartCache = filterStripToGrid(cur, pw, ph, nextGridRevision());
    l.transform.originX = ox;
    l.transform.originY = oy;
    l.transform.width = static_cast<double>(pw);
    l.transform.height = static_cast<double>(ph);
    l.smartCacheHash = fp;
    refreshMaskComposite(l);
    return true;
}

void renderDocument(Document& doc) {
    std::string err;
    for (Layer& l : doc.layers) {
        if (l.smart != nullptr) {
            renderLayer(l, err);
        }
    }
}

bool layersToSmart(const std::string& name, LayerId newId, std::vector<Layer>& members,
                   Layer& out, std::string& err) {
    if (members.empty()) {
        err = "no layers to convert";
        return false;
    }
    // 1) 成员外观 doc 域包围盒并集（effective 外观；含样式/形状/蒙版/嵌套 SO 派生）
    bool any = false;
    double rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
    for (const Layer& l : members) {
        const std::shared_ptr<const TileGrid> eff = l.effectivePixels();
        if (eff == nullptr) {
            continue;
        }
        uint32_t ix = 0, iy = 0, iw = 0, ih = 0;
        if (!gridInkBBox(*eff, ix, iy, iw, ih)) {
            continue;
        }
        const double x0 = l.effectiveOriginX() + static_cast<double>(ix);
        const double y0 = l.effectiveOriginY() + static_cast<double>(iy);
        const double x1 = x0 + static_cast<double>(iw);
        const double y1 = y0 + static_cast<double>(ih);
        if (!any) {
            rx0 = x0;
            ry0 = y0;
            rx1 = x1;
            ry1 = y1;
            any = true;
        } else {
            rx0 = std::min(rx0, x0);
            ry0 = std::min(ry0, y0);
            rx1 = std::max(rx1, x1);
            ry1 = std::max(ry1, y1);
        }
    }
    if (!any || rx1 - rx0 < 1.0 || ry1 - ry0 < 1.0) {
        err = "the layer is empty, so there is nothing to convert";
        return false;
    }
    // 2) 子文档：成员平移进 [0,0] 域（16px 余量）
    constexpr double kMargin = 16.0;
    const double regionX = rx0 - kMargin;
    const double regionY = ry0 - kMargin;
    Document sub;
    sub.name = name;
    sub.width = static_cast<uint32_t>(std::ceil(rx1 - rx0)) + 32u;
    sub.height = static_cast<uint32_t>(std::ceil(ry1 - ry0)) + 32u;
    sub.layers = std::move(members);
    shiftStack(sub.layers, -regionX, -regionY);
    // 3) 派生重建 + 扁平合成（子文档全幅）
    uint32_t fw = 0;
    uint32_t fh = 0;
    std::vector<uint8_t> flat;
    if (!flattenSubDocument(sub, fw, fh, flat, err)) {
        return false;
    }
    // 4) 合成墨迹包围盒 → 裁剪 + 二次平移（源域 = 墨迹精确，photocraft crop_surface 同语义）
    uint32_t bx = 0, by = 0, bw = 0, bh = 0;
    if (!stripInkBBox(flat, fw, fh, bx, by, bw, bh)) {
        err = "the layer is empty, so there is nothing to convert";
        return false;
    }
    std::vector<uint8_t> cropped(static_cast<size_t>(bw) * bh * 4u, 0);
    for (uint32_t r = 0; r < bh; ++r) {
        std::memcpy(cropped.data() + static_cast<size_t>(r) * bw * 4u,
                    flat.data() + (static_cast<size_t>(by + r) * fw + bx) * 4u,
                    static_cast<size_t>(bw) * 4u);
    }
    shiftStack(sub.layers, -static_cast<double>(bx), -static_cast<double>(by));
    sub.width = bw;
    sub.height = bh;
    // 5) 子文档 → .montage 内存包（嵌入源）
    std::vector<uint8_t> bytes;
    if (!io::saveProjectBytes(sub, bytes, err)) {
        return false;
    }
    // 6) SO 层组装：cache = 裁剪后合成（外观不变）；m = 文档平移
    out = Layer{};
    out.id = newId;
    out.name = name;
    const double ox = regionX + static_cast<double>(bx);
    const double oy = regionY + static_cast<double>(by);
    auto sm = std::make_shared<SmartLayerState>();
    sm->contentsId = nextContentsId();
    sm->embedded = true;
    sm->fileName = name + ".montage";
    sm->sourceBytes = std::move(bytes);
    sm->sourceHash = fnvBytes(sm->sourceBytes.data(), sm->sourceBytes.size());
    sm->m[0] = 1.0;
    sm->m[1] = 0.0;
    sm->m[2] = ox;
    sm->m[3] = 0.0;
    sm->m[4] = 1.0;
    sm->m[5] = oy;
    out.smart = std::move(sm);
    out.smartCache = filterStripToGrid(cropped, bw, bh, nextGridRevision());
    out.smartCacheHash = fingerprint(*out.smart);
    out.transform.originX = ox;
    out.transform.originY = oy;
    out.transform.width = static_cast<double>(bw);
    out.transform.height = static_cast<double>(bh);
    return true;
}

bool layerToSmart(const Layer& l, LayerId newId, Layer& out, std::string& err) {
    const std::string name = l.name;
    std::vector<Layer> members;
    members.push_back(l);  // 拷贝（成员状态由 SO 层保留：可见/不透明/混合/剪贴在 wrapper 上）
    if (!layersToSmart(name, newId, members, out, err)) {
        return false;
    }
    // photocraft layer_to_smart：单层转换保留外观态（可见性/不透明度/混合/剪贴留 SO 层）
    out.visible = l.visible;
    out.opacity = l.opacity;
    out.blendMode = l.blendMode;
    out.clipping = l.clipping;
    return true;
}

void clearSessionCaches() {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    g_sourceCache.clear();
    g_placedCache.clear();
    g_cacheTotal = 0;
}

size_t cacheBytes() {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    return g_cacheTotal;
}

}  // namespace smart
}  // namespace montage
