#include "engine/layer_ops.h"

// M14b 实现。合成核 = io/image_export 的组感知 CPU flatten（与 GL 逐公式同构，
// M7.5 B-1 对拍 + M14b 宿主单测双重背书）；合并结果网格经 filterStripToGrid 发布
// （region = 参与层内容 bbox 并集，避免全画布缓冲）。

#include <algorithm>
#include <atomic>
#include <memory>

#include "engine/filter_ops.h"
#include "io/image_export.h"

namespace montage {

namespace {

std::atomic<uint64_t> g_mergeGridRev{1};

int indexOf(const Document& doc, LayerId id) {
    for (size_t i = 0; i < doc.layers.size(); ++i) {
        if (doc.layers[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

Layer* findById(Document& doc, LayerId id) {
    for (Layer& l : doc.layers) {
        if (l.id == id) {
            return &l;
        }
    }
    return nullptr;
}

const Layer* findConst(const Document& doc, LayerId id) {
    for (const Layer& l : doc.layers) {
        if (l.id == id) {
            return &l;
        }
    }
    return nullptr;
}

// 参与合并的层可合成判据：像素/文本层（调整层/组不直接参与，v1 拒绝）
bool mergeablePixelLayer(const Layer& l) {
    return !l.isGroup && l.adjustment == nullptr;
}

// flatten 层段 [start, end) 到 bbox 域并发布为网格（origin 随 bbox 返回）。
// 任一层可见且非空才返回 true。
bool flattenRangeToGrid(const Document& doc, int start, int end, bool opaqueWhite,
                        std::shared_ptr<const TileGrid>& outGrid, int& ox, int& oy,
                        std::string& err) {
    int rx = 0, ry = 0, rw = 0, rh = 0;
    if (!io::rangeContentBBox(doc, start, end, rx, ry, rw, rh)) {
        err = "no visible content to merge";
        return false;
    }
    io::FlattenJob job = io::BuildFlattenJobRange(doc, start, end);
    job.opaqueWhite = opaqueWhite;
    io::FlattenResult res = io::FlattenVisible(job, rx, ry, rw, rh);
    if (!res.ok) {
        err = "flatten failed: " + res.error;
        return false;
    }
    outGrid = filterStripToGrid(res.rgba, res.width, res.height, g_mergeGridRev.fetch_add(1));
    ox = rx;
    oy = ry;
    return true;
}

}  // namespace

bool mergeDown(Document& doc, std::string& err) {
    const int idx = indexOf(doc, doc.activeId);
    if (idx <= 0) {
        err = "no layer below to merge into";
        return false;
    }
    Layer& upper = doc.layers[static_cast<size_t>(idx)];
    Layer& below = doc.layers[static_cast<size_t>(idx - 1)];
    if (upper.isGroup) {
        err = "active layer is a group";
        return false;
    }
    if (upper.adjustment != nullptr) {
        err = "merging an adjustment layer down is not supported yet";
        return false;
    }
    if (below.isGroup) {
        err = "the layer below is a group";
        return false;
    }
    if (below.adjustment != nullptr) {
        err = "the layer below is an adjustment layer";
        return false;
    }
    if (below.clipping) {
        err = "the layer below is a clipping layer (merge its base first)";
        return false;
    }
    std::shared_ptr<const TileGrid> grid;
    int ox = 0, oy = 0;
    if (!flattenRangeToGrid(doc, idx - 1, idx + 1, false, grid, ox, oy, err)) {
        return false;
    }
    // 结果替换下方槽位：id/name/blend/visible/parentId/clipping 保留，opacity 归 1
    // （上层不透明度已烘焙；再乘会二次衰减——photocraft 同语义）
    Layer merged = below;
    merged.pixels = grid;
    merged.mask = nullptr;
    merged.render = nullptr;
    merged.text = nullptr;
    merged.adjustment = nullptr;
    merged.opacity = 1.0;
    merged.transform.originX = ox;
    merged.transform.originY = oy;
    merged.transform.width = 0.0;
    merged.transform.height = 0.0;
    doc.layers[static_cast<size_t>(idx - 1)] = std::move(merged);
    doc.layers.erase(doc.layers.begin() + idx);
    doc.activeId = doc.layers[static_cast<size_t>(idx - 1)].id;
    return true;
}

bool mergeVisible(Document& doc, std::string& err) {
    std::vector<int> visibleIdx;
    for (size_t i = 0; i < doc.layers.size(); ++i) {
        if (doc.layers[i].visible) {
            visibleIdx.push_back(static_cast<int>(i));
        }
    }
    if (visibleIdx.size() < 2) {
        err = "there are not enough visible layers to merge";
        return false;
    }
    const int lowest = visibleIdx.front();
    const Layer& base = doc.layers[static_cast<size_t>(lowest)];
    std::shared_ptr<const TileGrid> grid;
    int ox = 0, oy = 0;
    if (!flattenRangeToGrid(doc, 0, static_cast<int>(doc.layers.size()), false, grid, ox, oy,
                            err)) {
        return false;
    }
    Layer merged;
    merged.id = base.id;
    merged.name = base.name;
    merged.blendMode = BlendMode::Normal;
    merged.opacity = 1.0;
    merged.visible = true;
    merged.parentId = base.parentId;
    merged.pixels = grid;
    merged.transform.originX = ox;
    merged.transform.originY = oy;
    // 重建层栈：最低可见层槽位放合并结果，其余可见层移除，隐藏层保留
    // （photocraft merge_visible_keeps_hidden_layers 语义）
    std::vector<LayerId> dropped;
    for (size_t i = 0; i < doc.layers.size(); ++i) {
        if (static_cast<int>(i) != lowest && doc.layers[i].visible) {
            dropped.push_back(doc.layers[i].id);
        }
    }
    const LayerId mergedParent = merged.parentId;
    std::vector<Layer> out;
    out.reserve(doc.layers.size());
    for (size_t i = 0; i < doc.layers.size(); ++i) {
        Layer l = std::move(doc.layers[i]);
        if (static_cast<int>(i) == lowest) {
            out.push_back(std::move(merged));
            continue;
        }
        if (l.visible) {
            continue;  // 可见层已并入结果
        }
        out.push_back(std::move(l));
    }
    doc.layers = std::move(out);
    // 隐藏层的祖先被移除（曾在可见组内）→ 挂到合并结果的父级，保持平铺连续
    for (Layer& l : doc.layers) {
        if (l.parentId == 0) {
            continue;
        }
        LayerId cur = l.parentId;
        int guard = 0;
        while (cur != 0 && guard++ < 256) {
            if (std::find(dropped.begin(), dropped.end(), cur) != dropped.end()) {
                l.parentId = mergedParent;
                break;
            }
            const Layer* p = findConst(doc, cur);
            if (p == nullptr) {
                l.parentId = mergedParent;  // 父链悬空（防御）
                break;
            }
            cur = p->parentId;
        }
    }
    doc.activeId = base.id;
    return true;
}

bool flattenImage(Document& doc, LayerId newId, std::string& err) {
    if (doc.width == 0 || doc.height == 0) {
        err = "no document";
        return false;
    }
    std::shared_ptr<const TileGrid> grid;
    int ox = 0, oy = 0;
    if (!flattenRangeToGrid(doc, 0, static_cast<int>(doc.layers.size()), true, grid, ox, oy,
                            err)) {
        // 全隐/全空：拼合仍产出白底背景层（PS 语义）
        const size_t bytes = static_cast<size_t>(doc.width) * doc.height * 4;
        std::vector<uint8_t> white(bytes, 255);
        for (size_t k = 3; k < bytes; k += 4) {
            white[k] = 255;  // 直 alpha 域（assign 已全 255，保留清晰表达）
        }
        grid = filterStripToGrid(white, doc.width, doc.height, g_mergeGridRev.fetch_add(1));
        ox = 0;
        oy = 0;
        err.clear();
    }
    Layer bg;
    bg.id = newId;
    bg.name = "背景";
    bg.blendMode = BlendMode::Normal;
    bg.opacity = 1.0;
    bg.visible = true;
    bg.pixels = grid;
    bg.transform.originX = 0.0;
    bg.transform.originY = 0.0;
    std::vector<Layer> out;
    out.push_back(std::move(bg));
    doc.layers = std::move(out);
    doc.activeId = doc.layers.front().id;
    return true;
}

LayerId stampVisible(Document& doc, LayerId newId, const std::string& name, std::string& err) {
    bool anyVisible = false;
    for (const Layer& l : doc.layers) {
        if (l.visible && mergeablePixelLayer(l)) {
            anyVisible = true;
            break;
        }
    }
    if (!anyVisible) {
        err = "there are no visible layers to stamp";
        return 0;
    }
    std::shared_ptr<const TileGrid> grid;
    int ox = 0, oy = 0;
    if (!flattenRangeToGrid(doc, 0, static_cast<int>(doc.layers.size()), false, grid, ox, oy,
                            err)) {
        return 0;
    }
    Layer stamped;
    stamped.id = newId;
    stamped.name = name;
    stamped.pixels = grid;
    stamped.transform.originX = ox;
    stamped.transform.originY = oy;
    // 插到活动层上方（photocraft insert_above(active) 同语义）
    size_t at = doc.layers.size();
    for (size_t i = 0; i < doc.layers.size(); ++i) {
        if (doc.layers[i].id == doc.activeId) {
            at = i + 1;
            break;
        }
    }
    doc.layers.insert(doc.layers.begin() + static_cast<long>(at), std::move(stamped));
    doc.activeId = doc.layers[at].id;
    return doc.layers[at].id;
}

LayerId duplicateLayer(Document& doc, LayerId id, LayerId newId, std::string& err) {
    const int idx = indexOf(doc, id);
    if (idx < 0) {
        err = "layer not found";
        return 0;
    }
    Layer dup = doc.layers[static_cast<size_t>(idx)];
    dup.id = newId;
    dup.name = doc.layers[static_cast<size_t>(idx)].name + " 副本";
    dup.linkId = 0;  // PS 语义：副本不继承链接
    dup.collapsed = false;
    // 像素/蒙版/文本/调整 = 不可变共享指针值拷贝（编辑侧 COW，天然隔离）
    const size_t at = static_cast<size_t>(idx) + 1;
    doc.layers.insert(doc.layers.begin() + static_cast<long>(at), std::move(dup));
    doc.activeId = doc.layers[at].id;
    return doc.layers[at].id;
}

bool setLayerLink(Document& doc, const std::vector<LayerId>& ids, bool link, std::string& err) {
    if (ids.empty()) {
        err = "no layers";
        return false;
    }
    for (LayerId id : ids) {
        const Layer* l = findById(doc, id);
        if (l == nullptr) {
            err = "layer not found";
            return false;
        }
        if (l->isGroup) {
            err = "cannot link a group";
            return false;
        }
    }
    if (link) {
        if (ids.size() < 2) {
            err = "link requires at least 2 layers";
            return false;
        }
        uint64_t next = 1;
        for (const Layer& l : doc.layers) {
            if (l.linkId >= next) {
                next = l.linkId + 1;
            }
        }
        for (LayerId id : ids) {
            findById(doc, id)->linkId = next;
        }
    } else {
        // 清除这些层所在链接组的全体成员
        std::vector<uint64_t> groups;
        for (LayerId id : ids) {
            const uint64_t g = findById(doc, id)->linkId;
            if (g != 0) {
                bool hit = false;
                for (uint64_t e : groups) {
                    if (e == g) {
                        hit = true;
                        break;
                    }
                }
                if (!hit) {
                    groups.push_back(g);
                }
            }
        }
        for (Layer& l : doc.layers) {
            if (l.linkId != 0) {
                for (uint64_t g : groups) {
                    if (l.linkId == g) {
                        l.linkId = 0;
                        break;
                    }
                }
            }
        }
    }
    return true;
}

std::vector<LayerId> collectLinkedIds(const Document& doc, LayerId id) {
    const Layer* l = findConst(doc, id);
    if (l == nullptr || l->linkId == 0) {
        return {id};
    }
    std::vector<LayerId> out;
    const uint64_t g = l->linkId;
    for (const Layer& e : doc.layers) {
        if (e.linkId == g && !e.isGroup) {
            out.push_back(e.id);
        }
    }
    if (out.empty()) {
        out.push_back(id);
    }
    return out;
}

}  // namespace montage
