// 栅格化器实现（翻译自 photocraft crates/vector/src/raster.rs，MIT OR Apache-2.0）。

#include "engine/vector_raster.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/vector_flatten.h"
#include "engine/vector_stroke.h"

namespace montage {

void VectorRasterizer::addComponent(const std::vector<std::vector<VPt>>& polys, PathOp op, FillRule rule) {
    uint32_t comp = static_cast<uint32_t>(comps_.size());
    comps_.push_back({op, rule});
    if (op != PathOp::Combine && comp != 0) {
        unionOnly_ = false;
    }
    for (const std::vector<VPt>& poly : polys) {
        size_t n = poly.size();
        if (n < 2) {
            continue;
        }
        for (size_t i = 0; i < n; i++) {
            VPt a = poly[i];
            VPt b = poly[(i + 1) % n];
            if (!(std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(b.x) && std::isfinite(b.y))) {
                continue;
            }
            if (!hasBounds_) {
                bx0_ = bx1_ = a.x;
                by0_ = by1_ = a.y;
                hasBounds_ = true;
            }
            bx0_ = std::min({bx0_, a.x, b.x});
            by0_ = std::min({by0_, a.y, b.y});
            bx1_ = std::max({bx1_, a.x, b.x});
            by1_ = std::max({by1_, a.y, b.y});
            if (a.y == b.y) {
                continue;
            }
            VPt p = a, q = b;
            int32_t dir = 1;
            if (!(a.y < b.y)) {
                p = b;
                q = a;
                dir = -1;
            }
            Edge e;
            e.x0 = p.x;
            e.y0 = p.y;
            e.y1 = q.y;
            e.dir = dir;
            e.comp = comp;
            e.dxdy = (q.x - p.x) / (q.y - p.y);
            edges_.push_back(e);
        }
    }
    std::stable_sort(edges_.begin(), edges_.end(),
                     [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
}

bool VectorRasterizer::pixelBounds(int32_t& x0, int32_t& y0, int32_t& x1, int32_t& y1) const {
    if (inverted_ || !hasBounds_) {
        return false;
    }
    constexpr double kMaxI32 = 2147483647.0;
    double fx0 = std::floor(bx0_);
    double fy0 = std::floor(by0_);
    double fx1 = std::min(kMaxI32, std::ceil(bx1_) + 1.0);
    double fy1 = std::min(kMaxI32, std::ceil(by1_) + 1.0);
    if (fx0 >= fx1 || fy0 >= fy1 || fx0 < -kMaxI32 || fy0 < -kMaxI32) {
        return false;
    }
    x0 = static_cast<int32_t>(fx0);
    y0 = static_cast<int32_t>(fy0);
    x1 = static_cast<int32_t>(fx1);
    y1 = static_cast<int32_t>(fy1);
    return x0 < x1 && y0 < y1;
}

bool VectorRasterizer::inside(const int32_t* wind, int32_t unionCount) const {
    if (unionOnly_) {
        return (unionCount > 0) != inverted_;
    }
    bool acc = false;
    for (size_t i = 0; i < comps_.size(); i++) {
        bool v = comps_[i].rule == FillRule::NonZero ? wind[i] != 0 : (wind[i] & 1) != 0;
        // PS 语义：首分量恒按 Combine 应用。
        PathOp op = i == 0 ? PathOp::Combine : comps_[i].op;
        switch (op) {
            case PathOp::Combine:
            case PathOp::Join:
                acc = acc || v;
                break;
            case PathOp::Subtract:
                acc = acc && !v;
                break;
            case PathOp::Intersect:
                acc = acc && v;
                break;
            case PathOp::Exclude:
                acc = acc != v;
                break;
        }
    }
    return acc != inverted_;
}

bool VectorRasterizer::compInside(size_t comp, int32_t w) const {
    return comps_[comp].rule == FillRule::NonZero ? w != 0 : (w & 1) != 0;
}

namespace {

// 覆盖度 h 加到整行（边界在矩形左侧的情形）。
inline void addFull(float* acc, double h) {
    acc[0] += static_cast<float>(h);
}

// 把段 (xa,ya)→(xb,yb)（ya<yb，同处一个像素行）右侧面积 × w 加进 cell 累加器
//（cell 0 = 列 x0）。
void draw(float* acc, size_t accLen, int32_t x0, double xa, double ya, double xb, double yb, double w) {
    double width = static_cast<double>(accLen - 1);
    double left = static_cast<double>(x0);
    double ua = xa - left;
    double ub = xb - left;
    double dy = yb - ya;
    if (dy <= 0.0) {
        return;
    }
    if (ua > ub) {
        std::swap(ua, ub);
    }
    double dx = ub - ua;
    auto hOf = [&](double u0, double u1) { return dx > 1e-12 ? dy * (u1 - u0) / dx : dy; };
    // 矩形左侧部分：每列全覆盖。
    if (ua < 0.0) {
        double cut = std::min(ub, 0.0);
        acc[0] += static_cast<float>(w * hOf(ua, cut));
        if (ub <= 0.0) {
            return;
        }
        ua = 0.0;
    }
    if (ua >= width) {
        return;
    }
    double end = std::min(ub, width);
    if (dx <= 1e-12) {
        double c = std::floor(ua);
        double f = ua - c;
        size_t ci = static_cast<size_t>(c);
        acc[ci] += static_cast<float>(w * dy * (1.0 - f));
        acc[ci + 1] += static_cast<float>(w * dy * f);
        return;
    }
    double u = ua;
    while (u < end) {
        double c = std::floor(u);
        double v = std::min(c + 1.0, end);
        double h = hOf(u, v);
        double m = 0.5 * (u + v) - c;
        size_t ci = static_cast<size_t>(c);
        acc[ci] += static_cast<float>(w * h * (1.0 - m));
        acc[ci + 1] += static_cast<float>(w * h * m);
        if (v <= u) {
            break;
        }
        u = v;
    }
}

}  // namespace

void VectorRasterizer::band(double a, double b, int32_t x0, RowState& st, uint32_t depth) const {
    if (b - a <= 1e-12) {
        return;
    }
    st.band.clear();
    double mid = 0.5 * (a + b);
    for (size_t i : st.active) {
        const Edge& e = edges_[i];
        if (e.y0 <= mid && e.y1 >= mid) {
            double xa = e.xAt(a);
            double xb = e.xAt(b);
            st.band.push_back({xa, xb, 0.5 * (xa + xb), static_cast<uint32_t>(i)});
        }
    }
    if (st.band.empty()) {
        if (inverted_) {
            addFull(st.acc.data(), b - a);
        }
        return;
    }
    std::sort(st.band.begin(), st.band.end(), [](const BandEdge& p, const BandEdge& q) { return p.xm < q.xm; });
    // 带顶/带底处次序反转 = 两边在带内交叉：在交点处切开，让每段内次序恒定。
    if (depth < 24) {
        for (size_t k = 1; k < st.band.size(); k++) {
            const BandEdge& p = st.band[k - 1];
            const BandEdge& q = st.band[k];
            double top = p.xa - q.xa;
            double bot = p.xb - q.xb;
            if (top > 1e-9 || bot > 1e-9) {
                double t = std::fabs(top - bot) > 1e-15 ? top / (top - bot) : 0.5;
                double y = a + (b - a) * std::min(std::max(t, 0.0), 1.0);
                if (y <= a + 1e-9 || y >= b - 1e-9) {
                    y = mid;
                }
                band(a, y, x0, st, depth + 1);
                band(y, b, x0, st, depth + 1);
                return;
            }
        }
    }
    std::fill(st.wind.begin(), st.wind.end(), 0);
    int32_t unionCount = 0;
    bool in = inverted_;
    if (in) {
        addFull(st.acc.data(), b - a);
    }
    size_t n = st.band.size();
    for (size_t k = 0; k < n; k++) {
        const BandEdge be = st.band[k];
        const Edge& e = edges_[be.idx];
        size_t c = e.comp;
        if (unionOnly_) {
            bool was = compInside(c, st.wind[c]);
            st.wind[c] += e.dir;
            bool now = compInside(c, st.wind[c]);
            unionCount += (now ? 1 : 0) - (was ? 1 : 0);
        } else {
            st.wind[c] += e.dir;
        }
        bool now = inside(st.wind.data(), unionCount);
        if (now != in) {
            double w = now ? 1.0 : -1.0;
            draw(st.acc.data(), st.acc.size(), x0, be.xa, a, be.xb, b, w);
            in = now;
        }
    }
}

void VectorRasterizer::renderInto(int32_t rx0, int32_t ry0, int32_t rw, int32_t rh, float* out) const {
    size_t w = static_cast<size_t>(rw);
    RowState st(comps_.size(), w);
    double top = static_cast<double>(ry0);
    // edges_ 按 y0 排序；next = 首个未激活边。
    size_t next = static_cast<size_t>(
        std::partition_point(edges_.begin(), edges_.end(), [&](const Edge& e) { return e.y0 < top; }) -
        edges_.begin());
    for (size_t i = 0; i < next; i++) {
        if (edges_[i].y1 > top) {
            st.active.push_back(i);
        }
    }
    for (int32_t row = 0; row < rh; row++) {
        int32_t y = ry0 + row;
        double ya = static_cast<double>(y);
        double yb = ya + 1.0;
        st.active.erase(std::remove_if(st.active.begin(), st.active.end(),
                                       [&](size_t i) { return edges_[i].y1 <= ya; }),
                        st.active.end());
        while (next < edges_.size() && edges_[next].y0 < yb) {
            if (edges_[next].y1 > ya) {
                st.active.push_back(next);
            }
            next++;
        }
        std::fill(st.acc.begin(), st.acc.end(), 0.0f);
        float* rowOut = out + static_cast<size_t>(row) * w;
        if (st.active.empty()) {
            float v = inverted_ ? 1.0f : 0.0f;
            std::fill(rowOut, rowOut + w, v);
            continue;
        }
        // 子带边界：行端 + 行内的每条边端点。
        st.cuts.clear();
        st.cuts.push_back(ya);
        st.cuts.push_back(yb);
        for (size_t i : st.active) {
            const Edge& e = edges_[i];
            if (e.y0 > ya && e.y0 < yb) {
                st.cuts.push_back(e.y0);
            }
            if (e.y1 > ya && e.y1 < yb) {
                st.cuts.push_back(e.y1);
            }
        }
        std::sort(st.cuts.begin(), st.cuts.end());
        st.cuts.erase(std::unique(st.cuts.begin(), st.cuts.end()), st.cuts.end());
        for (size_t ci = 0; ci + 1 < st.cuts.size(); ci++) {
            band(st.cuts[ci], st.cuts[ci + 1], rx0, st, 0);
        }
        float sum = 0.0f;
        for (size_t x = 0; x < w; x++) {
            sum += st.acc[x];
            // 反号边界在 f32 里不完全抵消，形状右缘外留 ~1e-7 残差会被当成内容；
            // 钳到 0（NaN 同）。
            rowOut[x] = sum >= kRasterResidue ? std::min(sum, 1.0f) : 0.0f;
        }
    }
}

std::vector<float> VectorRasterizer::render(int32_t rx0, int32_t ry0, int32_t rx1, int32_t ry1) const {
    int32_t rw = rx1 - rx0;
    int32_t rh = ry1 - ry0;
    std::vector<float> out(rw > 0 && rh > 0 ? static_cast<size_t>(rw) * static_cast<size_t>(rh) : 0, 0.0f);
    if (rw > 0 && rh > 0) {
        renderInto(rx0, ry0, rw, rh, out.data());
    }
    return out;
}

VectorRasterizer fillRasterizer(const VectorPath& path, double tol) {
    VectorRasterizer r(path.inverted);
    for (auto& comp : path.components()) {
        std::vector<std::vector<VPt>> polys;
        for (size_t i = comp.first; i < comp.second; i++) {
            polys.push_back(flattenSubpath(path.subpaths[i], tol).pts);
        }
        r.addComponent(polys, path.effectiveOp(comp.first), path.fillRule);
    }
    return r;
}

VectorRasterizer strokeRasterizer(const VectorPath& path, double width, LineCap cap, LineJoin join,
                                  double miterLimit, const std::vector<double>& dashesPx,
                                  double dashOffsetPx, double tol) {
    StrokeStyle style;
    style.width = width;
    style.cap = cap;
    style.join = join;
    style.miterLimit = miterLimit;
    style.dashes = dashesPx;
    style.dashOffset = dashOffsetPx;
    // 描边轮廓用更粗的展平容差：(tol×4, 宽度×0.1, 1e-3) 取小再兜底。
    double tolStroke = std::max(1e-3, std::min(tol * 4.0, std::max(style.width, 0.01) * 0.1));
    std::vector<Polyline> lines = flattenPath(path, tolStroke);
    std::vector<std::vector<VPt>> polys = strokePolygons(lines, style, tol);
    VectorRasterizer r(false);
    r.addComponent(polys, PathOp::Combine, FillRule::NonZero);
    return r;
}

}  // namespace montage
