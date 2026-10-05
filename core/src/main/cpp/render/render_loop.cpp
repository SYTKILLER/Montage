#include "render/render_loop.h"

#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

#include <EGL/eglext.h>
#include <hilog/log.h>

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
    if (OH_NativeVSync_RequestFrame(vsync_, &RenderLoop::frameCallback, this) != 0) {
        failInit("OH_NativeVSync_RequestFrame failed");
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

    while (true) {
        std::unique_lock<std::mutex> lk(lifecycleMtx_);
        if (stopCv_.wait_for(lk, std::chrono::milliseconds(500), [this] { return stop_.load(); })) {
            break;
        }
    }
    // 等 vsync 回调侧完成 GL 清理（其落点线程可能不是本线程）
    for (int i = 0; i < 200 && !glReleased_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (vsync_ != nullptr) {
        OH_NativeVSync_Destroy(vsync_);
        vsync_ = nullptr;
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
    if (stop_.load()) {
        releaseGl();
        return;
    }
    if (!ensureCurrent()) {
        OH_NativeVSync_RequestFrame(vsync_, &RenderLoop::frameCallback, this);
        return;
    }

    int32_t h = 0;
    int32_t w = 0;
    if (OH_NativeWindow_NativeWindowHandleOpt(window_, GET_BUFFER_GEOMETRY, &h, &w) == 0 && w > 0 &&
        h > 0) {
        glViewport(0, 0, w, h);
        std::lock_guard<std::mutex> lk(statsMtx_);
        width_ = w;
        height_ = h;
    }

    glClearColor(0.10f, 0.42f, 0.85f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
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
    OH_NativeVSync_RequestFrame(vsync_, &RenderLoop::frameCallback, this);
}

void RenderLoop::releaseGl() {
    if (glReleased_.exchange(true)) {
        return;
    }
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }
    if (context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(display_, context_);
        context_ = EGL_NO_CONTEXT;
    }
    currentThread_ = std::thread::id();
    OH_LOG_Print(kLogType, LOG_INFO, kDomain, kTag, "GL released on frame thread");
}

}  // namespace montage
