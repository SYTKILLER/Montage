#ifndef MONTAGE_ENGINE_VANISHING_OPS_H
#define MONTAGE_ENGINE_VANISHING_OPS_H

// M20d 消失点（photocraft crates/algo/src/vanishing.rs C++ 翻译，MIT OR Apache-2.0）。
// 平面（与照片中平面对齐的四边形）两消失点给相机焦距 f²=−(v₁−c)·(v₂−c)（Hartley &
// Zisserman §8.8 正交消失方向），把四边形举到 3-D（角点反投影到平面）——得平面度量
// 纵横比（贴图保比例）、垂直平面可绕共享边 3-D 撕出。贴图=源矩形进平面度量坐标后复合
// 单应 warp 裁剪到平面；透视克隆=度量偏移固定的 dab 拷贝（纹理随距离缩小）。

#include "engine/inpaint_ops.h"  // RectI
#include "engine/perspective_ops.h"  // Homography

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace montage {
namespace vanishing {

using inpaint::RectI;

struct Img {
    std::vector<float> px;  // w*h*4 RGBA
    int w = 0;
    int h = 0;
};

using V3 = std::array<double, 3>;

// 相机 + 举到 3-D 的平面
struct Lifted;

// 透视平面：角点图 px，自平面左上顺时针
struct VpPlane {
    std::array<std::array<double, 2>, 4> corners;

    // 单位方形 (u,v) → 图
    bool homography(perspective::Homography& out) const;
    // 两消失点推焦距（px；不可行 false）
    std::optional<double> focal(const std::array<double, 2>& c) const;
    // 平面可用性（PS 网格蓝/黄/红）
    // 0=Valid 1=Questionable 2=Invalid
    int check(const std::array<double, 2>& center) const;
    // 相机焦距 f 的 3-D 举起
    Lifted lift(double f, const std::array<double, 2>& c) const;

   private:
    bool turnsImpl(std::array<double, 4>& out) const;

   public:
    // 归一化：屏幕顺时针且自左上角起始
    VpPlane normalized() const;
};

// 撕出边
enum class Edge : int32_t { Top = 0, Right = 1, Bottom = 2, Left = 3 };

// 相机 + 举到 3-D 的平面
struct Lifted {
    double focal = 0.0;
    std::array<double, 2> center = {0.0, 0.0};
    V3 pts[4];
    std::optional<std::array<double, 2>> project(const V3& x) const;
    // 度量边长（u 沿顶边，v 沿左边）
    void lengths(double& lu, double& lv) const;
};

// 场景：平面 + 共享焦距
struct Scene {
    std::vector<VpPlane> planes;
    double focal = 0.0;
    std::array<double, 2> center = {0.0, 0.0};

    Scene(const VpPlane& first, const std::array<double, 2>& c, double focalIn, double fallback);
    Lifted lifted(int i) const;
    // 从 from 的 edge 撕出转 angle 度（90=垂直）的新平面，depth×邻边延长；返回序号
    std::optional<int> tearOff(int from, Edge edge, double angle, double depth);
    // 平面 i 的度量框架（单位方形单应，度量宽高）
    bool frame(int i, perspective::Homography& h, double& lu, double& lv) const;
    // 含图点 p 的平面（后建优先）
    std::optional<int> planeAt(const std::array<double, 2>& p) const;
};

// 贴图：src 在 srcRect 的内容贴上平面 i（左上 at 0..1，宽 width 平面占比，度量保比例）。
// 返回 warp 后裁剪到平面的图（含文档锚定 origin）。
struct PasteResult {
    Img img;
    double originX = 0.0;
    double originY = 0.0;
};
std::optional<PasteResult> paste(const Scene& sc, int i, const Img& src, const RectI& srcRect,
                                 const std::array<double, 2>& at, double width);

// 透视克隆：points 为目标平面图点一笔 dab，从 source 拷贝（偏移固定在首 dab 的度量单位）。
// size=首 dab 处笔刷直径 px；返回落地 dab 数。
int cloneStroke(const Scene& sc, Img& surf, const std::array<double, 2>& source,
                const std::vector<std::array<double, 2>>& points, double size, double hardness,
                float opacity);

}  // namespace vanishing

}  // namespace montage

#endif  // MONTAGE_ENGINE_VANISHING_OPS_H
