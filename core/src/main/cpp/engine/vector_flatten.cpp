// 展平实现（翻译自 photocraft crates/vector/src/flatten.rs，MIT OR Apache-2.0）。

#include "engine/vector_flatten.h"

#include <algorithm>
#include <cmath>

namespace montage {

void Polyline::push(VPt p, bool isKnot) {
    if (!pts.empty()) {
        const VPt& last = pts.back();
        if (std::fabs(last.x - p.x) < 1e-12 && std::fabs(last.y - p.y) < 1e-12) {
            if (isKnot && !knot.empty()) {
                knot.back() = true;
            }
            return;
        }
    }
    pts.push_back(p);
    knot.push_back(isKnot);
}

VPt evalBezier(const std::array<VPt, 4>& seg, double t) {
    const VPt& p0 = seg[0];
    const VPt& c1 = seg[1];
    const VPt& c2 = seg[2];
    const VPt& p1 = seg[3];
    double mt = 1.0 - t;
    double a = mt * mt * mt;
    double b = 3.0 * mt * mt * t;
    double c = 3.0 * mt * t * t;
    double d = t * t * t;
    return vpt(a * p0.x + b * c1.x + c * c2.x + d * p1.x, a * p0.y + b * c1.y + c * c2.y + d * p1.y);
}

bool bezierIsLine(const std::array<VPt, 4>& seg) {
    const VPt& p0 = seg[0];
    const VPt& c1 = seg[1];
    const VPt& c2 = seg[2];
    const VPt& p1 = seg[3];
    double dx = p1.x - p0.x;
    double dy = p1.y - p0.y;
    double len2 = dx * dx + dy * dy;
    auto onChord = [&](const VPt& c) {
        double vx = c.x - p0.x;
        double vy = c.y - p0.y;
        if (len2 < 1e-24) {
            return std::fabs(vx) < 1e-9 && std::fabs(vy) < 1e-9;
        }
        double t = (vx * dx + vy * dy) / len2;
        double cross = std::fabs(vx * dy - vy * dx) / std::sqrt(len2);
        return cross < 1e-9 && t >= -1e-9 && t <= 1.0 + 1e-9;
    };
    return onChord(c1) && onChord(c2);
}

size_t bezierPieces(const std::array<VPt, 4>& seg, double tol) {
    const VPt& p0 = seg[0];
    const VPt& c1 = seg[1];
    const VPt& c2 = seg[2];
    const VPt& p1 = seg[3];
    double d1 = std::sqrt((p0.x - 2.0 * c1.x + c2.x) * (p0.x - 2.0 * c1.x + c2.x) +
                          (p0.y - 2.0 * c1.y + c2.y) * (p0.y - 2.0 * c1.y + c2.y));
    double d2 = std::sqrt((c1.x - 2.0 * c2.x + p1.x) * (c1.x - 2.0 * c2.x + p1.x) +
                          (c1.y - 2.0 * c2.y + p1.y) * (c1.y - 2.0 * c2.y + p1.y));
    double m = d1 > d2 ? d1 : d2;
    // 小曲线按比例收紧容差，保面积精确。
    double size = std::hypot(c1.x - p0.x, c1.y - p0.y) + std::hypot(c2.x - c1.x, c2.y - c1.y) +
                  std::hypot(p1.x - c2.x, p1.y - c2.y);
    tol = std::max(1e-4, std::min(tol, size * 1e-3));
    double n = std::ceil(std::sqrt(0.75 * m / tol));
    if (!std::isfinite(n)) {
        return 1;
    }
    double clamped = std::min(std::max(n, 1.0), 65536.0);
    return static_cast<size_t>(clamped);
}

Polyline flattenSubpath(const Subpath& s, double tol) {
    Polyline out;
    out.closed = s.closed;
    if (!s.knots.empty()) {
        out.push(s.knots.front().anchor, true);
    }
    for (const std::array<VPt, 4>& seg : s.segments()) {
        if (bezierIsLine(seg)) {
            out.push(seg[3], true);
            continue;
        }
        size_t n = bezierPieces(seg, tol);
        for (size_t j = 1; j <= n; j++) {
            out.push(evalBezier(seg, static_cast<double>(j) / static_cast<double>(n)), j == n);
        }
    }
    if (s.closed && out.pts.size() > 1) {
        const VPt& a = out.pts.front();
        const VPt& b = out.pts.back();
        if (std::fabs(a.x - b.x) < 1e-12 && std::fabs(a.y - b.y) < 1e-12) {
            out.pts.pop_back();
            out.knot.pop_back();
            out.knot[0] = true;
        }
    }
    return out;
}

std::vector<Polyline> flattenPath(const VectorPath& p, double tol) {
    std::vector<Polyline> out;
    out.reserve(p.subpaths.size());
    for (const Subpath& s : p.subpaths) {
        out.push_back(flattenSubpath(s, tol));
    }
    return out;
}

bool validateShape(const VectorPath& path, const ShapeStroke* stroke, std::string& err) {
    size_t knots = 0;
    for (const Subpath& s : path.subpaths) {
        if (knots + s.knots.size() < knots) {  // 溢出
            err = "路径锚点过多";
            return false;
        }
        knots += s.knots.size();
    }
    if (knots > 4096 || path.subpaths.size() > 4096) {
        err = "形状路径超过 4096 锚点/子路径上限";
        return false;
    }
    size_t points = 0;
    double length = 0.0;
    for (const Subpath& sub : path.subpaths) {
        for (const Knot& k : sub.knots) {
            const VPt pts[3] = {k.anchor, k.inCtrl, k.outCtrl};
            for (const VPt& p : pts) {
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::fabs(p.x) > 1000000.0 ||
                    std::fabs(p.y) > 1000000.0) {
                    err = "形状坐标必须是 ±1000000 px 内的有限值";
                    return false;
                }
            }
        }
        points += 1;
        for (const std::array<VPt, 4>& seg : sub.segments()) {
            std::array<VPt, 4> w = {seg[0], seg[1], seg[2], seg[3]};
            for (size_t i = 0; i + 1 < 4; i++) {
                length += std::hypot(w[i + 1].x - w[i].x, w[i + 1].y - w[i].y);
            }
            points += bezierIsLine(seg) ? 1 : bezierPieces(seg, kVectorTolerance);
        }
    }
    if (points > 100000) {
        err = "形状细分超过 100000 点上限";
        return false;
    }
    if (stroke != nullptr) {
        if (!stroke->validate(err)) {
            return false;
        }
        if (stroke->width > 0.0 && !stroke->dashes.empty()) {
            double period = 0.0;
            for (double d : stroke->dashes) {
                period += d;
            }
            period *= stroke->width;
            double transitions = (length / period + 2.0) * static_cast<double>(stroke->dashes.size()) +
                                 static_cast<double>(points);
            if (!std::isfinite(transitions) || transitions > 100000.0) {
                err = "虚线图案对该路径过密（100000 步预算）";
                return false;
            }
        }
    }
    return true;
}

}  // namespace montage
