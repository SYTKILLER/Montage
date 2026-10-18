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
#include "engine/clipboard_ops.h"
#include "engine/document.h"
#include "engine/history.h"
#include "io/dng_decode.h"
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
    uint64_t draftGen_ = 0;                 // 草稿代次（每次新建 draft 递增；渲染快照去重状态换代依据）
    BrushSettings brush;                    // 当前笔刷参数（ArkTS setBrushSettings 写入）
    float fgColor[3] = {0.0f, 0.0f, 0.0f};  // M9c 前景色（直 alpha 0..1；起笔覆盖 brush 色）
    float bgColor[3] = {1.0f, 1.0f, 1.0f};  // M9c 背景色（渐变/填充 M11 用）
    bool brushOnMask = false;               // M5b：落笔目标 = 活动图层蒙版（setBrushTarget 写入）
    bool brushIsClone = false;              // M11d：仿制图章工具激活（setBrushClone 写入）
    BrushEffect brushEffect = BrushEffect::None;  // M11e：效果工具（setBrushEffect 写入）
    int historyBrushSteps = 0;              // M11f：历史画笔源步数（0=未激活；setHistoryBrush 写入）

    // M11d 仿制图章克隆源（会话态：不随快照/持久化走；Alt+点击设置）
    struct CloneSource {
        bool active = false;
        LayerId layerId = 0;   // 取源时的活动层（v1 同层/跨层均可，采样用源层原像素）
        double srcX = 0.0;     // 源点（文档域）
        double srcY = 0.0;
        bool aligned = true;   // 对齐：笔画间保持同一偏移映射；否则每笔重置为源点起步
        bool hasOffset = false; // 对齐模式：首个克隆笔画后锁定偏移
        double lockOffsetX = 0.0;
        double lockOffsetY = 0.0;
    };
    CloneSource cloneSource;
    std::thread strokeThread_;
    bool strokeThreadStarted_ = false;
    void ensureStrokeThreadLocked();        // 须持 strokeMtx；幂等，先 join 已退出线程
    void strokeThreadMain();

    // M8.1 自由变换预览（overlay 拖拽实时预览；docMutex 保护）
    struct TransformPreview {
        LayerId layerId = 0;
        bool active = false;
        double m[6] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0};  // doc→doc 行主序
    };
    TransformPreview transformPreview;

    // M10a 重新选择（PS 语义会话态）：最近一次取消的选区，reselectSelection 恢复。
    // 不随 History 快照走（撤销不恢复"重新选择"来源）。
    std::shared_ptr<const TileGrid> lastSelection;

    // M12 文本编辑会话（PS 语义：整个编辑会话 = 一步撤销）：begin 端 history.beginEdit，
    // 内容/样式变更走无事务 relayout，end 端 endEdit（before==after 自动丢弃）。
    // History 单 pending 槽——会话打开期间禁止其他事务命令（UI 纪律：runCommand 先退编辑）。
    bool textEditOpen = false;
    LayerId textEditLayerId = 0;

    // M13 滤镜预览会话（对话框打开期间）：proxy = 1/4 核结果网格；渲染快照把该层
    // pixels 换成 proxy 并复用 M8.1 xform 矩阵放大显示。endFilterPreview 后清理。
    struct FilterPreview {
        bool active = false;
        LayerId layerId = 0;
        std::shared_ptr<const TileGrid> proxy;
        double originX = 0.0;   // proxy 域文档原点（bbox 缩小域）
        double originY = 0.0;
        uint32_t fullW = 0;     // 全分辨率域尺寸（proxy × scale 显示）
        uint32_t fullH = 0;
        double scale = 4.0;
        std::vector<uint8_t> srcStrip;  // proxy 输入（1/4 降采样 strip）
        uint32_t srcW = 0;
        uint32_t srcH = 0;
    };
    FilterPreview filterPreview;

    // M15e：DNG 会话 RAW 缓存——显影参数面板数据源。导入 DNG 成功后缓存原始字节，
    // 面板重显影恒从 RAW 出发（会话内非破坏）；随 resetDocument 清、不入 .montage/History，
    // 导入下一个 DNG 覆盖（面板始终作用于最近导入的 RAW 层）。
    struct RawDevelopSession {
        std::vector<uint8_t> bytes;
        LayerId layerId = 0;
        uint32_t w = 0;
        uint32_t h = 0;
        io::DevelopParams last;  // 最近一次提交的参数（面板回显）
        bool valid = false;
    };
    RawDevelopSession rawDevelop;

    // M10c 内部剪贴板（引擎内存；不随快照/持久化走，关闭文档保留——PS 同语义）
    ClipBuffer clipboard;
    // M17a 样式剪贴板（copyLayerStyle/pasteLayerStyle；跨文档保留，同 ClipBuffer 语义）
    std::shared_ptr<LayerStyle> styleClipboard = nullptr;

    // M4a 撤销
    History history;
    std::atomic<uint32_t> historyVersion{0};
    // M17d 图案 id 计数（definePattern 锁内单调发号；openProject 按包内最大后缀回填防撞号）
    uint64_t patternSeq = 0;

    // 渲染帧快照携带的笔画瓦片（docMutex 内拷贝，避免渲染线程跨锁悬挂）。
    // 修复①（对拍 Compositor identity 去重）：tiles 只带自上帧以来 workRev_ 变化的瓦片，
    // 渲染器对未变瓦片沿用已上传纹理（strokeTex_/qmStrokeTex_ 常驻笔画期）；
    // erased = 自上帧被尾段还原撤出的瓦片（渲染器删纹理，尾段残影不再滞留）。
    struct StrokeSnapshot {
        LayerId layerId = 0;
        uint32_t gridCols = 0;
        uint32_t gridRows = 0;
        std::map<uint32_t, std::vector<uint8_t>> tiles;
        std::vector<uint32_t> erased;
        bool active = false;
        bool quickMask = false;  // M10d：草稿瓦片 = 快速蒙版通道灰度（渲染走红 overlay 替换）
    };
    StrokeSnapshot copyStrokeSnapshotLocked();

    // GL 上下文重建后由渲染线程调用：overlay 纹理已全部销毁，去重状态作废（下帧全量重建）
    void resetStrokeSnapSeen() {
        std::lock_guard<std::mutex> lk(docMutex);
        strokeSnapSeen_.clear();
        ++strokeSnapGen_;  // 与 draftGen_ 失配 → copyStrokeSnapshotLocked 首帧清态全量
    }

    // 修复①：渲染快照增量去重状态（仅渲染线程经 copyStrokeSnapshotLocked 持 docMutex 访问）
    uint64_t strokeSnapGen_ = 0;             // 已同步快照所属草稿代次
    std::map<uint32_t, uint32_t> strokeSnapSeen_;  // tile key → 上次交给渲染器的 workRev

    // 修复②：笔末样式重算移出 docMutex——锁外对 CoW 快照 restyleDocument 后，
    // 锁内按身份护栏发布（id 命中 + pixels/mask/style/fill 期间未变才换入 styledRender）
    void publishStyledLocked(const Document& work);

    int64_t docRevision = 0;
    napi_threadsafe_function revisionTsfn = nullptr;   // docVersion 事件
    napi_threadsafe_function progressTsfn = nullptr;   // importProgress 事件（payload=permille intptr）

    LayerId nextLayerId();  // 单调递增不回收（02 §1 ID 策略），锁外调用安全

    // 状态变更后调用：置脏 + 唤一帧（可在任意线程）
    void requestRender();

    // 锁内 bump 文档版本并经 TSFN 通知（须持有 docMutex）
    void bumpRevisionLocked();
    // 锁内只广播当前 docRevision（不递增）——撤销/重做恢复快照版本后用（M7.5）
    void fireRevisionLocked();

    void resetDocument();  // 关闭文档：取消导入、清空、请求渲染

  private:
    Engine() = default;
};

}  // namespace montage

#endif  // MONTAGE_ENGINE_ENGINE_H
