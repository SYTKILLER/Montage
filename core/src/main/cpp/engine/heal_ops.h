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

// M19b 内容感知填充：选区 = 孔，采样窗口（PS 4·√ 公式）内受限源补全（content_aware::fill
// 默认适配），RGBA 直混经选区灰度写回。一步撤销由调用方包 history。
bool contentAwareFill(Document& doc, std::string& err);

// M19b 内容感知移动：选区内容按 (dx,dy) 泊松融入新址；extend=false 时旧址内容感知填充。
bool contentAwareMove(Document& doc, int dx, int dy, bool extend, std::string& err);

}  // namespace montage

#endif  // MONTAGE_ENGINE_HEAL_OPS_H
