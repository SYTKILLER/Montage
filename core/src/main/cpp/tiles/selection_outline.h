#ifndef MONTAGE_TILES_SELECTION_OUTLINE_H
#define MONTAGE_TILES_SELECTION_OUTLINE_H

// M7b：选区轮廓提取（Marching Squares，阈值 127/255，02 §1 定案）。
// 输出文档坐标折线段（每条闭合环首尾相接；y 向下，与文档坐标系一致）。

#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/document.h"

namespace montage {
namespace tiles {

struct OutlinePolyline {
    std::vector<float> x;  // 顶点（文档 px）
    std::vector<float> y;
};

// 从选区掩码提取 ≥阈值(127) 区域的轮廓；空选区返回 false。
bool extractSelectionOutline(const TileGrid& mask, std::vector<OutlinePolyline>& out);

}  // namespace tiles
}  // namespace montage

#endif  // MONTAGE_TILES_SELECTION_OUTLINE_H
