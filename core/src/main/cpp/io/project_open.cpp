#include "io/project_open.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>
#include <zlib.h>

#include <hilog/log.h>

#include "engine/document.h"
#include "engine/engine.h"
#include "io/png_decode.h"

namespace montage {
namespace io {
namespace {

constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Open";
constexpr const char* kFormatId = "com.sytkiller.montage.project";
constexpr int kMinVersion = 1;
constexpr int kMaxVersion = 2;  // v2：+图层蒙版（读取方按字段存在性兼容 v1）
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
        layers.push_back(l);
    }
    if (totalPixels > kMaxTotalPixels) {
        err = "total layer pixels exceed format budget";
        return false;
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
    if (!parseManifest(manifestBytes, doc, records, err)) {
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

    // 整档替换（02 §5 语义）+ History 清空（撤销不跨文档）
    auto& engine = Engine::get();
    {
        std::lock_guard<std::mutex> lk(engine.docMutex);
        engine.doc = Document{};
        engine.doc.width = doc.width;
        engine.doc.height = doc.height;
        engine.doc.name = doc.name;
        engine.doc.layers = std::move(layers);
        engine.doc.activeId = doc.activeId;
        engine.history.clear();
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
