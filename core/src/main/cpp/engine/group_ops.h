#ifndef MONTAGE_ENGINE_GROUP_OPS_H
#define MONTAGE_ENGINE_GROUP_OPS_H

// M14a 图层编组操作（平铺栈方案，连续不变式由本模块维护）：
// - 组节点 = isGroup 层（无像素），直接子层在平铺序中连续且位于组节点之后。
// - 全部操作含连续性校验（破坏即拒绝并返回错误，调用方 History 事务包裹）。

#include <string>

#include "engine/document.h"

namespace montage {

// 新建空组，插到活动层上方（bottom→top 序：组节点在子层段之前），激活并返回组 id。
// newId = 调用方生成的层 id（nextLayerId 属 Engine，本模块保持宿主机可测）。
LayerId createGroup(Document& doc, LayerId newId, const std::string& name, std::string& err);

// Ctrl+G：把活动层包进新建组（组节点占原层位，层移入组内）。
LayerId groupLayer(Document& doc, LayerId newId, LayerId layerId, std::string& err);

// 解散组：直接子层上提到组节点位置（parentId=0，保持序），删除组节点，
// 激活第一个上提子层。嵌套子组整体上提（子组的父子关系保持）。
bool ungroupLayer(Document& doc, LayerId groupId, std::string& err);

// 移动层到指定父组（0=根）的 index（目标父层级位置）。插入点若落在其他组段内部则拒绝。
// 组节点不可移入自身后代（环检测）。
bool setLayerParent(Document& doc, LayerId id, LayerId newParent, int index, std::string& err);

// 组折叠（面板显示态，影响渲染：折叠组整段跳过）。
bool setGroupCollapsed(Document& doc, LayerId groupId, bool collapsed, std::string& err);

// 组级联删除校验：组 → 连同整棵子树删除（removeLayer 的组语义在此实现）。
bool deleteGroupRecursive(Document& doc, LayerId groupId, std::string& err);

// 连续性校验（调试/测试用）：全部组的子层在平铺序中连续。
bool groupsContiguous(const Document& doc);

// 平铺序先序不变式终检（consumeSubtree 全量消费）：reorderLayer/setLayerParent 守卫共用。
bool contiguousOn(const std::vector<Layer>& layers);

}  // namespace montage

#endif  // MONTAGE_ENGINE_GROUP_OPS_H
