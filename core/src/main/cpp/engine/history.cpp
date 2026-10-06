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
// 共享瓦片零成本，代价 ∝ 被修改瓦片）。
size_t History::exclusiveBytes(const Document& a, const Document& b) {
    std::set<const void*> aTiles;
    for (const Layer& l : a.layers) {
        if (l.pixels != nullptr) {
            for (const auto& kv : l.pixels->tiles) {
                aTiles.insert(kv.second.get());
            }
        }
    }
    size_t bytes = 0;
    for (const Layer& l : b.layers) {
        if (l.pixels != nullptr) {
            for (const auto& kv : l.pixels->tiles) {
                if (aTiles.count(kv.second.get()) == 0) {
                    bytes += static_cast<size_t>(kTileSize) * kTileSize * 4;
                }
            }
        }
    }
    return bytes;
}

void History::beginEdit(const Document& doc, const std::string& label, uint64_t revision) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (pendingFlag_) {
        return;  // 嵌套：外层事务优先（M4a 简化）
    }
    pendingSnap_.doc = doc;
    pendingSnap_.revision = revision;
    pendingLabel_ = label;
    pendingFlag_ = true;
}

void History::endEdit(const Document& doc, uint64_t revision) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!pendingFlag_) {
        return;
    }
    pendingFlag_ = false;
    Entry entry;
    entry.label = pendingLabel_;
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
    redo_.push_back(std::move(current));
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
    Snapshot next = std::move(redo_.back());
    redo_.pop_back();
    Snapshot current;
    current.doc = doc;
    current.revision = revision;
    Entry entry;
    entry.label = pendingLabel_.empty() ? std::string("edit") : pendingLabel_;
    entry.before = std::move(current);
    entry.after = next;
    undo_.push_back(std::move(entry));
    doc = next.doc;
    revision = next.revision;
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

bool History::canRedo() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return !redo_.empty();
}

}  // namespace montage
