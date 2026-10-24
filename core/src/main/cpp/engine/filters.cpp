#include "engine/filters.h"

#include "engine/camera_raw.h"

#include "io/cms/lutfile.h"
#include "engine/lens_ops.h"

// M13 滤镜核实现。算法对拍 photocraft algo crate（MIT，07 §3.4 注入，公式语义逐条照抄）：
// gaussian_kernel/boxes_for_gauss/conv_sep（blur.rs）、unsharp/high_pass（sharpen.rs）、
// hash01/add（noise.rs）、mosaic/emboss/find_edges（stylize.rs）、min_max（other.rs）。
// 像素域：核内 float 0..1 直 alpha；卷积类先预乘后去预乘；域外 = 透明（r 0,0,0,0）。

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace montage {
namespace filters {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// ---- 像素存取（直 alpha uint8 域） ----

inline void getPx(const uint8_t* img, uint32_t w, uint32_t h, int x, int y, float out[4]) {
    const int cx = std::clamp(x, 0, static_cast<int>(w) - 1);
    const int cy = std::clamp(y, 0, static_cast<int>(h) - 1);
    const uint8_t* p = img + (static_cast<size_t>(cy) * w + cx) * 4u;
    out[0] = p[0] / 255.0f;
    out[1] = p[1] / 255.0f;
    out[2] = p[2] / 255.0f;
    out[3] = p[3] / 255.0f;
}

inline void setPx(uint8_t* img, uint32_t w, int x, int y, const float v[4]) {
    uint8_t* p = img + (static_cast<size_t>(y) * w + x) * 4u;
    for (int c = 0; c < 4; ++c) {
        const float f = std::clamp(v[c], 0.0f, 1.0f);
        p[c] = static_cast<uint8_t>(f * 255.0f + 0.5f);
    }
}

// 域外 = 透明（全域语义）
inline void getPxT(const uint8_t* img, uint32_t w, uint32_t h, int x, int y, float out[4]) {
    if (x < 0 || y < 0 || x >= static_cast<int>(w) || y >= static_cast<int>(h)) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    getPx(img, w, h, x, y, out);
}

inline uint8_t luma255(const uint8_t* p) {
    // photocraft luma（sRGB 权重）
    return static_cast<uint8_t>(0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2] + 0.5f);
}

// ---- 噪声（photocraft noise.rs hash01） ----

inline float hash01(int x, int y, uint32_t c, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^ static_cast<uint32_t>(y) * 0xd8163841u ^
                 c * 0xcb1ab31fu ^ seed * 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / static_cast<float>(1u << 24);
}

// ---- 预乘 float 缓冲（卷积域） ----

void toPremul(const uint8_t* src, uint32_t w, uint32_t h, std::vector<float>& out) {
    out.resize(static_cast<size_t>(w) * h * 4u);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const float a = src[i * 4u + 3u] / 255.0f;
        out[i * 4u + 0u] = src[i * 4u + 0u] / 255.0f * a;
        out[i * 4u + 1u] = src[i * 4u + 1u] / 255.0f * a;
        out[i * 4u + 2u] = src[i * 4u + 2u] / 255.0f * a;
        out[i * 4u + 3u] = a;
    }
}

void fromPremulToFractions(const std::vector<float>& pm, std::vector<float>& out) {
    out.resize(pm.size());
    for (size_t i = 0; i < pm.size(); i += 4) {
        const float a = pm[i + 3];
        if (a > 1e-6f) {
            const float inv = 1.0f / a;
            out[i + 0] = std::clamp(pm[i + 0] * inv, 0.0f, 1.0f);
            out[i + 1] = std::clamp(pm[i + 1] * inv, 0.0f, 1.0f);
            out[i + 2] = std::clamp(pm[i + 2] * inv, 0.0f, 1.0f);
        } else {
            out[i + 0] = out[i + 1] = out[i + 2] = 0.0f;
        }
        out[i + 3] = a;
    }
}

// ---- 可分离卷积（photocraft conv_sep 语义；edge clamp 透明由调用侧 pad 保证） ----

void convSep(std::vector<float>& img, uint32_t w, uint32_t h, const std::vector<float>& k) {
    const int r = static_cast<int>(k.size()) / 2;
    // 水平趟
    std::vector<float> tmp(static_cast<size_t>(w) * h * 4u);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int i = -r; i <= r; ++i) {
                const int xx = std::clamp(static_cast<int>(x) + i, 0, static_cast<int>(w) - 1);
                const float* p = &img[(static_cast<size_t>(y) * w + xx) * 4u];
                const float kv = k[i + r];
                for (int c = 0; c < 4; ++c) {
                    acc[c] += p[c] * kv;
                }
            }
            std::memcpy(&tmp[(static_cast<size_t>(y) * w + x) * 4u], acc, sizeof(acc));
        }
    }
    // 垂直趟（写回 img）
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int i = -r; i <= r; ++i) {
                const int yy = std::clamp(static_cast<int>(y) + i, 0, static_cast<int>(h) - 1);
                const float* p = &tmp[(static_cast<size_t>(yy) * w + x) * 4u];
                const float kv = k[i + r];
                for (int c = 0; c < 4; ++c) {
                    acc[c] += p[c] * kv;
                }
            }
            std::memcpy(&img[(static_cast<size_t>(y) * w + x) * 4u], acc, sizeof(acc));
        }
    }
}

std::vector<float> gaussianKernel(float sigma) {
    if (sigma < 0.05f) {
        return {1.0f};
    }
    const int r = static_cast<int>(std::ceil(sigma * 3.0f));
    std::vector<float> k(static_cast<size_t>(2 * r + 1));
    float sum = 0.0f;
    for (int i = -r; i <= r; ++i) {
        k[i + r] = std::exp(-static_cast<float>(i * i) / (2.0f * sigma * sigma));
        sum += k[i + r];
    }
    for (float& v : k) {
        v /= sum;
    }
    return k;
}

// boxes_for_gauss（photocraft blur.rs，三 box 高斯近似）
std::vector<size_t> boxesForGauss(float sigma, size_t n) {
    const float nf = static_cast<float>(n);
    const float wIdeal = std::sqrt(12.0f * sigma * sigma / nf + 1.0f);
    int wl = static_cast<int>(std::floor(wIdeal));
    if (wl % 2 == 0) {
        --wl;
    }
    wl = std::max(wl, 1);
    const int wu = wl + 2;
    const float mIdeal =
        (12.0f * sigma * sigma - nf * wl * wl - 4.0f * nf * wl - 3.0f * nf) / (-4.0f * wl - 4.0f);
    const size_t m = static_cast<size_t>(std::clamp(std::round(mIdeal), 0.0f, nf));
    std::vector<size_t> boxes(n);
    for (size_t i = 0; i < n; ++i) {
        boxes[i] = i < m ? static_cast<size_t>(wl) : static_cast<size_t>(wu);
    }
    return boxes;
}

// 单趟 box 模糊（滑动窗口平均；width=窗口宽）
void boxPassAxis(std::vector<float>& img, uint32_t w, uint32_t h, size_t boxW, bool horizontal) {
    if (boxW < 1) {
        return;
    }
    const int r = static_cast<int>(boxW) / 2;
    std::vector<float> out(img.size());
    const uint32_t major = horizontal ? w : h;
    const uint32_t minor = horizontal ? h : w;
    for (uint32_t m = 0; m < minor; ++m) {
        for (uint32_t i = 0; i < major; ++i) {
            float acc[4] = {0, 0, 0, 0};
            for (int j = -r; j <= r; ++j) {
                const int idx = std::clamp(static_cast<int>(i) + j, 0, static_cast<int>(major) - 1);
                const float* p = horizontal ? &img[(static_cast<size_t>(m) * w + idx) * 4u]
                                            : &img[(static_cast<size_t>(idx) * w + m) * 4u];
                for (int c = 0; c < 4; ++c) {
                    acc[c] += p[c];
                }
            }
            float* d = horizontal ? &out[(static_cast<size_t>(m) * w + i) * 4u]
                                  : &out[(static_cast<size_t>(i) * w + m) * 4u];
            const float norm = 1.0f / static_cast<float>(2 * r + 1);
            for (int c = 0; c < 4; ++c) {
                d[c] = acc[c] * norm;
            }
        }
    }
    img.swap(out);
}

void boxBlur(std::vector<float>& img, uint32_t w, uint32_t h, size_t boxW) {
    boxPassAxis(img, w, h, boxW, true);
    boxPassAxis(img, w, h, boxW, false);
}

// 高斯模糊（σ≤4 精确核；σ>4 三 box 近似——photocraft box_widths 分界）
void gaussianBlur(std::vector<float>& pm, uint32_t w, uint32_t h, float sigma) {
    if (sigma <= 4.0f) {
        convSep(pm, w, h, gaussianKernel(sigma));
    } else {
        const std::vector<size_t> boxes = boxesForGauss(sigma, 3);
        for (size_t bw : boxes) {
            boxBlur(pm, w, h, bw);
        }
    }
}

// 模糊后回直 alpha float
void blurFraction(const uint8_t* src, uint32_t w, uint32_t h, float sigma, std::vector<float>& out) {
    std::vector<float> pm;
    toPremul(src, w, h, pm);
    gaussianBlur(pm, w, h, sigma);
    fromPremulToFractions(pm, out);
}

// 亮度（float 0..1 域）
inline float luma(const float p[4]) {
    return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
}

inline float wrap01(float v) {
    return v - std::floor(v);
}

// 值噪声（云彩用；八 octave 分形）
float valueNoise(float x, float y, uint32_t seed) {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float xf = x - xi;
    const float yf = y - yi;
    const float u = xf * xf * (3.0f - 2.0f * xf);
    const float v = yf * yf * (3.0f - 2.0f * yf);
    const float a = hash01(xi, yi, 7, seed);
    const float b = hash01(xi + 1, yi, 7, seed);
    const float c = hash01(xi, yi + 1, 7, seed);
    const float d = hash01(xi + 1, yi + 1, 7, seed);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

float fbmNoise(float x, float y, uint32_t seed, int octaves) {
    float sum = 0.0f;
    float amp = 0.5f;
    float freq = 1.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += valueNoise(x * freq, y * freq, seed + static_cast<uint32_t>(o) * 131u) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum / (norm > 1e-6f ? norm : 1.0f);
}

}  // namespace

bool applyFilter(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h,
                 const FilterParams& p, std::string& err) {
    if (w == 0 || h == 0) {
        err = "empty area";
        return false;
    }
    const size_t n = static_cast<size_t>(w) * h;
    switch (p.kind) {
        case FGaussianBlur:
        case FBoxBlur: {
            const float radius = std::max(0.0f, p.p0);
            std::vector<float> pm;
            toPremul(src, w, h, pm);
            if (p.kind == FGaussianBlur) {
                gaussianBlur(pm, w, h, radius);
            } else if (radius > 4.0f) {
                boxBlur(pm, w, h, static_cast<size_t>(std::round(radius)) * 2u + 1u);
            } else {
                convSep(pm, w, h, gaussianKernel(radius * 0.55f));  // 小半径核近似（box 窗）
            }
            // 预乘 → uint8 直 alpha
            for (size_t i = 0; i < n; ++i) {
                const float a = pm[i * 4u + 3u];
                const float inv = a > 1e-6f ? 1.0f / a : 0.0f;
                for (int c = 0; c < 3; ++c) {
                    dst[i * 4u + c] = static_cast<uint8_t>(
                        std::clamp(pm[i * 4u + c] * inv, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                dst[i * 4u + 3] = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            break;
        }
        case FMotionBlur: {
            // 沿角度方向线采样平均（photocraft motion 大距离走 FFT——v1 直接采样，登记）
            const float ang = p.p0 * kPi / 180.0f;
            const int dist = std::max(1, static_cast<int>(std::round(p.p1)));
            std::vector<float> frac(n * 4u);
            const float dx = std::cos(ang);
            const float dy = std::sin(ang);
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    float acc[4] = {0, 0, 0, 0};
                    for (int s = -dist; s <= dist; ++s) {
                        float sp[4];
                        getPxT(src, w, h, static_cast<int>(x) + static_cast<int>(std::lround(dx * s)),
                               static_cast<int>(y) + static_cast<int>(std::lround(dy * s)), sp);
                        for (int c = 0; c < 4; ++c) {
                            acc[c] += sp[c];
                        }
                    }
                    for (int c = 0; c < 4; ++c) {
                        frac[(static_cast<size_t>(y) * w + x) * 4u + c] = acc[c] / (2.0f * dist + 1.0f);
                    }
                }
            }
            for (size_t i = 0; i < n * 4u; ++i) {
                dst[i] = static_cast<uint8_t>(std::clamp(frac[i], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            break;
        }
        case FUnsharpMask:
        case FSmartSharpen:
        case FHighPass: {
            std::vector<float> orig(n * 4u);
            std::vector<float> blurred;
            for (size_t i = 0; i < n; ++i) {
                for (int c = 0; c < 4; ++c) {
                    orig[i * 4u + c] = src[i * 4u + c] / 255.0f;
                }
            }
            blurFraction(src, w, h, std::max(0.1f, p.kind == FHighPass ? p.p0 : p.p1), blurred);
            const float k = p.p0 / 100.0f;                       // amount%
            const float t = p.p2 / 255.0f;                       // threshold levels
            const float nr = std::clamp(p.p2 / 100.0f, 0.0f, 1.0f);  // reduceNoise%
            for (size_t i = 0; i < n; ++i) {
                float o[4], b[4];
                for (int c = 0; c < 4; ++c) {
                    o[c] = orig[i * 4u + c];
                    b[c] = blurred[i * 4u + c];
                }
                if (o[3] > 1e-6f) {
                    for (int c = 0; c < 3; ++c) {
                        const float d = o[c] - b[c];
                        float v = o[c];
                        if (p.kind == FUnsharpMask) {
                            if (std::abs(d) + 1e-6f >= t) {
                                v = o[c] + k * d;
                            }
                        } else if (p.kind == FSmartSharpen) {
                            v = o[c] + k * d * (1.0f - nr * std::exp(-40.0f * std::abs(d)));
                        } else {  // HighPass
                            v = 0.5f + d;
                        }
                        o[c] = std::clamp(v, 0.0f, 1.0f);
                    }
                }
                for (int c = 0; c < 4; ++c) {
                    dst[i * 4u + c] = static_cast<uint8_t>(o[c] * 255.0f + 0.5f);
                }
            }
            break;
        }
        case FAddNoise: {
            const float a = std::max(0.0f, p.p0) / 100.0f * 0.5f;
            const bool gauss = p.p1 > 0.5f;
            const bool mono = p.p2 > 0.5f;
            std::vector<float> frac(n * 4u);
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t i = (static_cast<size_t>(y) * w + x) * 4u;
                    float v[4];
                    getPx(src, w, h, static_cast<int>(x), static_cast<int>(y), v);
                    if (v[3] > 0.0f) {
                        for (int c = 0; c < 3; ++c) {
                            const uint32_t ch = mono ? 0u : static_cast<uint32_t>(c);
                            float d = 0.0f;
                            if (!gauss) {
                                d = (hash01(x, y, ch, p.seed) - 0.5f) * 2.0f * a;
                            } else {
                                const float u1 = std::max(1e-7f, hash01(x, y, ch * 2u + 101u, p.seed));
                                const float u2 = hash01(x, y, ch * 2u + 102u, p.seed);
                                d = std::sqrt(-2.0f * std::log(u1)) *
                                    std::cos(2.0f * kPi * u2) * a * 0.5f;
                            }
                            v[c] = std::clamp(v[c] + d, 0.0f, 1.0f);
                        }
                    }
                    std::memcpy(&frac[i], v, sizeof(v));
                }
            }
            for (size_t i = 0; i < n * 4u; ++i) {
                dst[i] = static_cast<uint8_t>(std::clamp(frac[i], 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            break;
        }
        case FMedian:
        case FDustScratches: {
            const int r = std::max(1, static_cast<int>(p.p0));
            const float thr = p.p1 / 255.0f;  // DustScratches threshold
            const int side = 2 * r + 1;
            std::vector<float> win;
            win.reserve(static_cast<size_t>(side * side));
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    uint8_t out[4] = {0, 0, 0, 0};
                    out[3] = src[(static_cast<size_t>(y) * w + x) * 4u + 3u];
                    for (int c = 0; c < 3; ++c) {
                        win.clear();
                        for (int dy = -r; dy <= r; ++dy) {
                            for (int dx = -r; dx <= r; ++dx) {
                                const int xx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(w) - 1);
                                const int yy = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(h) - 1);
                                win.push_back(src[(static_cast<size_t>(yy) * w + xx) * 4u + c] / 255.0f);
                            }
                        }
                        const size_t mid = win.size() / 2;
                        std::nth_element(win.begin(), win.begin() + static_cast<long>(mid), win.end());
                        float m = win[mid];
                        if (p.kind == FDustScratches) {
                            const float o = src[(static_cast<size_t>(y) * w + x) * 4u + c] / 255.0f;
                            if (std::abs(o - m) < thr) {
                                m = o;  // 差异小于阈值保持原值（PS 蒙尘与划痕语义）
                            }
                        }
                        out[c] = static_cast<uint8_t>(std::clamp(m, 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                    std::memcpy(dst + (static_cast<size_t>(y) * w + x) * 4u, out, 4);
                }
            }
            break;
        }
        case FReduceNoise: {
            // 双边滤波近似（07 风险条款：v1 近似并登记）
            const int r = std::max(1, static_cast<int>(p.p0));
            const float sigmacolor = 0.15f;
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    float c0[4];
                    getPx(src, w, h, static_cast<int>(x), static_cast<int>(y), c0);
                    float acc[3] = {0, 0, 0};
                    float wsum = 0.0f;
                    if (c0[3] > 0.0f) {
                        for (int dy = -r; dy <= r; ++dy) {
                            for (int dx = -r; dx <= r; ++dx) {
                                float s[4];
                                getPx(src, w, h, static_cast<int>(x) + dx, static_cast<int>(y) + dy, s);
                                if (s[3] <= 0.0f) {
                                    continue;
                                }
                                const float d2 = (s[0] - c0[0]) * (s[0] - c0[0]) +
                                                 (s[1] - c0[1]) * (s[1] - c0[1]) +
                                                 (s[2] - c0[2]) * (s[2] - c0[2]);
                                const float wt = std::exp(-d2 / (2.0f * sigmacolor * sigmacolor));
                                for (int c = 0; c < 3; ++c) {
                                    acc[c] += s[c] * wt;
                                }
                                wsum += wt;
                            }
                        }
                    }
                    uint8_t* d = dst + (static_cast<size_t>(y) * w + x) * 4u;
                    if (wsum > 1e-6f) {
                        for (int c = 0; c < 3; ++c) {
                            d[c] = static_cast<uint8_t>(
                                std::clamp(acc[c] / wsum, 0.0f, 1.0f) * 255.0f + 0.5f);
                        }
                    } else {
                        d[0] = d[1] = d[2] = 0;
                    }
                    d[3] = static_cast<uint8_t>(c0[3]);
                }
            }
            break;
        }
        case FMosaic: {
            const int cell = std::max(2, static_cast<int>(std::round(p.p0)));
            for (uint32_t cy = 0; cy < h; cy += static_cast<uint32_t>(cell)) {
                for (uint32_t cx = 0; cx < w; cx += static_cast<uint32_t>(cell)) {
                    float sum[4] = {0, 0, 0, 0};
                    float cnt = 0.0f;
                    for (uint32_t y = cy; y < std::min(h, cy + static_cast<uint32_t>(cell)); ++y) {
                        for (uint32_t x = cx; x < std::min(w, cx + static_cast<uint32_t>(cell)); ++x) {
                            float v[4];
                            getPx(src, w, h, static_cast<int>(x), static_cast<int>(y), v);
                            const float a = v[3];
                            for (int c = 0; c < 3; ++c) {
                                sum[c] += v[c] * a;
                            }
                            sum[3] += a;
                            cnt += 1.0f;
                        }
                    }
                    float avg[4] = {0, 0, 0, 0};
                    if (cnt > 0.0f && sum[3] > 1e-6f) {
                        for (int c = 0; c < 3; ++c) {
                            avg[c] = std::clamp(sum[c] / sum[3], 0.0f, 1.0f);
                        }
                        avg[3] = std::clamp(sum[3] / cnt, 0.0f, 1.0f);
                    }
                    for (uint32_t y = cy; y < std::min(h, cy + static_cast<uint32_t>(cell)); ++y) {
                        for (uint32_t x = cx; x < std::min(w, cx + static_cast<uint32_t>(cell)); ++x) {
                            setPx(dst, w, static_cast<int>(x), static_cast<int>(y), avg);
                        }
                    }
                }
            }
            break;
        }
        case FCrystallize:
        case FPointillize: {
            // Voronoi 网格抖动（photocraft crystallize 语义）：像素归最近 jitter 中心
            const float cell = std::max(3.0f, p.p0);
            const float inv = 1.0f / cell;
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const int gy = static_cast<int>(std::floor(y * inv));
                    const int gx = static_cast<int>(std::floor(x * inv));
                    float best = 1e12f;
                    float bc[4] = {0, 0, 0, 0};
                    for (int oy = -1; oy <= 1; ++oy) {
                        for (int ox = -1; ox <= 1; ++ox) {
                            const int cgy = gy + oy;
                            const int cgx = gx + ox;
                            const float jx = (cgx + 0.2f + 0.6f * hash01(cgx, cgy, 11u, p.seed)) * cell;
                            const float jy = (cgy + 0.2f + 0.6f * hash01(cgx, cgy, 13u, p.seed)) * cell;
                            const float d2 = (x - jx) * (x - jx) + (y - jy) * (y - jy);
                            if (d2 < best) {
                                best = d2;
                                float v[4];
                                getPxT(src, w, h, static_cast<int>(jx), static_cast<int>(jy), v);
                                std::memcpy(bc, v, sizeof(v));
                            }
                        }
                    }
                    if (p.kind == FPointillize && bc[3] > 0.0f) {
                        // 点状化：点保留、格底透明（photocraft background 参数 v1 = 透明）
                        const float jx = (gx + 0.2f + 0.6f * hash01(gx, gy, 11u, p.seed)) * cell;
                        const float jy = (gy + 0.2f + 0.6f * hash01(gx, gy, 13u, p.seed)) * cell;
                        const float d2 = (x - jx) * (x - jx) + (y - jy) * (y - jy);
                        if (d2 > cell * cell * 0.2f) {
                            bc[3] = 0.0f;
                        }
                    }
                    setPx(dst, w, static_cast<int>(x), static_cast<int>(y), bc);
                }
            }
            break;
        }
        case FMezzotint: {
            // 铜版画：随机噪点二值纹理化（细/中/粗 = 噪点密度档位）
            const float density = p.p0 <= 0.5f ? 0.06f : (p.p0 <= 1.5f ? 0.12f : 0.2f);
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    float v[4];
                    getPx(src, w, h, static_cast<int>(x), static_cast<int>(y), v);
                    const float g = luma(v);
                    const float rnd = hash01(x, y, 3u, p.seed);
                    const float out = (rnd < density * (1.0f - g)) ? 0.0f
                                      : (rnd > 1.0f - density * g) ? 1.0f : g;
                    v[0] = v[1] = v[2] = out;
                    setPx(dst, w, static_cast<int>(x), static_cast<int>(y), v);
                }
            }
            break;
        }
        case FFragment: {
            // 碎片：4 副本 ±1px 偏移平均（PS 语义）
            const int offs[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    float acc[4] = {0, 0, 0, 0};
                    for (const auto& o : offs) {
                        float v[4];
                        getPxT(src, w, h, static_cast<int>(x) + o[0], static_cast<int>(y) + o[1], v);
                        for (int c = 0; c < 4; ++c) {
                            acc[c] += v[c];
                        }
                    }
                    for (int c = 0; c < 4; ++c) {
                        acc[c] *= 0.25f;
                    }
                    setPx(dst, w, static_cast<int>(x), static_cast<int>(y), acc);
                }
            }
            break;
        }
        case FColorHalftone: {
            // 彩色半调（简化圆形网点：CMYK 通道各 15°/45°/75°/105° 网点，v1 规则网格——登记放宽）
            const float maxR = std::clamp(p.p0, 4.0f, 127.0f);
            const float cell = maxR * 2.0f;
            const float angles[4] = {15.0f, 45.0f, 75.0f, 105.0f};
            // 先算 K（暗度）用于网点半径：v1 按各通道值
            std::vector<float> chan[4];
            for (int ci = 0; ci < 4; ++ci) {
                chan[ci].assign(n, 0.0f);
            }
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    float v[4];
                    getPx(src, w, h, static_cast<int>(x), static_cast<int>(y), v);
                    // CMYK 近似：K = 1-max(R,G,B)；C/M/Y 归一
                    const float k = 1.0f - std::max(v[0], std::max(v[1], v[2]));
                    chan[0][static_cast<size_t>(y) * w + x] = (1.0f - v[0] - k) > 0 ? (1.0f - v[0] - k) / (1.0f - k) : 0;
                    chan[1][static_cast<size_t>(y) * w + x] = (1.0f - v[1] - k) > 0 ? (1.0f - v[1] - k) / (1.0f - k) : 0;
                    chan[2][static_cast<size_t>(y) * w + x] = (1.0f - v[2] - k) > 0 ? (1.0f - v[2] - k) / (1.0f - k) : 0;
                    chan[3][static_cast<size_t>(y) * w + x] = k;
                }
            }
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    // 各通道在其网点角度网格内取值（旋转采样）
                    float out[4] = {1, 1, 1, 1};
                    out[3] = src[(static_cast<size_t>(y) * w + x) * 4u + 3u];
                    for (int ci = 0; ci < 4; ++ci) {
                        const float a = angles[ci] * kPi / 180.0f;
                        const float ca = std::cos(a);
                        const float sa = std::sin(a);
                        // 反向旋转到网点网格坐标
                        const float rx = (x * ca + y * sa) / cell;
                        const float ry = (-x * sa + y * ca) / cell;
                        // 网点中心 = 格中心；半径 ∝ sqrt(通道值)
                        const float cx = (std::floor(rx) + 0.5f) * cell;
                        const float cy = (std::floor(ry) + 0.5f) * cell;
                        const int gx = static_cast<int>(std::floor(cx * ca - cy * sa));
                        const int gy = static_cast<int>(std::floor(cx * sa + cy * ca));
                        const float v = chan[ci][static_cast<size_t>(std::clamp(gy, 0, static_cast<int>(h) - 1)) * w +
                                                static_cast<size_t>(std::clamp(gx, 0, static_cast<int>(w) - 1))];
                        const float dotR = std::sqrt(std::clamp(v, 0.0f, 1.0f)) * maxR;
                        const float dd = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
                        const float dot = dd <= dotR ? 0.0f : 1.0f;  // 网点=着墨
                        if (ci == 3) {
                            out[0] = out[1] = out[2] = dot;  // K 版灰度底
                        } else if (dot == 0.0f) {
                            float ink[3];
                            if (ci == 0) {
                                ink[0] = 0.0f; ink[1] = 1.0f; ink[2] = 1.0f;  // C
                            } else if (ci == 1) {
                                ink[0] = 1.0f; ink[1] = 0.0f; ink[2] = 1.0f;  // M
                            } else {
                                ink[0] = 1.0f; ink[1] = 1.0f; ink[2] = 0.0f;  // Y
                            }
                            for (int c = 0; c < 3; ++c) {
                                out[c] = std::min(out[c], ink[c]);
                            }
                        }
                    }
                    setPx(dst, w, static_cast<int>(x), static_cast<int>(y), out);
                }
            }
            break;
        }
        case FFindEdges: {
            // Sobel 幅值每通道（photocraft find_edges）：暗边白底
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    uint8_t* d = dst + (static_cast<size_t>(y) * w + x) * 4u;
                    d[3] = src[(static_cast<size_t>(y) * w + x) * 4u + 3u];
                    for (int c = 0; c < 3; ++c) {
                        const auto g = [&](int dx, int dy) -> float {
                            float v[4];
                            getPxT(src, w, h, static_cast<int>(x) + dx, static_cast<int>(y) + dy, v);
                            return v[c];
                        };
                        const float gx = (g(1, -1) + 2.0f * g(1, 0) + g(1, 1)) -
                                         (g(-1, -1) + 2.0f * g(-1, 0) + g(-1, 1));
                        const float gy = (g(-1, 1) + 2.0f * g(0, 1) + g(1, 1)) -
                                         (g(-1, -1) + 2.0f * g(0, -1) + g(1, -1));
                        d[c] = static_cast<uint8_t>(
                            std::clamp(1.0f - std::sqrt(gx * gx + gy * gy) / 4.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                }
            }
            break;
        }
        case FEmboss: {
            // 灰度浮雕（photocraft emboss）：亮角方向亮度差
            const float ang = p.p0 * kPi / 180.0f;
            const float hh = std::max(1.0f, p.p1);
            const int dx = static_cast<int>(std::lround(std::cos(ang) * hh));
            const int dy = static_cast<int>(std::lround(-std::sin(ang) * hh));
            const float k = p.p2 / 100.0f;
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const auto lumAt = [&](int lx, int ly) -> float {
                        float v[4];
                        getPxT(src, w, h, lx, ly, v);
                        return luma(v);
                    };
                    const float g = std::clamp(0.5f + k * (lumAt(x + dx, y + dy) - lumAt(x - dx, y - dy)),
                                               0.0f, 1.0f);
                    uint8_t* d = dst + (static_cast<size_t>(y) * w + x) * 4u;
                    d[0] = d[1] = d[2] = static_cast<uint8_t>(g * 255.0f + 0.5f);
                    d[3] = src[(static_cast<size_t>(y) * w + x) * 4u + 3u];
                }
            }
            break;
        }
        case FWind: {
            // 风：水平拖尾衰减（photocraft wind 语义简化——方向性 streak）
            const int dir = p.p0 > 0.5f ? 1 : -1;
            const int strength = std::max(1, static_cast<int>(p.p1));
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    float acc[3] = {0, 0, 0};
                    float wsum = 0.0f;
                    for (int s = 0; s < strength; ++s) {
                        float v[4];
                        getPxT(src, w, h, static_cast<int>(x) - dir * s, static_cast<int>(y), v);
                        const float wt = 1.0f / static_cast<float>(s + 1);
                        for (int c = 0; c < 3; ++c) {
                            acc[c] += v[c] * wt;
                        }
                        wsum += wt;
                    }
                    uint8_t* d = dst + (static_cast<size_t>(y) * w + x) * 4u;
                    for (int c = 0; c < 3; ++c) {
                        d[c] = static_cast<uint8_t>(
                            std::clamp(acc[c] / wsum, 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                    d[3] = src[(static_cast<size_t>(y) * w + x) * 4u + 3u];
                }
            }
            break;
        }
        case FTiles: {
            // 拼贴：tile 网格随机偏移采样 + 透明缝隙
            const int tw = std::max(8, static_cast<int>(p.p0));
            const int thh = std::max(8, static_cast<int>(p.p1));
            const float offPct = std::clamp(p.p2, 0.0f, 99.0f) / 100.0f;
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    float out[4] = {0, 0, 0, 0};
                    const int ty = static_cast<int>(y) / thh;
                    const int tx = static_cast<int>(x) / tw;
                    const float ox = (hash01(tx, ty, 21u, p.seed) - 0.5f) * offPct * tw;
                    const float oy = (hash01(tx, ty, 23u, p.seed) - 0.5f) * offPct * thh;
                    const int lx = static_cast<int>(x) % tw;
                    const int ly = static_cast<int>(y) % thh;
                    const bool inTile = lx >= std::abs(static_cast<int>(std::lround(ox))) &&
                                        ly >= std::abs(static_cast<int>(std::lround(oy)));
                    if (inTile) {
                        getPxT(src, w, h, static_cast<int>(x) + static_cast<int>(std::lround(ox)),
                               static_cast<int>(y) + static_cast<int>(std::lround(oy)), out);
                    }
                    setPx(dst, w, static_cast<int>(x), static_cast<int>(y), out);
                }
            }
            break;
        }
        case FExposure: {
            // 曝光过度：图像与自身反相正片叠底（PS 语义 out = orig ⊗ (1−orig)），alpha 保留
            for (size_t i = 0; i < n; ++i) {
                for (int c = 0; c < 3; ++c) {
                    const float v = src[i * 4u + c] / 255.0f;
                    dst[i * 4u + c] = static_cast<uint8_t>(
                        std::clamp(v * (1.0f - v) * 4.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                dst[i * 4u + 3u] = src[i * 4u + 3u];
            }
            break;
        }
        case FLensCorrection: {
            // M20d 镜头校正（lens_ops correct 全管线：畸变/透视/角度缩放/暗角；
            // strip 域 = 层 bbox，边缘延展避免域外透明吃边）
            lens::Correction lc;
            lc.distortion = p.p0;
            lc.vertical = p.p1;
            lc.horizontal = p.p2;
            lc.angle = p.p3;
            lc.scale = p.p4;
            lc.vignetteAmount = p.p5;
            lc.edge = lens::EdgeMode::Extension;
            lens::Img in;
            in.w = static_cast<int>(w);
            in.h = static_cast<int>(h);
            in.px.resize(static_cast<size_t>(n) * 4u);
            for (size_t i = 0; i < n; ++i) {
                for (int c = 0; c < 4; ++c) {
                    in.px[i * 4u + c] = src[i * 4u + c] / 255.0f;
                }
            }
            const lens::Img out = lens::correct(in, inpaint::RectI{0, 0, static_cast<int>(w), static_cast<int>(h)}, lc);
            for (size_t i = 0; i < n; ++i) {
                for (int c = 0; c < 4; ++c) {
                    dst[i * 4u + c] = static_cast<uint8_t>(
                        std::clamp(out.px[i * 4u + c], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
            break;
        }
        case FClouds:
        case FDifferenceClouds: {
            // 云彩：fg/bg 分形噪声插值（PS 语义，八 octave）；分层云彩 = |orig − clouds|
            const float scale = 1.0f / 64.0f;
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const float t = fbmNoise(x * scale, y * scale, p.seed, 8);
                    uint8_t* d = dst + (static_cast<size_t>(y) * w + x) * 4u;
                    for (int c = 0; c < 3; ++c) {
                        const float cloud = std::clamp(p.colorFg[c] * t + p.colorBg[c] * (1.0f - t), 0.0f, 1.0f);
                        if (p.kind == FClouds) {
                            d[c] = static_cast<uint8_t>(cloud * 255.0f + 0.5f);
                        } else {
                            const float o = src[(static_cast<size_t>(y) * w + x) * 4u + c] / 255.0f;
                            d[c] = static_cast<uint8_t>(std::abs(o - cloud) * 255.0f + 0.5f);
                        }
                    }
                    d[3] = p.kind == FClouds ? 255 : src[(static_cast<size_t>(y) * w + x) * 4u + 3u];
                }
            }
            break;
        }
        case FLensFlare: {
            // 镜头光晕：中心亮核 + 光环 + ghost 串（屏幕混合）
            const float cx = p.p0;
            const float cy = p.p1;
            const float bright = std::clamp(p.p2, 10.0f, 300.0f) / 100.0f;
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const float dx = static_cast<float>(x) - cx;
                    const float dy = static_cast<float>(y) - cy;
                    const float dist = std::sqrt(dx * dx + dy * dy);
                    const float core = bright * std::exp(-dist * dist / (2.0f * 40.0f * 40.0f));
                    const float ring = bright * 0.4f * std::exp(-std::abs(dist - 90.0f) / 6.0f);
                    const float flare = std::min(core + ring, 1.0f);
                    uint8_t* d = dst + (static_cast<size_t>(y) * w + x) * 4u;
                    for (int c = 0; c < 3; ++c) {
                        const float warm = c == 0 ? 1.0f : (c == 1 ? 0.95f : 0.8f);
                        const float o = src[(static_cast<size_t>(y) * w + x) * 4u + c] / 255.0f;
                        const float s = o + (1.0f - o) * flare * warm;  // screen
                        d[c] = static_cast<uint8_t>(std::clamp(s, 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                    d[3] = src[(static_cast<size_t>(y) * w + x) * 4u + 3u];
                }
            }
            break;
        }
        case FMinimum:
        case FMaximum: {
            // Min/Max：方形窗口或圆盘（van Herk 行窗；v1 简化朴素窗口——登记性能）
            const int r = static_cast<int>(std::max(0.0f, p.p0));
            const bool disc = p.p1 > 0.5f;
            const bool isMax = p.kind == FMaximum;
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    uint8_t* d = dst + (static_cast<size_t>(y) * w + x) * 4u;
                    d[3] = src[(static_cast<size_t>(y) * w + x) * 4u + 3u];
                    for (int c = 0; c < 3; ++c) {
                        float best = isMax ? 0.0f : 1.0f;
                        for (int dy = -r; dy <= r; ++dy) {
                            const int hw = disc ? static_cast<int>(
                                std::floor(std::sqrt(std::max(0.0f, static_cast<float>(r * r - dy * dy))) + 1e-4f))
                                               : r;
                            for (int dx = -hw; dx <= hw; ++dx) {
                                float v[4];
                                getPxT(src, w, h, static_cast<int>(x) + dx, static_cast<int>(y) + dy, v);
                                best = isMax ? std::max(best, v[c]) : std::min(best, v[c]);
                            }
                        }
                        d[c] = static_cast<uint8_t>(best * 255.0f + 0.5f);
                    }
                }
            }
            break;
        }
        case FOffset: {
            // 位移：setUndefinedAreas=v1 透明填充（PS 未定义区域三态——v1 透明，登记）
            const int dxi = static_cast<int>(p.p0);
            const int dyi = static_cast<int>(p.p1);
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    float v[4];
                    getPxT(src, w, h, static_cast<int>(x) - dxi, static_cast<int>(y) - dyi, v);
                    setPx(dst, w, static_cast<int>(x), static_cast<int>(y), v);
                }
            }
            break;
        }
        case FCustom: {
            // 自定 5×5 卷积（预乘域；scale/offset 归一）
            std::vector<float> pm;
            toPremul(src, w, h, pm);
            std::vector<float> out(pm.size(), 0.0f);
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    float acc[4] = {0, 0, 0, 0};
                    for (int ky = -2; ky <= 2; ++ky) {
                        for (int kx = -2; kx <= 2; ++kx) {
                            const int xx = std::clamp(static_cast<int>(x) + kx, 0, static_cast<int>(w) - 1);
                            const int yy = std::clamp(static_cast<int>(y) + ky, 0, static_cast<int>(h) - 1);
                            const float kv = p.kernel[static_cast<size_t>(ky + 2) * 5u + (kx + 2)];
                            if (kv == 0.0f) {
                                continue;
                            }
                            const float* s = &pm[(static_cast<size_t>(yy) * w + xx) * 4u];
                            for (int c = 0; c < 4; ++c) {
                                acc[c] += s[c] * kv;
                            }
                        }
                    }
                    float* d = &out[(static_cast<size_t>(y) * w + x) * 4u];
                    for (int c = 0; c < 4; ++c) {
                        d[c] = acc[c] * p.scale + p.offset;
                    }
                }
            }
            for (size_t i = 0; i < n; ++i) {
                const float a = std::clamp(out[i * 4u + 3u], 0.0f, 1.0f);
                const float inv = a > 1e-6f ? 1.0f / a : 0.0f;
                for (int c = 0; c < 3; ++c) {
                    dst[i * 4u + c] = static_cast<uint8_t>(
                        std::clamp(out[i * 4u + c] * inv, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                dst[i * 4u + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
            }
            break;
        }
        case FCameraRaw: {
            // M23c：Camera Raw 9 级链（cameraJson → CameraRaw；域 = 全帧直 alpha）。
            CameraRaw cam;
            parseCameraJson(p.cameraJson, cam);
            if (cam.isIdentity()) {
                if (dst != src) {
                    std::memcpy(dst, src, n * 4u);
                }
                break;
            }
            std::vector<float> fpx(n * 4u);
            for (size_t i = 0; i < n; ++i) {
                fpx[i * 4u] = src[i * 4u] / 255.0f;
                fpx[i * 4u + 1u] = src[i * 4u + 1u] / 255.0f;
                fpx[i * 4u + 2u] = src[i * 4u + 2u] / 255.0f;
                fpx[i * 4u + 3u] = src[i * 4u + 3u] / 255.0f;
            }
            cameraRawDevelop(fpx.data(), w, h, cam, false);
            for (size_t i = 0; i < n; ++i) {
                for (int c = 0; c < 4; ++c) {
                    dst[i * 4u + c] = static_cast<uint8_t>(
                        std::clamp(fpx[i * 4u + c], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
            break;
        }
        case FColorLookup: {
            // M22：颜色查找（图像 › 调整 › 颜色查找）。表来源：内置外观（lutfile
            // builtinLook，33³ 程序生成 CC0）或进程级导入表（lutImportSlot ≥100）。
            std::optional<cms::LutFile> lut;
            if (p.p0 >= 100.0f) {
                lut = cms::importedLookup(static_cast<unsigned>(p.p0) - 100u);
                if (!lut) {
                    err = "lookup table not loaded";
                    return false;
                }
            } else {
                size_t count = 0;
                const auto* table = cms::builtinLookTable(count);
                unsigned sel = static_cast<unsigned>(p.p0);
                if (sel >= count) {
                    err = "lookup preset out of range";
                    return false;
                }
                lut = cms::builtinLook(table[sel].first);
                if (!lut) {
                    err = "builtin look unavailable";
                    return false;
                }
            }
            const size_t sz = lut->size;
            const float m = static_cast<float>(sz - 1);
            auto sample = [&](float c, int ch) {
                float t = std::clamp(c, 0.0f, 1.0f);
                if (!lut->domainIsDefault()) {
                    t = std::clamp((t - lut->domainMin[ch]) /
                                       std::max(1e-6f, lut->domainMax[ch] - lut->domainMin[ch]),
                                   0.0f, 1.0f);
                }
                return t;
            };
            for (size_t i = 0; i < n; ++i) {
                const float r = sample(src[i * 4u] / 255.0f, 0);
                const float g = sample(src[i * 4u + 1u] / 255.0f, 1);
                const float b = sample(src[i * 4u + 2u] / 255.0f, 2);
                // 红最快索引 = ((b·size + g)·size + r)·3；三线性（PS 颜色查找默认）。
                const float fx = r * m;
                const float fy = g * m;
                const float fz = b * m;
                const size_t x0 = std::min(static_cast<size_t>(fx), sz - 2);
                const size_t y0 = std::min(static_cast<size_t>(fy), sz - 2);
                const size_t z0 = std::min(static_cast<size_t>(fz), sz - 2);
                const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
                const float ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
                const float tz = std::clamp(fz - static_cast<float>(z0), 0.0f, 1.0f);
                float out[3] = {0.0f, 0.0f, 0.0f};
                for (int corner = 0; corner < 8; ++corner) {
                    const size_t cx = x0 + (corner & 1u);
                    const size_t cy = y0 + ((corner >> 1) & 1u);
                    const size_t cz = z0 + ((corner >> 2) & 1u);
                    const float wx = (corner & 1u) ? tx : 1.0f - tx;
                    const float wy = ((corner >> 1) & 1u) ? ty : 1.0f - ty;
                    const float wz = ((corner >> 2) & 1u) ? tz : 1.0f - tz;
                    const float w = wx * wy * wz;
                    const size_t idx = ((cz * sz + cy) * sz + cx) * 3;
                    for (int ch = 0; ch < 3; ++ch) {
                        out[ch] += w * lut->data[idx + ch];
                    }
                }
                for (int ch = 0; ch < 3; ++ch) {
                    dst[i * 4u + ch] = static_cast<uint8_t>(
                        std::clamp(out[ch], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                dst[i * 4u + 3] = src[i * 4u + 3];
            }
            break;
        }
        default:
            err = "unknown filter kind: " + std::to_string(p.kind);
            return false;
    }
    return true;
}

}  // namespace filters
}  // namespace montage
