#ifndef MONTAGE_ENGINE_VECTOR_PATH_H
#define MONTAGE_ENGINE_VECTOR_PATH_H

// M18 矢量数据模型。翻译自 photocraft crates/doc/src/vector.rs（MIT OR Apache-2.0）：
// Knot/Subpath/Path/PathOp/FillRule/VectorMask/StrokeAlign/LineCap/LineJoin/ShapeStroke/
// LiveShape。坐标 = 文档像素（double，y 向下），三次贝塞尔。子路径段 i = knots[i].anchor →
// knots[i].out_ctrl → knots[i+1].in_ctrl → knots[i+1].anchor（closed 补回首 knot 段）；
// 控制点等于锚点的 knot = 角点（直线段）。
// 路径运算（PS 语义）：每个子路径按路径 fillRule 独立填充后按 op 依序折叠；首个子路径恒为
// Combine（effectiveOp）；Join = 并入前一子路径的形状分量（自定义形状的洞，反向绕数挖孔）；
// inverted = 最终区域取反（PSD 矢量蒙版 invert 位）。空矢量蒙版 = 全显（PS「添加矢量蒙版」）。

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace montage {

struct VPt {
    double x = 0.0;
    double y = 0.0;
};
inline VPt vpt(double x, double y) { return {x, y}; }
inline bool operator==(const VPt& a, const VPt& b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(const VPt& a, const VPt& b) { return !(a == b); }

struct Knot {
    VPt anchor;
    VPt inCtrl;   // 到达本 knot 的段的控制点
    VPt outCtrl;  // 离开本 knot 的段的控制点
    bool smooth = false;  // 平滑（联动）knot：钢笔保持双柄共线
};
inline bool operator==(const Knot& a, const Knot& b) {
    return a.anchor == b.anchor && a.inCtrl == b.inCtrl && a.outCtrl == b.outCtrl && a.smooth == b.smooth;
}
inline bool operator!=(const Knot& a, const Knot& b) { return !(a == b); }

// 子路径与前面子路径的组合方式（PS 路径运算）。
enum class PathOp : int32_t {
    Combine = 0,    // 组合（并）
    Subtract = 1,   // 减去
    Intersect = 2,  // 交集
    Exclude = 3,    // 排除重叠（异或）
    Join = 4,       // 并入前一子路径的形状分量（PSD op −1；按路径 fillRule 一起填充挖洞）
};

enum class FillRule : int32_t { NonZero = 0, EvenOdd = 1 };

struct Subpath {
    bool closed = false;
    std::vector<Knot> knots;
    PathOp op = PathOp::Combine;

    // 段数：closed = n，open = n−1；每段 [anchor, outCtrl, inCtrl, nextAnchor]。
    std::vector<std::array<VPt, 4>> segments() const;
};

inline bool operator==(const Subpath& a, const Subpath& b) {
    return a.closed == b.closed && a.knots == b.knots && a.op == b.op;
}
inline bool operator!=(const Subpath& a, const Subpath& b) { return !(a == b); }

struct VectorPath {
    std::vector<Subpath> subpaths;
    FillRule fillRule = FillRule::NonZero;
    bool inverted = false;  // 最终区域取反（PSD 矢量蒙版 invert 位 / 初始填充规则）

    bool empty() const;  // 全部子路径无 knot
    PathOp effectiveOp(size_t i) const;  // 首子路径恒 Combine（孤立「交集」形状也要能画）
    // 形状分量 = 一起填充的子路径区间 [start,end)；Join 并入前一分量。
    std::vector<std::pair<size_t, size_t>> components() const;
    // 全部锚点+控制点包围盒（曲线的超集）；空路径返回 false。
    bool controlBounds(double& x0, double& y0, double& x1, double& y1) const;
};
inline bool operator==(const VectorPath& a, const VectorPath& b) {
    return a.subpaths == b.subpaths && a.fillRule == b.fillRule && a.inverted == b.inverted;
}
inline bool operator!=(const VectorPath& a, const VectorPath& b) { return !(a == b); }

// 描边相对路径的位置。
enum class StrokeAlign : int32_t { Inside = 0, Center = 1, Outside = 2 };
enum class LineCap : int32_t { Butt = 0, Round = 1, Square = 2 };
enum class LineJoin : int32_t { Miter = 0, Round = 1, Bevel = 2 };

// 形状描边（PS 形状「描边」选项；区别于图层样式描边）。v1 描边/填充 paint = 纯色
// （渐变/图案挂 M11 stops 遗留合并批）；dash 长度 = 宽度的倍数（PS 约定）。
struct ShapeStroke {
    double width = 3.0;
    float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // 直 alpha
    double opacity = 1.0;
    StrokeAlign align = StrokeAlign::Center;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    double miterLimit = 100.0;  // 宽度倍数（PS 默认 100 = 恒 miter）
    std::vector<double> dashes;  // 宽度倍数交替 on/off；空 = 实线
    double dashOffset = 0.0;

    // 交互命令入口的合法性闸（photocraft ShapeStroke::validate 同语义）；导入方保留原始
    // 数据不受此限，渲染方另有工作量钳制。
    bool validate(std::string& err) const;
};
inline bool operator==(const ShapeStroke& a, const ShapeStroke& b) {
    return a.width == b.width && a.color[0] == b.color[0] && a.color[1] == b.color[1] &&
           a.color[2] == b.color[2] && a.color[3] == b.color[3] && a.opacity == b.opacity &&
           a.align == b.align && a.cap == b.cap && a.join == b.join && a.miterLimit == b.miterLimit &&
           a.dashes == b.dashes && a.dashOffset == b.dashOffset;
}

// 参数化（live）形状：路径的生成来源，保留供再编辑（属性条）。kind 见 ShapeLiveKind。
enum class ShapeLiveKind : int32_t {
    None = 0,
    Rect = 1,     // rect[4] + radii[4]（tl,tr,br,bl；全 0 = 直角）
    Ellipse = 2,  // rect[4]
    Polygon = 3,  // rect[4] + sides + starRatio（1 = 正多边形，<1 = 星形）
    Line = 4,     // from→to，weight px 条带
};
struct LiveShape {
    ShapeLiveKind kind = ShapeLiveKind::None;
    double rect[4] = {0.0, 0.0, 0.0, 0.0};  // x,y,w,h
    double radii[4] = {0.0, 0.0, 0.0, 0.0};
    uint32_t sides = 5;
    double starRatio = 1.0;
    double from[2] = {0.0, 0.0};
    double to[2] = {0.0, 0.0};
    double weight = 4.0;
};
inline bool operator==(const LiveShape& a, const LiveShape& b) {
    if (a.kind != b.kind || a.sides != b.sides) return false;
    for (int i = 0; i < 4; i++) {
        if (a.rect[i] != b.rect[i] || a.radii[i] != b.radii[i]) return false;
    }
    if (a.starRatio != b.starRatio || a.weight != b.weight || a.from[0] != b.from[0] ||
        a.from[1] != b.from[1] || a.to[0] != b.to[0] || a.to[1] != b.to[1])
        return false;
    return true;
}

// 图层矢量蒙版（M18d；PS「图层 › 矢量蒙版」）。feather 仅存储（渲染 v1 不羽化，登记）。
struct VectorMaskData {
    VectorPath path;
    bool enabled = true;
    bool linked = true;
    float density = 1.0f;  // 0..1（属性面板）
    float feather = 0.0f;
};
inline bool operator==(const VectorMaskData& a, const VectorMaskData& b) {
    return a.path == b.path && a.enabled == b.enabled && a.linked == b.linked &&
           a.density == b.density && a.feather == b.feather;
}

}  // namespace montage

#endif  // MONTAGE_ENGINE_VECTOR_PATH_H
