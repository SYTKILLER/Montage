#ifndef MONTAGE_IO_RAW_RAW_COLOR_H
#define MONTAGE_IO_RAW_RAW_COLOR_H

// M23a RAW 显影颜色科学：3×3 矩阵助手、色度转换、相关色温、Bradford 适应与
// DNG 规范（第 6 章）的相机 → XYZ(D50) → 输出空间映射。
// 翻译自 photocraft crates/raw/src/color.rs（MIT OR Apache-2.0）。

#include <array>
#include <optional>

#include "io/raw/raw_types.h"

namespace montage {
namespace raw {

Mat3 matMul(const Mat3& a, const Mat3& b);
std::array<double, 3> matApply(const Mat3& m, const std::array<double, 3>& v);
Mat3 matDiag(const std::array<double, 3>& v);
Mat3 matScale(const Mat3& m, double k);
// 逆矩阵；(近)奇异或非有限返回空。
std::optional<Mat3> matInvert(const Mat3& m);
// 9 值行主序 3×3。
std::optional<Mat3> matFromSlice(const double* v);

std::array<double, 3> xyToXyz(const std::array<double, 2>& xy);
std::optional<std::array<double, 2>> xyzToXy(const std::array<double, 3>& v);

// 色度的相关色温（McCamy 1992）；钳制到 2000–50000 K。
double cct(const std::array<double, 2>& xy);
// EXIF LightSource 码的近似温度；未知返回空。
std::optional<double> illuminantTemperature(uint32_t code);

// 从白 `src`（XYZ）到白 `dst` 的 Bradford 色适应。
Mat3 bradford(const std::array<double, 3>& src, const std::array<double, 3>& dst);

// 基色与白点（全部为色度）的 RGB → XYZ 矩阵，白映射到 Y = 1。
Mat3 rgbToXyz(const std::array<std::array<double, 2>, 3>& prim, const std::array<double, 2>& white);

// ROMM RGB（ProPhoto）基色，ISO 22028-2；白 D50。
extern const std::array<std::array<double, 2>, 3> kRommPrimaries;
// sRGB / Rec. 709 基色。
extern const std::array<std::array<double, 2>, 3> kRec709Primaries;
extern const std::array<double, 2> kD50Xy;
extern const std::array<double, 2> kD65Xy;
extern const std::array<double, 3> kD50White;

// XYZ (D50) → 线性 ProPhoto RGB。
Mat3 xyzD50ToProphoto();
// 线性 sRGB (D65) → 适应到 D50 的 XYZ：文件无颜色校准时的记录中性矩阵。
Mat3 srgbToXyzD50();
// 线性 ProPhoto (D50) → 线性 sRGB (D65)：Montage 发布链用（ProPhoto 显影 → TileGrid）。
Mat3 prophotoD50ToSrgb();

// ColorInfo 的温度相关求解（DNG 规范第 6 章）。
struct ColorSolve {
    Mat3 xyzToCam = kIdentityMat3;
    std::optional<Mat3> forwardMatrix;
    Mat3 abcc = kIdentityMat3;  // AB · CC
};

// ColorInfo 的方法（Rust impl 块直译）。
double colorWeight(const ColorInfo& info, double t);
std::optional<ColorSolve> colorAt(const ColorInfo& info, double t);
std::array<double, 2> neutralToXy(const ColorInfo& info, const std::array<double, 3>& neutral);
std::optional<std::array<double, 3>> xyToNeutral(const ColorInfo& info, const std::array<double, 2>& xy);
// 白平衡相机值（相机值除以 neutral/max(neutral)）到 XYZ D50 的矩阵，缩放使平衡白
// (1,1,1) 映射到 Y = 1 的 D50。无校准返回空。
std::optional<Mat3> balancedToXyzD50(const ColorInfo& info, const std::array<double, 3>& neutral);

}  // namespace raw
}  // namespace montage

#endif  // MONTAGE_IO_RAW_RAW_COLOR_H
