#ifndef MONTAGE_ENGINE_HEAL_OPS_H
#define MONTAGE_ENGINE_HEAL_OPS_H

// M19a 修补工具（PS Patch；photocraft retouch_cmds 同族语义）：
// 当前选区 = 目标域，源 = 选区内容按 (dx,dy) 平移；源纹理经泊松 seamlessClone 融入选区
// （保留源纹理、颜色/光照向选区周边收敛）。选区灰度作写入权重。撤销粒度 = 单次命令。

#include <string>

#include "engine/document.h"

namespace montage {

// dx/dy：文档域位移（拖拽向量；正 = 源在目标右侧/下方）。
bool patchSelection(Document& doc, int dx, int dy, std::string& err);

}  // namespace montage

#endif  // MONTAGE_ENGINE_HEAL_OPS_H
