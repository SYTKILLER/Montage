// 矢量模型实现（翻译自 photocraft doc/src/vector.rs impl 块，MIT OR Apache-2.0）。

#include "engine/vector_path.h"

#include <cmath>

namespace montage {

std::vector<std::array<VPt, 4>> Subpath::segments() const {
    std::vector<std::array<VPt, 4>> out;
    size_t n = knots.size();
    if (n < 2) {
        return out;
    }
    size_t count = closed ? n : n - 1;
    out.reserve(count);
    for (size_t i = 0; i < count; i++) {
        const Knot& a = knots[i];
        const Knot& b = knots[(i + 1) % n];
        out.push_back({a.anchor, a.outCtrl, b.inCtrl, b.anchor});
    }
    return out;
}

bool VectorPath::empty() const {
    for (const Subpath& s : subpaths) {
        if (!s.knots.empty()) {
            return false;
        }
    }
    return true;
}

PathOp VectorPath::effectiveOp(size_t i) const {
    return i == 0 ? PathOp::Combine : subpaths[i].op;
}

std::vector<std::pair<size_t, size_t>> VectorPath::components() const {
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t i = 0; i < subpaths.size(); i++) {
        if (!out.empty() && subpaths[i].op == PathOp::Join) {
            out.back().second = i + 1;
        } else {
            out.push_back({i, i + 1});
        }
    }
    return out;
}

bool VectorPath::controlBounds(double& x0, double& y0, double& x1, double& y1) const {
    bool any = false;
    for (const Subpath& s : subpaths) {
        for (const Knot& k : s.knots) {
            const VPt pts[3] = {k.anchor, k.inCtrl, k.outCtrl};
            for (const VPt& p : pts) {
                if (!any) {
                    x0 = x1 = p.x;
                    y0 = y1 = p.y;
                    any = true;
                } else {
                    x0 = x0 < p.x ? x0 : p.x;
                    y0 = y0 < p.y ? y0 : p.y;
                    x1 = x1 > p.x ? x1 : p.x;
                    y1 = y1 > p.y ? y1 : p.y;
                }
            }
        }
    }
    return any;
}

bool ShapeStroke::validate(std::string& err) const {
    auto finite = [](double v) { return std::isfinite(v); };
    if (!finite(width) || width < 0.0 || width > 1000000.0) {
        err = "描边宽度必须是 0..1000000 px 内的有限值";
        return false;
    }
    if (!finite(opacity) || opacity < 0.0 || opacity > 1.0) {
        err = "描边不透明度必须在 0..100%";
        return false;
    }
    if (!finite(miterLimit) || miterLimit < 1.0 || miterLimit > 500.0) {
        err = "斜接限制必须在 1..500";
        return false;
    }
    if (!finite(dashOffset) || (dashOffset < 0 ? -dashOffset : dashOffset) > 1000000.0) {
        err = "虚线偏移必须为 ±1000000 宽度内";
        return false;
    }
    if (dashes.size() > 32) {
        err = "虚线图案最多 32 段";
        return false;
    }
    double sum = 0.0;
    for (double d : dashes) {
        if (!finite(d) || d < 0.0 || d > 10000.0) {
            err = "虚线段长必须是 0..10000 宽度内的有限值";
            return false;
        }
        sum += d;
    }
    if (!dashes.empty() && sum < 0.01) {
        err = "虚线图案总长至少 0.01 宽度";
        return false;
    }
    return true;
}

}  // namespace montage
