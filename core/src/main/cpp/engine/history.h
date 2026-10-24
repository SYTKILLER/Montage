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

    // M23c：取回并弃置挂起的 pending 事务（Camera Raw 预览取消还原用）。
    // 有 pending 时把 before 快照浅拷给 out 并清标志，返回 true。
    bool takePending(Document& out);

    // M15b 历史面板：线性状态列表 + 当前指针。
    // 列表 = [初始态, undo_[0].label, ..., undo_[n-1].label, redo 倒序标签...]；
    // current = undo_.size()（0 = 初始态）。labels[0] 为空串，由 UI 渲染「打开/新建」。
    struct Info {
        std::vector<std::string> labels;
        size_t current = 0;
    };
    Info getInfo() const;
    // 跳转到线性索引 target（PS 历史面板点选语义 = 连续 undo/redo；整步快照精确）。
    // 返回 false = 目标越界（不打断当前状态）。
    bool jumpTo(Document& doc, uint64_t& revision, size_t target, std::string& label);

    // M11f 历史画笔：取 steps 步前（1 = 最近一次编辑的 before）快照中指定层的像素
    // （shared_ptr 共享，trim 不影响持有者）。越界/层不存在返回 nullptr。
    std::shared_ptr<const TileGrid> layerPixelsStepsBack(int steps, LayerId layerId) const;

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
    // M15b：redo 侧携带原条目标签（修复 redo() 重建 Entry 误用 pendingLabel_
    // 导致重做步骤名全丢的存量缺陷——历史面板依赖真实标签）
    struct RedoEntry {
        Snapshot snap;
        std::string label;
    };
    void trim();

    mutable std::mutex mtx_;
    std::vector<Entry> undo_;
    std::vector<RedoEntry> redo_;
    Snapshot pendingSnap_;     // beginEdit 后、endEdit 前的 before
    std::string pendingLabel_;
    bool pendingFlag_ = false;
    
    size_t maxEntries_ = 100;
    size_t maxExclusiveBytes_ = 128ull * 1024 * 1024;  // 02 §7：撤销独占字节 128MB

    static size_t exclusiveBytes(const Document& a, const Document& b);
};

}  // namespace montage

#endif  // MONTAGE_ENGINE_HISTORY_H
