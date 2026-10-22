#ifndef MONTAGE_ENGINE_PUPPET_OPS_H
#define MONTAGE_ENGINE_PUPPET_OPS_H

// M20c 操控变形（photocraft crates/algo/src/puppet.rs 全量 C++ 翻译，MIT OR Apache-2.0）。
// 覆盖区 SDF → 轮廓追踪/简化/加密 → Delaunay 三角网格（边恢复+外三角剔除）→
// ARAP（Liu 2008 三角形形式：局部步=2×2 协方差最优旋转 atan2(S10−S01,S00+S11)，
// 全局步=每轴一次稀疏 SPD 解——带状 Cholesky（RCM 序）失败回退 Jacobi-PCG），
// 钉=重心软约束（stiff），无钉连通分量锚定。三种模式改局部步：Rigid=纯旋转、
// Normal=旋转×√scale、Distort=相似（ASAP）。渲染=变形三角形仿射逆映射双线性采样。

#include "engine/document.h"
#include "engine/inpaint_ops.h"  // RectI

#include <memory>
#include <vector>

namespace montage {
using inpaint::RectI;

namespace puppet {

// 稠密 f32 RGBA 图像（非预乘存储；内核按需预乘）
struct Img {
    std::vector<float> px;  // w*h*4
    int w = 0;
    int h = 0;
};

enum class Mode : int32_t { Rigid = 0, Normal = 1, Distort = 2 };
enum class Density : int32_t { Fewer = 0, Normal = 1, More = 2 };

double densityCells(Density d);

// 钉：src=放置处，dst=现在处（文档像素）；rotateDeg≥0 表示固定旋转（角度），<0=自动
struct Pin {
    double src[2] = {0.0, 0.0};
    double dst[2] = {0.0, 0.0};
    bool hasRotate = false;
    double rotateDeg = 0.0;
    int depth = 0;
};

// 完整操控变形（命令参数）
struct Warp {
    std::vector<Pin> pins;
    Mode mode = Mode::Normal;
    Density density = Density::Normal;
    double expansion = 2.0;  // 覆盖区外扩（负=收缩）px
};

bool warpIsIdentity(const Warp& w);

// 三角网格（文档像素）
struct Mesh {
    std::vector<std::array<double, 2>> verts;
    std::vector<std::array<int, 3>> tris;
    double spacing = 0.0;
    bool empty() const { return tris.empty(); }
};

// 网格构建：覆盖区（alpha>0.05）外扩 expansion 的轮廓 → 简化（保角）→ 加密 →
// Delaunay（轮廓边恢复）→ 剔除外三角 → RCM 重编号
Mesh buildMesh(const Img& src, const RectI& bounds, Density density, double expansion);

// ARAP 求解器（网格 + 钉源位置绑定）
class Solver {
   public:
    Solver() = default;
    Solver(const Mesh& mesh, const std::vector<std::array<double, 2>>& pinSrc,
           const std::vector<bool>& fixedRotation);

    // 变形：pinDst 每钉一个；rotations 每钉可选固定角（度，hasRotate）；iterations 轮；
    // 返回变形后顶点与每轮刚性能量
    void solve(std::vector<std::array<double, 2>>& outVerts, std::vector<double>& energies,
               const std::vector<std::array<double, 2>>& pinDst,
               const std::vector<Pin>& pins, Mode mode, int iterations,
               const std::vector<std::array<double, 2>>* init) const;

    double energy(const std::vector<std::array<double, 2>>& v,
                  const std::vector<std::array<double, 2>>& pinDst,
                  const std::vector<Pin>& pins) const;

    // 三角形绘制顺序（按最近钉深度由小到大）
    std::vector<int> drawOrder(const std::vector<int>& depths,
                               const std::vector<std::array<double, 2>>& pinSrc) const;

    Mesh mesh;

   private:
    std::vector<std::array<double, 3>> w_;  // 每三角 cot 权重（对角 k 的对边）
    struct Bound {
        int verts[3];
        double bary[3];
    };
    std::vector<Bound> bound_;
    std::vector<std::vector<int>> pinTris_;
    std::vector<bool> anchored_;
    // 稀疏矩阵（CSR）+ Jacobi 对角
    struct Csr {
        std::vector<int> rows;
        std::vector<int> cols;
        std::vector<double> vals;
        std::vector<double> diag;
        void mul(const std::vector<double>& x, std::vector<double>& out) const;
        void solve(std::vector<double> b, std::vector<double>& x) const;
    };
    Csr matrix_;
    // 带状 Cholesky
    struct BandChol {
        int n = 0;
        int p = 0;
        std::vector<double> l;
        bool ok = false;
        void solve(const std::vector<double>& b, std::vector<double>& x) const;
    };
    BandChol chol_;
    double pinWeight_ = 0.0;

    std::vector<std::array<double, 4>> localStep(const std::vector<std::array<double, 2>>& v,
                                                 Mode mode,
                                                 const std::vector<Pin>& pins) const;
    void globalStep(const std::vector<std::array<double, 4>>& rots,
                    const std::vector<std::array<double, 2>>& pinDst,
                    std::vector<std::array<double, 2>>& v) const;
    double energyWith(const std::vector<std::array<double, 2>>& v,
                      const std::vector<std::array<double, 4>>& rots,
                      const std::vector<std::array<double, 2>>& pinDst) const;
};

// 命令迭代轮数（photocraft ITERATIONS）
constexpr int kIterations = 40;

// 变形结果跨度上限 px²（2³⁰；拖飞钉保护 #1019）
constexpr double kMaxWarpPixels = 1073741824.0;

struct DeformResult {
    Solver solver;
    std::vector<std::array<double, 2>> verts;
    std::vector<int> order;
    bool ok = false;  // false=跨度超限（调用方保持原样）
};

DeformResult deform(const Img& src, const RectI& bounds, const Warp& w, int iterations);

// 渲染：变形三角形栅格化（bbox 扫描 + 重心内测 + 仿射逆映射双线性采样），
// 按 order 顺序覆盖；网格外像素清透明。输出 origin/outW/outH 由调用方给（文档锚定）。
Img renderPuppet(const Img& src, const RectI& bounds, const Mesh& mesh,
                 const std::vector<std::array<double, 2>>& deformed,
                 const std::vector<int>& order, double originX, double originY,
                 int outW, int outH);

// 变形后内容包围盒（文档 px；空网格时 false）
bool deformedBounds(const std::vector<std::array<double, 2>>& verts, double out[4]);

// 引擎网格入口：RGBA8 TileGrid → 操控变形 → 新 TileGrid（失败/超限返回 nullptr=不改动）。
// outOrigin = 新网格 (0,0) 的文档坐标（调用方回写层 origin）。
std::shared_ptr<const TileGrid> applyPuppetGrid(const TileGrid& src, const RectI& bounds,
                                                const Warp& w, double outOrigin[2]);

}  // namespace puppet

}  // namespace montage

#endif  // MONTAGE_ENGINE_PUPPET_OPS_H
