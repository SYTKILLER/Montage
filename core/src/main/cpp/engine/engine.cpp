#include "engine/engine.h"

#include <algorithm>

#include <hilog/log.h>

namespace montage {

namespace {

// 单瓦片蒙版调制（草稿叠加用；瓦片位于图层局部坐标 tx/ty）
void applyMaskToTile(const LayerMask& mask, std::vector<uint8_t>& px, uint32_t tx, uint32_t ty) {
    constexpr uint32_t kTile = 256;
    const int x0 = static_cast<int>(tx * kTile);
    const int y0 = static_cast<int>(ty * kTile);
    for (uint32_t r = 0; r < kTile; ++r) {
        uint8_t* row = px.data() + static_cast<size_t>(r) * kTile * 4u;
        for (uint32_t c = 0; c < kTile; ++c) {
            const uint8_t m = layerMaskGrayAt(mask, x0 + static_cast<int>(c), y0 + static_cast<int>(r));
            row[c * 4u + 3u] = static_cast<uint8_t>((row[c * 4u + 3u] * m + 127) / 255);
        }
    }
}

}  // namespace

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
    fireRevisionLocked();
}

void Engine::fireRevisionLocked() {
    // 撤销/重做：docRevision 被恢复为快照版本（非自增），只广播不递增。
    // 此前 undo 复用 bump 的自增会让 AppStorage 值恰好不变 → @Watch 不触发 →
    // 图层面板/缩略图在撤销后不刷新（M7.5 模拟器实证修复）。
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
                    history.beginEdit(doc, brushOnMask ? "mask brush"
                                        : (brush.erasing ? "eraser" : "brush"),
                                      static_cast<uint64_t>(docRevision));
                    draft = std::make_unique<StrokeDraft>();
                    std::string err;
                    if (!draft->begin(doc, doc.activeId, brush, brushOnMask, err)) {
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
                    if (draft->maskTarget()) {
                        // M5b：提交到蒙版 patch 网格（COW 换 LayerMask）+ 重合成
                        // 必须以原 patch 网格为合并基底（nullptr 会丢未触碰瓦片 → patch 缺行）
                        const LayerId lid = draft->layerId();
                        for (Layer& l : doc.layers) {
                            if (l.id == lid && l.mask != nullptr && l.mask->pixels != nullptr) {
                                auto merged = draft->commitGrid(l.mask->pixels.get());
                                if (merged != nullptr) {
                                    auto m = std::make_shared<LayerMask>(*l.mask);
                                    m->pixels = merged;
                                    l.mask = std::move(m);
                                    l.render = (l.mask->enabled && l.pixels != nullptr)
                                                   ? composeMasked(l.pixels, *l.mask)
                                                   : nullptr;
                                }
                                break;
                            }
                        }
                    } else {
                        // 提交（对拍 commit：工作瓦片合并入图层 grid）
                        const LayerId lid = draft->layerId();
                        auto grid = draft->commitGrid(nullptr);
                        for (Layer& l : doc.layers) {
                            if (l.id == lid && grid != nullptr) {
                                auto merged = draft->commitGrid(l.pixels.get());
                                l.pixels = merged;
                                // M5a：蒙版启用图层提交后重合成派生网格
                                if (l.mask != nullptr && l.mask->enabled) {
                                    l.render = composeMasked(l.pixels, *l.mask);
                                }
                                break;
                            }
                        }
                    }
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
    if (draft == nullptr || !draft->active()) {
        return snap;
    }
    snap.layerId = draft->layerId();
    snap.active = true;
    const Layer* layer = nullptr;
    for (const Layer& l : doc.layers) {
        if (l.id == snap.layerId) {
            layer = &l;
            break;
        }
    }
    if (layer == nullptr) {
        return snap;
    }
    if (draft->maskTarget()) {
        // M5b 蒙版落笔：草稿瓦片在 patch 网格空间；显示需按像素网格合成
        // （display = pixelTile × draftMask）。蒙版停用或空像素层 → 无叠加。
        if (layer->pixels == nullptr || layer->mask == nullptr || !layer->mask->enabled) {
            snap.active = false;
            return snap;
        }
        snap.gridCols = layer->pixels->cols;
        snap.gridRows = layer->pixels->rows;
        constexpr uint32_t kTile = 256;
        for (const uint32_t key : draft->touchedTiles()) {
            const uint32_t ptx = key % draft->gridCols();
            const uint32_t pty = key / draft->gridCols();
            std::vector<uint8_t> maskWork;
            if (!draft->tileFor(ptx, pty, maskWork)) {
                continue;
            }
            // patch 瓦片覆盖的图层局部矩形
            const int rx0 = layer->mask->offsetX + static_cast<int>(ptx * kTile);
            const int ry0 = layer->mask->offsetY + static_cast<int>(pty * kTile);
            const uint32_t ltx0 = std::max(0, rx0) / kTile;
            const uint32_t lty0 = std::max(0, ry0) / kTile;
            const uint32_t ltx1 = std::max(0, rx0 + static_cast<int>(kTile) - 1) / kTile;
            const uint32_t lty1 = std::max(0, ry0 + static_cast<int>(kTile) - 1) / kTile;
            for (uint32_t lty = lty0; lty <= lty1 && lty < snap.gridRows; ++lty) {
                for (uint32_t ltx = ltx0; ltx <= ltx1 && ltx < snap.gridCols; ++ltx) {
                    auto pit = layer->pixels->tiles.find(lty * snap.gridCols + ltx);
                    if (pit == layer->pixels->tiles.end()) {
                        continue;  // 像素空瓦片 × 蒙版 = 透明
                    }
                    const uint8_t* pxs = pit->second->pixels->mapCpu();
                    const uint32_t pstride = pit->second->pixels->rowBytes();
                    std::vector<uint8_t> out(static_cast<size_t>(kTile) * kTile * 4u);
                    for (uint32_t r = 0; r < kTile; ++r) {
                        const int gy = static_cast<int>(lty * kTile) + static_cast<int>(r);
                        const uint8_t* srow = pxs + static_cast<size_t>(r) * pstride;
                        uint8_t* orow = out.data() + static_cast<size_t>(r) * kTile * 4u;
                        for (uint32_t c = 0; c < kTile; ++c) {
                            const int gx = static_cast<int>(ltx * kTile) + static_cast<int>(c);
                            // patch 域坐标 → work 瓦片内坐标；内容矩形外走旧蒙版语义
                            // （256 对齐瓦片在 content 外的样本是 0，直采会误隐 outside 区）
                            const int pxg = gx - layer->mask->offsetX;
                            const int pyg = gy - layer->mask->offsetY;
                            const int mx = pxg - static_cast<int>(ptx * kTile);
                            const int my = pyg - static_cast<int>(pty * kTile);
                            uint8_t m;
                            if (pxg >= 0 && pyg >= 0 &&
                                pxg < static_cast<int>(layer->mask->width) &&
                                pyg < static_cast<int>(layer->mask->height) && mx >= 0 &&
                                my >= 0 && mx < static_cast<int>(kTile) && my < static_cast<int>(kTile)) {
                                m = maskWork[(static_cast<size_t>(my) * kTile) + static_cast<size_t>(mx)];
                            } else {
                                m = layerMaskGrayAt(*layer->mask, gx, gy);
                            }
                            const uint8_t* s = srow + static_cast<size_t>(c) * 4u;
                            uint8_t* d = orow + static_cast<size_t>(c) * 4u;
                            d[0] = s[0];
                            d[1] = s[1];
                            d[2] = s[2];
                            d[3] = static_cast<uint8_t>((s[3] * m + 127) / 255);
                        }
                    }
                    snap.tiles[lty * snap.gridCols + ltx] = std::move(out);
                }
            }
        }
        return snap;
    }
    // 像素落笔：草稿瓦片即显示瓦片；蒙版启用则乘蒙版（否则新笔画在蒙版外"画时可见、提交即消失"）
    snap.gridCols = draft->gridCols();
    snap.gridRows = draft->gridRows();
    const LayerMask* mask =
        (layer->mask != nullptr && layer->mask->enabled) ? layer->mask.get() : nullptr;
    for (const uint32_t key : draft->touchedTiles()) {
        std::vector<uint8_t> px;
        if (draft->tileFor(key % draft->gridCols(), key / draft->gridCols(), px)) {
            if (mask != nullptr) {
                applyMaskToTile(*mask, px, key % draft->gridCols(), key / draft->gridCols());
            }
            snap.tiles[key] = std::move(px);
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
