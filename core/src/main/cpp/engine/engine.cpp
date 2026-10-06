#include "engine/engine.h"

#include <hilog/log.h>

namespace montage {
namespace {
constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Engine";
}  // namespace

Engine& Engine::get() {
    static Engine inst;
    return inst;
}

void Engine::requestRender() {
    needsRender.store(true);
    render.wake();
}

void Engine::bumpRevisionLocked() {
    // 须持有 docMutex；版本经 TSFN 送 JS 线程（AppStorage docVersion，01 §6 事件面）
    docRevision++;
    if (revisionTsfn == nullptr) {
        return;
    }
    void* payload = reinterpret_cast<void*>(static_cast<intptr_t>(docRevision));
    if (napi_call_threadsafe_function(revisionTsfn, payload, napi_tsfn_nonblocking) != napi_ok) {
        OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag, "revision tsfn call failed");
    }
}

LayerId Engine::nextLayerId() {
    static std::atomic<LayerId> counter{1};
    return counter.fetch_add(1);
}

void Engine::ensureStrokeThreadLocked() {
    // 常驻线程存活期间绝不 join（appfreeze 实测：EndStroke 持 strokeMtx 调 join，
    // 线程回 cv.wait 需要同一把锁 → 互等死锁）。join 仅用于 quit 后清理已退出线程。
    if (strokeThreadStarted_) {
        return;
    }
    if (strokeThread_.joinable()) {
        strokeThread_.join();
    }
    strokeQuit = false;
    strokeThreadStarted_ = true;
    strokeThread_ = std::thread(&Engine::strokeThreadMain, this);
}

// T3 笔画工作线程（常驻，条件变量等待）：按序消费命令队列——begin 落 draft（含
// history.beginEdit）、点坐标落 dab、end 收尾提交（M4a 撤销粒度 = 单笔画）。
// 陈旧点（上一笔 end 之后、下一笔 begin 之前残留）因 draft 为空自然丢弃。
void Engine::strokeThreadMain() {
    while (true) {
        std::deque<StrokePoint> batch;
        {
            std::unique_lock<std::mutex> lk(strokeMtx);
            strokeCv.wait(lk, [this] { return strokeQuit || !strokeQueue.empty(); });
            if (strokeQuit && strokeQueue.empty()) {
                strokeThreadStarted_ = false;
                return;
            }
            batch.swap(strokeQueue);
        }
        {
            // 生命周期互斥：与 dispose 清理串行（锁序：cycle → doc）
            std::lock_guard<std::mutex> cycle(strokeCycleMtx);
            std::lock_guard<std::mutex> lk(docMutex);
            for (const StrokePoint& p : batch) {
                if (p.begin) {
                    if (draft != nullptr) {
                        continue;  // 上一笔未提交（异常序列）：丢弃新 begin 防串笔
                    }
                    history.beginEdit(doc, brush.erasing ? "eraser" : "brush",
                                      static_cast<uint64_t>(docRevision));
                    draft = std::make_unique<StrokeDraft>();
                    std::string err;
                    if (!draft->begin(doc, doc.activeId, brush, err)) {
                        OH_LOG_Print(LOG_APP, LOG_WARN, 0x4D30, "Montage.Brush",
                                     "draft begin failed: %{public}s", err.c_str());
                        history.endEdit(doc, static_cast<uint64_t>(docRevision));
                        draft.reset();
                        continue;
                    }
                    draft->append(p.x, p.y, p.pressure);
                } else if (p.end) {
                    if (draft == nullptr) {
                        continue;  // 陈旧 end 标记（背靠背笔画残留）
                    }
                    draft->end();
                    // 提交（对拍 commit：工作瓦片合并入图层 grid）
                    const LayerId lid = draft->layerId();
                    auto grid = draft->commitGrid(nullptr);
                    for (Layer& l : doc.layers) {
                        if (l.id == lid && grid != nullptr) {
                            auto merged = draft->commitGrid(l.pixels.get());
                            l.pixels = merged;
                            break;
                        }
                    }
                    (void)grid;
                    const int64_t rev = docRevision + 1;
                    history.endEdit(doc, static_cast<uint64_t>(rev));
                    bumpRevisionLocked();
                    draft.reset();
                    historyVersion.fetch_add(1);
                } else if (draft != nullptr) {
                    draft->append(p.x, p.y, p.pressure);
                }
            }
            requestRender();
        }
    }
}

Engine::StrokeSnapshot Engine::copyStrokeSnapshotLocked() {
    StrokeSnapshot snap;
    if (draft != nullptr && draft->active()) {
        snap.layerId = draft->layerId();
        snap.gridCols = draft->gridCols();
        snap.gridRows = draft->gridRows();
        snap.active = true;
        for (const uint32_t key : draft->touchedTiles()) {
            std::vector<uint8_t> px;
            if (draft->tileFor(key % draft->gridCols(), key / draft->gridCols(), px)) {
                snap.tiles[key] = std::move(px);
            }
        }
    }
    return snap;
}

void Engine::resetDocument() {
    {
        std::lock_guard<std::mutex> lk(strokeMtx);
        strokeQuit = true;
        strokeCv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(import.mtx);
        import.cancel = true;
    }
    std::lock_guard<std::mutex> lk(docMutex);
    doc = Document{};
    viewport = Viewport{};
    needsRender.store(true);
    render.wake();
}

}  // namespace montage
