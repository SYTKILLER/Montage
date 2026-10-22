// M20b 液化内核 —— photocraft crates/algo/src/liquify.rs 全量 C++ 翻译（MIT OR Apache-2.0）。
// 笔画游标跨段携带 dab 间距（快慢指针同场 #2606）、可达跨度剪枝（远点段不死循环 #937）、
// dab 前快照采样（新旧场混读）、奇偶规则扫描线套索冻结、边缘零斜率衰减（撕图 #2606 修复）。

#include "engine/liquify_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace montage {

namespace {
constexpr double kPi = 3.14159265358979323846;

RectI rectNew(int x0, int y0, int x1, int y1) {
    RectI r;
    r.x0 = x0;
    r.y0 = y0;
    r.x1 = x1;
    r.y1 = y1;
    return r;
}

RectI rectEmpty() {
    return rectNew(0, 0, 0, 0);
}

bool rectIsEmpty(const RectI& r) {
    return r.x1 <= r.x0 || r.y1 <= r.y0;
}

RectI rectUnion(const RectI& a, const RectI& b) {
    if (rectIsEmpty(a)) {
        return b;
    }
    if (rectIsEmpty(b)) {
        return a;
    }
    return rectNew(std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1));
}

RectI rectIntersect(const RectI& a, const RectI& b) {
    return rectNew(std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1));
}

bool isStationary(liquify::Tool t) {
    switch (t) {
        case liquify::Tool::ForwardWarp:
        case liquify::Tool::PushLeft:
        case liquify::Tool::LassoMask:
            return false;
        default:
            return true;
    }
}

bool isGlobal(liquify::Tool t) {
    return t == liquify::Tool::ReconstructAll || t == liquify::Tool::FreezeAll ||
           t == liquify::Tool::ThawAll || t == liquify::Tool::InvertFreeze;
}

// 笔刷半径 r 的 dab 间距：半径的 1/20（拖拽=许多小推，边缘平滑）
double dabSpacing(double r) {
    return std::max(r * 0.05, 0.5);
}

// 定点工具移动时每 dab 强度：按间距缩放（每 traveling 距离的效果与 1/5 半径 dab 一致）
double movingWeight(double r) {
    return dabSpacing(r) / std::max(r * 0.2, 0.5);
}

// 场坐标双线性（出界 [−1,w]×[−1,h] 外为 0；photocraft bilinear）
std::array<double, 2> bilinear2(const std::vector<std::array<float, 2>>& d, int w, int h,
                                double gx, double gy) {
    if (!(gx > -1.0 && gy > -1.0 && gx < static_cast<double>(w) && gy < static_cast<double>(h))) {
        return {0.0, 0.0};
    }
    gx = std::clamp(gx, 0.0, static_cast<double>(w - 1));
    gy = std::clamp(gy, 0.0, static_cast<double>(h - 1));
    const int i = std::min(static_cast<int>(gx), std::max(0, w - 2));
    const int j = std::min(static_cast<int>(gy), std::max(0, h - 2));
    const double fx = gx - i;
    const double fy = gy - j;
    const int i1 = std::min(i + 1, w - 1);
    const int j1 = std::min(j + 1, h - 1);
    const auto at = [&](int x, int y) -> const std::array<float, 2>& { return d[static_cast<size_t>(y) * w + x]; };
    const auto& a = at(i, j);
    const auto& b = at(i1, j);
    const auto& c = at(i, j1);
    const auto& e = at(i1, j1);
    return {(static_cast<double>(a[0]) * (1.0 - fx) + static_cast<double>(b[0]) * fx) * (1.0 - fy) +
                (static_cast<double>(c[0]) * (1.0 - fx) + static_cast<double>(e[0]) * fx) * fy,
            (static_cast<double>(a[1]) * (1.0 - fx) + static_cast<double>(b[1]) * fx) * (1.0 - fy) +
                (static_cast<double>(c[1]) * (1.0 - fx) + static_cast<double>(e[1]) * fx) * fy};
}

// Catmull-Rom 4 点权重
std::array<double, 4> catmullRom(double t) {
    const double t2 = t * t;
    const double t3 = t2 * t;
    return {-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t,
            0.5 * t3 - 0.5 * t2};
}

}  // namespace

namespace liquify {

Field::Field(const RectI& bounds_, double cell_) {
    bounds = bounds_;
    cell = std::clamp(cell_, 1.0, 64.0);
    w = static_cast<int>(std::ceil(static_cast<double>(bounds.width()) / cell)) + 2;
    h = static_cast<int>(std::ceil(static_cast<double>(bounds.height()) / cell)) + 2;
    d.assign(static_cast<size_t>(w) * h, {0.0f, 0.0f});
    freeze.assign(static_cast<size_t>(w) * h, 0.0f);
}

std::array<double, 2> Field::nodePos(int i, int j) const {
    return {static_cast<double>(bounds.x0) + 0.5 + static_cast<double>(i) * cell,
            static_cast<double>(bounds.y0) + 0.5 + static_cast<double>(j) * cell};
}

std::pair<double, double> Field::toGrid(double x, double y) const {
    return {(x - static_cast<double>(bounds.x0) - 0.5) / cell,
            (y - static_cast<double>(bounds.y0) - 0.5) / cell};
}

bool Field::isIdentity() const {
    for (const auto& v : d) {
        if (v[0] != 0.0f || v[1] != 0.0f) {
            return false;
        }
    }
    return true;
}

float Field::maxDisplacement() const {
    float m = 0.0f;
    for (const auto& v : d) {
        m = std::max(m, std::hypot(v[0], v[1]));
    }
    return m;
}

std::array<double, 2> Field::sample(double x, double y) const {
    const auto [gx, gy] = toGrid(x, y);
    return bilinear2(d, w, h, gx, gy);
}

float Field::freezeAt(double x, double y) const {
    const auto [gxn, gyn] = toGrid(x, y);
    const double gx = std::clamp(gxn, 0.0, static_cast<double>(w - 1));
    const double gy = std::clamp(gyn, 0.0, static_cast<double>(h - 1));
    const int i = std::min(static_cast<int>(gx), w - 2);
    const int j = std::min(static_cast<int>(gy), h - 2);
    const float fx = static_cast<float>(gx - i);
    const float fy = static_cast<float>(gy - j);
    const auto at = [&](int x2, int y2) -> float { return freeze[static_cast<size_t>(y2) * w + x2]; };
    return (at(i, j) * (1.0f - fx) + at(i + 1, j) * fx) * (1.0f - fy) +
           (at(i, j + 1) * (1.0f - fx) + at(i + 1, j + 1) * fx) * fy;
}

bool Field::sameAs(const Field& o) const {
    return bounds == o.bounds && cell == o.cell && w == o.w && h == o.h && d == o.d && freeze == o.freeze;
}

// 奇偶规则扫描线套索：把场节点行逐行求多边形交点后填充（photocraft apply_lasso）
RectI applyLasso(Field& f, const Stroke& s) {
    std::vector<std::array<double, 2>> poly;
    for (const auto& p : s.points) {
        if (std::isfinite(p[0]) && std::isfinite(p[1])) {
            poly.push_back({p[0], p[1]});
        }
    }
    if (poly.size() < 3 || f.w == 0 || f.h == 0) {
        return rectEmpty();
    }
    double px0 = 1e308, py0 = 1e308, px1 = -1e308, py1 = -1e308;
    for (const auto& p : poly) {
        px0 = std::min(px0, p[0]);
        py0 = std::min(py0, p[1]);
        px1 = std::max(px1, p[0]);
        py1 = std::max(py1, p[1]);
    }
    const double gy0 = f.toGrid(px0, py0).second;
    const double gy1 = f.toGrid(px1, py1).second;
    const double lastRow = static_cast<double>(std::max(0, f.h - 1));
    const double lastCol = static_cast<double>(std::max(0, f.w - 1));
    const double j0d = std::max(0.0, std::ceil(gy0));
    const double j1d = std::min(lastRow, std::floor(gy1));
    if (j1d < j0d) {
        return rectEmpty();
    }
    const float v = (s.hasAmount ? s.amount : 1.0) > 0.5 ? 1.0f : 0.0f;
    std::vector<double> xs;
    for (int j = static_cast<int>(j0d); j <= static_cast<int>(j1d); ++j) {
        const double y = f.nodePos(0, j)[1];
        xs.clear();
        std::array<double, 2> prev = poly.back();
        for (const auto& a : poly) {
            if ((a[1] > y) != (prev[1] > y)) {
                xs.push_back(a[0] + (y - a[1]) * (prev[0] - a[0]) / (prev[1] - a[1]));
            }
            prev = a;
        }
        std::sort(xs.begin(), xs.end());
        // 奇偶规则：xs[2k] <= x < xs[2k+1] 为内部
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const double ga = f.toGrid(xs[k], y).first;
            const double gb = f.toGrid(xs[k + 1], y).first;
            const double i0d = std::max(0.0, std::ceil(ga));
            const double i1d = std::min(lastCol, std::ceil(gb) - 1.0);
            if (i1d < i0d) {
                continue;
            }
            const size_t row = static_cast<size_t>(j) * f.w;
            std::fill(f.freeze.begin() + row + static_cast<size_t>(i0d),
                      f.freeze.begin() + row + static_cast<size_t>(i1d) + 1, v);
        }
    }
    return rectIntersect(rectNew(static_cast<int>(std::floor(px0)), static_cast<int>(std::floor(py0)),
                                 static_cast<int>(std::ceil(px1)), static_cast<int>(std::ceil(py1))),
                         f.bounds);
}

// 可达跨度：段上能触到节点网格（笔刷半径+2 节点）的距离区间（photocraft reachable_span）
bool reachableSpan(const Field& f, const std::array<double, 3>& a, const std::array<double, 2>& dir,
                   double len, double r, double out[2]) {
    if (f.w == 0 || f.h == 0) {
        return false;
    }
    const double pad = r + 2.0 * f.cell;
    const double lo[2] = {static_cast<double>(f.bounds.x0) + 0.5 - pad,
                          static_cast<double>(f.bounds.y0) + 0.5 - pad};
    const double hi[2] = {lo[0] + static_cast<double>(f.w - 1) * f.cell + 2.0 * pad,
                          lo[1] + static_cast<double>(f.h - 1) * f.cell + 2.0 * pad};
    double u0 = 0.0;
    double u1 = 1.0;
    for (int ax = 0; ax < 2; ++ax) {
        const double dd = dir[ax];
        if (dd == 0.0) {
            if (std::isnan(a[ax]) || a[ax] < lo[ax] || a[ax] > hi[ax]) {
                return false;
            }
            continue;
        }
        const double v0 = (lo[ax] - a[ax]) / dd;
        const double v1 = (hi[ax] - a[ax]) / dd;
        u0 = std::max(u0, std::min(v0, v1));
        u1 = std::min(u1, std::max(v0, v1));
    }
    if (std::isnan(u0) || std::isnan(u1) || u0 > u1) {
        return false;
    }
    out[0] = u0 * len;
    out[1] = u1 * len;
    return true;
}

// 一次 dab（photocraft dab）：受影响节点快照 + 新场写入；返回影响文档矩形
RectI dabAt(Field& f, const Stroke& s, const std::array<double, 2>& c,
            const std::array<double, 2>& delta, double pointPressure, double weight) {
    if (s.tool == Tool::LassoMask) {
        return rectEmpty();  // 套索只吃多边形，绝无笔刷 dab
    }
    const double r = std::max(s.size / 2.0, 0.5);
    const double strength = std::clamp(s.pressure / 100.0, 0.0, 1.0) * pointPressure * weight;
    const double rate = std::clamp(s.rate / 100.0, 0.0, 1.0);
    const double density = std::clamp(s.density / 100.0, 0.0, 1.0);
    if (strength <= 0.0) {
        return rectEmpty();
    }
    const auto [gx0v, gy0v] = f.toGrid(c[0] - r, c[1] - r);
    const auto [gx1v, gy1v] = f.toGrid(c[0] + r, c[1] + r);
    const int i0 = static_cast<int>(std::max(0.0, std::floor(gx0v)));
    const int j0 = static_cast<int>(std::max(0.0, std::floor(gy0v)));
    const int i1 = std::min(static_cast<int>(std::ceil(std::max(-1.0, gx1v))), f.w - 1);
    const int j1 = std::min(static_cast<int>(std::ceil(std::max(-1.0, gy1v))), f.h - 1);
    if (i1 < i0 || j1 < j0) {
        return rectEmpty();
    }
    // 受影响节点快照（含余量；采样会读到周围）
    const int m = static_cast<int>(std::ceil((std::max(std::fabs(delta[0]), std::fabs(delta[1])) + r * 0.2) / f.cell)) + 2;
    const int si0 = std::max(0, i0 - m);
    const int sj0 = std::max(0, j0 - m);
    const int si1 = std::min(i1 + m, f.w - 1);
    const int sj1 = std::min(j1 + m, f.h - 1);
    const int sw = si1 - si0 + 1;
    const int sh = sj1 - sj0 + 1;
    std::vector<std::array<float, 2>> snap(static_cast<size_t>(sw) * sh);
    for (int j = sj0; j <= sj1; ++j) {
        std::memcpy(&snap[static_cast<size_t>(j - sj0) * sw], &f.d[static_cast<size_t>(j) * f.w + si0],
                    static_cast<size_t>(sw) * sizeof(std::array<float, 2>));
    }
    const auto old = [&](double gx, double gy) -> std::array<double, 2> {
        const double lx = gx - si0;
        const double ly = gy - sj0;
        if (lx >= 0.0 && ly >= 0.0 && lx <= sw - 1 && ly <= sh - 1) {
            return bilinear2(snap, sw, sh, lx, ly);
        }
        // 快照外无变化：读活场
        return bilinear2(f.d, f.w, f.h, gx, gy);
    };
    struct Write {
        int idx;
        std::array<float, 2> d;
        float f;
    };
    std::vector<Write> writes;
    writes.reserve(static_cast<size_t>(i1 - i0 + 1) * (j1 - j0 + 1));
    const double invCell = 1.0 / f.cell;
    for (int j = j0; j <= j1; ++j) {
        for (int i = i0; i <= i1; ++i) {
            const std::array<double, 2> p = f.nodePos(i, j);
            const double dx = p[0] - c[0];
            const double dy = p[1] - c[1];
            const double t2 = (dx * dx + dy * dy) / (r * r);
            if (t2 >= 1.0) {
                continue;
            }
            const double fall = falloff(t2, density);
            const int idx = j * f.w + i;
            const double kRaw = strength * fall;
            if (s.tool == Tool::Freeze || s.tool == Tool::Thaw) {
                const double fv = f.freeze[static_cast<size_t>(idx)];
                const double v = s.tool == Tool::Freeze ? std::min(fv + kRaw, 1.0) : std::max(fv - kRaw, 0.0);
                writes.push_back({idx, f.d[static_cast<size_t>(idx)], static_cast<float>(v)});
                continue;
            }
            const double k = kRaw * (1.0 - static_cast<double>(f.freeze[static_cast<size_t>(idx)]));
            if (k <= 0.0) {
                continue;
            }
            const std::array<float, 2> cur = f.d[static_cast<size_t>(idx)];
            std::array<double, 2> nv;
            switch (s.tool) {
                case Tool::Reconstruct: {
                    const double a = 1.0 - k * (0.1 + 0.4 * rate);
                    nv = {static_cast<double>(cur[0]) * a, static_cast<double>(cur[1]) * a};
                    break;
                }
                case Tool::Smooth: {
                    const auto at = [&](int x, int y) -> const std::array<float, 2>& {
                        const int cx = std::clamp(x, 0, sw - 1);
                        const int cy = std::clamp(y, 0, sh - 1);
                        return snap[static_cast<size_t>(cy) * sw + cx];
                    };
                    double mean[2] = {0.0, 0.0};
                    const int offs[8][2] = {{-1, 0}, {1, 0},  {0, -1}, {0, 1},
                                            {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
                    for (const auto& o : offs) {
                        const auto& v = at(i - si0 + o[0], j - sj0 + o[1]);
                        mean[0] += static_cast<double>(v[0]) / 8.0;
                        mean[1] += static_cast<double>(v[1]) / 8.0;
                    }
                    const double a = k * (0.1 + 0.4 * rate);
                    nv = {static_cast<double>(cur[0]) + (mean[0] - cur[0]) * a,
                          static_cast<double>(cur[1]) + (mean[1] - cur[1]) * a};
                    break;
                }
                default: {
                    std::array<double, 2> q;
                    switch (s.tool) {
                        case Tool::ForwardWarp:
                            q = {p[0] - k * delta[0], p[1] - k * delta[1]};
                            break;
                        case Tool::PushLeft:  // 行进方向左侧（y 向下）：(dx,dy) → (dy,−dx)
                            q = {p[0] - k * delta[1], p[1] + k * delta[0]};
                            break;
                        case Tool::TwirlCw:
                        case Tool::TwirlCcw: {
                            // 内容转 +th，采样在转 −th 的点
                            const double th = k * rate * 0.12 * (s.tool == Tool::TwirlCw ? 1.0 : -1.0);
                            const double sn = std::sin(-th);
                            const double cs = std::cos(-th);
                            q = {c[0] + dx * cs - dy * sn, c[1] + dx * sn + dy * cs};
                            break;
                        }
                        case Tool::Pucker:
                            q = {c[0] + dx * (1.0 + k * rate * 0.06), c[1] + dy * (1.0 + k * rate * 0.06)};
                            break;
                        default:  // Bloat
                            q = {c[0] + dx * (1.0 - k * rate * 0.06), c[1] + dy * (1.0 - k * rate * 0.06)};
                            break;
                    }
                    const double qx = (q[0] - p[0]) * invCell + i;
                    const double qy = (q[1] - p[1]) * invCell + j;
                    const std::array<double, 2> dq = old(qx, qy);
                    nv = {q[0] + dq[0] - p[0], q[1] + dq[1] - p[1]};
                    break;
                }
            }
            writes.push_back({idx, {static_cast<float>(nv[0]), static_cast<float>(nv[1])},
                              f.freeze[static_cast<size_t>(idx)]});
        }
    }
    for (const auto& wr : writes) {
        f.d[static_cast<size_t>(wr.idx)] = wr.d;
        f.freeze[static_cast<size_t>(wr.idx)] = wr.f;
    }
    const int pad = static_cast<int>(std::ceil(r)) + 2;
    return rectIntersect(
        rectNew(static_cast<int>(std::floor(c[0])) - pad, static_cast<int>(std::floor(c[1])) - pad,
                static_cast<int>(std::ceil(c[0])) + pad, static_cast<int>(std::ceil(c[1])) + pad),
        f.bounds);
}

// ---- 笔画应用（photocraft apply_stroke/stroke_begin/walk_segment 直迁）----

namespace {
RectI walkSegmentL(Field& f, const Stroke& s, const std::array<double, 3>& a,
                   const std::array<double, 3>& b, bool skip);
}


RectI Field::applyStroke(const Stroke& s) {
    cursor = Cursor{};
    switch (s.tool) {
        case Tool::LassoMask:
            return applyLasso(*this, s);
        case Tool::ReconstructAll: {
            const double k = std::clamp((s.hasAmount ? s.amount : 100.0) / 100.0, 0.0, 1.0);
            for (size_t i = 0; i < d.size(); ++i) {
                const double m = 1.0 - k * (1.0 - static_cast<double>(freeze[i]));
                d[i][0] = static_cast<float>(static_cast<double>(d[i][0]) * m);
                d[i][1] = static_cast<float>(static_cast<double>(d[i][1]) * m);
            }
            return bounds;
        }
        case Tool::FreezeAll:
            std::fill(freeze.begin(), freeze.end(), 1.0f);
            break;
        case Tool::ThawAll:
            std::fill(freeze.begin(), freeze.end(), 0.0f);
            break;
        case Tool::InvertFreeze:
            for (float& fv : freeze) {
                fv = 1.0f - fv;
            }
            break;
        default:
            break;
    }
    if (isGlobal(s.tool)) {
        return bounds;
    }
    if (s.points.empty()) {
        return rectEmpty();
    }
    RectI dirty = strokeBegin(s, s.points.front());
    for (size_t i = 1; i < s.points.size(); ++i) {
        dirty = rectUnion(dirty, strokeSegment(s, s.points[i - 1], s.points[i]));
    }
    return dirty;
}

RectI Field::strokeBegin(const Stroke& s, const std::array<double, 3>& p) {
    cursor = Cursor{};
    cursor.valid = true;
    cursor.end = {p[0], p[1]};
    cursor.lastDab = {p[0], p[1]};
    cursor.carry = 0.0;
    if (isStationary(s.tool) && !isGlobal(s.tool)) {
        return dabAt(*this, s, {p[0], p[1]}, {0.0, 0.0}, std::clamp(p[2], 0.0, 1.0), 1.0);
    }
    return rectEmpty();
}

RectI Field::strokeSegment(const Stroke& s, const std::array<double, 3>& a,
                           const std::array<double, 3>& b) {
    return walkSegmentL(*this, s, a, b, true);
}

RectI Field::strokeSegmentFull(const Stroke& s, const std::array<double, 3>& a,
                               const std::array<double, 3>& b) {
    return walkSegmentL(*this, s, a, b, false);
}

namespace {
// walk_segment 本体：skip=true 剪掉触不到场的 dab（#937；宿主测试对比全行走）
RectI walkSegmentL(Field& f, const Stroke& s, const std::array<double, 3>& a,
                   const std::array<double, 3>& b, bool skip) {
    RectI dirty = rectEmpty();
    const double pa = std::clamp(a[2], 0.0, 1.0);
    const double pb = std::clamp(b[2], 0.0, 1.0);
    const bool stationary = isStationary(s.tool);
    Cursor cur;
    if (f.cursor.valid && f.cursor.end == std::array<double, 2>{a[0], a[1]}) {
        cur = f.cursor;
    } else {
        cur = Cursor{};
        cur.valid = true;
        cur.end = {a[0], a[1]};
        cur.lastDab = {a[0], a[1]};
        cur.carry = 0.0;
    }
    const double dx = b[0] - a[0];
    const double dy = b[1] - a[1];
    const double len = std::hypot(dx, dy);
    if (len == 0.0) {
        f.cursor = cur;
        return stationary ? dabAt(f, s, {b[0], b[1]}, {0.0, 0.0}, pb, 1.0) : dirty;
    }
    // NaN 或长度溢出：无意义行走；下一段重新起头
    if (!std::isfinite(len)) {
        f.cursor = Cursor{};
        return dirty;
    }
    const double r = std::max(s.size / 2.0, 0.5);
    const double spacing = dabSpacing(r);
    const double weight = stationary ? movingWeight(r) : 1.0;
    // 本段第一个 dab 的距离，之后每 spacing 一个
    const double first = std::clamp(spacing - cur.carry, 0.0, spacing);
    // 距离比较留一点余量：舍入使 dab 稍过段尾仍算落在段上（切分段不增不减 dab）
    const double slack = spacing * 1e-6;
    if (first > len + slack) {
        f.cursor = cur;
        f.cursor.end = {b[0], b[1]};
        f.cursor.carry = cur.carry + len;
        return dirty;
    }
    const double lastK = std::max(0.0, std::floor((len - first + slack) / spacing));
    const auto at = [&](double dist) -> std::array<double, 2> {
        return {a[0] + dx * dist / len, a[1] + dy * dist / len};
    };
    double k0 = 0.0;
    double k1 = lastK;
    if (skip) {
        double span[2] = {0.0, 0.0};
        const bool ok = reachableSpan(f, a, {dx, dy}, len, r, span);
        if (ok) {
            // 拖拽 dab 中心在其位置后一个 spacing：加该量与一步的余量
            k0 = std::max(k0, std::floor((span[0] - spacing - first) / spacing) - 1.0);
            k1 = std::min(k1, std::ceil((span[1] + spacing - first) / spacing) + 1.0);
        } else {
            k1 = -1.0;
        }
    }
    if (k0 <= k1) {
        std::array<double, 2> prev =
            k0 > 0.0 ? at(first + (k0 - 1.0) * spacing) : cur.lastDab;
        for (long long k = static_cast<long long>(k0); k <= static_cast<long long>(k1); ++k) {
            const double dist = std::min(first + static_cast<double>(k) * spacing, len);
            const std::array<double, 2> c = at(dist);
            const std::array<double, 2> delta = {c[0] - prev[0], c[1] - prev[1]};
            // Forward Warp 的 dab 中心在笔刷原处（它拖着下面的像素走）
            const std::array<double, 2> centre = stationary ? c : prev;
            dirty = rectUnion(dirty, dabAt(f, s, centre, delta, pa + (pb - pa) * dist / len, weight));
            prev = c;
        }
    }
    const double last = std::min(first + lastK * spacing, len);
    f.cursor = Cursor{};
    f.cursor.valid = true;
    f.cursor.end = {b[0], b[1]};
    f.cursor.lastDab = at(last);
    f.cursor.carry = std::clamp(len - last, 0.0, spacing);
    return dirty;
}
}  // namespace

Field fromStrokes(const RectI& bounds, double cell, const std::vector<Stroke>& strokes) {
    Field f(bounds, cell);
    for (const Stroke& s : strokes) {
        f.applyStroke(s);
    }
    return f;
}

// ---- 渲染（photocraft apply_liquify 直迁；稠密 f32 RGBA 源）----

Img applyLiquifyImage(const Img& src, const Field& field, int bitDepth) {
    Img out = src;
    const RectI b = field.bounds;
    if (field.isIdentity() || rectIsEmpty(b)) {
        return out;
    }
    const int maxd = static_cast<int>(std::ceil(static_cast<double>(field.maxDisplacement()))) + 3;
    auto workTile = [&](const RectI& t) -> void {
        const int tw = t.width();
        const int th = t.height();
        // 每像素场位移（Catmull-Rom），行共享 y 权重
        std::vector<std::array<double, 2>> disp(static_cast<size_t>(tw) * th);
        double fx0 = 1e308, fy0 = 1e308, fx1 = -1e308, fy1 = -1e308;
        bool moved = false;
        for (int y = 0; y < th; ++y) {
            const double py = static_cast<double>(t.y0) + y + 0.5;
            const double gy = (py - static_cast<double>(b.y0) - 0.5) / field.cell;
            const std::array<double, 4> wy = catmullRom(gy - std::floor(gy));
            for (int x = 0; x < tw; ++x) {
                const double px = static_cast<double>(t.x0) + x + 0.5;
                const double gx = (px - static_cast<double>(b.x0) - 0.5) / field.cell;
                const std::array<double, 4> wx = catmullRom(gx - std::floor(gx));
                // sample_cubic
                const int ix = static_cast<int>(std::floor(gx));
                const int iy = static_cast<int>(std::floor(gy));
                std::array<double, 2> dd{0.0, 0.0};
                for (int jj = 0; jj < 4; ++jj) {
                    const int cy = std::clamp(iy - 1 + jj, 0, field.h - 1);
                    for (int ii = 0; ii < 4; ++ii) {
                        const int cx = std::clamp(ix - 1 + ii, 0, field.w - 1);
                        const double k = wx[ii] * wy[jj];
                        const std::array<float, 2>& v = field.d[static_cast<size_t>(cy) * field.w + cx];
                        dd[0] += static_cast<double>(v[0]) * k;
                        dd[1] += static_cast<double>(v[1]) * k;
                    }
                }
                if (dd[0] != 0.0 || dd[1] != 0.0) {
                    moved = true;
                }
                disp[static_cast<size_t>(y) * tw + x] = dd;
                const double sx = px + dd[0];
                const double sy = py + dd[1];
                fx0 = std::min(fx0, sx);
                fy0 = std::min(fy0, sy);
                fx1 = std::max(fx1, sx);
                fy1 = std::max(fy1, sy);
            }
        }
        if (!moved) {
            return;
        }
        RectI foot = rectIntersect(
            rectNew(static_cast<int>(std::floor(fx0)) - 3, static_cast<int>(std::floor(fy0)) - 3,
                    static_cast<int>(std::ceil(fx1)) + 3, static_cast<int>(std::ceil(fy1)) + 3),
            rectNew(t.x0 - maxd, t.y0 - maxd, t.x1 + maxd, t.y1 + maxd));
        foot = rectIntersect(foot, b);
        if (rectIsEmpty(foot)) {
            return;
        }
        const int fw = foot.width();
        const int fh = foot.height();
        // 读足迹并预乘
        std::vector<float> px(static_cast<size_t>(fw) * fh * 4u);
        for (int y = 0; y < fh; ++y) {
            std::memcpy(&px[static_cast<size_t>(y) * fw * 4u],
                        &src.px[(static_cast<size_t>(foot.y0 + y) * src.w + foot.x0) * 4u],
                        static_cast<size_t>(fw) * 4u * sizeof(float));
        }
        for (size_t i = 0; i < px.size(); i += 4u) {
            const float al = px[i + 3u];
            px[i] *= al;
            px[i + 1u] *= al;
            px[i + 2u] *= al;
        }
        const auto at = [&](int x, int y) -> size_t {
            const int cx = std::clamp(x, foot.x0, foot.x1 - 1);
            const int cy = std::clamp(y, foot.y0, foot.y1 - 1);
            return (static_cast<size_t>(cy - foot.y0) * fw + (cx - foot.x0)) * 4u;
        };
        const auto snapT = [](double t) -> double {
            const double rr = std::round(t);
            return std::fabs(t - rr) < 1e-7 ? rr : t;
        };
        std::vector<float> outp(static_cast<size_t>(tw) * th * 4u, 0.0f);
        for (int y = 0; y < th; ++y) {
            for (int x = 0; x < tw; ++x) {
                const std::array<double, 2> dd = disp[static_cast<size_t>(y) * tw + x];
                const double u = snapT(static_cast<double>(t.x0) + x + dd[0]);
                const double v = snapT(static_cast<double>(t.y0) + y + dd[1]);
                const double ixd = std::floor(u);
                const double iyd = std::floor(v);
                const std::array<double, 4> wx = catmullRom(u - ixd);
                const std::array<double, 4> wy = catmullRom(v - iyd);
                double acc[4] = {0.0, 0.0, 0.0, 0.0};
                for (int j = 0; j < 4; ++j) {
                    for (int i = 0; i < 4; ++i) {
                        const double k = wx[i] * wy[j];
                        if (k == 0.0) {
                            continue;
                        }
                        const size_t o = at(static_cast<int>(ixd) - 1 + i, static_cast<int>(iyd) - 1 + j);
                        for (int c = 0; c < 4; ++c) {
                            acc[c] += static_cast<double>(px[o + static_cast<size_t>(c)]) * k;
                        }
                    }
                }
                const size_t o = (static_cast<size_t>(y) * tw + x) * 4u;
                const double al = std::clamp(acc[3], 0.0, 1.0);
                outp[o + 3u] = static_cast<float>(al);
                for (int c = 0; c < 3; ++c) {
                    outp[o + static_cast<size_t>(c)] =
                        al > 0.0 ? static_cast<float>(std::clamp(acc[c] / al, 0.0, 1.0)) : 0.0f;
                }
            }
        }
        // 写回 out（按位深度量化，bitDepth<0/32 不量化）
        const double maxV = static_cast<double>((1u << bitDepth) - 1);
        for (int y = 0; y < th; ++y) {
            for (int x = 0; x < tw; ++x) {
                const size_t o = (static_cast<size_t>(y) * tw + x) * 4u;
                float* dst = &out.px[(static_cast<size_t>(t.y0 + y) * out.w + t.x0 + x) * 4u];
                for (int c = 0; c < 4; ++c) {
                    float v = outp[o + static_cast<size_t>(c)];
                    if (bitDepth >= 0 && bitDepth < 32) {
                        v = static_cast<float>(
                            std::round(std::clamp(static_cast<double>(v), 0.0, 1.0) * maxV) / maxV);
                    }
                    dst[c] = v;
                }
            }
        }
    };
    constexpr int kTile = 256;
    const int tx0 = b.x0 / kTile;
    const int ty0 = b.y0 / kTile;
    const int tx1 = (b.x1 + kTile - 1) / kTile;
    const int ty1 = (b.y1 + kTile - 1) / kTile;
    for (int ty = ty0; ty < ty1; ++ty) {
        for (int tx = tx0; tx < tx1; ++tx) {
            RectI t = rectNew(std::max(tx * kTile, b.x0), std::max(ty * kTile, b.y0),
                              std::min((tx + 1) * kTile, b.x1), std::min((ty + 1) * kTile, b.y1));
            if (rectIsEmpty(t)) {
                continue;
            }
            // touches：影响 t 的任一节点有位移才重采样
            const double gxa = field.toGrid(static_cast<double>(t.x0), static_cast<double>(t.y0)).first;
            const double gya = field.toGrid(static_cast<double>(t.x0), static_cast<double>(t.y0)).second;
            const double gxb = field.toGrid(static_cast<double>(t.x1), static_cast<double>(t.y1)).first;
            const double gyb = field.toGrid(static_cast<double>(t.x1), static_cast<double>(t.y1)).second;
            const int i0 = std::clamp(static_cast<int>(std::floor(gxa)) - 2, 0, field.w - 1);
            const int j0 = std::clamp(static_cast<int>(std::floor(gya)) - 2, 0, field.h - 1);
            const int i1 = std::clamp(static_cast<int>(std::ceil(gxb)) + 2, 0, field.w - 1);
            const int j1 = std::clamp(static_cast<int>(std::ceil(gyb)) + 2, 0, field.h - 1);
            bool touch = false;
            for (int j = j0; j <= j1 && !touch; ++j) {
                for (int i = i0; i <= i1; ++i) {
                    const std::array<float, 2>& v = field.d[static_cast<size_t>(j) * field.w + i];
                    if (v[0] != 0.0f || v[1] != 0.0f) {
                        touch = true;
                        break;
                    }
                }
            }
            if (!touch) {
                continue;
            }
            workTile(t);
        }
    }
    return out;
}

double autoCell(const RectI& bounds) {
    const double area = static_cast<double>(bounds.width()) * static_cast<double>(bounds.height());
    return area <= 4200000.0 ? 2.0 : 4.0;
}

double falloff(double t2, double density) {
    t2 = std::clamp(t2, 0.0, 1.0);
    density = std::isnan(density) ? 0.5 : std::clamp(density, 0.0, 1.0);
    // 羽化：(1−t²)³；浓密：(1−t⁴)²，中部平坦。两者在 t=1 处值与斜率皆 0（撕图 #2606 修复）
    const double feathered = std::pow(1.0 - t2, 3.0);
    const double dense = std::pow(1.0 - t2 * t2, 2.0);
    return feathered + (dense - feathered) * density;
}

}  // namespace liquify

// 引擎网格入口：RGBA8 TileGrid ↔ 稠密 f32
std::shared_ptr<const TileGrid> applyLiquifyGrid(const TileGrid& src, const liquify::Field& field) {
    const uint32_t srcW = src.cols * kTileSize;
    const uint32_t srcH = src.rows * kTileSize;
    if (srcW == 0 || srcH == 0) {
        return nullptr;
    }
    liquify::Img in;
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
            float* dd = in.px.data() + (static_cast<size_t>(y) * srcW + x) * 4u;
            dd[0] = static_cast<float>(row[static_cast<size_t>(x) * 4u]) / 255.0f;
            dd[1] = static_cast<float>(row[static_cast<size_t>(x) * 4u + 1u]) / 255.0f;
            dd[2] = static_cast<float>(row[static_cast<size_t>(x) * 4u + 2u]) / 255.0f;
            dd[3] = static_cast<float>(row[static_cast<size_t>(x) * 4u + 3u]) / 255.0f;
        }
    }
    const liquify::Img out = liquify::applyLiquifyImage(in, field, 8);
    const uint32_t outW = static_cast<uint32_t>(out.w);
    const uint32_t outH = static_cast<uint32_t>(out.h);
    TileGridBuilder builder(src.cols, src.rows);
    for (uint32_t y = 0; y < outH; ++y) {
        bool any = false;
        std::vector<uint8_t> outRow(static_cast<size_t>(outW) * 4u);
        for (uint32_t x = 0; x < outW; ++x) {
            const float* sp = out.px.data() + (static_cast<size_t>(y) * outW + x) * 4u;
            uint8_t* d = outRow.data() + static_cast<size_t>(x) * 4u;
            for (int c = 0; c < 4; ++c) {
                d[static_cast<size_t>(c)] =
                    static_cast<uint8_t>(std::lround(std::clamp(sp[c], 0.0f, 1.0f) * 255.0f));
            }
            if (d[3] != 0) {
                any = true;
            }
        }
        if (!any) {
            continue;
        }
        for (uint32_t tx = 0; tx < src.cols; ++tx) {
            bool tileAny = false;
            for (uint32_t k = 0; k < kTileSize; ++k) {
                if (outRow[(static_cast<size_t>(tx) * kTileSize + k) * 4u + 3u] != 0) {
                    tileAny = true;
                    break;
                }
            }
            if (!tileAny) {
                continue;
            }
            uint8_t* dst = builder.ensureTile(tx, y / kTileSize).mapCpuWrite() +
                           (y % kTileSize) * kTileSize * 4u;
            std::memcpy(dst, outRow.data() + static_cast<size_t>(tx) * kTileSize * 4u,
                        static_cast<size_t>(kTileSize) * 4u);
        }
    }
    return builder.publish(src.revision + 1);
}

// montage 命名空间转发（头文件声明位）
liquify::Img applyLiquifyImage(const liquify::Img& src, const liquify::Field& field, int bitDepth) {
    return liquify::applyLiquifyImage(src, field, bitDepth);
}

}  // namespace montage
