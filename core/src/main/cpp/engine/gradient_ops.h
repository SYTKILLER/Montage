#ifndef MONTAGE_ENGINE_GRADIENT_OPS_H
#define MONTAGE_ENGINE_GRADIENT_OPS_H

// M11a 绘图工具第一批（06 规划 §4 M11）：渐变 / 填充 / 描边——CPU 像素核 + 单事务。
// - 渐变：拖线 (p0→p1) 距离场，线性 = 投影 t、径向 = 距离比；stops 升序分段线性插值
//   （直 alpha 域逐通道 lerp）。像素目标 = src-over 绘画语义（半透明端与底混合）；
//   蒙版目标 = 灰度域 src-over（outG = lum·sA + base·(1−sA)，同 M5b-1 蒙版笔模型）。
//   选区约束 = sA × 选区灰度（M7a 语义，蒙版/像素目标自动获得）。
//   全层覆盖为 PS 语义（p0 前取 stop0、p1 后取 stop1）；逐瓦片 memcmp 相同则保留
//   原共享瓦片（COW 最小化，撤销快照零成本）。
// - 填充：选区（或整层）src-over 不透明色替换（前景/背景/黑/白）。
// - 描边：M10 border 环带核（r = ceil(w/2) 中心描边）× 前景色 src-over。
// 引擎 fg/bg 为直 alpha 0..1（M9c）。宿主机单测：.debug/m11/test_gradient.cpp。

#include <cstdint>
#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {

enum class GradientType : int32_t { Linear = 0, Radial = 1 };

// 颜色标（pos 升序；直 alpha 域）。v1 由 fg/bg 样式构建两停，stops 编辑器面板留后续批。
struct GradientStop {
    float pos = 0.0f;  // 0..1
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// v1 样式（stops 编辑器面板留后续批）：由引擎 fg/bg 现值构建两停
enum class GradientStyle : int32_t {
    FgToBg = 0,        // 前景→背景（不透明两停）
    FgToTransparent = 1,  // 前景→透明（alpha 255→0）
};

enum class GradientTarget : int32_t { Pixels = 0, LayerMask = 1 };

// 填充色源（Shift+F5 对话框；v1 四项）
enum class FillSource : int32_t { Foreground = 0, Background = 1, Black = 2, White = 3 };

struct GradientSpec {
    GradientType type = GradientType::Linear;
    GradientStyle style = GradientStyle::FgToBg;
    double x0 = 0.0;  // 拖线起点（文档域）
    double y0 = 0.0;
    double x1 = 0.0;  // 拖线终点
    double y1 = 0.0;
};

// 应用渐变到图层（像素或蒙版）。fg/bg = 直 alpha 0..1（引擎前景/背景色）。
// 返回 false 时 err 带原因。调用方包 history 事务。
bool applyGradient(Document& doc, LayerId layerId, const GradientSpec& spec,
                   GradientTarget target, const float fg[3], const float bg[3],
                   std::string& err);

// P1 stops 版：显式多停（pos 升序不校验，直 alpha 域；空表 = 报错）。stops 编辑器/
// 渐变预设库的引擎入口（预设的「前景/背景」停靠由调用方 resolve 成具体色后传入）。
bool applyGradientStops(Document& doc, LayerId layerId, const GradientSpec& spec,
                        GradientTarget target, const std::vector<GradientStop>& stops,
                        std::string& err);

// 填充（像素目标）：选区掩码约束（无选区 = 整层）；fg/bg 取引擎当前值。
bool fillWithColor(Document& doc, LayerId layerId, FillSource source, float fr, float fg,
                   float fb, float br, float bg_, float bb, std::string& err);

// P1 图案填充（油漆桶图案源/填充对话框图案项）：选区掩码约束，图案按文档原点锚定
// 环形平铺（PS「与图层对齐」off 语义简化，登记），nearest × scale（0.01..10 钳制）。
// pat = 图案像素网格（直 alpha；PresetLib 库图案或文档图案均可）。
bool fillWithPattern(Document& doc, LayerId layerId, const TileGrid& pat, uint32_t pw,
                     uint32_t ph, double scale, std::string& err);

// 描边（像素目标）：选区边界环带 × 前景色；width = 描边宽度 px（中心位置语义）。
bool strokeSelectionBorder(Document& doc, LayerId layerId, int width, float fr, float fgn,
                           float fb, std::string& err);

}  // namespace montage

#endif  // MONTAGE_ENGINE_GRADIENT_OPS_H
