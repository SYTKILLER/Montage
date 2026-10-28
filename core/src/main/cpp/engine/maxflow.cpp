// P5③：Boykov–Kolmogorov 最大流（photocraft segment/maxflow.rs 直译——bk/augment/
// adopt 三段与启发式逐行对拍；队列 = 环形缓冲替代 VecDeque）。
#include "engine/maxflow.h"

#include <cmath>
#include <deque>

namespace montage {

namespace {
constexpr uint32_t kNone = MaxflowGraph::kNone;
constexpr uint32_t kTerminal = MaxflowGraph::kTerminal;
constexpr uint32_t kOrphan = MaxflowGraph::kOrphan;
constexpr uint32_t kInfDist = MaxflowGraph::kInfDist;
constexpr float kEps = MaxflowGraph::kEps;
}  // namespace

MaxflowGraph::MaxflowGraph(size_t nodes, size_t edgesHint)
    : first_(nodes, kNone),
      trCap_(nodes, 0.0f),
      parent_(nodes, kNone),
      isSink_(nodes, 0),
      ts_(nodes, 0),
      dist_(nodes, 0),
      active_(nodes, 0) {
    head_.reserve(edgesHint * 2u);
    next_.reserve(edgesHint * 2u);
    rCap_.reserve(edgesHint * 2u);
}

void MaxflowGraph::addTweights(size_t i, float capSource, float capSink) {
    const float delta = trCap_[i];
    float s = capSource;
    float t = capSink;
    if (delta > 0.0f) {
        s += delta;
    } else {
        t -= delta;
    }
    flow_ += static_cast<double>(std::min(s, t));
    trCap_[i] = s - t;
}

void MaxflowGraph::addEdge(size_t i, size_t j, float cap, float revCap) {
    const uint32_t a = static_cast<uint32_t>(head_.size());
    head_.push_back(static_cast<uint32_t>(j));
    next_.push_back(first_[i]);
    rCap_.push_back(cap);
    first_[i] = a;
    head_.push_back(static_cast<uint32_t>(i));
    next_.push_back(first_[j]);
    rCap_.push_back(revCap);
    first_[j] = a + 1;
}

bool MaxflowGraph::inSource(size_t i) const {
    return parent_[i] != kNone && isSink_[i] == 0;
}

double MaxflowGraph::maxflow() {
    bk();
    return flow_;
}

void MaxflowGraph::bk() {
    const size_t n = first_.size();
    std::deque<uint32_t> queue;
    std::deque<uint32_t> orphans;
    for (size_t i = 0; i < n; ++i) {
        active_[i] = 0;
        ts_[i] = 0;
        if (std::abs(trCap_[i]) > kEps) {
            isSink_[i] = trCap_[i] < 0.0f ? 1u : 0u;
            parent_[i] = kTerminal;
            dist_[i] = 1;
            active_[i] = 1;
            queue.push_back(static_cast<uint32_t>(i));
        } else {
            parent_[i] = kNone;
        }
    }
    uint32_t time = 0;
    while (!queue.empty()) {
        const size_t i = queue.front();
        queue.pop_front();
        active_[i] = 0;
        if (parent_[i] == kNone) {
            continue;
        }
        // 生长段
        uint32_t contact = kNone;
        const bool sink = isSink_[i] != 0;
        uint32_t a = first_[i];
        while (a != kNone) {
            const size_t au = a;
            const size_t j = head_[au];
            const float cap = sink ? rCap_[au ^ 1] : rCap_[au];
            if (cap > kEps) {
                if (parent_[j] == kNone) {
                    isSink_[j] = sink ? 1u : 0u;
                    parent_[j] = a ^ 1;
                    ts_[j] = ts_[i];
                    dist_[j] = dist_[i] + 1;
                    if (active_[j] == 0) {
                        active_[j] = 1;
                        queue.push_back(static_cast<uint32_t>(j));
                    }
                } else if (isSink_[j] != (sink ? 1u : 0u)) {
                    // S 侧到 T 侧的弧 = 接触点
                    contact = sink ? (a ^ 1) : a;
                    break;
                } else if (ts_[j] <= ts_[i] && dist_[j] > dist_[i]) {
                    // 启发式：向终端缩短路径
                    parent_[j] = a ^ 1;
                    ts_[j] = ts_[i];
                    dist_[j] = dist_[i] + 1;
                }
            }
            a = next_[au];
        }
        if (contact == kNone) {
            continue;
        }
        // 增广后本节点继续扫描
        if (active_[i] == 0) {
            active_[i] = 1;
            queue.push_front(static_cast<uint32_t>(i));
        }
        ++time;
        augment(contact, orphans);
        // 收养段
        while (!orphans.empty()) {
            const size_t o = orphans.front();
            orphans.pop_front();
            adopt(o, time, queue, orphans);
        }
    }
}

void MaxflowGraph::augment(size_t m, std::deque<uint32_t>& orphans) {
    const size_t u = head_[m ^ 1];
    const size_t v = head_[m];
    // 瓶颈
    float b = rCap_[m];
    size_t x = u;
    while (true) {
        const uint32_t a = parent_[x];
        if (a == kTerminal) {
            break;
        }
        b = std::min(b, rCap_[a ^ 1]);
        x = head_[a];
    }
    b = std::min(b, trCap_[x]);
    x = v;
    while (true) {
        const uint32_t a = parent_[x];
        if (a == kTerminal) {
            break;
        }
        b = std::min(b, rCap_[a]);
        x = head_[a];
    }
    b = std::min(b, -trCap_[x]);
    // 推流
    rCap_[m ^ 1] += b;
    rCap_[m] -= b;
    x = u;
    while (true) {
        const uint32_t a = parent_[x];
        if (a == kTerminal) {
            break;
        }
        const size_t au = a;
        rCap_[au] += b;
        rCap_[au ^ 1] -= b;
        const size_t up = head_[au];
        if (rCap_[au ^ 1] <= kEps) {
            parent_[x] = kOrphan;
            orphans.push_back(static_cast<uint32_t>(x));
        }
        x = up;
    }
    trCap_[x] -= b;
    if (trCap_[x] <= kEps) {
        parent_[x] = kOrphan;
        orphans.push_back(static_cast<uint32_t>(x));
    }
    x = v;
    while (true) {
        const uint32_t a = parent_[x];
        if (a == kTerminal) {
            break;
        }
        const size_t au = a;
        rCap_[au ^ 1] += b;
        rCap_[au] -= b;
        const size_t up = head_[au];
        if (rCap_[au] <= kEps) {
            parent_[x] = kOrphan;
            orphans.push_back(static_cast<uint32_t>(x));
        }
        x = up;
    }
    trCap_[x] += b;
    if (trCap_[x] >= -kEps) {
        parent_[x] = kOrphan;
        orphans.push_back(static_cast<uint32_t>(x));
    }
    flow_ += static_cast<double>(b);
}

void MaxflowGraph::adopt(size_t i, uint32_t time, std::deque<uint32_t>& queue,
                         std::deque<uint32_t>& orphans) {
    const bool sink = isSink_[i] != 0;
    uint32_t best = kNone;
    uint32_t dMin = kInfDist;
    uint32_t a = first_[i];
    while (a != kNone) {
        const size_t au = a;
        const float cap = sink ? rCap_[au] : rCap_[au ^ 1];
        const size_t j = head_[au];
        if (cap > kEps && isSink_[j] == (sink ? 1u : 0u) && parent_[j] != kNone) {
            // j 的路径是否通到终端？
            uint32_t d = 0;
            size_t k = j;
            while (true) {
                if (ts_[k] == time) {
                    d = d > kInfDist - dist_[k] ? kInfDist : d + dist_[k];
                    break;
                }
                const uint32_t pa = parent_[k];
                d = d >= kInfDist ? kInfDist : d + 1;
                if (pa == kTerminal) {
                    ts_[k] = time;
                    dist_[k] = 1;
                    break;
                }
                if (pa == kOrphan) {
                    d = kInfDist;
                    break;
                }
                k = head_[pa];
            }
            if (d < kInfDist) {
                if (d < dMin) {
                    best = a;
                    dMin = d;
                }
                // 沿路径缓存距离
                size_t k = j;
                uint32_t dd = d;
                while (ts_[k] != time) {
                    ts_[k] = time;
                    dist_[k] = dd;
                    dd = dd > 0 ? dd - 1 : 0;
                    k = head_[parent_[k]];
                }
            }
        }
        a = next_[au];
    }
    if (best != kNone) {
        parent_[i] = best;
        ts_[i] = time;
        dist_[i] = dMin + 1;
        return;
    }
    // 无父：i 成为自由节点
    parent_[i] = kNone;
    a = first_[i];
    while (a != kNone) {
        const size_t au = a;
        const size_t j = head_[au];
        const uint32_t pj = parent_[j];
        if (isSink_[j] == (sink ? 1u : 0u) && pj != kNone) {
            const float cap = sink ? rCap_[au] : rCap_[au ^ 1];
            if (cap > kEps && active_[j] == 0) {
                active_[j] = 1;
                queue.push_back(static_cast<uint32_t>(j));
            }
            if (pj != kTerminal && pj != kOrphan && head_[pj] == i) {
                parent_[j] = kOrphan;
                orphans.push_back(static_cast<uint32_t>(j));
            }
        }
        a = next_[au];
    }
}

}  // namespace montage
