#ifndef MONTAGE_ENGINE_SEAM_OPS_H
#define MONTAGE_ENGINE_SEAM_OPS_H

// 内容感知比例（M19c，photocraft algo/seam.rs C++ 翻译，MIT OR Apache-2.0）。
// Avidan/Shamir, Seam Carving for Content-Aware Image Resizing, SIGGRAPH 2007：
// 缝 = 8 连通每行一像素的路径，梯度幅值能量图上动态规划求最小；收缩逐条移除最低能量缝
// （能量只重算缝邻域），放大在收缩副本上找 k 条最低缝并按右邻均值复制；高度经转置处理。
// 保护图（0..=1）叠加大幅能量使缝绕开。确定性；大图收缩走「行均值代理」路径
// （缝决策用垂直降采样信号，输出像素保持全分辨率）。
// 宿主测试：.debug/m19/test_seam.cpp——400 例优化实现 vs 朴素参照位级一致 +
// photocraft 输出位 pinned fnv 哈希 8 例直迁。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace montage {
namespace seam {

// 取消谓词：每条缝检查一次；取消返回空向量（结果不完整）。
using CancelFn = std::function<bool()>;

// 梯度幅值能量（论文 e1，前向+后向差分跨通道求和 ×0.5——单像素线也有能量）。
std::vector<float> energyOf(size_t w, size_t h, size_t ch, const std::vector<float>& img);

// 最低能量竖缝：每行一个 x。
std::vector<size_t> findSeam(size_t w, size_t h, const std::vector<float>& energy);

// 宽度方向缝雕刻（收缩移除/放大复制）；protect 可空（w*h，0..=1）。
// 取消返回空向量。
std::vector<float> carveWidth(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                              const std::vector<float>* protect, size_t newW,
                              const CancelFn& cancel = nullptr);

// 内容感知缩放到 newW×newH（先宽后高，高度经转置）。
std::vector<float> carve(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                         const std::vector<float>* protect, size_t newW, size_t newH,
                         const CancelFn& cancel = nullptr);

// 普通双线性缩放（Amount 混合的非感知部分 / 保护图跟随）。
std::vector<float> resizeBilinear(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                                  size_t nw, size_t nh);

// 肤色保护图（RGB 规则，Kovač/Peer/Solina EUROCON 2003；ch<3 返回全 0）。
std::vector<float> skinMask(size_t w, size_t h, size_t ch, const std::vector<float>& img);

}  // namespace seam
}  // namespace montage

#endif  // MONTAGE_ENGINE_SEAM_OPS_H
