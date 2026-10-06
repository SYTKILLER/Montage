#include "render/render_loop.h"

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
    return s;
}

void RenderLoop::wake() {
    // M1 实证：跨线程 OH_NativeVSync_RequestFrame 注册会静默丢失（回调不触发，framePending
    // 卡死吞掉后续请求）。帧循环改为回调线程内自续帧（03 §2 的"空闲停请求"降级为"空闲跳绘"，
    // LTPO 停帧留真机阶段经 SurfaceHolder 生命周期/降频实现），wake 因此退化为空操作。
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
    framePending_.store(false);
    if (stop_.load()) {
        releaseGl();
        return;
    }
    if (!ensureCurrent()) {
        return;  // 下次命令 wake 重试
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

    // 03 §2（M1 降级实现）：无脏跳过绘制与 swap；帧循环由回调线程自续（跨线程请求实证不可靠）。
    // 空闲成本 = vsync 空回调，省电的"停请求"待真机阶段实现。
    // M7b：选区存在时蚂蚁线持续动画 → 视作常脏（LTPO 分档留真机阶段）
    bool dirty = e.needsRender.exchange(false);
    if (!dirty) {
        std::lock_guard<std::mutex> lk(e.docMutex);
        dirty = e.doc.selection != nullptr;
    }
    if (!dirty) {
        OH_NativeVSync_RequestFrame(vsync_, &RenderLoop::frameCallback, this);
        return;
    }

    Document docSnap;
    Viewport vp;
    Engine::StrokeSnapshot strokeSnap;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        docSnap = e.doc;  // shared_ptr 拷贝（02 §7 帧快照）
        vp = e.viewport;
        strokeSnap = e.copyStrokeSnapshotLocked();
    }
    TileRenderer::StrokeOverlay overlay;
    if (strokeSnap.active) {
        overlay.layerId = strokeSnap.layerId;
        overlay.gridCols = strokeSnap.gridCols;
        overlay.gridRows = strokeSnap.gridRows;
        overlay.tiles = &strokeSnap.tiles;
    }

    renderer_.ensureInit();
    renderer_.drawFrame(docSnap, vp, w, h, strokeSnap.active ? &overlay : nullptr);

    if (eglSwapBuffers(display_, surface_) != EGL_TRUE) {
        std::lock_guard<std::mutex> lk(statsMtx_);
        lastError_ = "eglSwapBuffers failed";
    } else {
        std::lock_guard<std::mutex> lk(statsMtx_);
        const auto now = std::chrono::steady_clock::now();
        framesTotal_++;
        fpsWindowFrames_++;
        const double dt = std::chrono::duration<double>(now - fpsWindowStart_).count();
        if (dt >= 2.0) {
            fps_ = static_cast<double>(fpsWindowFrames_) / dt;
            fpsWindowFrames_ = 0;
            fpsWindowStart_ = now;
        }
    }
    OH_NativeVSync_RequestFrame(vsync_, &RenderLoop::frameCallback, this);  // 回调线程内自续
}

void RenderLoop::releaseGl() {
    if (glReleased_.exchange(true)) {
        return;
    }
    // 上下文若正被回调线程 current，由该线程调用本函数释放；空闲停帧时由 T2 兜底（未 current 可移动）
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    renderer_.invalidate();
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
