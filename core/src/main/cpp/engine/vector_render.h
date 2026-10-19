#ifndef MONTAGE_ENGINE_VECTOR_RENDER_H
#define MONTAGE_ENGINE_VECTOR_RENDER_H

// M18 形状层渲染核：ShapeLayerState → shapeCache 派生网格（文字层/样式层同架构）。
// 光栅化 = vector 内核（fill/stroke Rasterizer → 覆盖度 × 纯色，描边 src-over 填充之上，
// photocraft CompiledShape::render_rgba 同式）；域 = 形状包围盒 ∩ 画布（photocraft clip=canvas
// 同语义），原点可非零（Layer.shapeOriginX/Y）。预算闸 = 域像素 ≤ 64MP（style_ops 同量级）。
// live 形状先经 liveShapePath 生成路径；路径编辑后 live 清 None（UI 命令侧负责）。

#include <string>

#include "engine/document.h"

namespace montage {

// 渲染指纹（path 全字段 + 填充 + 描边 + 文档尺寸）。
uint64_t shapeCacheHash(const ShapeLayerState& s, uint32_t docW, uint32_t docH);

// 重派生形状缓存（前置 l.shape != nullptr）。指纹未变且已有缓存 → 直接 true。
// 空/全透明结果 → shapeCache = nullptr。调用方持 docMutex。
bool shapeRenderLayer(Document& doc, Layer& l, std::string& err);

// 全文档形状层重派生（工程打开/几何变换后调用；返回处理层数）。
int shapeRenderDocument(Document& doc, std::string& firstErr);

// 形状层平移：path 全部点位移（形状层唯一几何真源；move/对齐/画布重设联动）。
void translateShapeLayer(Layer& l, double dx, double dy);

// 形状层仿射（文档域绕原点）：flip/rotate90/重采样缩放/任意角旋转联动。
// 矩阵 = [a c e; b d f]（x' = a·x + c·y + e）。随 History 快照走（调用方包事务）。
void affineShapeLayer(Layer& l, double a, double b, double c, double d, double e, double f);

// 形状层 → 普通像素层（栅格化转换命令核）：shape 状态转 pixels（以 shapeCache 为像素、
// origin = shapeOrigin），丢弃 shape/vectorMask 派生。前置 l.shape != nullptr。
void rasterizeShapeLayerToPixels(Layer& l);

// 图层名（PS 语义：live 形状按种类「矩形/椭圆/多边形/星形/直线/形状 1」）。
std::string shapeLayerDisplayName(const ShapeLayerState& s);

// 路径 → 选区掩码（文档域 8-bit 覆盖灰度瓦片，selection 同约定；域 = 路径包围盒 ∩ 画布）。
// 空路径/域空返回 nullptr。
std::shared_ptr<const TileGrid> pathToSelectionMask(const VectorPath& path, uint32_t docW,
                                                    uint32_t docH);

// 覆盖度域（x0,y0 起 w×h）→ 灰度掩码瓦片（填充路径/描边路径共用，selection 同约定）。
std::shared_ptr<const TileGrid> coverageToMaskGrid(const std::vector<float>& cov, int32_t x0,
                                                   int32_t y0, int32_t w, int32_t h, uint32_t docW,
                                                   uint32_t docH);

// 选区 → 工作路径（M18c trace v1：选中度>127 轮廓追踪 → RDP 折线简化 + 角点检测；
// 样条平滑登记后续）。掩码空返回 false。maxPoints 钳制输出锚点总量（64..4096）。
bool traceSelectionToPath(const TileGrid& mask, VectorPath& out, size_t maxPoints);

}  // namespace montage

#endif  // MONTAGE_ENGINE_VECTOR_RENDER_H
