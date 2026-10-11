#ifndef MONTAGE_TILES_QUICK_MASK_H
#define MONTAGE_TILES_QUICK_MASK_H

// M10d 快速蒙版（06 规划 §4 M10，PS Q 键语义）：选区 ↔ 临时通道切换模式。
// - 进入：selection → doc.quickMask（不可变共享零拷贝；无选区 = 空网格，缺瓦片 = 0 =
//   全图红 overlay——PS 语义"无选区进入 Q = 全红，白笔擦红建选区"）；selection 置 null
//   （蚂蚁线隐藏、笔画不受旧选区约束）。
// - 编辑：蒙版笔/画笔直接落 quickMask 网格（灰度 L，StrokeDraft qmTarget 路径）。
// - 退出：quickMask → selection（通道语义 = 选中度，白 = 选中；全 0 → null = 取消选择，
//   对齐 PS"全红退出无选区"）。
// 灰度约定与选区掩码一致（R=G=B=灰度、A=255，文档域 TileGrid；缺瓦片 = 0）。
// 会话态：不持久化（.montage/PSD 均跳过），随 Document 快照走撤销。

#include "engine/document.h"

namespace montage {
namespace tiles {

// 掩码是否存在任何非零灰度（退出时决定 selection 置网格还是 null）
bool maskHasSelection(const TileGrid& mask);

// 进入快速蒙版（已处于快速蒙版或无文档返回 false）
bool enterQuickMask(Document& doc);

// 退出快速蒙版（不在快速蒙版返回 false）
bool exitQuickMask(Document& doc);

}  // namespace tiles
}  // namespace montage

#endif  // MONTAGE_TILES_QUICK_MASK_H
