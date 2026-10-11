#include "tiles/selection_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace montage {
namespace tiles {

namespace {

constexpr uint32_t kTile = kTileSize;

// 2×2 覆盖采样的子采样偏移（像素中心系：采样点 = 像素原点 + 偏移）
constexpr double kSubOff[4][2] = {{0.25, 0.25}, {0.75, 0.25}, {0.25, 0.75}, {0.75, 0.75}};

inline uint8_t coverageByte(int hits) {
    return static_cast<uint8_t>((255 * hits + 2) / 4);  // 0/64/128/191/255
}

// 掩码网格写入走 builder：灰度 0 不落瓦片（稀疏约定）
void writeGrayToBuilder(TileGridBuilder& builder, uint32_t docX, uint32_t docY, uint8_t gray) {
    uint8_t* px = builder.ensureTile(docX / kTile, docY / kTile).mapCpuWrite();
    uint8_t* p = px + static_cast<size_t>(docY % kTile) * kTile * 4 + static_cast<size_t>(docX % kTile) * 4;
    p[0] = gray;
    p[1] = gray;
    p[2] = gray;
    p[3] = 255;
}

// 从选区掩码读灰度（缺瓦片 = 0）
inline uint8_t maskGrayAt(const TileGrid& g, uint32_t x, uint32_t y) {
    auto it = g.tiles.find((y / kTile) * g.cols + (x / kTile));
    if (it == g.tiles.end() || it->second == nullptr || it->second->pixels == nullptr) {
        return 0;
    }
    const uint8_t* base = it->second->pixels->mapCpu();
    return base[static_cast<size_t>(y % kTile) * kTile * 4 + static_cast<size_t>(x % kTile) * 4];
}

}  // namespace

void setMaskGray(TileGridBuilder& builder, uint32_t cols, uint32_t docX, uint32_t docY,
                 uint8_t gray) {
    (void)cols;
    writeGrayToBuilder(builder, docX, docY, gray);
}

std::shared_ptr<const TileGrid> polygonMask(const std::vector<double>& xy,
                                            uint32_t docW, uint32_t docH) {
    const size_t n = xy.size() / 2;
    if (n < 3 || docW == 0 || docH == 0) {
        return nullptr;
    }
    const int w = static_cast<int>(docW);
    const int h = static_cast<int>(docH);

    // 每条子扫描线（2 条/像素行）的边交点 x 列表（半开区间规则防顶点重复计数）
    const int subRows = h * 2;
    std::vector<std::vector<double>> crossings(static_cast<size_t>(subRows));
    for (size_t e = 0; e < n; ++e) {
        const double x1 = xy[e * 2];
        const double y1 = xy[e * 2 + 1];
        const size_t f = (e + 1) % n;
        const double x2 = xy[f * 2];
        const double y2 = xy[f * 2 + 1];
        if (y1 == y2) {
            continue;  // 水平边不产生交点
        }
        // 粗筛放宽 ±2 子行（性能优化而已），半开区间检查精确过滤
        const int syLo = std::max(0, static_cast<int>(std::floor(std::min(y1, y2) * 2)) - 2);
        const int syHi = std::min(subRows - 1,
                                  static_cast<int>(std::floor(std::max(y1, y2) * 2)) + 2);
        for (int s = syLo; s <= syHi; ++s) {
            // 子扫描线实际 y：偶数子行 = py+0.25（py=s/2），奇数子行 = py+0.75
            const double sampleY = static_cast<double>(s / 2) + (s % 2 == 0 ? 0.25 : 0.75);
            if (sampleY < std::min(y1, y2) || sampleY >= std::max(y1, y2)) {
                continue;  // 半开区间 [min, max)
            }
            const double t = (sampleY - y1) / (y2 - y1);
            crossings[static_cast<size_t>(s)].push_back(x1 + t * (x2 - x1));
        }
    }
    for (auto& c : crossings) {
        std::sort(c.begin(), c.end());
    }

    TileGridBuilder builder((docW + kTile - 1u) / kTile, (docH + kTile - 1u) / kTile);
    bool any = false;
    for (int py = 0; py < h; ++py) {
        const std::vector<double>& c0 = crossings[static_cast<size_t>(py * 2)];
        const std::vector<double>& c1 = crossings[static_cast<size_t>(py * 2 + 1)];
        if (c0.empty() && c1.empty()) {
            continue;
        }
        // 可能命中像素的 x 范围：交点最小值左移 1（像素跨 0.25/0.75 两采样）
        const double minX = std::min(c0.empty() ? 1e18 : c0.front(), c1.empty() ? 1e18 : c1.front());
        const double maxX = std::max(c0.empty() ? -1e18 : c0.back(), c1.empty() ? -1e18 : c1.back());
        const int pxLo = std::max(0, static_cast<int>(std::floor(minX)) - 1);
        const int pxHi = std::min(w - 1, static_cast<int>(std::ceil(maxX)) + 1);
        for (int px = pxLo; px <= pxHi; ++px) {
            int hits = 0;
            for (int q = 0; q < 4; ++q) {
                const double sx = static_cast<double>(px) + kSubOff[q][0];
                const std::vector<double>& c = (q < 2) ? c0 : c1;  // 上两样本走 0.25 子行
                // 偶奇规则：x 右侧交点数为奇数 = 内部（排序数组二分）
                const auto it = std::upper_bound(c.begin(), c.end(), sx);
                if (static_cast<long>(std::distance(it, c.end())) % 2 == 1) {
                    ++hits;
                }
            }
            if (hits > 0) {
                writeGrayToBuilder(builder, static_cast<uint32_t>(px), static_cast<uint32_t>(py),
                                   coverageByte(hits));
                any = true;
            }
        }
    }
    return any ? builder.publish(1) : nullptr;
}

std::shared_ptr<const TileGrid> ellipseMask(double x, double y, double w, double h,
                                            uint32_t docW, uint32_t docH) {
    if (docW == 0 || docH == 0 || w <= 0 || h <= 0) {
        return nullptr;
    }
    const int x0 = std::max(0, static_cast<int>(std::floor(x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(y)));
    const int x1 = std::min(static_cast<int>(docW) - 1, static_cast<int>(std::ceil(x + w)) - 1);
    const int y1 = std::min(static_cast<int>(docH) - 1, static_cast<int>(std::ceil(y + h)) - 1);
    if (x0 > x1 || y0 > y1) {
        return nullptr;
    }
    const double cx = x + w / 2.0;
    const double cy = y + h / 2.0;
    const double rx = w / 2.0;
    const double ry = h / 2.0;

    TileGridBuilder builder((docW + kTile - 1u) / kTile, (docH + kTile - 1u) / kTile);
    bool any = false;
    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            int hits = 0;
            for (int q = 0; q < 4; ++q) {
                const double nx = (static_cast<double>(px) + kSubOff[q][0] - cx) / rx;
                const double ny = (static_cast<double>(py) + kSubOff[q][1] - cy) / ry;
                if (nx * nx + ny * ny <= 1.0) {
                    ++hits;
                }
            }
            if (hits > 0) {
                writeGrayToBuilder(builder, static_cast<uint32_t>(px), static_cast<uint32_t>(py),
                                   coverageByte(hits));
                any = true;
            }
        }
    }
    return any ? builder.publish(1) : nullptr;
}

std::shared_ptr<const TileGrid> rectMask(double x, double y, double w, double h,
                                         uint32_t docW, uint32_t docH) {
    if (docW == 0 || docH == 0 || w <= 0 || h <= 0) {
        return nullptr;
    }
    const int ix0 = std::max(0, static_cast<int>(std::ceil(x)));
    const int iy0 = std::max(0, static_cast<int>(std::ceil(y)));
    const int ix1 = std::min(static_cast<int>(docW), static_cast<int>(std::floor(x + w)));
    const int iy1 = std::min(static_cast<int>(docH), static_cast<int>(std::floor(y + h)));
    if (ix0 >= ix1 || iy0 >= iy1) {
        return nullptr;
    }
    TileGridBuilder builder((docW + kTile - 1u) / kTile, (docH + kTile - 1u) / kTile);
    for (int py = iy0; py < iy1; ++py) {
        for (int px = ix0; px < ix1; ++px) {
            writeGrayToBuilder(builder, static_cast<uint32_t>(px), static_cast<uint32_t>(py), 255);
        }
    }
    return builder.publish(1);
}

std::shared_ptr<const TileGrid> combineSelection(const Document& doc,
                                                 std::shared_ptr<const TileGrid> next,
                                                 SelectionOp op) {
    if (doc.width == 0 || doc.height == 0) {
        return nullptr;
    }
    const std::shared_ptr<const TileGrid> cur = doc.selection;
    if (op == SelectionOp::Replace || cur == nullptr) {
        if (op == SelectionOp::Subtract || op == SelectionOp::Intersect) {
            return nullptr;  // 无基选区时减/交无效（PS 语义）
        }
        return next;
    }
    if (next == nullptr) {
        return op == SelectionOp::Add ? cur : nullptr;
    }
    const TileGrid& a = *cur;
    const TileGrid& b = *next;
    const uint32_t cols = std::max(a.cols, b.cols);
    const uint32_t rows = std::max(a.rows, b.rows);
    TileGridBuilder builder(cols, rows);
    bool any = false;
    for (uint32_t ty = 0; ty < rows; ++ty) {
        for (uint32_t tx = 0; tx < cols; ++tx) {
            const uint32_t key = ty * cols + tx;
            const bool hasA = tx < a.cols && ty < a.rows && a.tiles.count(ty * a.cols + tx) > 0;
            const bool hasB = tx < b.cols && ty < b.rows && b.tiles.count(ty * b.cols + tx) > 0;
            if (!hasA && !hasB) {
                continue;
            }
            if (hasA != hasB) {
                // 单侧瓦片：Add 保非空侧；Subtract 保 A 侧；Intersect 丢弃
                if (op == SelectionOp::Add || (op == SelectionOp::Subtract && hasA)) {
                    const TileGrid& src = hasA ? a : b;
                    const Tile& t = *src.tiles.at(ty * src.cols + tx);
                    std::memcpy(builder.ensureTile(tx, ty).mapCpuWrite(), t.pixels->mapCpu(),
                                static_cast<size_t>(kTile) * kTile * 4);
                    any = true;
                }
                continue;
            }
            // 双侧瓦片逐像素合并
            const uint8_t* pa = a.tiles.at(ty * a.cols + tx)->pixels->mapCpu();
            const uint8_t* pb = b.tiles.at(ty * b.cols + tx)->pixels->mapCpu();
            uint8_t* out = builder.ensureTile(tx, ty).mapCpuWrite();
            for (size_t i = 0; i < static_cast<size_t>(kTile) * kTile * 4; i += 4) {
                int v = 0;
                const int av = pa[i];
                const int bv = pb[i];
                switch (op) {
                    case SelectionOp::Add:
                        v = std::max(av, bv);
                        break;
                    case SelectionOp::Subtract:
                        v = std::max(0, av - bv);
                        break;
                    case SelectionOp::Intersect:
                        v = std::min(av, bv);
                        break;
                    default:
                        v = bv;
                        break;
                }
                if (v > 0) {
                    out[i] = out[i + 1] = out[i + 2] = static_cast<uint8_t>(v);
                    out[i + 3] = 255;
                    any = true;
                }
            }
        }
    }
    if (!any) {
        return nullptr;
    }
    return builder.publish(a.revision + b.revision + 1);
}

std::shared_ptr<const TileGrid> invertSelection(const Document& doc) {
    if (doc.width == 0 || doc.height == 0) {
        return nullptr;
    }
    if (doc.selection == nullptr) {
        return fullSelection(doc.width, doc.height);
    }
    const TileGrid& a = *doc.selection;
    TileGridBuilder builder(a.cols, a.rows);
    bool any = false;
    for (uint32_t ty = 0; ty < a.rows; ++ty) {
        for (uint32_t tx = 0; tx < a.cols; ++tx) {
            for (uint32_t r = 0; r < kTile; ++r) {
                const uint32_t gy = ty * kTile + r;
                if (gy >= doc.height) {
                    break;
                }
                for (uint32_t c = 0; c < kTile; ++c) {
                    const uint32_t gx = tx * kTile + c;
                    if (gx >= doc.width) {
                        break;
                    }
                    const uint8_t v = static_cast<uint8_t>(255 - maskGrayAt(a, gx, gy));
                    if (v > 0) {
                        writeGrayToBuilder(builder, gx, gy, v);
                        any = true;
                    }
                }
            }
        }
    }
    return any ? builder.publish(a.revision + 1) : nullptr;
}

std::shared_ptr<const TileGrid> fullSelection(uint32_t docW, uint32_t docH) {
    if (docW == 0 || docH == 0) {
        return nullptr;
    }
    TileGridBuilder builder((docW + kTile - 1u) / kTile, (docH + kTile - 1u) / kTile);
    for (uint32_t ty = 0; ty < builder.rows(); ++ty) {
        for (uint32_t tx = 0; tx < builder.cols(); ++tx) {
            uint8_t* px = builder.ensureTile(tx, ty).mapCpuWrite();
            for (size_t i = 0; i < static_cast<size_t>(kTile) * kTile * 4; i += 4) {
                px[i] = px[i + 1] = px[i + 2] = 255;
                px[i + 3] = 255;
            }
        }
    }
    return builder.publish(1);
}

// ---- M10b 选区修改族 ----

namespace {

// 稀疏掩码 → w*h uint8 灰度平面（缺瓦片 = 0）
std::vector<uint8_t> maskToPlane(const TileGrid& g, uint32_t w, uint32_t h) {
    std::vector<uint8_t> plane(static_cast<size_t>(w) * h, 0);
    for (const auto& [key, tile] : g.tiles) {
        if (tile == nullptr || tile->pixels == nullptr) {
            continue;
        }
        const uint32_t tx = key % g.cols;
        const uint32_t ty = key / g.cols;
        const int32_t x0 = static_cast<int32_t>(tx * kTile);
        const int32_t y0 = static_cast<int32_t>(ty * kTile);
        const int32_t copyW = std::min<int32_t>(static_cast<int32_t>(kTile),
                                                static_cast<int32_t>(w) - x0);
        const int32_t copyH = std::min<int32_t>(static_cast<int32_t>(kTile),
                                                static_cast<int32_t>(h) - y0);
        if (copyW <= 0 || copyH <= 0) {
            continue;
        }
        const uint8_t* src = tile->pixels->mapCpu();
        for (int32_t r = 0; r < copyH; ++r) {
            uint8_t* dst = plane.data() + static_cast<size_t>(y0 + r) * w + x0;
            const uint8_t* s = src + static_cast<size_t>(r) * kTile * 4;
            for (int32_t c = 0; c < copyW; ++c) {
                dst[c] = s[static_cast<size_t>(c) * 4];
            }
        }
    }
    return plane;
}

// 灰度平面 → 稀疏网格（0 不落瓦片）；全 0 返回 nullptr
std::shared_ptr<const TileGrid> planeToMask(const std::vector<uint8_t>& plane,
                                            uint32_t w, uint32_t h, uint64_t revision) {
    TileGridBuilder builder((w + kTile - 1u) / kTile, (h + kTile - 1u) / kTile);
    bool any = false;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = plane.data() + static_cast<size_t>(y) * w;
        for (uint32_t x = 0; x < w; ++x) {
            if (row[x] != 0) {
                writeGrayToBuilder(builder, x, y, row[x]);
                any = true;
            }
        }
    }
    return any ? builder.publish(revision) : nullptr;
}

// 一维滑动窗极值（van Herk/Gil-Werman O(N)，pre/suf 同一网格分块）：edge-clamp 边界
void morphPass1d(uint8_t* line, int n, int r, bool dilation) {
    if (r <= 0 || n <= 0) {
        return;
    }
    const auto pick = [dilation](uint8_t a, uint8_t b) -> uint8_t {
        return dilation ? std::max(a, b) : std::min(a, b);
    };
    const int win = 2 * r + 1;
    std::vector<uint8_t> pre(static_cast<size_t>(n));
    std::vector<uint8_t> suf(static_cast<size_t>(n));
    // 前缀（块起点 k*win 重置）与后缀（块终点 k*win-1 重置——同一网格，保证窗口覆盖无缺口）
    for (int i = 0; i < n; ++i) {
        const uint8_t v = line[i];
        pre[i] = (i % win == 0) ? v : pick(pre[i - 1], v);
    }
    for (int i = n - 1; i >= 0; --i) {
        const uint8_t v = line[i];
        suf[i] = (((i + 1) % win == 0) || (i == n - 1)) ? v : pick(suf[i + 1], v);
    }
    std::vector<uint8_t> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int pa = std::min(n - 1, i + r);
        const int sa = std::max(0, i - r);
        out[i] = pick(pre[pa], suf[sa]);
    }
    std::memcpy(line, out.data(), static_cast<size_t>(n));
}

// 盒模糊 1D（clamp 窗口逐项和 → 滑动增量；round 除法）
void boxPass1d(const uint16_t* src, uint16_t* dst, int n, int r) {
    if (r <= 0 || n <= 0) {
        std::memcpy(dst, src, static_cast<size_t>(n) * sizeof(uint16_t));
        return;
    }
    const int win = 2 * r + 1;
    int64_t sum = 0;
    for (int k = -r; k <= r; ++k) {
        sum += src[std::min(n - 1, std::max(0, k))];
    }
    for (int i = 0; i < n; ++i) {
        dst[i] = static_cast<uint16_t>((sum + win / 2) / win);
        const int addIdx = std::min(n - 1, i + 1 + r);
        const int subIdx = std::max(0, i - r);
        sum += src[addIdx] - src[subIdx];
    }
}

// 方形 SE 形态学两趟（水平+垂直 van Herk）
std::vector<uint8_t> morphPlane(std::vector<uint8_t> plane, uint32_t w, uint32_t h,
                                int r, bool dilation) {
    for (uint32_t y = 0; y < h; ++y) {
        morphPass1d(plane.data() + static_cast<size_t>(y) * w, static_cast<int>(w), r, dilation);
    }
    for (uint32_t x = 0; x < w; ++x) {
        std::vector<uint8_t> col(static_cast<size_t>(h));
        for (uint32_t y = 0; y < h; ++y) {
            col[y] = plane[static_cast<size_t>(y) * w + x];
        }
        morphPass1d(col.data(), static_cast<int>(h), r, dilation);
        for (uint32_t y = 0; y < h; ++y) {
            plane[static_cast<size_t>(y) * w + x] = col[y];
        }
    }
    return plane;
}

std::shared_ptr<const TileGrid> morphSelection(const Document& doc, int radius, bool dilation) {
    if (doc.width == 0 || doc.height == 0 || doc.selection == nullptr || radius <= 0) {
        return nullptr;
    }
    std::vector<uint8_t> plane = maskToPlane(*doc.selection, doc.width, doc.height);
    plane = morphPlane(std::move(plane), doc.width, doc.height, radius, dilation);
    return planeToMask(plane, doc.width, doc.height, doc.selection->revision + 1);
}

}  // namespace

std::shared_ptr<const TileGrid> featherSelection(const Document& doc, double radius) {
    if (doc.width == 0 || doc.height == 0 || doc.selection == nullptr || radius <= 0) {
        return nullptr;
    }
    const uint32_t w = doc.width;
    const uint32_t h = doc.height;
    std::vector<uint8_t> out = maskToPlane(*doc.selection, w, h);
    // σ = r/2（PS 对拍校准起步值，登记）；三趟盒宽 ≈ 2σ = r（高斯三盒近似），最小 1
    const int r = std::max(1, static_cast<int>(std::lround(radius / 2.0)));
    const size_t n = out.size();
    std::vector<uint16_t> a(n);
    std::vector<uint16_t> b(n);
    for (size_t i = 0; i < n; ++i) {
        a[i] = out[i];
    }
    for (int pass = 0; pass < 3; ++pass) {
        for (uint32_t y = 0; y < h; ++y) {  // 水平：a → b
            boxPass1d(a.data() + static_cast<size_t>(y) * w,
                      b.data() + static_cast<size_t>(y) * w, static_cast<int>(w), r);
        }
        std::vector<uint16_t> col(static_cast<size_t>(h));
        std::vector<uint16_t> colOut(static_cast<size_t>(h));
        for (uint32_t x = 0; x < w; ++x) {  // 垂直：b → a（src/dst 必须分离——滑动增量读原值）
            for (uint32_t y = 0; y < h; ++y) {
                col[y] = b[static_cast<size_t>(y) * w + x];
            }
            boxPass1d(col.data(), colOut.data(), static_cast<int>(h), r);
            for (uint32_t y = 0; y < h; ++y) {
                a[static_cast<size_t>(y) * w + x] = colOut[y];
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>(std::min(255, std::max(0, static_cast<int>(a[i]))));
    }
    return planeToMask(out, w, h, doc.selection->revision + 1);
}

std::shared_ptr<const TileGrid> expandSelection(const Document& doc, int radius) {
    return morphSelection(doc, radius, true);
}

std::shared_ptr<const TileGrid> contractSelection(const Document& doc, int radius) {
    return morphSelection(doc, radius, false);
}

std::shared_ptr<const TileGrid> borderSelection(const Document& doc, int radius) {
    if (doc.width == 0 || doc.height == 0 || doc.selection == nullptr || radius <= 0) {
        return nullptr;
    }
    const std::vector<uint8_t> base = maskToPlane(*doc.selection, doc.width, doc.height);
    const std::vector<uint8_t> dil = morphPlane(base, doc.width, doc.height, radius, true);
    const std::vector<uint8_t> ero = morphPlane(base, doc.width, doc.height, radius, false);
    std::vector<uint8_t> band(base.size());
    for (size_t i = 0; i < base.size(); ++i) {
        band[i] = static_cast<uint8_t>(
            std::max(0, static_cast<int>(dil[i]) - static_cast<int>(ero[i])));
    }
    return planeToMask(band, doc.width, doc.height, doc.selection->revision + 1);
}

std::shared_ptr<const TileGrid> smoothSelection(const Document& doc) {
    if (doc.width == 0 || doc.height == 0 || doc.selection == nullptr) {
        return nullptr;
    }
    const uint32_t w = doc.width;
    const uint32_t h = doc.height;
    std::vector<uint8_t> bin = maskToPlane(*doc.selection, w, h);
    for (size_t i = 0; i < bin.size(); ++i) {
        bin[i] = bin[i] >= 127 ? 1 : 0;
    }
    // 两轮 3×3 多数滤波（≥5 含自身为选中）；边缘 clamp
    for (int round = 0; round < 2; ++round) {
        std::vector<uint8_t> nx(bin.size());
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                int cnt = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int yy = std::min(static_cast<int>(h) - 1,
                                            std::max(0, static_cast<int>(y) + dy));
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = std::min(static_cast<int>(w) - 1,
                                                std::max(0, static_cast<int>(x) + dx));
                        cnt += bin[static_cast<size_t>(yy) * w + xx];
                    }
                }
                nx[static_cast<size_t>(y) * w + x] = cnt >= 5 ? 1 : 0;
            }
        }
        bin = std::move(nx);
    }
    for (size_t i = 0; i < bin.size(); ++i) {
        bin[i] = bin[i] != 0 ? 255 : 0;
    }
    return planeToMask(bin, w, h, doc.selection->revision + 1);
}

}  // namespace tiles
}  // namespace montage
