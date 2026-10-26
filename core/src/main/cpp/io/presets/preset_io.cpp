// P1 预设资源导入内核实现（见 preset_io.h 头注）。photocraft crates/psd 逐式翻译；
// 所有读取 bounds-checked，坏输入报错不崩。

#include "io/presets/preset_io.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "io/psd/descriptor.h"

namespace montage {
namespace presets {
namespace {

// ---------- 大端读取（越界 = 报错返回 false） ----------

struct Rd {
    const uint8_t* d = nullptr;
    size_t n = 0;
    size_t p = 0;
    bool ok = true;

    bool take(size_t k, const uint8_t** out) {
        if (!ok || p + k > n) {
            ok = false;
            return false;
        }
        if (out != nullptr) {
            *out = d + p;
        }
        p += k;
        return true;
    }
    bool u16(uint16_t& v) {
        const uint8_t* s;
        if (!take(2, &s)) return false;
        v = static_cast<uint16_t>((s[0] << 8) | s[1]);
        return true;
    }
    bool u32(uint32_t& v) {
        const uint8_t* s;
        if (!take(4, &s)) return false;
        v = (static_cast<uint32_t>(s[0]) << 24) | (static_cast<uint32_t>(s[1]) << 16) |
            (static_cast<uint32_t>(s[2]) << 8) | static_cast<uint32_t>(s[3]);
        return true;
    }
    bool i32(int32_t& v) {
        uint32_t u;
        if (!u32(u)) return false;
        v = static_cast<int32_t>(u);
        return true;
    }
    bool u8(uint8_t& v) {
        const uint8_t* s;
        if (!take(1, &s)) return false;
        v = s[0];
        return true;
    }
    bool f32(float& v) {
        uint32_t u;
        if (!u32(u)) return false;
        std::memcpy(&v, &u, 4);
        return true;
    }
    size_t left() const { return ok ? n - p : 0; }
};

// UTF-16BE 单元列 → UTF-8（有损；去尾部 NUL——photocraft from_utf16_lossy 同语义）
std::string utf16beToUtf8(const uint8_t* d, size_t units) {
    std::u16string u(units, u'\0');
    for (size_t i = 0; i < units; ++i) {
        u[i] = static_cast<char16_t>((d[i * 2] << 8) | d[i * 2 + 1]);
    }
    while (!u.empty() && u.back() == u'\0') {
        u.pop_back();
    }
    std::string out;
    for (size_t i = 0; i < u.size(); ++i) {
        char16_t c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < u.size() && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            c = static_cast<char16_t>(0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00));
            i++;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

void appendUtf16be(std::vector<uint8_t>& out, const std::string& utf8, bool withNul) {
    // UTF-8 → UTF-16BE（简化：BMP 直映；代理外码点 U+FFFD）
    for (size_t i = 0; i < utf8.size();) {
        const uint8_t c = static_cast<uint8_t>(utf8[i]);
        uint32_t cp = 0;
        int len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            cp = ((c & 0x1Fu) << 6) | (utf8[i + 1] & 0x3Fu);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((c & 0x0Fu) << 12) | ((utf8[i + 1] & 0x3Fu) << 6) | (utf8[i + 2] & 0x3Fu);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
            cp = 0xFFFD;  // v1 不处理四字节星面（emoji 降 U+FFFD，登记）
            len = 4;
        } else {
            cp = 0xFFFD;
            len = 1;
        }
        i += static_cast<size_t>(len);
        out.push_back(static_cast<uint8_t>(cp >> 8));
        out.push_back(static_cast<uint8_t>(cp & 0xFF));
    }
    if (withNul) {
        out.push_back(0);
        out.push_back(0);
    }
}

// PackBits 行解码（Apple 标准；对拍 photocraft packbits::decode_into——输出不足 = 报错）
bool packBitsDecode(const uint8_t* src, size_t len, size_t rowBytes, std::vector<uint8_t>& out) {
    size_t i = 0;
    size_t written = 0;
    const size_t total = out.size();
    while (i < len && written < total) {
        const int8_t n = static_cast<int8_t>(src[i]);
        i++;
        if (n >= 0) {
            const size_t k = static_cast<size_t>(n) + 1;
            if (i + k > len || written + k > total) {
                return false;
            }
            std::memcpy(out.data() + written, src + i, k);
            i += k;
            written += k;
        } else if (n != -128) {
            const size_t k = static_cast<size_t>(1 - static_cast<int>(n));
            if (i >= len || written + k > total) {
                return false;
            }
            std::memset(out.data() + written, src[i], k);
            i++;
            written += k;
        }
    }
    return written == total;
}

// PackBits 行编码（对拍 photocraft packbits::encode：≥2 重复发 run 包，字面量含尾）
void packBitsEncode(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    size_t i = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 128 && src[i + run] == src[i]) {
            run++;
        }
        if (run >= 2) {
            out.push_back(static_cast<uint8_t>(1 - static_cast<int>(run)));
            out.push_back(src[i]);
            i += run;
            continue;
        }
        size_t lit = 1;
        while (i + lit < n && lit < 128) {
            size_t r = 1;
            while (i + lit + r < n && r < 3 && src[i + lit + r] == src[i + lit]) {
                r++;
            }
            if (r >= 3) break;
            lit++;
        }
        out.push_back(static_cast<uint8_t>(lit - 1));
        out.insert(out.end(), src + i, src + i + lit);
        i += lit;
    }
}

void putU16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v & 0xFF));
}
void putU32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v >> 24));
    o.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    o.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    o.push_back(static_cast<uint8_t>(v & 0xFF));
}
void putF32(std::vector<uint8_t>& o, float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    putU32(o, u);
}

// ---------- 颜色转换（photocraft color/convert.rs naive 同式） ----------

inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

}  // namespace

void hsbToRgb(float h, float s, float v, float out[3]) {
    s = clamp01(s);
    v = clamp01(v);
    if (!std::isfinite(h)) h = 0;
    h = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(h)) {
        case 0: r = c; g = x; break;
        case 1: r = x; g = c; break;
        case 2: g = c; b = x; break;
        case 3: g = x; b = c; break;
        case 4: r = x; b = c; break;
        default: r = c; b = x; break;
    }
    const float m = v - c;
    out[0] = r + m;
    out[1] = g + m;
    out[2] = b + m;
}

void cmykToRgbNaive(const float c[4], float out[3]) {
    const float k = 1.0f - clamp01(c[3]);
    out[0] = (1.0f - clamp01(c[0])) * k;
    out[1] = (1.0f - clamp01(c[1])) * k;
    out[2] = (1.0f - clamp01(c[2])) * k;
}

void labToSrgbNaive(float l, float a, float b, float out[3]) {
    const float fy = (l + 16.0f) / 116.0f;
    const float fx = fy + a / 500.0f;
    const float fz = fy - b / 200.0f;
    auto finv = [](float t) {
        const double e = 6.0 / 29.0;
        return t > e ? t * t * t : static_cast<float>(3.0 * e * e * (t - e));
    };
    const float D50[3] = {0.96422f, 1.0f, 0.82521f};
    const float M[3][3] = {{3.133856f, -1.616867f, -0.490615f},
                           {-0.978768f, 1.916142f, 0.033454f},
                           {0.071945f, -0.228991f, 1.405243f}};
    const float xyz[3] = {finv(fx) * D50[0], finv(fy) * D50[1], finv(fz) * D50[2]};
    for (int i = 0; i < 3; ++i) {
        const float lin = std::clamp(M[i][0] * xyz[0] + M[i][1] * xyz[1] + M[i][2] * xyz[2], 0.0f, 1.0f);
        out[i] = lin <= 0.0031308f ? lin * 12.92f : 1.055f * std::pow(lin, 1.0f / 2.4f) - 0.055f;
    }
}

namespace {

// aco 灰度：文件存墨量 0..10000（0 = 白，photocraft swatch_cmds from_aco 1−g/10000 同式）
inline void acoGrayToRgb(uint16_t level, float out[3]) {
    const float ink = static_cast<float>(std::min<uint16_t>(level, 10000)) / 10000.0f;
    out[0] = out[1] = out[2] = 1.0f - ink;
}

// aco 色记录 → sRGB（space id：0 RGB / 1 HSB / 2 CMYK / 7 Lab / 8 Gray / 其它 = 黑兜底+警告）
bool acoRecordToRgb(uint16_t space, const uint16_t v[4], float out[3], SwatchFile& f) {
    switch (space) {
        case 0:
            out[0] = v[0] / 65535.0f;
            out[1] = v[1] / 65535.0f;
            out[2] = v[2] / 65535.0f;
            return true;
        case 1: {
            float hsb[3] = {v[0] / 65535.0f * 360.0f, v[1] / 65535.0f, v[2] / 65535.0f};
            hsbToRgb(hsb[0], hsb[1], hsb[2], out);
            return true;
        }
        case 2: {
            // 文件存补数（0 = 100% 墨）
            float cmyk[4] = {1.0f - v[0] / 65535.0f, 1.0f - v[1] / 65535.0f,
                             1.0f - v[2] / 65535.0f, 1.0f - v[3] / 65535.0f};
            cmykToRgbNaive(cmyk, out);
            return true;
        }
        case 7: {
            const int16_t a = static_cast<int16_t>(v[1]);
            const int16_t b = static_cast<int16_t>(v[2]);
            labToSrgbNaive(static_cast<float>(std::min<uint16_t>(v[0], 10000)) / 100.0f,
                           static_cast<float>(a) / 100.0f, static_cast<float>(b) / 100.0f, out);
            return true;
        }
        case 8:
            acoGrayToRgb(v[0], out);
            return true;
        default:
            f.warnings.push_back("aco: unsupported colour space " + std::to_string(space) +
                                 " imported as black");
            out[0] = out[1] = out[2] = 0.0f;
            return true;
    }
}

// 读一个 aco 节（named = v2 带名）。minBytes = 每记录最少字节数（越界预检）。
bool acoSection(Rd& r, bool named, size_t count, SwatchFile& f, std::string& err) {
    if (r.left() < count * (named ? 14u : 10u)) {
        err = "aco: truncated section";
        return false;
    }
    // 组语义：v2 节全部平铺进单隐式组（aco 无组结构）
    if (f.groups.empty()) {
        f.groups.push_back(SwatchGroup{});
    }
    SwatchGroup& g = f.groups.front();
    for (size_t i = 0; i < count; ++i) {
        uint16_t space = 0;
        uint16_t v[4] = {0, 0, 0, 0};
        if (!r.u16(space)) break;
        for (int k = 0; k < 4; ++k) {
            if (!r.u16(v[k])) {
                err = "aco: truncated record";
                return false;
            }
        }
        std::string name;
        if (named) {
            uint32_t units = 0;
            if (!r.u32(units) || units > 65536) {
                err = "aco: bad name length";
                return false;
            }
            const uint8_t* s;
            if (!r.take(static_cast<size_t>(units) * 2, &s)) {
                err = "aco: truncated name";
                return false;
            }
            name = utf16beToUtf8(s, units);
        }
        Swatch sw;
        sw.name = name;
        acoRecordToRgb(space, v, &sw.r, f);
        g.items.push_back(std::move(sw));
    }
    return true;
}

}  // namespace

// ---------- .aco ----------

bool parseAco(const uint8_t* d, size_t n, SwatchFile& out, std::string& err) {
    Rd r{d, n, 0, true};
    uint16_t version = 0;
    if (!r.u16(version)) {
        err = "aco: empty file";
        return false;
    }
    if (version == 1) {
        uint16_t count = 0;
        if (!r.u16(count)) {
            err = "aco: truncated count";
            return false;
        }
        if (!acoSection(r, false, count, out, err)) return false;
        // PS6+ 在 v1 节后跟 v2 节（[0,2] 开头）；其余尾随字节忽略
        if (r.left() >= 4 && r.d[r.p] == 0 && r.d[r.p + 1] == 2) {
            r.p += 2;
            uint16_t count2 = 0;
            if (!r.u16(count2)) {
                err = "aco: truncated v2 count";
                return false;
            }
            SwatchFile v2;
            if (!acoSection(r, true, count2, v2, err)) return false;
            out.groups = std::move(v2.groups);
            for (std::string& w : v2.warnings) {
                out.warnings.push_back(std::move(w));
            }
            out.hasGroups = false;
        }
        return true;
    }
    if (version == 2) {
        uint16_t count = 0;
        if (!r.u16(count)) {
            err = "aco: truncated count";
            return false;
        }
        if (!acoSection(r, true, count, out, err)) return false;
        return true;
    }
    err = "aco: unsupported version " + std::to_string(version);
    return false;
}

bool writeAco(const std::vector<Swatch>& sw, std::vector<uint8_t>& out, std::string& err) {
    if (sw.size() > 65535) {
        err = "aco: at most 65535 swatches";
        return false;
    }
    // RGB 色记录（v1/v2 双节，photocraft write 布局）
    auto putRecord = [&](const Swatch& s, bool named) {
        putU16(out, 0);  // RGB
        putU16(out, static_cast<uint16_t>(std::lround(clamp01(s.r) * 65535.0f)));
        putU16(out, static_cast<uint16_t>(std::lround(clamp01(s.g) * 65535.0f)));
        putU16(out, static_cast<uint16_t>(std::lround(clamp01(s.b) * 65535.0f)));
        putU16(out, 0);
        if (named) {
            std::vector<uint8_t> units;
            appendUtf16be(units, s.name, true);
            putU32(out, static_cast<uint32_t>(units.size() / 2));
            out.insert(out.end(), units.begin(), units.end());
        }
    };
    for (int ver = 1; ver <= 2; ++ver) {
        putU16(out, static_cast<uint16_t>(ver));
        putU16(out, static_cast<uint16_t>(sw.size()));
        for (const Swatch& s : sw) {
            putRecord(s, ver == 2);
        }
    }
    return true;
}

// ---------- .ase ----------

namespace {

constexpr uint16_t kAseGroupStart = 0xC001;
constexpr uint16_t kAseGroupEnd = 0xC002;
constexpr uint16_t kAseColor = 0x0001;

bool aseName(Rd& r, std::string& out, std::string& err) {
    if (r.left() == 0) {
        out.clear();
        return true;
    }
    uint16_t units = 0;
    if (!r.u16(units)) {
        err = "ase: truncated name";
        return false;
    }
    const uint8_t* s;
    if (!r.take(static_cast<size_t>(units) * 2, &s)) {
        err = "ase: truncated name";
        return false;
    }
    out = utf16beToUtf8(s, units);
    return true;
}

bool aseColor(Rd& body, Swatch& sw, std::string& err) {
    if (!aseName(body, sw.name, err)) return false;
    const uint8_t* model;
    if (!body.take(4, &model)) {
        err = "ase: truncated colour model";
        return false;
    }
    float comps[4] = {0, 0, 0, 0};
    int nc = 0;
    if (std::memcmp(model, "RGB ", 4) == 0) {
        nc = 3;
    } else if (std::memcmp(model, "CMYK", 4) == 0) {
        nc = 4;
    } else if (std::memcmp(model, "LAB ", 4) == 0) {
        nc = 3;
    } else if (std::memcmp(model, "Gray", 4) == 0) {
        nc = 1;
    } else {
        // 未知模型：整个块跳过（色记黑 + 警告，photocraft Other 保留语义的 v1 简化）
        sw.r = sw.g = sw.b = 0.0f;
        return true;
    }
    for (int i = 0; i < nc; ++i) {
        if (!body.f32(comps[i])) {
            err = "ase: truncated colour components";
            return false;
        }
        if (!std::isfinite(comps[i])) comps[i] = 0.0f;
    }
    if (nc == 3 && std::memcmp(model, "RGB ", 4) == 0) {
        sw.r = comps[0];
        sw.g = comps[1];
        sw.b = comps[2];
    } else if (nc == 4) {
        cmykToRgbNaive(comps, &sw.r);
    } else if (std::memcmp(model, "LAB ", 4) == 0) {
        labToSrgbNaive(comps[0], comps[1], comps[2], &sw.r);
    } else {
        sw.r = sw.g = sw.b = clamp01(comps[0]);  // ase Gray 0..1（1 = 白）
    }
    return true;
}

}  // namespace

bool parseAse(const uint8_t* d, size_t n, SwatchFile& out, std::string& err) {
    Rd r{d, n, 0, true};
    const uint8_t* sig;
    if (!r.take(4, &sig) || std::memcmp(sig, "ASEF", 4) != 0) {
        err = "ase: bad signature (need ASEF)";
        return false;
    }
    uint16_t major = 0, minor = 0;
    if (!r.u16(major) || !r.u16(minor)) {
        err = "ase: truncated header";
        return false;
    }
    if (major != 1) {
        err = "ase: unsupported version " + std::to_string(major);
        return false;
    }
    uint32_t count = 0;
    if (!r.u32(count)) {
        err = "ase: truncated block count";
        return false;
    }
    if (r.left() < static_cast<size_t>(count) * 6 && count > 0) {
        err = "ase: block count beyond data";
        return false;
    }
    size_t total = 0;
    bool openGroup = false;
    bool looseInited = false;
    for (uint32_t bi = 0; bi < count; ++bi) {
        uint16_t kind = 0;
        uint32_t len = 0;
        if (!r.u16(kind) || !r.u32(len)) {
            err = "ase: truncated block header";
            return false;
        }
        if (r.left() < len) {
            err = "ase: truncated block body";
            return false;
        }
        Rd body{r.d + r.p, len, 0, true};
        r.p += len;
        if (kind == kAseGroupStart) {
            SwatchGroup g;
            if (!aseName(body, g.name, err)) return false;
            out.groups.push_back(std::move(g));
            out.hasGroups = true;
            openGroup = true;
        } else if (kind == kAseGroupEnd) {
            openGroup = false;
        } else if (kind == kAseColor) {
            Swatch sw;
            if (!aseColor(body, sw, err)) return false;
            if (++total > kMaxSwatches) {
                err = "ase: too many swatches";
                return false;
            }
            if (openGroup && !out.groups.empty()) {
                out.groups.back().items.push_back(std::move(sw));
            } else {
                // 散色落隐式首组（已有组结构时插最前，仅一次）
                if (!out.hasGroups) {
                    if (out.groups.empty()) {
                        out.groups.push_back(SwatchGroup{});
                    }
                } else if (!looseInited) {
                    out.groups.insert(out.groups.begin(), SwatchGroup{});
                    looseInited = true;
                }
                out.groups.front().items.push_back(std::move(sw));
            }
        }
        // 其它块类型跳过
    }
    if (out.groups.empty()) {
        err = "ase: no colour entries";
        return false;
    }
    return true;
}

bool writeAse(const SwatchFile& f, std::vector<uint8_t>& out, std::string& err) {
    (void)err;
    size_t blocks = 0;
    std::vector<uint8_t> body;
    auto putColor = [&](const Swatch& s) {
        std::vector<uint8_t> b;
        {
            std::vector<uint8_t> units;
            appendUtf16be(units, s.name, true);
            putU16(b, static_cast<uint16_t>(units.size() / 2));
            b.insert(b.end(), units.begin(), units.end());
        }
        b.insert(b.end(), {'R', 'G', 'B', ' '});
        putF32(b, clamp01(s.r));
        putF32(b, clamp01(s.g));
        putF32(b, clamp01(s.b));
        putU16(b, 2);  // process
        putU16(body, kAseColor);
        putU32(body, static_cast<uint32_t>(b.size()));
        body.insert(body.end(), b.begin(), b.end());
        blocks++;
    };
    for (const SwatchGroup& g : f.groups) {
        if (f.hasGroups) {
            std::vector<uint8_t> nb;
            {
                std::vector<uint8_t> units;
                appendUtf16be(units, g.name, true);
                putU16(nb, static_cast<uint16_t>(units.size() / 2));
                nb.insert(nb.end(), units.begin(), units.end());
            }
            putU16(body, kAseGroupStart);
            putU32(body, static_cast<uint32_t>(nb.size()));
            body.insert(body.end(), nb.begin(), nb.end());
            for (const Swatch& s : g.items) {
                putColor(s);
            }
            putU16(body, kAseGroupEnd);
            putU32(body, 0);
            blocks += 2;
        } else {
            for (const Swatch& s : g.items) {
                putColor(s);
            }
        }
    }
    out.insert(out.end(), {'A', 'S', 'E', 'F'});
    putU16(out, 1);
    putU16(out, 0);
    putU32(out, static_cast<uint32_t>(blocks));
    out.insert(out.end(), body.begin(), body.end());
    return true;
}

// ---------- .grd（v5 描述符；依赖 io/psd/descriptor） ----------

namespace {

double grdNum(const psd::DescObject& o, const char* k, double dflt) {
    const psd::DescValue* v = o.get(k);
    if (v == nullptr) return dflt;
    switch (v->type) {
        case psd::DescValue::Double:
        case psd::DescValue::UnitFloat:
            return v->dbl;
        case psd::DescValue::Integer:
        case psd::DescValue::LargeInteger:
            return static_cast<double>(v->integer);
        default:
            return dflt;
    }
}

bool grdEnumIs(const psd::DescObject& o, const char* k, const char* val) {
    const psd::DescValue* v = o.get(k);
    return v != nullptr && v->type == psd::DescValue::Enumerated && v->enumValue.is(val);
}

const psd::DescObject* grdObj(const psd::DescObject& o, const char* k) {
    const psd::DescValue* v = o.get(k);
    if (v == nullptr || (v->type != psd::DescValue::Object && v->type != psd::DescValue::GlobalObject)) {
        return nullptr;
    }
    return v->obj.get();
}

// 停靠色（classId 分派；越界钳制——photocraft color() 同式）
bool grdColor(const psd::DescObject& c, GrdStop& stop) {
    if (c.classId.is("RGBC")) {
        stop.kind = 0;
        stop.r = clamp01(static_cast<float>(grdNum(c, "Rd  ", 0) / 255.0));
        stop.g = clamp01(static_cast<float>(grdNum(c, "Grn ", 0) / 255.0));
        stop.b = clamp01(static_cast<float>(grdNum(c, "Bl  ", 0) / 255.0));
        return true;
    }
    if (c.classId.is("HSBC")) {
        float hsb[3] = {static_cast<float>(grdNum(c, "H   ", 0)),
                        clamp01(static_cast<float>(grdNum(c, "Strt", 0) / 100.0)),
                        clamp01(static_cast<float>(grdNum(c, "Brgh", 0) / 100.0))};
        hsbToRgb(hsb[0], hsb[1], hsb[2], &stop.r);
        stop.kind = 0;
        return true;
    }
    if (c.classId.is("CMYC")) {
        float cmyk[4] = {clamp01(static_cast<float>(grdNum(c, "Cyn ", 0) / 100.0)),
                         clamp01(static_cast<float>(grdNum(c, "Mgnt", 0) / 100.0)),
                         clamp01(static_cast<float>(grdNum(c, "Ylw ", 0) / 100.0)),
                         clamp01(static_cast<float>(grdNum(c, "Blck", 0) / 100.0))};
        cmykToRgbNaive(cmyk, &stop.r);
        stop.kind = 0;
        return true;
    }
    if (c.classId.is("LbCl")) {
        labToSrgbNaive(clamp01(static_cast<float>(grdNum(c, "Lmnc", 0))),
                       std::clamp(static_cast<float>(grdNum(c, "A   ", 0)), -128.0f, 127.0f),
                       std::clamp(static_cast<float>(grdNum(c, "B   ", 0)), -128.0f, 127.0f),
                       &stop.r);
        stop.kind = 0;
        return true;
    }
    if (c.classId.is("Grsc")) {
        const float g = clamp01(static_cast<float>(grdNum(c, "Gry ", 0) / 100.0));
        stop.kind = 0;
        stop.r = stop.g = stop.b = g;
        return true;
    }
    return false;
}

bool grdGradient(const psd::DescObject& g, GrdGradient& out) {
    out.name = g.textOf("Nm  ");
    out.noise = grdEnumIs(g, "GrdF", "ClNs");
    const psd::DescValue* clrs = g.get("Clrs");
    if (clrs != nullptr && clrs->type == psd::DescValue::List) {
        for (const psd::DescValue& v : clrs->list) {
            if (v.type != psd::DescValue::Object || out.stops.size() >= 1000) continue;
            const psd::DescObject& s = *v.obj;
            GrdStop stop;
            stop.loc = clamp01(static_cast<float>(grdNum(s, "Lctn", 0) / 4096.0));
            stop.mid = clamp01(static_cast<float>(grdNum(s, "Mdpn", 50.0) / 100.0));
            if (grdEnumIs(s, "Type", "FrgC")) {
                stop.kind = 1;
                out.stops.push_back(stop);
            } else if (grdEnumIs(s, "Type", "BckC")) {
                stop.kind = 2;
                out.stops.push_back(stop);
            } else {
                const psd::DescObject* c = grdObj(s, "Clr ");
                if (c != nullptr && grdColor(*c, stop)) {
                    out.stops.push_back(stop);
                }
            }
        }
    }
    const psd::DescValue* trns = g.get("Trns");
    if (trns != nullptr && trns->type == psd::DescValue::List) {
        for (const psd::DescValue& v : trns->list) {
            if (v.type != psd::DescValue::Object || out.opacity.size() >= 1000) continue;
            const psd::DescObject& s = *v.obj;
            GrdOpacityStop o;
            o.loc = clamp01(static_cast<float>(grdNum(s, "Lctn", 0) / 4096.0));
            o.opacity = clamp01(static_cast<float>(grdNum(s, "Opct", 100.0) / 100.0));
            o.mid = clamp01(static_cast<float>(grdNum(s, "Mdpn", 50.0) / 100.0));
            out.opacity.push_back(o);
        }
    }
    return true;
}

}  // namespace

bool parseGrd(const uint8_t* d, size_t n, std::vector<GrdGradient>& out, int& skippedNoise,
              std::string& err) {
    skippedNoise = 0;
    if (n < 6) {
        err = "grd: file too short";
        return false;
    }
    if (std::memcmp(d, "8BGR", 4) != 0) {
        err = "grd: bad signature (need 8BGR)";
        return false;
    }
    const uint16_t version = static_cast<uint16_t>((d[4] << 8) | d[5]);
    if (version != 5) {
        err = "grd: unsupported version " + std::to_string(version) + " (need 5)";
        return false;
    }
    psd::DescObject vd;
    size_t consumed = 0;
    if (!psd::parseVersionedDescriptor(d + 6, n - 6, vd, &consumed, err)) {
        err = "grd: " + err;
        return false;
    }
    const psd::DescValue* list = vd.get("GrdL");
    if (list == nullptr || list->type != psd::DescValue::List) {
        err = "grd: no GrdL list";
        return false;
    }
    for (const psd::DescValue& v : list->list) {
        if (v.type != psd::DescValue::Object) continue;
        const psd::DescObject* grad = grdObj(*v.obj, "Grad");
        if (grad == nullptr) continue;
        GrdGradient g;
        if (!grdGradient(*grad, g)) continue;
        if (g.noise) {
            skippedNoise++;
            continue;
        }
        out.push_back(std::move(g));
    }
    if (out.empty()) {
        err = skippedNoise > 0 ? "grd: only noise gradients (unsupported in v1)" : "grd: no gradients";
        return false;
    }
    return true;
}

bool writeGrd(const std::vector<GrdGradient>& gs, std::vector<uint8_t>& out, std::string& err) {
    using namespace psd;
    auto mkStop = [&](const GrdStop& s) {
        DescObject d;
        d.classId = DescId::str("Clrt");
        if (s.kind == 0) {
            DescObject rgb;
            rgb.classId = DescId::str("RGBC");
            DescValue dr;
            dr.type = DescValue::Double;
            dr.dbl = s.r * 255.0;
            rgb.set("Rd  ", dr);
            DescValue dg = dr;
            dg.dbl = s.g * 255.0;
            rgb.set("Grn ", dg);
            DescValue db = dr;
            db.dbl = s.b * 255.0;
            rgb.set("Bl  ", db);
            DescValue clr;
            clr.type = DescValue::Object;
            clr.obj = std::make_shared<DescObject>(std::move(rgb));
            d.set("Clr ", clr);
        }
        DescValue ty;
        ty.type = DescValue::Enumerated;
        ty.enumType = DescId::str("Clry");
        ty.enumValue = DescId::str(s.kind == 1 ? "FrgC" : (s.kind == 2 ? "BckC" : "UsrS"));
        d.set("Type", ty);
        DescValue loc;
        loc.type = DescValue::Integer;
        loc.integer = static_cast<int64_t>(std::lround(s.loc * 4096.0));
        d.set("Lctn", loc);
        DescValue mid;
        mid.type = DescValue::Integer;
        mid.integer = static_cast<int64_t>(std::lround(s.mid * 100.0));
        d.set("Mdpn", mid);
        return d;
    };
    auto mkTrn = [&](const GrdOpacityStop& s) {
        DescObject d;
        d.classId = DescId::str("TrnS");
        DescValue op;
        op.type = DescValue::UnitFloat;
        std::memcpy(op.unit, "#Prc", 4);
        op.dbl = s.opacity * 100.0;
        d.set("Opct", op);
        DescValue loc;
        loc.type = DescValue::Integer;
        loc.integer = static_cast<int64_t>(std::lround(s.loc * 4096.0));
        d.set("Lctn", loc);
        DescValue mid;
        mid.type = DescValue::Integer;
        mid.integer = static_cast<int64_t>(std::lround(s.mid * 100.0));
        d.set("Mdpn", mid);
        return d;
    };
    std::vector<DescValue> items;
    for (const GrdGradient& g : gs) {
        DescObject grad;
        grad.classId = DescId::str("Grdn");
        DescValue name;
        name.type = DescValue::Text;
        name.text = psd::utf8ToUtf16(g.name + std::string(1, '\0'));
        grad.set("Nm  ", name);
        DescValue form;
        form.type = DescValue::Enumerated;
        form.enumType = DescId::str("GrdF");
        form.enumValue = DescId::str("CstS");
        grad.set("GrdF", form);
        DescValue intr;
        intr.type = DescValue::Double;
        intr.dbl = 4096.0;
        grad.set("Intr", intr);
        std::vector<DescValue> stops;
        for (const GrdStop& s : g.stops) {
            DescValue v;
            v.type = DescValue::Object;
            v.obj = std::make_shared<DescObject>(mkStop(s));
            stops.push_back(std::move(v));
        }
        DescValue clrs;
        clrs.type = DescValue::List;
        clrs.list = std::move(stops);
        grad.set("Clrs", clrs);
        std::vector<DescValue> trns;
        for (const GrdOpacityStop& s : g.opacity) {
            DescValue v;
            v.type = DescValue::Object;
            v.obj = std::make_shared<DescObject>(mkTrn(s));
            trns.push_back(std::move(v));
        }
        DescValue tv;
        tv.type = DescValue::List;
        tv.list = std::move(trns);
        grad.set("Trns", tv);
        // GrdL 项 = 外层 Grdn 包一层 "Grad" 键（photocraft write 同构）
        DescObject outer;
        outer.classId = DescId::str("Grdn");
        DescValue gv;
        gv.type = DescValue::Object;
        gv.obj = std::make_shared<DescObject>(std::move(grad));
        outer.set("Grad", gv);
        DescValue wrap;
        wrap.type = DescValue::Object;
        wrap.obj = std::make_shared<DescObject>(std::move(outer));
        items.push_back(std::move(wrap));
    }
    DescObject root;
    root.classId = DescId::str("null");
    DescValue list;
    list.type = DescValue::List;
    list.list = std::move(items);
    root.set("GrdL", list);
    out.clear();
    out.insert(out.end(), {'8', 'B', 'G', 'R'});
    putU16(out, 5);
    psd::writeVersionedDescriptor(root, out);
    return true;
}

// ---------- .pat ----------

namespace {

// VMA 通道数组解码：从 [depth u32][rect 4×i32][depth u16][compression u8][data] 读起
//（written/alen 两字段由调用方消费）。解码到通道矩形平面后放置到全图案平面 dst。
bool patDecodeChannel(Rd& v, uint32_t w, uint32_t h, std::vector<uint8_t>& dst) {
    uint32_t d32 = 0;
    int32_t ct = 0, cl = 0, cb = 0, cr = 0;
    uint16_t d16 = 0;
    uint8_t comp = 0;
    if (!v.u32(d32) || !v.i32(ct) || !v.i32(cl) || !v.i32(cb) || !v.i32(cr) || !v.u16(d16) ||
        !v.u8(comp)) {
        return false;
    }
    const uint16_t depth = (d16 == 1 || d16 == 8 || d16 == 16 || d16 == 32)
                               ? d16
                               : static_cast<uint16_t>(d32);
    if (depth == 1 || (depth != 8 && depth != 16 && depth != 32)) {
        return false;  // 1 位 v1 不支持；其余未知深度拒绝
    }
    if (ct < 0 || cl < 0 || cb < ct || cr < cl || static_cast<uint32_t>(cb) > h ||
        static_cast<uint32_t>(cr) > w) {
        return false;
    }
    const uint32_t cw = static_cast<uint32_t>(cr - cl);
    const uint32_t ch = static_cast<uint32_t>(cb - ct);
    const size_t bytesPer = depth / 8;
    const size_t rowBytes = static_cast<size_t>(cw) * bytesPer;
    const size_t planeBytes = static_cast<size_t>(cw) * ch * bytesPer;
    std::vector<uint8_t> plane(planeBytes, 0);
    if (comp == 0) {
        if (v.left() < planeBytes) {
            return false;  // raw 数据不足
        }
        const uint8_t* data = nullptr;
        v.take(planeBytes, &data);
        std::memcpy(plane.data(), data, planeBytes);
    } else if (comp == 1) {
        // RLE：行表 = ch × u16 BE；逐行 PackBits 顺序填满整个通道平面
        std::vector<uint32_t> counts(ch);
        for (uint32_t i = 0; i < ch; ++i) {
            uint16_t c = 0;
            if (!v.u16(c)) return false;
            counts[i] = c;
        }
        for (uint32_t y = 0; y < ch; ++y) {
            if (v.left() < counts[y]) return false;
            const uint8_t* row = nullptr;
            v.take(counts[y], &row);
            std::vector<uint8_t> dst(rowBytes, 0);
            if (!packBitsDecode(row, counts[y], rowBytes, dst)) {
                return false;
            }
            std::memcpy(plane.data() + static_cast<size_t>(y) * rowBytes, dst.data(), rowBytes);
        }
    } else {
        return false;  // ZIP 等 v1 不支持（photocraft 同拒绝）
    }
    // 放置到全图案平面 dst
    for (uint32_t y = 0; y < ch; ++y) {
        std::memcpy(dst.data() + (static_cast<size_t>(ct + y) * w + cl) * bytesPer,
                    plane.data() + static_cast<size_t>(y) * rowBytes, rowBytes);
    }
    return true;
}

// 通道平面（w×h×bytesPer 大端样本）→ 0..1 线性值表（16 位取高字节/65535，32 位 float 钳制）
void patPlaneToUnit(const uint8_t* p, size_t n, uint16_t depth, std::vector<float>& out) {
    out.resize(n);
    if (depth == 8) {
        for (size_t i = 0; i < n; ++i) out[i] = p[i] / 255.0f;
    } else if (depth == 16) {
        for (size_t i = 0; i < n; ++i) {
            out[i] = static_cast<float>(((p[i * 2] << 8) | p[i * 2 + 1])) / 65535.0f;
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            uint32_t u = (p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
            float f;
            std::memcpy(&f, &u, 4);
            if (!std::isfinite(f)) f = 0.0f;
            out[i] = clamp01(f);
        }
    }
}

}  // namespace

bool parsePat(const uint8_t* d, size_t n, std::vector<PatTile>& out, std::string& err) {
    Rd r{d, n, 0, true};
    const uint8_t* sig;
    if (!r.take(4, &sig) || std::memcmp(sig, "8BPT", 4) != 0) {
        err = "pat: bad signature (need 8BPT)";
        return false;
    }
    uint16_t version = 0;
    uint32_t count = 0;
    if (!r.u16(version) || !r.u32(count)) {
        err = "pat: truncated header";
        return false;
    }
    if (count > 100000) {
        err = "pat: too many patterns";
        return false;
    }
    for (uint32_t pi = 0; pi < count && r.left() > 0; ++pi) {
        uint32_t ver = 0, mode = 0;
        uint16_t h16 = 0, w16 = 0;
        uint32_t nameUnits = 0;
        if (!r.u32(ver) || ver != 1) {
            err = "pat: unsupported pattern version";
            return false;
        }
        if (!r.u32(mode) || !r.u16(h16) || !r.u16(w16) || !r.u32(nameUnits)) {
            err = "pat: truncated pattern header";
            return false;
        }
        if (nameUnits > 65536) {
            err = "pat: name too long";
            return false;
        }
        const uint8_t* nameBuf;
        if (!r.take(static_cast<size_t>(nameUnits) * 2, &nameBuf)) {
            err = "pat: truncated name";
            return false;
        }
        PatTile tile;
        tile.name = utf16beToUtf8(nameBuf, nameUnits);
        uint8_t idLen = 0;
        if (!r.u8(idLen)) {
            err = "pat: truncated id length";
            return false;
        }
        const uint8_t* idBuf;
        if (!r.take(idLen, &idBuf)) {
            err = "pat: truncated id";
            return false;
        }
        tile.id.assign(reinterpret_cast<const char*>(idBuf), idLen);
        std::vector<uint8_t> palette;
        if (mode == 2) {
            const uint8_t* pal;
            if (!r.take(768, &pal)) {
                err = "pat: truncated palette";
                return false;
            }
            palette.assign(pal, pal + 768);
            // 部分写手在调色板后跟 4 字节（ag-psd 同跳）
            if (r.left() >= 4 && !(r.d[r.p] == 0 && r.d[r.p + 1] == 0 && r.d[r.p + 2] == 0 && r.d[r.p + 3] == 3)) {
                r.p += 4;
            }
        }
        uint32_t vver = 0, vlen = 0;
        if (!r.u32(vver) || vver != 3) {
            err = "pat: bad virtual memory array version";
            return false;
        }
        if (!r.u32(vlen) || r.left() < vlen) {
            err = "pat: truncated virtual memory array";
            return false;
        }
        Rd v{r.d + r.p, vlen, 0, true};
        r.p += vlen;
        int32_t top = 0, left = 0, bottom = 0, right = 0;
        if (!v.i32(top) || !v.i32(left) || !v.i32(bottom) || !v.i32(right)) {
            err = "pat: truncated rectangle";
            return false;
        }
        const int64_t w64 = static_cast<int64_t>(right) - left;
        const int64_t h64 = static_cast<int64_t>(bottom) - top;
        if (w64 <= 0 || h64 <= 0 || w64 > kPatMaxEdge || h64 > kPatMaxEdge) {
            err = "pat: pattern size out of range (v1 cap " + std::to_string(kPatMaxEdge) + ")";
            return false;
        }
        tile.w = static_cast<uint32_t>(w64);
        tile.h = static_cast<uint32_t>(h64);
        uint32_t slots = 0;
        if (!v.u32(slots) || slots > 64) {
            err = "pat: bad channel count";
            return false;
        }
        // 读全部已写通道（前 nc = 色通道，下一个已写 = alpha；photocraft/ag-psd 同规则）。
        // 数组布局：written u32 / alen u32 / depth u32 / rect 4×i32 / depth u16 / comp u8 / data。
        struct Chan {
            std::vector<uint8_t> raw;  // 解码后的全图案平面（w×h×bytes，通道外零填充）
            uint32_t cw = 0, ch = 0;
            uint16_t depth = 8;
            int32_t x = 0, y = 0;
        };
        std::vector<Chan> chans;
        uint16_t depth = 8;
        size_t totalBytes = 0;
        for (uint32_t si = 0; si < slots + 2; ++si) {
            if (v.left() < 4) break;
            uint32_t written = 0, alen = 0;
            if (!v.u32(written)) return false;
            if (written == 0) continue;
            if (!v.u32(alen) || alen < 23 || v.left() < alen - 23) {
                err = "pat: bad channel array length";
                return false;
            }
            // 预读 23 字节头做预算校验后回退，统一由 patDecodeChannel 消费
            const size_t headerAt = v.p;
            uint32_t d32 = 0;
            int32_t ct = 0, cl = 0, cb = 0, cr = 0;
            uint16_t d16 = 0;
            uint8_t comp = 0;
            if (!v.u32(d32) || !v.i32(ct) || !v.i32(cl) || !v.i32(cb) || !v.i32(cr) || !v.u16(d16) ||
                !v.u8(comp)) {
                err = "pat: truncated channel";
                return false;
            }
            v.p = headerAt;
            depth = (d16 == 1 || d16 == 8 || d16 == 16 || d16 == 32) ? d16 : static_cast<uint16_t>(d32);
            if (depth == 1) {
                err = "pat: 1-bit patterns not supported in v1";
                return false;
            }
            if (ct < top || cl < left || cb > bottom || cr > right || cb < ct || cr < cl) {
                err = "pat: channel rectangle outside pattern";
                return false;
            }
            Chan c;
            c.cw = static_cast<uint32_t>(cr - cl);
            c.ch = static_cast<uint32_t>(cb - ct);
            c.depth = depth;
            c.x = cl - left;
            c.y = ct - top;
            totalBytes += static_cast<size_t>(c.cw) * c.ch * (depth / 8);
            if (totalBytes > (static_cast<size_t>(64) << 20)) {
                err = "pat: channel data exceeds budget";
                return false;
            }
            c.raw.assign(static_cast<size_t>(tile.w) * tile.h * (depth / 8), 0);
            if (!patDecodeChannel(v, tile.w, tile.h, c.raw)) {
                err = "pat: channel decode failed";
                return false;
            }
            chans.push_back(std::move(c));
            if (chans.size() > 64) {
                err = "pat: too many channels";
                return false;
            }
        }
        const size_t nc = (mode == 3 || mode == 9) ? 3 : (mode == 4 ? 4 : 1);
        if (chans.size() < nc) {
            err = "pat: fewer channels than colour mode";
            return false;
        }
        // 平面解码到 0..1 值表（通道已在解码时放置到全图案域）
        const size_t np = static_cast<size_t>(tile.w) * tile.h;
        std::vector<std::vector<float>> planes(nc + (chans.size() > nc ? 1 : 0));
        for (size_t ci = 0; ci < chans.size() && ci < planes.size(); ++ci) {
            planes[ci].assign(np, 0.0f);
            patPlaneToUnit(chans[ci].raw.data(), np, chans[ci].depth, planes[ci]);
        }
        tile.rgba.assign(np * 4, 0);
        for (size_t i = 0; i < np; ++i) {
            float rgb[3];
            if (mode == 3 || mode == 9) {
                rgb[0] = planes[0][i];
                rgb[1] = planes[1][i];
                rgb[2] = planes[2][i];
            } else if (mode == 4) {
                // CMYK 平面反相存储（0 = 全墨）
                const float cmyk[4] = {1.0f - planes[0][i], 1.0f - planes[1][i],
                                       1.0f - planes[2][i], 1.0f - planes[3][i]};
                cmykToRgbNaive(cmyk, rgb);
            } else if (mode == 2) {
                const unsigned idx = static_cast<unsigned>(planes[0][i] * 255.0f + 0.5f) * 3;
                rgb[0] = palette.size() > idx + 2 ? palette[idx] / 255.0f : 0.0f;
                rgb[1] = palette.size() > idx + 2 ? palette[idx + 1] / 255.0f : 0.0f;
                rgb[2] = palette.size() > idx + 2 ? palette[idx + 2] / 255.0f : 0.0f;
            } else {
                rgb[0] = rgb[1] = rgb[2] = planes[0][i];  // Gray/多通道 = 灰度
            }
            tile.rgba[i * 4 + 0] = static_cast<uint8_t>(std::lround(clamp01(rgb[0]) * 255.0f));
            tile.rgba[i * 4 + 1] = static_cast<uint8_t>(std::lround(clamp01(rgb[1]) * 255.0f));
            tile.rgba[i * 4 + 2] = static_cast<uint8_t>(std::lround(clamp01(rgb[2]) * 255.0f));
            tile.rgba[i * 4 + 3] =
                planes.size() > nc ? static_cast<uint8_t>(std::lround(planes[nc][i] * 255.0f)) : 255;
        }
        out.push_back(std::move(tile));
    }
    if (out.empty()) {
        err = "pat: no patterns";
        return false;
    }
    return true;
}

bool writePat(const std::vector<PatTile>& ts, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    out.insert(out.end(), {'8', 'B', 'P', 'T'});
    putU16(out, 1);
    putU32(out, static_cast<uint32_t>(ts.size()));
    for (const PatTile& t : ts) {
        if (t.w == 0 || t.h == 0 || t.w > kPatMaxEdge || t.h > kPatMaxEdge) {
            err = "pat: tile size out of range";
            return false;
        }
        putU32(out, 1);   // version
        putU32(out, 3);   // mode RGB
        putU16(out, static_cast<uint16_t>(t.h));
        putU16(out, static_cast<uint16_t>(t.w));
        std::vector<uint8_t> units;
        appendUtf16be(units, t.name, true);
        putU32(out, static_cast<uint32_t>(units.size() / 2));
        out.insert(out.end(), units.begin(), units.end());
        const std::string id = t.id.empty() ? std::string("montage") : t.id;
        out.push_back(static_cast<uint8_t>(std::min<size_t>(id.size(), 255)));
        out.insert(out.end(), id.begin(), id.begin() + std::min<size_t>(id.size(), 255));
        // VMA：rect + 24 槽（3 色通道 + alpha + 未用槽）——photocraft write_pattern 同布局
        std::vector<uint8_t> body;
        const int32_t rect[4] = {0, 0, static_cast<int32_t>(t.h), static_cast<int32_t>(t.w)};
        for (int32_t v : rect) putU32(body, static_cast<uint32_t>(v));
        putU32(body, 24);
        auto putArray = [&](const uint8_t* plane, size_t bytes) {
            (void)bytes;
            // RLE 流 = h × u16 行长表 + 逐行 PackBits（photocraft encode_rle 同布局）
            std::vector<uint8_t> enc;
            const size_t rowBytes = static_cast<size_t>(t.w);
            std::vector<uint16_t> counts;
            std::vector<uint8_t> rows;
            for (uint32_t y = 0; y < t.h; ++y) {
                const size_t before = rows.size();
                packBitsEncode(plane + static_cast<size_t>(y) * rowBytes, rowBytes, rows);
                counts.push_back(static_cast<uint16_t>(rows.size() - before));
            }
            for (uint16_t c : counts) {
                putU16(enc, c);
            }
            enc.insert(enc.end(), rows.begin(), rows.end());
            putU32(body, 1);  // written
            putU32(body, static_cast<uint32_t>(23 + enc.size()));
            putU32(body, 8);  // depth
            for (int32_t v : rect) putU32(body, static_cast<uint32_t>(v));
            putU16(body, 8);
            body.push_back(1);  // RLE
            body.insert(body.end(), enc.begin(), enc.end());
        };
        for (int c = 0; c < 3; ++c) {
            // RGBA → 平面（RGB 大端 8 位）
            std::vector<uint8_t> plane(static_cast<size_t>(t.w) * t.h);
            for (size_t i = 0; i < plane.size(); ++i) {
                plane[i] = t.rgba[i * 4 + c];
            }
            putArray(plane.data(), plane.size());
        }
        {
            std::vector<uint8_t> plane(static_cast<size_t>(t.w) * t.h);
            for (size_t i = 0; i < plane.size(); ++i) {
                plane[i] = t.rgba[i * 4 + 3];
            }
            putArray(plane.data(), plane.size());
        }
        for (int i = 0; i < 21; ++i) putU32(body, 0);  // 剩余色槽（3 色 + alpha + 21 空 = 24 槽）
        putU32(body, 0);  // sheet mask 槽
        putU32(out, 3);   // VMA version
        putU32(out, static_cast<uint32_t>(body.size()));
        out.insert(out.end(), body.begin(), body.end());
    }
    return true;
}

// ---------- .abr ----------

namespace {

struct AbrBudget {
    size_t left;
    bool spend(size_t k) {
        if (k > left) return false;
        left -= k;
        return true;
    }
};

// 采样位图主体（bounds 4×i32 + depth u16 + comp u8 + 数据——legacy 与 samp 共用）
bool abrSampleBody(Rd& b, const std::string& id, AbrBudget& budget, std::vector<AbrSample>& out,
                   std::string& err) {
    int32_t top = 0, left = 0, bottom = 0, right = 0;
    if (!b.i32(top) || !b.i32(left) || !b.i32(bottom) || !b.i32(right)) {
        err = "abr: truncated sample bounds";
        return false;
    }
    const int64_t w = static_cast<int64_t>(right) - left;
    const int64_t h = static_cast<int64_t>(bottom) - top;
    if (w <= 0 || h <= 0 || w > kAbrMaxEdge || h > kAbrMaxEdge) {
        err = "abr: sample size out of range";
        return false;
    }
    uint16_t depth = 0;
    if (!b.u16(depth) || (depth != 8 && depth != 16)) {
        err = "abr: bad sample depth";
        return false;
    }
    uint8_t comp = 0;
    if (!b.u8(comp) || comp > 1) {
        err = "abr: bad sample compression";
        return false;
    }
    const size_t planeBytes = static_cast<size_t>(w) * h * (depth / 8);
    if (!budget.spend(planeBytes)) {
        err = "abr: samples exceed budget";
        return false;
    }
    AbrSample s;
    s.id = id;
    s.w = static_cast<uint32_t>(w);
    s.h = static_cast<uint32_t>(h);
    s.depth = depth;
    s.data.assign(planeBytes, 0);
    if (comp == 0) {
        if (b.left() < planeBytes) {
            err = "abr: truncated raw sample";
            return false;
        }
        const uint8_t* src;
        b.take(planeBytes, &src);
        std::memcpy(s.data.data(), src, planeBytes);
    } else {
        // RLE：行表 = h × u16 BE
        std::vector<uint32_t> counts(h);
        for (uint32_t i = 0; i < h; ++i) {
            uint16_t c = 0;
            if (!b.u16(c)) {
                err = "abr: truncated RLE table";
                return false;
            }
            counts[i] = c;
        }
        const size_t rowBytes = static_cast<size_t>(w) * (depth / 8);
        for (uint32_t y = 0; y < h; ++y) {
            if (b.left() < counts[y]) {
                err = "abr: truncated RLE row";
                return false;
            }
            const uint8_t* row;
            b.take(counts[y], &row);
            std::vector<uint8_t> dst(rowBytes, 0);
            if (!packBitsDecode(row, counts[y], rowBytes, dst)) {
                err = "abr: RLE decode failed";
                return false;
            }
            std::memcpy(s.data.data() + static_cast<size_t>(y) * rowBytes, dst.data(), rowBytes);
        }
    }
    out.push_back(std::move(s));
    return true;
}

bool abrUcs2(Rd& b, std::string& out, std::string& err) {
    uint32_t units = 0;
    if (!b.u32(units) || units > 4096) {
        err = "abr: bad name length";
        return false;
    }
    const uint8_t* s;
    if (!b.take(static_cast<size_t>(units) * 2, &s)) {
        err = "abr: truncated name";
        return false;
    }
    out = utf16beToUtf8(s, units);
    return true;
}

// v6 desc 描述符预设 → AbrBrush（abr_map preset 的参数面：Nm/Brsh(Dmtr/Hrdn/Spcn/Angl/Rndn/
// sampledData)/toolOptions(Opct/flow)；动态/纹理/双笔刷等无对应参数 → warnings）
void abrMapPreset(const psd::DescObject& d, const std::vector<AbrSample>& samples, size_t selfIdx,
                  AbrFile& f, AbrBrush& out) {
    out.name = d.textOf("Nm  ");
    if (out.name.empty()) {
        out.name = "Brush " + std::to_string(selfIdx + 1);
    }
    const psd::DescValue* brshV = d.get("Brsh");
    if (brshV == nullptr ||
        (brshV->type != psd::DescValue::Object && brshV->type != psd::DescValue::GlobalObject)) {
        f.warnings.push_back("abr: preset \"" + out.name + "\" has no tip; skipped");
        out.diameter = 0;
        return;
    }
    const psd::DescObject& t = *brshV->obj;
    const bool sampled = t.classId.is("sampledBrush") || t.get("sampledData") != nullptr;
    out.sampled = sampled;
    if (sampled) {
        const std::string sid = t.textOf("sampledData");
        size_t found = samples.size();
        for (size_t i = 0; i < samples.size(); ++i) {
            if (!sid.empty() && samples[i].id == sid) {
                found = i;
                break;
            }
        }
        if (found == samples.size() && samples.size() == 1) found = 0;
        if (found == samples.size()) {
            f.warnings.push_back("abr: \"" + out.name + "\" sampled tip missing; skipped");
            out.diameter = 0;
            return;
        }
        out.sampleIndex = found;
        out.diameter = static_cast<float>(std::max(samples[found].w, samples[found].h));
    }
    const double dmtr = grdNum(t, "Dmtr", 0);
    if (dmtr > 0) {
        out.diameter = static_cast<float>(std::clamp(dmtr, 1.0, 5000.0));
    }
    if (t.get("Hrdn") != nullptr) {
        out.hardness = clamp01(static_cast<float>(grdNum(t, "Hrdn", 100.0) / 100.0));
    }
    out.spacing = clamp01(static_cast<float>(grdNum(t, "Spcn", 25.0) / 100.0));
    out.angle = static_cast<float>(grdNum(t, "Angl", 0.0));
    out.roundness = std::clamp(static_cast<float>(grdNum(t, "Rndn", 100.0) / 100.0), 0.01f, 1.0f);
    const psd::DescObject* to = grdObj(d, "toolOptions");
    if (to != nullptr) {
        if (to->get("Opct") != nullptr) {
            out.opacity = clamp01(static_cast<float>(grdNum(*to, "Opct", 100.0) / 100.0));
        }
        if (to->get("flow") != nullptr) {
            out.flow = clamp01(static_cast<float>(grdNum(*to, "flow", 100.0) / 100.0));
        }
    }
}

}  // namespace

bool parseAbr(const uint8_t* d, size_t n, AbrFile& out, std::string& err) {
    Rd r{d, n, 0, true};
    uint16_t version = 0;
    if (!r.u16(version)) {
        err = "abr: empty file";
        return false;
    }
    out.version = version;
    AbrBudget budget{(static_cast<size_t>(512) << 20)};
    if (version == 1 || version == 2) {
        uint16_t count = 0;
        if (!r.u16(count)) {
            err = "abr: truncated count";
            return false;
        }
        for (uint16_t i = 0; i < count; ++i) {
            if (r.left() == 0) {
                out.warnings.push_back("abr: file ends after " + std::to_string(i) + " brushes");
                break;
            }
            uint16_t ty = 0;
            uint32_t len = 0;
            if (!r.u16(ty) || !r.u32(len)) {
                err = "abr: truncated brush header";
                return false;
            }
            if (r.left() < len) {
                out.warnings.push_back("abr: brush " + std::to_string(i + 1) + " truncated");
                break;
            }
            Rd b{r.d + r.p, len, 0, true};
            r.p += len;
            uint32_t misc = 0;
            uint16_t spacing = 0;
            if (!b.u32(misc) || !b.u16(spacing)) {
                err = "abr: truncated brush body";
                return false;
            }
            AbrBrush br;
            br.spacing = spacing == 0 ? 0.25f
                                      : std::clamp(static_cast<float>(spacing) / 100.0f, 0.01f, 10.0f);
            if (ty == 1) {
                uint16_t diameter = 0, roundness = 0, angle = 0, hardness = 0;
                if (!b.u16(diameter) || !b.u16(roundness) || !b.u16(angle) || !b.u16(hardness)) {
                    err = "abr: truncated computed tip";
                    return false;
                }
                br.name = std::to_string(diameter) + " px Round";
                br.diameter = static_cast<float>(std::max<uint16_t>(diameter, 1));
                br.hardness = clamp01(static_cast<float>(hardness) / 100.0f);
                br.angle = static_cast<float>(static_cast<int16_t>(angle));
                br.roundness = std::clamp(static_cast<float>(roundness) / 100.0f, 0.01f, 1.0f);
                out.brushes.push_back(std::move(br));
            } else if (ty == 2) {
                if (version == 2) {
                    if (!abrUcs2(b, br.name, err)) return false;
                }
                uint8_t aa = 0;
                if (!b.u8(aa)) {
                    err = "abr: truncated sampled header";
                    return false;
                }
                if (!b.take(8, nullptr)) {
                    err = "abr: truncated short bounds";
                    return false;
                }
                br.sampled = true;
                br.name = br.name.empty() ? "Sampled Brush " + std::to_string(out.brushes.size() + 1)
                                          : br.name;
                const size_t idx = out.samples.size();
                if (!abrSampleBody(b, "", budget, out.samples, err)) {
                    // 坏笔刷跳过（photocraft warnings 同语义）
                    out.warnings.push_back("abr: brush " + std::to_string(i + 1) + ": " + err);
                    err.clear();
                    continue;
                }
                br.sampleIndex = idx;
                br.diameter = static_cast<float>(std::max(out.samples[idx].w, out.samples[idx].h));
                out.brushes.push_back(std::move(br));
            } else {
                out.warnings.push_back("abr: brush " + std::to_string(i + 1) + ": unknown type " +
                                       std::to_string(ty));
            }
        }
    } else if (version >= 6 && version <= 10) {
        uint16_t subversion = 0;
        if (!r.u16(subversion) || (subversion != 1 && subversion != 2)) {
            err = "abr: bad subversion";
            return false;
        }
        std::vector<uint8_t> descData;
        bool hasDesc = false;
        while (r.left() >= 12) {
            const uint8_t* sig = nullptr;
            const uint8_t* key = nullptr;
            uint32_t len = 0;
            if (!r.take(4, &sig) || !r.take(4, &key) || !r.u32(len)) {
                err = "abr: truncated section header";
                return false;
            }
            if (std::memcmp(sig, "8BIM", 4) != 0) {
                out.warnings.push_back("abr: unexpected section data ignored");
                break;
            }
            const uint8_t* sec = nullptr;
            const uint32_t take = std::min<uint32_t>(len, static_cast<uint32_t>(r.left()));
            r.take(take, &sec);
            // 节按 4 字节对齐；容忍不写 pad 的写手（photocraft 同款探测）
            const size_t pad = (4 - len % 4) % 4;
            if (pad > 0 && r.left() >= pad + 4) {
                if (std::memcmp(r.d + r.p, "8BIM", 4) != 0 &&
                    std::memcmp(r.d + r.p + pad, "8BIM", 4) == 0) {
                    r.p += pad;
                }
            }
            if (std::memcmp(key, "samp", 4) == 0) {
                Rd s{sec, take, 0, true};
                while (s.left() >= 4) {
                    uint32_t slen = 0;
                    if (!s.u32(slen) || slen == 0) break;
                    const size_t padded = (slen + 3u) & ~3u;
                    if (s.left() < std::min<size_t>(padded, slen)) {
                        out.warnings.push_back("abr: sampled tip truncated");
                        break;
                    }
                    Rd body{s.d + s.p, slen, 0, true};
                    s.p += std::min<size_t>(padded, s.left());
                    uint8_t idLen = 0;
                    if (!body.u8(idLen)) {
                        out.warnings.push_back("abr: sampled tip id truncated");
                        break;
                    }
                    const uint8_t* idBuf;
                    if (!body.take(idLen, &idBuf)) {
                        out.warnings.push_back("abr: sampled tip id truncated");
                        break;
                    }
                    if (!body.take(subversion == 1 ? 10 : 264, nullptr)) {
                        out.warnings.push_back("abr: sampled tip header truncated");
                        break;
                    }
                    const size_t idx = out.samples.size();
                    std::string sid(reinterpret_cast<const char*>(idBuf), idLen);
                    if (!abrSampleBody(body, sid, budget, out.samples, err)) {
                        out.warnings.push_back("abr: sampled tip skipped: " + err);
                        err.clear();
                        continue;
                    }
                }
            } else if (std::memcmp(key, "desc", 4) == 0) {
                descData.assign(sec, sec + take);
                hasDesc = true;
            }
            // patt/lPdc/phry 等 v1 忽略（层级/纹理登记）
        }
        if (hasDesc) {
            psd::DescObject vd;
            size_t consumed = 0;
            std::string derr;
            if (psd::parseVersionedDescriptor(descData.data(), descData.size(), vd, &consumed, derr)) {
                const psd::DescValue* list = vd.get("Brsh");
                if (list != nullptr && list->type == psd::DescValue::List) {
                    size_t idx = 0;
                    for (const psd::DescValue& v : list->list) {
                        if (v.type != psd::DescValue::Object) continue;
                        AbrBrush br;
                        abrMapPreset(*v.obj, out.samples, idx, out, br);
                        if (br.diameter > 0) {
                            out.brushes.push_back(std::move(br));
                        }
                        idx++;
                    }
                } else {
                    out.warnings.push_back("abr: desc holds no brush list");
                }
            } else {
                out.warnings.push_back("abr: desc unreadable: " + derr);
            }
        }
        if (out.brushes.empty() && !out.samples.empty()) {
            // 早期 v6：仅有采样位图——每 tip 一预设（abr_map 同语义）
            for (size_t i = 0; i < out.samples.size(); ++i) {
                AbrBrush br;
                br.name = "Sampled Brush " + std::to_string(i + 1);
                br.sampled = true;
                br.sampleIndex = i;
                br.diameter = static_cast<float>(std::max(out.samples[i].w, out.samples[i].h));
                out.brushes.push_back(std::move(br));
            }
        }
    } else {
        err = "abr: unsupported version " + std::to_string(version);
        return false;
    }
    if (out.brushes.empty()) {
        err = "abr: no readable brushes";
        return false;
    }
    return true;
}

}  // namespace presets
}  // namespace montage
