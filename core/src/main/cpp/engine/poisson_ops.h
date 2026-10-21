#ifndef MONTAGE_ENGINE_POISSON_OPS_H
#define MONTAGE_ENGINE_POISSON_OPS_H

// 梯度域（泊松）融合内核（M19a，photocraft algo/poisson.rs 全量 C++ 翻译，MIT OR Apache-2.0）。
// Pérez/Gangnet/Blake, Poisson Image Editing, SIGGRAPH 2003：引导场取源梯度时解可拆为
// f = g + h，h 是边界失配的膜（调和）插值——修复像素保留源纹理（梯度），颜色/光照被
// 拉向笔画周边的目标。拉普拉斯方程在 4 连通像素网格上用 SOR 求解，级联多重网格加速
// （半分辨率递归求解 → 双线性延拓为初值 → 每层少量 SOR 扫掠扫掉高频残差）。
// 缓冲布局：交错 w×h×ch 归一化 float（任意颜色模型）；通道独立求解。
// 宿主测试：.debug/m19/test_poisson.cpp（photocraft 三项 oracle 直迁）。

#include <cstddef>
#include <functional>
#include <vector>

namespace montage {
namespace poisson {

// 取消谓词：每个 SOR 扫掠/递归层检查一次；取消后返回的解不完整，调用方应丢弃。
using StopFn = std::function<bool()>;

// 解 Δv = 0（unknown 像素），Dirichlet 边界取 v 中已知像素值（出格邻居忽略 = Neumann）。
// v 入参为已知值，出参在 unknown 处写解。级联多重网格 + SOR。
void solveMembrane(size_t w, size_t h, const std::vector<bool>& unknown, std::vector<float>& v,
                   const StopFn& stop = nullptr);

// 测试参考解：零初值纯 Gauss-Seidel 直到更新低于 tol（oracle 对拍用）。
void solveMembraneReference(size_t w, size_t h, const std::vector<bool>& unknown,
                            std::vector<float>& v, float tol, size_t maxIter);

// 无缝克隆（引导场 v = ∇src）：返回 f——mask 外 = dst，mask 内 = src + 消除边界失配的膜。
// 保留源纹理，取目标颜色/光照。src/dst 交错 w×h×ch；mask w×h（true = 修复）。
// 网格边缘上的 mask 像素在该处见 Neumann（自由）边界——调用方应留 1px 不遮罩边距。
std::vector<float> seamlessClone(size_t w, size_t h, size_t ch, const std::vector<float>& src,
                                 const std::vector<float>& dst, const std::vector<bool>& mask);

// 膜填充（无源纹理）：已知像素的调和插值填入 hole。
std::vector<float> membraneFill(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                                const std::vector<bool>& hole);

// 可取消版膜填充；取消返回空向量（结果不完整）。
std::vector<float> membraneFillWith(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                                    const std::vector<bool>& hole, const StopFn& stop);

}  // namespace poisson
}  // namespace montage

#endif  // MONTAGE_ENGINE_POISSON_OPS_H
