#ifndef MONTAGE_ENGINE_LAYER_OPS_H
#define MONTAGE_ENGINE_LAYER_OPS_H

// M14b 图层深化操作：合并族（向下/合并可见/拼合/盖印）+ 复制图层 + 链接层。
// 语义对拍 photocraft（MIT，07 §3.4 注入）：mergeDown 保下层 id/name/blend、opacity 归 1、
// 上层不透明度烘焙；mergeVisible 保留隐藏层（结果落最低可见层槽位并沿用其名）；拼合 =
// 全栈单层「背景」白底垫底；盖印 = 可见合成 → 活动层上方新层，原层全保留。
// 约定：调用方持 docMutex；history 事务由 bridge 层 beginEdit/endEdit 包裹。
// 简化登记（v1）：蒙版/文字在合并时烘焙为像素（结果无蒙版/不可编辑文本）；
// 下方为调整层/组/剪贴层的 mergeDown 拒绝执行（PS 深语义登记后续）。

#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {

// 合并活动层到其下方相邻像素层（结果替换下方槽位：id/name/blend 保留、opacity=1）。
bool mergeDown(Document& doc, std::string& err);

// 合并全部可见层为单一像素层（落最低可见层槽位、沿用其名；隐藏层保留在原位）。
bool mergeVisible(Document& doc, std::string& err);

// 拼合图像：全栈合成单层「背景」（透明区白底垫底；隐藏层丢弃）。
bool flattenImage(Document& doc, LayerId newId, std::string& err);

// 盖印：可见层合成 → 新像素层插到活动层上方（原层全保留，激活新层）。
LayerId stampVisible(Document& doc, LayerId newId, const std::string& name, std::string& err);

// 复制图层：像素/蒙版/文本共享不可变指针（编辑时 COW），名加「副本」，链接不继承；
// 插到原层上方并激活。
LayerId duplicateLayer(Document& doc, LayerId id, LayerId newId, std::string& err);

// 链接层（联动移动）：link=true 给全部 id 分配同一新 linkId（≥2 层、均非组）；
// link=false 清除这些层所在链接组（全体成员解除）。
bool setLayerLink(Document& doc, const std::vector<LayerId>& ids, bool link, std::string& err);

// 移动联动集合：id 的 linkId 非零时返回同组全部层 id（含自身），否则 {id}。
std::vector<LayerId> collectLinkedIds(const Document& doc, LayerId id);

}  // namespace montage

#endif  // MONTAGE_ENGINE_LAYER_OPS_H
