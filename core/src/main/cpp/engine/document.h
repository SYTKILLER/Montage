#ifndef MONTAGE_ENGINE_DOCUMENT_H
#define MONTAGE_ENGINE_DOCUMENT_H

// 数据模型（02 文档 §1/§2 定案）：256px 稀疏瓦片 + PixelSource 抽象 + 不可变发布 + 图层栈。
// 上层（渲染上传/导入/后续 kernels）一律经 PixelSource 接口取数，禁止摸裸指针。
// 源对拍：ImageLayer/CanvasDocument（EditorSession.swift）——layers 自底向上、
// opacity 默认 1、blendMode 默认 normal、空图层画笔开始才分配像素。

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <native_buffer/native_buffer.h>

#include "engine/filters.h"
#include "engine/vector_path.h"

namespace montage {

constexpr uint32_t kTileSize = 256;  // 定案 R2：256px（256KB/瓦片 RGBA8888）

using LayerId = uint64_t;

// 混合模式全集（M2.1 定案：对齐源 LayerBlendMode 顺序，24 值 = PS 25 减 Dissolve/Darker/Lighter；
// 索引即 UI Select 序号；非可分离 4 值 hue/saturation/color/luminosity 走 W3C SetLum/SetSat/ClipColor）
enum class BlendMode : int32_t {
    Normal = 0,
    Darken = 1,
    Multiply = 2,
    ColorBurn = 3,
    LinearBurn = 4,
    Lighten = 5,
    Screen = 6,
    ColorDodge = 7,
    LinearDodge = 8,
    Overlay = 9,
    SoftLight = 10,
    HardLight = 11,
    VividLight = 12,
    LinearLight = 13,
    PinLight = 14,
    HardMix = 15,
    Difference = 16,
    Exclusion = 17,
    Subtract = 18,
    Divide = 19,
    Hue = 20,
    Saturation = 21,
    Color = 22,
    Luminosity = 23,
};
constexpr int32_t kBlendModeCount = 24;

// D2.4 像素源抽象（DMA 扩展点预留）。
class PixelSource {
  public:
    virtual ~PixelSource() = default;
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
    virtual uint32_t rowBytes() const = 0;  // stride：遍历禁止用 width*4
    virtual const uint8_t* mapCpu() const = 0;
    virtual uint8_t* mapCpuWrite() = 0;  // 仅构建期（可变→不可变规则）
    virtual OH_NativeBuffer* nativeBuffer() = 0;  // GL/DMA 直通路径；EngineBuffer 恒为 nullptr
    virtual void unmap() = 0;
};

// 初版唯一实现：malloc 内存，内部一律 RGBA8888。
class EngineBuffer : public PixelSource {
  public:
    EngineBuffer(uint32_t width, uint32_t height);
    ~EngineBuffer() override;
    EngineBuffer(const EngineBuffer&) = delete;
    EngineBuffer& operator=(const EngineBuffer&) = delete;

    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }
    uint32_t rowBytes() const override { return rowBytes_; }
    const uint8_t* mapCpu() const override;
    uint8_t* mapCpuWrite() override;
    OH_NativeBuffer* nativeBuffer() override { return nullptr; }
    void unmap() override {}

  private:
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t rowBytes_ = 0;
    uint8_t* bytes_ = nullptr;
};

struct Tile {
    std::unique_ptr<PixelSource> pixels;
};

// 不可变瓦片组（发布后不再改）。revision 随每次发布递增，供脏追踪与缓存键。
struct TileGrid {
    uint32_t cols = 0;
    uint32_t rows = 0;
    uint64_t revision = 0;
    // key = ty*cols+tx；哈希定位 O(1)，稀疏：空白瓦片不存在
    std::unordered_map<uint32_t, std::shared_ptr<const Tile>> tiles;
};

// 构建期可变视图（导入/绘制线程私有），publish 生成不可变快照（map 拷贝，量级=瓦片数）。
class TileGridBuilder {
  public:
    TileGridBuilder(uint32_t cols, uint32_t rows) : cols_(cols), rows_(rows) {}

    uint32_t cols() const { return cols_; }
    uint32_t rows() const { return rows_; }
    uint32_t tileIndex(uint32_t tx, uint32_t ty) const { return ty * cols_ + tx; }

    EngineBuffer& ensureTile(uint32_t tx, uint32_t ty);
    bool hasTile(uint32_t tx, uint32_t ty) const;

    std::shared_ptr<const TileGrid> publish(uint64_t revision);

  private:
    uint32_t cols_ = 0;
    uint32_t rows_ = 0;
    std::unordered_map<uint32_t, std::shared_ptr<const Tile>> tiles_;
};

// 图层变换（M2：origin+size；旋转/翻转 M8）。origin 为图层左上角在文档坐标系的落点。
struct Transform {
    double originX = 0.0;
    double originY = 0.0;
    double width = 0.0;   // 0 = 用像素尺寸
    double height = 0.0;
};

// 图层蒙版（M5a，对拍上游 LayerMask 语义）：8-bit 灰度（白显黑隐），网格恒 RGBA8888
// （R=G=B=gray、A=255）。patch 网格 = PSD maskRect 原始范围，在图层局部坐标系由
// offsetX/Y 定位；patch 之外的区域取 outside（PSD 语义 = maskDefault，通常 255 全显）。
// 不可变共享（改 = 整份 COW 换新，撤销快照才不会被原地改写）。
struct LayerMask {
    std::shared_ptr<const TileGrid> pixels;  // patch 网格；null = 无 patch（整层取 outside）
    uint32_t width = 0;                      // patch 像素尺寸（网格可能 256 对齐略大）
    uint32_t height = 0;
    int offsetX = 0;
    int offsetY = 0;
    uint8_t outside = 255;
    bool enabled = true;
    bool linked = true;  // v1 恒 true（独立 placement 随 M8 变换引入）
};

// 调整层（M6a，对齐源 LayerAdjustment 的 v1 子集）： Levels/Curves 统一为 256 LUT 渲染；
// 对下方全部合成结果生效（acc 全帧），v1 忽略剪贴/混合模式（仅 opacity lerp，登记 M6b）。
enum class AdjustmentKind : int32_t {
    Levels = 0,
    Curves = 1,
    // M9a 扩充（LUT 类：Invert/Threshold/Posterize/BC/ColorBalance；shader 类：HueSat）
    BrightnessContrast = 2,  // p0=brightness(-150..150) p1=contrast(-50..100)
    Invert = 3,
    Threshold = 4,           // p0=level(0..255)
    Posterize = 5,           // p0=levels(2..32)
    ColorBalance = 6,        // p0/p1/p2=R/G/B 增量(-100..100)
    HueSaturation = 7,       // p0=hue(-180..180) p1=sat(-100..100) p2=light(-100..100)；shader 类
};

struct LayerAdjustment {
    AdjustmentKind kind = AdjustmentKind::Levels;
    // Levels：主通道输入黑/白点 + gamma（0..1、0..1、0.1..10）
    float inBlack = 0.0f;
    float inWhite = 1.0f;
    float gamma = 1.0f;
    // Curves：控制点（x/y 各 0..1，x 单调；v1 线性插值）
    std::vector<float> curveX;
    std::vector<float> curveY;
    // M9a 通用参数槽（语义随 kind，见 AdjustmentKind 注释）
    float p0 = 0.0f;
    float p1 = 0.0f;
    float p2 = 0.0f;
    float p3 = 0.0f;
};

// M17d 等高线（photocraft Contour）：效果覆盖度的传递曲线（PS「等高线」编辑器）。
// custom=false = Linear（恒等，等价无等高线）；custom=true 时 pts 为 x,y 交错点列（0..1，
// x 单调），corners 与点一一对应（1 = 角点：曲线在该点折角，photocraft contour_curve_lut
// 按角点分段各自自然样条）。
struct StyleContour {
    bool custom = false;
    std::vector<float> pts;        // x0,y0,x1,y1,…（0..1，长度 = 2×点数）
    std::vector<uint8_t> corners;  // 与点一一对应（0/1）

    bool operator==(const StyleContour& o) const {
        return custom == o.custom && pts == o.pts && corners == o.corners;
    }
    bool operator!=(const StyleContour& o) const { return !(*this == o); }
};

// M17d 图案资源（文档级，对齐 PS「编辑 → 定义图案」产物）：图案叠加效果与（后续）图案
// 图章/填充引用。随 Document 快照走（快照拷贝仅 id/name/w/h + shared_ptr，廉价）；变更经
// definePattern 命令不入 History（setGlobalLight 同语义——PS 定义图案不在历史面板）。
struct DocumentPattern {
    std::string id;     // 稳定 id（"pat_<n>"，进程内单调；.montage v11 持久化）
    std::string name;   // 显示名（「图案 1」）
    uint32_t w = 0;
    uint32_t h = 0;
    std::shared_ptr<const TileGrid> pixels;  // 直 alpha 瓦片网格（w×h，通常单瓦片）
};

// M14c 图层样式第一批（PS 语义子集）：投影 / 描边（v1 外部）/ 颜色叠加。
// 参数直存（角度+距离由 UI 换算 offset？——直存角度/距离保持 PS 参数语义，渲染核换算）。
// 随 Layer 走 History 快照（COW 换指针）；渲染 = styledRender 派生网格（蒙版 composeMasked
// 同架构：GL/缩略图/CPU flatten 统一走 effectivePixels，渲染器零感知）。
struct LayerStyle {
    bool enabled = true;  // fx 总开关（PS「效果」眼睛）
    // 投影：角度（度，PS 120=右下）+ 距离 + 大小（σ = size，M13 三 box 高斯）+ 颜色 + 不透明度
    bool dropShadow = false;
    double shadowAngleDeg = 120.0;
    double shadowDistance = 10.0;
    double shadowSize = 8.0;
    float shadowColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // 直 alpha
    double shadowOpacity = 0.5;
    // 描边：外部（dilate − 原始 alpha 环带，M10 van Herk 方形 SE）+ 宽 + 颜色
    bool stroke = false;
    double strokeWidth = 3.0;
    float strokeColor[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    // 颜色叠加：本体之上 × alpha 覆盖
    bool colorOverlay = false;
    float overlayColor[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    double overlayOpacity = 1.0;
    // M17a 内阴影（photocraft shadow_map inner 语义：反向 alpha 平移+模糊、出界填满、限层内；
    // v1 无 spread/等高线/噪点）
    bool innerShadow = false;
    double isAngleDeg = 135.0;
    double isDistance = 5.0;
    double isSize = 5.0;
    float isColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    double isOpacity = 0.75;
    // M17a 外发光（层下，Softer 技术 = 膨胀+高斯；v1 无 spread/等高线）
    bool outerGlow = false;
    double ogSize = 10.0;
    float ogColor[4] = {1.0f, 0.894f, 0.431f, 1.0f};  // PS 默认淡黄
    double ogOpacity = 0.5;
    // M17a 内发光（本体内边缘衰减带；v1 Edge 源、Softer 技术）
    bool innerGlow = false;
    double igSize = 10.0;
    float igColor[4] = {1.0f, 0.894f, 0.431f, 1.0f};
    double igOpacity = 0.5;
    // M17a 渐变叠加（v1 双色线性、层 bbox 对齐、90=上→下；多段 stops 挂 M17b）
    bool gradientOverlay = false;
    float gdFrom[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float gdTo[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    double gdAngleDeg = 90.0;
    double gdOpacity = 1.0;
    // M17b spread（photocraft spread_split：dilate r=size·spread 先、blur bw=size·(1−spread) 后；
    // 投影=spread、内阴影/内发光=choke、外发光=spread）
    double shadowSpread = 0.0;
    double isSpread = 0.0;
    double ogSpread = 0.0;
    double igSpread = 0.0;
    // M17b 全局光（photocraft use_global_light：真 = 用 Engine::globalLightAngle）
    bool shadowUseGL = true;
    bool isUseGL = true;
    bool bevelUseGL = true;
    // M17b 光泽（photocraft satin_map：双向平移 σ=size 模糊取 |a−b| ×alpha；invert 默认关）
    bool satin = false;
    float satinColor[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    double satinAngleDeg = 120.0;
    double satinDistance = 8.0;
    double satinSize = 8.0;
    double satinOpacity = 1.0;
    bool satinInvert = false;
    // M17b 斜面浮雕（photocraft bevel_height Smooth + bevel_maps 光照；v1 Smooth 技术，
    // 无 contour/texture/soften/Emboss 族；Inner=层上、Outer=层下）
    bool bevel = false;
    int bevelStyle = 0;  // 0=InnerBevel 1=OuterBevel（Emboss 族挂 M17c）
    double bevelDepth = 2.0;   // 0..10（PS 100% = 1）
    bool bevelUp = true;
    double bevelSize = 5.0;
    double bevelAngleDeg = 120.0;
    double bevelAltitudeDeg = 30.0;
    float bevelHiColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    double bevelHiOpacity = 1.0;
    float bevelShColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    double bevelShOpacity = 1.0;
    // M17d 等高线（photocraft Contour：阴影/光泽 = coverage 传递、发光 = contour(min(v/range,1))、
    // 斜面 = 高度图轮廓 + 明暗图光泽等高线）。custom=false = Linear 恒等；Range 仅发光有
    //（PS UI「范围」，默认 50% = photocraft Glow::default——对 v10 及以前无 Range 的文档，
    // 读取补默认 0.5，发光渲染相对 M17a 增强为 PS 语义，登记）。
    StyleContour shadowContour;
    StyleContour isContour;
    StyleContour ogContour;
    StyleContour igContour;
    StyleContour satinContour;      // PS 光泽「等高线」
    StyleContour bevelContour;      // PS 斜面「等高线」（高度图轮廓，photocraft bevel_contour_lut）
    StyleContour bevelGlossContour; // PS 斜面「光泽等高线」（明暗图传递）
    double ogRange = 0.5;
    double igRange = 0.5;
    // M17d 图案叠加（photocraft FxPaint::Pattern）：图案按 ink bbox 左上锚定平铺
    //（PS「与图层对齐」默认语义），nearest 采样 × scale；patternId 空 = 未选图案（不渲染）。
    bool patternOverlay = false;
    std::string patternId;
    double poScale = 1.0;    // 0.01..10
    double poOpacity = 1.0;  // 0..1
};

// M14c 填充图层（非破坏内容层）：无像素，渲染 = 全文档域派生网格（改参重生成）。
struct FillLayerState {
    int kind = 0;  // 0=纯色 1=线性渐变（fg→bg，角度 deg，M11 渐变核）
    float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};  // 纯色（直 alpha）
    double angleDeg = 90.0;  // 渐变方向（90 = 上→下）
};

// 文本层状态（M12，路线 A：native OH_Drawing_Typography 栅格化）。随 Layer 走 History
// 快照（COW 换指针）；pixels 网格 = 最近一次排版栅格结果（直 alpha），origin = 文本框
// 左上角文档落点（PS 点文字锚点在首行基线左，v1 简化左上角，登记）。
struct TextLayerState {
    std::string content;      // UTF-8；\n 分行
    std::string fontFamily;   // FontParser/系统字体族名；空 = 系统默认
    double fontSize = 40.0;   // 文档 px
    double lineHeight = 1.2;  // 行距倍数（行高 px = fontSize × lineHeight）
    double tracking = 0.0;    // 字距（千分比 em，PS 语义）→ px = fontSize × tracking / 1000
    bool bold = false;
    bool italic = false;
    float color[3] = {0.0f, 0.0f, 0.0f};  // 直 alpha 0..1
    int32_t align = 0;        // 0=左 1=中 2=右（OH_Drawing_TextAlign LEFT/CENTER/RIGHT）
    double wrapWidth = 0.0;   // >0 = 段落文字换行宽（文档 px）；0 = 点文字（不自动换行）
    uint64_t styleHash = 0;   // 参数指纹（relayout 缓存键；0 = 从未栅格化）
    double rasterW = 0.0;     // 最近栅格化实际像素尺寸（派生缓存，不入指纹；DTO/overlay 用）
    double rasterH = 0.0;
};

// M18 形状层状态（非破坏矢量内容层）：path（文档域坐标）+ 填充 + 描边 + live（参数形状，
// kind != None = 可再编辑；路径经钢笔/直接选择编辑后 live 清 None）。渲染 = shapeRender
// 派生网格（文字层/样式层同架构：shapeCache 随 History 快照走，变更点主动维护）。
// v1 填充/描边 paint = 纯色（渐变/图案登记后续批，随 M11 stops 遗留合并）。
struct ShapeLayerState {
    VectorPath path;
    int fillKind = 1;  // 0=无填充 1=纯色
    float fillColor[4] = {1.0f, 0.0f, 0.0f, 1.0f};  // 直 alpha
    bool hasStroke = false;
    ShapeStroke stroke;
    LiveShape live;
    uint64_t cacheHash = 0;  // 最近渲染指纹（path+fill+stroke；0 = 从未渲染）
};

// M18 路径面板条目（文档级保存路径；工作路径单独挂 Document.workPath）。
struct NamedPathEntry {
    std::string name;
    VectorPath path;
};
inline bool operator==(const NamedPathEntry& a, const NamedPathEntry& b) {
    return a.name == b.name && a.path == b.path;
}

// M18 自定义形状库（编辑 → 定义自定形状；文档级，.montage v12 持久化；定义动作不入
// History——「定义图案」同语义）。
struct CustomShapeEntry {
    std::string id;  // "csh_<n>" 进程内单调
    std::string name;
    VectorPath path;  // 以原点为中心的规范域（定义时归一）
};
inline bool operator==(const CustomShapeEntry& a, const CustomShapeEntry& b) {
    return a.id == b.id && a.name == b.name && a.path == b.path;
}


// M21 智能滤镜（photocraft SmartFilter 对齐，参数即数据）：kind = filters::FilterKind
// （29 值核）；-1 = 未实现滤镜（PSD filterFX 导入保 descriptor 原样 hex，可隐藏/重排/
// 删除，导出原样回写——渲染按直通）。psdClass = PSD filter class id（四字节，"GsnB" 等；
// Montage 内建滤镜置入时为空，PSD 双向映射用）。
struct SmartFilter {
    int32_t kind = -1;               // filters::FilterKind；-1 = 未实现（psdHex 非空）
    std::string psdClass;            // "GsnB"/"MtnB"/…（内建置入 = 空）
    std::string name;                // 显示名（「高斯模糊」/PS 原名）
    filters::FilterParams params;    // 参数槽（kind >= 0 时有效）
    BlendMode blend = BlendMode::Normal;
    double opacity = 1.0;
    bool visible = true;
    std::string psdHex;              // kind<0：filterFX item descriptor 原样 hex

    bool operator==(const SmartFilter& o) const {
        return kind == o.kind && psdClass == o.psdClass && name == o.name &&
               params.kind == o.params.kind && params.p0 == o.params.p0 && params.p1 == o.params.p1 &&
               params.p2 == o.params.p2 && params.p3 == o.params.p3 && params.p4 == o.params.p4 &&
               params.p5 == o.params.p5 && params.seed == o.params.seed &&
               params.scale == o.params.scale && params.offset == o.params.offset &&
               blend == o.blend && opacity == o.opacity && visible == o.visible &&
               psdHex == o.psdHex;
    }
    bool operator!=(const SmartFilter& o) const { return !(*this == o); }
};

// M21 智能对象（photocraft SmartObject 子集，07 §3.12）：源（嵌入原样字节 / 链接路径）+
// 放置仿射 + 智能滤镜栈 + 滤镜蒙版（每 SO 一个，PS 语义）。渲染 = smartCache 派生网格
//（文字/形状层同架构）；源解码/放置/逐滤镜结果挂引擎级缓存（smart_ops，contentsId 键）。
struct SmartLayerState {
    uint64_t contentsId = 0;           // 共享内容实例 id（duplicate 共享、via copy 独立）
    bool embedded = true;              // true = 嵌入源字节 / false = 链接路径
    std::string fileName;              // 源文件名（扩展名驱动解码分发：.montage/.psd/.png/…）
    std::vector<uint8_t> sourceBytes;  // 嵌入源原样字节（PSD lnk2 往返/编辑→替换 保真基础）
    std::string linkedPath;            // embedded = false 时的链接路径
    uint64_t sourceHash = 0;           // fnv1a(sourceBytes)（0 = 未算，渲染时现算）
    double m[6] = {1, 0, 0, 0, 1, 0};  // 放置仿射（行主序：源 strip 坐标 → 文档 px）
    std::vector<SmartFilter> filters;  // 应用序（栈底在前，PS filterFXList 同序）
    bool filtersEnabled = true;        // 整栈开关（PS「禁用智能滤镜」，不动单滤镜 visible）
    std::shared_ptr<LayerMask> filterMask = nullptr;  // 滤镜蒙版（图层局部坐标 = 放置域局部）
    // PSD 透传：psdRaw = 导入的 SoLd/SoLE 块原始数据（内容未变时原样回写 = 字节级往返，
    // photocraft psd_raw 同语义）；placedId/uuid = FEid 键与 lnk2 链接键（v1 仅透传）。
    std::vector<uint8_t> psdRaw;
    std::string placedId;
    std::string uuid;
    double srcW = 0.0;   // 源像素尺寸（SoLd Sz/置入解码/转换裁剪回填；导出四角出参）
    double srcH = 0.0;
    uint64_t psdImportHash = 0;  // 导入时渲染指纹（导出比对判定「未编辑」→ psdRaw 原样回写）
};

// 图层（值语义，对齐源 ImageLayer；M2 子集：蒙版/剪贴 M5、调整层 M6a、文本层 M12）。
struct Layer {
    LayerId id = 0;
    std::string name;
    bool visible = true;
    double opacity = 1.0;
    BlendMode blendMode = BlendMode::Normal;
    Transform transform;
    std::shared_ptr<const TileGrid> pixels;  // null = 空图层（源：画笔开始才分配）
    std::shared_ptr<LayerMask> mask = nullptr;         // null = 无蒙版
    std::shared_ptr<const TileGrid> render = nullptr;  // 蒙版启用时的合成结果（派生缓存，
                                                       // 变更点主动维护、随 History 快照走）
    bool clipping = false;  // M5b-2：剪贴蒙版——剪贴到下方最近非剪贴层（渲染期生效，PS 语义）
    std::shared_ptr<LayerAdjustment> adjustment = nullptr;  // M6a：非空 = 调整层（无像素）
    std::shared_ptr<TextLayerState> text = nullptr;  // M12：非空 = 文本层（pixels = 栅格结果）
    std::shared_ptr<LayerStyle> style = nullptr;     // M14c：非空 = 带样式（渲染走 styledRender）
    std::shared_ptr<FillLayerState> fill = nullptr;  // M14c：非空 = 填充层（渲染走 styledRender）
    std::shared_ptr<ShapeLayerState> shape = nullptr;  // M18：非空 = 形状层（pixels 恒 null，
                                                       // 渲染走 shapeCache 派生网格）
    std::shared_ptr<SmartLayerState> smart = nullptr;  // M21：非空 = 智能对象层（pixels 恒
                                                       // null，渲染走 smartCache 派生网格）
    // M21 智能对象派生渲染（放置+滤镜链+滤镜蒙版；origin = transform.origin——渲染回填）。
    // smartCacheHash = 渲染指纹（命中跳过重渲）；随 History 快照走（undo 即恢复）。
    std::shared_ptr<const TileGrid> smartCache = nullptr;
    uint64_t smartCacheHash = 0;
    std::shared_ptr<VectorMaskData> vectorMask = nullptr;  // M18d：非空 = 矢量蒙版
    // M18 形状派生渲染结果（文档域；origin 独立——域 = 形状包围盒 ∩ 画布）。
    std::shared_ptr<const TileGrid> shapeCache = nullptr;
    double shapeOriginX = 0.0;
    double shapeOriginY = 0.0;
    // M18d 矢量蒙版派生（底片 = 蒙版合成 → 形状缓存/原像素 × 路径覆盖度；域同底片、
    // origin 同底片；enabled=false 或路径空时为 null）。随 History 快照走。
    std::shared_ptr<const TileGrid> vectorRender = nullptr;
    // M14c 样式/填充派生渲染结果（域可超出原网格——投影/描边扩展；origin 独立于 transform）。
    // 样式参数/像素/蒙版变更点重算；随 History 快照走（undo 即恢复）。
    std::shared_ptr<const TileGrid> styledRender = nullptr;
    double styledOriginX = 0.0;
    double styledOriginY = 0.0;
    // M14a 图层编组（平铺栈方案）：isGroup = 组节点（无像素/无蒙版/无调整/无文本）；
    // parentId = 直接父组 id（0 = 根）；直接子层在 layers 平铺序中**连续**且位于组节点
    // 之后（bottom→top）；collapsed = 面板折叠（渲染跳过子层段）。
    bool isGroup = false;
    LayerId parentId = 0;
    bool collapsed = false;
    // M14b 链接层：同一非零 linkId 的层随移动工具联动平移（对齐 PS 链接语义）。
    // 会话态 v1：.montage 不持久化（PSD 链接导出随 M15 评估），随 History 快照走。
    uint64_t linkId = 0;
    // M14c/M18/M21 渲染/缩略图取数优先级：样式/填充派生（含独立 origin，经 effectiveRenderOrigin）
    // → 矢量蒙版派生（已含像素蒙版）→ 蒙版合成 → 智能对象派生 → 形状派生 → 原像素
    std::shared_ptr<const TileGrid> effectivePixels() const {
        if ((style != nullptr || fill != nullptr) && styledRender != nullptr &&
            (style == nullptr || style->enabled)) {
            return styledRender;
        }
        if (vectorMask != nullptr && vectorMask->enabled && vectorRender != nullptr) {
            return vectorRender;
        }
        if (mask != nullptr && mask->enabled && render != nullptr) {
            return render;
        }
        if (smart != nullptr && smartCache != nullptr) {
            return smartCache;
        }
        if (shape != nullptr && shapeCache != nullptr) {
            return shapeCache;
        }
        return pixels;
    }

    // 派生渲染生效时的文档落点（与 transform.origin 分离——投影/描边扩展域偏移）
    double effectiveOriginX() const {
        if (style != nullptr && style->enabled && styledRender != nullptr) {
            return styledOriginX;
        }
        if (shape != nullptr) {
            return shapeOriginX;  // render/vectorRender（启用时）由 shapeCache 派生、域相同
        }
        return transform.originX;
    }
    double effectiveOriginY() const {
        if (style != nullptr && style->enabled && styledRender != nullptr) {
            return styledOriginY;
        }
        if (shape != nullptr) {
            return shapeOriginY;
        }
        return transform.originY;
    }
};

// M10c Alpha 通道（06 规划 §4 M10）：文档级 8-bit 灰度集合——存储选区（PS 通道语义）。
// 掩码网格与 Document.selection 同约定（RGBA 灰度瓦片）；随 Document 快照走撤销。
struct AlphaChannel {
    std::string name;
    std::shared_ptr<const TileGrid> mask;
};

// M2：图层栈文档（无组；蒙版/剪贴/调整层 M5/M6；选区 M7a）。
struct Document {
    uint32_t width = 0;
    uint32_t height = 0;
    std::string name;
    std::vector<Layer> layers;  // bottom → top（对齐源 CanvasDocument.layers）
    LayerId activeId = 0;
    // M7a 选区：文档域 8-bit 覆盖掩码（null = 无选区，全图可编辑）；随 Document 快照走撤销
    std::shared_ptr<const TileGrid> selection;
    // M10d 快速蒙版（非 null = 快速蒙版模式中）：临时通道（选中度灰度，缺瓦片 = 0 = 全红）；
    // 会话态不持久化；随 Document 快照走撤销（进出 Q 与通道涂画均为历史事务）
    std::shared_ptr<const TileGrid> quickMask;
    // M10c Alpha 通道（存储选区；主选区保持会话态不持久化，通道经 .montage v5 持久化）
    std::vector<AlphaChannel> channels;
    // M15a 参考线（会话态：不随 History 快照走撤销；经 .montage v9 可选块持久化）。
    // guidesH = 水平参考线的 y（文档 px，升序）；guidesV = 垂直参考线的 x；锁定 = 全局
    // 开关（PS「锁定参考线」语义，锁定后 add/move/remove 拒绝）；吸附几何在 UI 层算。
    std::vector<float> guidesH;
    std::vector<float> guidesV;
    bool guidesLocked = false;
    bool guidesVisible = true;
    // M17b 全局光角度（度；投影/内阴影/斜面 useGL=true 时引用。PS 文档级设置——
    // 改动经 setGlobalLight 命令全文档重样式，不走 History；.montage v10 可选持久化）
    double globalLightAngle = 120.0;
    // M17d 图案资源（编辑 → 定义图案；不入 History，随文档替换清空；.montage v11 持久化）
    std::vector<DocumentPattern> patterns;
    // M18 矢量（路径面板/形状库；workPath = 工作路径会话态、随文档替换清空；v12 持久化；
    // 两者均不入 History——PS 路径面板同理，路径编辑经形状层命令走历史）
    VectorPath workPath;
    std::vector<NamedPathEntry> paths;
    std::vector<CustomShapeEntry> customShapes;
    uint32_t customShapeSeq = 0;  // csh_N 单调号（openProject 回填防撞号）
};

// 视口：pan = 视口左上角的文档坐标（doc px），screen = (doc - pan) * zoom。
// 视图：pan = 视口左上角的文档坐标（doc px），screen = (doc - pan) * zoom。
struct Viewport {
    double zoom = 1.0;
    double panX = 0.0;
    double panY = 0.0;
};

double clampZoom(double zoom);  // [1/32, 32]

// 选区灰度采样（02 D2.3：8-bit 覆盖掩码主表示；mask = 文档域网格，null = 无选区全图可编辑）
uint8_t selectionGrayAt(const TileGrid& mask, int docX, int docY);

const char* blendModeName(BlendMode mode);  // DTO 显示名（对齐源 LayerBlendMode raw 值）
bool blendModeFromName(const std::string& name, BlendMode& out);  // 逆映射（.montage 读取，M4c）

// 调整层 RGB LUT 生成（M9a；从 tile_renderer 迁出以便宿主机单测）。
// r/g/b 各 256 项输出 0..1；多数 kind 三通道同值，ColorBalance 按通道独立。
void buildAdjustmentRgbLut(const LayerAdjustment& adj, float r[256], float g[256], float b[256]);

// M14a 组辅助：组节点 layers[gi] 的直接子层连续段尾（gi+1 起 parentId==组 id 的连续区间；
// 信任平铺连续不变式，UI/命令维护）。gi 非组节点返回 gi+1。
int groupChildEnd(const std::vector<Layer>& layers, int gi);

// 蒙版合成（M5a）：out.a = a × gray/255（直 alpha 域，rgb 不变）；patch 外取 mask.outside。
// 无蒙版像素/网格外按语义共享或丢弃；返回的网格与 src 同维度。mask.pixels == null 时
// 整层取 outside（255 → 原样共享指针；0 → 空网格）。
std::shared_ptr<const TileGrid> composeMasked(std::shared_ptr<const TileGrid> src, const LayerMask& mask);

// 图层局部坐标 (gx,gy) 处的蒙版灰度（patch 内查网格，patch 外取 outside）
uint8_t layerMaskGrayAt(const LayerMask& mask, int gx, int gy);

// 网格墨迹精确包围盒（alpha > 0 逐像素扫描；返回网格局部坐标）。与瓦片粒度的
// layerContentBBox（filter_ops.h）分工：trim/对齐分布等需要贴墨迹的语义用它，
// 滤镜 strip 域保留瓦片对齐（卷积核边界余量）。
bool gridInkBBox(const TileGrid& grid, uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h);

}  // namespace montage

#endif  // MONTAGE_ENGINE_DOCUMENT_H
