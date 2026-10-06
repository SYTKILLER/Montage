#ifndef MONTAGE_ENGINE_ENGINE_H
#define MONTAGE_ENGINE_ENGINE_H

// 引擎会话单例（02 §5 Session 的 M1 子集）：文档 + 视口 + 渲染请求位 + 导入任务 + 事件 TSFN。
// 并发规则（02 §7）：一把粗锁 docMutex；T3 导入线程锁外计算、带边界加锁发布。

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include <napi/native_api.h>

#include "engine/brush.h"
#include "engine/document.h"
#include "engine/history.h"
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

    // M3 笔画：命令入队（T1）→ 工作线程按序消费（begin 落 draft / append / end 提交）
    struct StrokePoint {
        float x = 0;
        float y = 0;
        float pressure = 1.0f;  // M3.1：∈[0,1]，调制 dab 直径（无压感设备传 1）
        bool begin = false;     // 开始标记：创建 draft + history.beginEdit（线程内，保证与提交同序）
        bool end = false;       // 结束标记：收尾曲线段 + 提交 + history.endEdit
    };
    std::mutex strokeMtx;
    std::condition_variable strokeCv;
    std::deque<StrokePoint> strokeQueue;
    bool strokeQuit = false;                // 常驻线程退出标记（dispose 置位）
    std::mutex strokeCycleMtx;              // 生命周期互斥：begin/end 处理段与 dispose 清理串行
    std::unique_ptr<StrokeDraft> draft;     // 绘制中的笔画（mutex 内访问）
    BrushSettings brush;                    // 当前笔刷参数（ArkTS setBrushSettings 写入）
    bool brushOnMask = false;               // M5b：落笔目标 = 活动图层蒙版（setBrushTarget 写入）
    std::thread strokeThread_;
    bool strokeThreadStarted_ = false;
    void ensureStrokeThreadLocked();        // 须持 strokeMtx；幂等，先 join 已退出线程
    void strokeThreadMain();

    // M4a 撤销
    History history;
    std::atomic<uint32_t> historyVersion{0};

    // 渲染帧快照携带的笔画瓦片（docMutex 内拷贝，避免渲染线程跨锁悬挂）
    struct StrokeSnapshot {
        LayerId layerId = 0;
        uint32_t gridCols = 0;
        uint32_t gridRows = 0;
        std::map<uint32_t, std::vector<uint8_t>> tiles;
        bool active = false;
    };
    StrokeSnapshot copyStrokeSnapshotLocked();

    int64_t docRevision = 0;
    napi_threadsafe_function revisionTsfn = nullptr;   // docVersion 事件
    napi_threadsafe_function progressTsfn = nullptr;   // importProgress 事件（payload=permille intptr）

    LayerId nextLayerId();  // 单调递增不回收（02 §1 ID 策略），锁外调用安全

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
