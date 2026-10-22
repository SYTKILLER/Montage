// M20c 操控变形内核 —— photocraft crates/algo/src/puppet.rs 全量 C++ 翻译（MIT OR Apache-2.0）。
// 覆盖 SDF（Felzenszwalb-Huttenlocher 可分离 EDT）+ marching squares 轮廓 +
// Douglas–Peucker 简化 + Bowyer–Watson 增量 Delaunay（可见性行走定位/stamp 空腔）+
// ARAP 交替优化（带状 Cholesky 主路径，RCM 序压带宽；超带回退 Jacobi-PCG）。

#include "engine/puppet_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace montage {

namespace {
constexpr int kNone = -1;
constexpr double kFar = 1e20;

double dist2p(const std::array<double, 2>& a, const std::array<double, 2>& b) {
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    return dx * dx + dy * dy;
}

double orient2(const std::array<double, 2>& a, const std::array<double, 2>& b,
               const std::array<double, 2>& c) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

std::array<double, 2> lerp2(const std::array<double, 2>& a, const std::array<double, 2>& b, double t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
}

// p 到线段 a–b 距离
double segDist(const std::array<double, 2>& p, const std::array<double, 2>& a,
               const std::array<double, 2>& b) {
    const double dx = b[0] - a[0];
    const double dy = b[1] - a[1];
    const double l2 = dx * dx + dy * dy;
    double t = 0.0;
    if (l2 > 0.0) {
        t = std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2, 0.0, 1.0);
    }
    const std::array<double, 2> q = {a[0] + dx * t, a[1] + dy * t};
    return std::sqrt(dist2p(p, q));
}

// ---- 可分离平方距离变换（Felzenszwalb & Huttenlocher）----
void edt1d(const std::vector<double>& f, std::vector<double>& out, std::vector<int>& v,
           std::vector<double>& z) {
    const int n = static_cast<int>(f.size());
    if (n == 0) {
        return;
    }
    int k = 0;
    v[0] = 0;
    z[0] = -kFar;
    z[1] = kFar;
    auto meet = [&](int q, int p) -> double {
        return ((f[q] + static_cast<double>(q) * q) - (f[p] + static_cast<double>(p) * p)) /
               (2.0 * (static_cast<double>(q) - static_cast<double>(p)));
    };
    for (int q = 1; q < n; ++q) {
        double s = meet(q, v[k]);
        while (k > 0 && s <= z[k]) {
            --k;
            s = meet(q, v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kFar;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < static_cast<double>(q)) {
            ++k;
        }
        const int p = v[k];
        out[q] = static_cast<double>(q - p) * (q - p) + f[p];
    }
}

// 每块到最近 `site` 块的平方距离（块单位）
std::vector<double> edt(const std::vector<bool>& mask, bool site, int w, int h) {
    std::vector<double> d(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < d.size(); ++i) {
        d[i] = mask[i] == site ? 0.0 : kFar;
    }
    const int m = std::max(w, h);
    std::vector<double> f(m, 0.0);
    std::vector<double> out(m, 0.0);
    std::vector<int> v(m, 0);
    std::vector<double> z(m + 1, 0.0);
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
            f[y] = d[static_cast<size_t>(y) * w + x];
        }
        edt1d(f, out, v, z);
        for (int y = 0; y < h; ++y) {
            d[static_cast<size_t>(y) * w + x] = out[y];
        }
    }
    for (int y = 0; y < h; ++y) {
        const double* row = &d[static_cast<size_t>(y) * w];
        edt1d(std::vector<double>(row, row + w), out, v, z);
        std::memcpy(&d[static_cast<size_t>(y) * w], out.data(), static_cast<size_t>(w) * sizeof(double));
    }
    return d;
}

// 闭环 Douglas–Peucker 简化（容差 tol px，保角）
std::vector<std::array<double, 2>> simplify(const std::vector<std::array<double, 2>>& lp, double tol) {
    const int n = static_cast<int>(lp.size());
    if (n < 4) {
        return lp;
    }
    int far = n / 2;
    double bestd = -1.0;
    for (int i = 1; i < n; ++i) {
        const double d = dist2p(lp[0], lp[i]);
        if (d > bestd) {
            bestd = d;
            far = i;
        }
    }
    std::vector<bool> keep(n, false);
    keep[0] = true;
    keep[far] = true;
    std::vector<std::pair<int, int>> stack = {{0, far}, {far, n}};
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        const std::array<double, 2>& pa = lp[a];
        const std::array<double, 2>& pb = lp[b % n];
        double bd = 0.0;
        int bi = 0;
        for (int i = a + 1; i < b; ++i) {
            const double d = segDist(lp[i], pa, pb);
            if (d > bd) {
                bd = d;
                bi = i;
            }
        }
        if (bd > tol) {
            keep[bi] = true;
            stack.emplace_back(a, bi);
            stack.emplace_back(bi, b);
        }
    }
    std::vector<std::array<double, 2>> out;
    for (int i = 0; i < n; ++i) {
        if (keep[i]) {
            out.push_back(lp[i]);
        }
    }
    if (out.size() >= 3) {
        return out;
    }
    // 比容差还小的斑点：保留部分点
    out.clear();
    for (int i = 0; i < n; i += std::max(1, n / 4)) {
        out.push_back(lp[i]);
    }
    return out;
}

// 闭环边长超过 step 的等分加密
std::vector<std::array<double, 2>> densify(const std::vector<std::array<double, 2>>& lp, double step) {
    std::vector<std::array<double, 2>> out;
    out.reserve(lp.size());
    for (size_t k = 0; k < lp.size(); ++k) {
        const auto& a = lp[k];
        const auto& b = lp[(k + 1) % lp.size()];
        const int parts = static_cast<int>(std::clamp(std::ceil(std::sqrt(dist2p(a, b)) / step), 1.0, 1e4));
        for (int i = 0; i < parts; ++i) {
            out.push_back(lerp2(a, b, static_cast<double>(i) / parts));
        }
    }
    return out;
}

// d 在正定向三角形 a,b,c 外接圆内时为正
double inCircle(const std::array<double, 2>& a, const std::array<double, 2>& b,
                const std::array<double, 2>& c, const std::array<double, 2>& d) {
    const double ax = a[0] - d[0], ay = a[1] - d[1];
    const double bx = b[0] - d[0], by = b[1] - d[1];
    const double cx = c[0] - d[0], cy = c[1] - d[1];
    return (ax * ax + ay * ay) * (bx * cy - cx * by) - (bx * bx + by * by) * (ax * cy - cx * ay) +
           (cx * cx + cy * cy) * (ax * by - bx * ay);
}

// Bowyer–Watson 增量 Delaunay（photocraft Delaunay 直迁）
struct DTri {
    int v[3];
    int n[3];
    bool alive = true;
};

struct Delaunay {
    std::vector<std::array<double, 2>> pts;
    std::vector<DTri> tris;
    std::vector<uint32_t> stamp;
    uint32_t pass = 0;
    int last = 0;

    Delaunay(const std::array<double, 2>& lo, const std::array<double, 2>& hi) {
        const double cx = 0.5 * (lo[0] + hi[0]);
        const double cy = 0.5 * (lo[1] + hi[1]);
        const double d = std::max(std::max(hi[0] - lo[0], hi[1] - lo[1]), 1.0) * 4.0;
        pts = {{cx - 2.0 * d, cy - d}, {cx + 2.0 * d, cy - d}, {cx, cy + 2.0 * d}};
        if (orient2(pts[0], pts[1], pts[2]) < 0.0) {
            std::swap(pts[1], pts[2]);
        }
        DTri t;
        t.v[0] = 0; t.v[1] = 1; t.v[2] = 2;
        t.n[0] = kNone; t.n[1] = kNone; t.n[2] = kNone;
        t.alive = true;
        tris.push_back(t);
        stamp.push_back(0);
        last = 0;
    }

    // 持有 p 的三角形（可见性行走）
    int locate(const std::array<double, 2>& p) const {
        int t = -1;
        if (last >= 0 && last < static_cast<int>(tris.size()) && tris[last].alive) {
            t = last;
        } else {
            for (int i = 0; i < static_cast<int>(tris.size()); ++i) {
                if (tris[i].alive) {
                    t = i;
                    break;
                }
            }
            if (t < 0) {
                return kNone;
            }
        }
        for (int guard = 0; guard < static_cast<int>(tris.size()) + 3; ++guard) {
            const DTri& tri = tris[t];
            int step = kNone;
            for (int k = 0; k < 3; ++k) {
                if (orient2(pts[tri.v[(k + 1) % 3]], pts[tri.v[(k + 2) % 3]], p) < 0.0) {
                    step = k;
                    break;
                }
            }
            if (step == kNone) {
                return t;
            }
            if (tri.n[step] != kNone) {
                t = tri.n[step];
            } else {
                return kNone;
            }
        }
        for (int i = 0; i < static_cast<int>(tris.size()); ++i) {
            const DTri& tri = tris[i];
            if (!tri.alive) {
                continue;
            }
            bool all = true;
            for (int k = 0; k < 3; ++k) {
                if (orient2(pts[tri.v[(k + 1) % 3]], pts[tri.v[(k + 2) % 3]], p) < 0.0) {
                    all = false;
                    break;
                }
            }
            if (all) {
                return i;
            }
        }
        return kNone;
    }

    // 插入 p；返回顶点序号（与既有顶点重合时返回该顶点）
    int insert(const std::array<double, 2>& p) {
        const int t0 = locate(p);
        if (t0 == kNone) {
            return kNone;
        }
        for (int vv : tris[t0].v) {
            if (dist2p(pts[vv], p) < 1e-12) {
                return vv;
            }
        }
        pass += 1;
        const uint32_t passv = pass;
        std::vector<int> bad = {t0};
        stamp[t0] = passv;
        size_t i = 0;
        while (i < bad.size()) {
            const int t = bad[i++];
            for (int k = 0; k < 3; ++k) {
                const int nb = tris[t].n[k];
                if (nb == kNone || stamp[nb] == passv) {
                    continue;
                }
                const std::array<double, 2> a = pts[tris[nb].v[0]];
                const std::array<double, 2> b = pts[tris[nb].v[1]];
                const std::array<double, 2> c = pts[tris[nb].v[2]];
                if (inCircle(a, b, c, p) > 0.0) {
                    stamp[nb] = passv;
                    bad.push_back(nb);
                }
            }
        }
        std::vector<std::tuple<int, int, int>> rim;  // (a, b, 邻接外三角)
        for (const int t : bad) {
            const DTri tri = tris[t];
            for (int k = 0; k < 3; ++k) {
                const int nb = tri.n[k];
                if (nb == kNone || stamp[nb] != passv) {
                    rim.emplace_back(tri.v[(k + 1) % 3], tri.v[(k + 2) % 3], nb);
                }
            }
            tris[t].alive = false;
        }
        const int pi = static_cast<int>(pts.size());
        pts.push_back(p);
        const int base = static_cast<int>(tris.size());
        for (size_t j = 0; j < rim.size(); ++j) {
            const auto [a, b, o] = rim[j];
            DTri nt;
            nt.v[0] = a; nt.v[1] = b; nt.v[2] = pi;
            nt.n[0] = kNone; nt.n[1] = kNone; nt.n[2] = o;
            nt.alive = true;
            tris.push_back(nt);
            stamp.push_back(0);
            if (o >= 0 && o < static_cast<int>(tris.size())) {
                DTri& ot = tris[o];
                for (int k = 0; k < 3; ++k) {
                    const int x = ot.v[(k + 1) % 3];
                    const int y = ot.v[(k + 2) % 3];
                    if ((x == b && y == a) || (x == a && y == b)) {
                        ot.n[k] = base + static_cast<int>(j);
                    }
                }
            }
        }
        for (size_t j = 0; j < rim.size(); ++j) {
            const auto [a, b, o] = rim[j];
            int acrossB = kNone;
            int acrossA = kNone;
            for (size_t q = 0; q < rim.size(); ++q) {
                if (std::get<0>(rim[q]) == b) {
                    acrossB = base + static_cast<int>(q);
                }
                if (std::get<1>(rim[q]) == a) {
                    acrossA = base + static_cast<int>(q);
                }
            }
            tris[base + static_cast<int>(j)].n[0] = acrossB;
            tris[base + static_cast<int>(j)].n[1] = acrossA;
        }
        last = base;
        return pi;
    }

    std::vector<std::pair<int, int>> edges() const {
        std::vector<std::pair<int, int>> e;
        for (const DTri& t : tris) {
            if (!t.alive) {
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                const int a = t.v[k];
                const int b = t.v[(k + 1) % 3];
                e.emplace_back(std::min(a, b), std::max(a, b));
            }
        }
        std::sort(e.begin(), e.end());
        e.erase(std::unique(e.begin(), e.end()), e.end());
        return e;
    }
};

// 覆盖区符号距离场（块网格；负=内）
struct Sdf {
    double x0 = 0.0;
    double y0 = 0.0;
    double g = 1.0;
    int bw = 0;
    int bh = 0;
    std::vector<float> sd;
    double expansion = 0.0;

    Sdf(const puppet::Img& src, const RectI& bounds, double expansionIn) {
        const double lng = static_cast<double>(std::max(bounds.width(), bounds.height()));
        const double grow = std::ceil(std::max(expansionIn, 0.0));
        constexpr double kFieldBlocks = 1024.0;
        g = std::max(1.0, std::ceil((lng + 2.0 * grow + 8.0) / kFieldBlocks));
        RectI area;
        area.x0 = bounds.x0 - static_cast<int>(grow) - 2 * static_cast<int>(g);
        area.y0 = bounds.y0 - static_cast<int>(grow) - 2 * static_cast<int>(g);
        area.x1 = bounds.x1 + static_cast<int>(grow) + 2 * static_cast<int>(g);
        area.y1 = bounds.y1 + static_cast<int>(grow) + 2 * static_cast<int>(g);
        bw = std::max(1, (area.width() + static_cast<int>(g) - 1) / static_cast<int>(g));
        bh = std::max(1, (area.height() + static_cast<int>(g) - 1) / static_cast<int>(g));
        std::vector<bool> cov(static_cast<size_t>(bw) * bh, false);
        // 逐块扫 alpha
        for (int by = 0; by < bh; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                const int x0b = area.x0 + bx * static_cast<int>(g);
                const int y0b = area.y0 + by * static_cast<int>(g);
                const int x1b = std::min(x0b + static_cast<int>(g), bounds.x1);
                const int y1b = std::min(y0b + static_cast<int>(g), bounds.y1);
                if (x0b >= bounds.x1 || y0b >= bounds.y1 || x1b <= x0b || y1b <= y0b) {
                    continue;
                }
                bool any = false;
                for (int y = std::max(y0b, bounds.y0); y < y1b && !any; ++y) {
                    const float* row = &src.px[(static_cast<size_t>(y) * src.w) * 4u];
                    for (int x = std::max(x0b, bounds.x0); x < x1b; ++x) {
                        if (row[static_cast<size_t>(x) * 4u + 3u] > 0.05f) {
                            any = true;
                            break;
                        }
                    }
                }
                cov[static_cast<size_t>(by) * bw + bx] = any;
            }
        }
        // photocraft：outside=edt(cov,true)（未覆盖块到最近覆盖块），inside=edt(cov,false)
        const std::vector<double> outside = edt(cov, true, bw, bh);
        const std::vector<double> inside = edt(cov, false, bw, bh);
        sd.resize(cov.size());
        for (size_t i = 0; i < cov.size(); ++i) {
            sd[i] = static_cast<float>(
                cov[i] ? -(std::sqrt(inside[i]) - 0.5) * g : (std::sqrt(outside[i]) - 0.5) * g);
        }
        x0 = static_cast<double>(area.x0);
        y0 = static_cast<double>(area.y0);
        expansion = expansionIn;
    }

    // 块中心处场值（精确）
    double at(int bx, int by) const {
        if (bx < 0 || by < 0 || bx >= bw || by >= bh) {
            return kFar;
        }
        return static_cast<double>(sd[static_cast<size_t>(by) * bw + bx]) - expansion;
    }

    // 任意点场值（块中心双线性；钳到网格）
    double value(const std::array<double, 2>& p) const {
        const double fx = std::clamp((p[0] - x0) / g - 0.5, 0.0, static_cast<double>(bw - 1));
        const double fy = std::clamp((p[1] - y0) / g - 0.5, 0.0, static_cast<double>(bh - 1));
        const int ix = static_cast<int>(fx);
        const int iy = static_cast<int>(fy);
        const int ix1 = std::min(ix + 1, bw - 1);
        const int iy1 = std::min(iy + 1, bh - 1);
        const double tx = fx - ix;
        const double ty = fy - iy;
        const double top = at(ix, iy) * (1.0 - tx) + at(ix1, iy) * tx;
        const double bottom = at(ix, iy1) * (1.0 - tx) + at(ix1, iy1) * tx;
        return top * (1.0 - ty) + bottom * ty;
    }

    // 场零交线闭环（marching squares，块中心网格）
    std::vector<std::vector<std::array<double, 2>>> outline() const {
        std::vector<std::vector<std::array<double, 2>>> loops;
        if (bw < 2 || bh < 2) {
            return loops;
        }
        auto insideF = [&](int x, int y) -> bool { return at(x, y) < 0.0; };
        auto idF = [&](int x, int y, bool down) -> int { return 2 * (y * bw + x) + (down ? 1 : 0); };
        auto pointF = [&](int idv) -> std::array<double, 2> {
            const int b = idv / 2;
            const int x = b % bw;
            const int y = b / bw;
            const bool right = idv % 2 == 0;
            const int x2 = right ? x + 1 : x;
            const int y2 = right ? y : y + 1;
            const double fa = at(x, y);
            const double fb = at(x2, y2);
            const double t = std::fabs(fa - fb) > 1e-12 ? std::clamp(fa / (fa - fb), 0.0, 1.0) : 0.5;
            const double dx = static_cast<double>(x2 - x);
            const double dy = static_cast<double>(y2 - y);
            return {x0 + (static_cast<double>(x) + 0.5 + dx * t) * g,
                    y0 + (static_cast<double>(y) + 0.5 + dy * t) * g};
        };
        std::unordered_map<int, std::pair<int, int>> links;
        auto link = [&](int a, int b) {
            for (const int p : {a, b}) {
                auto it = links.find(p);
                if (it == links.end()) {
                    links[p] = {kNone, kNone};
                    it = links.find(p);
                }
                if (it->second.first == kNone) {
                    it->second.first = (p == a ? b : a);
                } else {
                    it->second.second = (p == a ? b : a);
                }
            }
        };
        for (int y = 0; y < bh - 1; ++y) {
            for (int x = 0; x < bw - 1; ++x) {
                const bool c[4] = {insideF(x, y), insideF(x + 1, y), insideF(x + 1, y + 1),
                                   insideF(x, y + 1)};
                if (c[0] == c[1] && c[1] == c[2] && c[2] == c[3]) {
                    continue;
                }
                const int edges[4] = {idF(x, y, false), idF(x + 1, y, true), idF(x, y + 1, false),
                                      idF(x, y, true)};
                const bool cross[4] = {c[0] != c[1], c[1] != c[2], c[3] != c[2], c[0] != c[3]};
                std::vector<int> ks;
                for (int k = 0; k < 4; ++k) {
                    if (cross[k]) {
                        ks.push_back(k);
                    }
                }
                if (ks.size() == 2) {
                    link(edges[ks[0]], edges[ks[1]]);
                } else if (ks.size() == 4) {
                    // 鞍点：中心值决定对角连接
                    const bool centre = at(x, y) + at(x + 1, y) + at(x + 1, y + 1) + at(x, y + 1) < 0.0;
                    if (c[0] == centre) {
                        link(edges[0], edges[1]);
                        link(edges[2], edges[3]);
                    } else {
                        link(edges[3], edges[0]);
                        link(edges[1], edges[2]);
                    }
                }
            }
        }
        std::vector<int> keys;
        keys.reserve(links.size());
        for (const auto& kv : links) {
            keys.push_back(kv.first);
        }
        std::sort(keys.begin(), keys.end());
        std::unordered_set<int> seen;
        for (const int start : keys) {
            if (seen.count(start)) {
                continue;
            }
            std::vector<std::array<double, 2>> lp;
            int prev = kNone;
            int cur = start;
            while (true) {
                seen.insert(cur);
                lp.push_back(pointF(cur));
                auto it = links.find(cur);
                if (it == links.end()) {
                    break;
                }
                int next = it->second.first != prev ? it->second.first : it->second.second;
                if (next == kNone || seen.count(next)) {
                    break;
                }
                prev = cur;
                cur = next;
            }
            if (lp.size() >= 3) {
                loops.push_back(std::move(lp));
            }
        }
        return loops;
    }
};

// RCM 顶点重编号（解矩阵窄带）
std::vector<int> bandOrder(const std::vector<std::array<double, 2>>& pos,
                           const std::vector<std::array<int, 3>>& tris) {
    std::vector<std::vector<int>> adj(pos.size());
    for (const auto& t : tris) {
        for (int k = 0; k < 3; ++k) {
            adj[t[k]].push_back(t[(k + 1) % 3]);
            adj[t[(k + 1) % 3]].push_back(t[k]);
        }
    }
    for (auto& a : adj) {
        std::sort(a.begin(), a.end());
        a.erase(std::unique(a.begin(), a.end()), a.end());
    }
    std::vector<int> starts;
    for (size_t i = 0; i < pos.size(); ++i) {
        if (!adj[i].empty()) {
            starts.push_back(static_cast<int>(i));
        }
    }
    std::sort(starts.begin(), starts.end(), [&](int a, int b) {
        if (pos[a][1] != pos[b][1]) {
            return pos[a][1] < pos[b][1];
        }
        return pos[a][0] < pos[b][0];
    });
    std::vector<bool> seen(pos.size(), false);
    std::vector<int> order;
    order.reserve(starts.size());
    for (const int s : starts) {
        if (seen[s]) {
            continue;
        }
        seen[s] = true;
        const size_t first = order.size();
        order.push_back(s);
        size_t i = first;
        while (i < order.size()) {
            const int vv = order[i++];
            std::vector<int> next;
            for (const int wv : adj[vv]) {
                if (!seen[wv]) {
                    next.push_back(wv);
                }
            }
            std::sort(next.begin(), next.end(),
                      [&](int a, int b) { return adj[a].size() < adj[b].size(); });
            for (const int wv : next) {
                seen[wv] = true;
                order.push_back(wv);
            }
        }
    }
    std::reverse(order.begin(), order.end());
    return order;
}

// 角 a 的余切
double cot(const std::array<double, 2>& a, const std::array<double, 2>& b,
           const std::array<double, 2>& c) {
    const double u[2] = {b[0] - a[0], b[1] - a[1]};
    const double v[2] = {c[0] - a[0], c[1] - a[1]};
    const double cross = std::fabs(u[0] * v[1] - u[1] * v[0]);
    if (cross < 1e-12) {
        return 0.0;
    }
    return (u[0] * v[0] + u[1] * v[1]) / cross;
}

}  // namespace

namespace puppet {

double densityCells(Density d) {
    switch (d) {
        case Density::Fewer:
            return 12.0;
        case Density::More:
            return 32.0;
        default:
            return 20.0;
    }
}

bool warpIsIdentity(const Warp& w) {
    for (const Pin& p : w.pins) {
        if (p.src[0] != p.dst[0] || p.src[1] != p.dst[1]) {
            return false;
        }
        if (p.hasRotate && std::fmod(p.rotateDeg, 360.0) != 0.0) {
            return false;
        }
    }
    return true;
}

Mesh buildMesh(const Img& src, const RectI& bounds, Density density, double expansion) {
    Mesh empty;
    empty.spacing = 0.0;
    if (bounds.empty()) {
        return empty;
    }
    const double e = std::isfinite(expansion) ? std::clamp(expansion, -200.0, 200.0) : 0.0;
    const double lng = static_cast<double>(std::max(bounds.width(), bounds.height())) + 2.0 * std::max(e, 0.0);
    const double spacing = std::max(lng / densityCells(density), 2.0);
    const Sdf field(src, bounds, e);
    const auto loops = field.outline();
    empty.spacing = spacing;
    if (loops.empty()) {
        return empty;
    }
    double tol = std::max(0.5 * field.g, 0.5);
    constexpr int kMaxOutlinePoints = 20000;
    std::vector<std::vector<std::array<double, 2>>> rings;
    while (true) {
        rings.clear();
        size_t total = 0;
        for (const auto& l : loops) {
            rings.push_back(densify(simplify(l, tol), spacing));
            total += rings.back().size();
        }
        if (total <= kMaxOutlinePoints || tol > spacing) {
            break;
        }
        tol *= 2.0;
    }
    const std::array<double, 2> lo = {field.x0, field.y0};
    const std::array<double, 2> hi = {field.x0 + field.bw * field.g, field.y0 + field.bh * field.g};
    Delaunay dl(lo, hi);
    std::vector<std::pair<int, int>> segs;
    for (const auto& ring : rings) {
        std::vector<int> idx;
        idx.reserve(ring.size());
        for (const auto& p : ring) {
            const int v = dl.insert(p);
            if (v != kNone) {
                idx.push_back(v);
            }
        }
        for (size_t k = 0; k < idx.size(); ++k) {
            const int a = idx[k];
            const int b = idx[(k + 1) % idx.size()];
            if (a != b) {
                segs.emplace_back(a, b);
            }
        }
    }
    // 内部：三角格点，离轮廓半步以上
    const double rise = spacing * std::sqrt(3.0) / 2.0;
    const int rows = static_cast<int>(std::ceil((hi[1] - lo[1]) / rise));
    const int cols = static_cast<int>(std::ceil((hi[0] - lo[0]) / spacing));
    for (int j = 0; j <= rows; ++j) {
        for (int i = 0; i <= cols; ++i) {
            const std::array<double, 2> p = {
                lo[0] + (static_cast<double>(i) + 0.5 * static_cast<double>(j % 2)) * spacing,
                lo[1] + static_cast<double>(j) * rise};
            if (field.value(p) < -0.5 * spacing) {
                dl.insert(p);
            }
        }
    }
    // 轮廓边恢复：三角化缺的边在中点分裂
    int budget = static_cast<int>(segs.size()) * 2;
    for (int round = 0; round < 8; ++round) {
        const auto edges = dl.edges();
        auto hasEdge = [&](int a, int b) {
            return std::binary_search(edges.begin(), edges.end(),
                                      std::make_pair(std::min(a, b), std::max(a, b)));
        };
        std::vector<std::pair<int, int>> next;
        next.reserve(segs.size());
        bool split = false;
        for (const auto& [a, b] : segs) {
            if (budget == 0 || hasEdge(a, b)) {
                next.emplace_back(a, b);
                continue;
            }
            const int c = dl.insert(lerp2(dl.pts[a], dl.pts[b], 0.5));
            if (c != kNone && c != a && c != b) {
                next.emplace_back(a, c);
                next.emplace_back(c, b);
                --budget;
                split = true;
            } else {
                next.emplace_back(a, b);
            }
        }
        segs = std::move(next);
        if (!split) {
            break;
        }
    }
    // 奇偶规则内测（分段按行分桶）
    struct Inside {
        const std::vector<std::array<double, 2>>* pts;
        const std::vector<std::pair<int, int>>* segs;
        double y0 = 0.0;
        double row = 1.0;
        std::vector<std::vector<int>> rows2;

        Inside(const std::vector<std::array<double, 2>>& pts_,
               const std::vector<std::pair<int, int>>& segs_, double row_)
            : pts(&pts_), segs(&segs_), row(row_) {
            double ymin = 1e308;
            double ymax = -1e308;
            for (const auto& [a, b] : segs_) {
                ymin = std::min(ymin, std::min(pts_[a][1], pts_[b][1]));
                ymax = std::max(ymax, std::max(pts_[a][1], pts_[b][1]));
            }
            y0 = ymin;
            const int n = ymax >= ymin
                              ? static_cast<int>(std::clamp(std::floor((ymax - ymin) / row), 0.0, 1e5)) + 1
                              : 0;
            rows2.assign(n, {});
            for (size_t i = 0; i < segs_.size(); ++i) {
                const double loY = std::min(pts_[segs_[i].first][1], pts_[segs_[i].second][1]);
                const double hiY = std::max(pts_[segs_[i].first][1], pts_[segs_[i].second][1]);
                const int r0 = static_cast<int>(std::floor((loY - y0) / row));
                const int r1 = static_cast<int>(std::floor((hiY - y0) / row));
                for (int r = std::max(0, r0); r <= r1 && r < n; ++r) {
                    rows2[r].push_back(static_cast<int>(i));
                }
            }
        }

        bool contains(const std::array<double, 2>& p) const {
            if (p[1] < y0) {
                return false;
            }
            const int r = static_cast<int>(std::floor((p[1] - y0) / row));
            if (r < 0 || r >= static_cast<int>(rows2.size())) {
                return false;
            }
            bool odd = false;
            for (const int i : rows2[r]) {
                const auto& a = (*pts)[(*segs)[i].first];
                const auto& b = (*pts)[(*segs)[i].second];
                if ((a[1] > p[1]) != (b[1] > p[1]) &&
                    p[0] < a[0] + (p[1] - a[1]) / (b[1] - a[1]) * (b[0] - a[0])) {
                    odd = !odd;
                }
            }
            return odd;
        }
    };
    const Inside insideTest(dl.pts, segs, spacing);
    std::vector<std::array<int, 3>> tris;
    for (const DTri& t : dl.tris) {
        if (!t.alive || t.v[0] < 3 || t.v[1] < 3 || t.v[2] < 3) {
            continue;
        }
        const auto& a = dl.pts[t.v[0]];
        const auto& b = dl.pts[t.v[1]];
        const auto& c = dl.pts[t.v[2]];
        if (orient2(a, b, c) > 1e-9 &&
            insideTest.contains({(a[0] + b[0] + c[0]) / 3.0, (a[1] + b[1] + c[1]) / 3.0})) {
            tris.push_back({t.v[0], t.v[1], t.v[2]});
        }
    }
    if (tris.empty()) {
        return empty;
    }
    // 保留在用顶点，RCM 重编号
    const std::vector<int> order = bandOrder(dl.pts, tris);
    std::vector<int> remap(dl.pts.size(), kNone);
    for (size_t i = 0; i < order.size(); ++i) {
        remap[order[i]] = static_cast<int>(i);
    }
    Mesh out;
    out.spacing = spacing;
    out.verts.reserve(order.size());
    for (const int i : order) {
        out.verts.push_back(dl.pts[i]);
    }
    out.tris.reserve(tris.size());
    for (const auto& t : tris) {
        out.tris.push_back({remap[t[0]], remap[t[1]], remap[t[2]]});
    }
    return out;
}

// ---- 稀疏矩阵与求解器 ----

void Solver::Csr::mul(const std::vector<double>& x, std::vector<double>& out) const {
    for (size_t i = 0; i < out.size(); ++i) {
        double s = 0.0;
        for (int k = rows[i]; k < rows[i + 1]; ++k) {
            s += vals[k] * x[cols[k]];
        }
        out[i] = s;
    }
}

// Jacobi 预条件共轭梯度（x 暖启动）
void Solver::Csr::solve(std::vector<double> b, std::vector<double>& x) const {
    const int n = static_cast<int>(b.size());
    std::vector<double> r(n, 0.0);
    mul(x, r);
    double bn = 0.0;
    for (const double v : b) {
        bn += v * v;
    }
    bn = std::max(std::sqrt(bn), 1e-30);
    std::vector<double> z(n);
    for (int i = 0; i < n; ++i) {
        r[i] = b[i] - r[i];
        z[i] = r[i] / diag[i];
    }
    std::vector<double> p = z;
    double rz = 0.0;
    for (int i = 0; i < n; ++i) {
        rz += r[i] * z[i];
    }
    std::vector<double> ap(n, 0.0);
    const int iters = std::clamp(4 * n, 50, 2000);
    for (int it = 0; it < iters; ++it) {
        double rr = 0.0;
        for (const double v : r) {
            rr += v * v;
        }
        if (std::sqrt(rr) <= 1e-12 * bn) {
            break;
        }
        mul(p, ap);
        double pap = 0.0;
        for (size_t i = 0; i < p.size(); ++i) {
            pap += p[i] * ap[i];
        }
        if (std::fabs(pap) < 1e-300) {
            break;
        }
        const double alpha = rz / pap;
        for (int i = 0; i < n; ++i) {
            x[i] += alpha * p[i];
            r[i] -= alpha * ap[i];
            z[i] = r[i] / diag[i];
        }
        double rz2 = 0.0;
        for (int i = 0; i < n; ++i) {
            rz2 += r[i] * z[i];
        }
        const double beta = rz2 / rz;
        rz = rz2;
        for (int i = 0; i < n; ++i) {
            p[i] = z[i] + beta * p[i];
        }
    }
}

void Solver::BandChol::solve(const std::vector<double>& b, std::vector<double>& x) const {
    const int w = p + 1;
    std::vector<double> y = b;
    for (int i = 0; i < n; ++i) {
        double s = y[i];
        for (int k = std::max(0, i - p); k < i; ++k) {
            s -= l[static_cast<size_t>(i) * w + (k + p - i)] * y[k];
        }
        y[i] = s / l[static_cast<size_t>(i) * w + p];
    }
    for (int i = n - 1; i >= 0; --i) {
        double s = y[i];
        for (int k = i + 1; k < std::min(i + p + 1, n); ++k) {
            s -= l[static_cast<size_t>(k) * w + (i + p - k)] * x[k];
        }
        x[i] = s / l[static_cast<size_t>(i) * w + p];
    }
}

// 钉绑定网格：三角顶点重心权重
Solver::Solver(const Mesh& meshIn, const std::vector<std::array<double, 2>>& pinSrc,
               const std::vector<bool>& fixedRotation) {
    mesh = meshIn;
    const int nv = static_cast<int>(mesh.verts.size());
    w_.reserve(mesh.tris.size());
    for (const auto& t : mesh.tris) {
        const auto& a = mesh.verts[t[0]];
        const auto& b = mesh.verts[t[1]];
        const auto& c = mesh.verts[t[2]];
        w_.push_back({0.5 * cot(a, b, c), 0.5 * cot(b, c, a), 0.5 * cot(c, a, b)});
    }
    if (nv == 0) {
        return;  // 无顶点：钉忽略（#951）
    }
    auto locateBound = [&](const std::array<double, 2>& p) -> Bound {
        for (const auto& t : mesh.tris) {
            const auto& a = mesh.verts[t[0]];
            const auto& b = mesh.verts[t[1]];
            const auto& c = mesh.verts[t[2]];
            const double det = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
            if (std::fabs(det) < 1e-12) {
                continue;
            }
            const double l1 = ((p[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (p[1] - a[1])) / det;
            const double l2 = ((b[0] - a[0]) * (p[1] - a[1]) - (p[0] - a[0]) * (b[1] - a[1])) / det;
            const double l0 = 1.0 - l1 - l2;
            if (l0 >= -1e-9 && l1 >= -1e-9 && l2 >= -1e-9) {
                return {{t[0], t[1], t[2]}, {l0, l1, l2}};
            }
        }
        // 网格外：最近顶点
        int bi = 0;
        double bd = 1e308;
        for (int i = 0; i < nv; ++i) {
            const double d = dist2p(mesh.verts[i], p);
            if (d < bd) {
                bd = d;
                bi = i;
            }
        }
        return {{bi, bi, bi}, {1.0, 0.0, 0.0}};
    };
    for (const auto& p : pinSrc) {
        bound_.push_back(locateBound(p));
    }
    // 连通分量（三角边并查集）
    std::vector<int> parent(nv);
    for (int i = 0; i < nv; ++i) {
        parent[i] = i;
    }
    std::function<int(int)> find = [&](int i) -> int {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    for (const auto& t : mesh.tris) {
        for (int k = 1; k < 3; ++k) {
            const int a = find(t[0]);
            const int b = find(t[k]);
            if (a != b) {
                parent[a] = b;
            }
        }
    }
    std::vector<bool> pinnedRoot(nv, false);
    for (const auto& b : bound_) {
        for (const int v : b.verts) {
            pinnedRoot[find(v)] = true;
        }
    }
    anchored_.resize(nv);
    for (int v = 0; v < nv; ++v) {
        anchored_[v] = !pinnedRoot[find(v)];
    }
    // 旋转固定钉：钉最近顶点周围的三角形
    pinTris_.resize(bound_.size());
    for (size_t k = 0; k < bound_.size() && k < pinSrc.size(); ++k) {
        if (k >= fixedRotation.size() || !fixedRotation[k]) {
            continue;
        }
        const auto& p = pinSrc[k];
        int bv = 0;
        double bd = 1e308;
        for (const int v : bound_[k].verts) {
            const double d = dist2p(mesh.verts[v], p);
            if (d < bd) {
                bd = d;
                bv = v;
            }
        }
        for (size_t t = 0; t < mesh.tris.size(); ++t) {
            if (mesh.tris[t][0] == bv || mesh.tris[t][1] == bv || mesh.tris[t][2] == bv) {
                pinTris_[k].push_back(static_cast<int>(t));
            }
        }
    }
    // 矩阵三元组
    std::vector<std::tuple<int, int, double>> trip;
    trip.reserve(mesh.tris.size() * 12 + nv);
    double diagSum = 0.0;
    for (size_t t = 0; t < mesh.tris.size(); ++t) {
        for (int k = 0; k < 3; ++k) {
            const int i = mesh.tris[t][(k + 1) % 3];
            const int j = mesh.tris[t][(k + 2) % 3];
            const double wk = w_[t][k];
            trip.emplace_back(i, i, wk);
            trip.emplace_back(j, j, wk);
            trip.emplace_back(i, j, -wk);
            trip.emplace_back(j, i, -wk);
            diagSum += 2.0 * wk;
        }
    }
    pinWeight_ = 1e4 * std::max(diagSum / std::max(nv, 1), 1.0);
    for (const auto& b : bound_) {
        for (int a = 0; a < 3; ++a) {
            for (int c = 0; c < 3; ++c) {
                trip.emplace_back(b.verts[a], b.verts[c], pinWeight_ * b.bary[a] * b.bary[c]);
            }
        }
    }
    for (int v = 0; v < nv; ++v) {
        if (anchored_[v]) {
            trip.emplace_back(v, v, pinWeight_);
        }
    }
    // CSR 组装（photocraft Csr::from_triplets）
    std::sort(trip.begin(), trip.end(), [](const auto& a, const auto& b) {
        if (std::get<0>(a) != std::get<0>(b)) {
            return std::get<0>(a) < std::get<0>(b);
        }
        return std::get<1>(a) < std::get<1>(b);
    });
    Csr& m = matrix_;
    m.rows.assign(nv + 1, 0);
    std::pair<int, int> last = {kNone, kNone};
    for (const auto& [i, j, v] : trip) {
        if (std::make_pair(i, j) == last) {
            if (!m.vals.empty()) {
                m.vals.back() += v;
            }
            continue;
        }
        last = {i, j};
        m.cols.push_back(j);
        m.vals.push_back(v);
        m.rows[i + 1] = static_cast<int>(m.cols.size());
    }
    for (int i = 0; i < nv; ++i) {
        m.rows[i + 1] = std::max(m.rows[i + 1], m.rows[i]);
    }
    m.diag.assign(nv, 1.0);
    for (int i = 0; i < nv; ++i) {
        for (int k = m.rows[i]; k < m.rows[i + 1]; ++k) {
            if (m.cols[k] == i && m.vals[k] > 0.0) {
                m.diag[i] = m.vals[k];
            }
        }
    }
    // 带状 Cholesky（带宽过大/非正定则留空走 PCG）
    BandChol& ch = chol_;
    ch.n = nv;
    int p = 0;
    for (int i = 0; i < nv; ++i) {
        for (int k = m.rows[i]; k < m.rows[i + 1]; ++k) {
            p = std::max(p, std::abs(i - m.cols[k]));
        }
    }
    if (static_cast<double>(nv) * (p + 1) > 40000000.0) {
        return;
    }
    const int w = p + 1;
    ch.p = p;
    ch.l.assign(static_cast<size_t>(nv) * w, 0.0);
    for (int i = 0; i < nv; ++i) {
        for (int k = m.rows[i]; k < m.rows[i + 1]; ++k) {
            const int j = m.cols[k];
            if (j <= i) {
                ch.l[static_cast<size_t>(i) * w + (j + p - i)] = m.vals[k];
            }
        }
    }
    for (int i = 0; i < nv; ++i) {
        const int lo = std::max(0, i - p);
        for (int j = lo; j <= i; ++j) {
            const int jlo = std::max(std::max(0, j - p), lo);
            double s = ch.l[static_cast<size_t>(i) * w + (j + p - i)];
            for (int k = jlo; k < j; ++k) {
                s -= ch.l[static_cast<size_t>(i) * w + (k + p - i)] *
                     ch.l[static_cast<size_t>(j) * w + (k + p - j)];
            }
            if (i == j) {
                if (s <= 0.0 || !std::isfinite(s)) {
                    ch.ok = false;
                    return;
                }
                ch.l[static_cast<size_t>(i) * w + p] = std::sqrt(s);
            } else {
                ch.l[static_cast<size_t>(i) * w + (j + p - i)] = s / ch.l[static_cast<size_t>(j) * w + p];
            }
        }
    }
    ch.ok = true;
}

double Solver::energyWith(const std::vector<std::array<double, 2>>& v,
                          const std::vector<std::array<double, 4>>& rots,
                          const std::vector<std::array<double, 2>>& pinDst) const {
    const auto& x = mesh.verts;
    double e = 0.0;
    for (size_t t = 0; t < mesh.tris.size(); ++t) {
        for (int k = 0; k < 3; ++k) {
            const int i = mesh.tris[t][(k + 1) % 3];
            const int j = mesh.tris[t][(k + 2) % 3];
            const double d[2] = {v[i][0] - v[j][0], v[i][1] - v[j][1]};
            const double ex[2] = {x[i][0] - x[j][0], x[i][1] - x[j][1]};
            const double re[2] = {rots[t][0] * ex[0] + rots[t][1] * ex[1],
                                  rots[t][2] * ex[0] + rots[t][3] * ex[1]};
            e += w_[t][k] * ((d[0] - re[0]) * (d[0] - re[0]) + (d[1] - re[1]) * (d[1] - re[1]));
        }
    }
    for (size_t b = 0; b < bound_.size() && b < pinDst.size(); ++b) {
        double p[2] = {0.0, 0.0};
        for (int k = 0; k < 3; ++k) {
            p[0] += bound_[b].bary[k] * v[bound_[b].verts[k]][0];
            p[1] += bound_[b].bary[k] * v[bound_[b].verts[k]][1];
        }
        e += pinWeight_ * dist2p({p[0], p[1]}, pinDst[b]);
    }
    return e;
}

std::vector<std::array<double, 4>> Solver::localStep(const std::vector<std::array<double, 2>>& v,
                                                     Mode mode,
                                                     const std::vector<Pin>& pins) const {
    const auto& x = mesh.verts;
    std::vector<std::array<double, 4>> rots(mesh.tris.size());
    for (size_t t = 0; t < mesh.tris.size(); ++t) {
        double s[4] = {0.0, 0.0, 0.0, 0.0};
        double norm = 0.0;
        for (int k = 0; k < 3; ++k) {
            const int i = mesh.tris[t][(k + 1) % 3];
            const int j = mesh.tris[t][(k + 2) % 3];
            const double d[2] = {v[i][0] - v[j][0], v[i][1] - v[j][1]};
            const double e[2] = {x[i][0] - x[j][0], x[i][1] - x[j][1]};
            s[0] += w_[t][k] * d[0] * e[0];
            s[1] += w_[t][k] * d[0] * e[1];
            s[2] += w_[t][k] * d[1] * e[0];
            s[3] += w_[t][k] * d[1] * e[1];
            norm += w_[t][k] * (e[0] * e[0] + e[1] * e[1]);
        }
        const double c = s[0] + s[3];
        const double sn = s[2] - s[1];
        const double th = std::atan2(sn, c);
        double scale = 1.0;
        if (mode == Mode::Normal) {
            scale = std::sqrt(std::hypot(c, sn) / std::max(norm, 1e-12));
        } else if (mode == Mode::Distort) {
            scale = std::hypot(c, sn) / std::max(norm, 1e-12);
        }
        if (!std::isfinite(scale) || scale <= 0.0) {
            scale = 1.0;
        }
        const double sth = std::sin(th);
        const double cth = std::cos(th);
        rots[t] = {scale * cth, -scale * sth, scale * sth, scale * cth};
    }
    // 旋转固定钉盖写周围三角形
    for (size_t k = 0; k < pinTris_.size() && k < pins.size(); ++k) {
        if (!pins[k].hasRotate) {
            continue;
        }
        const double rad = pins[k].rotateDeg * 3.14159265358979323846 / 180.0;
        const double s = std::sin(rad);
        const double c2 = std::cos(rad);
        for (const int t : pinTris_[k]) {
            rots[t] = {c2, -s, s, c2};
        }
    }
    return rots;
}

void Solver::globalStep(const std::vector<std::array<double, 4>>& rots,
                        const std::vector<std::array<double, 2>>& pinDst,
                        std::vector<std::array<double, 2>>& v) const {
    const int nv = static_cast<int>(v.size());
    const auto& x = mesh.verts;
    std::vector<double> bx(nv, 0.0);
    std::vector<double> by(nv, 0.0);
    for (size_t t = 0; t < mesh.tris.size(); ++t) {
        for (int k = 0; k < 3; ++k) {
            const int i = mesh.tris[t][(k + 1) % 3];
            const int j = mesh.tris[t][(k + 2) % 3];
            const double e[2] = {x[i][0] - x[j][0], x[i][1] - x[j][1]};
            const double re[2] = {rots[t][0] * e[0] + rots[t][1] * e[1],
                                  rots[t][2] * e[0] + rots[t][3] * e[1]};
            bx[i] += w_[t][k] * re[0];
            by[i] += w_[t][k] * re[1];
            bx[j] -= w_[t][k] * re[0];
            by[j] -= w_[t][k] * re[1];
        }
    }
    for (size_t b = 0; b < bound_.size() && b < pinDst.size(); ++b) {
        for (int k = 0; k < 3; ++k) {
            bx[bound_[b].verts[k]] += pinWeight_ * bound_[b].bary[k] * pinDst[b][0];
            by[bound_[b].verts[k]] += pinWeight_ * bound_[b].bary[k] * pinDst[b][1];
        }
    }
    for (int i = 0; i < nv; ++i) {
        if (anchored_[i]) {
            bx[i] += pinWeight_ * x[i][0];
            by[i] += pinWeight_ * x[i][1];
        }
    }
    std::vector<double> sx(nv);
    std::vector<double> sy(nv);
    for (int i = 0; i < nv; ++i) {
        sx[i] = v[i][0];
        sy[i] = v[i][1];
    }
    if (chol_.ok) {
        chol_.solve(bx, sx);
        chol_.solve(by, sy);
    } else {
        matrix_.solve(bx, sx);
        matrix_.solve(by, sy);
    }
    for (int i = 0; i < nv; ++i) {
        v[i] = {sx[i], sy[i]};
    }
}

void Solver::solve(std::vector<std::array<double, 2>>& outVerts, std::vector<double>& energies,
                   const std::vector<std::array<double, 2>>& pinDst, const std::vector<Pin>& pins,
                   Mode mode, int iterations,
                   const std::vector<std::array<double, 2>>* init) const {
    const int nv = static_cast<int>(mesh.verts.size());
    std::vector<std::array<double, 2>> v =
        (init != nullptr && static_cast<int>(init->size()) == nv) ? *init : mesh.verts;
    outVerts = v;
    if (nv == 0) {
        return;
    }
    const bool useInit = init != nullptr && static_cast<int>(init->size()) == nv;
    if (!useInit) {
        // R=I 起步；有旋转钉时每三角取反距加权角（纯 ARAP 传播旋转太慢）
        std::vector<std::pair<std::array<double, 2>, double>> rotated;
        for (size_t k = 0; k < bound_.size() && k < pins.size(); ++k) {
            if (!pins[k].hasRotate) {
                continue;
            }
            std::array<double, 2> p = {0.0, 0.0};
            for (int kk = 0; kk < 3; ++kk) {
                p[0] += bound_[k].bary[kk] * mesh.verts[bound_[k].verts[kk]][0];
                p[1] += bound_[k].bary[kk] * mesh.verts[bound_[k].verts[kk]][1];
            }
            rotated.emplace_back(p, pins[k].rotateDeg * 3.14159265358979323846 / 180.0);
        }
        std::vector<std::array<double, 4>> r(mesh.tris.size(), {1.0, 0.0, 0.0, 1.0});
        if (!rotated.empty()) {
            for (size_t t = 0; t < mesh.tris.size(); ++t) {
                std::array<double, 2> c = {0.0, 0.0};
                for (const int i : mesh.tris[t]) {
                    c[0] += mesh.verts[i][0] / 3.0;
                    c[1] += mesh.verts[i][1] / 3.0;
                }
                double sw = 0.0;
                double sa = 0.0;
                for (const auto& [p, a] : rotated) {
                    const double wv = 1.0 / (dist2p(p, c) + 1.0);
                    sw += wv;
                    sa += wv * a;
                }
                const double ang = sa / sw;
                r[t] = {std::cos(ang), -std::sin(ang), std::sin(ang), std::cos(ang)};
            }
        }
        globalStep(r, pinDst, v);
    }
    energies.clear();
    energies.reserve(iterations);
    for (int it = 0; it < iterations; ++it) {
        const auto rots = localStep(v, mode, pins);
        globalStep(rots, pinDst, v);
        const auto rigid = localStep(v, Mode::Rigid, pins);
        energies.push_back(energyWith(v, rigid, pinDst));
    }
    outVerts = v;
}

double Solver::energy(const std::vector<std::array<double, 2>>& v,
                      const std::vector<std::array<double, 2>>& pinDst,
                      const std::vector<Pin>& pins) const {
    return energyWith(v, localStep(v, Mode::Rigid, pins), pinDst);
}

std::vector<int> Solver::drawOrder(const std::vector<int>& depths,
                                   const std::vector<std::array<double, 2>>& pinSrc) const {
    std::vector<int> order(mesh.tris.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<int>(i);
    }
    bool any = false;
    for (const int d : depths) {
        if (d != 0) {
            any = true;
        }
    }
    if (!any || pinSrc.empty()) {
        return order;
    }
    auto depthOf = [&](int t) -> int {
        std::array<double, 2> c = {0.0, 0.0};
        for (const int i : mesh.tris[t]) {
            c[0] += mesh.verts[i][0] / 3.0;
            c[1] += mesh.verts[i][1] / 3.0;
        }
        int bk = 0;
        double bd = 1e308;
        for (size_t k = 0; k < pinSrc.size(); ++k) {
            const double d = dist2p(pinSrc[k], c);
            if (d < bd) {
                bd = d;
                bk = static_cast<int>(k);
            }
        }
        return bk < static_cast<int>(depths.size()) ? depths[bk] : 0;
    };
    std::vector<int> d(order.size());
    for (size_t i = 0; i < order.size(); ++i) {
        d[i] = depthOf(order[i]);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return d[a] < d[b]; });
    return order;
}

DeformResult deform(const Img& src, const RectI& bounds, const Warp& w, int iterations) {
    DeformResult res;
    const Mesh mesh = buildMesh(src, bounds, w.density, w.expansion);
    std::vector<std::array<double, 2>> pinSrc;
    std::vector<bool> fixed;
    std::vector<std::array<double, 2>> dst;
    std::vector<int> depths;
    for (const Pin& p : w.pins) {
        pinSrc.push_back({p.src[0], p.src[1]});
        dst.push_back({p.dst[0], p.dst[1]});
        fixed.push_back(p.hasRotate);
        depths.push_back(p.depth);
    }
    res.solver = Solver(mesh, pinSrc, fixed);
    std::vector<double> energies;
    res.solver.solve(res.verts, energies, dst, w.pins, w.mode, iterations, nullptr);
    if (res.solver.mesh.tris.empty()) {
        res.ok = true;  // 空网格=无需变形（原样保留）
        res.order.clear();
        return res;
    }
    res.order = res.solver.drawOrder(depths, pinSrc);
    double x0 = 1e308, y0 = 1e308, x1 = -1e308, y1 = -1e308;
    for (const auto& p : res.verts) {
        x0 = std::min(x0, p[0]);
        y0 = std::min(y0, p[1]);
        x1 = std::max(x1, p[0]);
        y1 = std::max(y1, p[1]);
    }
    const double area = (x1 - x0 + 1.0) * (y1 - y0 + 1.0);
    if (std::isnan(area) || area > kMaxWarpPixels) {
        res.ok = false;
        return res;
    }
    res.ok = true;
    return res;
}

bool deformedBounds(const std::vector<std::array<double, 2>>& verts, double out[4]) {
    double x0 = 1e308, y0 = 1e308, x1 = -1e308, y1 = -1e308;
    for (const auto& p : verts) {
        x0 = std::min(x0, p[0]);
        y0 = std::min(y0, p[1]);
        x1 = std::max(x1, p[0]);
        y1 = std::max(y1, p[1]);
    }
    if (x1 < x0 || y1 < y0) {
        return false;
    }
    out[0] = x0;
    out[1] = y0;
    out[2] = x1;
    out[3] = y1;
    return true;
}

Img renderPuppet(const Img& src, const RectI& bounds, const Mesh& mesh,
                 const std::vector<std::array<double, 2>>& deformed,
                 const std::vector<int>& order, double originX, double originY,
                 int outW, int outH) {
    Img out;
    out.w = outW;
    out.h = outH;
    out.px.assign(static_cast<size_t>(out.w) * out.h * 4u, 0.0f);
    const double x0 = originX;
    const double y0 = originY;
    // 预乘源
    Img pre = src;
    for (size_t i = 0; i < pre.px.size(); i += 4u) {
        const float a = pre.px[i + 3u];
        pre.px[i] *= a;
        pre.px[i + 1u] *= a;
        pre.px[i + 2u] *= a;
    }
    const auto sampleBilinear = [&](double sx, double sy, float outPx[4]) {
        sx = std::clamp(sx, 0.0, static_cast<double>(pre.w - 1));
        sy = std::clamp(sy, 0.0, static_cast<double>(pre.h - 1));
        const int ix = static_cast<int>(sx);
        const int iy = static_cast<int>(sy);
        const int ix1 = std::min(ix + 1, pre.w - 1);
        const int iy1 = std::min(iy + 1, pre.h - 1);
        const double tx = sx - ix;
        const double ty = sy - iy;
        for (int c = 0; c < 4; ++c) {
            const double a = pre.px[(static_cast<size_t>(iy) * pre.w + ix) * 4u + c];
            const double b = pre.px[(static_cast<size_t>(iy) * pre.w + ix1) * 4u + c];
            const double d = pre.px[(static_cast<size_t>(iy1) * pre.w + ix) * 4u + c];
            const double e = pre.px[(static_cast<size_t>(iy1) * pre.w + ix1) * 4u + c];
            outPx[c] = static_cast<float>((a * (1.0 - tx) + b * tx) * (1.0 - ty) +
                                          (d * (1.0 - tx) + e * tx) * ty);
        }
    };
    for (const int t : order) {
        const auto& tri = mesh.tris[t];
        // (dst, src) 顶点对
        const std::array<double, 2> p0 = deformed[tri[0]];
        const std::array<double, 2> p1 = deformed[tri[1]];
        const std::array<double, 2> p2 = deformed[tri[2]];
        const std::array<double, 2>& s0 = mesh.verts[tri[0]];
        const std::array<double, 2>& s1 = mesh.verts[tri[1]];
        const std::array<double, 2>& s2 = mesh.verts[tri[2]];
        const double det = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
        if (std::fabs(det) < 1e-12) {
            continue;
        }
        const int bx0 = std::max(0, static_cast<int>(std::floor(std::min({p0[0], p1[0], p2[0]}) - x0)));
        const int by0 = std::max(0, static_cast<int>(std::floor(std::min({p0[1], p1[1], p2[1]}) - y0)));
        const int bx1 = std::min(out.w - 1, static_cast<int>(std::ceil(std::max({p0[0], p1[0], p2[0]}) - x0)));
        const int by1 = std::min(out.h - 1, static_cast<int>(std::ceil(std::max({p0[1], p1[1], p2[1]}) - y0)));
        for (int py = by0; py <= by1; ++py) {
            for (int px2 = bx0; px2 <= bx1; ++px2) {
                // 像素中心在文档域
                const double cx = x0 + px2 + 0.5;
                const double cy = y0 + py + 0.5;
                const double l1 = ((cx - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (cy - p0[1])) / det;
                const double l2 = ((p1[0] - p0[0]) * (cy - p0[1]) - (cx - p0[0]) * (p1[1] - p0[1])) / det;
                const double l0 = 1.0 - l1 - l2;
                if (l0 < 0.0 || l1 < 0.0 || l2 < 0.0) {
                    continue;
                }
                const double sx = l0 * s0[0] + l1 * s1[0] + l2 * s2[0];
                const double sy = l0 * s0[1] + l1 * s1[1] + l2 * s2[1];
                float rgba[4];
                sampleBilinear(sx, sy, rgba);
                float* dst = &out.px[(static_cast<size_t>(py) * out.w + px2) * 4u];
                for (int c = 0; c < 4; ++c) {
                    dst[c] = rgba[c];
                }
            }
        }
    }
    // 反预乘 + 量化到 8 位域
    for (size_t i = 0; i < out.px.size(); i += 4u) {
        const float a = out.px[i + 3u];
        if (a > 1e-6f) {
            const float inv = 1.0f / a;
            out.px[i] = std::min(1.0f, out.px[i] * inv);
            out.px[i + 1u] = std::min(1.0f, out.px[i + 1u] * inv);
            out.px[i + 2u] = std::min(1.0f, out.px[i + 2u] * inv);
        } else {
            out.px[i] = out.px[i + 1u] = out.px[i + 2u] = 0.0f;
        }
    }
    return out;
}

std::shared_ptr<const TileGrid> applyPuppetGrid(const TileGrid& src, const RectI& bounds,
                                                const Warp& w, double outOrigin[2]) {
    const uint32_t srcW = src.cols * kTileSize;
    const uint32_t srcH = src.rows * kTileSize;
    if (srcW == 0 || srcH == 0 || bounds.empty()) {
        return nullptr;
    }
    Img in;
    in.w = static_cast<int>(srcW);
    in.h = static_cast<int>(srcH);
    in.px.assign(static_cast<size_t>(srcW) * srcH * 4u, 0.0f);
    std::vector<uint8_t> row(static_cast<size_t>(srcW) * 4u);
    for (uint32_t y = 0; y < srcH; ++y) {
        const uint32_t ty = y / kTileSize;
        const uint32_t inY = y % kTileSize;
        std::memset(row.data(), 0, static_cast<size_t>(srcW) * 4u);
        for (uint32_t tx = 0; tx < src.cols; ++tx) {
            auto it = src.tiles.find(ty * src.cols + tx);
            if (it == src.tiles.end() || it->second == nullptr || it->second->pixels == nullptr) {
                continue;
            }
            const uint8_t* sp = it->second->pixels->mapCpu();
            if (sp == nullptr) {
                continue;
            }
            std::memcpy(row.data() + static_cast<size_t>(tx) * kTileSize * 4u,
                        sp + static_cast<size_t>(inY) * it->second->pixels->rowBytes(),
                        static_cast<size_t>(kTileSize) * 4u);
        }
        for (uint32_t x = 0; x < srcW; ++x) {
            float* d = in.px.data() + (static_cast<size_t>(y) * srcW + x) * 4u;
            d[0] = static_cast<float>(row[static_cast<size_t>(x) * 4u]) / 255.0f;
            d[1] = static_cast<float>(row[static_cast<size_t>(x) * 4u + 1u]) / 255.0f;
            d[2] = static_cast<float>(row[static_cast<size_t>(x) * 4u + 2u]) / 255.0f;
            d[3] = static_cast<float>(row[static_cast<size_t>(x) * 4u + 3u]) / 255.0f;
        }
    }
    const DeformResult res = deform(in, bounds, w, kIterations);
    if (!res.ok || res.solver.mesh.tris.empty()) {
        return nullptr;  // 跨度超限/空网格：不改动
    }
    double bb[4] = {0, 0, 0, 0};
    if (!deformedBounds(res.verts, bb)) {
        return nullptr;
    }
    outOrigin[0] = std::floor(bb[0]);
    outOrigin[1] = std::floor(bb[1]);
    const int outW = static_cast<int>(std::ceil(bb[2])) + 1 - static_cast<int>(outOrigin[0]);
    const int outH = static_cast<int>(std::ceil(bb[3])) + 1 - static_cast<int>(outOrigin[1]);
    const Img out = renderPuppet(in, bounds, res.solver.mesh, res.verts, res.order,
                                 outOrigin[0], outOrigin[1], outW, outH);
    const uint32_t cols = std::max(1u, (static_cast<uint32_t>(out.w) + kTileSize - 1) / kTileSize);
    const uint32_t rows2 = std::max(1u, (static_cast<uint32_t>(out.h) + kTileSize - 1) / kTileSize);
    TileGridBuilder builder(cols, rows2);
    for (int y = 0; y < out.h; ++y) {
        std::vector<uint8_t> outRow(static_cast<size_t>(out.w) * 4u);
        bool any = false;
        for (int x = 0; x < out.w; ++x) {
            const float* sp = &out.px[(static_cast<size_t>(y) * out.w + x) * 4u];
            uint8_t* d = &outRow[static_cast<size_t>(x) * 4u];
            for (int c = 0; c < 4; ++c) {
                d[c] = static_cast<uint8_t>(std::lround(std::clamp(sp[c], 0.0f, 1.0f) * 255.0f));
            }
            if (d[3] != 0) {
                any = true;
            }
        }
        if (!any) {
            continue;
        }
        for (uint32_t tx = 0; tx < cols; ++tx) {
            bool tileAny = false;
            for (uint32_t k = 0; k < kTileSize && tx * kTileSize + k < static_cast<uint32_t>(out.w); ++k) {
                if (outRow[(static_cast<size_t>(tx) * kTileSize + k) * 4u + 3u] != 0) {
                    tileAny = true;
                    break;
                }
            }
            if (!tileAny) {
                continue;
            }
            uint8_t* dst2 = builder.ensureTile(tx, y / kTileSize).mapCpuWrite() +
                            (y % kTileSize) * kTileSize * 4u;
            std::memcpy(dst2, &outRow[static_cast<size_t>(tx) * kTileSize * 4u],
                        static_cast<size_t>(std::min<uint64_t>(kTileSize, out.w - tx * kTileSize)) * 4u);
        }
    }
    return builder.publish(src.revision + 1);
}

}  // namespace puppet

}  // namespace montage
