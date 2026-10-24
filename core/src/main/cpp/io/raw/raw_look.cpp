// M23c：per-file 相机 look 拟合实现。翻译自 photocraft crates/raw/src/look.rs
// （MIT OR Apache-2.0）。par::map 单线程化（photocraft 测试已证线程数无关）。

#include "io/raw/raw_look.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <utility>

#include "io/raw/raw_color.h"
#include "io/raw/raw_develop.h"
#include "io/raw/raw_internal.h"

namespace montage {
namespace raw {
namespace {

using Vec3 = std::array<double, 3>;

double srgbToLinear(double v) {
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

// 分段线性插值：from 坐标在 t 处的 to 坐标（首个到达 t 的段胜出）。
double interp(const std::vector<std::array<double, 2>>& k, double t, size_t from, size_t to) {
    // partition_point: 首个 p[from] >= t
    size_t lo = 0, hi = k.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (k[mid][from] < t) lo = mid + 1;
        else hi = mid;
    }
    size_t i = std::clamp<size_t>(lo, 1, std::max<size_t>(k.size() - 1, 1));
    if (i == 0 || i >= k.size() + 1) return t;
    if (i >= k.size()) return k.empty() ? t : k.back()[to];
    const auto& a = k[i - 1];
    const auto& b = k[i];
    double d = b[from] - a[from];
    if (d <= 0.0) return a[to];
    double f = std::clamp((t - a[from]) / d, 0.0, 1.0);
    return a[to] + (b[to] - a[to]) * f;
}

// TIFF orientation：直立坐标 (x2,y2)（w2×h2）→ 存储坐标 (x,y)。
void sourceXY(uint16_t o, size_t w, size_t h, size_t x2, size_t y2, size_t& x, size_t& y) {
    switch (o) {
        case 2: x = w - 1 - x2; y = y2; break;
        case 3: x = w - 1 - x2; y = h - 1 - y2; break;
        case 4: x = x2; y = h - 1 - y2; break;
        case 5: x = y2; y = x2; break;
        case 6: x = y2; y = h - 1 - x2; break;
        case 7: x = w - 1 - y2; y = h - 1 - x2; break;
        case 8: x = w - 1 - y2; y = x2; break;
        default: x = x2; y = y2; break;  // 1 与未定义 = 恒等
    }
}

// 一个网格单元。
struct Cell {
    Vec3 cam{0, 0, 0};        // 白平衡相机 RGB 0..1
    bool camClipped = false;  // 任一采样接近剪裁
    Vec3 rgb{0, 0, 0};        // JPEG 的线性 ProPhoto 颜色
    double lum = 0.0;         // rgb 的 Y(D50)
    double refClipped = 0.0;  // 单元 JPEG 像素通道 ≥250 的比例
    double edgeWeight = 0.0;  // JPEG 局部对比度权重（边缘少计）
};

// 训练/评估样本。
struct Sample {
    Vec3 cam{};
    Vec3 rgb{};
    double lum = 0.0;
    double perceptual = 0.0;
    double weight = 0.0;
};

// 线性 ProPhoto RGB 的 CIE L*a*b* (D50)。
struct Lab {
    Mat3 toXyz;
    Vec3 white{};

    Lab() : toXyz(rgbToXyz(kRommPrimaries, kD50Xy)) {
        white = matApply(toXyz, {1.0, 1.0, 1.0});
    }

    std::array<double, 3> lab(const Vec3& rgb) const {
        Vec3 xyz = matApply(toXyz, rgb);
        auto f = [](double t) {
            return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0;
        };
        double fx = f(xyz[0] / white[0]);
        double fy = f(xyz[1] / white[1]);
        double fz = f(xyz[2] / white[2]);
        return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
    }

    double deltaE(const Vec3& a, const Vec3& b) const {
        auto p = lab(a);
        auto q = lab(b);
        return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) +
                         (p[2] - q[2]) * (p[2] - q[2]));
    }
};

// 显影管线对一个样本的输出（如 16 位编码般钳到 0..1）。
Vec3 renderLook(const Mat3& m, const ToneCurve* tone, const Vec3& cam) {
    Vec3 v = matApply(m, cam);
    for (int k = 0; k < 3; ++k) {
        double x = std::isfinite(v[k]) ? std::clamp(v[k], 0.0, 1.0) : 0.0;
        v[k] = tone != nullptr ? tone->eval(x) : x;
    }
    return v;
}

double meanDeltaE(const Lab& lab, const std::vector<const Sample*>& samples, const Mat3& m,
                  const ToneCurve* tone) {
    if (samples.empty()) return std::numeric_limits<double>::infinity();
    double sum = 0.0;
    for (const Sample* s : samples) {
        sum += lab.deltaE(renderLook(m, tone, s->cam), s->rgb);
    }
    return sum / static_cast<double>(samples.size());
}

// 亮度 lum 附近把线性差折成 ~L* 单位的误差权重标度。
double perceptual(double lum) {
    return (116.0 / 3.0) * std::pow(std::max(lum, 0.0) + 0.008, -2.0 / 3.0);
}

// (a,b) 对的 Pearson 相关（无散布 NaN）。
double correlation(const std::vector<std::array<double, 2>>& p) {
    double n = static_cast<double>(p.size());
    if (n == 0.0) return std::numeric_limits<double>::quiet_NaN();
    double ma = 0.0, mb = 0.0;
    for (const auto& v : p) {
        ma += v[0];
        mb += v[1];
    }
    ma /= n;
    mb /= n;
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (const auto& v : p) {
        sab += (v[0] - ma) * (v[1] - mb);
        saa += (v[0] - ma) * (v[0] - ma);
        sbb += (v[1] - mb) * (v[1] - mb);
    }
    return sab / std::sqrt(saa * sbb);
}

// 线性 ProPhoto → XYZ 的 Y 行（其余行 0），取亮度。
const Mat3 kProToXyzYRow = {{{{0.288040, 0.711874, 0.000086}},
                             {{0.0, 0.0, 0.0}},
                             {{0.0, 0.0, 0.0}}}};

double proLuminance(const Vec3& rgb) {
    return 0.288040 * rgb[0] + 0.711874 * rgb[1] + 0.000086 * rgb[2];
}

// 交替尝试的岭强度。
constexpr double kRidges[3] = {0.01, 0.05, 0.15};
// 色调曲线拟合的分箱数（sqrt(x) 均匀）。
constexpr size_t kCurveBins = 32;

// 白平衡相机 → ProPhoto 的合理矩阵（去掉曝光）：对角/串扰适度、行和 ~1
//（中性保持近中性）、非镜像未塌缩。
bool plausible(const Mat3& rel) {
    for (int i = 0; i < 3; ++i) {
        double sum = 0.0;
        for (int j = 0; j < 3; ++j) {
            double v = rel[i][j];
            sum += v;
            if (i == j) {
                if (!(v >= 0.2 && v <= 3.0)) return false;
            } else if (std::abs(v) > 2.0) {
                return false;
            }
        }
        if (!(sum >= 0.6 && sum <= 1.6)) return false;
    }
    double det = rel[0][0] * (rel[1][1] * rel[2][2] - rel[1][2] * rel[2][1]) -
                 rel[0][1] * (rel[1][0] * rel[2][2] - rel[1][2] * rel[2][0]) +
                 rel[0][2] * (rel[1][0] * rel[2][1] - rel[1][1] * rel[2][0]);
    return det > 0.05;
}

// 加权分箱（sqrt 域）+ 池相邻违约者单调化拟合色调曲线。
std::optional<ToneCurve> fitCurve(const std::vector<Sample>& samples, const std::vector<double>& robust,
                                  const Mat3& m) {
    std::vector<std::array<double, 3>> bins(kCurveBins, {0.0, 0.0, 0.0});  // w, Σw·sqrt x, Σw·sqrt y
    double total = 0.0;
    for (size_t si = 0; si < samples.size() && si < robust.size(); ++si) {
        const Sample& s = samples[si];
        Vec3 x = matApply(m, s.cam);
        double w = s.weight * robust[si];
        if (w <= 0.0) continue;
        for (int k = 0; k < 3; ++k) {
            double xv = x[k];
            double yv = s.rgb[k];
            if (!(xv > 1e-5 && xv < 1.0 && std::isfinite(yv))) continue;
            double sx = std::sqrt(xv);
            double sy = std::sqrt(std::clamp(yv, 0.0, 1.0));
            size_t b = std::min(static_cast<size_t>(sx * static_cast<double>(kCurveBins)),
                                kCurveBins - 1);
            bins[b][0] += w;
            bins[b][1] += w * sx;
            bins[b][2] += w * sy;
            total += w;
        }
    }
    if (std::isnan(total) || total <= 0.0) return std::nullopt;
    // 有货分箱的 (weight, sqrt x, sqrt y)，随后对 y 做 PAVA。
    std::vector<std::array<double, 3>> pts;
    for (const auto& b : bins) {
        if (b[0] > total * 2e-4) pts.push_back({b[0], b[1] / b[0], b[2] / b[0]});
    }
    if (pts.size() < 4) return std::nullopt;
    std::vector<std::array<double, 3>> blocks;  // (weight, mean y, count)
    for (const auto& p : pts) {
        blocks.push_back({p[0], p[2], 1.0});
        while (blocks.size() >= 2) {
            const auto& b = blocks[blocks.size() - 1];
            const auto& a = blocks[blocks.size() - 2];
            if (a[1] <= b[1]) break;
            double w = a[0] + b[0];
            double y = (a[0] * a[1] + b[0] * b[1]) / w;
            size_t n = static_cast<size_t>(a[2] + b[2]);
            blocks.resize(blocks.size() - 2);
            blocks.push_back({w, y, static_cast<double>(n)});
        }
    }
    size_t i = 0;
    for (const auto& blk : blocks) {
        size_t n = static_cast<size_t>(blk[2]);
        for (size_t q = i; q < i + n && q < pts.size(); ++q) {
            pts[q][2] = blk[1];
        }
        i += n;
    }
    std::vector<std::array<double, 2>> knots{{0.0, 0.0}};
    for (const auto& p : pts) {
        double x = p[1] * p[1];
        double y = std::clamp(p[2] * p[2], 0.0, 1.0);
        const auto& last = knots.back();
        if (x > last[0] + 1e-6 && x < 1.0 - 1e-6) {
            knots.push_back({x, std::max(y, last[1])});
        }
    }
    knots.push_back({1.0, 1.0});
    return ToneCurve::create(knots);
}

// f⁻¹(rgb) 对 cam 的加权最小二乘求 M，向 prior 岭拉近，再按「平衡白保持
// 亮度 gain」缩放（矩阵与曲线否则会交换曝光）。
Mat3 fitMatrix(const std::vector<Sample>& samples, const std::vector<double>& robust,
               const ToneCurve& tone, const Mat3& prior, double gain, double ridge) {
    Mat3 out = prior;
    for (int i = 0; i < 3; ++i) {
        double a[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        double b[3] = {0, 0, 0};
        double level = 0.0;
        for (size_t si = 0; si < samples.size() && si < robust.size(); ++si) {
            const Sample& s = samples[si];
            const Vec3& c = s.cam;
            double lv = (c[0] + c[1] + c[2]) / 3.0;
            lv *= lv;
            double y = s.rgb[i];
            if (!(y > 0.0 && y < 0.995)) continue;
            auto [t, slope] = tone.inverseAndSlope(y);
            double q = slope * s.perceptual;
            double w = s.weight * robust[si] * q * q;
            if (!(std::isfinite(w) && w > 0.0)) continue;
            level += w * lv;
            for (int j = 0; j < 3; ++j) {
                for (int k = 0; k < 3; ++k) {
                    a[j][k] += w * c[j] * c[k];
                }
                b[j] += w * c[j] * t;
            }
        }
        double lambda = ridge * ridge * level + 1e-12;
        for (int j = 0; j < 3; ++j) {
            a[j][j] += lambda;
            b[j] += lambda * prior[i][j];
        }
        Mat3 am{};
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) am[j][k] = a[j][k];
        }
        if (auto inv = matInvert(am)) {
            Vec3 x = matApply(*inv, {b[0], b[1], b[2]});
            if (std::all_of(x.begin(), x.end(), [](double v) { return std::isfinite(v); })) {
                for (int j = 0; j < 3; ++j) out[i][j] = x[j];
            }
        }
    }
    Vec3 white = matApply(out, {1.0, 1.0, 1.0});
    double whiteY = proLuminance(white);
    if (std::isfinite(whiteY) && whiteY > 1e-6) {
        return matScale(out, gain / whiteY);
    }
    return prior;
}

// 各样本 L* 标度残差的 Tukey biweight。
void updateRobust(const std::vector<Sample>& samples, std::vector<double>& robust, const Mat3& m,
                  const ToneCurve& tone) {
    std::vector<double> err(samples.size(), 0.0);
    for (size_t si = 0; si < samples.size(); ++si) {
        const Sample& s = samples[si];
        Vec3 o = renderLook(m, &tone, s.cam);
        double e = 0.0;
        for (int k = 0; k < 3; ++k) e += (o[k] - s.rgb[k]) * (o[k] - s.rgb[k]);
        err[si] = std::sqrt(e) * s.perceptual;
    }
    std::vector<double> sorted;
    for (double v : err) {
        if (std::isfinite(v)) sorted.push_back(v);
    }
    if (sorted.empty()) return;
    std::sort(sorted.begin(), sorted.end());
    double median = sorted[sorted.size() / 2];
    double c = 4.685 * std::max(1.4826 * median, 0.5);
    for (size_t si = 0; si < robust.size() && si < err.size(); ++si) {
        double u = err[si] / c;
        robust[si] = (std::isfinite(u) && u < 1.0) ? std::pow(1.0 - u * u, 2) : 0.0;
    }
}

// 色调曲线与矩阵交替拟合 + Tukey 重加权（6 轮）。
std::optional<std::pair<Mat3, ToneCurve>> fitModel(const std::vector<Sample>& train,
                                                   const Mat3& fallback, double gain, double ridge) {
    Mat3 m = fallback;
    std::vector<double> robust(train.size(), 1.0);
    auto tone = fitCurve(train, robust, m);
    if (!tone) return std::nullopt;
    for (int it = 0; it < 6; ++it) {
        m = fitMatrix(train, robust, *tone, fallback, gain, ridge);
        tone = fitCurve(train, robust, m);
        if (!tone) return std::nullopt;
        updateRobust(train, robust, m, *tone);
    }
    return std::make_pair(m, *tone);
}

// ---- 拟合网格 ----

// 均值线性 sRGB + 剪裁比例（任一通道 ≥250 的像素占比）+ 边缘权重。
std::optional<std::tuple<Vec3, double, double>> referenceCell(const LookReference& r,
                                                              const std::vector<double>& lut,
                                                              size_t x0, size_t x1, size_t y0,
                                                              size_t y1) {
    size_t rw = r.width;
    double sum[3] = {0, 0, 0};
    double l1 = 0.0, l2 = 0.0;
    size_t n = 0, clipped = 0;
    for (size_t y = y0; y < y1; ++y) {
        for (size_t x = x0; x < x1; ++x) {
            size_t idx = (y * rw + x) * 3;
            if (idx + 2 >= r.rgb.size()) return std::nullopt;
            double v[3];
            bool clip = false;
            for (int k = 0; k < 3; ++k) {
                uint8_t byte = r.rgb[idx + k];
                v[k] = byte < lut.size() ? lut[byte] : 0.0;
                sum[k] += v[k];
                clip = clip || byte >= 250;
            }
            if (clip) ++clipped;
            double l = 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
            l1 += l;
            l2 += l * l;
            ++n;
        }
    }
    if (n == 0) return std::nullopt;
    double nf = static_cast<double>(n);
    double mean = l1 / nf;
    double sd = std::sqrt(std::max(l2 / nf - mean * mean, 0.0));
    double cv = sd / (mean + 0.01);
    Vec3 meanRgb{sum[0] / nf, sum[1] / nf, sum[2] / nf};
    double clipFrac = static_cast<double>(clipped) / nf;
    double edge = 1.0 / (1.0 + std::pow(cv / 0.15, 2));
    return std::make_tuple(meanRgb, clipFrac, edge);
}

}  // namespace

// ---- ToneCurve ----

std::optional<ToneCurve> ToneCurve::create(const std::vector<std::array<double, 2>>& knots) {
    if (knots.size() < 2 || knots.size() > kMaxKnots) return std::nullopt;
    auto in01 = [](double v) { return std::isfinite(v) && v >= 0.0 && v <= 1.0; };
    for (const auto& k : knots) {
        if (!in01(k[0]) || !in01(k[1])) return std::nullopt;
    }
    for (size_t i = 1; i < knots.size(); ++i) {
        if (!(knots[i][0] > knots[i - 1][0] && knots[i][1] >= knots[i - 1][1])) return std::nullopt;
    }
    if (knots.front()[0] != 0.0 || knots.front()[1] != 0.0) return std::nullopt;
    if (knots.back()[0] != 1.0 || knots.back()[1] != 1.0) return std::nullopt;
    ToneCurve t;
    t.k_.reserve(knots.size());
    for (const auto& kn : knots) {
        t.k_.push_back({std::sqrt(kn[0]), std::sqrt(kn[1])});
    }
    return t;
}

std::vector<std::array<float, 2>> ToneCurve::knots() const {
    std::vector<std::array<float, 2>> out;
    out.reserve(k_.size());
    for (const auto& kn : k_) {
        out.push_back({static_cast<float>(kn[0] * kn[0]), static_cast<float>(kn[1] * kn[1])});
    }
    return out;
}

double ToneCurve::eval(double x) const {
    double t = std::isnan(x) ? 0.0 : std::sqrt(std::clamp(x, 0.0, 1.0));
    double s = interp(k_, t, 0, 1);
    return s * s;
}

std::pair<double, double> ToneCurve::inverseAndSlope(double y) const {
    double t = std::isnan(y) ? 0.0 : std::sqrt(std::clamp(y, 0.0, 1.0));
    const auto& k = k_;
    if (k.size() < 2) return {y, 1.0};
    size_t lo = 0, hi = k.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (k[mid][1] < t) lo = mid + 1;
        else hi = mid;
    }
    size_t i = std::clamp<size_t>(lo, 1, std::max<size_t>(k.size() - 1, 1));
    const auto& a = k[i - 1];
    const auto& b = k[i];
    double dx = b[0] - a[0];
    double dy = b[1] - a[1];
    if (dy <= 0.0 || dx <= 0.0) {
        return {a[0] * a[0], 0.0};
    }
    double sx = a[0] + dx * std::clamp((t - a[1]) / dy, 0.0, 1.0);
    double slope = dy / dx;
    double d = sx > 1e-9 ? slope * t / sx : slope * slope;
    return {sx * sx, d};
}

// ---- CameraLook ----

bool CameraLook::isUsable() const {
    for (const auto& row : toXyz) {
        for (double v : row) {
            if (!std::isfinite(v) || std::abs(v) >= 64.0) return false;
        }
    }
    if (!matInvert(toXyz)) return false;
    return matApply(toXyz, {1.0, 1.0, 1.0})[1] > 1e-3;
}

std::string CameraLook::note() const {
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "colour fitted to the camera's embedded JPEG (mean dE %.1f against it; "
                  "%.1f with the neutral fallback)",
                  deltaE, deltaEFallback);
    return buf;
}

// ---- fit_look ----

std::optional<CameraLook> fitLook(const Sensor& s, const DevelopOptions& opts,
                                  const LookReference& reference, std::string& why) {
    // 白平衡 + Scale 由显影侧统一导出（与 developSensor 同一来源）。
    std::vector<std::string> warnings;
    Vec3 neutral;
    Vec3 mult{1.0, 1.0, 1.0};
    if (!lookWhiteBalance(s, opts, neutral, warnings)) {
        why = warnings.empty() ? "white balance unavailable" : warnings.back();
        return std::nullopt;
    }
    for (int i = 0; i < 3; ++i) mult[i] = 1.0 / neutral[i];
    auto valueAt = [&s](size_t x, size_t y, size_t sample) {
        return lookSampleValue(s, x, y, sample);
    };

    Rect c = s.crop;
    bool inData = c.x + c.width <= s.width && c.y + c.height <= s.height;
    size_t need = s.width * s.height * s.samples;
    if (c.empty() || !inData || s.data.size() < need) {
        why = "the crop lies outside the sensor data";
        return std::nullopt;
    }
    if (!(s.samples == 3 || (s.samples == 1 && s.cfa))) {
        why = "unsupported sensor layout";
        return std::nullopt;
    }
    size_t rw = reference.width;
    size_t rh = reference.height;
    if (rw < 32 || rh < 32 || rw * rh * 3 != reference.rgb.size()) {
        why = "the reference image is too small or its buffer does not match its size";
        return std::nullopt;
    }
    bool swap = s.orientation >= 5 && s.orientation <= 8;
    size_t uw = swap ? c.height : c.width;
    size_t uh = swap ? c.width : c.height;
    double aspect = (static_cast<double>(rw) / rh) / (static_cast<double>(uw) / uh);
    if (!(std::isfinite(aspect) && std::abs(std::log(aspect)) <= 0.03)) {
        why = "the reference does not have the raw frame's shape";
        return std::nullopt;
    }
    // 单元至少 4 传感器像素、2 参考像素宽。
    size_t longEdge = std::max(uw, uh);
    size_t edge = std::min(std::min(kProxyEdge, longEdge / 4), std::max(rw, rh) / 2);
    auto ceilDiv = [](size_t a, size_t b) { return (a + b - 1) / b; };
    size_t gwUp = std::max<size_t>(uw * edge / longEdge + (uw * edge % longEdge ? 1 : 0), 1);
    size_t ghUp = std::max<size_t>(uh * edge / longEdge + (uh * edge % longEdge ? 1 : 0), 1);
    gwUp = std::max(ceilDiv(uw * edge, longEdge), size_t{1});
    ghUp = std::max(ceilDiv(uh * edge, longEdge), size_t{1});
    if (gwUp < 16 || ghUp < 16) {
        why = "the image is too small to fit";
        return std::nullopt;
    }

    // 传感器朝向网格。
    size_t gw = swap ? ghUp : gwUp;
    size_t gh = swap ? gwUp : ghUp;
    auto cellStep = [](size_t a, size_t b) {
        return std::max<size_t>((b >= a ? (b - a + 15) / 16 : 1), 1) * 2;
    };
    std::vector<std::optional<std::pair<Vec3, bool>>> sensorCells(gw * gh);
    for (size_t j = 0; j < gh; ++j) {
        size_t y0 = c.y + j * c.height / gh;
        size_t y1 = c.y + (j + 1) * c.height / gh;
        size_t sy = cellStep(y0, y1);
        for (size_t i = 0; i < gw; ++i) {
            size_t x0 = c.x + i * c.width / gw;
            size_t x1 = c.x + (i + 1) * c.width / gw;
            size_t sx = cellStep(x0, x1);
            double sum[3] = {0, 0, 0};
            uint32_t n[3] = {0, 0, 0};
            bool clipped = false;
            auto add = [&](size_t ch, float v) {
                double dv = static_cast<double>(v);
                double b = dv * (ch < 3 ? mult[ch] : 1.0);
                clipped = clipped || dv >= 0.98 || b >= 0.98;
                sum[ch] += std::clamp(b, 0.0, 1.0);
                ++n[ch];
            };
            for (size_t y = y0; y < y1; y += sy) {
                for (size_t x = x0; x < x1; x += sx) {
                    static const int dxs[4] = {0, 1, 0, 1};
                    static const int dys[4] = {0, 0, 1, 1};
                    for (int q = 0; q < 4; ++q) {
                        size_t px = x + dxs[q];
                        size_t py = y + dys[q];
                        if (px >= x1 || py >= y1) continue;
                        if (s.cfa && s.samples == 1) {
                            add(std::min<size_t>(s.cfa->color(px, py), 2),
                                valueAt(px, py, 0));
                        } else {
                            for (int k = 0; k < 3; ++k) add(k, valueAt(px, py, k));
                        }
                    }
                }
            }
            if (n[0] > 0 && n[1] > 0 && n[2] > 0) {
                sensorCells[j * gw + i] =
                    std::make_pair(Vec3{sum[0] / n[0], sum[1] / n[1], sum[2] / n[2]}, clipped);
            } else {
                sensorCells[j * gw + i] = std::nullopt;
            }
        }
    }

    // 参考网格（直立），与传感器单元配对。
    std::vector<double> lut(256);
    for (int v = 0; v < 256; ++v) {
        lut[v] = srgbToLinear(v / 255.0);
    }
    Mat3 toPro = matMul(xyzD50ToProphoto(), srgbToXyzD50());
    Mat3 proToXyz = rgbToXyz(kRommPrimaries, kD50Xy);
    std::vector<Cell> cells;
    cells.reserve(gwUp * ghUp);
    for (size_t j = 0; j < ghUp; ++j) {
        size_t y0 = j * rh / ghUp;
        size_t y1 = std::max((j + 1) * rh / ghUp, y0 + 1);
        for (size_t i = 0; i < gwUp; ++i) {
            size_t x0 = i * rw / gwUp;
            size_t x1 = std::max((i + 1) * rw / gwUp, x0 + 1);
            auto rc = referenceCell(reference, lut, x0, std::min(x1, rw), y0, std::min(y1, rh));
            size_t sx, sy;
            sourceXY(s.orientation, gw, gh, i, j, sx, sy);
            auto sc = sx < gw && sy < gh ? sensorCells[sy * gw + sx] : std::nullopt;
            Cell cell;
            cell.camClipped = true;
            if (sc.has_value() && rc.has_value()) {
                cell.cam = sc->first;
                cell.camClipped = sc->second;
                cell.rgb = matApply(toPro, std::get<0>(*rc));
                cell.lum = matApply(proToXyz, cell.rgb)[1];
                cell.refClipped = std::get<1>(*rc);
                cell.edgeWeight = std::get<2>(*rc);
            }
            cells.push_back(cell);
        }
    }

    // ---- 拟合 ----
    double gain = std::pow(2.0, std::clamp(std::isfinite(s.baselineExposure) ? s.baselineExposure : 0.0,
                                           -10.0, 10.0));
    Mat3 fallback = matScale(matMul(xyzD50ToProphoto(), srgbToXyzD50()), gain);
    Lab lab;

    std::vector<Sample> train, test, all;
    for (size_t k = 0; k < cells.size(); ++k) {
        const Cell& cl = cells[k];
        bool finite = true;
        for (int q = 0; q < 3; ++q) {
            finite = finite && std::isfinite(cl.cam[q]) && std::isfinite(cl.rgb[q]);
        }
        if (!finite) continue;
        Sample sm;
        sm.cam = cl.cam;
        sm.rgb = cl.rgb;
        sm.lum = cl.lum;
        sm.perceptual = perceptual(cl.lum);
        sm.weight = cl.edgeWeight;
        all.push_back(sm);
        bool usable = !cl.camClipped && cl.refClipped < 0.01 && cl.lum > 0.002 &&
                      cl.cam[0] > 1e-4 && cl.cam[1] > 1e-4 && cl.cam[2] > 1e-4 && cl.edgeWeight > 0.05;
        if (!usable) continue;
        size_t x = k % std::max(gwUp, size_t{1});
        size_t y = k / std::max(gwUp, size_t{1});
        if ((x + 2 * y) % 3 == 0) {
            test.push_back(sm);
        } else {
            train.push_back(sm);
        }
    }
    if (train.size() < 300 || test.size() < 100) {
        why = "too few usable cells";
        return std::nullopt;
    }

    // 参考必须展示同一画面（亮度跟随 raw）且是彩色的。
    {
        std::vector<std::array<double, 2>> pairs;
        for (const Sample& sm : train) {
            Vec3 fb = matApply(fallback, sm.cam);
            pairs.push_back({std::sqrt(std::max(proLuminance(fb), 0.0)),
                             std::sqrt(std::max(sm.lum, 0.0))});
        }
        for (const Sample& sm : test) {
            Vec3 fb = matApply(fallback, sm.cam);
            pairs.push_back({std::sqrt(std::max(proLuminance(fb), 0.0)),
                             std::sqrt(std::max(sm.lum, 0.0))});
        }
        double r = correlation(pairs);
        if (std::getenv("MONTAGE_LOOK_DEBUG") != nullptr) {
            std::fprintf(stderr, "[look] pairs=%zu r=%.3f train=%zu test=%zu\n", pairs.size(), r,
                         train.size(), test.size());
        }
        if (std::isnan(r) || r < 0.7) {
            why = "the reference does not match the raw picture";
            return std::nullopt;
        }
        size_t coloured = 0;
        for (const Sample& sm : train) {
            auto l = lab.lab(sm.rgb);
            if (std::hypot(l[1], l[2]) > 4.0) ++coloured;
        }
        for (const Sample& sm : test) {
            auto l = lab.lab(sm.rgb);
            if (std::hypot(l[1], l[2]) > 4.0) ++coloured;
        }
        if (coloured * 50 < train.size() + test.size()) {
            why = "the reference is (nearly) colourless";
            return std::nullopt;
        }
    }

    std::vector<const Sample*> testRefs;
    for (const Sample& sm : test) testRefs.push_back(&sm);
    double heldFallback = meanDeltaE(lab, testRefs, fallback, nullptr);
    std::string lastWhy = "no look fitted";
    bool found = false;
    Mat3 m = fallback;
    Mat3 rel = fallback;
    ToneCurve toneOut;
    for (double ridge : kRidges) {
        auto fitted = fitModel(train, fallback, gain, ridge);
        if (!fitted) {
            lastWhy = "no tone curve could be fitted";
            continue;
        }
        Mat3 relM = matScale(fitted->first, 1.0 / gain);
        if (!plausible(relM)) {
            lastWhy = "implausible matrix";
            continue;
        }
        std::vector<const Sample*> empty;
        double heldFit = meanDeltaE(lab, testRefs, fitted->first, &fitted->second);
        if (std::isnan(heldFit) || heldFit > 0.85 * heldFallback) {
            lastWhy = "the fit does not improve enough on the neutral fallback";
            continue;
        }
        m = fitted->first;
        rel = relM;
        toneOut = fitted->second;
        found = true;
        break;
    }
    if (!found) {
        why = lastWhy;
        return std::nullopt;
    }
    std::vector<const Sample*> allRefs;
    for (const Sample& sm : all) allRefs.push_back(&sm);
    CameraLook look;
    look.toXyz = matMul(rgbToXyz(kRommPrimaries, kD50Xy), rel);
    look.tone = toneOut;
    look.deltaE = meanDeltaE(lab, allRefs, m, &toneOut);
    look.deltaEFallback = meanDeltaE(lab, allRefs, fallback, nullptr);
    if (!look.isUsable()) {
        why = "the fitted matrix is not usable";
        return std::nullopt;
    }
    return look;
}

}  // namespace raw
}  // namespace montage
