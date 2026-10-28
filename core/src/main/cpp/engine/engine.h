#ifndef MONTAGE_ENGINE_ENGINE_H
#define MONTAGE_ENGINE_ENGINE_H

// 引擎会话单例（02 §5 Session 的 M1 子集）：文档池 + 视口 + 渲染请求位 + 导入任务 + 事件 TSFN。
// 并发规则（02 §7）：一把粗锁 docMutex；T3 导入线程锁外计算、带边界加锁发布。
// D2 多文档（P4 批）：文档从单例扩为 DocSession 池——doc/history/会话态随会话走；
// 桥接命令面经访问器作用 active 会话（语义与单文档期一致）；异步任务以 DocStamp
// 闩锁归属会话，发布段锁内复核（会话仍存活且未重置才落账）。

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

// M8.1 自由变换预览（overlay 拖拽实时预览；docMutex 保护）
struct TransformPreview {
    LayerId layerId = 0;
    bool active = false;
    double m[6] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0};  // doc→doc 行主序
};

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

// M15e：DNG 会话 RAW 缓存——显影参数面板数据源。导入 DNG 成功后缓存原始字节，
// 面板重显影恒从 RAW 出发（会话内非破坏）；随会话清、不入 .montage/History，
// 导入下一个 DNG 覆盖（面板始终作用于最近导入的 RAW 层）。
struct RawDevelopSession {
    std::vector<uint8_t> bytes;
    LayerId layerId = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    io::DevelopParams last;  // 最近一次提交的参数（面板回显）
    bool valid = false;
};

// M23c：Camera Raw 实时预览会话（显影面板/滤镜对话框打开期间）。首预览
// history.beginEdit 挂 pending，后续预览无事务换像素（fireRevision 通知 UI），
// 确定 endEdit 一步撤销 / 取消 takePending 还原。fromRaw=true 走 developRaw
// （RAW 字节重显影，target=rawDevelop.layerId）；false 走 cameraRawFilter
// （层滤镜：恒从打开时的像素快照出发）。seq = 预览代次，发布段失配即作废
// （防旧任务覆盖新参数）；snap = 会话打开时层像素（取消还原 + 层滤镜基底）。
struct CameraPreviewSession {
    bool open = false;
    bool fromRaw = false;
    LayerId layerId = 0;
    uint32_t seq = 0;
    std::shared_ptr<const TileGrid> snap;
};

// P5⑤：感知移动实时预览会话（move 工具 caMoveMode>0 拖拽期）。base = 拖拽起点的
// 层像素共享快照（COW 只读）；preview = 基底 + 选区内容平移 (dx,dy) 盖章的合成网格，
// 渲染快照按层替换（FilterPreview 同机制）。松手 end 后走既有 contentAwareMove 一步事务。
struct CAMovePreview {
    bool active = false;
    LayerId layerId = 0;
    std::shared_ptr<const TileGrid> base;
    std::shared_ptr<const TileGrid> preview;
};

// D2 多文档：一个打开的文档会话。池元素为 shared_ptr——异步任务按裸指针+epoch
// 闩锁归属，发布段经 docIfUnchanged 锁内复核；池移除后 shared_ptr 仍护活到复核完。
struct DocSession {
    uint64_t id = 0;             // 会话号（单调；TS 侧标签条/保存路径键）
    Document doc;
    History history;             // 每文档一实例（「撤销不跨文档」天然成立）
    Viewport viewport;           // 每文档视图状态（PS 标签语义：缩放/平移互不干扰）
    uint32_t revision = 0;       // 原 docRevision（快照恢复/异步复核语义不变）
    uint64_t epoch = 0;          // 原 docEpoch（该会话 resetDocument 递增）
    uint32_t savedRevision = 0;  // 最近落盘时 revision（标签条修改指示 = revision 差异）
    std::string filePath;        // 最近打开/保存路径（标签 tooltip）
    // 会话态（原 Engine 层成员，随 doc 走；切换标签各自保留）
    bool textEditOpen = false;
    LayerId textEditLayerId = 0;
    TransformPreview transformPreview;
    std::shared_ptr<const TileGrid> lastSelection;  // M10a 重新选择来源
    FilterPreview filterPreview;
    RawDevelopSession rawDevelop;
    CameraPreviewSession camPreview;
    CAMovePreview camMove;  // P5⑤ 感知移动实时预览（拖拽期）
};

class Engine {
  public:
    static Engine& get();

    std::mutex docMutex;
    // D2 多文档池（须 docMutex 变更；恒非空——关空后补一个空会话，兼容「无文档」语义）
    std::vector<std::shared_ptr<DocSession>> docs;
    size_t activeIdx = 0;
    uint64_t sessionSeq = 0;

    // ---------- active 会话访问器（须持 docMutex；旧成员名的函数化，桥接零语义漂移） ----------
    DocSession& active() { return *docs[activeIdx]; }
    Document& doc() { return active().doc; }
    History& history() { return active().history; }
    Viewport& viewport() { return active().viewport; }
    uint32_t& docRevision() { return active().revision; }
    uint64_t& docEpoch() { return active().epoch; }

    // D2 多文档命令面（napi 层用；锁外调用，内部自取锁）
    size_t prepareOpenSlot();          // 打开整档替换类文件前落位：active 空档复用，否则新建并激活
    bool switchDoc(size_t idx);        // 切换 active（强制收尾在途笔画 + 广播刷新）
    bool closeDocAt(size_t idx);       // 关闭指定会话（池空补空会话；关 active 邻位接管）
    bool switchDocById(uint64_t id);
    bool closeDocById(uint64_t id);
    uint64_t newSession(bool activate);  // 新建空会话（返回会话 id）；activate=false 供批处理 scratch（暂不激活）

    // 异步归属闩锁：任务起点持锁取，发布段持锁 docIfUnchanged 复核（同指针在池 +
    // epoch/revision 未变）。导入族用 docIfEpoch（允许并发编辑，仅防跨 resetDocument）。
    struct DocStamp {
        DocSession* session = nullptr;
        uint64_t epoch = 0;
        uint32_t revision = 0;
    };
    DocStamp stampDoc() {
        DocStamp s;
        s.session = &active();
        s.epoch = active().epoch;
        s.revision = active().revision;
        return s;
    }
    std::shared_ptr<DocSession> docIfUnchanged(const DocStamp& s);
    std::shared_ptr<DocSession> docIfEpoch(DocSession* ptr, uint64_t epoch);

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
    bool strokeSuspend_ = false;            // D2：切换/关闭文档窗口期丢弃 begin 标记（防落错档）
    std::mutex strokeCycleMtx;              // 生命周期互斥：begin/end 处理段与 dispose 清理串行
    std::unique_ptr<StrokeDraft> draft;     // 绘制中的笔画（mutex 内访问）
    uint64_t draftGen_ = 0;                 // 草稿代次（每次新建 draft 递增；渲染快照去重状态换代依据）
    BrushSettings brush;                    // 当前笔刷参数（ArkTS setBrushSettings 写入）
    float fgColor[3] = {0.0f, 0.0f, 0.0f};  // M9c 前景色（直 alpha 0..1；起笔覆盖 brush 色）
    float bgColor[3] = {1.0f, 1.0f, 1.0f};  // M9c 背景色（渐变/填充 M11 用）
    bool brushOnMask = false;               // M5b：落笔目标 = 活动图层蒙版（setBrushTarget 写入）
    bool brushIsClone = false;              // M11d：仿制图章工具激活（setBrushClone 写入）
    BrushEffect brushEffect = BrushEffect::None;  // M11e：效果工具（setBrushEffect 写入）
    int brushHealKind = 0;                  // M19a：修复族（0 无/1 修复画笔/2-4 污点三模式；setBrushHeal 写入）
    int historyBrushSteps = 0;              // M11f：历史画笔源步数（0=未激活；setHistoryBrush 写入）

    // P1 图案图章（setBrushPatternStamp 写入）：激活时笔画 = 图案库网格 wrap 采样
    //（相位锚定文档原点；QM 优先、与克隆/历史画笔/修复族互斥——bridge 侧切换时清位）。
    bool brushIsPattern = false;
    std::shared_ptr<const TileGrid> patternStampGrid;
    uint32_t patternStampW = 0;
    uint32_t patternStampH = 0;
    double patternStampScale = 1.0;

    // M11d 仿制图章克隆源（会话态：不随快照/持久化走；Alt+点击设置）。
    // P1 五槽（PS Clone Source 面板语义）：Shift+数字切换活动槽，cs() = 活动槽引用。
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
    CloneSource cloneSlots[5];
    int cloneSlot = 0;
    CloneSource& cs() { return cloneSlots[cloneSlot]; }  // 须持 docMutex
    std::thread strokeThread_;
    bool strokeThreadStarted_ = false;
    void ensureStrokeThreadLocked();        // 须持 strokeMtx；幂等，先 join 已退出线程
    void strokeThreadMain();
    // 在途 draft 提交（笔画线程 end 分支与 switchDoc 强制收尾共用）。须持 cycle+doc 锁进入；
    // allowUnlock=true 样式重算放锁（线程路径防渲染冻结），false 锁内重算（切换路径：收尾
    // 必须先于 active 切换完成，历史账目落原会话）。
    void commitDraftLocked(std::unique_lock<std::mutex>& lk, std::unique_lock<std::mutex>& cycle,
                           bool allowUnlock);

    // M10c 内部剪贴板（引擎内存；不随快照/持久化走，关闭文档保留——PS 同语义）
    ClipBuffer clipboard;
    // M17a 样式剪贴板（copyLayerStyle/pasteLayerStyle；跨文档保留，同 ClipBuffer 语义）
    std::shared_ptr<LayerStyle> styleClipboard = nullptr;

    // M22 显示变换（视图级，PS「视图 › 校样设置/色域警告」语义：不随文档/撤销走，
    // resetDocument 保留——跨文档视图偏好）。bytes = Lut3d::toRgba8（渲染器 3D 纹理），
    // gamut = 单通道判定表（0/1）。会话只存最终字节；构建在 bridge（cms 内核）。
    struct ViewTransform {
        bool lutOn = false;
        std::vector<uint8_t> lut;   // size³×RGBA8
        uint32_t lutSize = 0;
        bool gamutOn = false;
        std::vector<uint8_t> gamut; // size³×1
        uint32_t gamutSize = 0;
        std::string desc;           // UI 镜像（状态栏/菜单勾选）
    };
    ViewTransform viewXform;
    bool viewXformDirty = false;    // 渲染器重上传标记（invalidate 后也置位）

    // M4a 撤销
    std::atomic<uint32_t> historyVersion{0};
    // M17d 图案 id 计数（definePattern 锁内单调发号；openProject 按包内最大后缀回填防撞号）
    uint64_t patternSeq = 0;
    // D2：版本事件载荷（全局单调——@Watch 需要每次广播值都变化；会话 revision 另行恢复）
    uint32_t revisionSeq = 1;

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

    napi_threadsafe_function revisionTsfn = nullptr;   // docVersion 事件
    napi_threadsafe_function progressTsfn = nullptr;   // importProgress 事件（payload=permille intptr）

    LayerId nextLayerId();  // 单调递增不回收（02 §1 ID 策略），锁外调用安全

    // 状态变更后调用：置脏 + 唤一帧（可在任意线程）
    void requestRender();

    // 锁内 bump 会话版本并经 TSFN 通知（须持有 docMutex；target=null = active）
    void bumpRevisionLocked(DocSession* target = nullptr);
    // 锁内只广播事件（不递增会话 revision）——撤销/重做恢复快照版本后用（M7.5）
    void fireRevisionLocked();

    void resetDocument();  // 重置 active 会话：取消导入、清空、请求渲染（关/新/整档替换统一口）

  private:
    Engine() {
        // 池恒非空：初始一个空会话（等价旧单文档空档语义）
        auto s = std::make_shared<DocSession>();
        s->id = ++sessionSeq;
        docs.push_back(std::move(s));
    }
};

}  // namespace montage

#endif  // MONTAGE_ENGINE_ENGINE_H
