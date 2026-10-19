// 编辑几何实现（翻译自 photocraft crates/vector/src/edit.rs，MIT OR Apache-2.0）。

#include "engine/vector_edit.h"

#include "engine/vector_flatten.h"

#include <algorithm>
#include <cmath>

namespace montage {

namespace {

double distP(VPt a, VPt b) { return std::hypot(a.x - b.x, a.y - b.y); }

VPt addD(VPt p, double dx, double dy) { return vpt(p.x + dx, p.y + dy); }

Knot* knotAt(VectorPath& path, size_t s, size_t k) {
    if (s >= path.subpaths.size()) {
        return nullptr;
    }
    Subpath& sp = path.subpaths[s];
    if (k >= sp.knots.size()) {
        return nullptr;
    }
    return &sp.knots[k];
}

const Knot* knotAtC(const VectorPath& path, size_t s, size_t k) {
    if (s >= path.subpaths.size()) {
        return nullptr;
    }
    const Subpath& sp = path.subpaths[s];
    if (k >= sp.knots.size()) {
        return nullptr;
    }
    return &sp.knots[k];
}

HandleId otherHandle(HandleId h) { return h == HandleId::In ? HandleId::Out : HandleId::In; }

VPt lerpP(VPt a, VPt b, double t) { return vpt(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t); }

// 离开 [s,k] 的段的末端 knot（closed 回绕首 knot）。
bool segmentEnd(const VectorPath& path, size_t s, size_t k, size_t& out) {
    if (s >= path.subpaths.size() || k >= path.subpaths[s].knots.size()) {
        return false;
    }
    const Subpath& sp = path.subpaths[s];
    size_t n = sp.knots.size();
    size_t next = k + 1;
    if (next < n) {
        out = next;
        return true;
    }
    if (sp.closed && n > 1) {
        out = 0;
        return true;
    }
    return false;
}

bool retracted(const Knot& k, HandleId h) { return distP(knotHandle(k, h), k.anchor) < 1e-9; }

}  // namespace

VPt knotHandle(const Knot& k, HandleId h) { return h == HandleId::In ? k.inCtrl : k.outCtrl; }

void keepSmooth(Knot& k, HandleId h) {
    if (!k.smooth) {
        return;
    }
    VPt a = k.anchor;
    VPt moved = knotHandle(k, h);
    VPt& opp = h == HandleId::In ? k.outCtrl : k.inCtrl;
    double len = distP(opp, a);
    double d = distP(moved, a);
    if (len > 1e-9 && d > 1e-9) {
        opp = vpt(a.x - (moved.x - a.x) / d * len, a.y - (moved.y - a.y) / d * len);
    }
}

bool movePathAnchors(VectorPath& path, const std::vector<std::pair<size_t, size_t>>& anchors, double dx,
                     double dy) {
    std::vector<std::pair<size_t, size_t>> list = anchors;
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    for (auto& r : list) {
        if (knotAt(path, r.first, r.second) == nullptr) {
            return false;
        }
    }
    for (auto& r : list) {
        Knot* k = knotAt(path, r.first, r.second);
        Knot nk;
        nk.anchor = addD(k->anchor, dx, dy);
        nk.inCtrl = addD(k->inCtrl, dx, dy);
        nk.outCtrl = addD(k->outCtrl, dx, dy);
        nk.smooth = k->smooth;
        *k = nk;
    }
    return true;
}

bool movePathHandle(VectorPath& path, size_t s, size_t k, HandleId h, VPt to, bool independent) {
    Knot* kn = knotAt(path, s, k);
    if (kn == nullptr) {
        return false;
    }
    (h == HandleId::In ? kn->inCtrl : kn->outCtrl) = to;
    if (independent) {
        kn->smooth = false;
    }
    keepSmooth(*kn, h);
    return true;
}

bool convertPathPoint(VectorPath& path, size_t s, size_t k, bool hasOut, VPt out) {
    Knot* kn = knotAt(path, s, k);
    if (kn == nullptr) {
        return false;
    }
    VPt a = kn->anchor;
    if (hasOut) {
        *kn = Knot();
        kn->anchor = a;
        kn->inCtrl = vpt(2.0 * a.x - out.x, 2.0 * a.y - out.y);
        kn->outCtrl = out;
        kn->smooth = true;
    } else {
        *kn = Knot();
        kn->anchor = a;
        kn->inCtrl = a;
        kn->outCtrl = a;
        kn->smooth = false;
    }
    return true;
}

bool bendPathSegment(VectorPath& path, size_t s, size_t k, double t, double dx, double dy) {
    size_t endK = 0;
    if (!segmentEnd(path, s, k, endK)) {
        return false;
    }
    const Knot* a = knotAtC(path, s, k);
    const Knot* b = knotAtC(path, s, endK);
    if (a == nullptr || b == nullptr) {
        return false;
    }
    if (retracted(*a, HandleId::Out) && retracted(*b, HandleId::In)) {
        return movePathAnchors(path, {{s, k}, {s, endK}}, dx, dy);
    }
    // B(t) 位移 = 3(1−t)²t·Δc1 + 3(1−t)t²·Δc2；取 Δc1 = (1−t)·s·d、Δc2 = t·s·d 则
    // = 3t(1−t)((1−t)²+t²)·s·d，下面的 s 使其恰为 d。端点钳制（锚点处右拖需无界柄移）。
    double tc = std::isfinite(t) ? std::min(std::max(t, 0.05), 0.95) : 0.5;
    double u = 1.0 - tc;
    double sc = 1.0 / (3.0 * tc * u * (u * u + tc * tc));
    double w1 = u * sc;
    double w2 = tc * sc;
    Knot* ka = knotAt(path, s, k);
    ka->outCtrl = addD(ka->outCtrl, dx * w1, dy * w1);
    keepSmooth(*ka, HandleId::Out);
    Knot* kb = knotAt(path, s, endK);
    kb->inCtrl = addD(kb->inCtrl, dx * w2, dy * w2);
    keepSmooth(*kb, HandleId::In);
    return true;
}

bool addPathAnchor(VectorPath& path, size_t s, size_t k, double t, size_t& outSub, size_t& outKnot) {
    if (!(std::isfinite(t) && t > 0.0 && t < 1.0)) {
        return false;
    }
    size_t endK = 0;
    if (!segmentEnd(path, s, k, endK)) {
        return false;
    }
    Knot a = *knotAtC(path, s, k);
    Knot b = *knotAtC(path, s, endK);
    Knot nw;
    if (retracted(a, HandleId::Out) && retracted(b, HandleId::In)) {
        VPt p = lerpP(a.anchor, b.anchor, t);
        nw.anchor = p;
        nw.inCtrl = p;
        nw.outCtrl = p;
        nw.smooth = false;
    } else {
        VPt p01 = lerpP(a.anchor, a.outCtrl, t);
        VPt p12 = lerpP(a.outCtrl, b.inCtrl, t);
        VPt p23 = lerpP(b.inCtrl, b.anchor, t);
        VPt p012 = lerpP(p01, p12, t);
        VPt p123 = lerpP(p12, p23, t);
        knotAt(path, s, k)->outCtrl = p01;
        knotAt(path, s, endK)->inCtrl = p23;
        nw.anchor = lerpP(p012, p123, t);
        nw.inCtrl = p012;
        nw.outCtrl = p123;
        nw.smooth = true;
    }
    if (s >= path.subpaths.size() || k >= path.subpaths[s].knots.size()) {
        return false;
    }
    size_t at = k + 1;  // k < len（段存在），at ≤ len 不溢出
    Subpath& sp = path.subpaths[s];
    sp.knots.insert(sp.knots.begin() + static_cast<std::ptrdiff_t>(std::min(at, sp.knots.size())), nw);
    outSub = s;
    outKnot = at;
    return true;
}

bool deletePathAnchor(VectorPath& path, size_t s, size_t k, bool& removedSubpath) {
    removedSubpath = false;
    if (knotAt(path, s, k) == nullptr) {
        return false;
    }
    size_t total = 0;
    for (const Subpath& sp : path.subpaths) {
        total += sp.knots.size();
    }
    if (total <= 1) {
        return false;  // 拒删路径仅剩的锚点（请删除整个路径）
    }
    if (s >= path.subpaths.size()) {
        return false;
    }
    Subpath& sp = path.subpaths[s];
    if (sp.knots.size() <= 1) {
        path.subpaths.erase(path.subpaths.begin() + static_cast<std::ptrdiff_t>(s));
        removedSubpath = true;
        return true;
    }
    sp.knots.erase(sp.knots.begin() + static_cast<std::ptrdiff_t>(k));
    return true;
}

std::vector<std::pair<size_t, size_t>> subpathAnchors(const VectorPath& path, size_t s) {
    std::vector<std::pair<size_t, size_t>> out;
    if (s < path.subpaths.size()) {
        for (size_t k = 0; k < path.subpaths[s].knots.size(); k++) {
            out.push_back({s, k});
        }
    }
    return out;
}

std::vector<std::pair<size_t, size_t>> anchorsInBox(const VectorPath& path, double x0, double y0, double x1,
                                                    double y1) {
    double bx0 = std::min(x0, x1);
    double bx1 = std::max(x0, x1);
    double by0 = std::min(y0, y1);
    double by1 = std::max(y0, y1);
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t s = 0; s < path.subpaths.size(); s++) {
        for (size_t k = 0; k < path.subpaths[s].knots.size(); k++) {
            const VPt& a = path.subpaths[s].knots[k].anchor;
            if (a.x >= bx0 && a.x <= bx1 && a.y >= by0 && a.y <= by1) {
                out.push_back({s, k});
            }
        }
    }
    return out;
}

std::vector<std::array<VPt, 3>> curvatureKnots(const std::vector<VPt>& anchors, const std::vector<bool>& corners,
                                               bool closed) {
    size_t n = anchors.size();
    std::vector<std::array<VPt, 3>> knots(n);
    for (size_t i = 0; i < n; i++) {
        knots[i] = {anchors[i], anchors[i], anchors[i]};
    }
    auto dist2 = [](VPt a, VPt b) { return std::hypot(b.x - a.x, b.y - a.y); };
    auto unit = [](VPt a, VPt b, VPt& out) {
        double l = std::hypot(b.x - a.x, b.y - a.y);
        if (l > 1e-9) {
            out = vpt((b.x - a.x) / l, (b.y - a.y) / l);
            return true;
        }
        return false;
    };
    auto cornerAt = [&](size_t i) { return i < corners.size() ? corners[i] : false; };
    auto inner = [&](size_t i) { return closed || (i > 0 && i + 1 < n); };
    for (size_t i = 0; i < n; i++) {
        if (n < 3 || cornerAt(i) || !inner(i)) {
            continue;
        }
        VPt prev = anchors[(i + n - 1) % n];
        VPt next = anchors[(i + 1) % n];
        VPt p = anchors[i];
        VPt d;
        if (!unit(prev, next, d) && !unit(p, next, d)) {
            continue;
        }
        double li = dist2(prev, p) / 3.0;
        double lo = dist2(p, next) / 3.0;
        knots[i] = {p, vpt(p.x - d.x * li, p.y - d.y * li), vpt(p.x + d.x * lo, p.y + d.y * lo)};
    }
    if (!closed && n >= 3) {
        const std::pair<size_t, size_t> ends[2] = {{0, 1}, {n - 1, n - 2}};
        for (auto& e : ends) {
            size_t end = e.first;
            size_t nb = e.second;
            if (cornerAt(end)) {
                continue;
            }
            VPt p = anchors[end];
            VPt aim = end == 0 ? knots[nb][1] : knots[nb][2];
            VPt d;
            if (!unit(p, aim, d)) {
                continue;
            }
            double l = dist2(p, anchors[nb]) / 3.0;
            VPt h = vpt(p.x + d.x * l, p.y + d.y * l);
            if (end == 0) {
                knots[end][2] = h;
            } else {
                knots[end][1] = h;
            }
        }
    }
    return knots;
}

PathHit hitTestPath(const VectorPath& path, VPt p, double tol,
                    const std::vector<std::pair<std::pair<size_t, size_t>, HandleId>>& handles) {
    double bestD = tol;
    PathHit best;
    best.kind = HitKind::None;
    // 手柄最上层。
    for (const auto& h : handles) {
        const Knot* kn = knotAtC(path, h.first.first, h.first.second);
        if (kn == nullptr) {
            continue;
        }
        double d = distP(knotHandle(*kn, h.second), p);
        if (d <= bestD) {
            bestD = d;
            best.kind = HitKind::Handle;
            best.sub = h.first.first;
            best.knot = h.first.second;
            best.handle = h.second;
        }
    }
    if (best.kind != HitKind::None) {
        return best;
    }
    // 锚点次之。
    for (size_t s = 0; s < path.subpaths.size(); s++) {
        for (size_t k = 0; k < path.subpaths[s].knots.size(); k++) {
            double d = distP(path.subpaths[s].knots[k].anchor, p);
            if (d <= bestD) {
                bestD = d;
                best.kind = HitKind::Anchor;
                best.sub = s;
                best.knot = k;
            }
        }
    }
    if (best.kind != HitKind::None) {
        return best;
    }
    // 段：64 均匀采样取最近，再在其邻域 16 步细化（屏幕容差下足够）。
    constexpr size_t kSamples = 64;
    double step = 1.0 / static_cast<double>(kSamples);
    for (size_t s = 0; s < path.subpaths.size(); s++) {
        auto segs = path.subpaths[s].segments();
        for (size_t k = 0; k < segs.size(); k++) {
            double bd = 1e300;
            double bt = 0.5;
            for (size_t i = 0; i <= kSamples; i++) {
                double t = static_cast<double>(i) / static_cast<double>(kSamples);
                double d = distP(evalBezier(segs[k], t), p);
                if (d < bd) {
                    bd = d;
                    bt = t;
                }
            }
            for (size_t i = 0; i <= 16; i++) {
                double t = bt - step + 2.0 * step * static_cast<double>(i) / 16.0;
                t = std::min(std::max(t, 0.0), 1.0);
                double d = distP(evalBezier(segs[k], t), p);
                if (d < bd) {
                    bd = d;
                    bt = t;
                }
            }
            if (bd <= bestD) {
                bestD = bd;
                best.kind = HitKind::Segment;
                best.sub = s;
                best.knot = k;  // Segment 命中时 knot = 段序号（离开 knot k 的段）
                best.t = bt;
            }
        }
    }
    return best;
}

}  // namespace montage
