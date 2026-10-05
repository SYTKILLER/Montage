#ifndef MONTAGE_ENGINE_ENGINE_H
#define MONTAGE_ENGINE_ENGINE_H

// 引擎会话单例（02 §5 Session 的 M1 子集）：文档 + 视口 + 渲染请求位 + 导入任务 + 事件 TSFN。
// 并发规则（02 §7）：一把粗锁 docMutex；T3 导入线程锁外计算、带边界加锁发布。

#include <atomic>
#include <mutex>

#include <napi/native_api.h>

#include "engine/document.h"
#include "render/render_loop.h"

namespace montage {

struct ImportJob {
    std::mutex mtx;
    bool running = false;
    bool cancel = false;
    double progress = 0.0;  // 0..1，最近一次带完成进度
};

class Engine {
  public:
    static Engine& get();

    std::mutex docMutex;
    Document doc;
    Viewport viewport;
    std::atomic<bool> needsRender{true};

    ImportJob import;

    RenderLoop render;  // T2 渲染循环（回调线程惰性绑 GL）

    int64_t docRevision = 0;
    napi_threadsafe_function revisionTsfn = nullptr;   // docVersion 事件
    napi_threadsafe_function progressTsfn = nullptr;   // importProgress 事件（payload=permille intptr）

    // 状态变更后调用：置脏 + 唤一帧（可在任意线程）
    void requestRender();

    // 锁内 bump 文档版本并经 TSFN 通知（须持有 docMutex）
    void bumpRevisionLocked();

    void resetDocument();  // 关闭文档：取消导入、清空、请求渲染

  private:
    Engine() = default;
};

}  // namespace montage

#endif  // MONTAGE_ENGINE_ENGINE_H
