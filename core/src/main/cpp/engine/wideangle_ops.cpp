// M20d 自适应广角 —— photocraft crates/algo/src/wideangle.rs C++ 翻译（MIT OR Apache-2.0）。

#include "engine/wideangle_ops.h"

#include <algorithm>
#include <cmath>

namespace montage {
namespace wideangle {

namespace {
constexpr double kPi = 3.14159265358979323846;

double dot3(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

std::array<double, 3> norm3(const std::array<double, 3>& v) {
    const double l = std::max(std::sqrt(dot3(v, v)), 1e-300);
    return {v[0] / l, v[1] / l, v[2] / l};
}

// 稀疏最小二乘 CGLS（photocraft SparseLs 直迁）
struct SparseLs {
    int n = 0;
    std::vector<std::pair<std::vector<std::pair<int, double>>, double>> rows;

    explicit SparseLs(int n_) : n(n_) {}

    void add(std::vector<std::pair<int, double>> row, double rhs, double w) {
        for (auto& [i, c] : row) {
            c *= w;
        }
        rows.emplace_back(std::move(row), rhs * w);
    }

    std::vector<double> ax(const std::vector<double>& x) const {
        std::vector<double> out(rows.size(), 0.0);
        for (size_t r = 0; r < rows.size(); ++r) {
            double s = 0.0;
            for (const auto& [i, c] : rows[r].first) {
                s += c * x[i];
            }
            out[r] = s;
        }
        return out;
    }

    std::vector<double> atv(const std::vector<double>& v) const {
        std::vector<double> out(n, 0.0);
        for (size_t r = 0; r < rows.size(); ++r) {
            for (const auto& [i, c] : rows[r].first) {
                out[i] += c * v[r];
            }
        }
        return out;
    }

    std::vector<double> solve(const std::vector<double>& x0, int iters) const {
        std::vector<double> x = x0;
        const std::vector<double> axv = ax(x);
        std::vector<double> r(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) {
            r[i] = rows[i].second - axv[i];
        }
        std::vector<double> s = atv(r);
        std::vector<double> p = s;
        double gamma = 0.0;
        for (const double v : s) {
            gamma += v * v;
        }
        const double g0 = std::max(gamma, 1e-30);
        for (int it = 0; it < iters; ++it) {
            if (gamma <= g0 * 1e-20) {
                break;
            }
            const std::vector<double> q = ax(p);
            double qq = 0.0;
            for (const double v : q) {
                qq += v * v;
            }
            if (qq <= 1e-300) {
                break;
            }
            const double alpha = gamma / qq;
            for (int i = 0; i < n; ++i) {
                x[i] += alpha * p[i];
            }
            for (size_t i = 0; i < r.size(); ++i) {
                r[i] -= alpha * q[i];
            }
            s = atv(r);
            double g1 = 0.0;
            for (const double v : s) {
                g1 += v * v;
            }
            const double beta = g1 / gamma;
            gamma = g1;
            for (int i = 0; i < n; ++i) {
                p[i] = s[i] + beta * p[i];
            }
        }
        return x;
    }
};
}  // namespace

Camera Camera::make(const Params& p, const RectI& frame) {
    const double w = std::max(frame.width(), 1);
    const double h = std::max(frame.height(), 1);
    double f35 = p.focalLength * std::max(p.cropFactor, 0.1);
    Model model = p.model;
    if (model == Model::Auto) {
        if (std::fabs(w / h - 2.0) < 0.01) {
            model = Model::FullSpherical;
        } else if (f35 > 0.0 && f35 < 17.0) {
            model = Model::Fisheye;
        } else {
            model = Model::Perspective;
        }
    }
    if (f35 <= 0.0) {
        f35 = model == Model::Fisheye ? 12.0 : 24.0;
    }
    Camera cam;
    cam.model = model;
    cam.f = f35 / 36.0 * std::max(w, h);
    cam.c = {(static_cast<double>(frame.x0) + frame.x1) / 2.0,
             (static_cast<double>(frame.y0) + frame.y1) / 2.0};
    cam.size = {w, h};
    return cam;
}

std::array<double, 3> Camera::ray(double x, double y) const {
    const double dx = x - c[0];
    const double dy = y - c[1];
    switch (model) {
        case Model::FullSpherical: {
            const double lon = dx / size[0] * 2.0 * kPi;
            const double lat = dy / size[1] * kPi;
            return norm3({std::sin(lon) * std::cos(lat), std::sin(lat), std::cos(lon) * std::cos(lat)});
        }
        case Model::Fisheye: {
            const double r = std::hypot(dx, dy);
            const double th = r / f;
            if (r < 1e-12) {
                return {0.0, 0.0, 1.0};
            }
            return norm3({std::sin(th) * dx / r, std::sin(th) * dy / r, std::cos(th)});
        }
        default:
            return norm3({dx, dy, f});
    }
}

bool Camera::pixel(const std::array<double, 3>& d, std::array<double, 2>& out) const {
    switch (model) {
        case Model::FullSpherical: {
            const double lon = std::atan2(d[0], d[2]);
            const double lat = std::asin(std::clamp(d[1], -1.0, 1.0));
            out = {c[0] + lon / (2.0 * kPi) * size[0], c[1] + lat / kPi * size[1]};
            return true;
        }
        case Model::Fisheye: {
            const double th = std::acos(std::clamp(d[2], -1.0, 1.0));
            const double s = std::hypot(d[0], d[1]);
            if (s < 1e-12) {
                out = c;
                return true;
            }
            out = {c[0] + f * th * d[0] / s, c[1] + f * th * d[1] / s};
            return true;
        }
        default:
            if (d[2] <= 1e-6) {
                return false;
            }
            out = {c[0] + f * d[0] / d[2], c[1] + f * d[1] / d[2]};
            return true;
    }
}

std::vector<std::array<double, 2>> Camera::arc(const std::array<double, 2>& a,
                                               const std::array<double, 2>& b, int n) const {
    const std::array<double, 3> da = ray(a[0], a[1]);
    const std::array<double, 3> db = ray(b[0], b[1]);
    const double omega = std::acos(std::clamp(dot3(da, db), -1.0, 1.0));
    std::vector<std::array<double, 2>> out;
    for (int k = 0; k <= n; ++k) {
        const double t = static_cast<double>(k) / n;
        std::array<double, 3> d = da;
        if (omega >= 1e-9) {
            const double sa = std::sin((1.0 - t) * omega) / std::sin(omega);
            const double sb = std::sin(t * omega) / std::sin(omega);
            d = norm3({sa * da[0] + sb * db[0], sa * da[1] + sb * db[1], sa * da[2] + sb * db[2]});
        }
        std::array<double, 2> pt;
        if (pixel(d, pt)) {
            out.push_back(pt);
        }
    }
    return out;
}

std::vector<std::array<int, 3>> Mesh::triangles() const {
    const int w1 = nx + 1;
    std::vector<std::array<int, 3>> t;
    t.reserve(static_cast<size_t>(nx) * ny * 2);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const int a = j * w1 + i;
            t.push_back({a, a + 1, a + w1 + 1});
            t.push_back({a, a + w1 + 1, a + w1});
        }
    }
    return t;
}

std::array<double, 2> Mesh::map(const RectI& frame, const std::array<double, 2>& p) const {
    const double cw = static_cast<double>(frame.width()) / nx;
    const double ch = static_cast<double>(frame.height()) / ny;
    const double gx = std::clamp((p[0] - frame.x0) / cw, 0.0, nx - 1e-9);
    const double gy = std::clamp((p[1] - frame.y0) / ch, 0.0, ny - 1e-9);
    const int i = static_cast<int>(gx);
    const int j = static_cast<int>(gy);
    const double tx = gx - i;
    const double ty = gy - j;
    const int w1 = nx + 1;
    auto v = [&](int ii, int jj) -> const std::array<double, 2>& {
        return verts[static_cast<size_t>(jj) * w1 + ii].first;
    };
    const auto& a = v(i, j);
    const auto& b = v(i + 1, j);
    const auto& c = v(i, j + 1);
    const auto& d = v(i + 1, j + 1);
    return {(a[0] * (1.0 - tx) + b[0] * tx) * (1.0 - ty) + (c[0] * (1.0 - tx) + d[0] * tx) * ty,
            (a[1] * (1.0 - tx) + b[1] * tx) * (1.0 - ty) + (c[1] * (1.0 - tx) + d[1] * tx) * ty};
}

Mesh solve(const Params& p, const RectI& frame) {
    const Camera cam = Camera::make(p, frame);
    const double fw = std::max(frame.width(), 1);
    const double fh = std::max(frame.height(), 1);
    constexpr double kCells = 48.0;
    const double cell = std::max(fw, fh) / kCells;
    const int nx = std::clamp(static_cast<int>(std::round(fw / cell)), 2, 96);
    const int ny = std::clamp(static_cast<int>(std::round(fh / cell)), 2, 96);
    const double cw = fw / nx;
    const double ch = fh / ny;
    const int w1 = nx + 1;
    const int nv = w1 * (ny + 1);
    std::vector<std::array<double, 2>> src(nv);
    for (int k = 0; k < nv; ++k) {
        src[k] = {frame.x0 + (k % w1) * cw, frame.y0 + (k / w1) * ch};
    }
    std::vector<std::vector<std::array<double, 2>>> curves;
    for (const Constraint& c : p.constraints) {
        curves.push_back(cam.arc(c.a, c.b, 24));
    }
    std::vector<double> x;
    x.reserve(2 * nv);
    for (const auto& s : src) {
        x.push_back(s[0]);
        x.push_back(s[1]);
    }
    auto weights = [&](const std::array<double, 2>& q) -> std::array<std::pair<int, double>, 4> {
        const double gx = std::clamp((q[0] - frame.x0) / cw, 0.0, nx - 1e-9);
        const double gy = std::clamp((q[1] - frame.y0) / ch, 0.0, ny - 1e-9);
        const int i = static_cast<int>(gx);
        const int j = static_cast<int>(gy);
        const double tx = gx - i;
        const double ty = gy - j;
        const int a = j * w1 + i;
        std::array<std::pair<int, double>, 4> out;
        out[0] = {a, (1.0 - tx) * (1.0 - ty)};
        out[1] = {a + 1, tx * (1.0 - ty)};
        out[2] = {a + w1, (1.0 - tx) * ty};
        out[3] = {a + w1 + 1, tx * ty};
        return out;
    };
    std::vector<std::array<int, 3>> tris;
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const int a = j * w1 + i;
            tris.push_back({a, a + 1, a + w1 + 1});
            tris.push_back({a, a + w1 + 1, a + w1});
            // 双对角：形状项对称
            tris.push_back({a, a + 1, a + w1});
            tris.push_back({a + 1, a + w1 + 1, a + w1});
        }
    }
    std::vector<int> active;
    for (size_t k = 0; k < curves.size(); ++k) {
        if (curves[k].size() >= 3) {
            active.push_back(static_cast<int>(k));
        }
    }
    if (!active.empty()) {
        std::vector<std::array<double, 2>> normals(active.size(), {0.0, 1.0});
        for (int round = 0; round < 4; ++round) {
            SparseLs ls(2 * nv);
            // 形状项：v0 = v1 + u(v2−v1) + v·R90(v2−v1)（(u,v) 源域）
            for (const auto& t : tris) {
                for (int rot = 0; rot < 3; ++rot) {
                    const int i0 = t[rot], i1 = t[(rot + 1) % 3], i2 = t[(rot + 2) % 3];
                    const std::array<double, 2>& p0 = src[i0];
                    const std::array<double, 2>& p1 = src[i1];
                    const std::array<double, 2>& p2 = src[i2];
                    const double e[2] = {p2[0] - p1[0], p2[1] - p1[1]};
                    const double l2 = e[0] * e[0] + e[1] * e[1];
                    const double d[2] = {p0[0] - p1[0], p0[1] - p1[1]};
                    const double u = (d[0] * e[0] + d[1] * e[1]) / l2;
                    const double v = (d[0] * -e[1] + d[1] * e[0]) / l2;
                    // x
                    ls.add({{2 * i0, 1.0}, {2 * i1, -1.0 + u}, {2 * i2, -u}, {2 * i2 + 1, v}, {2 * i1 + 1, -v}}, 0.0, 1.0);
                    // y
                    ls.add({{2 * i0 + 1, 1.0}, {2 * i1 + 1, -1.0 + u}, {2 * i2 + 1, -u}, {2 * i2, -v}, {2 * i1, v}}, 0.0, 1.0);
                }
            }
            // 弱锚定（固定全局相似）
            for (int k = 0; k < nv; ++k) {
                ls.add({{2 * k, 1.0}}, src[k][0], 0.02);
                ls.add({{2 * k + 1, 1.0}}, src[k][1], 0.02);
            }
            // 线约束
            for (size_t ci = 0; ci < active.size(); ++ci) {
                const int k = active[ci];
                const auto& c = curves[k];
                std::array<double, 2> nrm = {0.0, 1.0};
                switch (p.constraints[k].orientation) {
                    case Orientation::Horizontal:
                        nrm = {0.0, 1.0};
                        break;
                    case Orientation::Vertical:
                        nrm = {1.0, 0.0};
                        break;
                    default:
                        nrm = normals[ci];
                        break;
                }
                const auto w0 = weights(c[0]);
                for (size_t qi = 1; qi < c.size(); ++qi) {
                    const auto wq = weights(c[qi]);
                    std::vector<std::pair<int, double>> row;
                    row.reserve(16);
                    for (const auto& [i, w] : wq) {
                        row.emplace_back(2 * i, nrm[0] * w);
                        row.emplace_back(2 * i + 1, nrm[1] * w);
                    }
                    for (const auto& [i, w] : w0) {
                        row.emplace_back(2 * i, -nrm[0] * w);
                        row.emplace_back(2 * i + 1, -nrm[1] * w);
                    }
                    ls.add(std::move(row), 0.0, 10.0);
                }
            }
            x = ls.solve(x, 400 + 200 * round);
            // 自由方向法向重估
            auto pos = [&](const std::array<double, 2>& q) {
                std::array<double, 2> acc = {0.0, 0.0};
                for (const auto& [i, w] : weights(q)) {
                    acc[0] += w * x[2 * i];
                    acc[1] += w * x[2 * i + 1];
                }
                return acc;
            };
            bool changed = false;
            for (size_t ci = 0; ci < active.size(); ++ci) {
                const auto& c = curves[active[ci]];
                const std::array<double, 2> pa = pos(c[0]);
                const std::array<double, 2> pb = pos(c[c.size() - 1]);
                const double d[2] = {pb[0] - pa[0], pb[1] - pa[1]};
                const double l = std::max(std::hypot(d[0], d[1]), 1e-9);
                const std::array<double, 2> nn = {-d[1] / l, d[0] / l};
                if (std::fabs(nn[0] - normals[ci][0]) + std::fabs(nn[1] - normals[ci][1]) > 1e-3) {
                    changed = true;
                }
                normals[ci] = nn;
            }
            bool anyFree = false;
            for (const int k : active) {
                if (p.constraints[k].orientation == Orientation::Free) {
                    anyFree = true;
                }
            }
            if (round == 0 && !anyFree) {
                break;
            }
            if (round > 0 && !changed) {
                break;
            }
        }
    }
    // 绕中心缩放
    const double s = std::clamp(p.scale / 100.0, 0.1, 10.0);
    const std::array<double, 2>& c0 = cam.c;
    Mesh mesh;
    mesh.nx = nx;
    mesh.ny = ny;
    mesh.curves = curves;
    mesh.verts.resize(nv);
    for (int k = 0; k < nv; ++k) {
        mesh.verts[k] = {{c0[0] + (x[2 * k] - c0[0]) * s, c0[1] + (x[2 * k + 1] - c0[1]) * s}, src[k]};
    }
    // 直线度残差：曲线样本到映射端点弦的距离
    double worst = 0.0;
    for (const auto& cv : mesh.curves) {
        if (cv.size() < 3) {
            continue;
        }
        std::vector<std::array<double, 2>> pts;
        pts.reserve(cv.size());
        for (const auto& q : cv) {
            pts.push_back(mesh.map(frame, q));
        }
        const std::array<double, 2>& a = pts[0];
        const std::array<double, 2>& b = pts[pts.size() - 1];
        const double d[2] = {b[0] - a[0], b[1] - a[1]};
        const double l = std::max(std::hypot(d[0], d[1]), 1e-9);
        for (const auto& q : pts) {
            worst = std::max(worst, std::fabs((q[0] - a[0]) * d[1] - (q[1] - a[1]) * d[0]) / l);
        }
    }
    mesh.residual = worst;
    return mesh;
}

}  // namespace wideangle

}  // namespace montage
