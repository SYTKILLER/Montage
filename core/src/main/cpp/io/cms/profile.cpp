#include "io/cms/profile.h"

#include <algorithm>
#include <cstring>
#include <map>

#include "io/cms/builtin.h"
#include "io/cms/transform.h"

namespace montage {
namespace cms {

namespace {

// ---------- 字节读取器（对拍 profile.rs Reader） ----------

struct Reader {
    const uint8_t* b = nullptr;
    size_t len = 0;

    bool slice(size_t off, size_t count, const uint8_t** out) const {
        if (off + count > len || off + count < off) {
            return false;
        }
        *out = b + off;
        return true;
    }
    std::optional<uint8_t> u8(size_t off) const {
        if (off + 1 > len) {
            return std::nullopt;
        }
        return b[off];
    }
    std::optional<uint16_t> u16(size_t off) const {
        if (off + 2 > len) {
            return std::nullopt;
        }
        return static_cast<uint16_t>((b[off] << 8) | b[off + 1]);
    }
    std::optional<uint32_t> u32(size_t off) const {
        if (off + 4 > len) {
            return std::nullopt;
        }
        return static_cast<uint32_t>(b[off]) << 24 | static_cast<uint32_t>(b[off + 1]) << 16 |
               static_cast<uint32_t>(b[off + 2]) << 8 | b[off + 3];
    }
    std::optional<double> s15f16(size_t off) const {
        const auto v = u32(off);
        if (!v) {
            return std::nullopt;
        }
        return static_cast<int32_t>(*v) / 65536.0;
    }
    std::optional<std::array<double, 3>> xyz(size_t off) const {
        const auto x = s15f16(off), y = s15f16(off + 4), z = s15f16(off + 8);
        if (!x || !y || !z) {
            return std::nullopt;
        }
        return std::array<double, 3>{*x, *y, *z};
    }
};

uint32_t sigOf(const uint8_t* s) {
    return static_cast<uint32_t>(s[0]) << 24 | static_cast<uint32_t>(s[1]) << 16 |
           static_cast<uint32_t>(s[2]) << 8 | s[3];
}

constexpr uint32_t sigOf(const char (&s)[5]) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[3]));
}

// CLUT 节点 × 输出接受的上限（拒绝恶意文件）。
constexpr size_t kMaxClutValues = 16 * 1024 * 1024;

// 解析 `curv` 或 `para` 元素；返回曲线与元素字节长度（不含填充）。
std::optional<std::pair<Curve, size_t>> parseCurve(const uint8_t* d, size_t len, std::string& err) {
    Reader r{d, len};
    const uint8_t* ty = nullptr;
    if (!r.slice(0, 4, &ty)) {
        err = "ICC profile is truncated";
        return std::nullopt;
    }
    if (std::memcmp(ty, "curv", 4) == 0) {
        const auto n = r.u32(8);
        if (!n) {
            err = "ICC profile is truncated";
            return std::nullopt;
        }
        const size_t count = *n;
        const size_t elemLen = 12 + count * 2;
        if (count == 0) {
            return std::make_pair(Curve::identity(), elemLen);
        }
        if (count == 1) {
            const auto g = r.u16(12);
            if (!g) {
                err = "ICC profile is truncated";
                return std::nullopt;
            }
            return std::make_pair(Curve::gammaValue(*g / 256.0), elemLen);
        }
        if (count > (1u << 20)) {
            err = "invalid ICC profile: curve too long";
            return std::nullopt;
        }
        const uint8_t* s = nullptr;
        if (!r.slice(12, count * 2, &s)) {
            err = "ICC profile is truncated";
            return std::nullopt;
        }
        std::vector<float> t(count);
        for (size_t i = 0; i < count; ++i) {
            t[i] = static_cast<uint16_t>((s[2 * i] << 8) | s[2 * i + 1]) / 65535.0f;
        }
        return std::make_pair(Curve::fromTable(std::move(t)), elemLen);
    }
    if (std::memcmp(ty, "para", 4) == 0) {
        const auto kindV = r.u16(8);
        if (!kindV) {
            err = "ICC profile is truncated";
            return std::nullopt;
        }
        const uint16_t kind = *kindV;
        if (kind > 4) {
            err = "unsupported ICC feature: parametric curve type " + std::to_string(kind);
            return std::nullopt;
        }
        const int n = Curve::paramCount(kind);
        double p[7] = {0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < n; ++i) {
            const auto v = r.s15f16(12 + i * 4);
            if (!v) {
                err = "ICC profile is truncated";
                return std::nullopt;
            }
            p[i] = *v;
        }
        const size_t elemLen = 12 + static_cast<size_t>(n) * 4;
        const Curve c = kind == 0 ? Curve::gammaValue(p[0]) : Curve::parametric(kind, p);
        return std::make_pair(c, elemLen);
    }
    err = "unsupported ICC feature: curve type " + std::string(reinterpret_cast<const char*>(ty), 4);
    return std::nullopt;
}

// 在 d 的 off 处连续解析 n 个曲线元素（4 字节对齐步进）。
std::optional<std::vector<Curve>> parseCurvesAt(const uint8_t* d, size_t len, size_t off, size_t n,
                                                std::string& err) {
    std::vector<Curve> out;
    out.reserve(n);
    size_t o = off;
    for (size_t i = 0; i < n; ++i) {
        if (o > len) {
            err = "ICC profile is truncated";
            return std::nullopt;
        }
        auto sub = parseCurve(d + o, len - o, err);
        if (!sub) {
            return std::nullopt;
        }
        out.push_back(sub->first);
        o += (sub->second + 3) & ~static_cast<size_t>(3);
    }
    return out;
}

std::optional<Lut> parseLut(const uint8_t* d, size_t len, bool a2b, std::string& err) {
    Reader r{d, len};
    const uint8_t* ty = nullptr;
    if (!r.slice(0, 4, &ty)) {
        err = "ICC profile is truncated";
        return std::nullopt;
    }
    const bool mft2 = std::memcmp(ty, "mft2", 4) == 0;
    if (std::memcmp(ty, "mft1", 4) == 0 || mft2) {
        const auto iV = r.u8(8), oV = r.u8(9), gV = r.u8(10);
        if (!iV || !oV || !gV) {
            err = "ICC profile is truncated";
            return std::nullopt;
        }
        const size_t i = *iV, o = *oV, g = *gV;
        if (!(i >= 1 && i <= 15) || !(o >= 1 && o <= 15) || g == 0) {
            err = "invalid ICC profile: bad lut dimensions";
            return std::nullopt;
        }
        Mat3 m{};
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                const auto v = r.s15f16(12 + (a * 3 + b) * 4);
                if (!v) {
                    err = "ICC profile is truncated";
                    return std::nullopt;
                }
                m[a][b] = *v;
            }
        }
        size_t nIn = 256, nOut = 256, off = 48;
        if (mft2) {
            const auto ni = r.u16(48), no = r.u16(50);
            if (!ni || !no) {
                err = "ICC profile is truncated";
                return std::nullopt;
            }
            nIn = *ni;
            nOut = *no;
            off = 52;
            if (!(nIn >= 2 && nIn <= 4096) || !(nOut >= 2 && nOut <= 4096)) {
                err = "invalid ICC profile: bad lut16 table size";
                return std::nullopt;
            }
        }
        const size_t bps = mft2 ? 2 : 1;
        auto readTables = [&](size_t& offRef, size_t count, size_t tableLen,
                              std::string& e) -> std::optional<std::vector<Curve>> {
            std::vector<Curve> v;
            v.reserve(count);
            for (size_t k = 0; k < count; ++k) {
                const uint8_t* s = nullptr;
                if (!r.slice(offRef, tableLen * bps, &s)) {
                    e = "ICC profile is truncated";
                    return std::nullopt;
                }
                std::vector<float> t(tableLen);
                if (mft2) {
                    for (size_t j = 0; j < tableLen; ++j) {
                        t[j] = static_cast<uint16_t>((s[2 * j] << 8) | s[2 * j + 1]) / 65535.0f;
                    }
                } else {
                    for (size_t j = 0; j < tableLen; ++j) {
                        t[j] = s[j] / 255.0f;
                    }
                }
                offRef += tableLen * bps;
                v.push_back(Curve::fromTable(std::move(t)));
            }
            return v;
        };
        auto inCurves = readTables(off, i, nIn, err);
        if (!inCurves) {
            return std::nullopt;
        }
        // nodes = g^i（防溢出）。
        uint64_t nodes = 1;
        bool overflow = false;
        for (size_t k = 0; k < i; ++k) {
            if (nodes > UINT64_MAX / g) {
                overflow = true;
                break;
            }
            nodes *= g;
        }
        if (overflow || nodes > kMaxClutValues / (o > 0 ? o : 1)) {
            err = "invalid ICC profile: CLUT too large";
            return std::nullopt;
        }
        const size_t total = static_cast<size_t>(nodes) * o;
        const uint8_t* s = nullptr;
        if (!r.slice(off, total * bps, &s)) {
            err = "ICC profile is truncated";
            return std::nullopt;
        }
        std::vector<float> data(total);
        if (mft2) {
            for (size_t j = 0; j < total; ++j) {
                data[j] = static_cast<uint16_t>((s[2 * j] << 8) | s[2 * j + 1]) / 65535.0f;
            }
        } else {
            for (size_t j = 0; j < total; ++j) {
                data[j] = s[j] / 255.0f;
            }
        }
        off += total * bps;
        auto outCurves = readTables(off, o, nOut, err);
        if (!outCurves) {
            return std::nullopt;
        }
        std::vector<Stage> stages;
        // 矩阵仅在输入为 XYZ 时有意义（XYZ PCS 的 BToA）；Lab PCS 写端必须存恒等。
        bool ident = true;
        for (int a = 0; a < 3 && ident; ++a) {
            for (int b = 0; b < 3; ++b) {
                if (std::abs(m[a][b] - (a == b ? 1.0 : 0.0)) >= 1e-6) {
                    ident = false;
                    break;
                }
            }
        }
        if (!a2b && i == 3 && !ident) {
            stages.push_back(Stage::matrix3(m));
        }
        stages.push_back(Stage::curvesStage(std::move(*inCurves)));
        stages.push_back(Stage::clutStage(Clut::make(std::vector<size_t>(i, g), o, std::move(data))));
        stages.push_back(Stage::curvesStage(std::move(*outCurves)));
        Lut lut;
        lut.kind = mft2 ? LutKind::Lut16 : LutKind::Lut8;
        lut.inputs = i;
        lut.outputs = o;
        lut.stages = std::move(stages);
        return lut;
    }
    const bool mab = std::memcmp(ty, "mAB ", 4) == 0;
    const bool mba = std::memcmp(ty, "mBA ", 4) == 0;
    if (mab || mba) {
        const auto iV = r.u8(8), oV = r.u8(9);
        if (!iV || !oV) {
            err = "ICC profile is truncated";
            return std::nullopt;
        }
        const size_t i = *iV, o = *oV;
        if (!(i >= 1 && i <= 15) || !(o >= 1 && o <= 15)) {
            err = "invalid ICC profile: bad lutAB dimensions";
            return std::nullopt;
        }
        auto u32At = [&](size_t off) -> std::optional<size_t> {
            const auto v = r.u32(off);
            if (!v) {
                err = "ICC profile is truncated";
                return std::nullopt;
            }
            return static_cast<size_t>(*v);
        };
        const auto ob = u32At(12), om = u32At(16), omc = u32At(20), oc = u32At(24), oa = u32At(28);
        if (!ob || !om || !omc || !oc || !oa) {
            return std::nullopt;
        }
        auto matrixAt = [&](size_t off) -> std::optional<Stage> {
            std::vector<double> m(9, 0.0);
            for (int k = 0; k < 9; ++k) {
                const auto v = r.s15f16(off + k * 4);
                if (!v) {
                    err = "ICC profile is truncated";
                    return std::nullopt;
                }
                m[k] = *v;
            }
            std::vector<double> offset(3, 0.0);
            for (int k = 0; k < 3; ++k) {
                const auto v = r.s15f16(off + 36 + k * 4);
                if (!v) {
                    err = "ICC profile is truncated";
                    return std::nullopt;
                }
                offset[k] = *v;
            }
            return Stage::matrixStage(3, 3, std::move(m), std::move(offset));
        };
        auto clutAt = [&](size_t off, size_t cin, size_t cout) -> std::optional<Stage> {
            std::vector<size_t> grid(cin);
            for (size_t k = 0; k < cin; ++k) {
                const auto g = r.u8(off + k);
                if (!g) {
                    err = "ICC profile is truncated";
                    return std::nullopt;
                }
                if (*g < 1) {
                    err = "invalid ICC profile: zero grid points";
                    return std::nullopt;
                }
                grid[k] = *g;
            }
            const auto precV = r.u8(off + 16);
            if (!precV) {
                err = "ICC profile is truncated";
                return std::nullopt;
            }
            const size_t prec = *precV;
            // 节点数乘积（防溢出）。
            uint64_t nodes = 1;
            for (size_t g : grid) {
                if (nodes > UINT64_MAX / g) {
                    err = "invalid ICC profile: CLUT too large";
                    return std::nullopt;
                }
                nodes *= g;
            }
            if (nodes > kMaxClutValues / (cout > 0 ? cout : 1)) {
                err = "invalid ICC profile: CLUT too large";
                return std::nullopt;
            }
            const size_t total = static_cast<size_t>(nodes) * cout;
            std::vector<float> data(total);
            if (prec == 1) {
                const uint8_t* s = nullptr;
                if (!r.slice(off + 20, total, &s)) {
                    err = "ICC profile is truncated";
                    return std::nullopt;
                }
                for (size_t j = 0; j < total; ++j) {
                    data[j] = s[j] / 255.0f;
                }
            } else if (prec == 2) {
                const uint8_t* s = nullptr;
                if (!r.slice(off + 20, total * 2, &s)) {
                    err = "ICC profile is truncated";
                    return std::nullopt;
                }
                for (size_t j = 0; j < total; ++j) {
                    data[j] = static_cast<uint16_t>((s[2 * j] << 8) | s[2 * j + 1]) / 65535.0f;
                }
            } else {
                err = "invalid ICC profile: CLUT precision " + std::to_string(prec);
                return std::nullopt;
            }
            return Stage::clutStage(Clut::make(std::move(grid), cout, std::move(data)));
        };
        std::vector<Stage> stages;
        if (mab) {
            // A 曲线 → CLUT → M 曲线 → 矩阵 → B 曲线。
            if (*oc != 0) {
                if (*oa == 0) {
                    err = "invalid ICC profile: lutAtoB with CLUT but no A curves";
                    return std::nullopt;
                }
                auto aCurves = parseCurvesAt(d, len, *oa, i, err);
                if (!aCurves) {
                    return std::nullopt;
                }
                stages.push_back(Stage::curvesStage(std::move(*aCurves)));
                auto cl = clutAt(*oc, i, o);
                if (!cl) {
                    return std::nullopt;
                }
                stages.push_back(std::move(*cl));
            } else if (i != o) {
                err = "invalid ICC profile: lutAtoB without CLUT changes channel count";
                return std::nullopt;
            }
            if (*om != 0) {
                auto mCurves = parseCurvesAt(d, len, *omc, o, err);
                if (!mCurves) {
                    return std::nullopt;
                }
                stages.push_back(Stage::curvesStage(std::move(*mCurves)));
                auto mx = matrixAt(*om);
                if (!mx) {
                    return std::nullopt;
                }
                stages.push_back(std::move(*mx));
            }
            if (*ob == 0) {
                err = "invalid ICC profile: lutAtoB without B curves";
                return std::nullopt;
            }
            auto bCurves = parseCurvesAt(d, len, *ob, o, err);
            if (!bCurves) {
                return std::nullopt;
            }
            stages.push_back(Stage::curvesStage(std::move(*bCurves)));
        } else {
            // B 曲线 → 矩阵 → M 曲线 → CLUT → A 曲线。
            if (*ob == 0) {
                err = "invalid ICC profile: lutBtoA without B curves";
                return std::nullopt;
            }
            auto bCurves = parseCurvesAt(d, len, *ob, i, err);
            if (!bCurves) {
                return std::nullopt;
            }
            stages.push_back(Stage::curvesStage(std::move(*bCurves)));
            if (*om != 0) {
                auto mx = matrixAt(*om);
                if (!mx) {
                    return std::nullopt;
                }
                stages.push_back(std::move(*mx));
                auto mCurves = parseCurvesAt(d, len, *omc, i, err);
                if (!mCurves) {
                    return std::nullopt;
                }
                stages.push_back(Stage::curvesStage(std::move(*mCurves)));
            }
            if (*oc != 0) {
                if (*oa == 0) {
                    err = "invalid ICC profile: lutBtoA with CLUT but no A curves";
                    return std::nullopt;
                }
                auto cl = clutAt(*oc, i, o);
                if (!cl) {
                    return std::nullopt;
                }
                stages.push_back(std::move(*cl));
                auto aCurves = parseCurvesAt(d, len, *oa, o, err);
                if (!aCurves) {
                    return std::nullopt;
                }
                stages.push_back(Stage::curvesStage(std::move(*aCurves)));
            } else if (i != o) {
                err = "invalid ICC profile: lutBtoA without CLUT changes channel count";
                return std::nullopt;
            }
        }
        Lut lut;
        lut.kind = LutKind::Ab;
        lut.inputs = i;
        lut.outputs = o;
        lut.stages = std::move(stages);
        return lut;
    }
    err = "unsupported ICC feature: LUT type " + std::string(reinterpret_cast<const char*>(ty), 4);
    return std::nullopt;
}

// `desc`（textDescriptionType）、`mluc`（英文或首记录）或 `text` 的文本。
std::string parseText(const uint8_t* d, size_t len) {
    Reader r{d, len};
    const uint8_t* ty = nullptr;
    if (!r.slice(0, 4, &ty)) {
        return std::string();
    }
    auto ascii = [](const uint8_t* s, size_t n) {
        size_t end = 0;
        while (end < n && s[end] != 0) {
            ++end;
        }
        std::string out(reinterpret_cast<const char*>(s), end);
        // trim
        size_t b = out.find_first_not_of(" \t\r\n");
        size_t e = out.find_last_not_of(" \t\r\n");
        return b == std::string::npos ? std::string() : out.substr(b, e - b + 1);
    };
    if (std::memcmp(ty, "desc", 4) == 0) {
        const auto n = r.u32(8);
        if (!n) {
            return std::string();
        }
        const uint8_t* s = nullptr;
        if (!r.slice(12, std::min<size_t>(*n, len >= 12 ? len - 12 : 0), &s)) {
            return std::string();
        }
        return ascii(s, std::min<size_t>(*n, len >= 12 ? len - 12 : 0));
    }
    if (std::memcmp(ty, "text", 4) == 0) {
        if (len < 8) {
            return std::string();
        }
        return ascii(d + 8, len - 8);
    }
    if (std::memcmp(ty, "mluc", 4) == 0) {
        const auto n = r.u32(8), size = r.u32(12);
        if (!n || !size) {
            return std::string();
        }
        std::optional<std::pair<size_t, size_t>> best;
        const size_t records = std::min<size_t>(*n, 256);
        for (size_t k = 0; k < records; ++k) {
            const size_t o = 16 + k * static_cast<size_t>(*size);
            const auto lang = r.u16(o);
            const auto recLen = r.u32(o + 4);
            const auto recOff = r.u32(o + 8);
            if (!lang || !recLen || !recOff) {
                break;
            }
            const bool isEn = *lang == (static_cast<uint16_t>('e') << 8 | 'n');
            if (!best || isEn) {
                best = std::make_pair(static_cast<size_t>(*recOff), static_cast<size_t>(*recLen));
                if (isEn) {
                    break;
                }
            }
        }
        if (!best) {
            return std::string();
        }
        const uint8_t* s = nullptr;
        if (!r.slice(best->first, best->second, &s)) {
            return std::string();
        }
        // UTF-16BE → UTF-8（lossy）。
        std::string out;
        const size_t units = best->second / 2;
        for (size_t i = 0; i < units; ++i) {
            const uint32_t u = static_cast<uint16_t>((s[2 * i] << 8) | s[2 * i + 1]);
            if (u >= 0xD800 && u < 0xDC00 && i + 1 < units) {
                const uint32_t lo = static_cast<uint16_t>((s[2 * (i + 1)] << 8) | s[2 * (i + 1) + 1]);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    const uint32_t cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    // 4 字节 UTF-8
                    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    ++i;
                    continue;
                }
            }
            if (u < 0x80) {
                out.push_back(static_cast<char>(u));
            } else if (u < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (u >> 6)));
                out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (u >> 12)));
                out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
            }
        }
        // 去尾部 NUL 与空白。
        while (!out.empty() && (out.back() == '\0' || out.back() == ' ' || out.back() == '\r' || out.back() == '\n')) {
            out.pop_back();
        }
        return out;
    }
    return std::string();
}

std::optional<Profile> parseProfileBytes(const uint8_t* bytes, size_t bytesLen, std::string& err) {
    Reader r{bytes, bytesLen};
    if (bytesLen < 132) {
        err = "ICC profile is truncated";
        return std::nullopt;
    }
    const uint8_t* acsp = nullptr;
    if (!r.slice(36, 4, &acsp) || std::memcmp(acsp, "acsp", 4) != 0) {
        err = "not an ICC profile (missing 'acsp' signature)";
        return std::nullopt;
    }
    const auto declaredV = r.u32(0);
    if (!declaredV) {
        err = "ICC profile is truncated";
        return std::nullopt;
    }
    // 某些写端填充或截断；取两者较小值。
    const size_t end = (*declaredV >= 132 && *declaredV <= bytesLen) ? static_cast<size_t>(*declaredV) : bytesLen;
    r = Reader{bytes, end};
    const auto v8 = r.u8(8), v9 = r.u8(9);
    const auto classV = r.u32(12), csV = r.u32(16), pcsSigV = r.u32(20), intentV = r.u32(64);
    const auto countV = r.u32(128);
    if (!v8 || !v9 || !classV || !csV || !pcsSigV || !intentV || !countV) {
        err = "ICC profile is truncated";
        return std::nullopt;
    }
    const uint32_t pcsSig = *pcsSigV;
    const Pcs pcs = (pcsSig == sigOf("Lab ")) ? Pcs::Lab : Pcs::Xyz;
    const Intent renderingIntent = intentFromU32(*intentV & 0xFFFF).value_or(Intent::Perceptual);
    const size_t count = static_cast<size_t>(*countV);
    if (count > 1024) {
        err = "invalid ICC profile: " + std::to_string(count) + " tags";
        return std::nullopt;
    }
    std::map<uint32_t, std::pair<size_t, size_t>> tags;
    for (size_t i = 0; i < count; ++i) {
        const size_t o = 132 + i * 12;
        const auto s = r.u32(o), off = r.u32(o + 4), tlen = r.u32(o + 8);
        if (!s || !off || !tlen) {
            err = "ICC profile is truncated";
            return std::nullopt;
        }
        tags[*s] = std::make_pair(static_cast<size_t>(*off), static_cast<size_t>(*tlen));
    }
    auto tagData = [&](const char (&s)[5], const uint8_t** out, size_t* outLen) {
        auto it = tags.find(sigOf(s));
        if (it == tags.end()) {
            return false;  // 不存在
        }
        const uint8_t* p = nullptr;
        if (!r.slice(it->second.first, it->second.second, &p)) {
            err = "ICC profile is truncated";
            *outLen = 0;
            return true;  // 存在但截断 = 错误
        }
        *out = p;
        *outLen = it->second.second;
        return true;
    };
    // 返回：-1 无标签 / 0 有值 / 1 错误
    auto xyzTag = [&](const char (&s)[5], std::optional<std::array<double, 3>>& out) -> int {
        const uint8_t* d = nullptr;
        size_t dl = 0;
        if (!tagData(s, &d, &dl)) {
            return -1;
        }
        if (dl == 0) {
            return 1;
        }
        if (dl >= 20 && std::memcmp(d, "XYZ ", 4) == 0) {
            Reader rd{d, dl};
            out = rd.xyz(8);
            return out ? 0 : 1;
        }
        err = std::string("invalid ICC profile: tag ") + std::string(s, 4) + " is not XYZType";
        return 1;
    };
    auto curveTag = [&](const char (&s)[5], std::optional<Curve>& out) -> int {
        const uint8_t* d = nullptr;
        size_t dl = 0;
        if (!tagData(s, &d, &dl)) {
            return -1;
        }
        if (dl == 0) {
            return 1;
        }
        auto c = parseCurve(d, dl, err);
        if (!c) {
            return 1;
        }
        out = c->first;
        return 0;
    };

    Profile p;
    p.versionMajor = *v8;
    p.versionMinor = *v9;
    p.klass = profileClassFromSig(*classV);
    p.classSig = *classV;
    p.colorSpace = colorSpaceFromSig(*csV);
    p.colorSpaceSig = *csV;
    p.pcs = pcs;
    p.renderingIntent = renderingIntent;

    std::optional<std::array<double, 3>> wtpt;
    xyzTag("wtpt", wtpt);
    p.whitePoint = wtpt.value_or(std::array<double, 3>{montage::cms::kD50[0], montage::cms::kD50[1], montage::cms::kD50[2]});
    std::optional<std::array<double, 3>> bkpt;
    if (xyzTag("bkpt", bkpt) == 0) {
        p.blackPoint = bkpt;
    }
    {
        const uint8_t* d = nullptr;
        size_t dl = 0;
        if (tagData("chad", &d, &dl) && dl > 0) {
            if (dl >= 44 && std::memcmp(d, "sf32", 4) == 0) {
                Reader rd{d, dl};
                Mat3 m{};
                bool ok = true;
                for (int i = 0; i < 3 && ok; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        const auto v = rd.s15f16(8 + (i * 3 + j) * 4);
                        if (!v) {
                            ok = false;
                            break;
                        }
                        m[i][j] = *v;
                    }
                }
                if (ok) {
                    p.chad = m;
                }
            }
        }
    }
    std::optional<std::array<double, 3>> rr, gg, bb;
    const int rRet = xyzTag("rXYZ", rr), gRet = xyzTag("gXYZ", gg), bRet = xyzTag("bXYZ", bb);
    if (rRet == 1 || gRet == 1 || bRet == 1) {
        return std::nullopt;
    }
    if (rr && gg && bb) {
        p.matrix = Mat3{{{(*rr)[0], (*gg)[0], (*bb)[0]},
                         {(*rr)[1], (*gg)[1], (*bb)[1]},
                         {(*rr)[2], (*gg)[2], (*bb)[2]}}};
    }
    std::optional<Curve> cr, cg, cb;
    const int crRet = curveTag("rTRC", cr), cgRet = curveTag("gTRC", cg), cbRet = curveTag("bTRC", cb);
    if (crRet == 1 || cgRet == 1 || cbRet == 1) {
        return std::nullopt;
    }
    if (cr && cg && cb) {
        p.trc = std::array<Curve, 3>{*cr, *cg, *cb};
    }
    std::optional<Curve> ktrc;
    if (curveTag("kTRC", ktrc) == 0) {
        p.grayTrc = ktrc;
    }
    auto lutTag = [&](const char (&s)[5], bool a2b, std::optional<Lut>& out) -> int {
        const uint8_t* d = nullptr;
        size_t dl = 0;
        if (!tagData(s, &d, &dl)) {
            return -1;
        }
        if (dl == 0) {
            return 1;
        }
        auto l = parseLut(d, dl, a2b, err);
        if (!l) {
            return 1;
        }
        out = std::move(*l);
        return 0;
    };
    for (int i = 0; i < 3; ++i) {
        const char a2bNames[3][5] = {"A2B0", "A2B1", "A2B2"};
        const char b2aNames[3][5] = {"B2A0", "B2A1", "B2A2"};
        if (lutTag(a2bNames[i], true, p.a2b[i]) == 1 || lutTag(b2aNames[i], false, p.b2a[i]) == 1) {
            return std::nullopt;
        }
    }
    {
        const uint8_t* d = nullptr;
        size_t dl = 0;
        if (tagData("desc", &d, &dl) && dl > 0) {
            p.description = parseText(d, dl);
        }
        if (tagData("cprt", &d, &dl) && dl > 0) {
            p.copyright = parseText(d, dl);
        }
    }

    // 校验 LUT 通道数与头部一致。DeviceLink 连接两个设备空间：其头部 PCS 字段存
    // 输出色彩空间（ICC.1:2010 §7.2.7），故 CMYK→CMYK link 的 AToB0 是 4→4。
    const size_t dev = colorSpaceChannels(p.colorSpace);
    const size_t conn =
        p.klass == ProfileClass::DeviceLink ? colorSpaceChannels(colorSpaceFromSig(pcsSig)) : 3;
    for (const auto& l : p.a2b) {
        if (!l) {
            continue;
        }
        if ((dev != 0 && l->inputs != dev) || (conn != 0 && l->outputs != conn)) {
            err = "invalid ICC profile: AToB tag has " + std::to_string(l->inputs) + "\xE2\x86\x92" +
                  std::to_string(l->outputs) + " channels";
            return std::nullopt;
        }
    }
    for (const auto& l : p.b2a) {
        if (!l) {
            continue;
        }
        if ((conn != 0 && l->inputs != conn) || (dev != 0 && l->outputs != dev)) {
            err = "invalid ICC profile: BToA tag has " + std::to_string(l->inputs) + "\xE2\x86\x92" +
                  std::to_string(l->outputs) + " channels";
            return std::nullopt;
        }
    }
    return p;
}

}  // namespace

ProfileClass profileClassFromSig(uint32_t sig) {
    switch (sig) {
        case sigOf("scnr"): return ProfileClass::Input;
        case sigOf("mntr"): return ProfileClass::Display;
        case sigOf("prtr"): return ProfileClass::Output;
        case sigOf("link"): return ProfileClass::DeviceLink;
        case sigOf("spac"): return ProfileClass::ColorSpace;
        case sigOf("abst"): return ProfileClass::Abstract;
        case sigOf("nmcl"): return ProfileClass::NamedColor;
        default: return ProfileClass::Unknown;
    }
}

uint32_t profileClassSig(ProfileClass c) {
    switch (c) {
        case ProfileClass::Input: return sigOf("scnr");
        case ProfileClass::Display: return sigOf("mntr");
        case ProfileClass::Output: return sigOf("prtr");
        case ProfileClass::DeviceLink: return sigOf("link");
        case ProfileClass::ColorSpace: return sigOf("spac");
        case ProfileClass::Abstract: return sigOf("abst");
        case ProfileClass::NamedColor: return sigOf("nmcl");
        default: return 0;
    }
}

const char* profileClassName(ProfileClass c) {
    switch (c) {
        case ProfileClass::Input: return "Input";
        case ProfileClass::Display: return "Display";
        case ProfileClass::Output: return "Output";
        case ProfileClass::DeviceLink: return "DeviceLink";
        case ProfileClass::ColorSpace: return "ColorSpace";
        case ProfileClass::Abstract: return "Abstract";
        case ProfileClass::NamedColor: return "NamedColor";
        default: return "Unknown";
    }
}

ColorSpace colorSpaceFromSig(uint32_t sig) {
    switch (sig) {
        case sigOf("XYZ "): return ColorSpace::Xyz;
        case sigOf("Lab "): return ColorSpace::Lab;
        case sigOf("Luv "): return ColorSpace::Luv;
        case sigOf("YCbr"): return ColorSpace::YCbCr;
        case sigOf("Yxy "): return ColorSpace::Yxy;
        case sigOf("RGB "): return ColorSpace::Rgb;
        case sigOf("GRAY"): return ColorSpace::Gray;
        case sigOf("HSV "): return ColorSpace::Hsv;
        case sigOf("HLS "): return ColorSpace::Hls;
        case sigOf("CMYK"): return ColorSpace::Cmyk;
        case sigOf("CMY "): return ColorSpace::Cmy;
        default: break;
    }
    const uint8_t b[4] = {static_cast<uint8_t>(sig >> 24), static_cast<uint8_t>(sig >> 16),
                          static_cast<uint8_t>(sig >> 8), static_cast<uint8_t>(sig)};
    if (b[1] == 'C' && b[2] == 'L' && b[3] == 'R') {
        if (b[0] >= '2' && b[0] <= '9') {
            return ColorSpace::NColor;  // 通道数经 colorSpaceNChannels 查表（sig 保底）
        }
        if (b[0] >= 'A' && b[0] <= 'F') {
            return ColorSpace::NColor;
        }
    }
    return ColorSpace::Unknown;
}

uint32_t colorSpaceSig(ColorSpace cs) {
    switch (cs) {
        case ColorSpace::Xyz: return sigOf("XYZ ");
        case ColorSpace::Lab: return sigOf("Lab ");
        case ColorSpace::Luv: return sigOf("Luv ");
        case ColorSpace::YCbCr: return sigOf("YCbr");
        case ColorSpace::Yxy: return sigOf("Yxy ");
        case ColorSpace::Rgb: return sigOf("RGB ");
        case ColorSpace::Gray: return sigOf("GRAY");
        case ColorSpace::Hsv: return sigOf("HSV ");
        case ColorSpace::Hls: return sigOf("HLS ");
        case ColorSpace::Cmyk: return sigOf("CMYK");
        case ColorSpace::Cmy: return sigOf("CMY ");
        default: return 0;
    }
}

const char* colorSpaceName(ColorSpace cs) {
    switch (cs) {
        case ColorSpace::Xyz: return "Xyz";
        case ColorSpace::Lab: return "Lab";
        case ColorSpace::Luv: return "Luv";
        case ColorSpace::YCbCr: return "YCbCr";
        case ColorSpace::Yxy: return "Yxy";
        case ColorSpace::Rgb: return "Rgb";
        case ColorSpace::Gray: return "Gray";
        case ColorSpace::Hsv: return "Hsv";
        case ColorSpace::Hls: return "Hls";
        case ColorSpace::Cmyk: return "Cmyk";
        case ColorSpace::Cmy: return "Cmy";
        case ColorSpace::NColor: return "NColor";
        default: return "Unknown";
    }
}

size_t colorSpaceChannels(ColorSpace cs) {
    switch (cs) {
        case ColorSpace::Gray: return 1;
        case ColorSpace::Cmyk: return 4;
        case ColorSpace::Unknown: return 0;
        case ColorSpace::NColor: return 0;  // nCLR 具体通道数由 sig 决定（罕见，v1 不解）
        default: return 3;
    }
}

std::optional<Profile> Profile::parse(const uint8_t* data, size_t len, std::string& err) {
    auto p = parseProfileBytes(data, len, err);
    if (!p) {
        return std::nullopt;
    }
    auto stored = std::make_shared<const std::vector<uint8_t>>(data, data + len);
    p->bytes = stored;
    // fnv1a 64（进程内稳定缓存键）。
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : *stored) {
        h ^= c;
        h *= 1099511628211ull;
    }
    p->hash = h;
    return p;
}

std::shared_ptr<const std::vector<uint8_t>> Profile::toBytes() const {
    if (bytes) {
        return bytes;
    }
    auto enc = std::make_shared<const std::vector<uint8_t>>(encodeProfile(*this));
    return enc;
}

Profile& Profile::withEncodedBytes() {
    auto enc = std::make_shared<const std::vector<uint8_t>>(encodeProfile(*this));
    bytes = enc;
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : *enc) {
        h ^= c;
        h *= 1099511628211ull;
    }
    hash = h;
    return *this;
}

uint64_t Profile::contentHash() const {
    if (hash != 0) {
        return hash;
    }
    auto b = toBytes();
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : *b) {
        h ^= c;
        h *= 1099511628211ull;
    }
    // const 方法内惰性缓存：hash 可变成员化在 C++ 侧用 const_cast（单写语义，代价可忽略）。
    const_cast<Profile*>(this)->bytes = b;
    const_cast<Profile*>(this)->hash = h;
    return h;
}

bool Profile::sameColors(const Profile& other) const {
    if (contentHash() == other.contentHash()) {
        return true;
    }
    const size_t n = channels();
    if (colorSpace != other.colorSpace || n == 0 || n > 4) {
        return false;
    }
    // Lab 内置 profile（builtin.cpp）经 PCS 比较两 profile 的颜色。
    const Profile& lab = builtinProfile(Builtin::LabD50);
    std::string err;
    auto toLab = [&](const Profile& p) { return Transform::make(p, lab, Intent::RelativeColorimetric, false, err); };
    auto a = toLab(*this);
    auto b = toLab(other);
    if (!a || !b) {
        return false;
    }
    // 每通道 17 步抓只在曲线趾部不同的 TRC；CMYK 用 9 步控制在 6561 采样。
    const size_t steps = n <= 3 ? 17 : 9;
    auto decode = [](const float* o) {
        return std::array<double, 3>{static_cast<double>(o[0]) * 100.0, static_cast<double>(o[1]) * 255.0 - 128.0,
                                     static_cast<double>(o[2]) * 255.0 - 128.0};
    };
    float input[4] = {0, 0, 0, 0};
    size_t total = 1;
    for (size_t i = 0; i < n; ++i) {
        total *= steps;
    }
    for (size_t i = 0; i < total; ++i) {
        size_t k = i;
        for (size_t c = 0; c < n; ++c) {
            input[c] = static_cast<float>(k % steps) / static_cast<float>(steps - 1);
            k /= steps;
        }
        float la[3] = {0, 0, 0}, lb[3] = {0, 0, 0};
        a->eval(input, la);
        b->eval(input, lb);
        if (deltaE76(decode(la), decode(lb)) > kSameColorsMaxDeltaE) {
            return false;
        }
    }
    return true;
}

std::array<double, 3> Profile::mediaWhite() const {
    if (versionMajor < 4 && klass == ProfileClass::Display) {
        return {kD50[0], kD50[1], kD50[2]};
    }
    return whitePoint;
}

std::optional<Profile> Profile::grayAsRgb() const {
    if (!grayTrc || pcs != Pcs::Xyz) {
        return std::nullopt;
    }
    const auto w = kD50;
    const Mat3 m = {{{w[0] / 3.0, w[0] / 3.0, w[0] / 3.0},
                     {w[1] / 3.0, w[1] / 3.0, w[1] / 3.0},
                     {w[2] / 3.0, w[2] / 3.0, w[2] / 3.0}}};
    Profile p = *this;
    p.colorSpace = ColorSpace::Rgb;
    p.colorSpaceSig = montage::cms::colorSpaceSig(ColorSpace::Rgb);
    p.description = description + " (as RGB)";
    p.matrix = m;
    p.trc = std::array<Curve, 3>{*grayTrc, *grayTrc, *grayTrc};
    p.grayTrc = std::nullopt;
    // 灰 LUT 是单输入；此 profile 是上面的矩阵/TRC 模型。
    p.a2b[0] = std::nullopt;
    p.a2b[1] = std::nullopt;
    p.a2b[2] = std::nullopt;
    p.b2a[0] = std::nullopt;
    p.b2a[1] = std::nullopt;
    p.b2a[2] = std::nullopt;
    p.bytes = nullptr;
    p.hash = 0;
    return p;
}

bool Profile::isMatrixShaper() const {
    const bool noLuts = !a2b[0] && !a2b[1] && !a2b[2] && !b2a[0] && !b2a[1] && !b2a[2];
    const bool matrixTrc = matrix.has_value() && trc.has_value();
    const bool gray = grayTrc.has_value();
    return noLuts && (matrixTrc || gray);
}

bool Profile::supportsIntent(Intent intent, bool input) const {
    const auto& luts = input ? a2b : b2a;
    const size_t i = intent == Intent::Perceptual ? 0
                     : (intent == Intent::Saturation ? 2 : 1);
    return luts[i].has_value() || (!luts[0] && !luts[1] && !luts[2]);
}

namespace {
const Lut* lutFor(const std::optional<Lut> (&luts)[3], Intent intent) {
    const size_t i = intent == Intent::Perceptual ? 0 : (intent == Intent::Saturation ? 2 : 1);
    return luts[i] ? &*luts[i] : (luts[0] ? &*luts[0] : nullptr);
}

Stage decodePcsStage(Pcs pcs, LutKind kind) {
    if (pcs == Pcs::Xyz) {
        Stage s;
        s.kind = Stage::Kind::DecodeXyz;
        return s;
    }
    Stage s;
    s.kind = kind == LutKind::Lut16 ? Stage::Kind::DecodeLabV2 : Stage::Kind::DecodeLabV4;
    return s;
}

Stage encodePcsStage(Pcs pcs, LutKind kind) {
    if (pcs == Pcs::Xyz) {
        Stage s;
        s.kind = Stage::Kind::EncodeXyz;
        return s;
    }
    Stage s;
    s.kind = kind == LutKind::Lut16 ? Stage::Kind::EncodeLabV2 : Stage::Kind::EncodeLabV4;
    return s;
}
}  // namespace

std::optional<std::pair<std::vector<Stage>, Pcs>> Profile::deviceToPcs(Intent intent, std::string& err) const {
    if (const Lut* l = lutFor(a2b, intent)) {
        std::vector<Stage> st = l->stages;
        st.push_back(decodePcsStage(pcs, l->kind));
        return std::make_pair(std::move(st), pcs);
    }
    switch (colorSpace) {
        case ColorSpace::Rgb: {
            if (!matrix || !trc) {
                err = "unsupported ICC feature: RGB profile without matrix/TRC or AToB tags";
                return std::nullopt;
            }
            std::vector<Stage> st;
            st.push_back(Stage::curvesStage(std::vector<Curve>(trc->begin(), trc->end())));
            st.push_back(Stage::matrix3(*matrix));
            return std::make_pair(std::move(st), Pcs::Xyz);
        }
        case ColorSpace::Gray: {
            if (!grayTrc) {
                err = "unsupported ICC feature: gray profile without kTRC";
                return std::nullopt;
            }
            std::vector<Stage> st;
            st.push_back(Stage::curvesStage(std::vector<Curve>{*grayTrc}));
            if (pcs == Pcs::Lab) {
                // 曲线输出 L*/100。
                st.push_back(Stage::matrixStage(3, 1, std::vector<double>{100.0, 0.0, 0.0}, std::vector<double>(3, 0.0)));
                return std::make_pair(std::move(st), Pcs::Lab);
            }
            st.push_back(Stage::matrixStage(3, 1, std::vector<double>{kD50[0], kD50[1], kD50[2]}, std::vector<double>(3, 0.0)));
            return std::make_pair(std::move(st), Pcs::Xyz);
        }
        case ColorSpace::Lab: {
            std::vector<Stage> st;
            Stage s;
            s.kind = Stage::Kind::DecodeLabV4;
            st.push_back(s);
            return std::make_pair(std::move(st), Pcs::Lab);
        }
        case ColorSpace::Xyz: {
            std::vector<Stage> st;
            Stage s;
            s.kind = Stage::Kind::DecodeXyz;
            st.push_back(s);
            return std::make_pair(std::move(st), Pcs::Xyz);
        }
        default:
            err = std::string("unsupported ICC feature: ") + colorSpaceName(colorSpace) +
                  " profile without AToB tags";
            return std::nullopt;
    }
}

std::optional<std::pair<std::vector<Stage>, Pcs>> Profile::pcsToDevice(Intent intent, std::string& err) const {
    if (const Lut* l = lutFor(b2a, intent)) {
        std::vector<Stage> st;
        st.push_back(encodePcsStage(pcs, l->kind));
        st.insert(st.end(), l->stages.begin(), l->stages.end());
        return std::make_pair(std::move(st), pcs);
    }
    switch (colorSpace) {
        case ColorSpace::Rgb: {
            if (!matrix || !trc) {
                err = "unsupported ICC feature: RGB profile without matrix/TRC or BToA tags";
                return std::nullopt;
            }
            const auto inv = invert(*matrix);
            if (!inv) {
                err = "invalid ICC profile: singular colorant matrix";
                return std::nullopt;
            }
            std::vector<Stage> st;
            st.push_back(Stage::matrix3(*inv));
            st.push_back(Stage::invCurvesStage(std::vector<Curve>(trc->begin(), trc->end())));
            return std::make_pair(std::move(st), Pcs::Xyz);
        }
        case ColorSpace::Gray: {
            if (!grayTrc) {
                err = "unsupported ICC feature: gray profile without kTRC";
                return std::nullopt;
            }
            std::vector<Stage> st;
            if (pcs == Pcs::Lab) {
                st.push_back(Stage::matrixStage(1, 3, std::vector<double>{0.01, 0.0, 0.0}, std::vector<double>(1, 0.0)));
            } else {
                st.push_back(Stage::matrixStage(1, 3, std::vector<double>{0.0, 1.0, 0.0}, std::vector<double>(1, 0.0)));
            }
            st.push_back(Stage::invCurvesStage(std::vector<Curve>{*grayTrc}));
            return std::make_pair(std::move(st), pcs);
        }
        case ColorSpace::Lab: {
            std::vector<Stage> st;
            Stage s;
            s.kind = Stage::Kind::EncodeLabV4;
            st.push_back(s);
            return std::make_pair(std::move(st), Pcs::Lab);
        }
        case ColorSpace::Xyz: {
            std::vector<Stage> st;
            Stage s;
            s.kind = Stage::Kind::EncodeXyz;
            st.push_back(s);
            return std::make_pair(std::move(st), Pcs::Xyz);
        }
        default:
            err = std::string("unsupported ICC feature: ") + colorSpaceName(colorSpace) +
                  " profile without BToA tags";
            return std::nullopt;
    }
}

bool Profile::operator==(const Profile& o) const {
    return versionMajor == o.versionMajor && versionMinor == o.versionMinor && klass == o.klass &&
           colorSpace == o.colorSpace && pcs == o.pcs && renderingIntent == o.renderingIntent &&
           description == o.description && copyright == o.copyright && whitePoint == o.whitePoint &&
           blackPoint == o.blackPoint && chad == o.chad && matrix == o.matrix && trc == o.trc &&
           grayTrc == o.grayTrc && a2b[0] == o.a2b[0] && a2b[1] == o.a2b[1] && a2b[2] == o.a2b[2] &&
           b2a[0] == o.b2a[0] && b2a[1] == o.b2a[1] && b2a[2] == o.b2a[2];
}

// ---------------------------------------------------------------- writer（write.rs）

namespace {

void putS15(std::vector<uint8_t>& o, double v) {
    const int32_t q = static_cast<int32_t>(std::llround(v * 65536.0));
    o.push_back(static_cast<uint8_t>(q >> 24));
    o.push_back(static_cast<uint8_t>(q >> 16));
    o.push_back(static_cast<uint8_t>(q >> 8));
    o.push_back(static_cast<uint8_t>(q));
}

std::vector<uint8_t> xyzType(const std::array<double, 3>& v) {
    std::vector<uint8_t> o = {'X', 'Y', 'Z', ' ', 0, 0, 0, 0};
    for (double c : v) {
        putS15(o, c);
    }
    return o;
}

void putU16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v));
}

void putU32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(static_cast<uint8_t>(v >> 24));
    o.push_back(static_cast<uint8_t>(v >> 16));
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v));
}

// UTF-8 → UTF-16 码元。
std::vector<uint16_t> toUtf16(const std::string& s) {
    std::vector<uint16_t> units;
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        if (c < 0x80) {
            units.push_back(c);
            i += 1;
        } else if ((c >> 5) == 0x6 && i + 1 < s.size()) {
            units.push_back(static_cast<uint16_t>(((c & 0x1F) << 6) | (s[i + 1] & 0x3F)));
            i += 2;
        } else if ((c >> 4) == 0xE && i + 2 < s.size()) {
            units.push_back(static_cast<uint16_t>(((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F)));
            i += 3;
        } else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
            const uint32_t cp = ((c & 0x07) << 18) | ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 12) |
                                ((static_cast<uint8_t>(s[i + 2]) & 0x3F) << 6) | (static_cast<uint8_t>(s[i + 3]) & 0x3F);
            units.push_back(static_cast<uint16_t>(0xD800 + ((cp - 0x10000) >> 10)));
            units.push_back(static_cast<uint16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF)));
            i += 4;
        } else {
            units.push_back(0xFFFD);
            i += 1;
        }
    }
    return units;
}

std::vector<uint8_t> mlucType(const std::string& text) {
    const auto units = toUtf16(text);
    std::vector<uint8_t> o = {'m', 'l', 'u', 'c', 0, 0, 0, 0};
    putU32(o, 1);   // 记录数
    putU32(o, 12);  // 记录尺寸
    o.push_back('e');
    o.push_back('n');
    o.push_back('U');
    o.push_back('S');
    putU32(o, static_cast<uint32_t>(units.size() * 2));
    putU32(o, 28);
    for (uint16_t u : units) {
        putU16(o, u);
    }
    return o;
}

inline uint16_t u16q(float v) {
    return static_cast<uint16_t>(std::min(std::max(v, 0.0f), 1.0f) * 65535.0f + 0.5f);
}

// 曲线的 `curv`/`para` 元素。
std::vector<uint8_t> curveElem(const Curve& c) {
    std::vector<uint8_t> o;
    switch (c.kind) {
        case Curve::Kind::Identity:
            o = {'c', 'u', 'r', 'v', 0, 0, 0, 0};
            putU32(o, 0);
            break;
        case Curve::Kind::Gamma:
            o = {'p', 'a', 'r', 'a', 0, 0, 0, 0};
            putU16(o, 0);
            putU16(o, 0);
            putS15(o, c.gamma);
            break;
        case Curve::Kind::Parametric: {
            o = {'p', 'a', 'r', 'a', 0, 0, 0, 0};
            putU16(o, c.paraKind);
            putU16(o, 0);
            const int n = Curve::paramCount(c.paraKind);
            for (int i = 0; i < n; ++i) {
                putS15(o, c.p[i]);
            }
            break;
        }
        case Curve::Kind::Table: {
            o = {'c', 'u', 'r', 'v', 0, 0, 0, 0};
            putU32(o, static_cast<uint32_t>(c.table.size()));
            for (float v : c.table) {
                putU16(o, u16q(v));
            }
            break;
        }
    }
    return o;
}

void pad4(std::vector<uint8_t>& v) {
    while (v.size() % 4 != 0) {
        v.push_back(0);
    }
}

std::vector<float> curveTable(const Curve& c, size_t n) {
    if (c.kind == Curve::Kind::Table && c.table.size() == n) {
        return c.table;
    }
    return c.sample(n);
}

size_t tableLen(const std::vector<Curve>& cs) {
    size_t m = 2;
    for (const Curve& c : cs) {
        const size_t l = c.kind == Curve::Kind::Table ? c.table.size() : (c.kind == Curve::Kind::Identity ? 2 : 1024);
        m = std::max(m, l);
    }
    return std::min(std::max<size_t>(m, 2), static_cast<size_t>(4096));
}

// LUT 阶段拆分为 (矩阵, 输入曲线, clut, 输出曲线) —— mft1/mft2 可存的布局。
struct LutParts {
    std::optional<std::vector<double>> mat;
    const std::vector<Curve>* in = nullptr;
    const Clut* clutP = nullptr;
    const std::vector<Curve>* out = nullptr;
};

std::optional<LutParts> lutParts(const std::vector<Stage>& stages) {
    LutParts parts;
    size_t i = 0;
    if (!stages.empty() && stages[0].kind == Stage::Kind::Matrix && stages[0].rows == 3 && stages[0].cols == 3) {
        parts.mat = stages[0].m;
        i = 1;
    }
    if (stages.size() != i + 3) {
        return std::nullopt;
    }
    if (stages[i].kind != Stage::Kind::Curves || stages[i + 1].kind != Stage::Kind::Clut ||
        stages[i + 2].kind != Stage::Kind::Curves) {
        return std::nullopt;
    }
    parts.in = &stages[i].curves;
    parts.clutP = &stages[i + 1].clut;
    parts.out = &stages[i + 2].curves;
    return parts;
}

std::optional<std::vector<uint8_t>> encodeMft(const std::vector<Stage>& stages, bool wide) {
    auto parts = lutParts(stages);
    if (!parts) {
        return std::nullopt;
    }
    const Clut& clut = *parts->clutP;
    if (clut.grid.empty()) {
        return std::nullopt;
    }
    const size_t g = clut.grid[0];
    for (size_t x : clut.grid) {
        if (x != g) {
            return std::nullopt;
        }
    }
    std::vector<uint8_t> o;
    const char* ty = wide ? "mft2" : "mft1";
    o.insert(o.end(), ty, ty + 4);
    o.insert(o.end(), {0, 0, 0, 0});
    o.push_back(static_cast<uint8_t>(clut.inputs));
    o.push_back(static_cast<uint8_t>(clut.outputs));
    o.push_back(static_cast<uint8_t>(g));
    o.push_back(0);
    if (parts->mat) {
        for (double v : *parts->mat) {
            putS15(o, v);
        }
    } else {
        const double ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        for (double v : ident) {
            putS15(o, v);
        }
    }
    const size_t nIn = wide ? tableLen(*parts->in) : 256;
    const size_t nOut = wide ? tableLen(*parts->out) : 256;
    if (wide) {
        putU16(o, static_cast<uint16_t>(nIn));
        putU16(o, static_cast<uint16_t>(nOut));
    }
    auto put = [&](std::vector<uint8_t>& dst, float v) {
        if (wide) {
            putU16(dst, u16q(v));
        } else {
            dst.push_back(static_cast<uint8_t>(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f));
        }
    };
    for (const Curve& c : *parts->in) {
        for (float v : curveTable(c, nIn)) {
            put(o, v);
        }
    }
    for (float v : clut.data) {
        put(o, v);
    }
    for (const Curve& c : *parts->out) {
        for (float v : curveTable(c, nOut)) {
            put(o, v);
        }
    }
    return o;
}

// 阶段不是本方向 lutAtoB/lutBtoA 可存布局时返回空。
std::optional<std::vector<uint8_t>> encodeAb(const std::vector<Stage>& stages, bool a2b, size_t inputs,
                                             size_t outputs) {
    const std::vector<Curve>* aCurves = nullptr;
    const Clut* clut = nullptr;
    const std::vector<Curve>* mCurves = nullptr;
    const std::vector<double>* matrixM = nullptr;
    const std::vector<double>* matrixOff = nullptr;
    const std::vector<Curve>* bCurves = nullptr;
    if (a2b) {
        // [A, CLUT] [M, matrix] B
        size_t i = 0;
        if (stages.size() >= 2 && stages[0].kind == Stage::Kind::Curves && stages[1].kind == Stage::Kind::Clut) {
            aCurves = &stages[0].curves;
            clut = &stages[1].clut;
            i = 2;
        }
        const size_t rest = stages.size() - i;
        if (rest == 3 && stages[i].kind == Stage::Kind::Curves && stages[i + 1].kind == Stage::Kind::Matrix &&
            stages[i + 2].kind == Stage::Kind::Curves) {
            mCurves = &stages[i].curves;
            matrixM = &stages[i + 1].m;
            matrixOff = &stages[i + 1].offset;
            bCurves = &stages[i + 2].curves;
        } else if (rest == 1 && stages[i].kind == Stage::Kind::Curves) {
            bCurves = &stages[i].curves;
        } else {
            return std::nullopt;
        }
    } else {
        // B [matrix, M] [CLUT, A]
        size_t k = 0;
        if (!stages.empty() && stages[0].kind == Stage::Kind::Curves) {
            bCurves = &stages[0].curves;
            k = 1;
        }
        if (stages.size() >= k + 2 && stages[k].kind == Stage::Kind::Matrix && stages[k + 1].kind == Stage::Kind::Curves) {
            matrixM = &stages[k].m;
            matrixOff = &stages[k].offset;
            mCurves = &stages[k + 1].curves;
            k += 2;
        }
        if (stages.size() >= k + 2 && stages[k].kind == Stage::Kind::Clut && stages[k + 1].kind == Stage::Kind::Curves) {
            clut = &stages[k].clut;
            aCurves = &stages[k + 1].curves;
            k += 2;
        }
        if (k != stages.size()) {
            return std::nullopt;
        }
    }
    if (bCurves == nullptr) {
        return std::nullopt;
    }
    std::vector<uint8_t> o;
    const char* ty = a2b ? "mAB " : "mBA ";
    o.insert(o.end(), ty, ty + 4);
    o.insert(o.end(), {0, 0, 0, 0});
    o.push_back(static_cast<uint8_t>(inputs));
    o.push_back(static_cast<uint8_t>(outputs));
    o.push_back(0);
    o.push_back(0);
    for (int i = 0; i < 20; ++i) {
        o.push_back(0);  // 偏移占位，随后回填
    }
    uint32_t offs[5] = {0, 0, 0, 0, 0};  // B, matrix, M, CLUT, A
    auto curves = [&](std::vector<uint8_t>& dst, const std::vector<Curve>& cs) {
        for (const Curve& c : cs) {
            std::vector<uint8_t> e = curveElem(c);
            dst.insert(dst.end(), e.begin(), e.end());
            pad4(dst);
        }
    };
    offs[0] = static_cast<uint32_t>(o.size());
    curves(o, *bCurves);
    if (matrixM != nullptr) {
        offs[1] = static_cast<uint32_t>(o.size());
        for (double v : *matrixM) {
            putS15(o, v);
        }
        for (double v : *matrixOff) {
            putS15(o, v);
        }
        offs[2] = static_cast<uint32_t>(o.size());
        if (mCurves == nullptr) {
            return std::nullopt;
        }
        curves(o, *mCurves);
    }
    if (clut != nullptr) {
        offs[3] = static_cast<uint32_t>(o.size());
        uint8_t g[16] = {0};
        for (size_t k = 0; k < clut->grid.size() && k < 16; ++k) {
            g[k] = static_cast<uint8_t>(clut->grid[k]);
        }
        o.insert(o.end(), g, g + 16);
        o.push_back(2);
        o.push_back(0);
        o.push_back(0);
        o.push_back(0);
        for (float v : clut->data) {
            putU16(o, u16q(v));
        }
        pad4(o);
        offs[4] = static_cast<uint32_t>(o.size());
        if (aCurves == nullptr) {
            return std::nullopt;
        }
        curves(o, *aCurves);
    }
    for (int k = 0; k < 5; ++k) {
        o[12 + k * 4] = static_cast<uint8_t>(offs[k] >> 24);
        o[13 + k * 4] = static_cast<uint8_t>(offs[k] >> 16);
        o[14 + k * 4] = static_cast<uint8_t>(offs[k] >> 8);
        o[15 + k * 4] = static_cast<uint8_t>(offs[k]);
    }
    return o;
}

}  // namespace

std::vector<uint8_t> encodeProfile(const Profile& p) {
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> tags;
    auto pushTag = [&](const char* sig4, std::vector<uint8_t> data) {
        const uint8_t* s = reinterpret_cast<const uint8_t*>(sig4);
        tags.emplace_back(static_cast<uint32_t>(s[0]) << 24 | static_cast<uint32_t>(s[1]) << 16 |
                              static_cast<uint32_t>(s[2]) << 8 | static_cast<uint32_t>(s[3]),
                          std::move(data));
    };
    pushTag("desc", mlucType(p.description));
    pushTag("cprt", mlucType(p.copyright));
    pushTag("wtpt", xyzType(p.whitePoint));
    if (p.blackPoint) {
        pushTag("bkpt", xyzType(*p.blackPoint));
    }
    if (p.chad) {
        std::vector<uint8_t> o = {'s', 'f', '3', '2', 0, 0, 0, 0};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                putS15(o, (*p.chad)[r][c]);
            }
        }
        pushTag("chad", std::move(o));
    }
    if (p.matrix) {
        const Mat3& m = *p.matrix;
        std::array<double, 3> rc = {m[0][0], m[1][0], m[2][0]};
        std::array<double, 3> gc = {m[0][1], m[1][1], m[2][1]};
        std::array<double, 3> bc = {m[0][2], m[1][2], m[2][2]};
        pushTag("rXYZ", xyzType(rc));
        pushTag("gXYZ", xyzType(gc));
        pushTag("bXYZ", xyzType(bc));
    }
    if (p.trc) {
        pushTag("rTRC", curveElem((*p.trc)[0]));
        pushTag("gTRC", curveElem((*p.trc)[1]));
        pushTag("bTRC", curveElem((*p.trc)[2]));
    }
    if (p.grayTrc) {
        pushTag("kTRC", curveElem(*p.grayTrc));
    }
    {
        const char* a2bNames[3] = {"A2B0", "A2B1", "A2B2"};
        const char* b2aNames[3] = {"B2A0", "B2A1", "B2A2"};
        for (int i = 0; i < 3; ++i) {
            if (p.a2b[i]) {
                std::optional<std::vector<uint8_t>> data;
                switch (p.a2b[i]->kind) {
                    case LutKind::Lut16: data = encodeMft(p.a2b[i]->stages, true); break;
                    case LutKind::Lut8: data = encodeMft(p.a2b[i]->stages, false); break;
                    case LutKind::Ab: data = encodeAb(p.a2b[i]->stages, true, p.a2b[i]->inputs, p.a2b[i]->outputs); break;
                }
                if (data) {
                    pushTag(a2bNames[i], std::move(*data));
                }
            }
            if (p.b2a[i]) {
                std::optional<std::vector<uint8_t>> data;
                switch (p.b2a[i]->kind) {
                    case LutKind::Lut16: data = encodeMft(p.b2a[i]->stages, true); break;
                    case LutKind::Lut8: data = encodeMft(p.b2a[i]->stages, false); break;
                    case LutKind::Ab: data = encodeAb(p.b2a[i]->stages, false, p.b2a[i]->inputs, p.b2a[i]->outputs); break;
                }
                if (data) {
                    pushTag(b2aNames[i], std::move(*data));
                }
            }
        }
    }

    // 布局：头部、标签表、数据（相同负载共享一份）。
    const uint32_t tableLen = 4 + static_cast<uint32_t>(tags.size()) * 12;
    std::vector<uint8_t> data;
    struct Entry {
        uint32_t sig;
        uint32_t off;
        uint32_t len;
    };
    struct Placed {
        size_t index;
        uint32_t off;
        uint32_t len;
    };
    std::vector<Entry> entries;
    std::vector<Placed> placed;
    for (size_t i = 0; i < tags.size(); ++i) {
        bool shared = false;
        for (const Placed& pl : placed) {
            if (tags[pl.index].second == tags[i].second) {
                entries.push_back({tags[i].first, pl.off, pl.len});
                shared = true;
                break;
            }
        }
        if (shared) {
            continue;
        }
        const uint32_t off = 128 + tableLen + static_cast<uint32_t>(data.size());
        data.insert(data.end(), tags[i].second.begin(), tags[i].second.end());
        pad4(data);
        entries.push_back({tags[i].first, off, static_cast<uint32_t>(tags[i].second.size())});
        placed.push_back({i, off, static_cast<uint32_t>(tags[i].second.size())});
    }
    const uint32_t size = 128 + tableLen + static_cast<uint32_t>(data.size());
    std::vector<uint8_t> o;
    o.reserve(size);
    putU32(o, size);
    o.insert(o.end(), {0, 0, 0, 0});          // preferred CMM
    o.insert(o.end(), {4, 0x30, 0, 0});       // version 4.3
    putU32(o, profileClassSig(p.klass) ? profileClassSig(p.klass) : p.classSig);
    putU32(o, p.colorSpaceSig != 0 ? p.colorSpaceSig : colorSpaceSig(p.colorSpace));
    putU32(o, p.pcs == Pcs::Lab ? sigOf("Lab ") : sigOf("XYZ "));
    for (uint16_t v : {static_cast<uint16_t>(2026), static_cast<uint16_t>(1), static_cast<uint16_t>(1),
                       static_cast<uint16_t>(0), static_cast<uint16_t>(0), static_cast<uint16_t>(0)}) {
        putU16(o, v);
    }
    o.insert(o.end(), {'a', 'c', 's', 'p'});
    o.insert(o.end(), 4, 0);   // platform
    o.insert(o.end(), 4, 0);   // flags
    o.insert(o.end(), 4, 0);   // manufacturer
    o.insert(o.end(), 4, 0);   // model
    o.insert(o.end(), 8, 0);   // attributes
    putU32(o, static_cast<uint32_t>(p.renderingIntent));
    for (double v : kD50) {
        putS15(o, v);
    }
    o.insert(o.end(), 4, 0);   // creator
    o.insert(o.end(), 16, 0);  // profile id（不计算）
    o.resize(128, 0);
    putU32(o, static_cast<uint32_t>(entries.size()));
    for (const Entry& e : entries) {
        o.push_back(static_cast<uint8_t>(e.sig >> 24));
        o.push_back(static_cast<uint8_t>(e.sig >> 16));
        o.push_back(static_cast<uint8_t>(e.sig >> 8));
        o.push_back(static_cast<uint8_t>(e.sig));
        putU32(o, e.off);
        putU32(o, e.len);
    }
    o.insert(o.end(), data.begin(), data.end());
    return o;
}

}  // namespace cms
}  // namespace montage
