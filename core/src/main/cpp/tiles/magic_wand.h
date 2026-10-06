#ifndef MONTAGE_TILES_MAGIC_WAND_H
#define MONTAGE_TILES_MAGIC_WAND_H

// M7c：魔棒选区（对拍上游 MagicWand/WandPixels 语义 v1 子集）：
// 采样点击像素颜色，容差内像素入选；contiguous=连通（BFS 4 邻接）/ 全图扫描。
// 输出文档域选区掩码（255 入选 / 0 未选，随 Document.selection 约定）。

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/document.h"

namespace montage {
namespace tiles {

// 从图层有效像素做魔棒选择；返回新掩码网格（与 doc 同维度）。空图层返回 nullptr。
std::shared_ptr<const TileGrid> wandMask(const TileGrid& pixels, uint32_t docW, uint32_t docH,
                                         int sampleX, int sampleY, int tolerance,
                                         bool contiguous);

}  // namespace tiles
}  // namespace montage

#endif  // MONTAGE_TILES_MAGIC_WAND_H
