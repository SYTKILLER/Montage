#ifndef MONTAGE_TILES_MAGIC_WAND_H
#define MONTAGE_TILES_MAGIC_WAND_H

// M7c：魔棒选区（对拍上游 MagicWand/WandPixels 语义）：
// 采样点击像素颜色（sampleSize N×N 均值，1=单像素），容差内像素入选；
// contiguous=连通（BFS 4 邻接）/ 全图扫描。
// M10d 内存治理：采样源改稀疏瓦片域惰性读取（按需加载触及瓦片），退役
// 整层 RGBA flatten（4096²≈67MB 峰值，06 §8.1 债务清偿）；allLayers 采样源 =
// CPU flatten 合成结果（v1 登记整域一次性，仅所有图层路径）。
// 输出文档域选区掩码（255 入选 / 0 未选；灰度 0 不落瓦片）。

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/document.h"

namespace montage {
namespace tiles {

// 采样源抽象：单层=稀疏瓦片惰性缓存；所有图层=flatten 连续平面
class WandSource {
  public:
    virtual ~WandSource() = default;
    virtual const uint8_t* at(int x, int y) = 0;  // 文档坐标 → RGBA；出界 nullptr
};

// 单层：按需加载触及瓦片
std::unique_ptr<WandSource> makeTileSource(const TileGrid& grid);

// 所有图层：flatten 合成平面（调用方持有平面生命周期）
std::unique_ptr<WandSource> makeFlatSource(std::shared_ptr<std::vector<uint8_t>> rgba,
                                           uint32_t w, uint32_t h);

// 魔棒选择。sampleSize：奇数 1..101（N×N 均值种子色）；allLayers 由 source 决定。
// 返回新掩码网格（与 doc 同维度）；无命中返回 nullptr。
std::shared_ptr<const TileGrid> wandMask(WandSource& src, uint32_t docW, uint32_t docH,
                                         int sampleX, int sampleY, int tolerance,
                                         bool contiguous, int sampleSize);

}  // namespace tiles
}  // namespace montage

#endif  // MONTAGE_TILES_MAGIC_WAND_H
