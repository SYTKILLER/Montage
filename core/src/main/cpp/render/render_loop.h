#ifndef MONTAGE_RENDER_LOOP_H
#define MONTAGE_RENDER_LOOP_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <native_vsync/native_vsync.h>
#include <native_window/external_window.h>

namespace montage {

// T2 渲染线程：独占 EGL 上下文，OH_NativeVSync 驱动清屏帧循环（M0 尖刺实现）。
// 线程模型（01 文档 §5/§7）：谁持有 GL 上下文由 vsync 回调实际落点决定——初始化在
// 自持 std::thread 上完成，首帧回调线程惰性 eglMakeCurrent 并绑定，运行时实测后定案。
class RenderLoop {
public:
    struct Snapshot {
        bool running = false;
        uint64_t frames = 0;
        double fps = 0.0;
        int32_t width = 0;
        int32_t height = 0;
        std::string lastError;
        std::string callbackThread;
    };

    RenderLoop() = default;
    ~RenderLoop();
    RenderLoop(const RenderLoop&) = delete;
    RenderLoop& operator=(const RenderLoop&) = delete;

    // 阻塞直至 GL 初始化完成（≤3s）；失败时 errorOut 返回原因。
    bool start(uint64_t surfaceId, std::string* errorOut);
    void stop();
    Snapshot snapshot() const;

private:
    void threadMain(uint64_t surfaceId);
    static void frameCallback(long long timestamp, void* data);
    void doFrame();
    bool ensureCurrent();
    void releaseGl();
    void failInit(const std::string& reason);

    std::thread thread_;
    std::mutex lifecycleMtx_;
    std::condition_variable stopCv_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> glReleased_{false};

    std::mutex initMtx_;
    std::condition_variable initCv_;
    bool initDone_ = false;
    bool initOk_ = false;
    std::string initError_;

    mutable std::mutex statsMtx_;
    uint64_t framesTotal_ = 0;
    double fps_ = 0.0;
    uint64_t fpsWindowFrames_ = 0;
    std::chrono::steady_clock::time_point fpsWindowStart_;
    int32_t width_ = 0;
    int32_t height_ = 0;
    std::string lastError_;
    std::string callbackThread_;

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    OHNativeWindow* window_ = nullptr;
    OH_NativeVSync* vsync_ = nullptr;
    std::thread::id currentThread_{};
};

}  // namespace montage

#endif  // MONTAGE_RENDER_LOOP_H
