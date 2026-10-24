#ifndef MONTAGE_IO_RAW_RAW_LOOK_H
#define MONTAGE_IO_RAW_RAW_LOOK_H

// M23c：per-file 相机 look（色彩渲染）拟合——无颜色校准的 raw（NEF/ARW/CR2/
// RW2/ORF…，中性回退会渲染得灰平偏绿）用相机自己的内嵌 JPEG 拟合「一个 3×3
// 矩阵 + 一条单调色调曲线」的渲染模型。翻译自 photocraft crates/raw/src/look.rs
// （MIT OR Apache-2.0；方法源自 LightCraft docs/camera-preview-colour.md 的散文
// 描述，photocraft 为针对自身管线的独立实现，本文件为其中文注释直译）。
//
// 模型：y ≈ f(M·c)，c = 白平衡相机 RGB（与 develop 同一白平衡）、y = 内嵌 JPEG
// 的线性 ProPhoto 颜色；f 为逐通道单调色调曲线（过 (0,0) 与 (1,1)）。拟合为
// 交替：f = 分箱（sqrt 域）池相邻违约者单调回归；M = f⁻¹(y) 对 c 的加权最小
// 二乘（权重近似 CIE L* 差、向中性回退岭拉近、Tukey biweight 抗错位边缘）。
// 接受准则：矩阵合理（对角 0.2–3、非对角 ±2、行和 0.6–1.6、行列式 >0.05）且
// 留出集平均 CIE76 ΔE 降到中性回退的 85% 以下。

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "io/raw/raw_types.h"

namespace montage {
namespace raw {

class DevelopOptions;
struct Sensor;

// 拟合网格长边（格数）。
constexpr size_t kProxyEdge = 256;

// 相机渲染：直立交错 8-bit sRGB。
struct LookReference {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb;  // width * height * 3
};

// 线性 0..1 上的单调色调曲线：sqrt 域分段线性，恒过 (0,0) 与 (1,1)。
class ToneCurve {
  public:
    static constexpr size_t kMaxKnots = 256;

    // 线性光节点 (x,y)：2..kMaxKnots 个 0..1 有限点、x 严格递增、y 不减、
    // 首 (0,0) 末 (1,1)；否则空。
    static std::optional<ToneCurve> create(const std::vector<std::array<double, 2>>& knots);

    // 线性光节点表（GammaLut 折叠用）。
    std::vector<std::array<float, 2>> knots() const;

    // f(x)（线性 x 钳到 0..1）。
    double eval(double x) const;
    // (x, f'(x))，x = f⁻¹(y)（平台重复 y 取最低 x）。
    std::pair<double, double> inverseAndSlope(double y) const;

  private:
    std::vector<std::array<double, 2>> k_;  // (sqrt x, sqrt y)
};

// 拟合出的相机渲染：经 DevelopOptions::cameraLook 使用。
struct CameraLook {
    // 白平衡相机 RGB → XYZ(D50)，白映射到 Y=1 的 D50。
    Mat3 toXyz = kIdentityMat3;
    std::optional<ToneCurve> tone;
    // 拟合网格上对相机 JPEG 的平均 CIE76 ΔE（带 look / 中性回退）。
    double deltaE = 0.0;
    double deltaEFallback = 0.0;

    // 有限、有界、可逆、白亮度为正。
    bool isUsable() const;
    // develop 报告的 note。
    std::string note() const;
};

// 拟合失败原因（中性回退保留）。
struct LookRejected {
    std::string why;
};

// 拟合 CameraLook 使 `s` 以 `opts`（白平衡/基线曝光）显影后匹配相机的
// `reference` 渲染。
std::optional<CameraLook> fitLook(const Sensor& s, const DevelopOptions& opts,
                                  const LookReference& reference, std::string& why);

}  // namespace raw
}  // namespace montage

#endif  // MONTAGE_IO_RAW_RAW_LOOK_H
