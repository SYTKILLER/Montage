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

namespace montage {

constexpr uint32_t kTileSize = 256;  // 定案 R2：256px（256KB/瓦片 RGBA8888）

using LayerId = uint64_t;

// M2 混合模式子集（03 §5：M2.1 再扩全集；索引即 UI Select 序号）
enum class BlendMode : int32_t {
    Normal = 0,
    Multiply = 1,
    Screen = 2,
    Overlay = 3,
};

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

// 图层（值语义，对齐源 ImageLayer；M2 子集：蒙版/调整/形状/文字 M5+ 逐步补）。
struct Layer {
    LayerId id = 0;
    std::string name;
    bool visible = true;
    double opacity = 1.0;
    BlendMode blendMode = BlendMode::Normal;
    Transform transform;
    std::shared_ptr<const TileGrid> pixels;  // null = 空图层（源：画笔开始才分配）
};

// M2：图层栈文档（无组/蒙版/选区；History M4）。
struct Document {
    uint32_t width = 0;
    uint32_t height = 0;
    std::string name;
    std::vector<Layer> layers;  // bottom → top（对齐源 CanvasDocument.layers）
    LayerId activeId = 0;
};

// 视口：pan = 视口左上角的文档坐标（doc px），screen = (doc - pan) * zoom。
struct Viewport {
    double zoom = 1.0;
    double panX = 0.0;
    double panY = 0.0;
};

double clampZoom(double zoom);  // [1/32, 32]

const char* blendModeName(BlendMode mode);  // DTO 显示名（对齐源 LayerBlendMode raw 值）

}  // namespace montage

#endif  // MONTAGE_ENGINE_DOCUMENT_H
