#ifndef MONTAGE_ENGINE_LIQUIFY_OPS_H
#define MONTAGE_ENGINE_LIQUIFY_OPS_H

// M20b 液化（photocraft crates/algo/src/liquify.rs 全量 C++ 翻译，MIT OR Apache-2.0）。
// 规则网格稠密位移场 + 冻结蒙版；笔画=纯数据（工具/笔刷/点列）按序重放，同界同笔画
// 位级同场（可重放命令/智能滤镜基建）。每工具写成「新输出 p 从当前输出的哪里来」
// q=M(p)，d'(p)=q+d(q)−p 从 dab 前快照双线性采样。渲染=场 Catmull-Rom 逐像素上采样 +
// 预乘源三次采样，零场瓦片原样保留。

#include "engine/document.h"
#include "engine/inpaint_ops.h"  // RectI

#include <memory>
#include <vector>

namespace montage {
using inpaint::RectI;

namespace liquify {

// 稠密 f32 RGBA 图像（非预乘存储；内核预乘运算）
struct Img {
    std::vector<float> px;  // w*h*4
    int w = 0;
    int h = 0;
};

// 液化工具（PS 左工具条去 Hand/Zoom/Face；数值序 = photocraft 枚举声明序）
enum class Tool : int32_t {
    ForwardWarp = 0,
    Reconstruct = 1,
    Smooth = 2,
    TwirlCw = 3,
    TwirlCcw = 4,
    Pucker = 5,
    Bloat = 6,
    PushLeft = 7,
    Freeze = 8,
    Thaw = 9,
    LassoMask = 10,
    ReconstructAll = 11,
    FreezeAll = 12,
    ThawAll = 13,
    InvertFreeze = 14,
};

// dab 行走游标（笔画进行态；非结果一部分，相等性不含它）
struct Cursor {
    std::array<double, 2> end{0.0, 0.0};
    std::array<double, 2> lastDab{0.0, 0.0};
    double carry = 0.0;
    bool valid = false;
};

// 一条笔画：工具 + 笔刷选项 + 点列（文档像素；第三维=压感 0..1）
struct Stroke {
    Tool tool = Tool::ForwardWarp;
    double size = 100.0;      // 笔刷直径 px
    double density = 50.0;    // 全效达到笔缘的距离 0..100（恒在边缘衰减为 0）
    double pressure = 100.0;  // 总强度 0..100
    double rate = 80.0;       // 定点工具速度 0..100
    std::vector<std::array<double, 3>> points;
    bool hasAmount = false;
    double amount = 0.0;      // reconstructAll 恢复量 / lasso 1=冻结 0=解冻
};

// 位移场 + 冻结蒙版（bounds 文档像素）
struct Field {
    RectI bounds;
    double cell = 2.0;  // 每场节点文档像素
    int w = 0;
    int h = 0;
    std::vector<std::array<float, 2>> d;    // 逆位移/节点，行主序
    std::vector<float> freeze;              // 冻结蒙版/节点（0 自由 1 冻结）
    Cursor cursor;                          // 笔画进行态（跨段携带 dab 间距）

    Field() = default;
    Field(const RectI& bounds_, double cell_);

    std::array<double, 2> nodePos(int i, int j) const;
    std::pair<double, double> toGrid(double x, double y) const;
    bool isIdentity() const;
    float maxDisplacement() const;
    std::array<double, 2> sample(double x, double y) const;
    float freezeAt(double x, double y) const;
    bool sameAs(const Field& o) const;

    // 应用一条笔画，返回输出改变的文档矩形
    RectI applyStroke(const Stroke& s);
    RectI strokeBegin(const Stroke& s, const std::array<double, 3>& p);
    RectI strokeSegment(const Stroke& s, const std::array<double, 3>& a,
                        const std::array<double, 3>& b);
    // 全行走（不剪枝；宿主 oracle 对比用，photocraft walk_segment skip=false 直迁）
    RectI strokeSegmentFull(const Stroke& s, const std::array<double, 3>& a,
                            const std::array<double, 3>& b);

    bool operator==(const Field& o) const { return sameAs(o); }
};

// bounds 默认场分辨率：≤4.2MP 每节点 2px， beyond 4px（photocraft auto_cell）
double autoCell(const RectI& bounds);

// dab 强度随平方距离 t²（笔刷半径单位）与 Density（0..1）的衰减（photocraft falloff）
double falloff(double t2, double density);

// 从零场重放笔画
Field fromStrokes(const RectI& bounds, double cell, const std::vector<Stroke>& strokes);

}  // namespace liquify

// 渲染：src（f32 RGBA 稠密，预乘域内核算）经场重采样；bounds 外原样保留。
// bitDepth 用于宿主 oracle 深度对比（8=引擎），<0 不量化。
liquify::Img applyLiquifyImage(const liquify::Img& src, const liquify::Field& field, int bitDepth);

// 引擎网格入口：RGBA8 TileGrid → 液化 → 新 TileGrid（revision=src+1；全域 f32 转换）
std::shared_ptr<const TileGrid> applyLiquifyGrid(const TileGrid& src, const liquify::Field& field);

// 区域化入口（M20b-2 交互路径）：field 以 region 局部坐标建场；region 内渲染、区外原样
// 保留（触区瓦片 COW 拷贝）；region 须已钳在网格内。源采样在 region 边缘钳制
//（=photocraft 足迹 ∩ field.bounds 同语义）。
std::shared_ptr<const TileGrid> applyLiquifyGridRegion(const TileGrid& src, const RectI& region,
                                                       const liquify::Field& field);

}  // namespace montage

#endif  // MONTAGE_ENGINE_LIQUIFY_OPS_H
