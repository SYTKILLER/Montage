#include "io/cms/lut3d.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace montage {
namespace cms {

Lut3d Lut3d::fromTransform(const Transform& t, size_t size) {
    // 显示 LUT 需要 3→3 或 3→1 变换。
    Lut3d l;
    l.size = size;
    l.data.assign(size * size * size, {0.0f, 0.0f, 0.0f, 1.0f});
    const float s = static_cast<float>(size - 1);
    float out[16] = {0};
    for (size_t b = 0; b < size; ++b) {
        for (size_t g = 0; g < size; ++g) {
            for (size_t r = 0; r < size; ++r) {
                const float in[3] = {static_cast<float>(r) / s, static_cast<float>(g) / s,
                                     static_cast<float>(b) / s};
                t.eval(in, out);
                const float v0 = t.outputs() == 1 ? out[0] : out[0];
                const float v1 = t.outputs() == 1 ? out[0] : out[1];
                const float v2 = t.outputs() == 1 ? out[0] : out[2];
                l.data[r + g * size + b * size * size] = {v0, v1, v2, 1.0f};
            }
        }
    }
    return l;
}

std::array<float, 3> Lut3d::sample(const std::array<float, 3>& rgb) const {
    const size_t n = size;
    auto loc = [&](float v) -> std::pair<size_t, float> {
        const float p = std::min(std::max(v, 0.0f), 1.0f) * static_cast<float>(n - 1);
        size_t i = static_cast<size_t>(p);
        if (i > n - 2) {
            i = n - 2;
        }
        return {i, p - static_cast<float>(i)};
    };
    const auto [r0, fr] = loc(rgb[0]);
    const auto [g0, fg] = loc(rgb[1]);
    const auto [b0, fb] = loc(rgb[2]);
    auto at = [&](size_t r, size_t g, size_t b) -> const std::array<float, 4>& {
        return data[r + g * n + b * n * n];
    };
    std::array<float, 3> o = {0.0f, 0.0f, 0.0f};
    for (size_t k = 0; k < 3; ++k) {
        auto c = [&](size_t r, size_t g, size_t b) { return at(r, g, b)[k]; };
        const float x00 = c(r0, g0, b0) + (c(r0 + 1, g0, b0) - c(r0, g0, b0)) * fr;
        const float x10 = c(r0, g0 + 1, b0) + (c(r0 + 1, g0 + 1, b0) - c(r0, g0 + 1, b0)) * fr;
        const float x01 = c(r0, g0, b0 + 1) + (c(r0 + 1, g0, b0 + 1) - c(r0, g0, b0 + 1)) * fr;
        const float x11 = c(r0, g0 + 1, b0 + 1) + (c(r0 + 1, g0 + 1, b0 + 1) - c(r0, g0 + 1, b0 + 1)) * fr;
        const float y0 = x00 + (x10 - x00) * fg;
        const float y1 = x01 + (x11 - x01) * fg;
        o[k] = y0 + (y1 - y0) * fb;
    }
    return o;
}

uint16_t f32ToF16(float v) {
    uint32_t x = 0;
    std::memcpy(&x, &v, 4);
    const uint16_t sign = static_cast<uint16_t>((x >> 16) & 0x8000);
    const int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF);
    const uint32_t man = x & 0x7FFFFF;
    if (exp == 0xFF) {
        return static_cast<uint16_t>(sign | 0x7C00 | (man != 0 ? 0x200 : 0));
    }
    const int32_t e = exp - 127 + 15;
    if (e >= 0x1F) {
        return static_cast<uint16_t>(sign | 0x7C00);
    }
    if (e <= 0) {
        if (e < -10) {
            return sign;
        }
        const uint32_t m = man | 0x800000;
        const uint32_t shift = static_cast<uint32_t>(14 - e);
        const uint32_t half = 1u << (shift - 1);
        const uint32_t rem = m & ((1u << shift) - 1);
        uint32_t r = m >> shift;
        if (rem > half || (rem == half && (r & 1) == 1)) {
            r += 1;
        }
        return static_cast<uint16_t>(sign | r);
    }
    uint32_t r = (static_cast<uint32_t>(e) << 10) | (man >> 13);
    const uint32_t rem = man & 0x1FFF;
    if (rem > 0x1000 || (rem == 0x1000 && (r & 1) == 1)) {
        r += 1;
    }
    return static_cast<uint16_t>(sign | r);
}

float f16ToF32(uint16_t h) {
    const float sign = (h & 0x8000) != 0 ? -1.0f : 1.0f;
    const int32_t e = (h >> 10) & 0x1F;
    const float m = static_cast<float>(h & 0x3FF);
    switch (e) {
        case 0:
            return sign * m * std::pow(2.0f, -24.0f);
        case 0x1F:
            return m == 0.0f ? sign * std::numeric_limits<float>::infinity()
                             : std::numeric_limits<float>::quiet_NaN();
        default:
            return sign * (1.0f + m / 1024.0f) * std::pow(2.0f, static_cast<float>(e - 15));
    }
}

std::vector<uint8_t> Lut3d::toRgba16fBytes() const {
    std::vector<uint8_t> o;
    o.reserve(data.size() * 8);
    for (const auto& px : data) {
        for (float v : px) {
            const uint16_t h = f32ToF16(v);
            o.push_back(static_cast<uint8_t>(h & 0xFF));
            o.push_back(static_cast<uint8_t>(h >> 8));
        }
    }
    return o;
}

std::vector<uint8_t> Lut3d::toRgba8() const {
    std::vector<uint8_t> o;
    o.reserve(data.size() * 4);
    for (const auto& px : data) {
        for (float v : px) {
            o.push_back(static_cast<uint8_t>(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f));
        }
    }
    return o;
}

}  // namespace cms
}  // namespace montage
