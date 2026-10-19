// 参数形状实现（翻译自 photocraft crates/vector/src/shapes.rs，MIT OR Apache-2.0）。

#include "engine/vector_shapes.h"

#include <algorithm>
#include <cmath>

namespace montage {

namespace {

Knot cornerKnot(double x, double y) {
    Knot k;
    k.anchor = vpt(x, y);
    k.inCtrl = k.anchor;
    k.outCtrl = k.anchor;
    k.smooth = false;
    return k;
}

Knot smoothKnot(VPt anchor, VPt inCtrl, VPt outCtrl) {
    Knot k;
    k.anchor = anchor;
    k.inCtrl = inCtrl;
    k.outCtrl = outCtrl;
    k.smooth = true;
    return k;
}

}  // namespace

VectorPath shapeRect(double x, double y, double w, double h) {
    Subpath sp;
    sp.closed = true;
    sp.op = PathOp::Combine;
    const VPt pts[4] = {vpt(x, y), vpt(x + w, y), vpt(x + w, y + h), vpt(x, y + h)};
    for (const VPt& p : pts) {
        sp.knots.push_back(cornerKnot(p.x, p.y));
    }
    VectorPath path;
    path.subpaths.push_back(std::move(sp));
    return path;
}

void clampRadii(double w, double h, double radii[4]) {
    double r[4];
    for (int i = 0; i < 4; i++) {
        r[i] = radii[i] > 0.0 ? radii[i] : 0.0;
    }
    double f = 1.0;
    const double sides[4][3] = {{r[0], r[1], w}, {r[1], r[2], h}, {r[2], r[3], w}, {r[3], r[0], h}};
    for (auto& c : sides) {
        if (c[0] + c[1] > c[2] && c[0] + c[1] > 0.0) {
            f = std::min(f, c[2] / (c[0] + c[1]));
        }
    }
    for (int i = 0; i < 4; i++) {
        radii[i] = r[i] * f;
    }
}

VectorPath shapeRoundedRect(double x, double y, double w, double h, const double radiiIn[4]) {
    w = std::fabs(w);
    h = std::fabs(h);
    double radii[4] = {radiiIn[0], radiiIn[1], radiiIn[2], radiiIn[3]};
    clampRadii(w, h, radii);
    const double tl = radii[0];
    const double tr = radii[1];
    const double br = radii[2];
    const double bl = radii[3];
    if (tl <= 0.0 && tr <= 0.0 && br <= 0.0 && bl <= 0.0) {
        return shapeRect(x, y, w, h);
    }
    const double x1 = x + w;
    const double y1 = y + h;
    std::vector<Knot> k;
    k.reserve(8);
    // 每个圆角弧从前一边的终点画到下一边的起点（顺时针）。r ≤ 0 = 直角单点。
    auto pushArc = [&](VPt start, VPt end, VPt cStart, VPt cEnd, double r) {
        if (r <= 0.0) {
            k.push_back(cornerKnot(start.x, start.y));
            return;
        }
        Knot a;
        a.anchor = start;
        a.inCtrl = start;
        a.outCtrl = cStart;
        a.smooth = false;
        k.push_back(a);
        Knot b;
        b.anchor = end;
        b.inCtrl = cEnd;
        b.outCtrl = end;
        b.smooth = false;
        k.push_back(b);
    };
    pushArc(vpt(x1 - tr, y), vpt(x1, y + tr), vpt(x1 - tr + kShapeKappa * tr, y), vpt(x1, y + tr - kShapeKappa * tr), tr);
    pushArc(vpt(x1, y1 - br), vpt(x1 - br, y1), vpt(x1, y1 - br + kShapeKappa * br), vpt(x1 - br + kShapeKappa * br, y1), br);
    pushArc(vpt(x + bl, y1), vpt(x, y1 - bl), vpt(x + bl - kShapeKappa * bl, y1), vpt(x, y1 - bl + kShapeKappa * bl), bl);
    pushArc(vpt(x, y + tl), vpt(x + tl, y), vpt(x, y + tl - kShapeKappa * tl), vpt(x + tl - kShapeKappa * tl, y), tl);
    // 旋转到首 knot 落在顶边上（左上角弧的终点）。
    if (!k.empty()) {
        std::rotate(k.begin(), k.end() - 1, k.end());
    }
    // 合并重复相邻锚点（零长直边）。
    std::vector<Knot> out;
    out.reserve(k.size());
    for (Knot kn : k) {
        if (!out.empty()) {
            Knot& last = out.back();
            if (std::fabs(last.anchor.x - kn.anchor.x) < 1e-12 &&
                std::fabs(last.anchor.y - kn.anchor.y) < 1e-12) {
                last.outCtrl = kn.outCtrl;
                continue;
            }
        }
        out.push_back(kn);
    }
    if (out.size() > 1) {
        Knot& f = out.front();
        Knot& l = out.back();
        if (std::fabs(f.anchor.x - l.anchor.x) < 1e-12 && std::fabs(f.anchor.y - l.anchor.y) < 1e-12) {
            f.inCtrl = l.inCtrl;
            out.pop_back();
        }
    }
    Subpath sp;
    sp.closed = true;
    sp.op = PathOp::Combine;
    sp.knots = std::move(out);
    VectorPath path;
    path.subpaths.push_back(std::move(sp));
    return path;
}

VectorPath shapeEllipse(double x, double y, double w, double h) {
    double rx = w / 2.0;
    double ry = h / 2.0;
    double cx = x + rx;
    double cy = y + ry;
    double kx = rx * kShapeKappa;
    double ky = ry * kShapeKappa;
    Subpath sp;
    sp.closed = true;
    sp.op = PathOp::Combine;
    sp.knots.push_back(smoothKnot(vpt(cx, cy - ry), vpt(cx - kx, cy - ry), vpt(cx + kx, cy - ry)));
    sp.knots.push_back(smoothKnot(vpt(cx + rx, cy), vpt(cx + rx, cy - ky), vpt(cx + rx, cy + ky)));
    sp.knots.push_back(smoothKnot(vpt(cx, cy + ry), vpt(cx + kx, cy + ry), vpt(cx - kx, cy + ry)));
    sp.knots.push_back(smoothKnot(vpt(cx - rx, cy), vpt(cx - rx, cy + ky), vpt(cx - rx, cy - ky)));
    VectorPath path;
    path.subpaths.push_back(std::move(sp));
    return path;
}

VectorPath shapePolygon(double x, double y, double w, double h, uint32_t sides, double starRatio) {
    uint32_t n = sides < 3 ? 3 : (sides > 100 ? 100 : sides);
    double rx = w / 2.0;
    double ry = h / 2.0;
    double cx = x + rx;
    double cy = y + ry;
    bool star = starRatio > 0.0 && starRatio < 1.0;
    size_t count = star ? static_cast<size_t>(n) * 2 : static_cast<size_t>(n);
    Subpath sp;
    sp.closed = true;
    sp.op = PathOp::Combine;
    for (size_t i = 0; i < count; i++) {
        double a = -1.5707963267948966 + 6.283185307179586 * static_cast<double>(i) / static_cast<double>(count);
        double k = (star && i % 2 == 1) ? starRatio : 1.0;
        sp.knots.push_back(cornerKnot(cx + rx * k * std::cos(a), cy + ry * k * std::sin(a)));
    }
    VectorPath path;
    path.subpaths.push_back(std::move(sp));
    return path;
}

VectorPath shapeLine(VPt a, VPt b, double weight) {
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    double l = std::hypot(dx, dy);
    double hw = std::max(weight, 0.0) / 2.0;
    double nx = l > 0.0 ? -dy / l * hw : 0.0;
    double ny = l > 0.0 ? dx / l * hw : hw;
    Subpath sp;
    sp.closed = true;
    sp.op = PathOp::Combine;
    const VPt pts[4] = {vpt(a.x + nx, a.y + ny), vpt(b.x + nx, b.y + ny), vpt(b.x - nx, b.y - ny),
                        vpt(a.x - nx, a.y - ny)};
    for (const VPt& p : pts) {
        sp.knots.push_back(cornerKnot(p.x, p.y));
    }
    VectorPath path;
    path.subpaths.push_back(std::move(sp));
    return path;
}

VectorPath liveShapePath(const LiveShape& s) {
    switch (s.kind) {
        case ShapeLiveKind::Rect:
            return shapeRoundedRect(s.rect[0], s.rect[1], s.rect[2], s.rect[3], s.radii);
        case ShapeLiveKind::Ellipse:
            return shapeEllipse(s.rect[0], s.rect[1], s.rect[2], s.rect[3]);
        case ShapeLiveKind::Polygon:
            return shapePolygon(s.rect[0], s.rect[1], s.rect[2], s.rect[3], s.sides, s.starRatio);
        case ShapeLiveKind::Line:
            return shapeLine(vpt(s.from[0], s.from[1]), vpt(s.to[0], s.to[1]), s.weight);
        case ShapeLiveKind::None:
        default:
            return VectorPath{};
    }
}

}  // namespace montage
