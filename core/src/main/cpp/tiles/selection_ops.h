#ifndef MONTAGE_TILES_SELECTION_OPS_H
#define MONTAGE_TILES_SELECTION_OPS_H

// M10a 选区几何与布尔核（06 规划 §4 M10）：扫描线填充（偶奇规则 + 2×2 覆盖采样抗锯齿）、
// 椭圆、矩形、布尔四态、反选、全选。输入/输出掩码均为文档域 TileGrid
// （R=G=B=灰度、A=255，与 M7a 选区掩码约定一致；selectionGrayAt 消费，
// 蚂蚁线/笔画约束自动生效）。缺瓦片 = 灰度 0。

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/document.h"

namespace montage {
namespace tiles {

// 选区布尔四态（PS 选项条语义；Shift 临时加 / Alt 临时减由 UI 层映射到此枚举）
enum class SelectionOp : int32_t {
    Replace = 0,   // 新选区
    Add = 1,       // 添加到选区（并集）
    Subtract = 2,  // 从选区减去（差）
    Intersect = 3, // 与选区交叉
};

// 多边形掩码（套索/多边形套索）：xy = [x0,y0,x1,y1,...] 文档域浮点坐标，自动闭合；
// 点数 < 3 返回 nullptr。偶奇规则（PS 套索自交语义：交叉区挖空）。
std::shared_ptr<const TileGrid> polygonMask(const std::vector<double>& xy,
                                            uint32_t docW, uint32_t docH);

// 椭圆掩码（椭圆选框）：内切于文档域矩形 (x,y,w,h)；w/h ≤ 0 或与文档无交集返回 nullptr。
std::shared_ptr<const TileGrid> ellipseMask(double x, double y, double w, double h,
                                            uint32_t docW, uint32_t docH);

// 矩形掩码（选框工具布尔化改造用）：硬边，语义对齐既有 setRectSelection（半开区间）。
std::shared_ptr<const TileGrid> rectMask(double x, double y, double w, double h,
                                         uint32_t docW, uint32_t docH);

// 布尔合并：next（本次新掩码）与 doc.selection（现有，可 null）按 op 合并。
// Replace → next（null = 清除）；Add 无基选区 → next；Subtract/Intersect 无基选区 → null
// （PS：无基选区时减/交无效）；结果全 0 → null。doc.width==0 返回 null。
std::shared_ptr<const TileGrid> combineSelection(const Document& doc,
                                                 std::shared_ptr<const TileGrid> next,
                                                 SelectionOp op);

// 反选：255 − gray；无选区 = 全选（fullSelection）；全 0（原全选）→ null。
std::shared_ptr<const TileGrid> invertSelection(const Document& doc);

// 全选网格：文档域满铺 255（PS 全选有蚂蚁线边界，非"无选区"语义）。
std::shared_ptr<const TileGrid> fullSelection(uint32_t docW, uint32_t docH);

// ---- M10b 选区修改族（PS 选择→修改/羽化；全部 Replace 语义，无选区返回 nullptr）----

// 羽化：3×盒模糊近似高斯（σ = r/2，PS 对拍校准起步值），edge-clamp（文档边缘不衰减）。
// r ≤ 0 返回 nullptr。
std::shared_ptr<const TileGrid> featherSelection(const Document& doc, double radius);

// 扩展：方形 SE 形态学膨胀（两趟可分离 van Herk O(N)；圆盘角差异登记 v1 近似）。
std::shared_ptr<const TileGrid> expandSelection(const Document& doc, int radius);

// 收缩：形态学腐蚀；全部腐蚀掉 → nullptr。
std::shared_ptr<const TileGrid> contractSelection(const Document& doc, int radius);

// 边界：环带 = dilate(r) − erode(r)。
std::shared_ptr<const TileGrid> borderSelection(const Document& doc, int radius);

// 平滑：阈值 127 二值化 + 两轮 3×3 多数滤波（≥5 为选中），消除锯齿与孤立点。
std::shared_ptr<const TileGrid> smoothSelection(const Document& doc);

// 灰度写助手（布尔/反选共享）：把灰度行带写入 builder（0 保持稀疏不落瓦片）。
// 返回是否写过任何非零像素。
void setMaskGray(TileGridBuilder& builder, uint32_t cols, uint32_t docX, uint32_t docY,
                 uint8_t gray);

}  // namespace tiles
}  // namespace montage

#endif  // MONTAGE_TILES_SELECTION_OPS_H
