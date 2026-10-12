#include "engine/group_ops.h"

// M14a 组操作实现。连续不变式：任意组节点的直接子层在平铺序中连续。
// setLayerParent = 虚拟副本上执行移动 + 连续性终检（前置穿插校验对"目标组嵌套于
// 其他组段"的合法移动会误拒——祖先组段包含目标插入点天经地义）。组 id 由调用方
// 生成传入（nextLayerId 属 Engine，本模块保持宿主机可测）。

#include <algorithm>
#include <vector>

namespace montage {

namespace {

Layer* findById(Document& doc, LayerId id) {
    for (Layer& l : doc.layers) {
        if (l.id == id) {
            return &l;
        }
    }
    return nullptr;
}

int indexOf(const Document& doc, LayerId id) {
    for (size_t i = 0; i < doc.layers.size(); ++i) {
        if (doc.layers[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// id 是否为 ancestor 的后代（含自身）——环检测
bool isDescendant(const Document& doc, LayerId id, LayerId ancestor) {
    LayerId cur = id;
    int guard = 0;
    while (cur != 0 && guard++ < 256) {
        if (cur == ancestor) {
            return true;
        }
        const Layer* l = findById(const_cast<Document&>(doc), cur);
        if (l == nullptr) {
            return false;
        }
        cur = l->parentId;
    }
    return false;
}

// M14a 连续不变式（树形）：平铺序必须是层树的先序 DFS 序——组节点后紧跟其完整子树，
// 嵌套子组的子层合法地夹在祖先组的直接子层之间（朴素线性连续检查会误判嵌套）。
size_t consumeSubtree(const std::vector<Layer>& layers, size_t i, LayerId parentId);

// 从 layers[i] 消费 parent 的一个孩子（及其子树），返回消费层数；不匹配返回 0
size_t consumeChild(const std::vector<Layer>& layers, size_t i, LayerId parentId) {
    if (i >= layers.size() || layers[i].parentId != parentId) {
        return 0;
    }
    size_t consumed = 1;
    if (layers[i].isGroup) {
        consumed += consumeSubtree(layers, i + 1, layers[i].id);
    }
    return consumed;
}

// 从 layers[i] 连续消费 parent 的全部直接子层，返回消费层数
size_t consumeSubtree(const std::vector<Layer>& layers, size_t i, LayerId parentId) {
    const size_t start = i;
    while (true) {
        const size_t c = consumeChild(layers, i, parentId);
        if (c == 0) {
            break;
        }
        i += c;
    }
    return i - start;
}

bool contiguousOn(const std::vector<Layer>& layers) {
    return consumeSubtree(layers, 0, 0) == layers.size();
}

}  // namespace

LayerId createGroup(Document& doc, LayerId newId, const std::string& name, std::string& err) {
    if (doc.width == 0) {
        err = "no document";
        return 0;
    }
    // 插入点：活动层之后（bottom→top：组节点在子层段之前）
    int at = static_cast<int>(doc.layers.size());
    for (int i = 0; i < static_cast<int>(doc.layers.size()); ++i) {
        if (doc.layers[i].id == doc.activeId) {
            at = i + 1;
            break;
        }
    }
    Layer g;
    g.id = newId;
    g.name = name.empty() ? "组 " + std::to_string(newId) : name;
    g.isGroup = true;
    g.parentId = 0;
    doc.layers.insert(doc.layers.begin() + at, std::move(g));
    doc.activeId = doc.layers[at].id;
    return doc.layers[at].id;
}

LayerId groupLayer(Document& doc, LayerId newId, LayerId layerId, std::string& err) {
    const int idx = indexOf(doc, layerId);
    if (idx < 0) {
        err = "layer not found";
        return 0;
    }
    if (doc.layers[idx].isGroup) {
        err = "cannot group a group (select a layer)";
        return 0;
    }
    Layer g;
    g.id = newId;
    g.name = "组 " + std::to_string(newId);
    g.isGroup = true;
    // 组节点占层当前位置（bottom 侧），层后移入组
    doc.layers.insert(doc.layers.begin() + idx, std::move(g));
    doc.layers[idx + 1].parentId = newId;
    doc.activeId = newId;
    return newId;
}

bool ungroupLayer(Document& doc, LayerId groupId, std::string& err) {
    const int idx = indexOf(doc, groupId);
    if (idx < 0) {
        err = "group not found";
        return false;
    }
    if (!doc.layers[idx].isGroup) {
        err = "not a group";
        return false;
    }
    const int end = groupChildEnd(doc.layers, idx);
    for (int j = idx + 1; j < end; ++j) {
        // 直接子层上提为根；嵌套子组（parentId==组节点）一并上提为根组——
        // 子组的子层指向子组，内部关系保持
        if (doc.layers[j].parentId == groupId) {
            doc.layers[j].parentId = 0;
        }
    }
    doc.layers.erase(doc.layers.begin() + idx);
    if (!doc.layers.empty()) {
        doc.activeId = doc.layers[std::min(idx, static_cast<int>(doc.layers.size()) - 1)].id;
    }
    return true;
}

bool setLayerParent(Document& doc, LayerId id, LayerId newParent, int index, std::string& err) {
    const int from = indexOf(doc, id);
    if (from < 0) {
        err = "layer not found";
        return false;
    }
    if (newParent != 0) {
        const Layer* p = findById(doc, newParent);
        if (p == nullptr || !p->isGroup) {
            err = "target parent is not a group";
            return false;
        }
        if (isDescendant(doc, newParent, id)) {
            err = "cannot move into own descendant";
            return false;
        }
    }
    // 虚拟副本上执行移动 + 连续性终检
    std::vector<Layer> tmp = doc.layers;
    Layer moved = tmp[static_cast<size_t>(from)];
    tmp.erase(tmp.begin() + from);
    const int n = static_cast<int>(tmp.size());
    const int at = std::clamp(index > from ? index - 1 : index, 0, n);
    tmp.insert(tmp.begin() + at, std::move(moved));
    tmp[static_cast<size_t>(at)].parentId = newParent;
    if (!contiguousOn(tmp)) {
        err = "move breaks group contiguity";
        return false;
    }
    doc.layers = std::move(tmp);
    doc.activeId = doc.layers[static_cast<size_t>(at)].id;
    return true;
}

bool setGroupCollapsed(Document& doc, LayerId groupId, bool collapsed, std::string& err) {
    Layer* g = findById(doc, groupId);
    if (g == nullptr || !g->isGroup) {
        err = "group not found";
        return false;
    }
    g->collapsed = collapsed;
    return true;
}

bool deleteGroupRecursive(Document& doc, LayerId groupId, std::string& err) {
    const int idx = indexOf(doc, groupId);
    if (idx < 0) {
        err = "group not found";
        return false;
    }
    if (!doc.layers[idx].isGroup) {
        err = "not a group";
        return false;
    }
    const int end = groupChildEnd(doc.layers, idx);
    doc.layers.erase(doc.layers.begin() + idx, doc.layers.begin() + end);
    if (!doc.layers.empty()) {
        doc.activeId = doc.layers[std::min(idx, static_cast<int>(doc.layers.size()) - 1)].id;
    }
    return true;
}

bool groupsContiguous(const Document& doc) {
    return contiguousOn(doc.layers);
}

}  // namespace montage
