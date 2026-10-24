// M23a：raw 显影实现。翻译自 photocraft crates/raw/src/develop.rs（MIT OR Apache-2.0）。
// par 单线程化（photocraft 测试已证线程数无关）。

#include "io/raw/raw_develop.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "io/raw/raw_color.h"

namespace montage {
namespace raw {

namespace {

// 一个样本的线性化 + 黑/白缩放（白平衡前），0..1。
struct Scale {
    const Sensor& s;
    const std::vector<uint16_t>* lin = nullptr;

    explicit Scale(const Sensor& sensor) : s(sensor) {
        if (sensor.linearization) lin = &*sensor.linearization;
    }

    inline float value(size_t x, size_t y, size_t sample) const {
        const Sensor& s = this->s;
        size_t idx = (y * s.width + x) * s.samples + sample;
        uint16_t raw = idx < s.data.size() ? s.data[idx] : 0;
        float v;
        if (lin) {
            size_t li = raw;
            v = static_cast<float>(li < lin->size() ? (*lin)[li] : lin->back());
        } else {
            v = static_cast<float>(raw);
        }
        size_t ax = x > s.active.x ? x - s.active.x : 0;
        size_t ay = y > s.active.y ? y - s.active.y : 0;
        float black = s.black.at(ax, ay, sample, s.samples);
        float white = s.white[std::min<size_t>(sample, 2)];
        float range = white - black;
        if (std::isnan(range) || range < 1.0f) return 0.0f;
        float n = (v - black) / range;
        return std::isfinite(n) ? n : 0.0f;
    }
};

double max3(const std::array<double, 3>& v) { return std::max({v[0], v[1], v[2]}); }

// 未剪裁中间调样本的灰世界中性（相机空间 R, G, B）。
std::array<double, 3> greyWorld(const Sensor& s, const Scale& sc) {
    Rect c = s.crop;
    if (c.width == 0 || c.height == 0) return {1.0, 1.0, 1.0};
    double sum[3] = {0, 0, 0};
    uint64_t n[3] = {0, 0, 0};
    for (size_t y = c.y; y < c.y + c.height; ++y) {
        for (size_t x = c.x; x < c.x + c.width; ++x) {
            for (size_t k = 0; k < std::min<size_t>(s.samples, 3); ++k) {
                size_t ch = s.cfa ? s.cfa->color(x, y) : k;
                if (ch > 2) continue;
                float v = sc.value(x, y, k);
                if (v > 0.002f && v < 0.9f) {
                    sum[ch] += v;
                    ++n[ch];
                }
            }
        }
    }
    double mean[3];
    for (int c = 0; c < 3; ++c) mean[c] = n[c] > 0 ? sum[c] / static_cast<double>(n[c]) : 0.0;
    for (double m : mean)
        if (std::isnan(m) || m <= 1e-6) return {1.0, 1.0, 1.0};
    double mx = std::max({mean[0], mean[1], mean[2]});
    return {mean[0] / mx, mean[1] / mx, mean[2] / mx};
}

std::optional<std::array<double, 3>> normalize3(const std::array<double, 3>& n) {
    double m = max3(n);
    if (!(m > 0.0)) return std::nullopt;
    for (double v : n)
        if (!std::isfinite(v) || v <= 1e-4) return std::nullopt;
    return std::array<double, 3>{n[0] / m, n[1] / m, n[2] / m};
}

// `opts.whiteBalance` 对应的相机空间中性（最大分量 1）。
std::array<double, 3> resolveNeutral(const Sensor& s, const DevelopOptions& opts, const Scale& sc, std::vector<std::string>& warnings) {
    // 单色数据无颜色可平衡。
    if (s.isMonochrome()) return {1.0, 1.0, 1.0};
    // 白平衡：相机空间中性，最大分量 1。
    std::optional<std::array<double, 3>> asShot = s.color.asShotNeutral;
    if (!asShot && s.color.asShotWhiteXY) {
        asShot = xyToNeutral(s.color, *s.color.asShotWhiteXY);
    }
    if (!asShot && s.cameraWb) {
        std::array<double, 3> inv = {1.0 / (*s.cameraWb)[0], 1.0 / (*s.cameraWb)[1], 1.0 / (*s.cameraWb)[2]};
        asShot = normalize3(inv);
    }
    switch (opts.whiteBalance.mode) {
        case WhiteBalance::AsShot:
            if (asShot) return *asShot;
            warnings.push_back("no as-shot white balance in the file; estimated automatically (grey world)");
            return greyWorld(s, sc);
        case WhiteBalance::Auto:
            return greyWorld(s, sc);
        default: {
            std::array<double, 3> inv = {1.0 / opts.whiteBalance.multipliers[0], 1.0 / opts.whiteBalance.multipliers[1],
                                         1.0 / opts.whiteBalance.multipliers[2]};
            auto n = normalize3(inv);
            if (!n) throw RawError::malformed("white-balance multipliers must be positive");
            return *n;
        }
    }
}

// 增益图（DNG OpcodeList2 GainMap）在数据行 y、列 x0..x0+w、样本 k 的因子；
// 无图触及该行返回空。
std::optional<std::vector<float>> gainRow(const Sensor& s, size_t y, size_t x0, size_t w, size_t k) {
    Rect a = s.active;
    if (s.gainMaps.empty() || y < a.y || a.width == 0 || a.height == 0) return std::nullopt;
    size_t ay = y - a.y;
    double rv = (static_cast<double>(ay) + 0.5) / static_cast<double>(a.height);
    std::optional<std::vector<float>> out;
    for (const GainMap& m : s.gainMaps) {
        if (k < m.plane || k >= m.plane + m.planes || ay < m.top || ay >= m.bottom) continue;
        if (m.rowPitch == 0 || (ay - m.top) % m.rowPitch != 0) continue;
        std::vector<float> row = m.rowOf(rv, k - m.plane);
        if (!out) out = std::vector<float>(w, 1.0f);
        for (size_t i = 0; i < w; ++i) {
            size_t x = x0 + i;
            if (x >= a.x && m.covers(x - a.x, ay)) {
                (*out)[i] *= m.at(row, (static_cast<double>(x - a.x) + 0.5) / static_cast<double>(a.width));
            }
        }
    }
    return out;
}

// `curve` 在线性 v：点间按 log₂ 输入线性插值，首点下为过 0 直线，末点外取末值。
float toneCurveEval(const std::vector<std::array<float, 2>>& curve, float v) {
    if (curve.empty()) return v;
    const auto& first = curve.front();
    const auto& last = curve.back();
    if (v <= first[0]) return first[0] > 0.0f ? v * first[1] / first[0] : first[1];
    if (v >= last[0]) return last[1];
    // partition_point 等价：第一个 p[0] > v 的下标，钳到 [1, len-1]。
    size_t lo = 1, hi = curve.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (curve[mid][0] <= v) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    size_t i = std::clamp<size_t>(lo, 1, curve.size() - 1);
    const auto& p0 = curve[i - 1];
    const auto& p1 = curve[i];
    float f = (std::log2(v) - std::log2(p0[0])) / (std::log2(p1[0]) - std::log2(p0[0]));
    return p0[1] + (p1[1] - p0[1]) * f;
}

}  // namespace

GammaLut::GammaLut(const std::vector<std::array<float, 2>>& curve) {
    table.reserve(kSteps + 2);
    for (size_t i = 0; i <= kSteps + 1; ++i) {
        float s = std::min(static_cast<float>(i) / static_cast<float>(kSteps), 1.0f);
        float v = std::clamp(toneCurveEval(curve, s * s), 0.0f, 1.0f);
        table.push_back(std::pow(v, 1.0f / 1.8f) * 65535.0f);
    }
}

std::tuple<std::vector<uint16_t>, size_t, size_t> orientRgb(std::vector<uint16_t> src, size_t w, size_t h, uint16_t o) {
    if (o < 2 || o > 8) return {std::move(src), w, h};
    bool swap = o >= 5;
    size_t ow = swap ? h : w;
    size_t oh = swap ? w : h;
    std::vector<uint16_t> out(src.size(), 0);
    for (size_t y2 = 0; y2 < oh; ++y2) {
        for (size_t x2 = 0; x2 < ow; ++x2) {
            size_t x, y;
            switch (o) {
                case 2: x = w - 1 - x2; y = y2; break;
                case 3: x = w - 1 - x2; y = h - 1 - y2; break;
                case 4: x = x2; y = h - 1 - y2; break;
                case 5: x = y2; y = x2; break;
                case 6: x = y2; y = h - 1 - x2; break;
                case 7: x = w - 1 - y2; y = h - 1 - x2; break;
                default: x = w - 1 - y2; y = x2; break;
            }
            std::memcpy(out.data() + (y2 * ow + x2) * 3, src.data() + (y * w + x) * 3, 3 * sizeof(uint16_t));
        }
    }
    return {std::move(out), ow, oh};
}

// M23c：fit_look 复用——与 developSensor 同源的白平衡（含用户 temp/tint 调制）
// 与样本线性化值。Multipliers 非法时返回 false。
bool lookWhiteBalance(const Sensor& s, const DevelopOptions& opts, std::array<double, 3>& neutral,
                      std::vector<std::string>& warnings) {
    try {
        std::array<double, 3> n = resolveNeutral(s, opts, Scale(s), warnings);
        float temp = std::clamp(opts.temp, -100.0f, 100.0f);
        float tint = std::clamp(opts.tint, -100.0f, 100.0f);
        double rMod = 1.0 + temp * 0.003;
        double bMod = 1.0 - temp * 0.003;
        double gMod = 1.0 - tint * 0.003;
        if (rMod > 0.1) n[0] /= rMod;
        if (bMod > 0.1) n[2] /= bMod;
        if (gMod > 0.1) n[1] /= gMod;
        neutral = n;
        return true;
    } catch (const RawError&) {
        return false;
    }
}

float lookSampleValue(const Sensor& s, size_t x, size_t y, size_t sample) {
    return Scale(s).value(x, y, sample);
}

Developed developSensor(const Sensor& s, const DevelopOptions& opts) {
    Rect c = s.crop;
    if (c.empty() || c.x + c.width > s.width || c.y + c.height > s.height || s.data.size() < s.width * s.height * s.samples) {
        throw RawError::malformed("crop lies outside the sensor data");
    }
    if (s.samples != 1 && s.samples != 3) {
        throw RawError::unsupported(std::to_string(s.samples) + " samples per pixel");
    }
    // 工作缓冲：每像素一个 f32 平面 + f32 RGB + u16 RGB；非 Bayer 图案另有绿平面。
    bool bayer = !s.cfa || s.cfa->isBayer();
    opts.limits.check(c.width, c.height, bayer ? 4 + 12 + 6 : 4 + 4 + 12 + 6);
    std::vector<std::string> warnings = s.warnings;
    Scale sc(s);

    std::array<double, 3> neutral = resolveNeutral(s, opts, sc, warnings);
    // 用户色温/色调：中性色上调制（增益 = 1/中性，故中性反向除）。
    {
        float temp = std::clamp(opts.temp, -100.0f, 100.0f);
        float tint = std::clamp(opts.tint, -100.0f, 100.0f);
        double rMod = 1.0 + temp * 0.003;
        double bMod = 1.0 - temp * 0.003;
        double gMod = 1.0 - tint * 0.003;
        if (rMod > 0.1) neutral[0] /= rMod;
        if (bMod > 0.1) neutral[2] /= bMod;
        if (gMod > 0.1) neutral[1] /= gMod;
    }
    std::array<float, 3> mult{};
    for (int i = 0; i < 3; ++i) mult[i] = static_cast<float>(1.0 / neutral[i]);

    // 平衡相机 → XYZ D50：文件校准、per-file look（M23c）或中性回退。
    bool mono = s.isMonochrome();
    std::optional<Mat3> balanced = balancedToXyzD50(s.color, neutral);
    Mat3 toXyz;
    const ToneCurve* lookTone = nullptr;
    if (balanced) {
        toXyz = *balanced;
    } else if (opts.cameraLook && !mono && opts.cameraLook->isUsable()) {
        toXyz = opts.cameraLook->toXyz;
        lookTone = opts.cameraLook->tone ? &*opts.cameraLook->tone : nullptr;
        warnings.push_back(opts.cameraLook->note());
    } else {
        if (opts.cameraLook && !(opts.cameraLook->isUsable())) {
            warnings.push_back("camera look not usable; ignored");
        }
        if (s.format != RawFormat::Dng && !mono) {
            warnings.push_back("no colour calibration for this camera; camera colours are treated as sRGB primaries");
        }
        toXyz = srgbToXyzD50();
    }
    double gain = std::pow(2.0, s.baselineExposure + std::clamp(opts.exposure, -10.0, 10.0));
    Mat3 outM = matScale(matMul(xyzD50ToProphoto(), toXyz), gain);
    float outF[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) outF[i][j] = static_cast<float>(outM[i][j]);

    // 中性灰经 outF：仅增益（矩阵把相机白映射到 ProPhoto 白），三输出通道严格相等。
    float greyGain = static_cast<float>(gain);
    size_t w = c.width, h = c.height;
    std::vector<float> rgb;
    std::optional<std::string> pattern;
    if (s.cfa) {
        // 归一化、平衡、剪裁后的 CFA 平面。
        Padded plane(w, h);
        size_t stride = plane.stride;
        // 快路径：黑最多按 2×2 位置变化，黑与增益每 period 列重复（CFA 宽取偶）。
        bool periodic = s.black.deltaH.empty() && s.black.deltaV.empty() && (s.black.rows == 1 || s.black.rows == 2) &&
                        (s.black.cols == 1 || s.black.cols == 2) && s.cfa->width <= 16;
        size_t period = s.cfa->width % 2 == 0 ? std::max<size_t>(s.cfa->width, 2) : s.cfa->width * 2;
        const Cfa& cfa = *s.cfa;
        const std::vector<uint16_t>* lin = s.linearization ? &*s.linearization : nullptr;
        for (size_t r = 0; r < h; ++r) {
            float* row = plane.data.data() + (r + kDemosaicPad) * stride + kDemosaicPad;
            size_t y = c.y + r;
            if (periodic) {
                // 周期内每列：黑与组合的缩放 × 白平衡增益。
                std::vector<std::pair<float, float>> k(period);
                for (size_t p = 0; p < period; ++p) {
                    size_t x = c.x + p;
                    float black = s.black.at(x > s.active.x ? x - s.active.x : 0, y > s.active.y ? y - s.active.y : 0, 0, 1);
                    float range = s.white[0] - black;
                    float g = range >= 1.0f ? mult[std::min<size_t>(cfa.color(x, y), 2)] / range : 0.0f;
                    k[p] = {black, g};
                }
                const uint16_t* src = s.data.data() + y * s.width + c.x;
                for (size_t i = 0; i < w; ++i) {
                    auto [black, g] = k[i % period];
                    float v;
                    if (!lin) {
                        v = (static_cast<float>(src[i]) - black) * g;
                    } else {
                        uint16_t raw = src[i];
                        float lv = static_cast<float>(raw < lin->size() ? (*lin)[raw] : lin->back());
                        v = (lv - black) * g;
                    }
                    row[i] = std::clamp(v, 0.0f, 1.0f);
                }
            } else {
                for (size_t i = 0; i < w; ++i) {
                    size_t x = c.x + i;
                    size_t ch = std::min<size_t>(cfa.color(x, y), 2);
                    row[i] = std::clamp(sc.value(x, y, 0) * mult[ch], 0.0f, 1.0f);
                }
            }
            // 镜头阴影增益图，在白平衡剪裁之后（剪掉的高光保持中性）。
            if (auto g = gainRow(s, y, c.x, w, 0)) {
                for (size_t i = 0; i < w; ++i) row[i] = std::clamp(row[i] * (*g)[i], 0.0f, 1.0f);
            }
        }
        plane.fillBorders();
        auto names = [](const uint8_t* p, size_t n) {
            std::string out;
            for (size_t i = 0; i < n; ++i) out += "RGB"[std::min<uint8_t>(p[i], 2)];
            return out;
        };
        if (bayer) {
            auto p = cfa.phase(c.x, c.y);
            uint8_t ph[4] = {p[0], p[1], p[2], p[3]};
            pattern = names(ph, 4);
            rgb = demosaicBayer(plane, ph, opts.demosaic, toXyz);
        } else {
            DemosaicPattern pat;
            pat.w = std::clamp<size_t>(cfa.width, 1, 16);
            pat.h = std::clamp<size_t>(cfa.height, 1, 16);
            pat.colors.reserve(pat.w * pat.h);
            for (size_t i = 0; i < pat.w * pat.h; ++i) {
                pat.colors.push_back(std::min<uint8_t>(cfa.color(c.x + i % pat.w, c.y + i / pat.w), 2));
            }
            std::string joined;
            for (size_t row = 0; row < pat.h; ++row) {
                if (!joined.empty()) joined += "/";
                joined += names(pat.colors.data() + row * pat.w, pat.w);
            }
            pattern = joined;
            rgb = demosaicCfa(plane, pat);
        }
    } else {
        pattern = std::nullopt;
        rgb.assign(w * h * 3, 0.0f);
        for (size_t r = 0; r < h; ++r) {
            size_t y = c.y + r;
            float* row = rgb.data() + r * w * 3;
            for (size_t i = 0; i < w; ++i) {
                for (size_t k = 0; k < 3; ++k) {
                    // 单色：一个样本进三个通道。
                    size_t sample = std::min<size_t>(k, s.samples > 0 ? s.samples - 1 : 0);
                    row[i * 3 + k] = std::clamp(sc.value(c.x + i, y, sample) * mult[k], 0.0f, 1.0f);
                }
            }
            for (size_t k = 0; k < std::min<size_t>(s.samples, 3); ++k) {
                if (auto g = gainRow(s, y, c.x, w, k)) {
                    for (size_t i = 0; i < w; ++i) row[i * 3 + k] = std::clamp(row[i * 3 + k] * (*g)[i], 0.0f, 1.0f);
                }
            }
        }
    }

    // 相机 → ProPhoto、曝光、gamma 1.8、16 位。
    std::vector<uint16_t> out(w * h * 3);
    // M23c：look 的色调曲线折叠进编码表（曲线作用于线性 ProPhoto，与矩阵+曝光
    // 之后的编码前一步吻合；文件自带 toneCurve 仅在无 look 时使用）。
    GammaLut lut(lookTone != nullptr ? lookTone->knots() : s.toneCurve);
    for (size_t p = 0; p < w * h; ++p) {
        const float* src = rgb.data() + p * 3;
        uint16_t* o = out.data() + p * 3;
        if (mono) {
            uint16_t g = lut.encode(greyGain * src[0]);
            o[0] = o[1] = o[2] = g;
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            o[k] = lut.encode(outF[k][0] * src[0] + outF[k][1] * src[1] + outF[k][2] * src[2]);
        }
    }

    std::vector<uint16_t> finalOut = std::move(out);
    size_t ow = w, oh = h;
    if (opts.orient) {
        auto [rot, rw, rh] = orientRgb(std::move(finalOut), w, h, s.orientation);
        finalOut = std::move(rot);
        ow = rw;
        oh = rh;
    }
    float nmax = std::min({mult[0], mult[1], mult[2]});
    nmax = std::max(nmax, 1e-6f);
    Developed d;
    d.width = static_cast<uint32_t>(ow);
    d.height = static_cast<uint32_t>(oh);
    d.rgb = std::move(finalOut);
    d.info.format = s.format;
    d.info.make = s.make;
    d.info.model = s.model;
    d.info.sensorWidth = s.width;
    d.info.sensorHeight = s.height;
    d.info.cfa = pattern;
    for (int i = 0; i < 3; ++i) d.info.wbMultipliers[i] = static_cast<double>(mult[i] / nmax);
    d.info.orientation = s.orientation;
    d.info.baselineExposure = s.baselineExposure;
    d.warnings = std::move(warnings);
    return d;
}

}  // namespace raw
}  // namespace montage
