#ifndef MONTAGE_ENGINE_STYLE_OPS_H
#define MONTAGE_ENGINE_STYLE_OPS_H

// M14c 图层样式/填充渲染核：styledRender 派生网格（蒙版 composeMasked 同架构）。
// 底片 = 蒙版合成结果（render）；渲染顺序 = 投影（最底）→ 外描边 → 本体 → 颜色叠加（顶）。
// 投影模糊复用 M13 高斯（filters::applyFilter kind=0，三 box 近似）；描边 = van Herk
// 方形膨胀环带（PS 圆角略方，登记）。派生域 = 内容 bbox + pad（投影 blur/距离 + 描边宽），
// origin 独立于 transform（Layer.styledOriginX/Y）。网格/域外透明。
// 约定：调用方持 docMutex；历史事务由 bridge 层包裹；网格发布序号由调用方注入（缓存键）。

#include <cstdint>
#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {

// M17d 等高线 LUT（photocraft contour_lut/contour_curve_lut 对拍）：4096 项 0..1 传递表。
// custom=false → 空（恒等）；点列 ≥2 → 自然样条（photocraft curve_lut 同式：三对角解二阶
// 导数），角点按段各自成样条（全角点 = 折线）；不足 2 点 → 空。宿主测试直接断言数值。
std::vector<float> buildContourLut(const StyleContour& c);

// M17d 发光 Range（photocraft ranged_lut/glow_lut）：contour(min(v / range, 1))。
// range 钳 0.01..1；c 非 custom 且 range≥0.999 → 空（恒等，photocraft None 语义）。
std::vector<float> buildRangedLut(const StyleContour& c, double range);

// 重算派生渲染（样式/填充层）。样式禁用/无效果项 → styledRender = nullptr（走回原像素）。
// rev = 网格发布序号（调用方保证单调）。返回 false = 网格域超预算等硬错误（保守拒绝）。
bool restyleLayer(Document& doc, Layer& l, uint64_t rev, std::string& err);

// 全文档带样式/填充层重算（像素变更类命令收尾：笔画提交/滤镜/渐变/填充/形状/剪贴板/
// 几何变换等）。rev 进程内单调自增。锁内调用。
void restyleDocument(Document& doc);

// 几何/重采样操作后置空派生渲染（styledRender 基于 transform 前像素，翻转/旋转/重采样后失效；
// 命令收尾对带样式层统一 restyleLayer 重算）。返回是否曾有派生（调用方可据此批量重算）。
void invalidateStyled(Layer& l);

// 平移联动（移动工具/对齐分布/画布重设）：transform.origin 与 styledOrigin 同步平移
// （派生域为文档绝对坐标，样式随层移动）。
void shiftLayerOrigins(Layer& l, double dx, double dy);

}  // namespace montage

#endif  // MONTAGE_ENGINE_STYLE_OPS_H
