#ifndef MONTAGE_ENGINE_VECTOR_STROKE_H
#define MONTAGE_ENGINE_VECTOR_STROKE_H

// M18 多边形描边。翻译自 photocraft crates/vector/src/stroke.rs（MIT OR Apache-2.0）：
// 描边 = 每段一个四边形 + 每个 join 一个楔形 + 每个开端一个 cap 形状的并集。全部碎块以
// 正向面积发出，按 non-zero 规则一起填充即得精确并集（不算偏移曲线）。dash 先按弧长
// 切开展平折线。

#include <vector>

#include "engine/vector_flatten.h"
#include "engine/vector_path.h"

namespace montage {

// 像素域描边几何（模型倍数在 shapeRender 处换算，photocraft stroke_style 同分工）。
struct StrokeStyle {
    double width = 1.0;
    LineCap cap = LineCap::Butt;
    LineJoin join = LineJoin::Miter;
    double miterLimit = 4.0;  // 宽度倍数（斜接长/宽，同 SVG）
    std::vector<double> dashes;  // px 交替 on/off；空 = 实线
    double dashOffset = 0.0;
};

// lines 的描边轮廓碎块（全部正向面积）；按 non-zero 规则填充。
std::vector<std::vector<VPt>> strokePolygons(const std::vector<Polyline>& lines, const StrokeStyle& style,
                                             double tol);

// 把折线切成 dash 段（开放折线）。pattern 交替 on/off 长度（px）。
std::vector<Polyline> dashPolyline(const Polyline& pl, const std::vector<double>& pattern, double offset);

}  // namespace montage

#endif  // MONTAGE_ENGINE_VECTOR_STROKE_H
