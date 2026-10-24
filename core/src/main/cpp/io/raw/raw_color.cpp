// M23a RAW 显影颜色科学实现。翻译自 photocraft crates/raw/src/color.rs。

#include "io/raw/raw_color.h"

#include <cmath>

namespace montage {
namespace raw {

namespace {
Mat3 lerpMat(const Mat3& a, const Mat3& b, double g) {
    Mat3 o = a;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o[i][j] = a[i][j] * g + b[i][j] * (1.0 - g);
    return o;
}
}  // namespace

Mat3 matMul(const Mat3& a, const Mat3& b) {
    Mat3 o{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += a[i][k] * b[k][j];
            o[i][j] = s;
        }
    return o;
}

std::array<double, 3> matApply(const Mat3& m, const std::array<double, 3>& v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

Mat3 matDiag(const std::array<double, 3>& v) {
    return {{{{v[0], 0.0, 0.0}}, {{0.0, v[1], 0.0}}, {{0.0, 0.0, v[2]}}}};
}

Mat3 matScale(const Mat3& m, double k) {
    Mat3 o = m;
    for (auto& row : o)
        for (double& v : row) v *= k;
    return o;
}

std::optional<Mat3> matInvert(const Mat3& m) {
    double a = m[0][0], b = m[0][1], c = m[0][2];
    double d = m[1][0], e = m[1][1], f = m[1][2];
    double g = m[2][0], h = m[2][1], i = m[2][2];
    double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return std::nullopt;
    double r = 1.0 / det;
    Mat3 o = {{
        {{(e * i - f * h) * r, (c * h - b * i) * r, (b * f - c * e) * r}},
        {{(f * g - d * i) * r, (a * i - c * g) * r, (c * d - a * f) * r}},
        {{(d * h - e * g) * r, (b * g - a * h) * r, (a * e - b * d) * r}},
    }};
    for (const auto& row : o)
        for (double v : row)
            if (!std::isfinite(v)) return std::nullopt;
    return o;
}

std::optional<Mat3> matFromSlice(const double* v) {
    for (int i = 0; i < 9; ++i)
        if (!std::isfinite(v[i])) return std::nullopt;
    Mat3 o{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) o[i][j] = v[i * 3 + j];
    return o;
}

std::array<double, 3> xyToXyz(const std::array<double, 2>& xy) {
    double y = std::max(xy[1], 1e-6);
    return {xy[0] / y, 1.0, (1.0 - xy[0] - xy[1]) / y};
}

std::optional<std::array<double, 2>> xyzToXy(const std::array<double, 3>& v) {
    double s = v[0] + v[1] + v[2];
    if (!std::isfinite(s) || s <= 1e-9) return std::nullopt;
    return std::array<double, 2>{v[0] / s, v[1] / s};
}

double cct(const std::array<double, 2>& xy) {
    double n = (xy[0] - 0.3320) / (0.1858 - xy[1]);
    double t = 449.0 * n * n * n + 3525.0 * n * n + 6823.3 * n + 5520.33;
    if (!std::isfinite(t)) return 5000.0;
    return std::clamp(t, 2000.0, 50000.0);
}

std::optional<double> illuminantTemperature(uint32_t code) {
    switch (code) {
        case 17: return 2856.0;   // 标准光 A
        case 18: return 4874.0;   // 标准光 B
        case 19: return 6774.0;   // 标准光 C
        case 20: return 5503.0;   // D55
        case 21: return 6504.0;   // D65
        case 22: return 7504.0;   // D75
        case 23: return 5003.0;   // D50
        case 24: return 3200.0;   // ISO 摄影棚钨丝灯
        case 3: return 2850.0;    // 钨丝灯
        case 1: case 4: case 9: return 5500.0;  // 日光、闪光、晴天
        case 10: return 6500.0;   // 多云
        case 11: return 7500.0;   // 阴影
        case 12: return 6430.0;   // 日光荧光
        case 13: return 5000.0;   // 日白荧光
        case 14: return 4150.0;   // 冷白荧光
        case 15: return 3525.0;   // 白荧光
        case 16: return 2925.0;   // 暖白荧光
        case 2: return 4200.0;    // 荧光
        default: return std::nullopt;
    }
}

Mat3 bradford(const std::array<double, 3>& src, const std::array<double, 3>& dst) {
    const Mat3 m = {{{{0.8951, 0.2664, -0.1614}}, {{-0.7502, 1.7135, 0.0367}}, {{0.0389, -0.0685, 1.0296}}}};
    auto mi = matInvert(m);
    if (!mi) return kIdentityMat3;
    auto s = matApply(m, src);
    auto d = matApply(m, dst);
    for (double v : s)
        if (std::abs(v) < 1e-9) return kIdentityMat3;
    return matMul(*mi, matMul(matDiag({d[0] / s[0], d[1] / s[1], d[2] / s[2]}), m));
}

Mat3 rgbToXyz(const std::array<std::array<double, 2>, 3>& prim, const std::array<double, 2>& white) {
    std::array<std::array<double, 3>, 3> cols = {xyToXyz(prim[0]), xyToXyz(prim[1]), xyToXyz(prim[2])};
    Mat3 p{};
    p[0][0] = cols[0][0]; p[0][1] = cols[1][0]; p[0][2] = cols[2][0];
    p[1][0] = cols[0][1]; p[1][1] = cols[1][1]; p[1][2] = cols[2][1];
    p[2][0] = cols[0][2]; p[2][1] = cols[1][2]; p[2][2] = cols[2][2];
    auto pi = matInvert(p);
    if (!pi) return kIdentityMat3;
    auto s = matApply(*pi, xyToXyz(white));
    return matMul(p, matDiag(s));
}

const std::array<std::array<double, 2>, 3> kRommPrimaries = {{{{0.7347, 0.2653}}, {{0.1596, 0.8404}}, {{0.0366, 0.0001}}}};
const std::array<std::array<double, 2>, 3> kRec709Primaries = {{{{0.64, 0.33}}, {{0.30, 0.60}}, {{0.15, 0.06}}}};
const std::array<double, 2> kD50Xy = {0.3457, 0.3585};
const std::array<double, 2> kD65Xy = {0.3127, 0.3290};
const std::array<double, 3> kD50White = {0.9642, 1.0, 0.8249};

Mat3 xyzD50ToProphoto() {
    auto m = matInvert(rgbToXyz(kRommPrimaries, kD50Xy));
    return m ? *m : kIdentityMat3;
}

Mat3 srgbToXyzD50() {
    return matMul(bradford(xyToXyz(kD65Xy), xyToXyz(kD50Xy)), rgbToXyz(kRec709Primaries, kD65Xy));
}

Mat3 prophotoD50ToSrgb() {
    // 线性 ProPhoto (D50) → XYZ D50 → Bradford 适应 → 线性 sRGB (D65)。
    return matMul(bradford(kD50White, xyToXyz(kD65Xy)), rgbToXyz(kRommPrimaries, kD50Xy));
}

double colorWeight(const ColorInfo& info, double t) {
    if (info.calibrations.size() == 2) {
        double t1 = info.calibrations[0].temperature;
        double t2 = info.calibrations[1].temperature;
        if (std::abs(t1 - t2) > 1.0) {
            if (t <= t1) return 1.0;
            if (t >= t2) return 0.0;
            double w = (1.0 / t - 1.0 / t2) / (1.0 / t1 - 1.0 / t2);
            return std::clamp(w, 0.0, 1.0);
        }
    }
    return 1.0;
}

std::optional<ColorSolve> colorAt(const ColorInfo& info, double t) {
    if (info.calibrations.empty()) return std::nullopt;
    const Calibration& first = info.calibrations[0];
    double g = colorWeight(info, t);
    auto pick = [&](const Mat3& a, const std::optional<Mat3>& bOpt) -> Mat3 {
        if (info.calibrations.size() > 1 && bOpt) return lerpMat(a, *bOpt, g);
        return a;
    };
    ColorSolve out;
    out.xyzToCam = pick(first.colorMatrix, info.calibrations.size() > 1 ? std::optional<Mat3>(info.calibrations[1].colorMatrix) : std::nullopt);
    Mat3 cc = pick(first.cameraCalibration, info.calibrations.size() > 1 ? std::optional<Mat3>(info.calibrations[1].cameraCalibration) : std::nullopt);
    if (first.forwardMatrix && info.calibrations.size() > 1 && info.calibrations[1].forwardMatrix) {
        out.forwardMatrix = lerpMat(*first.forwardMatrix, *info.calibrations[1].forwardMatrix, g);
    } else if (first.forwardMatrix) {
        out.forwardMatrix = first.forwardMatrix;
    }
    auto ab = matDiag(info.analogBalance.value_or(std::array<double, 3>{1.0, 1.0, 1.0}));
    out.abcc = matMul(ab, cc);
    return out;
}

std::array<double, 2> neutralToXy(const ColorInfo& info, const std::array<double, 3>& neutral) {
    std::array<double, 2> xy = kD50Xy;
    for (int it = 0; it < 30; ++it) {
        auto solve = colorAt(info, cct(xy));
        if (!solve) return xy;
        auto camToXyz = matInvert(solve->xyzToCam);
        if (!camToXyz) return xy;
        auto next = xyzToXy(matApply(*camToXyz, neutral));
        if (!next) return xy;
        if (!(0.0 <= (*next)[0] && (*next)[0] < 1.0) || !(0.0 <= (*next)[1] && (*next)[1] < 1.0)) return xy;
        bool done = std::abs((*next)[0] - xy[0]) + std::abs((*next)[1] - xy[1]) < 1e-7;
        xy = *next;
        if (done) break;
    }
    return xy;
}

std::optional<std::array<double, 3>> xyToNeutral(const ColorInfo& info, const std::array<double, 2>& xy) {
    auto solve = colorAt(info, cct(xy));
    if (!solve) return std::nullopt;
    auto n = matApply(solve->xyzToCam, xyToXyz(xy));
    double m = std::max({n[0], n[1], n[2]});
    if (!(m > 1e-9)) return std::nullopt;
    for (double v : n)
        if (!std::isfinite(v) || v <= 0.0) return std::nullopt;
    return std::array<double, 3>{n[0] / m, n[1] / m, n[2] / m};
}

std::optional<Mat3> balancedToXyzD50(const ColorInfo& info, const std::array<double, 3>& neutral) {
    double nmax = std::max({neutral[0], neutral[1], neutral[2]});
    if (std::isnan(nmax) || nmax <= 0.0) return std::nullopt;
    for (double v : neutral)
        if (std::isnan(v) || v <= 0.0) return std::nullopt;
    std::array<double, 2> xy = neutralToXy(info, neutral);
    auto solve = colorAt(info, cct(xy));
    if (!solve) return std::nullopt;
    Mat3 unbalance = matDiag({neutral[0] / nmax, neutral[1] / nmax, neutral[2] / nmax});
    Mat3 m = kIdentityMat3;
    if (solve->forwardMatrix) {
        // CameraToXYZ_D50 = FM · D · Inverse(AB · CC)，D = Invert(diag(ReferenceNeutral))。
        auto abccInv = matInvert(solve->abcc);
        if (!abccInv) return std::nullopt;
        auto reference = matApply(*abccInv, neutral);
        for (double v : reference)
            if (std::isnan(v) || v <= 0.0) return std::nullopt;
        Mat3 d = matDiag({1.0 / reference[0], 1.0 / reference[1], 1.0 / reference[2]});
        m = matMul(matMul(*solve->forwardMatrix, matMul(d, *abccInv)), unbalance);
    } else {
        auto camToXyz = matInvert(solve->xyzToCam);
        if (!camToXyz) return std::nullopt;
        Mat3 adapt = bradford(xyToXyz(xy), kD50White);
        m = matMul(matMul(adapt, *camToXyz), unbalance);
    }
    // 归一化：平衡白 → Y = 1。
    double y = matApply(m, {1.0, 1.0, 1.0})[1];
    if (!std::isfinite(y) || y <= 1e-9) return std::nullopt;
    return matScale(m, 1.0 / y);
}

}  // namespace raw
}  // namespace montage
