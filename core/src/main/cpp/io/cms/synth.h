#ifndef MONTAGE_IO_CMS_SYNTH_H
#define MONTAGE_IO_CMS_SYNTH_H

// M22 色彩管理内核 · 合成 CMYK 输出 profile。
// Adobe 的 CMYK profile（U.S. Web Coated SWOP、Coated FOGRA39…）是专有的，可自由下载的
// 特征化 profile 带再分发条件，故内置一份自打印机模型程序生成的 CMYK profile：
// * 正向模型（CMYK→XYZ）：Yule–Nielsen 修正 Neugebauer（n=2）覆盖 16 个墨叠印，
//   Demichel 覆盖率权重 + 每墨抛物线网点增大（dot gain）。无 K 的八个原色用公开
//   ISO 12647-2 涂布纸 Lab 目标值（纸、C、M、Y、三个双色叠印）+ 估计的三色黑；
//   带 K 的叠印以首表面反射下限乘法推导。
// * AToB（全部 intent）：正向模型的 11^4 lut16 表，相对色度（纸 → L*100）。
// * B2A1（色度）：每个 Lab 节点先做灰成分（GCR）黑生成，再带总墨限的有界最小二乘
//   求 C/M/Y；域外颜色落在最近可达色（裁剪）。
// * B2A0/B2A2（感知/饱和）：目标先压入色域——L* 缩放到设备黑、 chroma 对色域边界描述子
//   软压缩——再同上求解。
// 翻译自 photocraft crates/cms/src/synth.rs（MIT OR Apache-2.0）。
// 模型对涂布胶印是「合理近似」而非实测特征化：生产打印请用印厂提供的真实 profile。

#include <array>
#include <string>

#include "io/cms/profile.h"

namespace montage {
namespace cms {

inline constexpr const char* kSynthDescription =
    "Photocraft Coated CMYK (synthetic, 300% TAC, medium GCR)";

// 合成 profile 参数。
struct CmykParams {
    std::string description = kSynthDescription;
    double tac = 3.0;            // 总面积覆盖上限（3.0 = 300%）
    double gcrStart = 0.2;       // 黑生成的灰成分起点（无 K 解的 min(C,M,Y)）
    double kMax = 0.95;          // 最大黑
    double gcrGamma = 1.1;       // gcrStart 之上黑生成曲线的指数
    double tvi[4] = {0.14, 0.14, 0.14, 0.17};  // C/M/Y/K 在 50% 处的网点增大
    size_t gridA2b = 11;         // AToB（4D）与 BToA（3D）表格点数
    size_t gridB2a = 21;
};

// 打印机模型。
struct CmykModel {
    // 16 个叠印的绝对 XYZ（D50），墨位掩码索引（C=1, M=2, Y=4, K=8），
    // 存 X^(1/n) 供 Yule–Nielsen 求和。
    std::array<std::array<double, 3>, 16> primN{};
    std::array<double, 3> paper{};  // 纸的绝对 XYZ
    double tvi[4] = {0, 0, 0, 0};

    static CmykModel coated(const double (&tvi)[4]);

    // 墨组合的绝对 XYZ（每个覆盖率 ∈ [0,1]）。
    std::array<double, 3> xyz(const std::array<double, 4>& cmyk) const;

    // ICC 相对 Lab（纸 = L*100）。
    std::array<double, 3> lab(const std::array<double, 4>& cmyk) const;
};

// 相对 Lab 目标的色度分离：GCR 黑生成 + CMY 求解。返回 (CMYK, ΔE76)。
std::pair<std::array<double, 4>, double> synthSeparate(const CmykModel& model, const CmykParams& p,
                                                       const std::array<double, 3>& target,
                                                       const std::array<double, 4>* warm);

// 生成合成 CMYK profile。
Profile synthCmykProfile(const CmykParams& p);

// 默认涂布纸 profile（内嵌 kCoatedCmykIcc 字节的生成来源）。
Profile synthCoatedCmyk();

}  // namespace cms
}  // namespace montage

#endif  // MONTAGE_IO_CMS_SYNTH_H
