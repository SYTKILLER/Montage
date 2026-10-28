#ifndef MONTAGE_ENGINE_INPAINT_OPS_H
#define MONTAGE_ENGINE_INPAINT_OPS_H

// 孔洞填充内核（M19a，photocraft algo/inpaint.rs + content_aware.rs(sampling_window) C++ 翻译，
// MIT OR Apache-2.0）：
// - complete()  内容感知填充：Wexler-Irani EM（TPAMI 2007）+ PatchMatch NNF（SIGGRAPH 2009）。
//   金字塔由粗到细；最粗层孔洞从边缘向内逐环「洋葱剥皮」初始化（每像素取周边已知域最佳
//   匹配源块），细层从粗层匹配场上采样 + 本层像素投票精化。颜色适配 = Image Melding
//   （SIGGRAPH 2012）的逐通道 gain·src+bias 光度变换。
// - bestOffset() 接近匹配：环域 SSD 最小位移搜索（自动修复画笔源）。
// - synthesize() 创建纹理：Efros-Freeman Image Quilting（SIGGRAPH 2001）。
// 全部输入交错 w×h×ch 归一化 float + w×h 孔洞掩码，返回孔洞外等于输入的全图缓冲。
// 确定性：固定种子 SplitMix64，结果与线程数无关（单线程带状扫掠 = photocraft 规范序）。
// 宿主测试：.debug/m19/test_inpaint.cpp（photocraft oracle 直迁 + fast/plain 位级对拍）。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace montage {
namespace inpaint {

// SplitMix64（photocraft inpaint::Rng，常数一致——随机数序即算法行为序）。
struct Rng {
    uint64_t state = 0;
    static Rng seeded(uint64_t seed);
    uint64_t next();
    void skip(uint64_t n);
    int range(int lo, int hi);  // lo..=hi 均匀整数
};

// 拷贝块可被调整的光度范围：gain·src + bias（gain ∈ [lo,hi]，bias ∈ ±bias）。
struct Adapt {
    float gainLo = 1.0f;
    float gainHi = 1.0f;
    float bias = 0.0f;
    bool isNone() const { return gainLo == 1.0f && gainHi == 1.0f && bias == 0.0f; }
    static Adapt none() { return {}; }
};

// 像素角色（completeMasked）：Void=非图像（忽略）/Known=上下文但不可拷贝/
// Source=可拷贝的已知/Hole=待填充。
enum class Px : uint8_t { Void = 0, Known = 1, Source = 2, Hole = 3 };

struct CompleteParams {
    int patchRadius = 3;      // 补丁半径（补丁为 2r+1 方块）
    int emIters = 10;         // 最粗层 EM 迭代（细层递减，下限 minEmIters）
    int minEmIters = 3;
    int pmIters = 2;          // 每 EM 迭代的 PatchMatch 趟数
    int refineExtent = 192;   // 孔洞超过此宽的层只精化（粗层定结构，细层加细节）
    Adapt adapt{};            // 拷贝的颜色适配
    uint64_t seed = 1;
};

// 取消谓词：按 EM 迭代/PatchMatch 趟/行带检查；取消后结果不完整（状态 Cancelled）。
using CancelFn = std::function<bool()>;

enum class CompleteStatus { Ok, NoSource, Cancelled };
struct CompleteResult {
    CompleteStatus status = CompleteStatus::NoSource;
    std::vector<float> img;  // Ok 时为填充结果；NoSource 时原样奉还输入
};

// 内容感知补全（从孔洞外任意像素拷贝）。无全已知补丁可拷时 NoSource（调用方可回退膜填充）。
CompleteResult complete(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                        const std::vector<bool>& hole, const CompleteParams& p,
                        const CancelFn& cancel = nullptr);

// 带像素角色版（修补工具「仅从指定源拷贝」等场景用）。
CompleteResult completeMasked(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                              const std::vector<Px>& px, const CompleteParams& p,
                              const CancelFn& cancel = nullptr);

// 接近匹配：在 maxRadius 内找环域 SSD 最小的位移（被移位的孔必须完全在格内且不与孔相交）。
std::optional<std::pair<int, int>> bestOffset(size_t w, size_t h, size_t ch,
                                              const std::vector<float>& img,
                                              const std::vector<bool>& hole, size_t ring,
                                              int maxRadius);

// 创建纹理：周边纹理块填充孔洞（quilting 最小误差边界切割）。
std::vector<float> synthesize(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                              const std::vector<bool>& hole, size_t block, uint64_t seed);

// Photoshop 的采样窗口（content_aware.rs sampling_window）：孔洞包围盒为中心、边长
// 4·√(max(w,50)·max(h,50)) 的方块，滑入画布内（画布更小处裁剪）。
struct RectI {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    bool operator==(const RectI& o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
};
RectI samplingWindow(const RectI& hole, const RectI& canvas);

// P5④ 采样窗口向孔收缩：超出预算（面积 > budget）时以孔中心为锚逐步半收缩，
// 下限 = 孔尺寸（孔周采样域不小于孔本身）。PS Content-Aware 采样域语义；
// heal_ops 两处调用（填充/感知移动）共用。budget = 16e6（M19 登记项 16MP）。
RectI shrinkWindowTowardHole(const RectI& win, const RectI& hole, double budget);

// ---- 以下暴露给宿主测试（photocraft pub(crate) 直迁）----

// 布尔网格积分图（(w+1)×(h+1)）与裁剪窗口计数。
std::vector<uint32_t> integralOf(size_t w, size_t h, const std::vector<bool>& m);
uint32_t windowCount(const std::vector<uint32_t>& s, size_t gw, size_t gh, int x0, int y0, int x1,
                     int y1);

// 有效源中心：整个 2r+1 补丁在格内且可拷贝。
std::vector<bool> validSources(size_t w, size_t h, int r, const std::vector<Px>& px);

// 洋葱剥皮（从孔边缘向内逐环取最佳匹配源）；photocraft 优化版语义（邻居种子候选 +
// 每 16 源 + 早停），与朴素顺序扫描位级一致（测试对拍锁定）。
bool onionPeel(size_t w, size_t h, size_t ch, int r, std::vector<float>& img,
               const std::vector<Px>& px, const std::vector<std::pair<int, int>>& sources,
               const CancelFn& cancel = nullptr);

// 最小和 e ≥ 0 使 e/n > v（strict）或 float 算术下 e/n ≥ v；v 无穷返回无穷。
float leastSum(float v, float n, bool strict);

}  // namespace inpaint
}  // namespace montage

#endif  // MONTAGE_ENGINE_INPAINT_OPS_H
