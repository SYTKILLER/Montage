#ifndef MONTAGE_ENGINE_VECTOR_FLATTEN_H
#define MONTAGE_ENGINE_VECTOR_FLATTEN_H

// M18 贝塞尔展平。翻译自 photocraft crates/vector/src/flatten.rs（MIT OR Apache-2.0）：
// 弦差容差 tol（默认 0.01 px）；直线段（控制点在弦上 1e-9 内）不再细分；分段数上界
// n = ceil(sqrt(0.75·max|B''|/tol))，小曲线按比例收紧 tol 保面积精确。

#include <vector>

#include "engine/vector_path.h"

namespace montage {

// 默认展平容差（曲线到弦距离，px）。
constexpr double kVectorTolerance = 0.01;

struct Polyline {
    std::vector<VPt> pts;
    std::vector<bool> knot;  // true = 源路径的 knot（join 落点）；false = 展平插入点
    bool closed = false;

    void push(VPt p, bool isKnot);
};

// 三次贝塞尔求值：seg = [p0, c1, c2, p1]。
VPt evalBezier(const std::array<VPt, 4>& seg, double t);

// 直线段判定：两个控制点都在弦上（±1e-9 px）。
bool bezierIsLine(const std::array<VPt, 4>& seg);

// 均匀分段数：弦偏差 ≤ tol。
size_t bezierPieces(const std::array<VPt, 4>& seg, double tol);

// 展平一个子路径；closed 子路径末点不重复首点。
Polyline flattenSubpath(const Subpath& s, double tol);

// 展平全部子路径。
std::vector<Polyline> flattenPath(const VectorPath& p, double tol);

// 交互渲染前置预算闸：有限坐标 + 有界细分量与 dash 工作量（photocraft validate_shape）。
// 弧长上界 = 三次控制多边形长度（不分配像素）。
bool validateShape(const VectorPath& path, const ShapeStroke* stroke, std::string& err);

}  // namespace montage

#endif  // MONTAGE_ENGINE_VECTOR_FLATTEN_H
