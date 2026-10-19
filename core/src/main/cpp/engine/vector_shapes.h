#ifndef MONTAGE_ENGINE_VECTOR_SHAPES_H
#define MONTAGE_ENGINE_VECTOR_SHAPES_H

// M18 参数化（live）形状 → 路径。翻译自 photocraft crates/vector/src/shapes.rs
// （MIT OR Apache-2.0）：矩形/圆角矩形（kappa 圆角、CSS 式半径钳制）/椭圆/正多边形/
// 星形/直线（weight 条带）。knot 顺时针（y 向下），首点在顶边。

#include "engine/vector_path.h"

namespace montage {

// 四分之一圆三次贝塞尔柄长 4/3·(√2 − 1)。
constexpr double kShapeKappa = 0.5522847498307934;

// 轴对齐矩形。
VectorPath shapeRect(double x, double y, double w, double h);

// 圆角半径钳制 [tl,tr,br,bl]：相邻半径不超边长（统一等比缩小，CSS border-radius 语义）。
void clampRadii(double w, double h, double radii[4]);

// 圆角矩形（[top-left, top-right, bottom-right, bottom-left]）。
VectorPath shapeRoundedRect(double x, double y, double w, double h, const double radii[4]);

// 内切于矩形的椭圆。
VectorPath shapeEllipse(double x, double y, double w, double h);

// 内切于矩形椭圆的正多边形/星形（starRatio<1 = 星形；首顶点在正上方）。
VectorPath shapePolygon(double x, double y, double w, double h, uint32_t sides, double starRatio);

// a→b 的 weight px 条带（直线形状）。
VectorPath shapeLine(VPt a, VPt b, double weight);

// live 形状的路径。
VectorPath liveShapePath(const LiveShape& s);

}  // namespace montage

#endif  // MONTAGE_ENGINE_VECTOR_SHAPES_H
