#include "render/render_loop.h"

#include "engine/text_raster.h"

#include <cstring>
#include <sstream>
#include <thread>

#include <EGL/eglext.h>
#include <hilog/log.h>

#include "engine/engine.h"

namespace montage {
namespace {
constexpr LogType kLogType = LOG_APP;
constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Render";

std::string threadIdString(const std::thread::id& id) {
    std::ostringstream oss;
    oss << id;
    return oss.str();
}
}  // namespace

RenderLoop::~RenderLoop() {
    stop();
}

bool RenderLoop::start(uint64_t surfaceId, std::string* errorOut) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    if (thread_.joinable()) {
        if (errorOut != nullptr) {
            *errorOut = "render loop already running";
        }
        return false;
    }
    stop_.store(false);
    running_.store(false);
    glReleased_.store(false);
    framePending_.store(false);
    {
        std::lock_guard<std::mutex> initLk(initMtx_);
        initDone_ = false;
        initOk_ = false;
        initError_.clear();
    }
    thread_ = std::thread(&RenderLoop::threadMain, this, surfaceId);
    std::unique_lock<std::mutex> initLk(initMtx_);
    const bool signaled =
        initCv_.wait_for(initLk, std::chrono::seconds(3), [this] { return initDone_; });
    if (!signaled || !initOk_) {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (errorOut != nullptr) {
            *errorOut = initError_.empty() ? "render init timeout(3s)" : initError_;
        }
        return false;
    }
    running_.store(true);
    return true;
}

void RenderLoop::stop() {
    std::unique_lock<std::mutex> lk(lifecycleMtx_);
    if (!thread_.joinable()) {
        return;
    }
    stop_.store(true);
    stopCv_.notify_all();
    {
        // M16a：回调线程可能正挂起在 paceCv（锁序 lifecycleMtx_→paceMtx_，无反向）
        std::lock_guard<std::mutex> paceLk(paceMtx_);
        paceCv_.notify_all();
    }
    lk.unlock();
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false);
}

RenderLoop::Snapshot RenderLoop::snapshot() const {
    std::lock_guard<std::mutex> lk(statsMtx_);
    Snapshot s;
    s.running = running_.load();
    s.frames = framesTotal_;
    s.fps = fps_;
    s.width = width_;
    s.height = height_;
    s.lastError = lastError_;
    s.callbackThread = callbackThread_;
    s.fullFrames = statFullFrames_;
    s.overlayFrames = statOverlayFrames_;
    s.paceTier = paceTier_.load();
    s.idleParks = idleParks_.load();
    s.callbacks = callbacksTotal_.load();
    return s;
}

void RenderLoop::wake() {
    // M16a governor 唤醒入口（03 §2"空闲停请求"转正，取代 M1 降级"空闲跳绘"）：
    // 回调线程在 paceCv 上挂起（parkBeforeFrame），needsRender 已由 requestRender 先行置位；
    // 此处持 paceMtx_ 通知——与挂起方的"持锁评估谓词"配对，堵「谓词评估与通知交错」丢唤醒竞态。
    // 不在此处直接 RequestFrame：M1 实证跨线程注册静默丢失，请求一律由回调线程自己做。
    std::lock_guard<std::mutex> lk(paceMtx_);
    paceCv_.notify_all();
}

void RenderLoop::requestFrameLocked() {
    if (vsync_ == nullptr || stop_.load()) {
        return;
    }
    if (framePending_.exchange(true)) {
        return;  // 已有一帧在途
    }
    const int rc = OH_NativeVSync_RequestFrame(vsync_, &RenderLoop::frameCallback, this);
    if (rc != 0) {
        framePending_.store(false);
        OH_LOG_Print(kLogType, LOG_ERROR, kDomain, kTag, "RequestFrame rc=%{public}d", rc);
    }
}

void RenderLoop::failInit(const std::string& reason) {
    OH_LOG_Print(kLogType, LOG_ERROR, kDomain, kTag, "init failed: %{public}s", reason.c_str());
    {
        std::lock_guard<std::mutex> lk(initMtx_);
        initDone_ = true;
        initOk_ = false;
        initError_ = reason;
    }
    initCv_.notify_all();
}

void RenderLoop::threadMain(uint64_t surfaceId) {
    int32_t rc = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window_);
    if (rc != 0 || window_ == nullptr) {
        failInit("create native window from surfaceId failed rc=" + std::to_string(rc));
        return;
    }

    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        failInit("eglGetDisplay failed");
        return;
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(display_, &major, &minor) != EGL_TRUE) {
        failInit("eglInitialize failed");
        return;
    }

    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE};
    EGLConfig config = nullptr;
    EGLint numConfigs = 0;
    if (eglChooseConfig(display_, configAttribs, &config, 1, &numConfigs) != EGL_TRUE ||
        numConfigs < 1) {
        failInit("eglChooseConfig: no usable config");
        return;
    }
    const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, ctxAttribs);
    if (context_ == EGL_NO_CONTEXT) {
        failInit("eglCreateContext failed");
        return;
    }
    surface_ =
        eglCreateWindowSurface(display_, config, reinterpret_cast<EGLNativeWindowType>(window_), nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        failInit("eglCreateWindowSurface failed");
        return;
    }
    // 不在初始化线程保持 current：GL 绑定哪条线程由 vsync 回调实际落点决定（doFrame 惰性绑定）。
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    vsync_ = OH_NativeVSync_Create("montage-render", static_cast<unsigned int>(strlen("montage-render")));
    if (vsync_ == nullptr) {
        failInit("OH_NativeVSync_Create failed");
        return;
    }

    {
        std::lock_guard<std::mutex> lk(initMtx_);
        initDone_ = true;
        initOk_ = true;
    }
    initCv_.notify_all();
    OH_LOG_Print(kLogType, LOG_INFO, kDomain, kTag,
                 "init ok: EGL %{public}d.%{public}d, initThread=%{public}s, surfaceId=%{public}llu",
                 major, minor, threadIdString(std::this_thread::get_id()).c_str(),
                 static_cast<unsigned long long>(surfaceId));

    {
        std::lock_guard<std::mutex> lk(statsMtx_);
        fpsWindowStart_ = std::chrono::steady_clock::now();
        lastError_.clear();
    }
    requestFrameLocked();  // 拉起首帧（脏驱动，Engine 初值 needsRender=true）

    while (true) {
        std::unique_lock<std::mutex> lk(lifecycleMtx_);
        bool stopNow = stopCv_.wait_for(lk, std::chrono::milliseconds(200),
                                        [this] { return stop_.load(); });
        if (stopNow) {
            break;
        }
        lk.unlock();
        // 帧调度由回调线程自续（见 wake 注释）；本循环只等待停止信号。
    }
    // 等 vsync 回调侧完成 GL 清理（其落点线程可能不是本线程）；空闲停帧时回调不再来，由本线程兜底
    for (int i = 0; i < 20 && !glReleased_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    releaseGl();
    {
        std::lock_guard<std::mutex> lk(vsyncMtx_);
        if (vsync_ != nullptr) {
            OH_NativeVSync_Destroy(vsync_);
            vsync_ = nullptr;
        }
    }
    if (window_ != nullptr) {
        OH_NativeWindow_DestroyNativeWindow(window_);
        window_ = nullptr;
    }
    OH_LOG_Print(kLogType, LOG_INFO, kDomain, kTag, "stopped, totalFrames=%{public}llu",
                 static_cast<unsigned long long>(snapshot().frames));
}

void RenderLoop::frameCallback(long long /*timestamp*/, void* data) {
    static_cast<RenderLoop*>(data)->doFrame();
}

bool RenderLoop::ensureCurrent() {
    if (currentThread_ == std::this_thread::get_id()) {
        return true;
    }
    if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
        std::lock_guard<std::mutex> lk(statsMtx_);
        lastError_ = "eglMakeCurrent failed on frame thread";
        return false;
    }
    currentThread_ = std::this_thread::get_id();
    {
        std::lock_guard<std::mutex> lk(statsMtx_);
        callbackThread_ = threadIdString(currentThread_);
    }
    OH_LOG_Print(kLogType, LOG_INFO, kDomain, kTag, "GL bound to frame thread=%{public}s",
                 threadIdString(currentThread_).c_str());
    return true;
}

void RenderLoop::doFrame() {
    callbacksTotal_.fetch_add(1);  // M16 仪表：vsync 回调次数 = CPU 唤醒次数（功耗量化直接代理）
    framePending_.store(false);
    if (stop_.load()) {
        releaseGl();
        return;
    }
    if (!ensureCurrent()) {
        parkAndSchedule();  // 下次命令 wake 重试（挂起等待，不再死循环空转）
        return;
    }
    auto& e = Engine::get();

    int32_t h = 0;
    int32_t w = 0;
    if (OH_NativeWindow_NativeWindowHandleOpt(window_, GET_BUFFER_GEOMETRY, &h, &w) == 0 && w > 0 &&
        h > 0) {
        bool sizeChanged = false;
        {
            std::lock_guard<std::mutex> lk(statsMtx_);
            if (width_ != w || height_ != h) {
                width_ = w;
                height_ = h;
                sizeChanged = true;
            }
        }
        if (sizeChanged) {
            e.needsRender.store(true);  // 尺寸变化强制重绘（视口重建）
        }
    }

    // 03 §2 帧调度（M16a governor 转正）：交互链=needsRender 驱动 60fps 自续；
    // 蚂蚁档=选区静置时挂起至下一 ~100ms tick 再请求（等效 10Hz overlayOnly，M15d 语义）；
    // 全闲档=无限挂起、零 vsync 回调，requestRender 经 paceCv 唤醒（见 parkAndSchedule）。
    // 旧实现的"每 vsync 空回调跳绘"与 M15d 的 60Hz 回调 10Hz 绘制均由此消除。
    bool dirty = e.needsRender.exchange(false);
    bool overlayOnly = false;
    if (!dirty) {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.selection != nullptr) {
            const auto now = std::chrono::steady_clock::now();
            if (!antsTickValid_ || now - lastAntsTick_ >= std::chrono::milliseconds(100)) {
                lastAntsTick_ = now;
                antsTickValid_ = true;
                overlayOnly = true;
            }
        }
    }
    if (!dirty && !overlayOnly) {
        parkAndSchedule();  // M16a：无工作——挂起（蚂蚁 tick / 命令唤醒），不再 60Hz 空转
        return;
    }

    Document docSnap;
    Viewport vp;
    Engine::StrokeSnapshot strokeSnap;
    Engine::TransformPreview transformPreview;
    bool viewXformDirtyLocal = false;  // M22
    Engine::ViewTransform vtLocal;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        docSnap = e.doc;  // shared_ptr 拷贝（02 §7 帧快照）
        vp = e.viewport;
        strokeSnap = e.copyStrokeSnapshotLocked();
        transformPreview = e.transformPreview;
        if (e.viewXformDirty) {  // M22：显示变换字节随帧快照锁内拷贝（防撕裂）
            e.viewXformDirty = false;
            viewXformDirtyLocal = true;
            vtLocal = e.viewXform;
        }
        // M12：文本编辑会话中隐藏该层（RichEditor overlay 承担编辑态视觉，防 GL+overlay 双影）
        if (e.textEditOpen && e.textEditLayerId != 0) {
            for (Layer& l : docSnap.layers) {
                if (l.id == e.textEditLayerId) {
                    l.pixels = nullptr;
                    l.render = nullptr;
                    break;
                }
            }
        }
        // M13：滤镜预览——该层像素换 1/4 proxy 网格 + 复用 M8.1 xform 矩阵放大显示
        if (e.filterPreview.active && e.filterPreview.proxy != nullptr) {
            static std::atomic<int> fpLogCount{0};
            if (fpLogCount.fetch_add(1) < 3) {
                OH_LOG_Print(LOG_APP, LOG_INFO, 0x4D30, "Montage.FilterRender",
                             "preview frame: layer=%{public}llu proxy %ux%u scale=%.1f",
                             static_cast<unsigned long long>(e.filterPreview.layerId),
                             e.filterPreview.proxy->cols * 256, e.filterPreview.proxy->rows * 256,
                             e.filterPreview.scale);
            }
            for (Layer& l : docSnap.layers) {
                if (l.id == e.filterPreview.layerId) {
                    l.pixels = e.filterPreview.proxy;
                    l.render = nullptr;  // 预览期不叠蒙版（登记）
                    l.mask = nullptr;
                    l.transform.originX = e.filterPreview.originX;
                    l.transform.originY = e.filterPreview.originY;
                    if (!transformPreview.active) {
                        const double s = e.filterPreview.scale;
                        const double ox = e.filterPreview.originX;
                        const double oy = e.filterPreview.originY;
                        transformPreview.active = true;
                        transformPreview.layerId = l.id;
                        transformPreview.m[0] = s;
                        transformPreview.m[1] = 0.0;
                        transformPreview.m[2] = ox - s * ox;
                        transformPreview.m[3] = 0.0;
                        transformPreview.m[4] = s;
                        transformPreview.m[5] = oy - s * oy;
                    }
                    break;
                }
            }
        }
    }
    TileRenderer::StrokeOverlay overlay;
    if (strokeSnap.active) {
        overlay.layerId = strokeSnap.layerId;
        overlay.gridCols = strokeSnap.gridCols;
        overlay.gridRows = strokeSnap.gridRows;
        overlay.tiles = &strokeSnap.tiles;
        overlay.erased = strokeSnap.erased;  // 修复①：撤出瓦片透传（渲染器删纹理）
        overlay.quickMask = strokeSnap.quickMask;  // M10d
    }

    renderer_.ensureInit();
    montage::warmUpTextRaster();  // M12：排版栈首帧预热（编辑期首触会撞 ArkUI 排版竞态）
    TileRenderer::LayerXformPreview xformPreview;
    if (transformPreview.active) {
        xformPreview.layerId = transformPreview.layerId;
        xformPreview.active = true;
        for (int i = 0; i < 6; ++i) {
            xformPreview.m[i] = transformPreview.m[i];
        }
    }
    // M22：显示变换字节上渲染器（dirty 才传；字节在 docSnap 锁内已拷贝到本地）
    if (viewXformDirtyLocal) {
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x4D30, "Montage.View", "upload: lutOn=%{public}d gamutOn=%{public}d",
                     vtLocal.lutOn ? 1 : 0, vtLocal.gamutOn ? 1 : 0);
        if (vtLocal.lutOn) {
            renderer_.setViewLut(vtLocal.lut.data(), vtLocal.lut.size(), vtLocal.lutSize);
        } else {
            renderer_.setViewLut(nullptr, 0, 0);
        }
        if (vtLocal.gamutOn) {
            renderer_.setGamutLut(vtLocal.gamut.data(), vtLocal.gamut.size(), vtLocal.gamutSize);
        } else {
            renderer_.setGamutLut(nullptr, 0, 0);
        }
    }
    renderer_.drawFrame(docSnap, vp, w, h, strokeSnap.active ? &overlay : nullptr,
                        transformPreview.active ? &xformPreview : nullptr, overlayOnly);

    if (eglSwapBuffers(display_, surface_) != EGL_TRUE) {
        std::lock_guard<std::mutex> lk(statsMtx_);
        lastError_ = "eglSwapBuffers failed";
    } else {
        std::lock_guard<std::mutex> lk(statsMtx_);
        const auto now = std::chrono::steady_clock::now();
        framesTotal_++;
        fpsWindowFrames_++;
        if (overlayOnly) {
            statOverlayFrames_++;
        } else {
            statFullFrames_++;
        }
        const double dt = std::chrono::duration<double>(now - fpsWindowStart_).count();
        if (dt >= 2.0) {
            fps_ = static_cast<double>(fpsWindowFrames_) / dt;
            fpsWindowFrames_ = 0;
            fpsWindowStart_ = now;
            static std::atomic<int> idleLog{0};
            if (idleLog.fetch_add(1) < 60) {  // M16a：放宽到 60 窗（2s/窗），覆盖整段验证会话
                OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                             "idle cadence: full=%{public}d overlay=%{public}d (2s window)\n",
                             statFullFrames_, statOverlayFrames_);
            }
            statFullFrames_ = 0;
            statOverlayFrames_ = 0;
        }
    }
    parkAndSchedule();  // 绘制趟也走 governor：交互中 needsRender 连续置位即 60fps 链，闲则挂起
}

// M16a 空闲停帧 governor——doFrame 全部尾部路径的统一出口，三分调度：
//  1) needsRender 已置（绘制期间又来命令/笔画）→ 立即自续（Active 档，60fps 链）；
//  2) 无脏但有选区 → 回调线程挂起至下一蚂蚁 tick（AntsPark 档，~10Hz，M15d 语义不变）；
//  3) 全闲 → 无限挂起（IdlePark 档，零 vsync 回调），requestRender→wake() 唤醒。
// 挂起发生在回调线程内部：不引入跨线程 RequestFrame（M1 实证注册静默丢失），且挂起本身
// 就是"停请求"——Idle 档下 NativeVSync 服务不再派发回调，功耗收益真机量化（M16 三件套）。
void RenderLoop::parkAndSchedule() {
    auto& e = Engine::get();
    if (e.needsRender.load()) {
        setPaceTier(PaceTier::Active);
        selfContinueFrame();  // P2-5：持 vsyncMtx_（与 threadMain 的 Destroy 互斥）
        return;
    }
    bool selectionLive = false;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        selectionLive = e.doc.selection != nullptr;
    }
    idleParks_.fetch_add(1);
    if (selectionLive) {
        setPaceTier(PaceTier::AntsPark);
    } else {
        setPaceTier(PaceTier::IdlePark);
    }
    if (parkBeforeFrame(selectionLive)) {
        releaseGl();  // stop_：与 doFrame 入口 stop 分支同款收尾（threadMain 兜底等待依赖此标志）
        return;
    }
    selfContinueFrame();
}

// 回调线程内挂起；antsPending=true 时至多睡到下一 ~100ms 蚂蚁 tick（对齐 doFrame 门拍），
// 醒来谓词仍假 = tick 到点。返回 true = stop_ 置位（勿再触碰 vsync）。
bool RenderLoop::parkBeforeFrame(bool antsPending) {
    std::unique_lock<std::mutex> lk(paceMtx_);
    const auto pred = [this] { return stop_.load() || Engine::get().needsRender.load(); };
    if (antsPending) {
        auto deadline = std::chrono::steady_clock::now();
        if (antsTickValid_) {
            deadline = lastAntsTick_ + std::chrono::milliseconds(100);
            const auto now = std::chrono::steady_clock::now();
            if (deadline < now) {
                deadline = now;
            }
        }
        paceCv_.wait_until(lk, deadline, pred);
    } else {
        paceCv_.wait(lk, pred);
    }
    return stop_.load();
}

void RenderLoop::setPaceTier(PaceTier t) {
    const int prev = paceTier_.exchange(static_cast<int>(t));
    if (prev == static_cast<int>(t)) {
        return;
    }
    static const char* kNames[] = {"active60", "ants10", "idle0"};
    // 档位迁移日志（M16 三件套验证依据）：500ms 限频，防交互抖动刷屏；
    // 附累计 frames/callbacks——回调数远小于帧间 vsync 数即停帧生效的差分证据
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    auto last = paceLogMs_.load();
    if (nowMs - last >= 500 && paceLogMs_.compare_exchange_strong(last, nowMs)) {
        OH_LOG_Print(kLogType, LOG_INFO, kDomain, kTag, "pace: %{public}s (frames=%{public}llu callbacks=%{public}llu)",
                     kNames[static_cast<int>(t)],
                     static_cast<unsigned long long>(framesTotal_),
                     static_cast<unsigned long long>(callbacksTotal_.load()));
    }
}

// P2-5：帧尾自续统一走这里——持 vsyncMtx_ 与 threadMain 停止路径的
// OH_NativeVSync_Destroy 互斥，并复查 stop_（消除「检查后销毁」竞态窗口下的
// 悬垂 vsync_ 访问）。回调线程内调用，不涉及 M1 记录的跨线程注册失效问题。
void RenderLoop::selfContinueFrame() {
    std::lock_guard<std::mutex> lk(vsyncMtx_);
    if (!stop_.load() && vsync_ != nullptr) {
        OH_NativeVSync_RequestFrame(vsync_, &RenderLoop::frameCallback, this);
    }
}

void RenderLoop::releaseGl() {
    if (glReleased_.exchange(true)) {
        return;
    }
    // 上下文若正被回调线程 current，由该线程调用本函数释放；空闲停帧时由 T2 兜底（未 current 可移动）
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    renderer_.invalidate();
    Engine::get().resetStrokeSnapSeen();  // overlay 纹理随上下文销毁：快照去重状态作废（下帧全量重建）
    if (surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }
    if (context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(display_, context_);
        context_ = EGL_NO_CONTEXT;
    }
    currentThread_ = std::thread::id();
    OH_LOG_Print(kLogType, LOG_INFO, kDomain, kTag, "GL released");
}

}  // namespace montage
