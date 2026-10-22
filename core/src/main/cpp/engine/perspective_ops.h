#ifndef MONTAGE_ENGINE_PERSPECTIVE_OPS_H
#define MONTAGE_ENGINE_PERSPECTIVE_OPS_H

// M20d 透视变形（photocraft crates/algo/src/perspective.rs C++ 翻译，MIT OR Apache-2.0）。
// 平面（四边形）→ 分段单应映射：每平面 M_i = H_dst ∘ H_src⁻¹（经单位方形，(u,v) 即平面
// 投影坐标）。共享边两侧单应对边上点目标间距一般不同——对每条共享边取两平面像的平均为
// 目标、差值按跨平面线性渐隐权重（1−u / u / 1−v / v）加回：角点处修正消失（共享角只有
// 一个目标），故跨共享边严格连续、单平面严格单应、全未动严格恒等。源上重合（≤0.5px）
// 的角点链接共用一源一目标（均值）——PS 吸附平面语义。

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace montage {
namespace perspective {

using Pt = std::array<double, 2>;

// 一平面：src=布局模式四边形，dst=变形模式四边形（角点顺时针自左上，文档像素）
struct Plane {
    std::array<Pt, 4> src;
    std::array<Pt, 4> dst;
};

Plane identityPlane(const std::array<Pt, 4>& q);

// 链接的角点组：源上重合的 (平面, 角) 对
std::vector<std::vector<std::pair<int, int>>> linkedCorners(const std::vector<Plane>& planes);

// 链接角点吸附到同一源/目标位（均值）
void unify(std::vector<Plane>& planes);

// 拉直模式（PS 变形模式按钮）
enum class Straighten : int32_t { Horizontal = 0, Vertical = 1, Auto = 2 };

// 近水平 dst 边强制水平/近垂直强制垂直（45° 内），链接角点联动
void straighten(std::vector<Plane>& planes, Straighten mode);

// 3×3 单应（公开供 vanishing_ops 等复用）
struct Homography {
    double h[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    Pt apply(double x, double y) const;
    bool invert(Homography& out) const;
    Homography mul(const Homography& o) const;
    // 单位方形 (0,0),(1,0),(1,1),(0,1) → 四边形（rect 参数仅保留语义占位）
    static bool rectToQuad(const double rect[4], const std::array<Pt, 4>& q, Homography& out);
};

// 准备好的透视变形（正向映射，源→目标）
class PerspectiveMap {
   public:
    // 四边形退化或无平面时 false
    bool build(const std::vector<Plane>& planes);
    // 源点 → 目标
    Pt map(double x, double y) const;
    // 全部平面映射到自身（恒等）
    static bool isIdentity(const std::vector<Plane>& planes);

    size_t planeCount() const { return planes_.size(); }

   private:
    struct PlaneMap {
        Homography src;
        Homography srcInv;
        Homography dst;
        std::array<Pt, 4> quad;
        int shared[4] = {-1, -1, -1, -1};
    };
    void locate(double x, double y, int& idx, double& u, double& v) const;
    std::vector<PlaneMap> planes_;
};

}  // namespace perspective

}  // namespace montage

#endif  // MONTAGE_ENGINE_PERSPECTIVE_OPS_H
