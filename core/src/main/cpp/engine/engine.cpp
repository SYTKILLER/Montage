#include "engine/engine.h"
#include "engine/smart_ops.h"

#include "engine/style_ops.h"

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

void Engine::bumpRevisionLocked(DocSession* target) {
    // 须持有 docMutex；版本经 TSFN 送 JS 线程（AppStorage docVersion，01 §6 事件面）。
    // D2：载荷 = 全局 revisionSeq（单调——@Watch 需每次值变；target 缺省 = active）
    DocSession& s = target != nullptr ? *target : active();
    s.revision++;
    revisionSeq++;
    if (revisionTsfn == nullptr) {
        return;
    }
    void* payload = reinterpret_cast<void*>(static_cast<intptr_t>(revisionSeq));
    if (napi_call_threadsafe_function(revisionTsfn, payload, napi_tsfn_nonblocking) != napi_ok) {
        OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag, "revision tsfn call failed");
    }
}

void Engine::fireRevisionLocked() {
    // 撤销/重做：会话 revision 被恢复为快照版本（非自增），只广播不递增；
    // 此前 undo 复用 bump 的自增会让 AppStorage 值恰好不变 → @Watch 不触发 →
    // 图层面板/缩略图在撤销后不刷新（M7.5 模拟器实证修复）。
    // D2：切档/恢复同样需要「值必变」→ revisionSeq 自增后广播。
    revisionSeq++;
    if (revisionTsfn == nullptr) {
        return;
    }
    void* payload = reinterpret_cast<void*>(static_cast<intptr_t>(revisionSeq));
    if (napi_call_threadsafe_function(revisionTsfn, payload, napi_tsfn_nonblocking) != napi_ok) {
        OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag, "revision tsfn call failed");
    }
}

LayerId Engine::nextLayerId() {
    static std::atomic<LayerId> counter{1};
    return counter.fetch_add(1);
}

// ---------- D2 多文档池命令面（锁外调用，内部自取锁） ----------

uint64_t Engine::newSession(bool activate) {
    auto s = std::make_shared<DocSession>();
    s->id = ++sessionSeq;
    const uint64_t id = s->id;
    {
        std::lock_guard<std::mutex> lk(docMutex);
        docs.push_back(s);
        if (activate) {
            activeIdx = docs.size() - 1;
        }
    }
    return id;
}

size_t Engine::prepareOpenSlot() {
    // 整档替换类打开（工程/PSD/分层 TIFF/文件打开流）前的落位：active 是空档
    //（无内容且无历史）→ 复用（旧单文档语义）；否则新建会话并激活（PS 多标签）。
    // 后续 resetDocument 作用于新 active，旧文档会话与历史原样保留。
    {
        std::lock_guard<std::mutex> lk(docMutex);
        const DocSession& s = active();
        if (s.doc.width == 0 && !s.history.canUndo() && !s.history.canRedo()) {
            return activeIdx;
        }
    }
    newSession(true);
    std::lock_guard<std::mutex> lk(docMutex);
    return activeIdx;
}

bool Engine::switchDoc(size_t idx) {
    // 1) 丢弃队列中未处理点（属旧 active——落新档=串档）
    {
        std::lock_guard<std::mutex> lk(strokeMtx);
        strokeQueue.clear();
        strokeSuspend_ = true;  // 窗口期 begin 标记丢弃（防切换临界区新笔画）
    }
    {
        // 2) 与笔画线程 batch 处理段互斥；在途 draft 原地收尾（账目落原会话）再切换
        std::unique_lock<std::mutex> cycle(strokeCycleMtx);
        std::unique_lock<std::mutex> lk(docMutex);
        if (idx >= docs.size() || idx == activeIdx) {
            lk.unlock();
            cycle.unlock();
            std::lock_guard<std::mutex> sm(strokeMtx);
            strokeSuspend_ = false;
            return idx == activeIdx;  // 同位切换 = 成功（幂等）
        }
        if (draft != nullptr) {
            commitDraftLocked(lk, cycle, false);
        }
        activeIdx = idx;
        fireRevisionLocked();   // UI 镜像/标签条刷新（onDocVersion 全家桶）
        historyVersion.fetch_add(1);  // 历史面板随会话切换
        needsRender.store(true);
    }
    render.wake();
    {
        std::lock_guard<std::mutex> sm(strokeMtx);
        strokeSuspend_ = false;
    }
    return true;
}

bool Engine::switchDocById(uint64_t id) {
    std::unique_lock<std::mutex> lk(docMutex);
    for (size_t i = 0; i < docs.size(); ++i) {
        if (docs[i]->id == id) {
            lk.unlock();
            return switchDoc(i);
        }
    }
    return false;
}

bool Engine::closeDocAt(size_t idx) {
    // 关闭指定会话：唯一会话 → 退化为 resetDocument（旧 closeDocument 语义）；
    // 池空补一个空会话（「无文档」UI 态保持）；关 active → 原位邻档接管（越界回退末位）。
    {
        std::unique_lock<std::mutex> lk(docMutex);
        if (idx >= docs.size()) {
            return false;
        }
        if (docs.size() <= 1) {
            lk.unlock();
            resetDocument();
            return true;
        }
    }
    {
        std::lock_guard<std::mutex> lk(strokeMtx);
        strokeQueue.clear();
        strokeSuspend_ = true;
    }
    {
        std::unique_lock<std::mutex> cycle(strokeCycleMtx);
        std::unique_lock<std::mutex> lk(docMutex);
        if (idx >= docs.size()) {
            lk.unlock();
            cycle.unlock();
            std::lock_guard<std::mutex> sm(strokeMtx);
            strokeSuspend_ = false;
            return false;
        }
        // active 上的在途 draft 原地收尾（历史账目入会话——随会话一起销毁亦可，
        // 但 draft 提交路径还引用全局去重状态，先提交再移除最稳）
        if (idx == activeIdx && draft != nullptr) {
            commitDraftLocked(lk, cycle, false);
        }
        docs.erase(docs.begin() + static_cast<long>(idx));
        if (docs.empty()) {
            auto s = std::make_shared<DocSession>();
            s->id = ++sessionSeq;
            docs.push_back(s);
            activeIdx = 0;
        } else if (activeIdx >= docs.size()) {
            activeIdx = docs.size() - 1;
        } else if (idx < activeIdx) {
            activeIdx--;
        }
        fireRevisionLocked();  // 标签条/面板镜像刷新（无论关的是否 active）
        historyVersion.fetch_add(1);
        needsRender.store(true);
    }
    render.wake();
    {
        std::lock_guard<std::mutex> sm(strokeMtx);
        strokeSuspend_ = false;
    }
    return true;
}

bool Engine::closeDocById(uint64_t id) {
    std::unique_lock<std::mutex> lk(docMutex);
    for (size_t i = 0; i < docs.size(); ++i) {
        if (docs[i]->id == id) {
            lk.unlock();
            return closeDocAt(i);
        }
    }
    return false;
}

std::shared_ptr<DocSession> Engine::docIfUnchanged(const DocStamp& s) {
    // 须持 docMutex：同指针仍在池 + epoch/revision 未变 → 会话活且内容未被并发改
    for (auto& p : docs) {
        if (p.get() == s.session) {
            if (p->epoch == s.epoch && p->revision == s.revision) {
                return p;
            }
            return nullptr;
        }
    }
    return nullptr;
}

std::shared_ptr<DocSession> Engine::docIfEpoch(DocSession* ptr, uint64_t epoch) {
    // 须持 docMutex：导入归属复核——会话仍在池且未整档重置（允许期间正常编辑）
    for (auto& p : docs) {
        if (p.get() == ptr) {
            return p->epoch == epoch ? p : nullptr;
        }
    }
    return nullptr;
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
            // 生命周期互斥：与 dispose 清理串行（锁序：cycle → doc）。
            // 修复②：end 分支可能在样式重算期间按同序临时放锁再重取，故用 unique_lock
            std::unique_lock<std::mutex> cycle(strokeCycleMtx);
            std::unique_lock<std::mutex> lk(docMutex);
            for (const StrokePoint& p : batch) {
                if (p.begin) {
                    if (draft != nullptr) {
                        continue;  // 上一笔未提交（异常序列）：丢弃新 begin 防串笔
                    }
                    if (strokeSuspend_) {
                        continue;  // D2：切换/关闭文档窗口期笔画不落错档
                    }
                    // M10d：快速蒙版模式（doc.quickMask 非 null）= 落笔目标即通道，
                    // 由文档状态推导（撤销跨 Q 边界回退时自动跟随，无独立标志位失同步）
                    const bool qm = doc().quickMask != nullptr;
                    // M19a：修复族落笔语义（QM/蒙版目标不可修复——降级普通笔）
                    HealKind healKind = HealKind::None;
                    if (!qm && !brushOnMask && brushHealKind > 0) {
                        healKind = static_cast<HealKind>(brushHealKind);
                    }
                    // M11d：克隆模式 = 图章工具激活 + 克隆源已设（QM 优先；蒙版目标与克隆互斥，
                    // UI 侧切换工具时已互斥置位）
                    StrokeDraft::CloneParams cloneParams;
                    const bool useClone = !qm && brushIsClone && cs().active &&
                                          cs().layerId != 0;
                    // M11f 历史画笔：源 = History steps 步前快照的同层像素（1:1 无偏移恢复，
                    // 复用克隆管线；克隆工具优先——两者互斥由 UI 切换保证）
                    bool useHistoryBrush = false;
                    if (!qm && !useClone && historyBrushSteps > 0) {
                        auto snap = history().layerPixelsStepsBack(historyBrushSteps, doc().activeId);
                        if (snap != nullptr) {
                            cloneParams.srcGrid = snap.get();
                            cloneParams.srcOriginX = 0;
                            cloneParams.srcOriginY = 0;
                            cloneParams.offsetX = 0.0;
                            cloneParams.offsetY = 0.0;
                            cloneParams.restoreMode = true;  // 含 alpha 还原（快照透明可恢复）
                            useHistoryBrush = true;
                        } else {
                            OH_LOG_Print(LOG_APP, LOG_WARN, 0x4D30, "Montage.HistoryBrush",
                                         "no snapshot at steps=%d for layer, fallback to brush",
                                         historyBrushSteps);
                        }
                    }
                    // P1 图案图章：源 = 图案库网格（wrap 采样；无克隆源/偏移语义，相位锚定
                    // 文档原点）。与克隆/历史画笔/修复族互斥（bridge 切换时清位，此处再护栏）。
                    bool usePattern = false;
                    if (!qm && !useClone && !useHistoryBrush && brushIsPattern &&
                        patternStampGrid != nullptr && patternStampW > 0 && patternStampH > 0 &&
                        healKind == HealKind::None) {
                        cloneParams.srcGrid = patternStampGrid.get();
                        cloneParams.patternMode = true;
                        cloneParams.patW = static_cast<int>(patternStampW);
                        cloneParams.patH = static_cast<int>(patternStampH);
                        cloneParams.patScale = patternStampScale;
                        usePattern = true;
                    }
                    // 修复画笔与图章共用克隆源/偏移机制（photocraft clone_mapping 同源）
                    const bool healNeedsSrc = healKind == HealKind::Heal;
                    if (useClone || healNeedsSrc) {
                        const Layer* srcLayer = nullptr;
                        for (const Layer& l : doc().layers) {
                            if (l.id == cs().layerId) {
                                srcLayer = &l;
                                break;
                            }
                        }
                        if (srcLayer != nullptr && srcLayer->pixels != nullptr) {
                            cloneParams.srcGrid = srcLayer->pixels.get();
                            cloneParams.srcOriginX =
                                static_cast<int>(std::lround(srcLayer->transform.originX));
                            cloneParams.srcOriginY =
                                static_cast<int>(std::lround(srcLayer->transform.originY));
                            // 对齐：首个克隆笔画锁定偏移持续复用；非对齐：每笔重置为源点起步
                            if (cs().aligned && cs().hasOffset) {
                                cloneParams.offsetX = cs().lockOffsetX;
                                cloneParams.offsetY = cs().lockOffsetY;
                            } else {
                                cloneParams.offsetX = p.x - cs().srcX;
                                cloneParams.offsetY = p.y - cs().srcY;
                                if (cs().aligned) {
                                    cs().lockOffsetX = cloneParams.offsetX;
                                    cs().lockOffsetY = cloneParams.offsetY;
                                    cs().hasOffset = true;
                                }
                            }
                        } else {
                            if (healNeedsSrc) {
                                OH_LOG_Print(LOG_APP, LOG_WARN, 0x4D30, "Montage.Heal",
                                             "healing brush without clone source, fallback to brush");
                                healKind = HealKind::None;  // 无源降级普通笔
                            } else {
                                OH_LOG_Print(LOG_APP, LOG_WARN, 0x4D30, "Montage.Clone",
                                             "clone source layer missing, fallback to brush");
                            }
                        }
                    }
                    history().beginEdit(doc(), healKind == HealKind::Heal ? "healing brush"
                                        : healKind != HealKind::None ? "spot healing"
                                        : useHistoryBrush ? "history brush"
                                        : useClone ? "clone stamp"
                                        : usePattern ? "pattern stamp"
                                        : qm ? "quick mask"
                                        : (brushOnMask ? "mask brush"
                                          : (brush.erasing ? "eraser" : "brush")),
                                      static_cast<uint64_t>(docRevision()));
                    draft = std::make_unique<StrokeDraft>();
                    ++draftGen_;  // 渲染快照去重状态换代（copyStrokeSnapshotLocked 侦测）
                    std::string err;
                    if (!draft->begin(doc(), doc().activeId, brush, !qm && brushOnMask, qm,
                                      (useClone || useHistoryBrush ||
                                       healKind == HealKind::Heal) ? &cloneParams : nullptr,
                                      err, qm ? BrushEffect::None : brushEffect, healKind)) {
                        OH_LOG_Print(LOG_APP, LOG_WARN, 0x4D30, "Montage.Brush",
                                     "draft begin failed: %{public}s", err.c_str());
                        history().endEdit(doc(), static_cast<uint64_t>(docRevision()));
                        draft.reset();
                        continue;
                    }
                    draft->append(p.x, p.y, p.pressure);
                } else if (p.end) {
                    if (draft == nullptr) {
                        continue;  // 陈旧 end 标记（背靠背笔画残留）
                    }
                    commitDraftLocked(lk, cycle, true);
                } else if (draft != nullptr) {
                    draft->append(p.x, p.y, p.pressure);
                }
            }
            requestRender();
        }
    }
}

// 在途 draft 提交（end 分支与 switchDoc/closeDocAt 强制收尾共用）。须持 cycle+doc 锁。
// 持 shared_ptr 引用：allowUnlock 放锁窗口内会话可能被切换/关闭——引用保证内存活，
// 复核 &active() != sess.get() 时丢弃发布（账目只写死会话的 history，无副作用）。
void Engine::commitDraftLocked(std::unique_lock<std::mutex>& lk,
                               std::unique_lock<std::mutex>& cycle, bool allowUnlock) {
    std::shared_ptr<DocSession> sess = docs[activeIdx];  // 收尾时 active 尚未切换（调用方保证）
    DocSession& s = *sess;
    draft->end();
    if (draft->qmTarget()) {
        // M10d：提交到快速蒙版通道（COW 换网格）。必须以原网格为合并基底
        //（nullptr 会丢未触碰瓦片——M5b-1 实证教训）
        auto merged = draft->commitGrid(s.doc.quickMask.get());
        if (merged != nullptr) {
            s.doc.quickMask = std::move(merged);
        }
    } else if (draft->maskTarget()) {
        // M5b：提交到蒙版 patch 网格（COW 换 LayerMask）+ 重合成
        // 必须以原 patch 网格为合并基底（nullptr 会丢未触碰瓦片 → patch 缺行）
        const LayerId lid = draft->layerId();
        for (Layer& l : s.doc.layers) {
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
        // 提交（对拍 commit：工作瓦片合并入图层 grid）。
        // P2-1：原实现先 commitGrid(nullptr) 判空再真合并——两次全量
        // 构建（paintTile + 逐瓦片拷贝），第一次结果整体丢弃
        const LayerId lid = draft->layerId();
        for (Layer& l : s.doc.layers) {
            if (l.id == lid) {
                l.pixels = draft->commitGrid(l.pixels.get());
                // M5a：蒙版启用图层提交后重合成派生网格
                if (l.mask != nullptr && l.mask->enabled) {
                    l.render = composeMasked(l.pixels, *l.mask);
                }
                break;
            }
        }
    }
    // M14c：提交像素变更后重算带样式/填充层派生渲染（样式实时跟随笔画）。
    // 修复②：样式重算是秒级 CPU（模糊/膨胀全域）——持 docMutex 会把渲染
    // 线程冻在 doFrame 等锁处，样式层每笔收尾一次（8 连笔 10fps 实测主因）。
    // 有样式层时改走「CoW 快照 → 放锁重算 → 重取锁身份护栏发布」；endEdit
    // 移到发布后，undo/redo 快照仍自带最终 styledRender（与原时序一致）。
    // 放锁窗口内 resetDocument/其他事务可能插入：发布护栏 + endEdit 的
    // pendingFlag_ 守卫保证不落错账（窗口内他端 beginEdit 会并入本条目，
    // 与既有嵌套语义同类）。
    bool needRestyle = false;
    for (const Layer& l : s.doc.layers) {
        if (l.style != nullptr || l.fill != nullptr) {
            needRestyle = true;
            break;
        }
    }
    if (needRestyle) {
        Document work = s.doc;  // CoW：瓦片 shared_ptr 共享，O(层+瓦)指针拷贝
        draft.reset();
        if (allowUnlock) {
            lk.unlock();
            cycle.unlock();
            restyleDocument(work);
            cycle.lock();
            lk.lock();
            // 放锁窗口内 active 可能被切换/重置：按代次护栏丢弃（不落错档）
            if (docs.empty() || &active() != sess.get()) {
                return;
            }
            publishStyledLocked(work);
            // 窗口内他端事务可能已并账：直接 endEdit 会撞他的 pending——由
            // history.endEdit 的 pendingFlag_ 守卫吸收（合并/丢弃同旧语义）
            s.history.endEdit(s.doc, static_cast<uint64_t>(s.revision + 1));
            bumpRevisionLocked(&s);
        } else {
            restyleDocument(s.doc);
            s.history.endEdit(s.doc, static_cast<uint64_t>(s.revision + 1));
            bumpRevisionLocked(&s);
            draft.reset();
        }
    } else {
        restyleDocument(s.doc);  // 无样式/填充层：空转遍历，保持锁内原路径
        s.history.endEdit(s.doc, static_cast<uint64_t>(s.revision + 1));
        bumpRevisionLocked(&s);
        draft.reset();
    }
    historyVersion.fetch_add(1);
}

Engine::StrokeSnapshot Engine::copyStrokeSnapshotLocked() {
    StrokeSnapshot snap;
    if (draft == nullptr || !draft->active()) {
        return snap;
    }
    // 修复①：草稿换代（新笔画）→ 去重状态作废，首帧全量重建
    if (strokeSnapGen_ != draftGen_) {
        strokeSnapGen_ = draftGen_;
        strokeSnapSeen_.clear();
    }
    if (draft->qmTarget()) {
        // M10d：快速蒙版草稿 = 文档域通道灰度（work 已并 base），渲染侧直接替换该瓦片
        snap.active = true;
        snap.quickMask = true;
        snap.gridCols = draft->gridCols();
        snap.gridRows = draft->gridRows();
        // 尾段还原撤出的瓦片：seen 中已不在 touched_ → 通知渲染器删纹理（残影）
        for (auto sit = strokeSnapSeen_.begin(); sit != strokeSnapSeen_.end();) {
            if (draft->touchedTiles().count(sit->first) == 0) {
                snap.erased.push_back(sit->first);
                sit = strokeSnapSeen_.erase(sit);
            } else {
                ++sit;
            }
        }
        for (const uint32_t key : draft->touchedTiles()) {
            const uint32_t rev = draft->tileRev(key % draft->gridCols(), key / draft->gridCols());
            auto seen = strokeSnapSeen_.find(key);
            if (seen != strokeSnapSeen_.end() && seen->second == rev) {
                continue;  // 自上帧未变：渲染器沿用已上传纹理，不重复拷贝 256KB
            }
            std::vector<uint8_t> px;
            if (draft->tileFor(key % draft->gridCols(), key / draft->gridCols(), px)) {
                snap.tiles[key] = std::move(px);
                strokeSnapSeen_[key] = rev;
            }
        }
        return snap;
    }
    snap.layerId = draft->layerId();
    snap.active = true;
    const Layer* layer = nullptr;
    for (const Layer& l : doc().layers) {
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
        // 多 patch 瓦可覆盖同一像素瓦（offset 非对齐）→ 输出与来源非一 一映射，
        // 不做增量去重，保持每帧全量（蒙版笔画低频路径）。
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
                            //（256 对齐瓦片在 content 外的样本是 0，直采会误隐 outside 区）
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
    // 尾段还原撤出的瓦片 → 通知渲染器删纹理（同 QM 分支）
    for (auto sit = strokeSnapSeen_.begin(); sit != strokeSnapSeen_.end();) {
        if (draft->touchedTiles().count(sit->first) == 0) {
            snap.erased.push_back(sit->first);
            sit = strokeSnapSeen_.erase(sit);
        } else {
            ++sit;
        }
    }
    for (const uint32_t key : draft->touchedTiles()) {
        const uint32_t tx = key % draft->gridCols();
        const uint32_t ty = key / draft->gridCols();
        const uint32_t rev = draft->tileRev(tx, ty);
        auto seen = strokeSnapSeen_.find(key);
        if (seen != strokeSnapSeen_.end() && seen->second == rev) {
            continue;  // 自上帧未变：渲染器沿用已上传纹理，不重复拷贝 256KB
        }
        std::vector<uint8_t> px;
        if (draft->tileFor(tx, ty, px)) {
            if (mask != nullptr) {
                applyMaskToTile(*mask, px, tx, ty);
            }
            snap.tiles[key] = std::move(px);
            strokeSnapSeen_[key] = rev;
        }
    }
    return snap;
}

// 修复②：锁外重算结果的发布（须持 docMutex）。身份护栏：锁外窗口内层被撤销/像素重换
//（pixels 身份变化）、蒙版变更、样式/填充替换任一发生 → 本次结果不发布（撤销恢复的
// 快照自带与像素配套的 styledRender；其余情形由下一次样式触发点重算覆盖）。
void Engine::publishStyledLocked(const Document& work) {
    for (const Layer& wl : work.layers) {
        if (wl.style == nullptr && wl.fill == nullptr) {
            continue;
        }
        for (Layer& l : doc().layers) {
            if (l.id != wl.id) {
                continue;
            }
            if (wl.pixels == l.pixels && wl.mask == l.mask && wl.style == l.style &&
                wl.fill == l.fill) {
                l.styledRender = wl.styledRender;
                l.styledOriginX = wl.styledOriginX;
                l.styledOriginY = wl.styledOriginY;
            }
            break;
        }
    }
}

// 重置 active 会话（关/新/整档替换统一入口——io 三路径在 prepareOpenSlot 落位后
// 调这里清「该会话」；其他会话与历史原样保留）。调用方不得持 docMutex 进入。
// 审查修复（P0-1/P0-3/P1-3）：此前仅 project_open/psd_import 清 history，新建/关闭/
// 导入图片三条路径都不清——Ctrl+Z 会把上一文档整档恢复（撤销跨文档）；且在途笔画
// 残留会用旧档 before + 新档 after 造出跨文档撤销条目，必须连同队列与 draft 一并作废。
// 锁序与笔画线程处理段一致（cycle → doc），与 cv.wait 无交互，无死锁环。
void Engine::resetDocument() {
    {
        std::lock_guard<std::mutex> lk(strokeMtx);
        strokeQuit = true;
        strokeQueue.clear();  // 在途点/end 标记作废（跨文档笔画不落新档）
        strokeSuspend_ = true;
        strokeCv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(import.mtx);
        import.cancel = true;
    }
    {
        // 与笔画线程 batch 处理段互斥：等其在途 batch 落完再清（线程随后见 quit 空队列退出，
        // 下次 BeginStroke 经 ensureStrokeThreadLocked 重建）
        std::lock_guard<std::mutex> cycle(strokeCycleMtx);
        std::lock_guard<std::mutex> lk(docMutex);
        draft.reset();
        DocSession& s = active();
        s.doc = Document{};
        s.viewport = Viewport{};
        s.history.clear();  // 撤销不许跨会话回退（连带复位 pendingFlag_，堵跨会话事务）
        s.revision = 0;
        s.epoch++;  // 该会话代次递增：在途异步发布（bake 等）凭旧代次作废
        s.savedRevision = 0;
        s.filePath.clear();
        s.textEditOpen = false;  // M12：编辑会话随文档清空
        s.textEditLayerId = 0;
        s.filterPreview = FilterPreview{};  // M13：预览会话随文档清空
        s.lastSelection.reset();  // M10a：重新选择来源不跨文档
        s.transformPreview = TransformPreview{};
        s.rawDevelop = RawDevelopSession{};  // M15e：RAW 显影会话随文档清空
        s.camPreview = CameraPreviewSession{};  // M23c：Camera Raw 预览会话随文档清空
        // 落笔目标标志随文档清空（否则新文档上蒙版目标笔画静默拒绝）
        brushOnMask = false;
        brushIsClone = false;
        brushEffect = BrushEffect::None;
        historyBrushSteps = 0;
        brushIsPattern = false;  // P1：图案图章随文档清（图案库进程级保留）
        patternStampGrid.reset();
        for (CloneSource& s2 : cloneSlots) { s2 = CloneSource{}; }
        smart::clearSessionCaches();  // M21：智能对象源解码/placed 缓存随会话清（v1 全局清，登记精细化）
        historyVersion.fetch_add(1);  // 历史面板随清空刷新（M15b）
        needsRender.store(true);
    }
    {
        std::lock_guard<std::mutex> sm(strokeMtx);
        strokeSuspend_ = false;
    }
    render.wake();
}

}  // namespace montage
