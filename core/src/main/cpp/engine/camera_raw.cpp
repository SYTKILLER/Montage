// M23b：Camera Raw 显影管线实现。翻译自 photocraft crates/algo/src/camera_raw.rs
// （MIT OR Apache-2.0；导向滤波/去雾暗通道/单调三次插值等论文出处见头文件注释）。
// par_rows/par_map 单线程化（photocraft 测试已证线程数无关）。

#include "engine/camera_raw.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace montage {
namespace {

// ---- 轻量 JSON 数值/串提取（CameraRaw 字段 camelCase；project_open 手写解析器同思路）----

struct JsonScan {
    const char* p;
    const char* end;

    // 找顶层（花括号深度 1）键 key 的值起点；未找到返回 nullptr。
    const char* find(const char* key) const {
        size_t klen = std::strlen(key);
        int depth = 0;
        bool inStr = false;
        for (const char* q = p; q < end; ++q) {
            char c = *q;
            if (inStr) {
                if (c == '\\') {
                    ++q;
                    continue;
                }
                if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') {
                inStr = true;
                // 可能是键名：检查 "key" 后跟 :
                if (depth == 1) {
                    const char* s = q + 1;
                    const char* e = s;
                    while (e < end && *e != '"' && *e != '\\') ++e;
                    if (e < end && *e == '"' && static_cast<size_t>(e - s) == klen && std::memcmp(s, key, klen) == 0) {
                        const char* r = e + 1;
                        while (r < end && *r == ' ') ++r;
                        if (r < end && *r == ':') return r + 1;
                    }
                }
                continue;
            }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') --depth;
        }
        return nullptr;
    }

    // 键存在（值任意）即真。
    bool has(const char* key) const { return find(key) != nullptr; }

    // 数值（标量）。
    bool num(const char* key, float& out) const {
        const char* v = find(key);
        if (!v) return false;
        char* e = nullptr;
        out = std::strtof(v, &e);
        return e != v;
    }

    // 串（vignetteStyle 等；最多 31 字节）。
    bool str(const char* key, char* out, size_t cap) const {
        const char* v = find(key);
        if (!v) return false;
        while (v < end && *v == ' ') ++v;
        if (v >= end || *v != '"') return false;
        ++v;
        size_t n = 0;
        while (v < end && *v != '"' && n + 1 < cap) out[n++] = *v++;
        out[n] = 0;
        return true;
    }

    // 数组（数值或 [x,y] 点对的扁平表）。
    std::vector<float> arr(const char* key) const {
        std::vector<float> out;
        const char* v = find(key);
        if (!v) return out;
        while (v < end && *v == ' ') ++v;
        if (v >= end || *v != '[') return out;
        ++v;
        while (v < end) {
            while (v < end && (*v == ' ' || *v == ',' || *v == '[' || *v == ']')) ++v;
            if (v >= end || *v == '}' || *v == ']') break;
            char* e = nullptr;
            float f = std::strtof(v, &e);
            if (e == v) break;
            out.push_back(f);
            v = e;
        }
        return out;
    }
};

}  // namespace

// CameraRaw JSON（camelCase）→ CameraRaw。未识别字段忽略（向前兼容）。
bool parseCameraJson(const std::string& json, CameraRaw& out) {
    if (json.empty()) return true;
    JsonScan js{json.c_str(), json.c_str() + json.size()};
    auto f = [&](const char* k, float& dst) { float v; if (js.num(k, v)) dst = v; };
    f("temperature", out.temperature);
    f("tint", out.tint);
    f("exposure", out.exposure);
    f("contrast", out.contrast);
    f("highlights", out.highlights);
    f("shadows", out.shadows);
    f("whites", out.whites);
    f("blacks", out.blacks);
    f("texture", out.texture);
    f("clarity", out.clarity);
    f("dehaze", out.dehaze);
    f("vibrance", out.vibrance);
    f("saturation", out.saturation);
    f("curveHighlights", out.curveHighlights);
    f("curveLights", out.curveLights);
    f("curveDarks", out.curveDarks);
    f("curveShadows", out.curveShadows);
    {
        auto v = js.arr("curveSplits");
        for (size_t i = 0; i < v.size() && i < 3; ++i) out.curveSplits[i] = v[i];
    }
    auto readCurve = [&](const char* k, std::vector<std::array<float, 2>>& dst) {
        auto v = js.arr(k);
        if (v.empty()) return;
        dst.clear();
        for (size_t i = 0; i + 1 < v.size(); i += 2) dst.push_back({v[i], v[i + 1]});
    };
    readCurve("pointCurve", out.pointCurve);
    readCurve("pointCurveRed", out.pointCurveRed);
    readCurve("pointCurveGreen", out.pointCurveGreen);
    readCurve("pointCurveBlue", out.pointCurveBlue);
    auto read8 = [&](const char* k, float* dst) {
        auto v = js.arr(k);
        for (size_t i = 0; i < v.size() && i < 8; ++i) dst[i] = v[i];
    };
    read8("hslHue", out.hslHue);
    read8("hslSat", out.hslSat);
    read8("hslLum", out.hslLum);
    auto wheel = [&](const char* base, CameraWheel& w) {
        std::string hKey = std::string(base) + "Hue";
        std::string sKey = std::string(base) + "Sat";
        std::string lKey = std::string(base) + "Lum";
        float v;
        if (js.num(hKey.c_str(), v)) w.hue = v;
        if (js.num(sKey.c_str(), v)) w.sat = v;
        if (js.num(lKey.c_str(), v)) w.lum = v;
    };
    wheel("gradeShadows", out.gradeShadows);
    wheel("gradeMidtones", out.gradeMidtones);
    wheel("gradeHighlights", out.gradeHighlights);
    wheel("gradeGlobal", out.gradeGlobal);
    f("gradeBlending", out.gradeBlending);
    f("gradeBalance", out.gradeBalance);
    f("sharpenAmount", out.sharpenAmount);
    f("sharpenRadius", out.sharpenRadius);
    f("sharpenDetail", out.sharpenDetail);
    f("sharpenMasking", out.sharpenMasking);
    f("noiseLuminance", out.noiseLuminance);
    f("noiseLuminanceDetail", out.noiseLuminanceDetail);
    f("noiseColor", out.noiseColor);
    f("noiseColorDetail", out.noiseColorDetail);
    f("grainAmount", out.grainAmount);
    f("grainSize", out.grainSize);
    f("grainRoughness", out.grainRoughness);
    f("vignetteAmount", out.vignetteAmount);
    f("vignetteMidpoint", out.vignetteMidpoint);
    f("vignetteRoundness", out.vignetteRoundness);
    f("vignetteFeather", out.vignetteFeather);
    f("vignetteHighlights", out.vignetteHighlights);
    {
        char style[32] = {0};
        if (js.str("vignetteStyle", style, sizeof style)) out.vignetteStyle = style;
    }
    f("seed", *reinterpret_cast<float*>(&out.seed));  // 数值通道；seed 小值即够
    // seed 是整数语义，直接 strtof 再转；重读为 uint32：
    {
        const char* v = js.find("seed");
        if (v) {
            char* e = nullptr;
            long lv = std::strtol(v, &e, 10);
            if (e != v && lv >= 0) out.seed = static_cast<uint32_t>(lv);
        }
    }
    f("pixelScale", out.pixelScale);
    // 点曲线校验（新输入拒绝畸形；空 = 线性）。
    for (auto* curve : {&out.pointCurve, &out.pointCurveRed, &out.pointCurveGreen, &out.pointCurveBlue}) {
        if (!validateCurve(*curve).empty()) {
            curve->clear();  // 容错：畸形曲线回退线性，不拒整个显影（滤镜路径 validate 先行）
        }
    }
    return true;
}


namespace {

constexpr float kTau = 6.2831855f;

inline float luma3(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

inline float smoothstep(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// sRGB 传递函数：解码（0 下线性延拓、1 上按幂延拓）与编码。
inline float srgbToLinear(float v) {
    if (v <= 0.04045f) return v / 12.92f;
    return std::pow((v + 0.055f) / 1.055f, 2.4f);
}
inline float linearToSrgb(float v) {
    if (v <= 0.0031308f) return v * 12.92f;
    return 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// 整数哈希 → 0..1（位置键控噪声，与平铺无关）。photocraft photo_util::hash01。
inline float hash01(int64_t x, int64_t y, uint64_t seed) {
    uint64_t z = static_cast<uint64_t>(x) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(y) * 0xC2B2AE3D27D4EB4Full ^
                 seed * 0x165667B19E3779F9ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<float>(z >> 40) / static_cast<float>(1ull << 24);
}

// 双线性采样 w×h×ch 缓冲的通道 c，边缘钳制。photocraft photo_util::bilinear。
inline float bilinearAt(const std::vector<float>& buf, size_t w, size_t h, size_t ch, size_t c, float x, float y) {
    x = std::clamp(x, 0.0f, static_cast<float>(w - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(h - 1));
    size_t x0 = static_cast<size_t>(std::floor(x));
    size_t y0 = static_cast<size_t>(std::floor(y));
    size_t x1 = std::min(x0 + 1, w - 1);
    size_t y1 = std::min(y0 + 1, h - 1);
    float tx = x - static_cast<float>(x0);
    float ty = y - static_cast<float>(y0);
    auto p = [&](size_t xx, size_t yy) { return buf[(yy * w + xx) * ch + c]; };
    float top = p(x0, y0) + (p(x1, y0) - p(x0, y0)) * tx;
    float bot = p(x0, y1) + (p(x1, y1) - p(x0, y1)) * tx;
    return top + (bot - top) * ty;
}

// Kovesi 2010 奇箱宽三重卷积近似 σ 高斯，返回半径；σ < ~0.577 退化时给一次半径 1
// 真实模糊（photocraft fxutil::gauss_box_radii，issue #706 修复）。
inline std::array<size_t, 3> gaussBoxRadii(float sigma) {
    const float n = 3.0f;
    float wIdeal = std::sqrt(12.0f * sigma * sigma / n + 1.0f);
    int wl = static_cast<int>(std::floor(wIdeal));
    if (wl % 2 == 0) --wl;
    wl = std::max(wl, 1);
    int wu = wl + 2;
    float mF = std::round((12.0f * sigma * sigma - n * (wl * wl) - 4.0f * n * wl - 3.0f * n) / (-4.0f * wl - 4.0f));
    size_t m = static_cast<size_t>(std::clamp(mF, 0.0f, n));
    std::array<size_t, 3> radii{};
    bool degenerate = m == 0 && wl <= 1;  // 全 0 半径：给最小真实模糊
    for (int i = 0; i < 3; ++i) {
        int w = i < static_cast<int>(m) ? wl : wu;
        radii[i] = static_cast<size_t>((w - 1) / 2);
        if (degenerate) radii[i] = 1;
    }
    return radii;
}

// 单通道缓冲的箱均值（半径 r，边缘钳制）：行滑动和，再列分带滑动和。
std::vector<float> boxMean(const std::vector<float>& v, size_t w, size_t h, size_t r) {
    if (r == 0 || w == 0 || h == 0) return v;
    float n = static_cast<float>(2 * r + 1);
    std::vector<float> tmp(w * h, 0.0f);
    auto at = [&](const float* src, int64_t k) { return src[std::clamp<int64_t>(k, 0, static_cast<int64_t>(w) - 1)]; };
    for (size_t y = 0; y < h; ++y) {
        const float* src = v.data() + y * w;
        float* row = tmp.data() + y * w;
        double acc = 0;
        for (int64_t k = -static_cast<int64_t>(r); k <= static_cast<int64_t>(r); ++k) acc += at(src, k);
        for (size_t x = 0; x < w; ++x) {
            row[x] = static_cast<float>(acc / n);
            acc += at(src, static_cast<int64_t>(x) + r + 1) - at(src, static_cast<int64_t>(x) - static_cast<int64_t>(r));
        }
    }
    // 列向：分带（256 列一带）滑动列和。
    constexpr size_t kB = 256;
    std::vector<float> out(w * h, 0.0f);
    for (size_t bx = 0; bx < w; bx += kB) {
        size_t x0 = bx;
        size_t x1 = std::min(bx + kB, w);
        size_t bw = x1 - x0;
        auto rowv = [&](int64_t y) -> const float* {
            int64_t yy = std::clamp<int64_t>(y, 0, static_cast<int64_t>(h) - 1);
            return tmp.data() + static_cast<size_t>(yy) * w + x0;
        };
        std::vector<double> acc(bw, 0.0);
        for (int64_t y = -static_cast<int64_t>(r); y <= static_cast<int64_t>(r); ++y) {
            const float* row = rowv(y);
            for (size_t k = 0; k < bw; ++k) acc[k] += row[k];
        }
        std::vector<float> band(bw * h, 0.0f);
        for (size_t y = 0; y < h; ++y) {
            for (size_t k = 0; k < bw; ++k) band[y * bw + k] = static_cast<float>(acc[k] / n);
            const float* add = rowv(static_cast<int64_t>(y) + r + 1);
            const float* sub = rowv(static_cast<int64_t>(y) - static_cast<int64_t>(r));
            for (size_t k = 0; k < bw; ++k) acc[k] += add[k] - sub[k];
        }
        for (size_t y = 0; y < h; ++y)
            std::memcpy(out.data() + y * w + x0, band.data() + y * bw, bw * sizeof(float));
    }
    return out;
}

// 近似高斯（三箱 pass）。
std::vector<float> gaussF(const std::vector<float>& v, size_t w, size_t h, float sigma) {
    if (sigma < 0.3f) return v;
    std::vector<float> out = v;
    for (size_t r : gaussBoxRadii(sigma)) {
        out = boxMean(out, w, h, r);
    }
    return out;
}

// 导向滤波（He et al. 2010）：p 由 i 引导。
std::vector<float> guided(const std::vector<float>& i, const std::vector<float>& p, size_t w, size_t h, size_t r, float eps) {
    size_t n = w * h;
    std::vector<float> mi = boxMean(i, w, h, r);
    std::vector<float> mp = boxMean(p, w, h, r);
    std::vector<float> ip(n), ii(n);
    for (size_t k = 0; k < n; ++k) {
        ip[k] = i[k] * p[k];
        ii[k] = i[k] * i[k];
    }
    std::vector<float> mip = boxMean(ip, w, h, r);
    std::vector<float> mii = boxMean(ii, w, h, r);
    std::vector<float> a(n), b(n);
    for (size_t k = 0; k < n; ++k) {
        a[k] = (mip[k] - mi[k] * mp[k]) / (mii[k] - mi[k] * mi[k] + eps);
        b[k] = mp[k] - a[k] * mi[k];
    }
    std::vector<float> ma = boxMean(a, w, h, r);
    std::vector<float> mb = boxMean(b, w, h, r);
    std::vector<float> out(n);
    for (size_t k = 0; k < n; ++k) out[k] = ma[k] * i[k] + mb[k];
    return out;
}

// 快速导向滤波（He & Sun 2015）：系数在 s× 降采样副本上算，双线性上采样——
// 大半径色调控制视觉等价、成本零头。
std::vector<float> guidedFast(const std::vector<float>& i, const std::vector<float>& p, size_t w, size_t h, size_t r, float eps) {
    size_t s = std::clamp<size_t>(r / 8, 1, 8);
    if (s == 1) return guided(i, p, w, h, r, eps);
    size_t sw = (w + s - 1) / s;
    size_t sh = (h + s - 1) / s;
    auto down = [&](const std::vector<float>& v) {
        std::vector<float> o(sw * sh, 0.0f);
        std::vector<float> c(sw * sh, 0.0f);
        for (size_t y = 0; y < h; ++y)
            for (size_t x = 0; x < w; ++x) {
                size_t k = (y / s) * sw + x / s;
                o[k] += v[y * w + x];
                c[k] += 1.0f;
            }
        for (size_t k = 0; k < sw * sh; ++k) o[k] /= std::max(c[k], 1.0f);
        return o;
    };
    std::vector<float> si = down(i);
    std::vector<float> sp = down(p);
    size_t rs = std::max<size_t>(r / s, 1);
    std::vector<float> mi = boxMean(si, sw, sh, rs);
    std::vector<float> mp = boxMean(sp, sw, sh, rs);
    std::vector<float> ip(sw * sh), ii(sw * sh);
    for (size_t k = 0; k < sw * sh; ++k) {
        ip[k] = si[k] * sp[k];
        ii[k] = si[k] * si[k];
    }
    std::vector<float> mip = boxMean(ip, sw, sh, rs);
    std::vector<float> mii = boxMean(ii, sw, sh, rs);
    std::vector<float> a(sw * sh), b(sw * sh);
    for (size_t k = 0; k < sw * sh; ++k) {
        a[k] = (mip[k] - mi[k] * mp[k]) / (mii[k] - mi[k] * mi[k] + eps);
        b[k] = mp[k] - a[k] * mi[k];
    }
    std::vector<float> ma = boxMean(a, sw, sh, rs);
    std::vector<float> mb = boxMean(b, sw, sh, rs);
    std::vector<float> out(w * h, 0.0f);
    float sf = static_cast<float>(s);
    for (size_t y = 0; y < h; ++y) {
        float fy = (static_cast<float>(y) + 0.5f) / sf - 0.5f;
        for (size_t x = 0; x < w; ++x) {
            float fx = (static_cast<float>(x) + 0.5f) / sf - 0.5f;
            float av = bilinearAt(ma, sw, sh, 1, 0, fx, fy);
            float bv = bilinearAt(mb, sw, sh, 1, 0, fx, fy);
            out[y * w + x] = av * i[y * w + x] + bv;
        }
    }
    return out;
}

inline float lutEval(const std::vector<float>& lut, float v) {
    if (!(v >= 0.0f && v <= 1.0f)) {
        // 浮点文档：界外线性延拓。
        return v < 0.0f ? v + lut[0] : lut[lut.size() - 1] + (v - 1.0f);
    }
    float x = v * static_cast<float>(lut.size() - 1);
    size_t i = std::min(static_cast<size_t>(x), lut.size() - 2);
    float t = x - static_cast<float>(i);
    return lut[i] + (lut[i + 1] - lut[i]) * t;
}

// 参数化色调曲线 LUT（区域按 splits 分割，每滑杆 ±100 平滑移动其区域至多四分之一域）。
std::vector<float> parametricLut(const CameraRaw& p, size_t n) {
    float s[3];
    for (int k = 0; k < 3; ++k) s[k] = std::clamp(p.curveSplits[k], 5.0f, 95.0f) / 100.0f;
    float centres[4] = {s[0] / 2.0f, (s[0] + s[1]) / 2.0f, (s[1] + s[2]) / 2.0f, (s[2] + 1.0f) / 2.0f};
    float amounts[4] = {p.curveShadows, p.curveDarks, p.curveLights, p.curveHighlights};
    float widths[4] = {std::max(s[0], 0.1f), std::max(s[1] - s[0], 0.1f), std::max(s[2] - s[1], 0.1f), std::max(1.0f - s[2], 0.1f)};
    std::vector<float> lut(n);
    for (size_t i = 0; i < n; ++i) {
        float x = static_cast<float>(i) / static_cast<float>(n - 1);
        float y = x;
        for (int k = 0; k < 4; ++k) {
            float d = (x - centres[k]) / (widths[k] * 1.2f);
            float bump = std::exp(-d * d * 2.0f) * x * (1.0f - x) * 4.0f;
            y += amounts[k] / 100.0f * 0.25f * bump;
        }
        lut[i] = std::clamp(y, 0.0f, 1.0f);
    }
    // 保单调。
    for (size_t i = 1; i < n; ++i) lut[i] = std::max(lut[i], lut[i - 1]);
    return lut;
}

std::array<float, 3> rgbToHsv(const std::array<float, 3>& c) {
    float mx = std::max({c[0], c[1], c[2]});
    float mn = std::min({c[0], c[1], c[2]});
    float d = mx - mn;
    float h = 0.0f;
    if (d <= 1e-9f) {
        h = 0.0f;
    } else if (mx == c[0]) {
        float v = (c[1] - c[2]) / d;
        h = 60.0f * (v - std::floor(v / 6.0f) * 6.0f);
    } else if (mx == c[1]) {
        h = 60.0f * ((c[2] - c[0]) / d + 2.0f);
    } else {
        h = 60.0f * ((c[0] - c[1]) / d + 4.0f);
    }
    return {h, mx > 1e-9f ? d / mx : 0.0f, mx};
}

std::array<float, 3> hsvToRgb(float h, float s, float v) {
    float hh = h - std::floor(h / 360.0f) * 360.0f;
    hh /= 60.0f;
    float c = v * s;
    float x = c * (1.0f - std::abs(std::fmod(hh, 2.0f) - 1.0f));
    float r, g, b;
    switch (static_cast<int>(hh)) {
        case 0: r = c; g = x; b = 0.0f; break;
        case 1: r = x; g = c; b = 0.0f; break;
        case 2: r = 0.0f; g = c; b = x; break;
        case 3: r = 0.0f; g = x; b = c; break;
        case 4: r = x; g = 0.0f; b = c; break;
        default: r = c; g = 0.0f; b = x; break;
    }
    float m = v - c;
    return {r + m, g + m, b + m};
}

// 值噪声（双线性平滑格点）。
inline float valueNoise(float x, float y, uint64_t seed) {
    float xi = std::floor(x);
    float yi = std::floor(y);
    float tx = x - xi;
    float ty = y - yi;
    float sx = tx * tx * (3.0f - 2.0f * tx);
    float sy = ty * ty * (3.0f - 2.0f * ty);
    auto h = [&](int64_t dx, int64_t dy) { return hash01(static_cast<int64_t>(xi) + dx, static_cast<int64_t>(yi) + dy, seed); };
    float top = h(0, 0) + (h(1, 0) - h(0, 0)) * sx;
    float bot = h(0, 1) + (h(1, 1) - h(0, 1)) * sx;
    return top + (bot - top) * sy;
}

// 去雾（He et al. CVPR 2009）：透射率在 ≤768px 代理上算，双线性上采样。
void dehaze(float* px, size_t w, size_t h, float amount) {
    size_t k = (std::max(w, h) + 767) / 768;
    if (k < 1) k = 1;
    size_t sw = (w + k - 1) / k;
    size_t sh = (h + k - 1) / k;
    std::vector<std::array<float, 3>> small(sw * sh, {0.0f, 0.0f, 0.0f});
    std::vector<float> cnt(sw * sh, 0.0f);
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x) {
            size_t i = (y / k) * sw + x / k;
            for (int c = 0; c < 3; ++c) small[i][c] += std::clamp(px[(y * w + x) * 4 + c], 0.0f, 1.0f);
            cnt[i] += 1.0f;
        }
    for (size_t i = 0; i < sw * sh; ++i)
        for (int c = 0; c < 3; ++c) small[i][c] /= std::max(cnt[i], 1.0f);
    size_t r = std::max<size_t>(std::max(sw, sh) / 60, 2);
    auto minFilter = [&](const std::vector<float>& v) {
        std::vector<float> a(sw * sh, 0.0f);
        for (size_t y = 0; y < sh; ++y)
            for (size_t x = 0; x < sw; ++x) {
                size_t x0 = x > r ? x - r : 0;
                size_t x1 = std::min(x + r, sw - 1);
                float m = v[y * sw + x0];
                for (size_t xx = x0; xx <= x1; ++xx) m = std::min(m, v[y * sw + xx]);
                a[y * sw + x] = m;
            }
        std::vector<float> b(sw * sh, 0.0f);
        for (size_t y = 0; y < sh; ++y)
            for (size_t x = 0; x < sw; ++x) {
                size_t y0 = y > r ? y - r : 0;
                size_t y1 = std::min(y + r, sh - 1);
                float m = a[y0 * sw + x];
                for (size_t yy = y0; yy <= y1; ++yy) m = std::min(m, a[yy * sw + x]);
                b[y * sw + x] = m;
            }
        return b;
    };
    std::vector<float> minc(sw * sh);
    for (size_t i = 0; i < sw * sh; ++i) minc[i] = std::min({small[i][0], small[i][1], small[i][2]});
    std::vector<float> dark = minFilter(minc);
    // 大气光：暗通道最亮 0.1% 的均值颜色。
    std::vector<size_t> idx(sw * sh);
    for (size_t i = 0; i < sw * sh; ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return dark[b] < dark[a]; });
    size_t top = std::max<size_t>(sw * sh / 1000, 1);
    std::array<float, 3> air = {0.0f, 0.0f, 0.0f};
    for (size_t k2 = 0; k2 < top; ++k2)
        for (int c = 0; c < 3; ++c) air[c] += small[idx[k2]][c] / static_cast<float>(top);
    for (int c = 0; c < 3; ++c) air[c] = std::max(air[c], 0.2f);
    std::vector<float> normDark(sw * sh);
    for (size_t i = 0; i < sw * sh; ++i)
        normDark[i] = std::min({small[i][0] / air[0], small[i][1] / air[1], small[i][2] / air[2]});
    std::vector<float> normDarkIn = minFilter(normDark);
    std::vector<float> tRaw(sw * sh);
    for (size_t i = 0; i < sw * sh; ++i) tRaw[i] = 1.0f - 0.95f * normDarkIn[i];
    std::vector<float> guide(sw * sh);
    for (size_t i = 0; i < sw * sh; ++i) guide[i] = luma3(small[i][0], small[i][1], small[i][2]);
    std::vector<float> t = guided(guide, tRaw, sw, sh, r * 2, 1e-3f);
    float ks = static_cast<float>(k);
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x) {
            float tv = std::clamp(bilinearAt(t, sw, sh, 1, 0, (static_cast<float>(x) + 0.5f) / ks - 0.5f, (static_cast<float>(y) + 0.5f) / ks - 0.5f), 0.1f, 1.0f);
            for (int c = 0; c < 3; ++c) {
                float i = px[(y * w + x) * 4 + c];
                float j = (i - air[c]) / tv + air[c];
                px[(y * w + x) * 4 + c] = amount >= 0.0f ? i + (j - i) * amount
                                                  : i + (air[c] - i) * (-amount) * 0.5f * (1.0f - tv * 0.5f);
            }
        }
}

// 细节（Y + 色度 BT.709：B−Y、R−Y）。
void detail(float* px, size_t w, size_t h, const CameraRaw& p, float ps) {
    size_t n = w * h;
    std::vector<float> y(n), cb(n), cr(n);
    for (size_t i = 0; i < n; ++i) {
        float l = luma3(px[(i) * 4 + 0], px[(i) * 4 + 1], px[(i) * 4 + 2]);
        y[i] = l;
        cb[i] = px[(i) * 4 + 2] - l;
        cr[i] = px[(i) * 4 + 0] - l;
    }
    if (p.noiseLuminance > 0.0f) {
        float nv = p.noiseLuminance / 100.0f;
        size_t r = std::max(static_cast<size_t>(std::round((1.0f + 3.0f * nv) * ps)), static_cast<size_t>(1));
        std::vector<float> smooth = guided(y, y, w, h, r, (nv * 0.06f) * (nv * 0.06f) + 1e-6f);
        float keep = p.noiseLuminanceDetail / 100.0f * 0.5f;
        for (size_t i = 0; i < n; ++i) y[i] = smooth[i] + (y[i] - smooth[i]) * keep;
    }
    if (p.noiseColor > 0.0f) {
        float sigma = std::max(p.noiseColor / 100.0f * 6.0f * ps, 0.3f);
        std::vector<float> bcb = gaussF(cb, w, h, sigma);
        std::vector<float> bcr = gaussF(cr, w, h, sigma);
        float keep = p.noiseColorDetail / 100.0f * 0.3f;
        for (size_t i = 0; i < n; ++i) {
            cb[i] = bcb[i] + (cb[i] - bcb[i]) * keep;
            cr[i] = bcr[i] + (cr[i] - bcr[i]) * keep;
        }
    }
    if (p.sharpenAmount > 0.0f) {
        float sigma = std::max(std::clamp(p.sharpenRadius, 0.5f, 3.0f) * ps, 0.3f);
        std::vector<float> blur = gaussF(y, w, h, sigma);
        // Masking：平滑亮度梯度的边缘蒙版。
        std::vector<float> mask;
        if (p.sharpenMasking > 0.0f) {
            std::vector<float> s = gaussF(y, w, h, 1.0f * std::max(ps, 0.5f));
            float t = p.sharpenMasking / 100.0f * 0.08f;
            mask.resize(n);
            for (size_t i = 0; i < n; ++i) {
                size_t x = i % w;
                size_t yy = i / w;
                float gx = s[yy * w + std::min(x + 1, w - 1)] - s[yy * w + (x > 0 ? x - 1 : 0)];
                float gy = s[std::min(yy + 1, h - 1) * w + x] - s[(yy > 0 ? yy - 1 : 0) * w + x];
                mask[i] = smoothstep(t * 0.5f, t + 1e-4f, std::sqrt(gx * gx + gy * gy));
            }
        }
        float amt = p.sharpenAmount / 100.0f * 1.2f;
        float det = p.sharpenDetail / 100.0f;
        for (size_t i = 0; i < n; ++i) {
            float hp = y[i] - blur[i];
            // 低 Detail 阻尼大差（产光晕）。
            float limit = 0.03f + det * 0.3f;
            hp = std::clamp(hp, -limit, limit) * (0.5f + 0.5f * det);
            float m = mask.empty() ? 1.0f : mask[i];
            y[i] += amt * hp * m;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        float l = y[i], b = cb[i], r = cr[i];
        float rr = r + l;
        float bb = b + l;
        float g = (l - 0.2126f * rr - 0.0722f * bb) / 0.7152f;
        px[(i) * 4 + 0] = rr;
        px[(i) * 4 + 1] = g;
        px[(i) * 4 + 2] = bb;
    }
}

// 裁后暗角。
void vignette(float* px, size_t w, size_t h, const CameraRaw& p) {
    float amt = p.vignetteAmount / 100.0f;
    float mid = std::clamp(p.vignetteMidpoint, 0.0f, 100.0f) / 100.0f;
    float feather = std::clamp(p.vignetteFeather, 0.0f, 100.0f) / 100.0f;
    float round = std::clamp(p.vignetteRoundness, -100.0f, 100.0f) / 100.0f;
    float hl = std::clamp(p.vignetteHighlights, 0.0f, 100.0f) / 100.0f;
    const std::string& style = p.vignetteStyle;
    float cx = static_cast<float>(w) / 2.0f;
    float cy = static_cast<float>(h) / 2.0f;
    // 圆度：0 随画幅纵横比，+1 圆，−1 圆角矩形。
    float ax, ay;
    if (round >= 0.0f) {
        float r = std::min(cx, cy) + (std::max(cx, cy) - std::min(cx, cy)) * (1.0f - round);
        ax = cx >= cy ? r : cx;
        ay = cy > cx ? r : cy;
    } else {
        ax = cx;
        ay = cy;
    }
    float pw = 2.0f + std::max(-round, 0.0f) * 6.0f;
    float inner = 1.0f - mid * 0.8f;
    float edge0 = inner - feather * 0.5f * inner;
    float edge1 = inner + feather * 0.5f * (1.4f - inner) + 0.02f;
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x) {
            float dx = std::abs((static_cast<float>(x) + 0.5f - cx) / ax);
            float dy = std::abs((static_cast<float>(y) + 0.5f - cy) / ay);
            float rr = std::pow(std::pow(dx, pw) + std::pow(dy, pw), 1.0f / pw);
            float m = smoothstep(edge0, edge1, rr);
            if (m <= 0.0f) continue;
            float l = std::clamp(luma3(px[(y * w + x) * 4 + 0], px[(y * w + x) * 4 + 1], px[(y * w + x) * 4 + 2]), 0.0f, 1.0f);
            if (style == "paintOverlay") {
                float target = amt < 0.0f ? 0.0f : 1.0f;
                float kk = m * std::abs(amt);
                for (int c = 0; c < 3; ++c) px[(y * w + x) * 4 + c] += (target - px[(y * w + x) * 4 + c]) * kk;
            } else {
                float kk = std::pow(2.0f, amt * 2.0f * m);
                if (amt < 0.0f && hl > 0.0f) {
                    // 高光：亮区从压暗暗角里透出。
                    float protect = smoothstep(0.6f, 1.0f, l) * hl;
                    kk += (1.0f - kk) * protect;
                }
                if (style == "colorPriority") {
                    float nl = l * kk;
                    for (int c = 0; c < 3; ++c) px[(y * w + x) * 4 + c] += nl - l;
                } else {
                    for (int c = 0; c < 3; ++c) px[(y * w + x) * 4 + c] *= kk;
                }
            }
        }
}

inline bool isLinearCurve(const std::vector<std::array<float, 2>>& c) {
    if (c.size() < 2) return true;
    for (const auto& p : c)
        if (std::abs(p[0] - p[1]) >= 1e-3f) return false;
    return true;
}

}  // namespace

bool CameraRaw::toneNeutral() const {
    for (float v : {contrast, highlights, shadows, whites, blacks, texture, clarity})
        if (v != 0.0f) return false;
    return true;
}

bool CameraRaw::hslNeutral() const {
    for (int k = 0; k < 8; ++k)
        if (hslHue[k] != 0.0f || hslSat[k] != 0.0f || hslLum[k] != 0.0f) return false;
    return true;
}

bool CameraRaw::curvesNeutral() const {
    for (float v : {curveHighlights, curveLights, curveDarks, curveShadows})
        if (v != 0.0f) return false;
    return isLinearCurve(pointCurve) && isLinearCurve(pointCurveRed) && isLinearCurve(pointCurveGreen) && isLinearCurve(pointCurveBlue);
}

bool CameraRaw::isIdentity() const {
    return wbNeutral() && toneNeutral() && dehaze == 0.0f && vibrance == 0.0f && saturation == 0.0f && curvesNeutral() && hslNeutral() &&
           gradeNeutral() && sharpenAmount == 0.0f && noiseLuminance == 0.0f && noiseColor == 0.0f && grainAmount == 0.0f &&
           vignetteAmount == 0.0f;
}

std::string validateCurve(const std::vector<std::array<float, 2>>& points) {
    if (points.size() == 1) return "a nonempty curve needs at least two points";
    if (points.size() > 16) return "at most 16 curve points";
    for (const auto& p : points) {
        for (float v : p) {
            if (!std::isfinite(v) || v < 0.0f || v > 255.0f) return "curve coordinates must be finite and in 0..=255";
        }
    }
    for (size_t k = 1; k < points.size(); ++k)
        if (points[k][0] - points[k - 1][0] < 1.0f) return "curve inputs must increase by at least one level";
    return "";
}

const float kHslBands[8] = {0.0f, 30.0f, 60.0f, 120.0f, 180.0f, 240.0f, 270.0f, 300.0f};

std::array<float, 3> whiteBalanceGains(float temperature, float tint) {
    float t = temperature / 100.0f;
    float tn = tint / 100.0f;
    return {1.0f + 0.35f * t, 1.0f - 0.25f * tn, 1.0f - 0.35f * t};
}

std::array<float, 8> bandWeights(float h) {
    std::array<float, 8> w{};
    auto rem360 = [](float v) {
        float r = v - std::floor(v / 360.0f) * 360.0f;
        return r;
    };
    for (int k = 0; k < 8; ++k) {
        float c = kHslBands[k];
        float prev = kHslBands[(k + 7) % 8];
        float next = kHslBands[(k + 1) % 8];
        float diff = rem360(h - c + 540.0f) - 180.0f;
        float span = diff >= 0.0f ? rem360(next - c) : rem360(c - prev);
        w[k] = std::max(1.0f - std::abs(diff) / std::max(span, 1e-3f), 0.0f);
    }
    return w;
}

std::vector<float> cameraCurveLut(const std::vector<std::array<float, 2>>& pts, size_t n) {
    std::vector<std::array<float, 2>> p;
    p.reserve(pts.size());
    for (const auto& q : pts) p.push_back({q[0] / 255.0f, q[1] / 255.0f});
    std::sort(p.begin(), p.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    p.erase(std::unique(p.begin(), p.end(), [](const auto& a, const auto& b) { return std::abs(a[0] - b[0]) < 1e-6f; }), p.end());
    std::vector<float> out(n);
    if (p.size() < 2) {
        for (size_t k = 0; k < n; ++k) out[k] = n > 1 ? static_cast<float>(k) / static_cast<float>(n - 1) : 0.0f;
        return out;
    }
    size_t m = p.size();
    std::vector<float> d(m - 1);
    for (size_t k = 0; k + 1 < m; ++k) d[k] = (p[k + 1][1] - p[k][1]) / std::max(p[k + 1][0] - p[k][0], 1e-6f);
    std::vector<float> t(m, 0.0f);
    t[0] = d[0];
    t[m - 1] = d[m - 2];
    for (size_t k = 1; k + 1 < m; ++k) t[k] = d[k - 1] * d[k] <= 0.0f ? 0.0f : (d[k - 1] + d[k]) / 2.0f;
    for (size_t k = 0; k + 1 < m; ++k) {
        if (d[k] == 0.0f) {
            t[k] = 0.0f;
            t[k + 1] = 0.0f;
            continue;
        }
        float a = t[k] / d[k];
        float b = t[k + 1] / d[k];
        float s = a * a + b * b;
        if (s > 9.0f) {
            float tau = 3.0f / std::sqrt(s);
            t[k] = tau * a * d[k];
            t[k + 1] = tau * b * d[k];
        }
    }
    for (size_t i = 0; i < n; ++i) {
        float x = static_cast<float>(i) / static_cast<float>(n - 1);
        if (x <= p[0][0]) {
            out[i] = p[0][1];
            continue;
        }
        if (x >= p[m - 1][0]) {
            out[i] = p[m - 1][1];
            continue;
        }
        size_t k = m - 2;
        for (size_t j = 0; j + 1 < m; ++j) {
            if (x <= p[j + 1][0]) {
                k = j;
                break;
            }
        }
        float hh = p[k + 1][0] - p[k][0];
        float s = (x - p[k][0]) / hh;
        float h00 = 2.0f * s * s * s - 3.0f * s * s + 1.0f;
        float h10 = s * s * s - 2.0f * s * s + s;
        float h01 = -2.0f * s * s * s + 3.0f * s * s;
        float h11 = s * s * s - s * s;
        out[i] = std::clamp(h00 * p[k][1] + h10 * hh * t[k] + h01 * p[k + 1][1] + h11 * hh * t[k + 1], 0.0f, 1.0f);
    }
    return out;
}

// M23c 多线程显影：行带切分（photocraft par 语义——行与行独立；photocraft 测试
// 已证输出与线程数无关）。小图（<256 行）直接串行，避免线程启停开销。
template <typename Fn>
static void parRows(size_t h, Fn&& fn) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw <= 1 || h < 256) {
        fn(0, h);
        return;
    }
    size_t n = std::min<size_t>(hw, 8);
    std::vector<std::thread> threads;
    threads.reserve(n);
    for (size_t t = 0; t < n; ++t) {
        size_t y0 = h * t / n;
        size_t y1 = h * (t + 1) / n;
        threads.emplace_back([&, y0, y1] { fn(y0, y1); });
    }
    for (auto& th : threads) th.join();
}

void cameraRawDevelop(float* px, size_t w, size_t h, const CameraRaw& p, bool floatDoc) {
    if (w == 0 || h == 0 || p.isIdentity()) return;
    float ps = std::clamp(p.pixelScale, 0.01f, 1.0f);
    float long_ = static_cast<float>(std::max(w, h));
    // 1. 线性域白平衡 + 曝光。
    if (!p.wbNeutral()) {
        std::array<float, 3> g = whiteBalanceGains(p.temperature, p.tint);
        float norm = luma3(g[0], g[1], g[2]);
        float gain = std::pow(2.0f, p.exposure);
        for (int c = 0; c < 3; ++c) g[c] = g[c] / norm * gain;
        parRows(h, [&](size_t y0, size_t y1) {
            for (size_t y = y0; y < y1; ++y) {
                for (size_t x = 0; x < w; ++x) {
                    size_t i = y * w + x;
                    for (int c = 0; c < 3; ++c) {
                        px[i * 4 + c] = linearToSrgb(std::max(srgbToLinear(px[i * 4 + c]) * g[c], 0.0f));
                    }
                }
            }
        });
    }
    // 2. 去雾。
    if (p.dehaze != 0.0f) dehaze(px, w, h, p.dehaze / 100.0f);
    // 3. 亮度域色调。
    if (!p.toneNeutral()) {
        size_t n = w * h;
        std::vector<float> l(n);
        std::vector<float> lc(n);
        for (size_t i = 0; i < n; ++i) {
            l[i] = std::max(luma3(px[(i) * 4 + 0], px[(i) * 4 + 1], px[(i) * 4 + 2]), 0.0f);
            lc[i] = std::min(l[i], 1.0f);
        }
        std::vector<float> base, clar, tex;
        bool hasBase = false, hasClar = false, hasTex = false;
        if (p.highlights != 0.0f || p.shadows != 0.0f) {
            size_t r = std::max<size_t>(static_cast<size_t>(long_ * 0.015f), 2);
            base = guidedFast(lc, lc, w, h, r, 0.02f);
            hasBase = true;
        }
        if (p.clarity != 0.0f) {
            clar = guidedFast(lc, lc, w, h, std::max<size_t>(static_cast<size_t>(long_ * 0.02f), 3), 0.005f);
            hasClar = true;
        }
        if (p.texture != 0.0f) {
            tex = gaussF(lc, w, h, std::max(long_ * 0.002f, 1.5f * ps));
            hasTex = true;
        }
        float kCon = p.contrast / 100.0f * 0.6f;
        parRows(h, [&](size_t y0, size_t y1) {
            for (size_t y = y0; y < y1; ++y) {
                for (size_t x = 0; x < w; ++x) {
                    size_t i = y * w + x;
                    float l0 = l[i];
                    float lv = lc[i];
                    if (hasBase) {
                        float bv = std::clamp(base[i], 0.0f, 1.0f);
                        float hm = smoothstep(0.45f, 0.95f, bv);
                        float sm = 1.0f - smoothstep(0.05f, 0.5f, bv);
                        lv *= std::pow(2.0f, p.highlights / 100.0f * 1.2f * hm + p.shadows / 100.0f * 1.4f * sm);
                    }
                    if (hasClar) {
                        float mw = 1.0f - std::pow(2.0f * std::clamp(lv, 0.0f, 1.0f) - 1.0f, 2.0f);
                        lv += p.clarity / 100.0f * 0.8f * (lc[i] - clar[i]) * mw;
                    }
                    if (hasTex) {
                        lv += p.texture / 100.0f * 0.9f * std::clamp(lc[i] - tex[i], -0.1f, 0.1f);
                    }
                    if (p.whites != 0.0f) lv += p.whites / 100.0f * 0.25f * smoothstep(0.4f, 1.0f, lv);
                    if (p.blacks != 0.0f) lv += p.blacks / 100.0f * 0.15f * (1.0f - smoothstep(0.0f, 0.45f, lv));
                    if (kCon != 0.0f) {
                        float xx = std::clamp(lv, 0.0f, 1.0f);
                        lv += -kCon * std::sin(kTau * xx) / kTau;
                    }
                    float lv2 = std::max(lv, 0.0f) + (l0 - lc[i]);
                    float ratio = l0 > 1e-5f ? lv2 / l0 : 1.0f;
                    for (int c = 0; c < 3; ++c) {
                        px[(i) * 4 + c] = l0 > 1e-5f ? px[(i) * 4 + c] * ratio : px[(i) * 4 + c] + (lv2 - l0);
                    }
                }
            }
        });
    }
    // 4–7：逐像素颜色级。
    bool needColor = p.vibrance != 0.0f || p.saturation != 0.0f || !p.curvesNeutral() || !p.hslNeutral() || !p.gradeNeutral();
    if (needColor) {
        bool hasPar = !(p.curveHighlights == 0.0f && p.curveLights == 0.0f && p.curveDarks == 0.0f && p.curveShadows == 0.0f);
        std::vector<float> par = hasPar ? parametricLut(p, 1024) : std::vector<float>();
        auto pts = [](const std::vector<std::array<float, 2>>& c) -> std::vector<float> {
            if (isLinearCurve(c)) return {};
            return cameraCurveLut(c, 1024);
        };
        std::vector<float> master = pts(p.pointCurve);
        std::vector<float> crv[3] = {pts(p.pointCurveRed), pts(p.pointCurveGreen), pts(p.pointCurveBlue)};
        bool hsl = !p.hslNeutral();
        bool grade = !p.gradeNeutral();
        auto wheelTint = [](const CameraWheel& wh) {
            std::array<float, 3> c = hsvToRgb(wh.hue, 1.0f, 1.0f);
            float l = luma3(c[0], c[1], c[2]);
            float k = wh.sat / 100.0f * 0.3f;
            return std::array<float, 3>{(c[0] - l) * k, (c[1] - l) * k, (c[2] - l) * k};
        };
        std::array<std::array<float, 3>, 4> tints = {wheelTint(p.gradeShadows), wheelTint(p.gradeMidtones), wheelTint(p.gradeHighlights),
                                                     wheelTint(p.gradeGlobal)};
        float blend = 0.15f + std::clamp(p.gradeBlending, 0.0f, 100.0f) / 100.0f * 0.35f;
        float bal = std::clamp(p.gradeBalance, -100.0f, 100.0f) / 100.0f * 0.25f;
        parRows(h, [&](size_t ry0, size_t ry1) {
          for (size_t y = ry0; y < ry1; ++y)
            for (size_t x = 0; x < w; ++x) {
            size_t i = y * w + x;
            std::array<float, 3> c = {px[(i) * 4 + 0], px[(i) * 4 + 1], px[(i) * 4 + 2]};
            if (p.vibrance != 0.0f || p.saturation != 0.0f) {
                float l = luma3(c[0], c[1], c[2]);
                float mx = std::max({c[0], c[1], c[2]});
                float mn = std::min({c[0], c[1], c[2]});
                float sat = mx > 1e-6f ? (mx - mn) / mx : 0.0f;
                // 自然饱和度像 ACR 一样略保护肤色系色相（橙）。
                float hue = rgbToHsv(c)[0];
                float skin = std::clamp(1.0f - std::abs(hue - 25.0f) / 25.0f, 0.0f, 1.0f) * 0.5f;
                float kk = 1.0f + p.saturation / 100.0f + p.vibrance / 100.0f * (1.0f - sat) * (1.0f - skin);
                kk = std::max(kk, 0.0f);
                for (int ch = 0; ch < 3; ++ch) c[ch] = l + (c[ch] - l) * kk;
            }
            if (hasPar)
                for (int ch = 0; ch < 3; ++ch) c[ch] = lutEval(par, c[ch]);
            if (!master.empty())
                for (int ch = 0; ch < 3; ++ch) c[ch] = lutEval(master, c[ch]);
            for (int ch = 0; ch < 3; ++ch)
                if (!crv[ch].empty()) c[ch] = lutEval(crv[ch], c[ch]);
            if (hsl) {
                std::array<float, 3> cm = {std::max(c[0], 0.0f), std::max(c[1], 0.0f), std::max(c[2], 0.0f)};
                auto hsv = rgbToHsv(cm);
                float hh = hsv[0], s = hsv[1], v = hsv[2];
                if (s > 1e-4f) {
                    std::array<float, 8> wts = bandWeights(hh);
                    float dh = 0.0f, ds = 0.0f, dl = 0.0f;
                    for (int k = 0; k < 8; ++k) {
                        dh += wts[k] * p.hslHue[k];
                        ds += wts[k] * p.hslSat[k];
                        dl += wts[k] * p.hslLum[k];
                    }
                    float nh = hh + dh / 100.0f * 30.0f;
                    float ns = std::clamp(s * (1.0f + ds / 100.0f), 0.0f, 1.0f);
                    std::array<float, 3> out = hsvToRgb(nh, ns, v);
                    // 保亮度，再按饱和度加权的亮度偏移。
                    float l0 = luma3(c[0], c[1], c[2]);
                    float l1 = luma3(out[0], out[1], out[2]);
                    if (l1 > 1e-6f)
                        for (int ch = 0; ch < 3; ++ch) out[ch] *= l0 / l1;
                    float kk = std::pow(2.0f, dl / 100.0f * 1.0f * s);
                    for (int ch = 0; ch < 3; ++ch) c[ch] = out[ch] * kk;
                }
            }
            if (grade) {
                float l = std::clamp(luma3(c[0], c[1], c[2]), 0.0f, 1.0f);
                float sh = 1.0f - smoothstep(0.0f, 0.5f + bal + blend, l);
                float hi = smoothstep(0.5f + bal - blend, 1.0f, l);
                float mid = std::max(1.0f - sh - hi, 0.0f);
                float masks[4] = {sh, mid, hi, 1.0f};
                float lums[4] = {p.gradeShadows.lum, p.gradeMidtones.lum, p.gradeHighlights.lum, p.gradeGlobal.lum};
                for (int k = 0; k < 4; ++k) {
                    for (int ch = 0; ch < 3; ++ch) c[ch] += masks[k] * tints[k][ch];
                    if (lums[k] != 0.0f) {
                        float f = std::pow(2.0f, masks[k] * lums[k] / 100.0f * 0.6f);
                        for (int ch = 0; ch < 3; ++ch) c[ch] *= f;
                    }
                }
            }
            px[(i) * 4 + 0] = c[0];
            px[(i) * 4 + 1] = c[1];
            px[(i) * 4 + 2] = c[2];
            }
        });
    }
    // 8. 细节。
    if (p.noiseLuminance > 0.0f || p.noiseColor > 0.0f || p.sharpenAmount > 0.0f) detail(px, w, h, p, ps);
    // 9. 效果。
    if (p.vignetteAmount != 0.0f) vignette(px, w, h, p);
    if (p.grainAmount > 0.0f) {
        float cell = 1.0f + p.grainSize / 25.0f;
        float rough = p.grainRoughness / 100.0f;
        float amt = p.grainAmount / 100.0f * 0.12f;
        uint64_t seed = p.seed;
        float pscale = 1.0f / ps;
        parRows(h, [&](size_t gy0, size_t gy1) {
          for (size_t y = gy0; y < gy1; ++y)
            for (size_t x = 0; x < w; ++x) {
                // 全分辨率坐标采样，代理预览与结果一致。
                float fx = static_cast<float>(x) * pscale / cell;
                float fy = static_cast<float>(y) * pscale / cell;
                float nv = valueNoise(fx, fy, seed) * (1.0f - rough) + valueNoise(fx * 2.3f, fy * 2.3f, seed + 7) * rough;
                float l = std::clamp(luma3(px[(y * w + x) * 4 + 0], px[(y * w + x) * 4 + 1], px[(y * w + x) * 4 + 2]), 0.0f, 1.0f);
                float g = (nv - 0.5f) * 2.0f * amt * (0.4f + 2.4f * l * (1.0f - l));
                for (int c = 0; c < 3; ++c) px[(y * w + x) * 4 + c] += g;
            }
        });
    }
    if (!floatDoc) {
        parRows(h, [&](size_t y0, size_t y1) {
            for (size_t i = y0 * w; i < y1 * w; ++i) {
                for (int c = 0; c < 3; ++c) px[i * 4 + c] = std::clamp(px[i * 4 + c], 0.0f, 1.0f);
            }
        });
    }
}

}  // namespace montage
