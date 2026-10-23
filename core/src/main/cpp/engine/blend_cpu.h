#ifndef MONTAGE_ENGINE_BLEND_CPU_H
#define MONTAGE_ENGINE_BLEND_CPU_H

// CPU 侧 24 混合模式公式唯一来源（M21 从 io/image_export.cpp 提取共享）：
// GL 管线（gen_shaders.py BODIES）与 CPU flatten/智能滤镜混合同构。Cs=源（上层），Cb=底。

#include <algorithm>
#include <cmath>

#include "engine/document.h"

namespace montage {
namespace blend_cpu {

inline float lum(const float c[3]) {
    return c[0] * 0.3f + c[1] * 0.59f + c[2] * 0.11f;
}

inline void clipColor(float c[3]) {
    const float l = lum(c);
    const float n = std::min(c[0], std::min(c[1], c[2]));
    const float x = std::max(c[0], std::max(c[1], c[2]));
    if (x > 1.0f) {
        for (int i = 0; i < 3; ++i) {
            c[i] = l + (c[i] - l) * ((1.0f - l) / std::max(1e-6f, x - l));
        }
    }
    if (n < 0.0f) {
        for (int i = 0; i < 3; ++i) {
            c[i] = l + (c[i] - l) * (l / std::max(1e-6f, l - n));
        }
    }
}

inline void setLum(float c[3], float l) {
    const float dl = l - lum(c);
    c[0] += dl;
    c[1] += dl;
    c[2] += dl;
    clipColor(c);
}

inline float sat(const float c[3]) {
    return std::max(c[0], std::max(c[1], c[2])) - std::min(c[0], std::min(c[1], c[2]));
}

inline void setSat(float c[3], float s) {
    float mn = std::min(c[0], std::min(c[1], c[2]));
    float mx = std::max(c[0], std::max(c[1], c[2]));
    float md = c[0] + c[1] + c[2] - mn - mx;
    float nv = mx > mn ? (md - mn) * s / (mx - mn) : 0.0f;
    if (c[0] == mx) {
        c[0] = s;
        c[1] = nv;
        c[2] = 0.0f;
    } else if (c[1] == mx) {
        c[0] = 0.0f;
        c[1] = s;
        c[2] = nv;
    } else {
        c[0] = nv;
        c[1] = 0.0f;
        c[2] = s;
    }
}

inline float stepHalf(float v) {
    return v >= 0.5f ? 1.0f : 0.0f;
}

inline float vclamp01(float v) {
    return std::min(1.0f, std::max(0.0f, v));
}

// mix(x, y, a) 按 GLSL 逐通道
inline float mix(float x, float y, float a) {
    return x + (y - x) * a;
}

// blendB 返回 B（写入 out[3]）
inline void blendBody(BlendMode mode, const float Cb[3], const float Cs[3], float out[3]) {
    switch (mode) {
        case BlendMode::Normal:
            out[0] = Cs[0];
            out[1] = Cs[1];
            out[2] = Cs[2];
            break;
        case BlendMode::Darken:
            for (int i = 0; i < 3; ++i) out[i] = std::min(Cb[i], Cs[i]);
            break;
        case BlendMode::Multiply:
            for (int i = 0; i < 3; ++i) out[i] = Cb[i] * Cs[i];
            break;
        case BlendMode::ColorBurn:
            for (int i = 0; i < 3; ++i)
                out[i] = std::max(1.0f - std::min(1.0f, (1.0f - Cb[i]) / std::max(1e-5f, Cs[i])), 0.0f);
            break;
        case BlendMode::LinearBurn:
            for (int i = 0; i < 3; ++i) out[i] = std::max(Cb[i] + Cs[i] - 1.0f, 0.0f);
            break;
        case BlendMode::Lighten:
            for (int i = 0; i < 3; ++i) out[i] = std::max(Cb[i], Cs[i]);
            break;
        case BlendMode::Screen:
            for (int i = 0; i < 3; ++i) out[i] = Cb[i] + Cs[i] - Cb[i] * Cs[i];
            break;
        case BlendMode::ColorDodge:
            for (int i = 0; i < 3; ++i)
                out[i] = std::max(std::min(1.0f, Cb[i] / std::max(1e-5f, 1.0f - Cs[i])), 0.0f);
            break;
        case BlendMode::LinearDodge:
            for (int i = 0; i < 3; ++i) out[i] = std::min(Cb[i] + Cs[i], 1.0f);
            break;
        case BlendMode::Overlay:
            for (int i = 0; i < 3; ++i) {
                const float a = stepHalf(Cb[i]);
                out[i] = mix(2.0f * Cb[i] * Cs[i], 1.0f - 2.0f * (1.0f - Cb[i]) * (1.0f - Cs[i]), a);
            }
            break;
        case BlendMode::SoftLight: {
            // GLSL: mix(Cb + (2Cs-1)*(mix((16Cb-12)Cb+4, sqrt(Cb), step(0.25, Cb)) - Cb),
            //           Cb - (1-2Cs)*Cb*(1-Cb), step(0.5, Cs))
            for (int i = 0; i < 3; ++i) {
                const float inner = mix((16.0f * Cb[i] - 12.0f) * Cb[i] + 4.0f,
                                        std::sqrt(std::max(0.0f, Cb[i])), stepHalf(Cb[i] - 0.25f));
                const float a = stepHalf(Cs[i]);
                out[i] = mix(Cb[i] + (2.0f * Cs[i] - 1.0f) * (inner - Cb[i]),
                             Cb[i] - (1.0f - 2.0f * Cs[i]) * Cb[i] * (1.0f - Cb[i]), a);
            }
            break;
        }
        case BlendMode::HardLight:
            for (int i = 0; i < 3; ++i) {
                const float a = stepHalf(Cs[i]);
                out[i] = mix(2.0f * Cb[i] * Cs[i], 1.0f - 2.0f * (1.0f - Cb[i]) * (1.0f - Cs[i]), a);
            }
            break;
        case BlendMode::VividLight:
            for (int i = 0; i < 3; ++i) {
                const float a = stepHalf(Cs[i]);
                const float t1 = 1.0f - std::min(1.0f, (1.0f - Cb[i]) / std::max(1e-5f, 2.0f * Cs[i]));
                const float t2 = std::min(1.0f, Cb[i] / std::max(1e-5f, 2.0f * (1.0f - Cs[i])));
                out[i] = std::max(mix(t1, t2, a), 0.0f);
            }
            break;
        case BlendMode::LinearLight:
            for (int i = 0; i < 3; ++i) out[i] = vclamp01(Cb[i] + 2.0f * Cs[i] - 1.0f);
            break;
        case BlendMode::PinLight:
            for (int i = 0; i < 3; ++i) {
                const float a = stepHalf(Cs[i]);
                out[i] = mix(std::min(Cb[i], 2.0f * Cs[i]), std::max(Cb[i], 2.0f * Cs[i] - 1.0f), a);
            }
            break;
        case BlendMode::HardMix:
            for (int i = 0; i < 3; ++i) {
                const float a = stepHalf(Cs[i]);
                const float m = mix(std::min(Cb[i], 2.0f * Cs[i]),
                                    std::max(Cb[i], 2.0f * Cs[i] - 1.0f), a);
                out[i] = stepHalf(m - 0.5f);
            }
            break;
        case BlendMode::Difference:
            for (int i = 0; i < 3; ++i) out[i] = std::fabs(Cb[i] - Cs[i]);
            break;
        case BlendMode::Exclusion:
            for (int i = 0; i < 3; ++i) out[i] = Cb[i] + Cs[i] - 2.0f * Cb[i] * Cs[i];
            break;
        case BlendMode::Subtract:
            for (int i = 0; i < 3; ++i) out[i] = std::max(Cb[i] - Cs[i], 0.0f);
            break;
        case BlendMode::Divide:
            for (int i = 0; i < 3; ++i) out[i] = std::min(1.0f, Cs[i] / std::max(1e-5f, 1.0f - Cb[i]));
            break;
        case BlendMode::Hue: {
            float c[3] = {Cs[0], Cs[1], Cs[2]};
            setSat(c, sat(Cb));
            setLum(c, lum(Cb));
            out[0] = c[0];
            out[1] = c[1];
            out[2] = c[2];
            break;
        }
        case BlendMode::Saturation: {
            float c[3] = {Cb[0], Cb[1], Cb[2]};
            setSat(c, sat(Cs));
            setLum(c, lum(Cb));
            out[0] = c[0];
            out[1] = c[1];
            out[2] = c[2];
            break;
        }
        case BlendMode::Color: {
            float c[3] = {Cs[0], Cs[1], Cs[2]};
            setLum(c, lum(Cb));
            out[0] = c[0];
            out[1] = c[1];
            out[2] = c[2];
            break;
        }
        case BlendMode::Luminosity: {
            float c[3] = {Cb[0], Cb[1], Cb[2]};
            setLum(c, lum(Cs));
            out[0] = c[0];
            out[1] = c[1];
            out[2] = c[2];
            break;
        }
        default:
            out[0] = Cs[0];
            out[1] = Cs[1];
            out[2] = Cs[2];
            break;
    }
}

// W3C 混合+合成（photocraft color/src/blend.rs composite 直译，直 alpha RGBA 0..1）：
// premul_c = (1−as)·ab·Cb + (1−ab)·as·Cs + as·ab·B(Cb,Cs)，as = αs·opacity；
// out_c = premul/ao、out_a = ao = as + ab·(1−as)。M21 智能滤镜混合用。
inline void composite(BlendMode mode, const float backdrop[4], const float source[4],
                      float opacity, float out[4]) {
    const float ab = backdrop[3];
    const float as = source[3] * opacity;
    if (as <= 0.0f) {
        out[0] = backdrop[0];
        out[1] = backdrop[1];
        out[2] = backdrop[2];
        out[3] = ab;
        return;
    }
    float b[3];
    blendBody(mode, backdrop, source, b);
    const float ao = as + ab * (1.0f - as);
    for (int i = 0; i < 3; ++i) {
        const float premul = (1.0f - as) * ab * backdrop[i] + (1.0f - ab) * as * source[i] +
                             as * ab * b[i];
        out[i] = ao > 0.0f ? premul / ao : 0.0f;
    }
    out[3] = ao;
}

}  // namespace blend_cpu
}  // namespace montage

#endif  // MONTAGE_ENGINE_BLEND_CPU_H
