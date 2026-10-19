#ifndef MONTAGE_ENGINE_VECTOR_RASTER_H
#define MONTAGE_ENGINE_VECTOR_RASTER_H

// M18 抗锯齿多边形栅格化器：精确面积覆盖 + 布尔组合。翻译自 photocraft
// crates/vector/src/raster.rs（MIT OR Apache-2.0）。
//
// 每像素行在每个边端点与每条边与行的交点处切成水平子带，子带内全部活动边贯穿整带高度
// 且左右序恒定。按序走边、每个「分量」一个绕数计数器，即可判断任意两相邻边之间是否在
// 组合形状内部（逐分量 non-zero/even-odd，再按路径运算折叠——PS 语义首分量恒 Combine）。
// 内部区间的边界边以有符号梯形面积（边右侧面积：左界 +1、右界 −1）累入每行 cell 缓冲，
// 前缀和 = 每像素精确覆盖面积。经典 scanline/area-coverage 技术（FreeType gray 栅格化器
// 同族），无需子采样，重叠分量处理精确。

#include <cstdint>
#include <vector>

#include "engine/vector_path.h"

namespace montage {

// 低于此值是 f32 求和的舍入残差而非面积：远小于半个 16-bit 步长（7.6e-6），
// 8/16-bit 输出不受影响。
constexpr float kRasterResidue = 1e-6f;

// 一个组合步骤：一组按 rule 填充的多边形及其折叠方式。
struct RasterComponent {
    PathOp op = PathOp::Combine;
    FillRule rule = FillRule::NonZero;
};

class VectorRasterizer {
  public:
    explicit VectorRasterizer(bool inverted) : inverted_(inverted), unionOnly_(true) {}

    // 加入一个由闭合多边形组成的分量（开放输入隐式闭合；<2 点跳过；非有限坐标跳过）。
    void addComponent(const std::vector<std::vector<VPt>>& polys, PathOp op, FillRule rule);

    // 可能被覆盖的像素包围盒（false = 空；inverted 返回 false——调用方自裁剪）。
    // 上边钳到 int32 最大值。
    bool pixelBounds(int32_t& x0, int32_t& y0, int32_t& x1, int32_t& y1) const;

    bool isInverted() const { return inverted_; }

    // rect（x0,y0,x1,y1 排他）内每像素覆盖度 0..=1（行主序）。
    std::vector<float> render(int32_t rx0, int32_t ry0, int32_t rx1, int32_t ry1) const;

    // 渲染到 out（rw*rh 项），顺序执行。
    void renderInto(int32_t rx0, int32_t ry0, int32_t rw, int32_t rh, float* out) const;

  private:
    struct Edge {
        double x0 = 0.0;
        double y0 = 0.0;
        double y1 = 0.0;
        int32_t dir = 1;  // +1 源段向下（y 增），−1 向上
        uint32_t comp = 0;
        double dxdy = 0.0;
        double xAt(double y) const { return x0 + (y - y0) * dxdy; }
    };
    struct BandEdge {
        double xa = 0.0;
        double xb = 0.0;
        double xm = 0.0;
        uint32_t idx = 0;
    };
    struct RowState {
        std::vector<size_t> active;
        std::vector<double> cuts;
        std::vector<BandEdge> band;
        std::vector<int32_t> wind;
        std::vector<float> acc;  // cell 累加器：width+1 格（多一格吸收外溢）
        RowState(size_t comps, size_t width)
            : wind(std::max(comps, static_cast<size_t>(1)), 0), acc(width + 1, 0.0f) {}
    };

    bool inside(const int32_t* wind, int32_t unionCount) const;
    bool compInside(size_t comp, int32_t w) const;
    void band(double a, double b, int32_t x0, RowState& st, uint32_t depth) const;

    std::vector<Edge> edges_;  // 按 y0 排序
    std::vector<RasterComponent> comps_;
    bool inverted_;
    bool unionOnly_;  // 全部分量 Combine 且无 inverted：inside = 任一分量 inside
    bool hasBounds_ = false;
    double bx0_ = 0.0, by0_ = 0.0, bx1_ = 0.0, by1_ = 0.0;
};

// 路径填充区域编译器：每子路径一个分量（按路径 fillRule），按分量 op 折叠；开放子路径
// 隐式闭合（photocraft fill_rasterizer）。
VectorRasterizer fillRasterizer(const VectorPath& path, double tol);

// 路径描边轮廓编译器（全部子路径，单一 non-zero 分量）。描边轮廓容忍更粗的展平
//（偏移掩盖弦误差）（photocraft stroke_rasterizer）。
VectorRasterizer strokeRasterizer(const VectorPath& path, double width, LineCap cap, LineJoin join,
                                  double miterLimit, const std::vector<double>& dashesPx,
                                  double dashOffsetPx, double tol);

}  // namespace montage

#endif  // MONTAGE_ENGINE_VECTOR_RASTER_H
