#ifndef MONTAGE_IO_CMS_CMS_MATH_H
#define MONTAGE_IO_CMS_CMS_MATH_H

// M22 色彩管理内核 · 基础数学：3×3 矩阵、CIE XYZ↔L*a*b*、Bradford 色适应、ΔE76。
// 翻译自 photocraft crates/cms/src/math.rs（MIT OR Apache-2.0）。
// 公式来源：CIE 15（L*a*b*）、ICC.1:2010 Annex A（PCS D50）、Lam 1985（Bradford）。

#include <array>
#include <cmath>
#include <optional>

namespace montage {
namespace cms {

// ICC PCS 光源（D50），即 ICC 头部 s15Fixed16 编码的值：X=0.9642, Y=1.0, Z=0.8249。
inline constexpr double kD50Arr[3] = {0.9642, 1.0, 0.8249};
inline constexpr std::array<double, 3> kD50 = {0.9642, 1.0, 0.8249};

// CIE D65 白点色度（0.3127, 0.3290）——sRGB / Display P3 / Adobe RGB 兼容空间所用。
inline constexpr std::array<double, 2> kD65Xy = {0.3127, 0.3290};

using Mat3 = std::array<std::array<double, 3>, 3>;

inline constexpr Mat3 kIdentityMat3 = {
    {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};

inline Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 o{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            o[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        }
    }
    return o;
}

inline std::array<double, 3> apply(const Mat3& m, const std::array<double, 3>& v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

inline double det(const Mat3& m) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

// 逆矩阵；(近)奇异返回空。
inline std::optional<Mat3> invert(const Mat3& m) {
    const double d = det(m);
    if (!std::isfinite(d) || std::abs(d) < 1e-12) {
        return std::nullopt;
    }
    const double inv = 1.0 / d;
    return Mat3{{
        {{(m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv,
          (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv}},
        {{(m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv,
          (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv}},
        {{(m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv,
          (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv}},
    }};
}

// 色度 (x, y) 的 XYZ（Y = 1）。
inline std::array<double, 3> xyToXyz(const std::array<double, 2>& xy) {
    const double x = xy[0], y = xy[1];
    return {x / y, 1.0, (1.0 - x - y) / y};
}

// Bradford 锥响应矩阵（Lam 1985）。
inline constexpr Mat3 kBradford = {
    {{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};

// 白 src → 白 dst（均为 XYZ）的 Bradford 色适应矩阵。
inline Mat3 bradford(const std::array<double, 3>& src, const std::array<double, 3>& dst) {
    const Mat3 inv = invert(kBradford).value_or(kIdentityMat3);  // 常数矩阵恒可逆
    const auto s = apply(kBradford, src);
    const auto d = apply(kBradford, dst);
    const Mat3 scale = {{{d[0] / s[0], 0.0, 0.0}, {0.0, d[1] / s[1], 0.0}, {0.0, 0.0, d[2] / s[2]}}};
    return mul(inv, mul(scale, kBradford));
}

// 色度基色 + 白点（Y = 1）的 RGB → XYZ 矩阵；基色线性相关返回空。
inline std::optional<Mat3> rgbToXyzMatrix(const std::array<double, 2>& r, const std::array<double, 2>& g,
                                          const std::array<double, 2>& b, const std::array<double, 3>& white) {
    const auto xr = xyToXyz(r), xg = xyToXyz(g), xb = xyToXyz(b);
    const Mat3 m = {{{xr[0], xg[0], xb[0]}, {xr[1], xg[1], xb[1]}, {xr[2], xg[2], xb[2]}}};
    const auto inv = invert(m);
    if (!inv) {
        return std::nullopt;
    }
    const auto s = apply(*inv, white);
    return Mat3{{{m[0][0] * s[0], m[0][1] * s[1], m[0][2] * s[2]},
                 {m[1][0] * s[0], m[1][1] * s[1], m[1][2] * s[2]},
                 {m[2][0] * s[0], m[2][1] * s[1], m[2][2] * s[2]}}};
}

inline constexpr double kLabEps = 216.0 / 24389.0;   // (6/29)^3
inline constexpr double kLabKappa = 24389.0 / 27.0;  // (29/3)^3

inline double labF(double t) {
    return t > kLabEps ? std::cbrt(t) : (kLabKappa * t + 16.0) / 116.0;
}

inline double labFinv(double f) {
    const double f3 = f * f * f;
    return f3 > kLabEps ? f3 : (116.0 * f - 16.0) / kLabKappa;
}

// CIE XYZ → L*a*b*（相对 white）。
inline std::array<double, 3> xyzToLab(const std::array<double, 3>& xyz, const std::array<double, 3>& white) {
    const double fx = labF(xyz[0] / white[0]);
    const double fy = labF(xyz[1] / white[1]);
    const double fz = labF(xyz[2] / white[2]);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

// CIE L*a*b* → XYZ（相对 white）。
inline std::array<double, 3> labToXyz(const std::array<double, 3>& lab, const std::array<double, 3>& white) {
    const double fy = (lab[0] + 16.0) / 116.0;
    const double fx = fy + lab[1] / 500.0;
    const double fz = fy - lab[2] / 200.0;
    return {labFinv(fx) * white[0], labFinv(fy) * white[1], labFinv(fz) * white[2]};
}

// 热路径 f32 版（仅 D50 PCS）。
inline std::array<float, 3> xyzToLabF32(const std::array<float, 3>& xyz) {
    constexpr float W[3] = {0.9642f, 1.0f, 0.8249f};
    constexpr float E = 216.0f / 24389.0f;
    constexpr float K = 24389.0f / 27.0f;
    auto f = [](float t) { return t > E ? std::cbrt(t) : (K * t + 16.0f) / 116.0f; };
    const float fx = f(xyz[0] / W[0]);
    const float fy = f(xyz[1] / W[1]);
    const float fz = f(xyz[2] / W[2]);
    return {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}

inline std::array<float, 3> labToXyzF32(const std::array<float, 3>& lab) {
    constexpr float W[3] = {0.9642f, 1.0f, 0.8249f};
    constexpr float E = 216.0f / 24389.0f;
    constexpr float K = 24389.0f / 27.0f;
    const float fy = (lab[0] + 16.0f) / 116.0f;
    const float fx = fy + lab[1] / 500.0f;
    const float fz = fy - lab[2] / 200.0f;
    auto finv = [](float f) {
        const float f3 = f * f * f;
        return f3 > E ? f3 : (116.0f * f - 16.0f) / K;
    };
    return {finv(fx) * W[0], finv(fy) * W[1], finv(fz) * W[2]};
}

// CIE 1976 色差。
inline double deltaE76(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    const double d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
    return std::sqrt(d0 * d0 + d1 * d1 + d2 * d2);
}

// ICC s15Fixed16 量化往返（profile 实际可存的精度）。
inline double s15f16Round(double v) {
    return std::round(v * 65536.0) / 65536.0;
}

// ICC 头部存储的精确 D50（s15Fixed16 量化后）。
inline std::array<double, 3> d50Quantized() {
    return {s15f16Round(kD50[0]), s15f16Round(kD50[1]), s15f16Round(kD50[2])};
}

}  // namespace cms
}  // namespace montage

#endif  // MONTAGE_IO_CMS_CMS_MATH_H
