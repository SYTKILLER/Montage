#ifndef MONTAGE_ENGINE_SHAPE_OPS_H
#define MONTAGE_ENGINE_SHAPE_OPS_H

// M11c 形状工具（06 规划 §4 M11）：栅格化直落当前层，单事务（A-4：一步撤销）。
// - 填充 = 前景色、描边 = 背景色（选项条约定，描边独立色留后续批）；描边叠填充之上。
// - 闭合形状（矩形/圆角/椭圆/多边形）描边 = 填充掩码轮廓带（M10 border 核，方形 SE
//   方形近似登记）；直线 = 胶囊带（tiles/shape_mask）。
// - 选区约束自动生效（alpha × 选区灰度）。
// - 未触碰瓦片 memcmp 保留共享（COW 零成本撤销），提交 revision = old+1。

#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {

struct ShapeSpec {
    enum class Kind : int32_t {
        Rect = 0,
        RoundedRect = 1,
        Ellipse = 2,
        Line = 3,
    };
    Kind kind = Kind::Rect;
    double x0 = 0.0;  // 拖拽对角（文档域；直线 = 端点）
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    double cornerRadius = 0.0;  // RoundedRect
    bool fill = true;           // Line 忽略（恒描边）
    bool stroke = false;
    double strokeWidth = 2.0;
};

// 栅格化形状到图层。fg/bg = 直 alpha 0..1。返回 false 时 err 带原因。调用方包 history 事务。
bool rasterizeShape(Document& doc, LayerId layerId, const ShapeSpec& spec,
                    const float fg[3], const float bg[3], std::string& err);

}  // namespace montage

#endif  // MONTAGE_ENGINE_SHAPE_OPS_H
