#ifndef MONTAGE_ENGINE_PLUGINS_H
#define MONTAGE_ENGINE_PLUGINS_H

// M26 插件内核（P8，docs/40-方案设计/14 方案）：manifest 校验 / 参数解析 / band 规划 /
// 输出清洗 / 选区 lerp / 发布 / 注册表——全部纯逻辑（无 JSVM 依赖，宿主 .debug/p8 可测）。
// JSVM 宿主见 plugins_jsvm.cpp（IPluginRuntime 的设备侧实现）。
// 语义对拍 photocraft crates/plugins（MIT）：manifest.rs 校验规则 / resolve_params /
// apply.rs band 划分 + read_clamped + 非 finite 清洗 + 选区 lerp。

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "engine/document.h"

namespace montage {
namespace plugins {

// ---- 参数 schema（photocraft ParamSpec 对位；宿主按 JSON 键序读入，对话框按序渲染）----
struct ParamSpec {
    enum Type { Number = 0, Int = 1, Bool = 2, Choice = 3 };
    Type type = Number;
    double mn = 0.0;       // Number/Int 值域下界
    double mx = 100.0;     // Number/Int 值域上界
    double def = 0.0;      // Number/Int 默认
    bool bdef = false;     // Bool 默认
    std::vector<std::string> options;  // Choice 选项（1..64）
    std::string cdef;                  // Choice 默认（空 = 首项）
};

// ---- manifest（validateManifest 产物 = 清洁可执行描述）----
struct Manifest {
    std::string id;        // [A-Za-z0-9._-]{1,64}
    std::string name;      // 1..64 可打印
    std::string version;   // ≤32
    std::string description;  // ≤1024
    std::string author;    // ≤128
    bool canvasArea = false;   // false=content（层内容域）true=canvas（全画布）
    uint32_t overlap = 0;      // ≤256
    std::vector<std::pair<std::string, ParamSpec>> params;  // 有序，≤64
};

// JSVM 读出侧的原始 manifest（字段未校验；字符串带原样值）。
struct RawManifest {
    std::string id, name, version, description, author;
    std::string kind;      // 应为 "filter"
    std::string area;      // "content"|"canvas"（缺省 content）
    double overlap = 0.0;
    bool hasId = false;
    bool hasName = false;
    bool hasVersion = false;
    bool hasDescription = false;
    bool hasAuthor = false;
    bool hasKind = false;
    bool hasArea = false;
    bool hasOverlap = false;
    std::vector<std::pair<std::string, ParamSpec>> params;  // 读出顺序保持
    // 必需字段（id/name/kind）齐全（读出端判定）
    bool complete = false;
};

// 校验 + 归一（photocraft Manifest::validate 同规则）。失败返回 false 且 err 带原因。
bool validateManifest(const RawManifest& raw, Manifest& out, std::string& err);

// ---- 用户参数解析（photocraft resolve_params 同式）----
struct PValue {
    enum T { Num = 0, Bool = 1, Str = 2 } t = Num;
    double num = 0.0;
    bool b = false;
    std::string s;
};

// userJson 为 JSON 对象串（''/null = 全缺省）。缺失→default；Number/Int 有限并 clamp
// 入域（Int 四舍五入）；Bool 严格；Choice 必须 ∈ options；类型错 = false+err；未知键丢弃。
bool resolveParams(const Manifest& m, const std::string& userJson,
                   std::vector<std::pair<std::string, PValue>>& out, std::string& err);

// ---- band 规划（photocraft apply.rs 同构：tile 对齐块，1 tile 行高 × across 宽）----
struct IRect {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // x1/y1 exclusive
    int32_t width() const { return x1 - x0; }
    int32_t height() const { return y1 - y0; }
    bool empty() const { return x0 >= x1 || y0 >= y1; }
};

IRect iIntersect(const IRect& a, const IRect& b);
IRect iInflate(const IRect& r, int32_t m);
IRect iUnion(const IRect& a, const IRect& b);

// band 输出域列表（area 划分为 256 对齐块；kBandTileAcross 块宽）。空 area → 空。
void planBands(const IRect& area, std::vector<IRect>& bands);

// band 输入域 = 输出域 inflate(overlap)。
IRect bandInput(const IRect& band, uint32_t overlap);

// read_clamped（photocraft 同式）：从稀疏网格读 inRect 到 U8 RGBA strip（行距 w*4，
// 直 alpha）。inRect ∩ extent 内 = 网格真实像素（缺瓦片 = 透明）；extent 外像素复制
// extent 边缘（inner 空时全透明）。extent = 画布 ∪ 内容 bbox（文档坐标）。
void readGridClamped(const TileGrid& grid, const IRect& inRect, const IRect& extent,
                     std::vector<uint8_t>& out);

// 选区灰度（0..1；doc 坐标；无选区 = 1）。
float selGrayAt(const TileGrid* sel, int docX, int docY);

// band 输出清洗 + 选区 lerp + U8 转换（photocraft run_band 尾段同式）：
// px = JS 写过的 band 缓冲（f32 RGBA，w×h = band 输出域），orig = 运行前副本；
// 非 finite → 原值；恒 clamp 0..1；sel 非空时 out = orig + (v−orig)·g（选区掩码在文档域，
// selOx/selOy = 层网格局部原点的文档落点，filter_ops SelSampler 同式）；
// 结果写入 dst（RGBA8，行距 dstW*4，dst 原点 = (dstOriginX, dstOriginY)，通常 = area 原点）。
void finishBand(const float* px, const float* orig, int w, int h, const TileGrid* sel,
                int32_t selOx, int32_t selOy, const IRect& band, uint8_t* dst, int32_t dstW,
                int32_t dstOriginX, int32_t dstOriginY);

// area strip 发布回网格（COW：未触碰瓦片共享，触碰瓦片整块重建；heal_ops 同式）。
std::shared_ptr<const TileGrid> publishAreaStrip(const TileGrid& orig, const IRect& area,
                                                 const uint8_t* strip, uint64_t revision);

// ---- 注册表（进程级；bridge/TS 消费。不持久化——v1 登记）----
struct PluginEntry {
    Manifest man;
    std::string source;    // JS 源码全文
    bool builtin = false;  // rawfile 内置
    bool poisoned = false; // 时限弃线程后禁用（重启恢复）
};

class PluginRegistry {
  public:
    static PluginRegistry& get();

    // 已存在同 id → 覆盖（更新源码/manifest，保留 poisoned=false 重置）。返回插入序。
    size_t upsert(const PluginEntry& e);
    bool remove(const std::string& id);
    PluginEntry* find(const std::string& id);
    void poison(const std::string& id);
    std::vector<PluginEntry> list() const;
    size_t count() const;

  private:
    mutable std::mutex mtx_;
    std::vector<PluginEntry> entries_;
};

// band 预算（tile 宽块数）：4MB / (256²×4×4) = 4 tile 宽（方案 §4.1）。
constexpr int kBandTileAcross = 4;

// 注册期源码/manifest 上限（方案 §4.2）。
constexpr size_t kMaxSourceBytes = 1u << 20;
constexpr size_t kMaxManifestBytes = 64u << 10;
constexpr uint32_t kMaxOverlap = 256;
constexpr size_t kMaxParams = 64;
// 运行时限（毫秒；注册校验 run 用 kRegisterTimeMs）。
constexpr int64_t kRunTimeMs = 60000;
constexpr int64_t kRegisterTimeMs = 5000;

}  // namespace plugins
}  // namespace montage

#endif  // MONTAGE_ENGINE_PLUGINS_H
