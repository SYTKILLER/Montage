#ifndef MONTAGE_ENGINE_BRUSH_H
#define MONTAGE_ENGINE_BRUSH_H

// 笔画草稿（M3，对拍 BrushStroke.swift 的 CPU 路径核心语义）：
// - coverage 覆盖层：笔画内 dab 按 max（硬刷）/screen（软刷）累计，保证笔画级 opacity
//   上限（重叠 dab 不超过设定不透明度）；publish 时 base + color × coverage × opacity。
// - Catmull-Rom 样本曲线（向心式，过每个样本）+ dab 均匀步进（spacing = d × 0.015/0.025）。
// - 软刷 falloff：归一化高斯 exp(-2.5u²) 跨整个半径衰减（BrushRaster.falloff）。
// - 橡皮：coverage 乘掉图层 alpha（destinationOut 语义）。
// - 简化（相对源，登记 M3.1）：无 provisional tail（末段随样本即时落 dab）、无 GPU 路径、
//   无压感（模拟器无压感）、无 clone/heal/blur（后续里程碑）。

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {

struct BrushSettings {
    float diameter = 40.0f;
    float hardness = 1.0f;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float opacity = 1.0f;
    bool erasing = false;
};

class StrokeDraft {
  public:
    // 绑定图层开始笔画。grid = 图层像素网格（origin 平移，1:1）；空图层从触笔区域动态扩展。
    bool begin(const Document& doc, LayerId layerId, const BrushSettings& settings, std::string& err);
    // 文档坐标追加样本（Catmull-Rom 落 dab）
    void append(float docX, float docY);
    void end();
    void cancel();

    bool active() const { return active_; }
    LayerId layerId() const { return layerId_; }
    // 渲染取数：draft 中该瓦片的合成像素（直通 RGBA，256×256）；无则返回 false
    bool tileFor(uint32_t tx, uint32_t ty, std::vector<uint8_t>& out) const;
    const std::set<uint32_t>& touchedTiles() const { return touched_; }
    uint32_t gridCols() const { return gridCols_; }
    uint32_t gridRows() const { return gridRows_; }

    // 提交：工作瓦片合并到图层原 grid → 新不可变 TileGrid（调用方负责换入 + bump）
    std::shared_ptr<const TileGrid> commitGrid(const TileGrid* original);
    uint64_t gridRevision = 1;

  private:
    struct Pt {
        float x = 0, y = 0;
    };
    void dab(float gx, float gy);
    void walkTo(float gx, float gy);
    void curveTo(float gx, float gy);
    void ensureWorking(uint32_t key);
    void allocWorking(uint32_t key, uint32_t tx, uint32_t ty);
    void paintTile(uint32_t key);
    static float falloff(float u) {
        constexpr float k = 2.5f;
        const float e = std::exp(-k * u * u);
        const float base = std::exp(-k);
        return std::max(0.0f, (e - base) / (1.0f - base));
    }

    std::shared_ptr<const TileGrid> base_;  // 图层原瓦片（工作瓦片 base 拷贝源）
    std::vector<float> radial_;             // 软刷 1D 径向 falloff 表

    BrushSettings settings_{};
    LayerId layerId_ = 0;
    bool active_ = false;
    bool layerHadPixels_ = false;
    int originX_ = 0;   // 网格原点 doc 坐标（= 图层 origin；空层 = 首触点整瓦片对齐）
    int originY_ = 0;
    uint32_t gridCols_ = 0;
    uint32_t gridRows_ = 0;
    uint32_t imgW_ = 0;  // 图层已有像素尺寸（0 = 空图层）
    uint32_t imgH_ = 0;
    // 工作瓦片（key = ty*cols+tx）：cov 灰度累计 + 合成结果（直通 RGBA）
    std::map<uint32_t, std::vector<uint8_t>> coverage_;
    std::map<uint32_t, std::vector<uint8_t>> work_;
    std::set<uint32_t> touched_;
    // dab 步进状态
    bool hasPrev_ = false;
    float prevX_ = 0;
    float prevY_ = 0;
    float distToNext_ = 0;
    // Catmull-Rom 样本（≤4）
    std::vector<Pt> samples_;

};

}  // namespace montage

#endif  // MONTAGE_ENGINE_BRUSH_H
