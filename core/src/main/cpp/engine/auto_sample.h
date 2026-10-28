#ifndef MONTAGE_ENGINE_AUTO_SAMPLE_H
#define MONTAGE_ENGINE_AUTO_SAMPLE_H

// P5③：内容感知填充 Auto 采样区（photocraft content_aware.rs auto_sampling +
// segment/mod.rs grid_cut C++ 翻译，语义对拍）。
//   gridCut      —— Boykov–Jolly 二值标定（8 连通，对比敏感平滑项）单最小割
//   autoSampling —— 窗口内「与孔周边相似」的采样区判定：降采样格（≤384 边）特征 =
//                   局部均值 + 局部对比（7×7 box mean），环带（孔外 6/k px）聚 6 簇
//                   k-means（确定性起点），τ = P90·1.6（下限 0.03）；对比敏感
//                   weight(d²) = 8·e^(−d²/(2·0.05²)) + 0.05 一刀最小割；源像素不足
//                   2·max(孔,64) 回退「孔外全部可用」。

#include <cstdint>
#include <functional>
#include <vector>

namespace montage {
namespace autosample {

// grid_cut（photocraft grid_cut_cancellable 语义；weight = 对比敏感边权函数，
// 入参 = 邻居颜色平方差和）。fixed: 0=FREE 1=HARD_FG 2=HARD_BG；返回逐像素前景标记。
std::vector<bool> gridCut(const float* img, int w, int h, int ch,
                          const std::vector<float>& costFg, const std::vector<float>& costBg,
                          const std::vector<uint8_t>& fixed,
                          const std::function<float(float)>& weight);

// Auto 采样区（img = w×h×ch float 0..1 行主序；hole = 孔掩码）。返回可采样掩码
//（恒不含孔）。失败/退化回退 = 孔外全部可用。
std::vector<bool> autoSampling(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                               const std::vector<bool>& hole);

}  // namespace autosample
}  // namespace montage

#endif  // MONTAGE_ENGINE_AUTO_SAMPLE_H
