#ifndef MONTAGE_ENGINE_HISTORY_H
#define MONTAGE_ENGINE_HISTORY_H

// 撤销历史（02 §4 定案）：整文档值快照（Document 拷贝 + TilesRef 共享，未修改瓦片
// shared_ptr 共享 → 内存 ∝ 被修改瓦片数）；条数 + 独占字节双限裁剪。
// M4a：单层事务（嵌套合并留 M4b）；异步线程安全性由调用方 docMutex 保证状态读写。

#include <mutex>
#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {

class History {
  public:
    // 开始编辑：记录 before 快照（须与 docMutex 同线程；配对 endEdit）
    void beginEdit(const Document& doc, const std::string& label, uint64_t revision);
    // 结束编辑：before==after 则丢弃；否则入 undo 栈并清 redo
    void endEdit(const Document& doc, uint64_t revision);

    bool undo(Document& doc, uint64_t& revision, std::string& label);
    bool redo(Document& doc, uint64_t& revision, std::string& label);
    bool canUndo() const;
    bool canRedo() const;
    // 整档替换（打开工程/PSD）时清空：撤销不许跨文档回退
    void clear();

  private:
    struct Snapshot {
        Document doc;
        uint64_t revision = 0;
    };
    struct Entry {
        std::string label;
        Snapshot before;
        Snapshot after;
    };
    void trim();

    mutable std::mutex mtx_;
    std::vector<Entry> undo_;
    std::vector<Snapshot> redo_;
    Snapshot pendingSnap_;     // beginEdit 后、endEdit 前的 before
    std::string pendingLabel_;
    bool pendingFlag_ = false;
    
    size_t maxEntries_ = 100;
    size_t maxExclusiveBytes_ = 128ull * 1024 * 1024;  // 02 §7：撤销独占字节 128MB

    static size_t exclusiveBytes(const Document& a, const Document& b);
};

}  // namespace montage

#endif  // MONTAGE_ENGINE_HISTORY_H
