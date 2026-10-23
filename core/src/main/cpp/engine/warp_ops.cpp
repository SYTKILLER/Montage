// M20 网格/三角纹理渲染 —— photocraft crates/algo/src/warp.rs C++ 翻译（MIT OR Apache-2.0）。
// 单缓冲稠密版（引擎 Img ≤20MP 预算；photocraft 的瓦片分箱并行留 GPU/后续批）。

#include "engine/warp_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace montage {
namespace warp {

namespace {
constexpr double kCell = 4.0;
constexpr int kMaxCells = 512;
constexpr double kMaxSs = 8.0;
constexpr double kPi = 3.14159265358979323846;

std::array<double, 4> catmullRom(double t) {
    const double t2 = t * t;
    const double t3 = t2 * t;
    return {-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t,
            0.5 * t3 - 0.5 * t2};
}
}  // namespace

TriRender warpTrianglesImg(const Img& src, const RectI& srcRect,
                           const std::vector<std::pair<std::array<double, 2>, std::array<double, 2>>>& verts,
                           const std::vector<std::array<int, 3>>& tris, Interp interp,
                           double originX, double originY, int outW, int outH) {
    TriRender res;
    res.originX = originX;
    res.originY = originY;
    res.out.w = outW;
    res.out.h = outH;
    res.out.px.assign(static_cast<size_t>(std::max(0, outW)) * std::max(0, outH) * 4u, 0.0f);
    if (srcRect.empty() || outW <= 0 || outH <= 0 || tris.empty()) {
        return res;
    }
    for (const auto& v : verts) {
        if (!std::isfinite(v.first[0]) || !std::isfinite(v.first[1])) {
            return res;  // photocraft：非有限顶点整图返回透明
        }
    }
    const int w = outW;
    const int h = outH;
    // 预乘源（srcRect 区域拷贝）
    const int fw = srcRect.width();
    const int fh = srcRect.height();
    std::vector<float> px(static_cast<size_t>(fw) * fh * 4u);
    for (int y = 0; y < fh; ++y) {
        std::memcpy(&px[static_cast<size_t>(y) * fw * 4u],
                    &src.px[(static_cast<size_t>(srcRect.y0 + y) * src.w + srcRect.x0) * 4u],
                    static_cast<size_t>(fw) * 4u * sizeof(float));
    }
    for (size_t i = 0; i < px.size(); i += 4u) {
        const float al = px[i + 3u];
        px[i] *= al;
        px[i + 1u] *= al;
        px[i + 2u] *= al;
    }
    const auto at = [&](int x, int y, int c) -> double {
        if (x < srcRect.x0 || y < srcRect.y0 || x >= srcRect.x1 || y >= srcRect.y1) {
            return 0.0;
        }
        return px[(static_cast<size_t>(y - srcRect.y0) * fw + (x - srcRect.x0)) * 4u + c];
    };
    // 单点预乘采样（像素中心=整数坐标约定：调用处 −0.5）
    auto point = [&](Interp it, double u, double v, std::array<double, 8>& acc) -> void {
        for (int c = 0; c < 4; ++c) {
            acc[c] = 0.0;
        }
        switch (it) {
            case Interp::Nearest: {
                const int ix = static_cast<int>(std::floor(u + 0.5));
                const int iy = static_cast<int>(std::floor(v + 0.5));
                for (int c = 0; c < 4; ++c) {
                    acc[c] = at(ix, iy, c);
                }
                break;
            }
            case Interp::Bilinear: {
                const int ix = static_cast<int>(std::floor(u));
                const int iy = static_cast<int>(std::floor(v));
                const double fx = u - ix;
                const double fy = v - iy;
                for (const auto& dy : {std::pair{0, 1.0 - fy}, std::pair{1, fy}}) {
                    for (const auto& dx : {std::pair{0, 1.0 - fx}, std::pair{1, fx}}) {
                        for (int c = 0; c < 4; ++c) {
                            acc[c] += at(ix + dx.first, iy + dy.first, c) * dx.second * dy.second;
                        }
                    }
                }
                break;
            }
            case Interp::Bicubic:
            default: {
                auto snap = [](double t) {
                    const double r = std::round(t);
                    return std::fabs(t - r) < 1e-7 ? r : t;
                };
                u = snap(u);
                v = snap(v);
                const int ix = static_cast<int>(std::floor(u));
                const int iy = static_cast<int>(std::floor(v));
                const std::array<double, 4> wx = catmullRom(u - ix);
                const std::array<double, 4> wy = catmullRom(v - iy);
                for (int j = 0; j < 4; ++j) {
                    for (int i = 0; i < 4; ++i) {
                        const double k = wx[i] * wy[j];
                        if (k == 0.0) {
                            continue;
                        }
                        for (int c = 0; c < 4; ++c) {
                            acc[c] += at(ix - 1 + i, iy - 1 + j, c) * k;
                        }
                    }
                }
                break;
            }
        }
    };
    // uv/owner + 每三角 Jacobian（整图单缓冲；覆盖像素被后来的三角形覆写=深度序）
    std::vector<std::array<double, 2>> uv(static_cast<size_t>(w) * h,
                                          {std::nan(""), std::nan("")});
    std::vector<int> owner(static_cast<size_t>(w) * h, -1);
    struct Jac {
        std::array<double, 4> j;
        int nx;
        int ny;
    };
    std::vector<Jac> jac;
    jac.reserve(tris.size());
    for (const auto& tri : tris) {
        const auto& p0v = verts[tri[0]].first;
        const auto& p1v = verts[tri[1]].first;
        const auto& p2v = verts[tri[2]].first;
        const auto& s0 = verts[tri[0]].second;
        const auto& s1 = verts[tri[1]].second;
        const auto& s2 = verts[tri[2]].second;
        const double det = (p1v[0] - p0v[0]) * (p2v[1] - p0v[1]) - (p2v[0] - p0v[0]) * (p1v[1] - p0v[1]);
        if (std::fabs(det) < 1e-12) {
            jac.push_back({{0.0, 0.0, 0.0, 0.0}, 1, 1});
            continue;
        }
        const double d1[2] = {s1[0] - s0[0], s1[1] - s0[1]};
        const double d2[2] = {s2[0] - s0[0], s2[1] - s0[1]};
        const double l1x = (p2v[1] - p0v[1]) / det;
        const double l1y = (p0v[0] - p2v[0]) / det;
        const double l2x = (p0v[1] - p1v[1]) / det;
        const double l2y = (p1v[0] - p0v[0]) / det;
        auto cap = [](std::array<double, 2> jv) {
            const double l = std::hypot(jv[0], jv[1]);
            if (l > kMaxSs) {
                return std::array<double, 2>{jv[0] * kMaxSs / l, jv[1] * kMaxSs / l};
            }
            return jv;
        };
        const std::array<double, 2> jx = cap({l1x * d1[0] + l2x * d2[0], l1x * d1[1] + l2x * d2[1]});
        const std::array<double, 2> jy = cap({l1y * d1[0] + l2y * d2[0], l1y * d1[1] + l2y * d2[1]});
        auto count = [&](const std::array<double, 2>& jv) -> int {
            if (interp == Interp::Nearest) {
                return 1;
            }
            return static_cast<int>(std::clamp(std::round(std::hypot(jv[0], jv[1])), 1.0, kMaxSs));
        };
        const int nx = count(jx);
        const int ny = count(jy);
        jac.push_back(nx * ny == 1 ? Jac{{0.0, 0.0, 0.0, 0.0}, 1, 1}
                                   : Jac{{jx[0], jx[1], jy[0], jy[1]}, nx, ny});
        const int bx0 = static_cast<int>(std::max(std::floor(std::min({p0v[0], p1v[0], p2v[0]}) - 0.5),
                                                  static_cast<double>(originX)));
        const int by0 = static_cast<int>(std::max(std::floor(std::min({p0v[1], p1v[1], p2v[1]}) - 0.5),
                                                  static_cast<double>(originY)));
        const int bx1 = static_cast<int>(std::min(std::ceil(std::max({p0v[0], p1v[0], p2v[0]}) + 0.5),
                                                  static_cast<double>(originX + w)));
        const int by1 = static_cast<int>(std::min(std::ceil(std::max({p0v[1], p1v[1], p2v[1]}) + 0.5),
                                                  static_cast<double>(originY + h)));
        constexpr double kEps = -1e-9;
        for (int y = by0; y < by1; ++y) {
            const double py = y + 0.5;
            for (int x = bx0; x < bx1; ++x) {
                const double pxx = x + 0.5;
                const double l1 = ((pxx - p0v[0]) * (p2v[1] - p0v[1]) - (p2v[0] - p0v[0]) * (py - p0v[1])) / det;
                const double l2 = ((p1v[0] - p0v[0]) * (py - p0v[1]) - (pxx - p0v[0]) * (p1v[1] - p0v[1])) / det;
                const double l0 = 1.0 - l1 - l2;
                if (l0 < kEps || l1 < kEps || l2 < kEps) {
                    continue;
                }
                const size_t o = static_cast<size_t>(y - originY) * w + (x - originX);
                uv[o] = {l0 * s0[0] + l1 * s1[0] + l2 * s2[0], l0 * s0[1] + l1 * s1[1] + l2 * s2[1]};
                owner[o] = static_cast<int>(jac.size()) - 1;
            }
        }
    }
    std::array<double, 8> acc{0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    std::array<double, 8> one{0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    for (size_t o = 0; o < uv.size(); ++o) {
        const std::array<double, 2>& q = uv[o];
        if (std::isnan(q[0])) {
            continue;
        }
        const Jac& jc = jac[static_cast<size_t>(owner[o])];
        if (jc.nx * jc.ny == 1) {
            point(interp, q[0] - 0.5, q[1] - 0.5, acc);
        } else {
            for (int c = 0; c < 4; ++c) {
                acc[c] = 0.0;
            }
            for (int sy = 0; sy < jc.ny; ++sy) {
                const double ty = (sy + 0.5) / jc.ny - 0.5;
                for (int sx = 0; sx < jc.nx; ++sx) {
                    const double tx = (sx + 0.5) / jc.nx - 0.5;
                    point(Interp::Bilinear, q[0] + tx * jc.j[0] + ty * jc.j[2] - 0.5,
                          q[1] + tx * jc.j[1] + ty * jc.j[3] - 0.5, one);
                    for (int c = 0; c < 4; ++c) {
                        acc[c] += one[c];
                    }
                }
            }
            const double inv = 1.0 / (jc.nx * jc.ny);
            for (int c = 0; c < 4; ++c) {
                acc[c] *= inv;
            }
        }
        const double al = std::clamp(acc[3], 0.0, 1.0);
        if (al <= 1e-6) {
            continue;  // 浮点噪声不留隐形色
        }
        float* outp = &res.out.px[o * 4u];
        for (int c = 0; c < 3; ++c) {
            outp[c] = static_cast<float>(std::clamp(acc[c] / al, 0.0, 1.0));
        }
        outp[3] = static_cast<float>(al);
    }
    return res;
}

TriRender warpMeshImg(const Img& src, const RectI& srcRect,
                      const std::function<std::array<double, 2>(double, double)>& f, Interp interp) {
    TriRender res;
    res.out.w = 0;
    res.out.h = 0;
    if (srcRect.empty()) {
        return res;
    }
    const double sw = srcRect.width();
    const double sh = srcRect.height();
    const int nx = std::clamp(static_cast<int>(std::ceil(sw / kCell)), 1, kMaxCells);
    const int ny = std::clamp(static_cast<int>(std::ceil(sh / kCell)), 1, kMaxCells);
    std::vector<std::pair<std::array<double, 2>, std::array<double, 2>>> verts;
    verts.reserve(static_cast<size_t>(nx + 1) * (ny + 1));
    double x0 = 1e308, y0 = 1e308, x1 = -1e308, y1 = -1e308;
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            const double sx = srcRect.x0 + sw * i / nx;
            const double sy = srcRect.y0 + sh * j / ny;
            const auto d = f(sx, sy);
            verts.push_back({{d[0], d[1]}, {sx, sy}});
            x0 = std::min(x0, d[0]);
            y0 = std::min(y0, d[1]);
            x1 = std::max(x1, d[0]);
            y1 = std::max(y1, d[1]);
        }
    }
    const int w1 = nx + 1;
    std::vector<std::array<int, 3>> tris;
    tris.reserve(static_cast<size_t>(nx) * ny * 2);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const int a = j * w1 + i;
            tris.push_back({a, a + 1, a + w1 + 1});
            tris.push_back({a, a + w1 + 1, a + w1});
        }
    }
    const double originX = std::floor(x0);
    const double originY = std::floor(y0);
    const int outW = static_cast<int>(std::ceil(x1)) - static_cast<int>(originX);
    const int outH = static_cast<int>(std::ceil(y1)) - static_cast<int>(originY);
    return warpTrianglesImg(src, srcRect, verts, tris, interp, originX, originY, outW, outH);
}

}  // namespace warp

}  // namespace montage
