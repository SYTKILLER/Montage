// 描边实现（翻译自 photocraft crates/vector/src/stroke.rs，MIT OR Apache-2.0）。

#include "engine/vector_stroke.h"

#include <algorithm>
#include <cmath>

namespace montage {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;

VPt subP(VPt a, VPt b) { return vpt(a.x - b.x, a.y - b.y); }
VPt addP(VPt a, VPt b) { return vpt(a.x + b.x, a.y + b.y); }
VPt mulP(VPt a, double k) { return vpt(a.x * k, a.y * k); }
double lenP(VPt a) { return std::hypot(a.x, a.y); }
VPt normP(VPt a) {
    double l = lenP(a);
    return l > 0.0 ? vpt(a.x / l, a.y / l) : vpt(0.0, 0.0);
}
// y 向下坐标系中的左法线。
VPt perpP(VPt a) { return vpt(a.y, -a.x); }
double crossP(VPt a, VPt b) { return a.x * b.y - a.y * b.x; }
double dotP(VPt a, VPt b) { return a.x * b.x + a.y * b.y; }

double signedArea(const std::vector<VPt>& p) {
    double sum = 0.0;
    size_t n = p.size();
    for (size_t i = 0; i < n; i++) {
        sum += crossP(p[i], p[(i + 1) % n]);
    }
    return sum * 0.5;
}

// 以正向面积发出（丢弃退化碎块）。
void emit(std::vector<std::vector<VPt>>& out, std::vector<VPt> poly) {
    double a = signedArea(poly);
    if (std::fabs(a) < 1e-12) {
        return;
    }
    if (a < 0.0) {
        std::reverse(poly.begin(), poly.end());
    }
    out.push_back(std::move(poly));
}

// 弧步长：弦对半径 r 圆的偏离 ≤ tol。
double arcStep(double r, double tol) {
    if (r <= tol) {
        return kPi / 2.0;
    }
    double v = 2.0 * std::acos(std::min(std::max(1.0 - tol / r, -1.0), 1.0));
    return std::min(std::max(v, 0.01), kPi / 2.0);
}

// 步长 step 弧度的弦顶点半径：每弦扇形面积 = 圆扇形面积（round cap/join 任意细分下
// 面积精确）。
double areaRadius(double r, double step) {
    return step < 1e-9 ? r : r * std::sqrt(step / std::sin(step));
}

std::vector<VPt> circlePts(VPt c, double r, double tol) {
    size_t n = static_cast<size_t>(std::ceil(kTwoPi / arcStep(r, tol)));
    if (n < 8) {
        n = 8;
    }
    double rr = areaRadius(r, kTwoPi / static_cast<double>(n));
    std::vector<VPt> out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
        double a = kTwoPi * static_cast<double>(i) / static_cast<double>(n);
        out.push_back(vpt(c.x + rr * std::cos(a), c.y + rr * std::sin(a)));
    }
    return out;
}

void joinPiece(VPt v, VPt d1, VPt d2, double hw, LineJoin join, double miterLimit, double tol,
               std::vector<std::vector<VPt>>& out) {
    double c = crossP(d1, d2);
    double d = dotP(d1, d2);
    if (std::fabs(c) < 1e-12 && d > 0.0) {
        return;
    }
    // 外侧：右转（y 向下 c>0）打开左侧。
    double s = c > 0.0 ? 1.0 : -1.0;
    VPt n1 = mulP(perpP(d1), hw * s);
    VPt n2 = mulP(perpP(d2), hw * s);
    VPt a = addP(v, n1);
    VPt b = addP(v, n2);
    switch (join) {
        case LineJoin::Bevel:
            emit(out, {v, a, b});
            break;
        case LineJoin::Miter: {
            // 斜接长/宽 = 1/cos(θ/2)，θ = 段间转角。
            double cosHalf = std::sqrt(std::max((1.0 + d) * 0.5, 0.0));
            if (cosHalf < 1e-9 || 1.0 / cosHalf > miterLimit) {
                emit(out, {v, a, b});
            } else {
                VPt bis = normP(addP(n1, n2));
                VPt m = addP(v, mulP(bis, hw / cosHalf));
                emit(out, {v, a, m, b});
            }
            break;
        }
        case LineJoin::Round: {
            double a0 = std::atan2(n1.y, n1.x);
            double a1 = std::atan2(n2.y, n2.x);
            double delta = a1 - a0;
            while (delta > kPi) {
                delta -= kTwoPi;
            }
            while (delta < -kPi) {
                delta += kTwoPi;
            }
            if (std::fabs(c) < 1e-12) {
                // U 形折返：在 n1 一侧转向 d1 的半圆。
                delta = kPi * (crossP(n1, d1) > 0.0 ? 1.0 : -1.0);
            }
            a1 = a0 + delta;
            size_t steps = static_cast<size_t>(std::ceil(std::fabs(delta) / arcStep(hw, tol)));
            if (steps < 1) {
                steps = 1;
            }
            double rr = areaRadius(hw, std::fabs(delta) / static_cast<double>(steps));
            std::vector<VPt> poly = {v, a};
            for (size_t k = 1; k < steps; k++) {
                double t = a0 + (a1 - a0) * static_cast<double>(k) / static_cast<double>(steps);
                poly.push_back(vpt(v.x + rr * std::cos(t), v.y + rr * std::sin(t)));
            }
            poly.push_back(b);
            emit(out, std::move(poly));
            break;
        }
    }
}

void capPiece(VPt p, VPt dir, double hw, LineCap cap, double tol, std::vector<std::vector<VPt>>& out) {
    switch (cap) {
        case LineCap::Butt:
            break;
        case LineCap::Round:
            emit(out, circlePts(p, hw, tol));
            break;
        case LineCap::Square: {
            VPt n = mulP(perpP(dir), hw);
            VPt e = mulP(dir, hw);
            emit(out, {addP(p, n), addP(addP(p, n), e), addP(subP(p, n), e), subP(p, n)});
            break;
        }
    }
}

void strokeOne(const Polyline& pl, const StrokeStyle& style, double tol,
               std::vector<std::vector<VPt>>& out) {
    double hw = style.width * 0.5;
    // 去重复点。
    std::vector<VPt> pts;
    std::vector<bool> knot;
    pts.reserve(pl.pts.size());
    knot.reserve(pl.pts.size());
    for (size_t i = 0; i < pl.pts.size(); i++) {
        if (!pts.empty() && lenP(subP(pl.pts[i], pts.back())) < 1e-9) {
            continue;
        }
        pts.push_back(pl.pts[i]);
        knot.push_back(i < pl.knot.size() ? pl.knot[i] : true);
    }
    bool closed = pl.closed;
    if (closed && pts.size() > 1 && lenP(subP(pts[0], pts[pts.size() - 1])) < 1e-9) {
        pts.pop_back();
        knot.pop_back();
    }
    if (pts.size() < 3) {
        closed = false;
    }
    if (pts.empty()) {
        return;
    }
    if (pts.size() == 1) {
        // 孤点：只有 round/square cap 画得出东西。
        VPt c = pts[0];
        if (style.cap == LineCap::Round) {
            emit(out, circlePts(c, hw, tol));
        } else if (style.cap == LineCap::Square) {
            emit(out, {vpt(c.x - hw, c.y - hw), vpt(c.x + hw, c.y - hw), vpt(c.x + hw, c.y + hw),
                       vpt(c.x - hw, c.y + hw)});
        }
        return;
    }
    size_t n = pts.size();
    size_t segs = closed ? n : n - 1;
    auto dir = [&](size_t i) { return normP(subP(pts[(i + 1) % n], pts[i])); };
    auto segLen = [&](size_t i) { return lenP(subP(pts[(i + 1) % n], pts[i])); };
    // 顶点延续 ribbon（无独立 join 碎块）的条件：展平点小折角且内偏不回折，或 knot 处
    // 路径本来直。
    double cosSmooth = std::cos(25.0 * kPi / 180.0);
    auto smooth = [&](size_t j) {
        if (j == 0 || (!closed && j == n - 1)) {
            return false;
        }
        size_t i0 = (j + n - 1) % n;
        double d = dotP(dir(i0), dir(j));
        if (knot[j]) {
            return d > 1.0 - 1e-12;
        }
        if (d < cosSmooth) {
            return false;
        }
        double tanHalf = std::sqrt(std::max((1.0 - d) / (1.0 + d), 0.0));
        return hw * tanHalf < 0.5 * std::min(segLen(i0), segLen(j));
    };
    std::vector<VPt> left;
    std::vector<VPt> right;
    for (size_t i = 0; i < segs; i++) {
        VPt a = pts[i];
        VPt b = pts[(i + 1) % n];
        VPt nrm = mulP(perpP(dir(i)), hw);
        if (left.empty()) {
            left.push_back(addP(a, nrm));
            right.push_back(subP(a, nrm));
        }
        size_t j = (i + 1) % n;
        if (i + 1 < segs + (closed ? 0u : 1u) && smooth(j)) {
            // 两段偏移线交于 b ± 平分线·hw/cos(θ/2)。
            VPt n2 = mulP(perpP(dir(j)), hw);
            VPt bis = normP(addP(nrm, n2));
            double cosHalf = std::sqrt(std::max((1.0 + dotP(dir(i), dir(j))) * 0.5, 1e-9));
            VPt off = mulP(bis, hw / cosHalf);
            left.push_back(addP(b, off));
            right.push_back(subP(b, off));
            continue;
        }
        left.push_back(addP(b, nrm));
        right.push_back(subP(b, nrm));
        std::vector<VPt> poly = std::move(left);
        poly.reserve(poly.size() + right.size());
        for (auto it = right.rbegin(); it != right.rend(); ++it) {
            poly.push_back(*it);
        }
        left.clear();
        right.clear();
        emit(out, std::move(poly));
        bool isJoin = closed || j != n - 1;
        if (isJoin) {
            VPt prev = pts[i];
            VPt v = pts[j];
            VPt next = pts[(j + 1) % n];
            VPt d1 = normP(subP(v, prev));
            VPt d2 = normP(subP(next, v));
            // 展平点用 miter（平滑曲线的真实偏移）；请求 round 则 round；bevel 只在真角点。
            LineJoin jn = style.join;
            double ml = style.miterLimit;
            if (!knot[j]) {
                jn = jn == LineJoin::Round ? LineJoin::Round : LineJoin::Miter;
                ml = 500.0;
            } else {
                ml = std::min(ml, 500.0);
            }
            joinPiece(v, d1, d2, hw, jn, ml, tol, out);
        }
    }
    if (!closed) {
        VPt d0 = normP(subP(pts[1], pts[0]));
        VPt d1 = normP(subP(pts[n - 1], pts[n - 2]));
        capPiece(pts[0], mulP(d0, -1.0), hw, style.cap, tol, out);
        capPiece(pts[n - 1], d1, hw, style.cap, tol, out);
    }
}

}  // namespace

std::vector<std::vector<VPt>> strokePolygons(const std::vector<Polyline>& lines, const StrokeStyle& style,
                                             double tol) {
    std::vector<std::vector<VPt>> out;
    if (style.width <= 0.0 || !std::isfinite(style.width) || !std::isfinite(style.miterLimit) ||
        style.miterLimit < 1.0) {
        return out;
    }
    for (const Polyline& pl : lines) {
        bool finite = true;
        for (const VPt& p : pl.pts) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                finite = false;
                break;
            }
        }
        if (!finite) {
            continue;
        }
        if (!style.dashes.empty()) {
            for (Polyline& dash : dashPolyline(pl, style.dashes, style.dashOffset)) {
                strokeOne(dash, style, tol, out);
            }
        } else {
            strokeOne(pl, style, tol, out);
        }
    }
    return out;
}

std::vector<Polyline> dashPolyline(const Polyline& pl, const std::vector<double>& pattern, double offset) {
    if (pattern.empty()) {
        return std::vector<Polyline>{pl};
    }
    if (pattern.size() > 32 || !std::isfinite(offset)) {
        return {};
    }
    for (double d : pattern) {
        if (!std::isfinite(d) || d < 0.0) {
            return {};
        }
    }
    for (const VPt& p : pl.pts) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
            return {};
        }
    }
    std::vector<double> pat = pattern;
    if (pat.size() % 2 == 1) {
        std::vector<double> copy = pat;
        pat.insert(pat.end(), copy.begin(), copy.end());
    }
    double total = 0.0;
    for (double d : pat) {
        total += d;
    }
    if (!std::isfinite(total) || total <= 1e-9 || pl.pts.size() < 2) {
        return {};
    }
    std::vector<VPt> pts = pl.pts;
    std::vector<bool> knots = pl.knot;
    knots.resize(pts.size(), true);
    if (pl.closed) {
        pts.push_back(pl.pts[0]);
        knots.push_back(true);
    }
    // 图案内位置。
    size_t idx = 0;
    double left = pat[0];
    double skip = std::fmod(std::fmod(offset, total) + total, total);
    while (skip > 0.0) {
        if (skip >= left) {
            skip -= left;
            idx = (idx + 1) % pat.size();
            left = pat[idx];
        } else {
            left -= skip;
            skip = 0.0;
        }
    }
    std::vector<Polyline> out;
    bool inDash = idx % 2 == 0;
    Polyline cur;
    if (inDash) {
        cur.pts.push_back(pts[0]);
        cur.knot.push_back(true);
        cur.closed = false;
    }
    size_t guard = 0;
    for (size_t wi = 0; wi + 1 < pts.size(); wi++) {
        VPt a = pts[wi];
        VPt b = pts[wi + 1];
        bool bKnot = knots[wi + 1];
        double seg = lenP(subP(b, a));
        double pos = 0.0;
        while (seg - pos > 1e-12) {
            guard++;
            if (guard > 100000) {
                // 上界分配与耗时（含文件提供的 pattern）。命令侧已在文档变更前拒绝
                // 超预算的 pattern（validateShape）。
                return {};
            }
            double step = std::min(left, seg - pos);
            if (step > 0.0 && pos + step == pos) {
                return {};
            }
            pos += step;
            left -= step;
            bool atEnd = seg - pos <= 1e-12;
            VPt p = atEnd ? b : addP(a, mulP(subP(b, a), pos / seg));
            if (inDash) {
                cur.pts.push_back(p);
                cur.knot.push_back(!atEnd || bKnot);
            }
            if (left <= 1e-12) {
                idx = (idx + 1) % pat.size();
                left = pat[idx];
                if (idx % 2 == 1) {
                    if (inDash) {
                        out.push_back(std::move(cur));
                        cur = Polyline{};
                        inDash = false;
                    }
                } else {
                    cur = Polyline{};
                    cur.pts.push_back(p);
                    cur.knot.push_back(true);
                    cur.closed = false;
                    inDash = true;
                }
            }
        }
        // 顶点 b 是 dash 内部的 join：保持（到点时已推入）。
    }
    if (inDash && cur.pts.size() > 1) {
        out.push_back(std::move(cur));
    }
    if (pl.closed) {
        // 跨闭合顶点的 dash 有一个 join（不是两个 cap）。缝合尾段到首段时保持 knot 标志。
        VPt seam = pl.pts.front();
        bool wraps = !out.empty() && out.front().pts.front() == seam && out.back().pts.back() == seam;
        if (wraps && out.size() == 1) {
            out.front().closed = true;
        } else if (wraps && out.size() > 1) {
            // 尾段（跨缝 dash）吸收首段第 2 点起的全部点后放回首位——闭合接缝处是
            // join 而非两个 cap（photocraft tail.extend(first.skip(1))）。
            Polyline tail = std::move(out.back());
            out.pop_back();
            Polyline& first = out.front();
            for (size_t i = 1; i < first.pts.size(); i++) {
                tail.pts.push_back(first.pts[i]);
                tail.knot.push_back(i < first.knot.size() ? first.knot[i] : true);
            }
            out.front() = std::move(tail);
        }
    }
    return out;
}

}  // namespace montage
