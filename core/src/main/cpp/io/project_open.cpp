#include "io/project_open.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>
#include <zlib.h>

#include <hilog/log.h>

#include "engine/document.h"
#include "engine/style_ops.h"
#include "engine/engine.h"
#include "io/png_decode.h"

namespace montage {
namespace io {
namespace {

constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Open";
constexpr const char* kFormatId = "com.sytkiller.montage.project";
constexpr int kMinVersion = 1;
constexpr int kMaxVersion = 11;  // v11：+等高线/Range/图案叠加/文档图案（M17d，可选字段读兼容 v1–v10）；v10：+图层样式效果扩展（M17a，可选字段读兼容 v1–v9）；v9：+参考线 guides 块（M15a）；v8：+图层样式/填充图层（M14c）；v7：+图层组（M14a）；v6：+文本层（M12）；v5：+Alpha 通道；v4：调整层扩充；v3：+调整层（按字段存在性兼容旧版）
constexpr uint32_t kMaxSide = 30000;
constexpr uint64_t kMaxTotalPixels = 100ull * 1000 * 1000;
constexpr size_t kMaxLayers = 10000;
constexpr size_t kMaxManifestBytes = 4 * 1024 * 1024;
constexpr size_t kMaxPngBytes = 512ull * 1024 * 1024;  // 对齐上游单资产上限

// ---------- 最小 zip 读取（EOCD + central directory；只读 STORE/DEFLATE，无 zip64） ----------

class ZipReader {
  public:
    struct CdEntry {
        uint16_t method = 0;
        uint32_t crc = 0;
        uint64_t csize = 0;
        uint64_t usize = 0;
        uint64_t dataOffset = 0;
    };

    bool open(const std::vector<uint8_t>& data) {
        data_ = data.data();
        size_ = data.size();
        // EOCD：从尾部 64KB 内回扫签名
        if (size_ < 22) {
            return false;
        }
        const uint64_t scanEnd = std::min<uint64_t>(size_, 22 + 65536);
        uint64_t eocd = 0;
        bool found = false;
        for (uint64_t i = size_ - 22 + 1; i-- >= size_ - scanEnd + 1;) {
            if (readU32(i) == 0x06054b50u) {
                eocd = i;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
        const uint16_t count = readU16(eocd + 10);
        const uint64_t cdSize = readU32(eocd + 12);
        const uint64_t cdOffset = readU32(eocd + 16);
        if (cdOffset + cdSize > size_) {
            return false;
        }
        uint64_t p = cdOffset;
        for (uint16_t i = 0; i < count; ++i) {
            if (p + 46 > size_ || readU32(p) != 0x02014b50u) {
                return false;
            }
            CdEntry e;
            e.method = readU16(p + 10);
            e.crc = readU32(p + 16);
            e.csize = readU32(p + 20);
            e.usize = readU32(p + 24);
            const uint16_t nameLen = readU16(p + 28);
            const uint64_t localOffset = readU32(p + 42);
            if (p + 46 + nameLen > size_) {
                return false;
            }
            std::string name(reinterpret_cast<const char*>(data_ + p + 46), nameLen);
            // 数据区起点：本地头 30B + 文件名 + 额外字段（以本地头自身长度为准）
            if (localOffset + 30 > size_ || readU32(localOffset) != 0x04034b50u) {
                return false;
            }
            const uint16_t lNameLen = readU16(localOffset + 26);
            const uint16_t lExtraLen = readU16(localOffset + 28);
            e.dataOffset = localOffset + 30u + lNameLen + lExtraLen;
            if (e.dataOffset + e.csize > size_) {
                return false;
            }
            entries_[std::move(name)] = e;
            p += 46u + nameLen + readU16(p + 30) + readU16(p + 32) + readU16(p + 34);
        }
        return true;
    }

    const CdEntry* find(const std::string& name) const {
        auto it = entries_.find(name);
        return it == entries_.end() ? nullptr : &it->second;
    }

    // 解出条目并校验 CRC（防损坏静默入库）
    bool extract(const CdEntry& e, std::vector<uint8_t>& out, std::string& err) const {
        if (e.usize > kMaxPngBytes) {
            err = "entry too large";
            return false;
        }
        out.resize(static_cast<size_t>(e.usize));
        if (e.method == 0) {
            if (e.csize != e.usize) {
                err = "store size mismatch";
                return false;
            }
            std::memcpy(out.data(), data_ + e.dataOffset, static_cast<size_t>(e.usize));
        } else if (e.method == 8) {
            z_stream zs {};
            if (inflateInit2(&zs, -15) != Z_OK) {
                err = "inflate init failed";
                return false;
            }
            zs.next_in = const_cast<Bytef*>(data_ + e.dataOffset);
            zs.avail_in = static_cast<uInt>(e.csize);
            zs.next_out = out.data();
            zs.avail_out = static_cast<uInt>(e.usize);
            const int rc = inflate(&zs, Z_FINISH);
            const bool ok = rc == Z_STREAM_END && zs.avail_out == 0;
            inflateEnd(&zs);
            if (!ok) {
                err = "inflate failed";
                return false;
            }
        } else {
            err = "unsupported zip method";
            return false;
        }
        if (crc32(0L, out.data(), static_cast<uInt>(e.usize)) != e.crc) {
            err = "entry crc mismatch";
            return false;
        }
        return true;
    }

  private:
    uint16_t readU16(uint64_t at) const {
        return static_cast<uint16_t>(data_[at]) |
               static_cast<uint16_t>(static_cast<uint16_t>(data_[at + 1]) << 8);
    }

    uint32_t readU32(uint64_t at) const {
        return static_cast<uint32_t>(data_[at]) |
               static_cast<uint32_t>(static_cast<uint32_t>(data_[at + 1]) << 8) |
               static_cast<uint32_t>(static_cast<uint32_t>(data_[at + 2]) << 16) |
               static_cast<uint32_t>(static_cast<uint32_t>(data_[at + 3]) << 24);
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    std::unordered_map<std::string, CdEntry> entries_;
};

// ---------- 最小 JSON 解析（04 manifest 固定 schema；严格收尾，字符串含 \u 代理对） ----------

struct JValue {
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    Type type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JValue> arr;
    std::vector<std::pair<std::string, JValue>> obj;  // 保序小对象，线性查找

    const JValue* find(const char* key) const {
        for (const auto& [k, v] : obj) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
};

class JsonParser {
  public:
    JsonParser(const uint8_t* p, size_t n) : p_(p), n_(n) {}

    bool parse(JValue& out, std::string& err) {
        skipWs();
        if (!parseValue(out, 0)) {
            err = msg_.empty() ? "json syntax error" : msg_;
            return false;
        }
        skipWs();
        if (pos_ != n_) {
            err = "json trailing data";
            return false;
        }
        return true;
    }

  private:
    static constexpr int kMaxDepth = 32;

    void skipWs() {
        while (pos_ < n_) {
            const char c = static_cast<char>(p_[pos_]);
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool fail(const std::string& m) {
        msg_ = m;
        return false;
    }

    bool parseValue(JValue& v, int depth) {
        if (depth > kMaxDepth) {
            return fail("json too deep");
        }
        if (pos_ >= n_) {
            return fail("json truncated");
        }
        const char c = static_cast<char>(p_[pos_]);
        switch (c) {
            case '{': return parseObj(v, depth);
            case '[': return parseArr(v, depth);
            case '"': v.type = JValue::Str;
                return parseString(v.str);
            case 't':
                if (n_ - pos_ >= 4 && std::memcmp(p_ + pos_, "true", 4) == 0) {
                    pos_ += 4;
                    v.type = JValue::Bool;
                    v.b = true;
                    return true;
                }
                return fail("bad literal");
            case 'f':
                if (n_ - pos_ >= 5 && std::memcmp(p_ + pos_, "false", 5) == 0) {
                    pos_ += 5;
                    v.type = JValue::Bool;
                    v.b = false;
                    return true;
                }
                return fail("bad literal");
            case 'n':
                if (n_ - pos_ >= 4 && std::memcmp(p_ + pos_, "null", 4) == 0) {
                    pos_ += 4;
                    v.type = JValue::Null;
                    return true;
                }
                return fail("bad literal");
            default:
                if (c == '-' || (c >= '0' && c <= '9')) {
                    return parseNumber(v);
                }
                return fail("unexpected char");
        }
    }

    bool parseObj(JValue& v, int depth) {
        v.type = JValue::Obj;
        ++pos_;  // {
        skipWs();
        if (pos_ < n_ && p_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skipWs();
            if (pos_ >= n_ || p_[pos_] != '"') {
                return fail("object key expected");
            }
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            skipWs();
            if (pos_ >= n_ || p_[pos_] != ':') {
                return fail("colon expected");
            }
            ++pos_;
            skipWs();
            JValue item;
            if (!parseValue(item, depth + 1)) {
                return false;
            }
            v.obj.emplace_back(std::move(key), std::move(item));
            skipWs();
            if (pos_ < n_ && p_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < n_ && p_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("comma or brace expected");
        }
    }

    bool parseArr(JValue& v, int depth) {
        v.type = JValue::Arr;
        ++pos_;  // [
        skipWs();
        if (pos_ < n_ && p_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            skipWs();
            JValue item;
            if (!parseValue(item, depth + 1)) {
                return false;
            }
            v.arr.push_back(std::move(item));
            skipWs();
            if (pos_ < n_ && p_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < n_ && p_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("comma or bracket expected");
        }
    }

    void appendUtf8(unsigned int cp, std::string& s) {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(unsigned int& out) {
        if (n_ - pos_ < 4) {
            return fail("bad \\u escape");
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = static_cast<char>(p_[pos_ + i]);
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<unsigned int>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<unsigned int>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<unsigned int>(c - 'A' + 10);
            } else {
                return fail("bad \\u escape");
            }
        }
        pos_ += 4;
        return true;
    }

    bool parseString(std::string& out) {
        ++pos_;  // "
        out.clear();
        for (;;) {
            if (pos_ >= n_) {
                return fail("unterminated string");
            }
            const uint8_t c = p_[pos_];
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= n_) {
                    return fail("unterminated escape");
                }
                const char e = static_cast<char>(p_[pos_++]);
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned int cp = 0;
                        if (!hex4(cp)) {
                            return false;
                        }
                        if (cp >= 0xD800 && cp <= 0xDBFF && n_ - pos_ >= 6 && p_[pos_] == '\\' &&
                            p_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            unsigned int lo = 0;
                            if (!hex4(lo)) {
                                return false;
                            }
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                appendUtf8(0xFFFD, out);  // 代理对残缺：替换符兜底
                                cp = 0xFFFD;
                            }
                        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                            cp = 0xFFFD;
                        }
                        appendUtf8(cp, out);
                        break;
                    }
                    default: return fail("bad escape");
                }
                continue;
            }
            if (c < 0x20) {
                return fail("raw control char");
            }
            out.push_back(static_cast<char>(c));
            ++pos_;
        }
    }

    bool parseNumber(JValue& v) {
        const size_t start = pos_;
        if (pos_ < n_ && p_[pos_] == '-') {
            ++pos_;
        }
        while (pos_ < n_ && p_[pos_] >= '0' && p_[pos_] <= '9') {
            ++pos_;
        }
        if (pos_ < n_ && p_[pos_] == '.') {
            ++pos_;
            while (pos_ < n_ && p_[pos_] >= '0' && p_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (pos_ < n_ && (p_[pos_] == 'e' || p_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < n_ && (p_[pos_] == '+' || p_[pos_] == '-')) {
                ++pos_;
            }
            while (pos_ < n_ && p_[pos_] >= '0' && p_[pos_] <= '9') {
                ++pos_;
            }
        }
        char buf[64];
        const size_t len = pos_ - start;
        if (len == 0 || len >= sizeof(buf)) {
            return fail("bad number");
        }
        std::memcpy(buf, p_ + start, len);
        buf[len] = '\0';
        char* end = nullptr;
        v.type = JValue::Num;
        v.num = std::strtod(buf, &end);
        if (end != buf + len || !std::isfinite(v.num)) {
            return fail("bad number");
        }
        return true;
    }

    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    std::string msg_;
};

// ---------- manifest 读取与校验（04 §1.2 规则自持） ----------

struct LayerRecord {
    LayerId id = 0;
    std::string name;
    bool visible = true;
    double opacity = 1.0;
    BlendMode blendMode = BlendMode::Normal;
    double originX = 0.0;
    double originY = 0.0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool hasImage = false;
    // v2 蒙版
    bool hasMask = false;
    bool maskEnabled = true;
    int maskOffsetX = 0;
    int maskOffsetY = 0;
    int maskOutside = 255;
    uint32_t maskWidth = 0;
    uint32_t maskHeight = 0;
    bool clipping = false;  // v2
    std::shared_ptr<LayerAdjustment> adjustment;  // v3
    std::shared_ptr<TextLayerState> text;  // v6（M12）
    std::shared_ptr<LayerStyle> style;  // v8（M14c）
    std::shared_ptr<FillLayerState> fill;  // v8（M14c）
    // v7（M14a）
    bool isGroup = false;
    uint64_t parentId = 0;
    bool collapsed = false;
};

bool getU64(const JValue& v, uint64_t& out) {
    if (v.type != JValue::Num || v.num < 0 || v.num >= 1e15 || v.num != std::floor(v.num)) {
        return false;
    }
    out = static_cast<uint64_t>(v.num);
    return true;
}

bool getU32(const JValue& v, uint32_t& out) {
    uint64_t wide = 0;
    if (!getU64(v, wide) || wide > 0xFFFFFFFFull) {
        return false;
    }
    out = static_cast<uint32_t>(wide);
    return true;
}

bool getDouble(const JValue& v, double& out) {
    if (v.type != JValue::Num) {
        return false;
    }
    out = v.num;
    return true;
}

bool parseManifest(const std::vector<uint8_t>& bytes, Document& doc, std::vector<LayerRecord>& layers,
                   std::vector<std::string>& channelFiles,
                   std::vector<DocumentPattern>& patterns,
                   std::string& err) {
    if (bytes.size() > kMaxManifestBytes) {
        err = "manifest too large";
        return false;
    }
    JValue root;
    {
        JsonParser parser(bytes.data(), bytes.size());
        if (!parser.parse(root, err) || root.type != JValue::Obj) {
            if (err.empty()) {
                err = "manifest not an object";
            }
            return false;
        }
    }
    // 头三门（对齐上游 readPackage：format/version 独立校验，错误可辨）
    const JValue* fmt = root.find("format");
    const JValue* ver = root.find("version");
    if (fmt == nullptr || fmt->type != JValue::Str || fmt->str != kFormatId) {
        err = "not a montage project";
        return false;
    }
    int version = 0;
    if (ver == nullptr || ver->type != JValue::Num || ver->num != std::floor(ver->num) ||
        ver->num < kMinVersion || ver->num > kMaxVersion) {
        err = "unsupported project version";
        return false;
    }
    version = static_cast<int>(ver->num);
    const JValue* cs = root.find("colorSpace");
    if (cs == nullptr || cs->type != JValue::Str || cs->str != "sRGB") {
        err = "unsupported color space";
        return false;
    }
    const JValue* jw = root.find("width");
    const JValue* jh = root.find("height");
    if (jw == nullptr || jh == nullptr || !getU32(*jw, doc.width) || !getU32(*jh, doc.height) ||
        doc.width == 0 || doc.height == 0 || doc.width > kMaxSide || doc.height > kMaxSide) {
        err = "document size exceeds format limit";
        return false;
    }
    const JValue* jname = root.find("name");
    if (jname != nullptr && jname->type == JValue::Str) {
        doc.name = jname->str;
    }
    const JValue* jlayers = root.find("layers");
    if (jlayers == nullptr || jlayers->type != JValue::Arr ||
        jlayers->arr.size() > kMaxLayers) {
        err = "layer count exceeds format limit";
        return false;
    }

    uint64_t totalPixels = 0;
    std::unordered_map<uint64_t, bool> ids;
    for (const JValue& item : jlayers->arr) {
        if (item.type != JValue::Obj) {
            err = "bad layer record";
            return false;
        }
        LayerRecord l;
        uint64_t id = 0;
        const JValue* jid = item.find("id");
        if (jid == nullptr || !getU64(*jid, id) || id == 0 || ids.count(id) != 0) {
            err = "bad or duplicate layer id";
            return false;
        }
        ids[id] = true;
        l.id = static_cast<LayerId>(id);
        const JValue* jn = item.find("name");
        if (jn == nullptr || jn->type != JValue::Str ||
            jn->str.find_first_not_of(" \t\r\n") == std::string::npos || jn->str.size() > 16384) {
            err = "bad layer name";
            return false;
        }
        l.name = jn->str;
        const JValue* jvis = item.find("visible");
        if (jvis != nullptr) {
            if (jvis->type != JValue::Bool) {
                err = "bad visible";
                return false;
            }
            l.visible = jvis->b;
        }
        const JValue* jop = item.find("opacity");
        if (jop != nullptr) {
            if (!getDouble(*jop, l.opacity) || !std::isfinite(l.opacity) || l.opacity < 0.0 ||
                l.opacity > 1.0) {
                err = "bad layer opacity";
                return false;
            }
        }
        const JValue* jcl = item.find("clipping");
        if (jcl != nullptr) {
            if (jcl->type != JValue::Bool) {
                err = "bad clipping";
                return false;
            }
            l.clipping = jcl->b;
        }
        const JValue* jbm = item.find("blendMode");
        if (jbm == nullptr || jbm->type != JValue::Str ||
            !blendModeFromName(jbm->str, l.blendMode)) {
            err = "unknown blend mode";
            return false;
        }
        const JValue* jimg = item.find("imageFile");
        if (jimg != nullptr && jimg->type == JValue::Str) {
            // 文件名与 id 强绑定（对齐上游 validate），杜绝路径注入
            if (jimg->str != "images/" + std::to_string(l.id) + ".png") {
                err = "imageFile mismatch";
                return false;
            }
            l.hasImage = true;
            const JValue* jlwidth = item.find("width");
            const JValue* jlheight = item.find("height");
            if (jlwidth == nullptr || jlheight == nullptr || !getU32(*jlwidth, l.width) ||
                !getU32(*jlheight, l.height) || l.width == 0 || l.height == 0 ||
                l.width > kMaxSide || l.height > kMaxSide) {
                err = "bad layer size";
                return false;
            }
            totalPixels += static_cast<uint64_t>(l.width) * l.height;
            const JValue* jox = item.find("originX");
            const JValue* joy = item.find("originY");
            if (jox == nullptr || joy == nullptr || !getDouble(*jox, l.originX) ||
                !getDouble(*joy, l.originY) || !std::isfinite(l.originX) || !std::isfinite(l.originY)) {
                err = "bad layer origin";
                return false;
            }
        }
        // v2 蒙版字段（v1 无 → 无蒙版）
        const JValue* jmf = item.find("maskFile");
        if (jmf != nullptr && jmf->type == JValue::Str) {
            const std::string expect = "images/" + std::to_string(l.id) + ".mask.png";
            if (jmf->str != expect) {
                err = "maskFile mismatch";
                return false;
            }
            l.hasMask = true;
            const JValue* jme = item.find("maskEnabled");
            if (jme != nullptr) {
                if (jme->type != JValue::Bool) {
                    err = "bad maskEnabled";
                    return false;
                }
                l.maskEnabled = jme->b;
            }
            const JValue* jmo = item.find("maskOutside");
            if (jmo != nullptr) {
                uint32_t mo = 255;
                if (!getU32(*jmo, mo) || mo > 255) {
                    err = "bad maskOutside";
                    return false;
                }
                l.maskOutside = static_cast<int>(mo);
            }
            const JValue* jmox = item.find("maskOffsetX");
            const JValue* jmoy = item.find("maskOffsetY");
            double ox = 0, oy = 0;
            if (jmox == nullptr || jmoy == nullptr || !getDouble(*jmox, ox) || !getDouble(*jmoy, oy) ||
                !std::isfinite(ox) || !std::isfinite(oy) || ox < -1e9 || ox > 1e9 || oy < -1e9 ||
                oy > 1e9) {
                err = "bad mask offset";
                return false;
            }
            l.maskOffsetX = static_cast<int>(ox);
            l.maskOffsetY = static_cast<int>(oy);
            const JValue* jmw = item.find("maskWidth");
            const JValue* jmh = item.find("maskHeight");
            if (jmw == nullptr || jmh == nullptr || !getU32(*jmw, l.maskWidth) ||
                !getU32(*jmh, l.maskHeight) || l.maskWidth == 0 || l.maskHeight == 0 ||
                l.maskWidth > kMaxSide || l.maskHeight > kMaxSide) {
                err = "bad mask size";
                return false;
            }
            totalPixels += static_cast<uint64_t>(l.maskWidth) * l.maskHeight;
        }
        // v3 调整层（v1/v2 无此字段 → 普通层）
        const JValue* jadj = item.find("adjustment");
        if (jadj != nullptr && jadj->type == JValue::Obj) {
            auto adj = std::make_shared<LayerAdjustment>();
            const JValue* jkind = jadj->find("kind");
            if (jkind != nullptr && jkind->type == JValue::Str) {
                if (jkind->str == "bc") {
                    adj->kind = AdjustmentKind::BrightnessContrast;
                } else if (jkind->str == "invert") {
                    adj->kind = AdjustmentKind::Invert;
                } else if (jkind->str == "threshold") {
                    adj->kind = AdjustmentKind::Threshold;
                } else if (jkind->str == "posterize") {
                    adj->kind = AdjustmentKind::Posterize;
                } else if (jkind->str == "colorbalance") {
                    adj->kind = AdjustmentKind::ColorBalance;
                } else if (jkind->str == "huesat") {
                    adj->kind = AdjustmentKind::HueSaturation;
                }
            }
            if (jkind != nullptr && jkind->type == JValue::Str && jkind->str == "curves") {
                adj->kind = AdjustmentKind::Curves;
                const JValue* jcx = jadj->find("curveX");
                const JValue* jcy = jadj->find("curveY");
                if (jcx != nullptr && jcy != nullptr && jcx->type == JValue::Arr &&
                    jcy->type == JValue::Arr && jcx->arr.size() == jcy->arr.size() &&
                    jcx->arr.size() >= 2 && jcx->arr.size() <= 64) {
                    for (size_t c = 0; c < jcx->arr.size(); ++c) {
                        double vx = 0, vy = 0;
                        if (!getDouble(jcx->arr[c], vx) || !getDouble(jcy->arr[c], vy) ||
                            !std::isfinite(vx) || !std::isfinite(vy) || vx < 0 || vx > 1 ||
                            vy < -4 || vy > 4) {
                            err = "bad curve point";
                            return false;
                        }
                        adj->curveX.push_back(static_cast<float>(vx));
                        adj->curveY.push_back(static_cast<float>(vy));
                    }
                }
            } else if (jkind == nullptr || jkind->type != JValue::Str || jkind->str != "levels") {
                err = "unknown adjustment kind";
                return false;
            }
            const JValue* jbl = jadj->find("inBlack");
            const JValue* jwl = jadj->find("inWhite");
            const JValue* jgm = jadj->find("gamma");
            double bl = 0, wl = 1, gm = 1;
            if (jbl == nullptr || jwl == nullptr || jgm == nullptr ||
                !getDouble(*jbl, bl) || !getDouble(*jwl, wl) || !getDouble(*jgm, gm) ||
                !std::isfinite(bl) || !std::isfinite(wl) || !std::isfinite(gm) || bl < 0 ||
                bl > 1 || wl < 0 || wl > 1 || gm < 0.1 || gm > 10) {
                err = "bad adjustment params";
                return false;
            }
            adj->inBlack = static_cast<float>(bl);
            adj->inWhite = static_cast<float>(wl);
            adj->gamma = static_cast<float>(gm);
            const JValue* jp0 = jadj->find("p0");
            const JValue* jp1 = jadj->find("p1");
            const JValue* jp2 = jadj->find("p2");
            const JValue* jp3 = jadj->find("p3");
            const JValue* jp[4] = {jp0, jp1, jp2, jp3};
            float* pf[4] = {&adj->p0, &adj->p1, &adj->p2, &adj->p3};
            for (int pi = 0; pi < 4; ++pi) {
                if (jp[pi] != nullptr) {
                    double pv = 0;
                    if (!getDouble(*jp[pi], pv) || !std::isfinite(pv) || pv < -1000 || pv > 1000) {
                        err = "bad adjustment param slot";
                        return false;
                    }
                    *pf[pi] = static_cast<float>(pv);
                }
            }
            l.adjustment = std::move(adj);
        }
        // v6 文本层（可选字段；旧版无此字段 = 普通层）。styleHash 置 0：首编辑触发重栅格。
        const JValue* jtx = item.find("text");
        if (jtx != nullptr && jtx->type == JValue::Obj) {
            auto t = std::make_shared<TextLayerState>();
            const JValue* jcontent = jtx->find("content");
            if (jcontent == nullptr || jcontent->type != JValue::Str) {
                err = "bad text content";
                return false;
            }
            if (jcontent->str.size() > 1u * 1024 * 1024) {
                err = "text content too long";
                return false;
            }
            t->content = jcontent->str;
            const JValue* jfam = jtx->find("fontFamily");
            if (jfam != nullptr && jfam->type == JValue::Str) {
                if (jfam->str.size() > 256) {
                    err = "text fontFamily too long";
                    return false;
                }
                t->fontFamily = jfam->str;
            }
            const JValue* jfs = jtx->find("fontSize");
            double fs = 40.0;
            if (jfs != nullptr && (!getDouble(*jfs, fs) || !std::isfinite(fs) || fs < 1 || fs > 2000)) {
                err = "bad text fontSize";
                return false;
            }
            t->fontSize = fs;
            const JValue* jlh = jtx->find("lineHeight");
            double lh = 1.2;
            if (jlh != nullptr && (!getDouble(*jlh, lh) || !std::isfinite(lh) || lh < 0.1 || lh > 10)) {
                err = "bad text lineHeight";
                return false;
            }
            t->lineHeight = lh;
            const JValue* jtr = jtx->find("tracking");
            double tr = 0;
            if (jtr != nullptr &&
                (!getDouble(*jtr, tr) || !std::isfinite(tr) || tr < -2000 || tr > 2000)) {
                err = "bad text tracking";
                return false;
            }
            t->tracking = tr;
            const JValue* jbold = jtx->find("bold");
            if (jbold != nullptr) {
                if (jbold->type != JValue::Bool) {
                    err = "bad text bold";
                    return false;
                }
                t->bold = jbold->b;
            }
            const JValue* jitalic = jtx->find("italic");
            if (jitalic != nullptr) {
                if (jitalic->type != JValue::Bool) {
                    err = "bad text italic";
                    return false;
                }
                t->italic = jitalic->b;
            }
            const JValue* jcolor = jtx->find("color");
            if (jcolor != nullptr) {
                if (jcolor->type != JValue::Arr || jcolor->arr.size() != 3) {
                    err = "bad text color";
                    return false;
                }
                for (int ci = 0; ci < 3; ++ci) {
                    double cv = 0;
                    if (!getDouble(jcolor->arr[ci], cv) || !std::isfinite(cv) || cv < 0 || cv > 1) {
                        err = "bad text color";
                        return false;
                    }
                    t->color[ci] = static_cast<float>(cv);
                }
            }
            const JValue* jalign = jtx->find("align");
            double al = 0;
            if (jalign != nullptr &&
                (!getDouble(*jalign, al) || al != std::floor(al) || al < 0 || al > 2)) {
                err = "bad text align";
                return false;
            }
            t->align = static_cast<int32_t>(al);
            const JValue* jwrap = jtx->find("wrapWidth");
            double ww = 0;
            if (jwrap != nullptr &&
                (!getDouble(*jwrap, ww) || !std::isfinite(ww) || ww < 0 || ww > 1e7)) {
                err = "bad text wrapWidth";
                return false;
            }
            t->wrapWidth = ww;
            l.text = std::move(t);
        }
        // v7 组字段（可选；旧版无此字段 = 普通根层）
        const JValue* jig = item.find("isGroup");
        if (jig != nullptr) {
            if (jig->type != JValue::Bool) {
                err = "bad isGroup";
                return false;
            }
            l.isGroup = jig->b;
        }
        const JValue* jpg = item.find("parentId");
        if (jpg != nullptr) {
            uint64_t pid = 0;
            if (!getU64(*jpg, pid)) {
                err = "bad parentId";
                return false;
            }
            l.parentId = pid;
        }
        const JValue* jcol = item.find("collapsed");
        if (jcol != nullptr) {
            if (jcol->type != JValue::Bool) {
                err = "bad collapsed";
                return false;
            }
            l.collapsed = jcol->b;
        }
        // v8 图层样式（M14c，可选字段——旧版无此字段 = 无样式）
        const JValue* jst = item.find("style");
        if (jst != nullptr) {
            if (jst->type != JValue::Obj) {
                err = "bad style";
                return false;
            }
            auto st = std::make_shared<LayerStyle>();
            const JValue* je = jst->find("enabled");
            if (je != nullptr && je->type == JValue::Bool) {
                st->enabled = je->b;
            }
            const JValue* jds = jst->find("dropShadow");
            if (jds != nullptr && jds->type == JValue::Bool && jds->b) {
                double ang = 0, dist = 0, size = 0, op = 0;
                const JValue* jn = jst->find("shAngle");
                if (jn == nullptr || !getDouble(*jn, ang)) { err = "bad shAngle"; return false; }
                jn = jst->find("shDist");
                if (jn == nullptr || !getDouble(*jn, dist)) { err = "bad shDist"; return false; }
                jn = jst->find("shSize");
                if (jn == nullptr || !getDouble(*jn, size)) { err = "bad shSize"; return false; }
                jn = jst->find("shOpacity");
                if (jn == nullptr || !getDouble(*jn, op)) { err = "bad shOpacity"; return false; }
                const JValue* jc = jst->find("shColor");
                if (jc == nullptr || jc->type != JValue::Arr || jc->arr.size() != 3) {
                    err = "bad shColor";
                    return false;
                }
                for (size_t ci = 0; ci < 3; ++ci) {
                    double cv = 0;
                    if (!getDouble(jc->arr[ci], cv) || !std::isfinite(cv) || cv < 0 || cv > 1) {
                        err = "bad shColor value";
                        return false;
                    }
                    st->shadowColor[ci] = static_cast<float>(cv);
                }
                st->dropShadow = true;
                st->shadowAngleDeg = ang;
                st->shadowDistance = dist;
                st->shadowSize = size;
                st->shadowOpacity = std::clamp(op, 0.0, 1.0);
                st->shadowColor[3] = 1.0f;
            }
            const JValue* jsk = jst->find("stroke");
            if (jsk != nullptr && jsk->type == JValue::Bool && jsk->b) {
                double w = 0;
                const JValue* jn = jst->find("stWidth");
                if (jn == nullptr || !getDouble(*jn, w)) { err = "bad stWidth"; return false; }
                const JValue* jc = jst->find("stColor");
                if (jc == nullptr || jc->type != JValue::Arr || jc->arr.size() != 3) {
                    err = "bad stColor";
                    return false;
                }
                for (size_t ci = 0; ci < 3; ++ci) {
                    double cv = 0;
                    if (!getDouble(jc->arr[ci], cv) || !std::isfinite(cv) || cv < 0 || cv > 1) {
                        err = "bad stColor value";
                        return false;
                    }
                    st->strokeColor[ci] = static_cast<float>(cv);
                }
                st->stroke = true;
                st->strokeWidth = w;
                st->strokeColor[3] = 1.0f;
            }
            const JValue* jco = jst->find("colorOverlay");
            if (jco != nullptr && jco->type == JValue::Bool && jco->b) {
                double op = 0;
                const JValue* jc = jst->find("ovColor");
                if (jc == nullptr || jc->type != JValue::Arr || jc->arr.size() != 3) {
                    err = "bad ovColor";
                    return false;
                }
                for (size_t ci = 0; ci < 3; ++ci) {
                    double cv = 0;
                    if (!getDouble(jc->arr[ci], cv) || !std::isfinite(cv) || cv < 0 || cv > 1) {
                        err = "bad ovColor value";
                        return false;
                    }
                    st->overlayColor[ci] = static_cast<float>(cv);
                }
                const JValue* jn = jst->find("ovOpacity");
                if (jn == nullptr || !getDouble(*jn, op)) { err = "bad ovOpacity"; return false; }
                st->colorOverlay = true;
                st->overlayColor[3] = 1.0f;
                st->overlayOpacity = std::clamp(op, 0.0, 1.0);
            }
            // v10 效果扩展（M17a，可选字段；缺省 = 结构体默认值）
            auto readColor3 = [&](const char* key, float out[4]) -> bool {
                const JValue* jc = jst->find(key);
                if (jc == nullptr || jc->type != JValue::Arr || jc->arr.size() != 3) {
                    err = std::string("bad ") + key;
                    return false;
                }
                for (size_t ci = 0; ci < 3; ++ci) {
                    double cv = 0;
                    if (!getDouble(jc->arr[ci], cv) || !std::isfinite(cv) || cv < 0 || cv > 1) {
                        err = std::string("bad ") + key + " value";
                        return false;
                    }
                    out[ci] = static_cast<float>(cv);
                }
                out[3] = 1.0f;
                return true;
            };
            auto readNum = [&](const char* key, double& out) -> bool {
                const JValue* jn2 = jst->find(key);
                return jn2 != nullptr && getDouble(*jn2, out) && std::isfinite(out);
            };
            if (jst->find("isAngle") != nullptr) {  // M17a：save 只写参数键（无总键），按首参数判定
                double v = 0;
                if (!readNum("isAngle", v)) { err = "bad isAngle"; return false; }
                st->isAngleDeg = v;
                if (!readNum("isDist", v)) { err = "bad isDist"; return false; }
                st->isDistance = std::max(0.0, v);
                if (!readNum("isSize", v)) { err = "bad isSize"; return false; }
                st->isSize = std::max(0.0, v);
                if (!readNum("isOpacity", v)) { err = "bad isOpacity"; return false; }
                st->isOpacity = std::clamp(v, 0.0, 1.0);
                if (!readColor3("isColor", st->isColor)) { return false; }
                st->innerShadow = true;
            }
            if (jst->find("ogSize") != nullptr) {
                double v = 0;
                if (!readNum("ogSize", v)) { err = "bad ogSize"; return false; }
                st->ogSize = std::max(0.0, v);
                if (!readNum("ogOpacity", v)) { err = "bad ogOpacity"; return false; }
                st->ogOpacity = std::clamp(v, 0.0, 1.0);
                if (!readColor3("ogColor", st->ogColor)) { return false; }
                st->outerGlow = true;
            }
            if (jst->find("igSize") != nullptr) {
                double v = 0;
                if (!readNum("igSize", v)) { err = "bad igSize"; return false; }
                st->igSize = std::max(0.0, v);
                if (!readNum("igOpacity", v)) { err = "bad igOpacity"; return false; }
                st->igOpacity = std::clamp(v, 0.0, 1.0);
                if (!readColor3("igColor", st->igColor)) { return false; }
                st->innerGlow = true;
            }
            if (jst->find("gdAngle") != nullptr) {
                double v = 0;
                if (!readNum("gdAngle", v)) { err = "bad gdAngle"; return false; }
                st->gdAngleDeg = v;
                if (!readNum("gdOpacity", v)) { err = "bad gdOpacity"; return false; }
                st->gdOpacity = std::clamp(v, 0.0, 1.0);
                if (!readColor3("gdFrom", st->gdFrom)) { return false; }
                if (!readColor3("gdTo", st->gdTo)) { return false; }
                st->gradientOverlay = true;
            }
            // M17b：spread / 全局光开关 / 光泽 / 斜面（可选键，缺省 = 结构体默认值）
            auto readOptNum = [&](const char* key, double& out) {
                const JValue* jn2 = jst->find(key);
                if (jn2 != nullptr && getDouble(*jn2, out) && std::isfinite(out)) {
                    return true;
                }
                return false;
            };
            double sv = 0;
            if (readOptNum("shSpread", sv)) { st->shadowSpread = std::clamp(sv, 0.0, 1.0); }
            if (readOptNum("isSpread", sv)) { st->isSpread = std::clamp(sv, 0.0, 1.0); }
            if (readOptNum("ogSpread", sv)) { st->ogSpread = std::clamp(sv, 0.0, 1.0); }
            if (readOptNum("igSpread", sv)) { st->igSpread = std::clamp(sv, 0.0, 1.0); }
            const JValue* jb = jst->find("shUseGL");
            if (jb != nullptr && jb->type == JValue::Bool) { st->shadowUseGL = jb->b; }
            jb = jst->find("isUseGL");
            if (jb != nullptr && jb->type == JValue::Bool) { st->isUseGL = jb->b; }
            if (jst->find("saAngle") != nullptr) {
                if (!readOptNum("saAngle", sv)) { err = "bad saAngle"; return false; }
                st->satinAngleDeg = sv;
                if (!readOptNum("saDist", sv)) { err = "bad saDist"; return false; }
                st->satinDistance = std::max(0.0, sv);
                if (!readOptNum("saSize", sv)) { err = "bad saSize"; return false; }
                st->satinSize = std::max(0.0, sv);
                if (!readOptNum("saOpacity", sv)) { err = "bad saOpacity"; return false; }
                st->satinOpacity = std::clamp(sv, 0.0, 1.0);
                if (!readColor3("saColor", st->satinColor)) { return false; }
                const JValue* ji = jst->find("saInvert");
                if (ji != nullptr && ji->type == JValue::Bool) { st->satinInvert = ji->b; }
                st->satin = true;
            }
            if (jst->find("bvStyle") != nullptr) {
                if (!readOptNum("bvStyle", sv)) { err = "bad bvStyle"; return false; }
                st->bevelStyle = sv != 0 ? 1 : 0;
                if (!readOptNum("bvDepth", sv)) { err = "bad bvDepth"; return false; }
                st->bevelDepth = std::clamp(sv, 0.0, 10.0);
                const JValue* ju = jst->find("bvUp");
                if (ju != nullptr && ju->type == JValue::Bool) { st->bevelUp = ju->b; }
                if (!readOptNum("bvSize", sv)) { err = "bad bvSize"; return false; }
                st->bevelSize = std::max(0.0, sv);
                if (!readOptNum("bvAngle", sv)) { err = "bad bvAngle"; return false; }
                st->bevelAngleDeg = sv;
                if (!readOptNum("bvAlt", sv)) { err = "bad bvAlt"; return false; }
                st->bevelAltitudeDeg = std::clamp(sv, 1.0, 89.0);
                if (!readOptNum("bvHiOp", sv)) { err = "bad bvHiOp"; return false; }
                st->bevelHiOpacity = std::clamp(sv, 0.0, 1.0);
                if (!readColor3("bvHi", st->bevelHiColor)) { return false; }
                if (!readOptNum("bvShOp", sv)) { err = "bad bvShOp"; return false; }
                st->bevelShOpacity = std::clamp(sv, 0.0, 1.0);
                if (!readColor3("bvSh", st->bevelShColor)) { return false; }
                ju = jst->find("bvUseGL");
                if (ju != nullptr && ju->type == JValue::Bool) { st->bevelUseGL = ju->b; }
                st->bevel = true;
            }
            // v11 等高线/Range/图案叠加（M17d，可选键；缺省 = 结构体默认值 = Linear/0.5/关）
            auto readContour = [&](const char* key, StyleContour& out) -> bool {
                const JValue* jc = jst->find(key);
                if (jc == nullptr) {
                    return true;
                }
                if (jc->type != JValue::Obj) { err = std::string("bad ") + key; return false; }
                const JValue* jp = jc->find("pts");
                if (jp == nullptr || jp->type != JValue::Arr || jp->arr.size() < 4 ||
                    jp->arr.size() % 2 != 0 || jp->arr.size() > 64) {
                    err = std::string("bad ") + key + " pts";
                    return false;
                }
                std::vector<float> pts;
                for (const JValue& jv : jp->arr) {
                    double d = 0;
                    if (jv.type != JValue::Num || !getDouble(jv, d) || !std::isfinite(d)) {
                        err = std::string("bad ") + key + " pt";
                        return false;
                    }
                    pts.push_back(static_cast<float>(std::clamp(d, 0.0, 1.0)));
                }
                std::vector<uint8_t> corners;
                const JValue* jcr = jc->find("corners");
                if (jcr != nullptr && jcr->type == JValue::Arr) {
                    for (const JValue& jv : jcr->arr) {
                        if (jv.type == JValue::Num) {
                            corners.push_back(jv.num != 0.0 ? 1 : 0);
                        }
                    }
                }
                out.custom = true;
                out.pts = std::move(pts);
                out.corners = std::move(corners);
                return true;
            };
            if (!readContour("shContour", st->shadowContour) ||
                !readContour("isContour", st->isContour) ||
                !readContour("ogContour", st->ogContour) ||
                !readContour("igContour", st->igContour) ||
                !readContour("saContour", st->satinContour) ||
                !readContour("bvContour", st->bevelContour) ||
                !readContour("bvGlossContour", st->bevelGlossContour)) {
                return false;
            }
            if (readOptNum("ogRange", sv)) { st->ogRange = std::clamp(sv, 0.01, 1.0); }
            if (readOptNum("igRange", sv)) { st->igRange = std::clamp(sv, 0.01, 1.0); }
            if (jst->find("poOn") != nullptr) {
                const JValue* jid = jst->find("poId");
                if (jid == nullptr || jid->type != JValue::Str) { err = "bad poId"; return false; }
                st->patternOverlay = true;
                st->patternId = jid->str;
                if (!readOptNum("poScale", sv)) { err = "bad poScale"; return false; }
                st->poScale = std::clamp(sv, 0.01, 10.0);
                if (!readOptNum("poOpacity", sv)) { err = "bad poOpacity"; return false; }
                st->poOpacity = std::clamp(sv, 0.0, 1.0);
            }
            l.style = std::move(st);
        }
        // v8 填充图层（M14c，可选字段）
        const JValue* jfl = item.find("fill");
        if (jfl != nullptr) {
            if (jfl->type != JValue::Obj) {
                err = "bad fill";
                return false;
            }
            auto fl = std::make_shared<FillLayerState>();
            double kind = 0, angle = 90;
            const JValue* jn = jfl->find("kind");
            if (jn == nullptr || !getDouble(*jn, kind)) { err = "bad fill kind"; return false; }
            const JValue* jc = jfl->find("color");
            if (jc == nullptr || jc->type != JValue::Arr || jc->arr.size() != 3) {
                err = "bad fill color";
                return false;
            }
            for (size_t ci = 0; ci < 3; ++ci) {
                double cv = 0;
                if (!getDouble(jc->arr[ci], cv) || !std::isfinite(cv) || cv < 0 || cv > 1) {
                    err = "bad fill color value";
                    return false;
                }
                fl->color[ci] = static_cast<float>(cv);
            }
            jn = jfl->find("angle");
            if (jn != nullptr && !getDouble(*jn, angle)) { err = "bad fill angle"; return false; }
            fl->kind = static_cast<int>(kind) == 1 ? 1 : 0;
            fl->color[3] = 1.0f;
            fl->angleDeg = angle;
            l.fill = std::move(fl);
        }
        layers.push_back(l);
    }
    if (totalPixels > kMaxTotalPixels) {
        err = "total layer pixels exceed format budget";
        return false;
    }
    // v5 Alpha 通道（可选字段；v4 及以下无此字段 = 无通道）
    const JValue* jch = root.find("channels");
    if (jch != nullptr && jch->type == JValue::Arr) {
        if (jch->arr.size() > 64) {
            err = "channel count exceeds format limit";
            return false;
        }
        for (const JValue& item : jch->arr) {
            if (item.type != JValue::Obj) {
                err = "bad channel record";
                return false;
            }
            const JValue* jn = item.find("name");
            const JValue* jf = item.find("file");
            if (jn == nullptr || jn->type != JValue::Str || jn->str.empty() ||
                jf == nullptr || jf->type != JValue::Str) {
                err = "bad channel record";
                return false;
            }
            // 文件名白名单（防路径注入）：channels/<十进制索引>.png
            const std::string expectPrefix = "channels/";
            const std::string expectSuffix = ".png";
            const std::string f = jf->str;
            bool ok = f.size() > expectPrefix.size() + expectSuffix.size() &&
                      f.compare(0, expectPrefix.size(), expectPrefix) == 0 &&
                      f.compare(f.size() - expectSuffix.size(), expectSuffix.size(), expectSuffix) == 0;
            if (ok) {
                for (size_t k = expectPrefix.size(); k + expectSuffix.size() < f.size(); ++k) {
                    if (f[k] < '0' || f[k] > '9') {
                        ok = false;
                        break;
                    }
                }
            }
            if (!ok) {
                err = "bad channel file reference";
                return false;
            }
            AlphaChannel ch;
            ch.name = jn->str;
            doc.channels.push_back(std::move(ch));
            channelFiles.push_back(f);
        }
    }
    // v9：参考线（可选块；v8 及以下无此字段 = 空参考线全默认）
    const JValue* jgd = root.find("guides");
    if (jgd != nullptr) {
        if (jgd->type != JValue::Obj) {
            err = "bad guides record";
            return false;
        }
        const auto parseAxis = [&](const char* key, std::vector<float>& out,
                                   double limit) -> bool {
            const JValue* ja = jgd->find(key);
            if (ja == nullptr) {
                return true;
            }
            if (ja->type != JValue::Arr || ja->arr.size() > 256) {
                return false;
            }
            for (const JValue& item : ja->arr) {
                double v = 0;
                if (!getDouble(item, v) || !std::isfinite(v) || v < 0.0 || v > limit) {
                    return false;
                }
                out.push_back(static_cast<float>(v));
            }
            return true;
        };
        if (!parseAxis("h", doc.guidesH, static_cast<double>(doc.width)) ||
            !parseAxis("v", doc.guidesV, static_cast<double>(doc.height))) {
            err = "bad guides record";
            return false;
        }
        std::sort(doc.guidesH.begin(), doc.guidesH.end());
        std::sort(doc.guidesV.begin(), doc.guidesV.end());
        const JValue* jl = jgd->find("locked");
        if (jl != nullptr && jl->type == JValue::Bool) {
            doc.guidesLocked = jl->b;
        }
        const JValue* jv = jgd->find("visible");
        if (jv != nullptr && jv->type == JValue::Bool) {
            doc.guidesVisible = jv->b;
        }
    }
    // v10：全局光角度（M17b，可选；缺省 120）
    const JValue* jgl = root.find("globalLight");
    if (jgl != nullptr && jgl->type == JValue::Num && std::isfinite(jgl->num)) {
        doc.globalLightAngle = std::clamp(jgl->num, 0.0, 360.0);
    }
    // v11：文档图案（M17d，可选块；像素走 images/pattern_<id>.png，openProject 装载）
    const JValue* jpats = root.find("patterns");
    if (jpats != nullptr) {
        if (jpats->type != JValue::Arr) {
            err = "bad patterns record";
            return false;
        }
        for (const JValue& jp : jpats->arr) {
            if (jp.type != JValue::Obj) {
                err = "bad pattern record";
                return false;
            }
            const JValue* jid = jp.find("id");
            const JValue* jnm = jp.find("name");
            const JValue* jw = jp.find("w");
            const JValue* jh = jp.find("h");
            double w = 0, h = 0;
            if (jid == nullptr || jid->type != JValue::Str || jid->str.empty() ||
                jid->str.size() > 64 || jnm == nullptr || jnm->type != JValue::Str ||
                jw == nullptr || !getDouble(*jw, w) || jh == nullptr || !getDouble(*jh, h) ||
                w < 1 || h < 1 || w > 4096 || h > 4096) {
                err = "bad pattern record fields";
                return false;
            }
            // id 只允许字母数字下划线（拼 zip 条目名，防路径注入——ZipReader 同纪律）
            for (char c : jid->str) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                    err = "bad pattern id";
                    return false;
                }
            }
            DocumentPattern p;
            p.id = jid->str;
            p.name = jnm->str;
            p.w = static_cast<uint32_t>(w);
            p.h = static_cast<uint32_t>(h);
            patterns.push_back(std::move(p));
        }
    }
    // activeLayerId 必须在集合内（对齐上游 validate）
    const JValue* jactive = root.find("activeLayerId");
    if (jactive != nullptr && jactive->type == JValue::Num && jactive->num > 0) {
        uint64_t active = 0;
        getU64(*jactive, active);
        doc.activeId = ids.count(active) != 0 ? static_cast<LayerId>(active) : 0;
    }
    return true;
}

}  // namespace

bool openProject(int fd, ProjectOpenResult* out, std::string& err) {
    // 整包读入（与 PSD 导入同策略：包体几十 MB 量级）
    std::vector<uint8_t> data;
    {
        off_t sz = lseek(fd, 0, SEEK_END);
        if (sz <= 0) {
            close(fd);
            err = "empty file";
            return false;
        }
        lseek(fd, 0, SEEK_SET);
        data.resize(static_cast<size_t>(sz));
        size_t got = 0;
        while (got < data.size()) {
            ssize_t n = read(fd, data.data() + got, data.size() - got);
            if (n <= 0) {
                break;
            }
            got += static_cast<size_t>(n);
        }
        close(fd);
        if (got != data.size()) {
            err = "read failed";
            return false;
        }
    }

    ZipReader zip;
    if (!zip.open(data)) {
        err = "not a valid montage package";
        return false;
    }
    const ZipReader::CdEntry* manifestEntry = zip.find("manifest.json");
    if (manifestEntry == nullptr) {
        err = "manifest.json missing";
        return false;
    }
    std::vector<uint8_t> manifestBytes;
    if (!zip.extract(*manifestEntry, manifestBytes, err)) {
        return false;
    }
    Document doc;
    std::vector<LayerRecord> records;
    std::vector<std::string> channelFiles;
    std::vector<DocumentPattern> patterns;
    if (!parseManifest(manifestBytes, doc, records, channelFiles, patterns, err)) {
        return false;
    }

    // 逐层 PNG 解码切瓦（PNG 字节先解出校验 CRC，再过系统解码器 + 预乘还原）
    std::vector<Layer> layers;
    layers.reserve(records.size());
    for (const LayerRecord& rec : records) {
        Layer l;
        l.id = rec.id;
        l.name = rec.name;
        l.visible = rec.visible;
        l.opacity = rec.opacity;
        l.blendMode = rec.blendMode;
        l.clipping = rec.clipping;
        l.adjustment = rec.adjustment;
        l.text = rec.text;
        l.isGroup = rec.isGroup;
        l.parentId = rec.parentId;
        l.collapsed = rec.collapsed;
        l.style = rec.style;
        l.fill = rec.fill;
        l.transform.originX = rec.originX;
        l.transform.originY = rec.originY;
        if (rec.hasImage) {
            const ZipReader::CdEntry* png = zip.find("images/" + std::to_string(rec.id) + ".png");
            if (png == nullptr) {
                err = "layer image missing: " + std::to_string(rec.id);
                return false;
            }
            std::vector<uint8_t> pngBytes;
            if (!zip.extract(*png, pngBytes, err)) {
                return false;
            }
            // 自研确定性 PNG 解码（直 RGBA；系统解码器字节序/预乘行为不可信，M4c 实证）
            std::shared_ptr<const TileGrid> grid = decodePngToGrid(pngBytes.data(), pngBytes.size(), err);
            if (grid == nullptr) {
                return false;
            }
            // 资产尺寸须与 manifest 声明一致（上游 validate 语义）
            const uint32_t expectCols = (rec.width + kTileSize - 1u) / kTileSize;
            const uint32_t expectRows = (rec.height + kTileSize - 1u) / kTileSize;
            if (grid->cols != expectCols || grid->rows != expectRows) {
                err = "layer image size mismatch: " + std::to_string(rec.id);
                return false;
            }
            l.pixels = std::move(grid);
        }
        if (rec.hasMask) {
            // v2 蒙版：patch PNG → LayerMask → 合成派生网格
            const ZipReader::CdEntry* mpng =
                zip.find("images/" + std::to_string(rec.id) + ".mask.png");
            if (mpng == nullptr) {
                err = "layer mask missing: " + std::to_string(rec.id);
                return false;
            }
            std::vector<uint8_t> maskBytes;
            if (!zip.extract(*mpng, maskBytes, err)) {
                return false;
            }
            std::shared_ptr<const TileGrid> mgrid =
                decodePngToGrid(maskBytes.data(), maskBytes.size(), err);
            if (mgrid == nullptr) {
                return false;
            }
            const uint32_t mCols = (rec.maskWidth + kTileSize - 1u) / kTileSize;
            const uint32_t mRows = (rec.maskHeight + kTileSize - 1u) / kTileSize;
            if (mgrid->cols != mCols || mgrid->rows != mRows) {
                err = "layer mask size mismatch: " + std::to_string(rec.id);
                return false;
            }
            auto m = std::make_shared<LayerMask>();
            m->pixels = std::move(mgrid);
            m->width = rec.maskWidth;
            m->height = rec.maskHeight;
            m->offsetX = rec.maskOffsetX;
            m->offsetY = rec.maskOffsetY;
            m->outside = static_cast<uint8_t>(rec.maskOutside);
            m->enabled = rec.maskEnabled;
            m->linked = true;
            l.mask = std::move(m);
            if (l.mask->enabled && l.pixels != nullptr) {
                l.render = composeMasked(l.pixels, *l.mask);
            }
        }
        layers.push_back(std::move(l));
    }

    // v5：Alpha 通道 PNG（文档域灰度网格；尺寸须与文档一致）
    for (size_t i = 0; i < channelFiles.size(); ++i) {
        const ZipReader::CdEntry* cpng = zip.find(channelFiles[i]);
        if (cpng == nullptr) {
            err = "channel image missing: " + channelFiles[i];
            return false;
        }
        std::vector<uint8_t> cBytes;
        if (!zip.extract(*cpng, cBytes, err)) {
            return false;
        }
        std::shared_ptr<const TileGrid> cgrid = decodePngToGrid(cBytes.data(), cBytes.size(), err);
        if (cgrid == nullptr) {
            return false;
        }
        const uint32_t cCols = (doc.width + kTileSize - 1u) / kTileSize;
        const uint32_t cRows = (doc.height + kTileSize - 1u) / kTileSize;
        if (cgrid->cols != cCols || cgrid->rows != cRows) {
            err = "channel image size mismatch: " + channelFiles[i];
            return false;
        }
        doc.channels[i].mask = std::move(cgrid);
    }

    // v11：文档图案 PNG（M17d；网格域须与 manifest 尺寸一致；id 数字后缀回填 patternSeq
    // 防打开后新建撞号）
    uint64_t maxPatternSeq = 0;
    for (DocumentPattern& p : patterns) {
        const std::string entry = "images/pattern_" + p.id + ".png";
        const ZipReader::CdEntry* ppng = zip.find(entry);
        if (ppng == nullptr) {
            err = "pattern image missing: " + entry;
            return false;
        }
        std::vector<uint8_t> pBytes;
        if (!zip.extract(*ppng, pBytes, err)) {
            return false;
        }
        std::shared_ptr<const TileGrid> grid = decodePngToGrid(pBytes.data(), pBytes.size(), err);
        if (grid == nullptr) {
            return false;
        }
        const uint32_t expCols = (p.w + kTileSize - 1u) / kTileSize;
        const uint32_t expRows = (p.h + kTileSize - 1u) / kTileSize;
        if (grid->cols != expCols || grid->rows != expRows) {
            err = "pattern image size mismatch: " + entry;
            return false;
        }
        p.pixels = std::move(grid);
        if (p.id.rfind("pat_", 0) == 0) {
            maxPatternSeq = std::max(maxPatternSeq,
                                     static_cast<uint64_t>(std::strtoull(p.id.c_str() + 4,
                                                                         nullptr, 10)));
        }
        doc.patterns.push_back(std::move(p));
    }
    Engine::get().patternSeq = std::max(Engine::get().patternSeq, maxPatternSeq);

    // 整档替换（02 §5 语义）：统一走 resetDocument（清撤销/会话态，撤销不跨文档）
    auto& engine = Engine::get();
    engine.resetDocument();
    {
        std::lock_guard<std::mutex> lk(engine.docMutex);
        engine.doc.width = doc.width;
        engine.doc.height = doc.height;
        engine.doc.name = doc.name;
        engine.doc.layers = std::move(layers);
        engine.doc.activeId = doc.activeId;
        engine.doc.channels = std::move(doc.channels);
        // M15a：参考线随工程恢复（会话态持久化）
        engine.doc.guidesH = std::move(doc.guidesH);
        engine.doc.guidesV = std::move(doc.guidesV);
        engine.doc.guidesLocked = doc.guidesLocked;
        engine.doc.guidesVisible = doc.guidesVisible;
        engine.doc.patterns = std::move(doc.patterns);  // M17d：图案资源随工程恢复
        // M14c：样式/填充派生渲染重建（styledRender 不入库，读入后按参数重生成）
        restyleDocument(engine.doc);
        engine.bumpRevisionLocked();
        engine.requestRender();
    }
    if (out != nullptr) {
        out->width = doc.width;
        out->height = doc.height;
        out->layers = records.size();
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "project opened: %{public}ux%{public}u, %{public}zu layers",
                 doc.width, doc.height, records.size());
    return true;
}

}  // namespace io
}  // namespace montage
