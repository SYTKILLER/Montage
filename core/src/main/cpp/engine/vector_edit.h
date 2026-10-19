#ifndef MONTAGE_ENGINE_VECTOR_EDIT_H
#define MONTAGE_ENGINE_VECTOR_EDIT_H

// M18 锚点级路径编辑（PS 直接选择 / 转换点 / 添加·删除锚点）。翻译自 photocraft
// crates/vector/src/edit.rs（MIT OR Apache-2.0）：移动锚点（连同手柄）、拖方向手柄、
// 拖段重塑、平滑/角点互转、插入/删除锚点、命中测试。纯几何；引擎 path.* 编辑命令与画布
// 预览共享，预览即所提交。
// knot 寻址 = [subpath, knot]；全部编辑先验证索引，失败返回 false 且不改动。

#include <cstdint>
#include <vector>

#include "engine/vector_path.h"

namespace montage {

// knot 的两根方向手柄之一。
enum class HandleId : int32_t { In = 0, Out = 1 };

// 命中结果类型。
enum class HitKind : int32_t { None = 0, Anchor = 1, Handle = 2, Segment = 3 };
struct PathHit {
    HitKind kind = HitKind::None;
    size_t sub = 0;     // [subpath, knot]
    size_t knot = 0;
    HandleId handle = HandleId::In;  // Handle 命中
    double t = 0.5;                  // Segment 命中：段参数
};

// 平滑 knot 保持手柄共线：把 h 的对侧柄转到反向等长（已收起的柄保持收起，PS 同）。
void keepSmooth(Knot& k, HandleId h);

VPt knotHandle(const Knot& k, HandleId h);

// 移动锚点（连同其手柄）位移 d；每个 knot 只动一次（重复列出去重）。索引非法返回 false。
bool movePathAnchors(VectorPath& path, const std::vector<std::pair<size_t, size_t>>& anchors, double dx, double dy);

// 手柄 h 放到 to。平滑 knot 的对侧柄保持共线，除非 independent（转换点/⌥）——变角点、
// 双柄独立。
bool movePathHandle(VectorPath& path, size_t s, size_t k, HandleId h, VPt to, bool independent);

// 转换点：out = null → 收柄角点（点击锚点）；有 out → 平滑 knot（柄 = out 与其镜像，
// 拖出锚点）。
bool convertPathPoint(VectorPath& path, size_t s, size_t k, bool hasOut, VPt out);

// 拖动离开 knot 的段的参数 t 处位移 d。直段（双柄收起）随两端锚点整体平移；曲线段重塑
// 使 t 处点精确跟随指针（近柄移动更多）。平滑端 knot 保持平滑。
bool bendPathSegment(VectorPath& path, size_t s, size_t k, double t, double dx, double dy);

// 添加锚点：插入到离开 knot 的段的参数 t（严格 0..1）处并返回新 knot 寻址。曲线按 t
// de Casteljau 分割（形状不变、新锚点平滑）；直段（双柄收起）落到线上为角点。
bool addPathAnchor(VectorPath& path, size_t s, size_t k, double t, size_t& outSub, size_t& outKnot);

// 删除锚点：邻居由一段连接（保前锚 out 柄与后锚 in 柄）；closed 子路径保持闭合。删除
// 子路径最后锚点删整个子路径。仅剩全路径一个锚点时拒绝。返回是否删除了整个子路径。
bool deletePathAnchor(VectorPath& path, size_t s, size_t k, bool& removedSubpath);

// 子路径 s 的全部锚点（直接选择 ⌥-click 选整条子路径）。
std::vector<std::pair<size_t, size_t>> subpathAnchors(const VectorPath& path, size_t s);

// 框 [x0,y0,x1,y1] 内的锚点（直接选择框选），按路径序。
std::vector<std::pair<size_t, size_t>> anchorsInBox(const VectorPath& path, double x0, double y0, double x1, double y1);

// 曲率钢笔 knots [anchor, in, out]：过全部锚点的平滑曲线（Catmull-Rom 式）。内锚手柄
// 在邻弦平行线上（共线）、各 1/3 邻距（chordal 缩放防不均间距过冲）；开放路径端点瞄向
// 邻点近柄。corners[i] = 收起锚点 i 手柄（角点；缺省平滑）。重合点收柄不产 NaN。
std::vector<std::array<VPt, 3>> curvatureKnots(const std::vector<VPt>& anchors, const std::vector<bool>& corners,
                                               bool closed);

// 命中测试：tol 内先手柄（在最上层）→ 锚点 → 段；同层取最近。handles = 当前显示的手柄
// 集合（隐藏手柄不参与）。
PathHit hitTestPath(const VectorPath& path, VPt p, double tol,
                    const std::vector<std::pair<std::pair<size_t, size_t>, HandleId>>& handles);

}  // namespace montage

#endif  // MONTAGE_ENGINE_VECTOR_EDIT_H
