#ifndef MONTAGE_RENDER_LOOP_H
#define MONTAGE_RENDER_LOOP_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <native_vsync/native_vsync.h>
#include <native_window/external_window.h>

#include "render/tile_renderer.h"

namespace montage {

// T2 渲染：OH_NativeVSync 驱动。M0 实测：回调不在创建线程执行——GL 上下文由首帧回调线程
// 惰性绑定（ensureCurrent），初始化线程不保 current，GL 销毁也在回调线程（01 §7 D6 修订）。
// 帧调度（03 §2）：needsRender 为真才绘制+swap；无脏不重请求帧（空闲停帧），命令/导入经
// wake() 唤醒。
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
    // 任意线程：请求下一帧（幂等；空闲停帧后的唤醒入口）
    void wake();

private:
    void threadMain(uint64_t surfaceId);
    static void frameCallback(long long timestamp, void* data);
    void doFrame();
    bool ensureCurrent();
    void releaseGl();
    void failInit(const std::string& reason);
    void requestFrameLocked();

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

    std::mutex vsyncMtx_;
    std::atomic<bool> framePending_{false};
    TileRenderer renderer_;

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    OHNativeWindow* window_ = nullptr;
    OH_NativeVSync* vsync_ = nullptr;
    std::thread::id currentThread_{};
};

}  // namespace montage

#endif  // MONTAGE_RENDER_LOOP_H
