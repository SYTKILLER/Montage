// M23a：DNG opcode 列表解析实现。翻译自 photocraft crates/raw/src/opcodes.rs。

#include "io/raw/raw_opcodes.h"

#include <cstring>

namespace montage {
namespace raw {

namespace {

std::string opcodeName(uint32_t id) {
    switch (id) {
        case 1: return "WarpRectilinear";
        case 2: return "WarpFisheye";
        case 3: return "FixVignetteRadial";
        case 4: return "FixBadPixelsConstant";
        case 5: return "FixBadPixelsList";
        case 6: return "TrimBounds";
        case 7: return "MapTable";
        case 8: return "MapPolynomial";
        case 9: return "GainMap";
        case 10: return "DeltaPerRow";
        case 11: return "DeltaPerColumn";
        case 12: return "ScalePerRow";
        case 13: return "ScalePerColumn";
        case 14: return "WarpRectilinear2";
        default: return "opcode " + std::to_string(id);
    }
}

std::optional<uint32_t> be32(const uint8_t* b, size_t len, size_t at) {
    if (at > len || len - at < 4) return std::nullopt;
    return static_cast<uint32_t>(b[at]) << 24 | static_cast<uint32_t>(b[at + 1]) << 16 |
           static_cast<uint32_t>(b[at + 2]) << 8 | static_cast<uint32_t>(b[at + 3]);
}

std::optional<double> be64f(const uint8_t* b, size_t len, size_t at) {
    if (at > len || len - at < 8) return std::nullopt;
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits = bits << 8 | b[at + i];
    double v;
    std::memcpy(&v, &bits, 8);
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

std::optional<GainMap> gainMap(const uint8_t* p, size_t len) {
    auto u = [&](size_t i) -> std::optional<size_t> {
        auto v = be32(p, len, 4 * i);
        if (!v) return std::nullopt;
        return static_cast<size_t>(*v);
    };
    auto top = u(0), left = u(1), bottom = u(2), right = u(3);
    auto plane = u(4), planes = u(5), rowPitch = u(6), colPitch = u(7);
    auto pointsV = u(8), pointsH = u(9);
    auto spacingV = be64f(p, len, 40), spacingH = be64f(p, len, 48);
    auto originV = be64f(p, len, 56), originH = be64f(p, len, 64);
    auto mapPlanes = u(18);
    if (!top || !left || !bottom || !right || !plane || !planes || !rowPitch || !colPitch || !pointsV || !pointsH ||
        !spacingV || !spacingH || !originV || !originH || !mapPlanes) {
        return std::nullopt;
    }
    size_t n = *pointsV * *pointsH;
    if (n > (1u << 20) / (*mapPlanes ? *mapPlanes : 1)) return std::nullopt;
    n *= *mapPlanes;
    if (n == 0 || n > (1u << 20) || *rowPitch == 0 || *colPitch == 0 || *spacingV <= 0.0 || *spacingH <= 0.0 ||
        *bottom <= *top || *right <= *left) {
        return std::nullopt;
    }
    if (76 + 4 * n > len) return std::nullopt;
    GainMap m;
    m.top = *top; m.left = *left; m.bottom = *bottom; m.right = *right;
    m.plane = *plane; m.planes = *planes;
    m.rowPitch = *rowPitch; m.colPitch = *colPitch;
    m.pointsV = *pointsV; m.pointsH = *pointsH;
    m.spacingV = *spacingV; m.spacingH = *spacingH;
    m.originV = *originV; m.originH = *originH;
    m.mapPlanes = *mapPlanes;
    m.gains.resize(n);
    for (size_t i = 0; i < n; ++i) {
        auto v = be32(p, len, 76 + 4 * i);
        if (!v) return std::nullopt;
        float g;
        std::memcpy(&g, &*v, 4);
        if (!std::isfinite(g)) return std::nullopt;
        m.gains[i] = g;
    }
    return m;
}

}  // namespace

OpcodeListResult parseOpcodeList(const uint8_t* list, size_t len) {
    OpcodeListResult out;
    auto countOpt = be32(list, len, 0);
    if (!countOpt) return out;
    size_t at = 4;
    size_t count = std::min<size_t>(*countOpt, 4096);
    for (size_t i = 0; i < count; ++i) {
        auto id = be32(list, len, at);
        auto size = be32(list, len, at + 12);
        if (!id || !size) break;
        size_t start = at + 16;
        if (start > len || *size > len - start) break;
        auto m = gainMap(list + start, *size);
        if (*id == 9 && m) {
            out.maps.push_back(std::move(*m));
        } else {
            std::string n = opcodeName(*id);
            bool found = false;
            for (const std::string& s : out.skipped)
                if (s == n) { found = true; break; }
            if (!found) out.skipped.push_back(n);
        }
        at = start + *size;
    }
    return out;
}

}  // namespace raw
}  // namespace montage
