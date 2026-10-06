#ifndef MONTAGE_ENGINE_BRUSH_H
#define MONTAGE_ENGINE_BRUSH_H

// 笔画草稿（M3，对拍 BrushStroke.swift 的 CPU 路径核心语义）：
// - coverage 覆盖层：笔画内 dab 按 max（硬刷）/screen（软刷）累计，保证笔画级 opacity
//   上限（重叠 dab 不超过设定不透明度）；publish 时 base + color × coverage × opacity。
// - Catmull-Rom 样本曲线（向心式，过每个样本）+ dab 均匀步进（spacing = d × 0.015/0.025）。
// - 软刷 falloff：归一化高斯 exp(-2.5u²) 跨整个半径衰减（BrushRaster.falloff）。
// - 橡皮：coverage 乘掉图层 alpha（destinationOut 语义）。
// - M3.1 补全：provisional tail（最新样本先以直线尾段连到光标 + 整瓦 coverage 备份，
//   下一样本/收尾时还原再落最终曲线段——笔画不滞后于光标）；压感调制 dab 直径
//   （几何安全：不透明度压感会破坏 coverage max/screen 笔画级模型，登记后续）。
// - 简化（相对源，登记后续里程碑）：无 GPU 路径、无 clone/heal/blur。

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
    // onMask = true 时绑定图层蒙版 patch 网格（灰度落笔：L×covA + base×(1-covA)；
    // 要求蒙版已有 patch 网格，erasing 标志被忽略）。
    bool begin(const Document& doc, LayerId layerId, const BrushSettings& settings, bool onMask,
               std::string& err);
    // 文档坐标追加样本（Catmull-Rom 落 dab；pressure ∈ [0,1]，调制 dab 直径）
    void append(float docX, float docY, float pressure);
    void end();
    void cancel();

    bool active() const { return active_; }
    LayerId layerId() const { return layerId_; }
    bool maskTarget() const { return maskTarget_; }
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
        float pressure = 1.0f;
    };
    void dab(float gx, float gy);
    float effRadius() const;  // 压感调制后的 dab 半径
    void walkTo(float gx, float gy);
    // 向心 Catmull-Rom 曲线段（显式四控制点；压感取 to 点）
    void curvePiece(const Pt& from, const Pt& to, const Pt& before, const Pt& after);
    // provisional tail：备份可能触及的整瓦 coverage + dab 步进状态，直线走到光标后还原状态
    void drawTail(const Pt& from, const Pt& to);
    // 还原尾段（coverage/工作瓦片/触达集），下一样本或收尾前调用
    void removeTail();
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
    std::vector<float> radial_;             // 软刷 1D 径向 falloff 表（归一化索引，尺度不变）

    BrushSettings settings_{};
    LayerId layerId_ = 0;
    bool active_ = false;
    bool maskTarget_ = false;  // M5b：落笔目标 = 图层蒙版（灰度）
    float maskLum_ = 0.0f;     // 蒙版笔刷亮度（settings 颜色均值）
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
    // provisional tail 备份：key → 尾段前整瓦 coverage（空 vector = 尾段前无 coverage）
    std::map<uint32_t, std::vector<uint8_t>> tailBackup_;
    // M7a：选区约束（文档域掩码；null = 无选区）。dab coverage × 选区灰度
    const TileGrid* selMask_ = nullptr;

    // dab 步进状态
    bool hasPrev_ = false;
    float prevX_ = 0;
    float prevY_ = 0;
    float distToNext_ = 0;
    float pressure_ = 1.0f;  // 当前段压感（dab 直径调制）
    // Catmull-Rom 样本（≤4）
    std::vector<Pt> samples_;

};

}  // namespace montage

#endif  // MONTAGE_ENGINE_BRUSH_H
