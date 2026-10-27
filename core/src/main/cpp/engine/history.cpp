#include "engine/history.h"

#include <mutex>
#include <set>

#include <hilog/log.h>

namespace {
constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.History";
}

namespace montage {

// 独占字节估算：只在 before 或 after 之一出现的瓦片引用数 × 256KB（02 §4 R5 语义：
// 共享瓦片零成本，代价 ∝ 被修改瓦片）。H3 扩展覆盖面：除图层像素外，蒙版 patch、
// 主选区、快速蒙版、Alpha 通道掩码同样计费（滑杆外的选区/蒙版编辑密集族不再漏账）；
// 智能对象嵌入源字节按差值计（替换内容/置入的大字节真实占用）。
namespace {
size_t gridExclusive(std::set<const void*>& aTiles, const std::shared_ptr<const TileGrid>& g) {
    if (g == nullptr) {
        return 0;
    }
    size_t bytes = 0;
    for (const auto& kv : g->tiles) {
        if (aTiles.insert(kv.second.get()).second) {
            bytes += static_cast<size_t>(kTileSize) * kTileSize * 4;
        }
    }
    return bytes;
}
}  // namespace

size_t History::exclusiveBytes(const Document& a, const Document& b) {
    std::set<const void*> aTiles;
    size_t bytes = 0;
    for (const Layer& l : a.layers) {
        if (l.pixels != nullptr) {
            for (const auto& kv : l.pixels->tiles) {
                aTiles.insert(kv.second.get());
            }
        }
        if (l.mask != nullptr && l.mask->pixels != nullptr) {
            for (const auto& kv : l.mask->pixels->tiles) {
                aTiles.insert(kv.second.get());
            }
        }
        if (l.smart != nullptr) {
            if (l.smart->filterMask != nullptr && l.smart->filterMask->pixels != nullptr) {
                for (const auto& kv : l.smart->filterMask->pixels->tiles) {
                    aTiles.insert(kv.second.get());
                }
            }
            aTiles.insert(l.smart->sourceBytes.data());
        }
    }
    if (a.selection != nullptr) {
        for (const auto& kv : a.selection->tiles) {
            aTiles.insert(kv.second.get());
        }
    }
    if (a.quickMask != nullptr) {
        for (const auto& kv : a.quickMask->tiles) {
            aTiles.insert(kv.second.get());
        }
    }
    for (const AlphaChannel& c : a.channels) {
        if (c.mask != nullptr) {
            for (const auto& kv : c.mask->tiles) {
                aTiles.insert(kv.second.get());
            }
        }
    }
    for (const Layer& l : b.layers) {
        bytes += gridExclusive(aTiles, l.pixels);
        if (l.mask != nullptr) {
            bytes += gridExclusive(aTiles, l.mask->pixels);
        }
        if (l.smart != nullptr) {
            if (l.smart->filterMask != nullptr) {
                bytes += gridExclusive(aTiles, l.smart->filterMask->pixels);
            }
            // 源字节按「before 中无同址指针」计差值（同址 = 共享同一 vector 堆块）
            if (aTiles.insert(l.smart->sourceBytes.data()).second) {
                bytes += l.smart->sourceBytes.size();
            }
        }
    }
    bytes += gridExclusive(aTiles, b.selection);
    bytes += gridExclusive(aTiles, b.quickMask);
    for (const AlphaChannel& c : b.channels) {
        bytes += gridExclusive(aTiles, c.mask);
    }
    return bytes;
}

void History::beginEdit(const Document& doc, const std::string& label, uint64_t revision,
                        const std::string& coalesceKey) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (pendingFlag_) {
        return;  // 嵌套：外层事务优先（M4a 简化）
    }
    pendingSnap_.doc = doc;
    pendingSnap_.revision = revision;
    pendingLabel_ = label;
    pendingCoalesce_ = coalesceKey;
    pendingFlag_ = true;
}

void History::endEdit(const Document& doc, uint64_t revision) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!pendingFlag_) {
        return;
    }
    pendingFlag_ = false;
    // H3 滑杆族合并：与末条目同合并键（且 redo 空——undo 后新编辑必须开新分支，
    // 否则 redo 指向的状态会被合并破坏）时替换末条目 after，整段拖动只落一条历史。
    // 键由桥接层按「效果:<层id>」构造，天然排除跨层/跨效果串账。
    if (!pendingCoalesce_.empty() && redo_.empty() && !undo_.empty()
        && undo_.back().coalesceKey == pendingCoalesce_) {
        Entry& last = undo_.back();
        if (exclusiveBytes(last.before.doc, doc) > maxExclusiveBytes_) {
            undo_.pop_back();  // 合并后超预算：连同被合并条目整体弃（同单步超限防御语义）
            OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag, "coalesce dropped: over budget");
            return;
        }
        last.after.doc = doc;
        last.after.revision = revision;
        return;
    }
    Entry entry;
    entry.label = pendingLabel_;
    entry.coalesceKey = pendingCoalesce_;
    entry.before = pendingSnap_;
    entry.after.doc = doc;
    entry.after.revision = revision;
    // 单步独占字节超预算：丢弃（防御性；双限裁剪见 trim）
    if (exclusiveBytes(entry.before.doc, entry.after.doc) > maxExclusiveBytes_) {
        OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag, "endEdit dropped: over budget");
        return;
    }
    undo_.push_back(std::move(entry));
    redo_.clear();
    trim();
}

void History::trim() {
    while (undo_.size() > maxEntries_) {
        undo_.erase(undo_.begin());
    }
    // 累计独占字节裁剪（02 §4 双限）：从最老开始逐出
    size_t total = 0;
    for (const Entry& e : undo_) {
        total += exclusiveBytes(e.before.doc, e.after.doc);
    }
    while (total > maxExclusiveBytes_ && !undo_.empty()) {
        total -= exclusiveBytes(undo_.front().before.doc, undo_.front().after.doc);
        undo_.erase(undo_.begin());
    }
}

bool History::undo(Document& doc, uint64_t& revision, std::string& label) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (undo_.empty()) {
        OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag, "undo: empty (depth=0)");
        return false;
    }
    Entry entry = std::move(undo_.back());
    undo_.pop_back();
    Snapshot current;
    current.doc = doc;
    current.revision = revision;
    RedoEntry re;
    re.snap = std::move(current);
    re.label = entry.label;
    redo_.push_back(std::move(re));
    doc = entry.before.doc;
    revision = entry.before.revision;
    label = entry.label;
    return true;
}

bool History::redo(Document& doc, uint64_t& revision, std::string& label) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (redo_.empty()) {
        return false;
    }
    RedoEntry next = std::move(redo_.back());
    redo_.pop_back();
    Snapshot current;
    current.doc = doc;
    current.revision = revision;
    Entry entry;
    entry.label = next.label;
    entry.before = std::move(current);
    entry.after = next.snap;
    undo_.push_back(std::move(entry));
    doc = next.snap.doc;
    revision = next.snap.revision;
    label = entry.label;
    return true;
}

bool History::canUndo() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return !undo_.empty();
}

void History::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    undo_.clear();
    redo_.clear();
    pendingFlag_ = false;
}

bool History::takePending(Document& out) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!pendingFlag_) {
        return false;
    }
    pendingFlag_ = false;
    out = pendingSnap_.doc;
    return true;
}

bool History::canRedo() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return !redo_.empty();
}

// M15b 历史面板数据：线性状态列表（undo 从老到新 + redo 从新到老拼接）+ 当前指针
History::Info History::getInfo() const {
    std::lock_guard<std::mutex> lk(mtx_);
    Info info;
    info.current = undo_.size();
    info.labels.push_back(std::string());  // 初始态（UI 渲染「打开/新建」）
    for (const Entry& e : undo_) {
        info.labels.push_back(e.label);
    }
    for (auto it = redo_.rbegin(); it != redo_.rend(); ++it) {
        info.labels.push_back(it->label);
    }
    return info;
}

// 跳转到线性索引（PS 历史面板点选语义）：连续 undo/redo 至 target；
// 中途任一步失败（栈耗尽）即停，返回实际是否到达
bool History::jumpTo(Document& doc, uint64_t& revision, size_t target, std::string& label) {
    std::lock_guard<std::mutex> lk(mtx_);
    const size_t total = undo_.size() + redo_.size();
    if (target > total) {
        return false;
    }
    while (undo_.size() > target) {
        Entry entry = std::move(undo_.back());
        undo_.pop_back();
        Snapshot current;
        current.doc = doc;
        current.revision = revision;
        RedoEntry re;
        re.snap = std::move(current);
        re.label = entry.label;
        redo_.push_back(std::move(re));
        doc = entry.before.doc;
        revision = entry.before.revision;
        label = entry.label;
    }
    while (undo_.size() < target && !redo_.empty()) {
        RedoEntry next = std::move(redo_.back());
        redo_.pop_back();
        Snapshot current;
        current.doc = doc;
        current.revision = revision;
        Entry entry;
        entry.label = next.label;
        entry.before = std::move(current);
        entry.after = next.snap;
        undo_.push_back(std::move(entry));
        doc = next.snap.doc;
        revision = next.snap.revision;
        label = entry.label;
    }
    return true;
}

// M11f 历史画笔源：steps 步前的层像素快照（shared_ptr 共享；调用方持 docMutex 亦可并行，
// 本函数自持 mtx_）
std::shared_ptr<const TileGrid> History::layerPixelsStepsBack(int steps, LayerId layerId) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (steps < 1 || static_cast<size_t>(steps) > undo_.size()) {
        return nullptr;
    }
    const Entry& e = undo_[undo_.size() - static_cast<size_t>(steps)];
    for (const Layer& l : e.before.doc.layers) {
        if (l.id == layerId) {
            return l.pixels;
        }
    }
    return nullptr;
}

}  // namespace montage
