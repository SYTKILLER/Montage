#ifndef MONTAGE_ENGINE_WIDEANGLE_OPS_H
#define MONTAGE_ENGINE_WIDEANGLE_OPS_H

// M20d 自适应广角（photocraft crates/algo/src/wideangle.rs C++ 翻译，MIT OR Apache-2.0）。
// 镜头模型（等距鱼眼 r=f·θ / 直线透视 r=f·tanθ / 2:1 全球形经纬）把约束线端点变射线，
// 场景直线=过两射线的大圆，采样回图=用户所见曲线；内容保持网格变形（Carroll 2009 /
// Igarashi 2005 相似步 + 线约束项，CGLS 稀疏最小二乘，自由方向法向 4 轮重估）使约束
// 曲线变直（水平/垂直约束法向固定）。渲染=网格三角形纹理映射（复用 M20c 渲染路径）。

#include "engine/inpaint_ops.h"  // RectI

#include <array>
#include <cstdint>
#include <vector>

namespace montage {
namespace wideangle {

using inpaint::RectI;

enum class Model : int32_t { Auto = 0, Fisheye = 1, Perspective = 2, FullSpherical = 3 };
enum class Orientation : int32_t { Free = 0, Horizontal = 1, Vertical = 2 };

struct Constraint {
    std::array<double, 2> a = {0.0, 0.0};
    std::array<double, 2> b = {0.0, 0.0};
    Orientation orientation = Orientation::Free;
};

struct Params {
    Model model = Model::Auto;
    double focalLength = 0.0;  // mm（35mm 等效）
    double cropFactor = 1.0;
    double scale = 100.0;  // 输出缩放 %
    std::vector<Constraint> constraints;
};

// 帧上的相机模型
struct Camera {
    Model model = Model::Perspective;
    double f = 1.0;  // px
    std::array<double, 2> c = {0.0, 0.0};
    std::array<double, 2> size = {1.0, 1.0};

    static Camera make(const Params& p, const RectI& frame);
    std::array<double, 3> ray(double x, double y) const;
    bool pixel(const std::array<double, 3>& d, std::array<double, 2>& out) const;
    // 场景线（大圆）a→b 上 n+1 个图点
    std::vector<std::array<double, 2>> arc(const std::array<double, 2>& a,
                                           const std::array<double, 2>& b, int n) const;
};

// 解得的变形：帧上网格，每顶点输出位
struct Mesh {
    int nx = 0;
    int ny = 0;
    // (output, source)/顶点，行主序 (nx+1)×(ny+1)
    std::vector<std::pair<std::array<double, 2>, std::array<double, 2>>> verts;
    std::vector<std::vector<std::array<double, 2>>> curves;  // 源域约束曲线（overlay 用）
    double residual = 0.0;  // 变形后约束曲线偏离直线的最大值 px

    std::vector<std::array<int, 3>> triangles() const;
    // 源点输出位（格内双线性）
    std::array<double, 2> map(const RectI& frame, const std::array<double, 2>& p) const;
};

// 解内容保持变形
Mesh solve(const Params& p, const RectI& frame);

}  // namespace wideangle

}  // namespace montage

#endif  // MONTAGE_ENGINE_WIDEANGLE_OPS_H
