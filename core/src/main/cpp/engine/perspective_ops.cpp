// M20d 透视变形 —— photocraft crates/algo/src/perspective.rs C++ 翻译（MIT OR Apache-2.0）。

#include "engine/perspective_ops.h"

#include <algorithm>
#include <cmath>

namespace montage {
namespace perspective {

namespace {
constexpr double kLinkEps = 0.5;

double dist2(const Pt& a, const Pt& b) {
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    return dx * dx + dy * dy;
}
}  // namespace

Pt PerspectiveMap::Homography::apply(double x, double y) const {
    const double w = h[6] * x + h[7] * y + h[8];
    if (std::fabs(w) < 1e-15) {
        return {1e308, 1e308};
    }
    return {(h[0] * x + h[1] * y + h[2]) / w, (h[3] * x + h[4] * y + h[5]) / w};
}

bool PerspectiveMap::Homography::invert(Homography& out) const {
    // 3×3 伴随/行列式
    const double* a = h;
    const double i0 = a[4] * a[8] - a[5] * a[7];
    const double i1 = a[5] * a[6] - a[3] * a[8];
    const double i2 = a[3] * a[7] - a[4] * a[6];
    const double det = a[0] * i0 + a[1] * i1 + a[2] * i2;
    if (std::fabs(det) < 1e-12) {
        return false;
    }
    out.h[0] = i0 / det;
    out.h[1] = (a[2] * a[7] - a[1] * a[8]) / det;
    out.h[2] = (a[1] * a[5] - a[2] * a[4]) / det;
    out.h[3] = i1 / det;
    out.h[4] = (a[0] * a[8] - a[2] * a[6]) / det;
    out.h[5] = (a[2] * a[3] - a[0] * a[5]) / det;
    out.h[6] = i2 / det;
    out.h[7] = (a[1] * a[6] - a[0] * a[7]) / det;
    out.h[8] = (a[0] * a[4] - a[1] * a[3]) / det;
    return true;
}

// 单位方形 (0,0),(1,0),(1,1),(0,1) → 四边形（Heckbert 方形到四边形单应）
bool PerspectiveMap::rectToQuad(const double rect[4], const std::array<Pt, 4>& q, Homography& out) {
    (void)rect;  // v1 只用单位方形（plane 坐标即 (u,v)）
    const double x0 = q[0][0], y0 = q[0][1];
    const double x1 = q[1][0], y1 = q[1][1];
    const double x2 = q[2][0], y2 = q[2][1];
    const double x3 = q[3][0], y3 = q[3][1];
    const double dx1 = x1 - x2, dx2 = x3 - x2;
    const double dy1 = y1 - y2, dy2 = y3 - y2;
    const double sx = x0 - x1 + x2 - x3;
    const double sy = y0 - y1 + y2 - y3;
    double* h = out.h;
    if (std::fabs(sx) < 1e-12 && std::fabs(sy) < 1e-12) {
        // 仿射
        h[0] = x1 - x0;
        h[1] = x2 - x1;
        h[2] = x0;
        h[3] = y1 - y0;
        h[4] = y2 - y1;
        h[5] = y0;
        h[6] = 0.0;
        h[7] = 0.0;
        h[8] = 1.0;
        return true;
    }
    const double den = dx1 * dy2 - dx2 * dy1;
    if (std::fabs(den) < 1e-12) {
        return false;
    }
    const double g = (sx * dy2 - sy * dx2) / den;
    const double hh = (dx1 * sy - dy1 * sx) / den;
    // Heckbert：a=x1(1+g)−x0，b=x3(1+h)−x0（(1,1) 约束定 g/h 后自洽）
    h[0] = x1 - x0 + g * x1;
    h[1] = x3 - x0 + hh * x3;
    h[2] = x0;
    h[3] = y1 - y0 + g * y1;
    h[4] = y3 - y0 + hh * y3;
    h[5] = y0;
    h[6] = g;
    h[7] = hh;
    h[8] = 1.0;
    return true;
}

Plane identityPlane(const std::array<Pt, 4>& q) {
    Plane p;
    p.src = q;
    p.dst = q;
    return p;
}

std::vector<std::vector<std::pair<int, int>>> linkedCorners(const std::vector<Plane>& planes) {
    std::vector<std::vector<std::pair<int, int>>> groups;
    for (size_t pi = 0; pi < planes.size(); ++pi) {
        for (size_t ci = 0; ci < 4; ++ci) {
            const Pt& c = planes[pi].src[ci];
            size_t found = groups.size();
            for (size_t gi = 0; gi < groups.size(); ++gi) {
                const auto [a, b] = groups[gi][0];
                const Pt& o = planes[a].src[b];
                if (std::fabs(o[0] - c[0]) <= kLinkEps && std::fabs(o[1] - c[1]) <= kLinkEps) {
                    found = gi;
                    break;
                }
            }
            if (found < groups.size()) {
                groups[found].emplace_back(static_cast<int>(pi), static_cast<int>(ci));
            } else {
                groups.push_back({{static_cast<int>(pi), static_cast<int>(ci)}});
            }
        }
    }
    return groups;
}

void unify(std::vector<Plane>& planes) {
    for (const auto& g : linkedCorners(planes)) {
        if (g.size() < 2) {
            continue;
        }
        const double n = static_cast<double>(g.size());
        Pt s = {0.0, 0.0};
        Pt d = {0.0, 0.0};
        for (const auto& [p, c] : g) {
            s[0] += planes[p].src[c][0] / n;
            s[1] += planes[p].src[c][1] / n;
            d[0] += planes[p].dst[c][0] / n;
            d[1] += planes[p].dst[c][1] / n;
        }
        for (const auto& [p, c] : g) {
            planes[p].src[c] = s;
            planes[p].dst[c] = d;
        }
    }
}

namespace {
// 平面边 k：角 k → 角 k+1。返回（边上点平面坐标，该边修正权重）
std::pair<Pt, double> edgeUv(int k, double u, double v) {
    switch (k) {
        case 0:
            return {{u, 0.0}, 1.0 - v};
        case 1:
            return {{1.0, v}, u};
        case 2:
            return {{u, 1.0}, v};
        default:
            return {{0.0, v}, 1.0 - u};
    }
}

double distToQuad(const std::array<Pt, 4>& q, const Pt& p) {
    double best = 1e308;
    for (int k = 0; k < 4; ++k) {
        const Pt& a = q[k];
        const Pt& b = q[(k + 1) % 4];
        const double dx = b[0] - a[0];
        const double dy = b[1] - a[1];
        const double l2 = dx * dx + dy * dy;
        double t = 0.0;
        if (l2 > 0.0) {
            t = std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2, 0.0, 1.0);
        }
        const double ex = a[0] + t * dx - p[0];
        const double ey = a[1] + t * dy - p[1];
        best = std::min(best, std::sqrt(ex * ex + ey * ey));
    }
    return best;
}
}  // namespace

void straighten(std::vector<Plane>& planes, Straighten mode) {
    unify(planes);
    const auto groups = linkedCorners(planes);
    auto groupOf = [&](int p, int c) -> int {
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            for (const auto& [pp, cc] : groups[gi]) {
                if (pp == p && cc == c) {
                    return static_cast<int>(gi);
                }
            }
        }
        return 0;
    };
    std::vector<Pt> pos;
    pos.reserve(groups.size());
    for (const auto& g : groups) {
        pos.push_back(planes[g[0].first].dst[g[0].second]);
    }
    std::vector<std::pair<int, int>> edges;
    for (size_t p = 0; p < planes.size(); ++p) {
        for (int k = 0; k < 4; ++k) {
            edges.emplace_back(groupOf(static_cast<int>(p), k),
                               groupOf(static_cast<int>(p), (k + 1) % 4));
        }
    }
    for (const auto& [a, b] : edges) {
        const Pt& pa = pos[a];
        const Pt& pb = pos[b];
        const double dx = std::fabs(pb[0] - pa[0]);
        const double dy = std::fabs(pb[1] - pa[1]);
        if (dy <= dx && (mode == Straighten::Horizontal || mode == Straighten::Auto)) {
            const double y = (pa[1] + pb[1]) / 2.0;
            pos[a][1] = y;
            pos[b][1] = y;
        } else if (dx < dy && (mode == Straighten::Vertical || mode == Straighten::Auto)) {
            const double x = (pa[0] + pb[0]) / 2.0;
            pos[a][0] = x;
            pos[b][0] = x;
        }
    }
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        for (const auto& [pi, ci] : groups[gi]) {
            planes[pi].dst[ci] = pos[gi];
        }
    }
}

bool PerspectiveMap::build(const std::vector<Plane>& planesIn) {
    planes_.clear();
    if (planesIn.empty()) {
        return false;
    }
    std::vector<Plane> ps = planesIn;
    unify(ps);
    auto near = [](const Pt& a, const Pt& b) {
        return std::fabs(a[0] - b[0]) <= kLinkEps && std::fabs(a[1] - b[1]) <= kLinkEps;
    };
    const double unitRect[4] = {0.0, 0.0, 1.0, 1.0};
    for (size_t i = 0; i < ps.size(); ++i) {
        const Plane& p = ps[i];
        PlaneMap pm;
        if (!rectToQuad(unitRect, p.src, pm.src)) {
            return false;
        }
        if (!pm.src.invert(pm.srcInv)) {
            return false;
        }
        if (!rectToQuad(unitRect, p.dst, pm.dst)) {
            return false;
        }
        pm.quad = p.src;
        for (int k = 0; k < 4; ++k) {
            pm.shared[k] = -1;
            const Pt& a = p.src[k];
            const Pt& b = p.src[(k + 1) % 4];
            for (size_t j = 0; j < ps.size() && pm.shared[k] < 0; ++j) {
                if (j == i) {
                    continue;
                }
                for (int m = 0; m < 4; ++m) {
                    const Pt& c = ps[j].src[m];
                    const Pt& d = ps[j].src[(m + 1) % 4];
                    if ((near(a, c) && near(b, d)) || (near(a, d) && near(b, c))) {
                        pm.shared[k] = static_cast<int>(j);
                        break;
                    }
                }
            }
        }
        planes_.push_back(pm);
    }
    return true;
}

void PerspectiveMap::locate(double x, double y, int& idx, double& u, double& v) const {
    int bi = 0;
    double bd = 1e308;
    double bu = 0.0, bv = 0.0;
    for (size_t i = 0; i < planes_.size(); ++i) {
        const Pt p = planes_[i].srcInv.apply(x, y);
        if (p[0] >= -1e-9 && p[0] <= 1.0 + 1e-9 && p[1] >= -1e-9 && p[1] <= 1.0 + 1e-9) {
            idx = static_cast<int>(i);
            u = p[0];
            v = p[1];
            return;
        }
        const double d = distToQuad(planes_[i].quad, {x, y});
        if (d < bd) {
            bd = d;
            bi = static_cast<int>(i);
            bu = p[0];
            bv = p[1];
        }
    }
    idx = bi;
    u = bu;
    v = bv;
}

Pt PerspectiveMap::map(double x, double y) const {
    if (planes_.empty()) {
        return {x, y};
    }
    int i = 0;
    double u = 0.0, v = 0.0;
    locate(x, y, i, u, v);
    const PlaneMap& pm = planes_[i];
    Pt o = pm.dst.apply(u, v);
    if (!std::isfinite(o[0]) || !std::isfinite(o[1])) {
        return {x, y};
    }
    const double cu = std::clamp(u, 0.0, 1.0);
    const double cv = std::clamp(v, 0.0, 1.0);
    for (int k = 0; k < 4; ++k) {
        const int j = pm.shared[k];
        if (j < 0) {
            continue;
        }
        const auto [e, w] = edgeUv(k, cu, cv);
        if (w <= 0.0) {
            continue;
        }
        const Pt mine = pm.dst.apply(e[0], e[1]);
        const Pt q = pm.src.apply(e[0], e[1]);
        const PlaneMap& other = planes_[j];
        const Pt ou = other.srcInv.apply(q[0], q[1]);
        const Pt theirs = other.dst.apply(ou[0], ou[1]);
        o[0] += w * ((mine[0] + theirs[0]) / 2.0 - mine[0]);
        o[1] += w * ((mine[1] + theirs[1]) / 2.0 - mine[1]);
    }
    return o;
}

bool PerspectiveMap::isIdentity(const std::vector<Plane>& planes) {
    for (const Plane& p : planes) {
        for (int k = 0; k < 4; ++k) {
            if (std::fabs(p.src[k][0] - p.dst[k][0]) >= 1e-9 ||
                std::fabs(p.src[k][1] - p.dst[k][1]) >= 1e-9) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace perspective

}  // namespace montage
