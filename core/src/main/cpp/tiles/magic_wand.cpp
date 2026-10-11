#include "tiles/magic_wand.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <queue>

namespace montage {
namespace tiles {

namespace {

inline bool withinTolerance(const uint8_t* a, const uint8_t* b, int tol) {
    const int dr = std::abs(static_cast<int>(a[0]) - static_cast<int>(b[0]));
    const int dg = std::abs(static_cast<int>(a[1]) - static_cast<int>(b[1]));
    const int db = std::abs(static_cast<int>(a[2]) - static_cast<int>(b[2]));
    return dr <= tol && dg <= tol && db <= tol;
}

// 单层采样源：瓦片惰性加载缓存（BFS 前沿活跃瓦片集，远小于整层 flatten）
class TileSource : public WandSource {
  public:
    explicit TileSource(const TileGrid& grid) : grid_(grid) {
    }

    const uint8_t* at(int x, int y) override {
        if (x < 0 || y < 0) {
            return nullptr;
        }
        const uint32_t ux = static_cast<uint32_t>(x);
        const uint32_t uy = static_cast<uint32_t>(y);
        const uint32_t tx = ux / kTileSize;
        const uint32_t ty = uy / kTileSize;
        if (tx >= grid_.cols || ty >= grid_.rows) {
            return nullptr;
        }
        const uint32_t key = ty * grid_.cols + tx;
        auto it = cache_.find(key);
        if (it == cache_.end()) {
            auto tileIt = grid_.tiles.find(key);
            if (tileIt == grid_.tiles.end() || tileIt->second == nullptr ||
                tileIt->second->pixels == nullptr) {
                cache_.emplace(key, Loaded{});  // 缺瓦片缓存空标记（避免重复查 map）
                return nullptr;
            }
            Loaded loaded;
            loaded.loaded = true;
            const uint8_t* src = tileIt->second->pixels->mapCpu();
            const uint32_t rb = tileIt->second->pixels->rowBytes();
            loaded.bytes.resize(static_cast<size_t>(kTileSize) * kTileSize * 4u);
            for (uint32_t r = 0; r < kTileSize; ++r) {
                std::memcpy(loaded.bytes.data() + static_cast<size_t>(r) * kTileSize * 4u,
                            src + static_cast<size_t>(r) * rb, static_cast<size_t>(kTileSize) * 4u);
            }
            it = cache_.emplace(key, std::move(loaded)).first;
        }
        if (!it->second.loaded) {
            return nullptr;
        }
        return it->second.bytes.data() +
               (static_cast<size_t>(uy % kTileSize) * kTileSize + ux % kTileSize) * 4u;
    }

  private:
    struct Loaded {
        bool loaded = false;
        std::vector<uint8_t> bytes;
    };
    const TileGrid& grid_;
    std::map<uint32_t, Loaded> cache_;
};

// 所有图层采样源：flatten 连续平面
class FlatSource : public WandSource {
  public:
    FlatSource(std::shared_ptr<std::vector<uint8_t>> rgba, uint32_t w)
        : rgba_(std::move(rgba)), w_(w) {
    }

    const uint8_t* at(int x, int y) override {
        if (x < 0 || y < 0) {
            return nullptr;
        }
        const uint32_t ux = static_cast<uint32_t>(x);
        const uint32_t uy = static_cast<uint32_t>(y);
        const size_t idx = (static_cast<size_t>(uy) * w_ + ux) * 4u;
        if (idx + 3 >= rgba_->size()) {
            return nullptr;
        }
        return rgba_->data() + idx;
    }

  private:
    std::shared_ptr<std::vector<uint8_t>> rgba_;
    uint32_t w_;
};

// N×N 均值种子色（sampleSize 奇数；clamp 边界；alpha 参与均值）
void sampleSeed(WandSource& src, int cx, int cy, int n, uint8_t out[4]) {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;
    int cnt = 0;
    const int half = n / 2;
    for (int dy = -half; dy <= half; ++dy) {
        for (int dx = -half; dx <= half; ++dx) {
            const uint8_t* p = src.at(cx + dx, cy + dy);
            if (p == nullptr) {
                continue;
            }
            r += p[0];
            g += p[1];
            b += p[2];
            a += p[3];
            ++cnt;
        }
    }
    if (cnt == 0) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    out[0] = static_cast<uint8_t>((r + cnt / 2) / cnt);
    out[1] = static_cast<uint8_t>((g + cnt / 2) / cnt);
    out[2] = static_cast<uint8_t>((b + cnt / 2) / cnt);
    out[3] = static_cast<uint8_t>((a + cnt / 2) / cnt);
}

}  // namespace

std::unique_ptr<WandSource> makeTileSource(const TileGrid& grid) {
    return std::make_unique<TileSource>(grid);
}

std::unique_ptr<WandSource> makeFlatSource(std::shared_ptr<std::vector<uint8_t>> rgba,
                                           uint32_t w, uint32_t /*h*/) {
    return std::make_unique<FlatSource>(std::move(rgba), w);
}

std::shared_ptr<const TileGrid> wandMask(WandSource& src, uint32_t docW, uint32_t docH,
                                         int sampleX, int sampleY, int tolerance,
                                         bool contiguous, int sampleSize) {
    if (docW == 0 || docH == 0) {
        return nullptr;
    }
    if (sampleX < 0 || sampleY < 0 || sampleX >= static_cast<int>(docW) ||
        sampleY >= static_cast<int>(docH)) {
        return nullptr;
    }
    const int n = std::min(101, std::max(1, sampleSize | 1));  // 强制奇数
    uint8_t seed[4];
    sampleSeed(src, sampleX, sampleY, n, seed);
    const int tol = std::min(255, std::max(0, tolerance));
    const int w = static_cast<int>(docW);
    const int h = static_cast<int>(docH);

    // 稀疏输出：命中灰度直接写 builder（0 不落瓦片）；位图 visited 防重复入队
    TileGridBuilder builder((docW + kTileSize - 1u) / kTileSize,
                            (docH + kTileSize - 1u) / kTileSize);
    auto writeHit = [&](int x, int y) {
        uint8_t* px = builder.ensureTile(static_cast<uint32_t>(x) / kTileSize,
                                         static_cast<uint32_t>(y) / kTileSize).mapCpuWrite();
        uint8_t* p = px + static_cast<size_t>(y % kTileSize) * kTileSize * 4 +
                     static_cast<size_t>(x % kTileSize) * 4;
        p[0] = p[1] = p[2] = 255;
        p[3] = 255;
    };

    size_t total = 0;
    if (contiguous) {
        // BFS 4 邻接；visited 位图（docW×docH bit ≈ 全图像素/8）
        const size_t bits = static_cast<size_t>(w) * h;
        std::vector<uint8_t> visited((bits + 7) / 8, 0);
        auto markVisited = [&](size_t idx) {
            visited[idx >> 3] |= static_cast<uint8_t>(1u << (idx & 7));
        };
        auto isVisited = [&](size_t idx) -> bool {
            return (visited[idx >> 3] >> (idx & 7)) & 1u;
        };
        std::queue<size_t> q;
        const size_t s0 = static_cast<size_t>(sampleY) * w + sampleX;
        markVisited(s0);
        const uint8_t* sp = src.at(sampleX, sampleY);
        if (sp != nullptr && withinTolerance(sp, seed, tol)) {
            writeHit(sampleX, sampleY);
            ++total;
            q.push(s0);
        }
        while (!q.empty()) {
            const size_t at = q.front();
            q.pop();
            const int x = static_cast<int>(at % w);
            const int y = static_cast<int>(at / w);
            const int dxs[4] = {1, -1, 0, 0};
            const int dys[4] = {0, 0, 1, -1};
            for (int d = 0; d < 4; ++d) {
                const int nx = x + dxs[d];
                const int ny = y + dys[d];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                    continue;
                }
                const size_t nAt = static_cast<size_t>(ny) * w + nx;
                if (isVisited(nAt)) {
                    continue;
                }
                markVisited(nAt);
                const uint8_t* p = src.at(nx, ny);
                if (p != nullptr && withinTolerance(p, seed, tol)) {
                    writeHit(nx, ny);
                    ++total;
                    q.push(nAt);
                }
            }
        }
    } else {
        // 全图扫描：逐瓦片顺序访问（TileSource 缓存友好；FlatSource 直读）
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const uint8_t* p = src.at(x, y);
                if (p != nullptr && withinTolerance(p, seed, tol)) {
                    writeHit(x, y);
                    ++total;
                }
            }
        }
    }
    if (total == 0) {
        return nullptr;
    }
    return builder.publish(1);
}

}  // namespace tiles
}  // namespace montage
