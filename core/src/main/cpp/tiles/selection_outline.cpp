#include "tiles/selection_outline.h"

#include <algorithm>
#include <unordered_map>

namespace montage {
namespace tiles {
namespace {

constexpr uint64_t keyOf(int x, int y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
           static_cast<uint64_t>(static_cast<uint32_t>(y));
}

// 二值化（阈值 127）并加 1px 零填充，消除画布边界分支
std::vector<uint8_t> binarize(const TileGrid& mask, int w, int h) {
    std::vector<uint8_t> bin(static_cast<size_t>(w + 2) * (h + 2), 0);
    const int stride = w + 2;
    for (int y = 0; y < h; ++y) {
        const uint32_t ty = static_cast<uint32_t>(y) / kTileSize;
        if (ty >= mask.rows) {
            continue;
        }
        for (int x = 0; x < w; ++x) {
            const uint32_t tx = static_cast<uint32_t>(x) / kTileSize;
            if (tx >= mask.cols) {
                continue;
            }
            auto it = mask.tiles.find(ty * mask.cols + tx);
            if (it == mask.tiles.end()) {
                continue;
            }
            const uint8_t* px = it->second->pixels->mapCpu();
            const uint8_t v = px[(static_cast<size_t>(y % kTileSize) * kTileSize +
                                  static_cast<size_t>(x % kTileSize)) * 4u];  // RGBA：R=灰度
            if (v >= 127) {
                bin[static_cast<size_t>(y + 1) * stride + (x + 1)] = 1;
            }
        }
    }
    return bin;
}

struct Seg {
    int ax;
    int ay;
    int bx;
    int by;
    bool used = false;
};

}  // namespace

std::vector<uint8_t> binarizeForTest(const TileGrid& mask, int w, int h) {
    return binarize(mask, w, h);
}

bool extractSelectionOutline(const TileGrid& mask, std::vector<OutlinePolyline>& out) {
    out.clear();
    if (mask.cols == 0 || mask.rows == 0 || mask.tiles.empty()) {
        return false;
    }
    const int w = static_cast<int>(mask.cols * kTileSize);
    const int h = static_cast<int>(mask.rows * kTileSize);
    const std::vector<uint8_t> bin = binarize(mask, w, h);
    const int stride = w + 2;
    auto at = [&](int x, int y) -> uint8_t {
        return bin[static_cast<size_t>(y + 1) * stride + (x + 1)];
    };

    // 边界段集：内部像素与外部像素相邻处的单位格边（格点坐标）
    std::vector<Seg> segs;
    std::unordered_map<uint64_t, std::vector<size_t>> byPoint;
    auto addSeg = [&](int ax, int ay, int bx, int by) {
        const size_t idx = segs.size();
        segs.push_back(Seg{ax, ay, bx, by, false});
        byPoint[keyOf(ax, ay)].push_back(idx);
        byPoint[keyOf(bx, by)].push_back(idx);
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (at(x, y) == 0) {
                continue;
            }
            if (at(x - 1, y) == 0) {
                addSeg(x, y, x, y + 1);       // 左边界（竖直段）
            }
            if (at(x + 1, y) == 0) {
                addSeg(x + 1, y, x + 1, y + 1);  // 右边界
            }
            if (at(x, y - 1) == 0) {
                addSeg(x, y, x + 1, y);       // 上边界（水平段）
            }
            if (at(x, y + 1) == 0) {
                addSeg(x, y + 1, x + 1, y + 1);  // 下边界
            }
        }
    }
    if (segs.empty()) {
        return false;
    }

    // 端点链接成环：从任一未用段出发，沿未用邻接段前进（棋盘角点可能有三条候选，任取其一）
    for (size_t start = 0; start < segs.size(); ++start) {
        if (segs[start].used) {
            continue;
        }
        segs[start].used = true;
        OutlinePolyline poly;
        int cx = segs[start].ax;
        int cy = segs[start].ay;
        poly.x.push_back(static_cast<float>(cx));
        poly.y.push_back(static_cast<float>(cy));
        int tx = segs[start].bx;
        int ty = segs[start].by;
        int guard = static_cast<int>(segs.size()) + 4;
        for (;;) {
            poly.x.push_back(static_cast<float>(tx));
            poly.y.push_back(static_cast<float>(ty));
            if (tx == static_cast<int>(segs[start].ax) && ty == static_cast<int>(segs[start].ay)) {
                break;  // 闭合
            }
            if (--guard < 0) {
                break;  // 防御
            }
            size_t next = segs.size();
            for (const size_t cand : byPoint[keyOf(tx, ty)]) {
                if (!segs[cand].used) {
                    next = cand;
                    break;
                }
            }
            if (next == segs.size()) {
                break;  // 开放链（不应发生，防御）
            }
            segs[next].used = true;
            // 段的另一端作为新链头
            if (segs[next].ax == tx && segs[next].ay == ty) {
                cx = segs[next].bx;
                cy = segs[next].by;
            } else {
                cx = segs[next].ax;
                cy = segs[next].ay;
            }
            tx = cx;
            ty = cy;
        }
        if (poly.x.size() >= 4) {
            out.push_back(std::move(poly));
        }
    }
    return !out.empty();
}

}  // namespace tiles
}  // namespace montage
