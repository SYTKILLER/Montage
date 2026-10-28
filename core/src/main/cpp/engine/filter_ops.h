#ifndef MONTAGE_ENGINE_FILTER_OPS_H
#define MONTAGE_ENGINE_FILTER_OPS_H

// M13 滤镜管线（06 §4 M13 定案）：域 = 层内容包围盒 → 展开 strip → filters 核 →
// 选区约束 lerp（M7a 同模型）→ 发布新网格。预览 = 1/4 proxy（Engine::filterPreview
// 会话态，渲染快照替换该层 + 复用 M8.1 xform 矩阵放大显示）。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/document.h"
#include "engine/filters.h"

namespace montage {

// 全分辨率应用（History 事务由调用侧 beginEdit/endEdit 包裹）。
// 选区存在时：out = orig×(1−g) + filtered×g（g = 选区灰度 0..1，M7a coverage 语义）。
bool applyFilterToLayer(Document& doc, LayerId layerId, const filters::FilterParams& p,
                        std::string& err);

// P5② 分块并行 strip 执行器：src/dst 全幅 strip（w×h）。halo>0 的平移不变核按
// 512 对齐块 + filterHalo halo 执行（块内整幅 applyFilter，有效区回写——块输出与
// 整幅逐字节一致）。halo=0 / halo≥256 / applyFilter 块内失败 → 返回 false（调用方
// 退整幅单线程路径）。
bool applyFilterStripTiled(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h,
                           const filters::FilterParams& p, int threads, std::string& err);

// 计算层内容包围盒（非空瓦片并集；空层返回 false）。
bool layerContentBBox(const TileGrid& grid, uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h);

// 网格 bbox ↔ 连续 strip（直 alpha RGBA8888，行距 = w*4；网格外 = 透明）。预览降采样共用。
void filterGridToStrip(const TileGrid& grid, uint32_t gx, uint32_t gy, uint32_t w, uint32_t h,
                       std::vector<uint8_t>& strip);
std::shared_ptr<const TileGrid> filterStripToGrid(const std::vector<uint8_t>& strip, uint32_t w,
                                                  uint32_t h, uint64_t revision);

// 发布网格缓存键 revision：进程级单调计数（P2-10 合并原 g_mergeGridRev/g_styleGridRev）。
// 无因果语义——仅保证新发布网格的 revision 唯一（渲染侧 Tile* 身份键之外的缓存判据）。
uint64_t nextGridRevision();

}  // namespace montage

#endif  // MONTAGE_ENGINE_FILTER_OPS_H
