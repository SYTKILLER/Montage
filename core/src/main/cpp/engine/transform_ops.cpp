#include "engine/transform_ops.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace montage {

namespace {

constexpr uint32_t kTile = kTileSize;

// ---- 网格虚拟大图行读写（跨瓦片拼接；缺瓦片零填充/跳过写） ----

void gridReadRow(const TileGrid& g, uint32_t rowY, uint8_t* dst) {
    const uint32_t w = g.cols * kTile;
    std::memset(dst, 0, static_cast<size_t>(w) * 4u);
    if (rowY >= g.rows * kTile) {
        return;
    }
    const uint32_t ty = rowY / kTile;
    const uint32_t inY = rowY % kTile;
    for (uint32_t tx = 0; tx < g.cols; ++tx) {
        auto it = g.tiles.find(ty * g.cols + tx);
        if (it == g.tiles.end() || it->second == nullptr || it->second->pixels == nullptr) {
            continue;
        }
        const uint8_t* src = it->second->pixels->mapCpu();
        if (src == nullptr) {
            continue;
        }
        const uint32_t rb = it->second->pixels->rowBytes();
        std::memcpy(dst + static_cast<size_t>(tx) * kTile * 4u,
                    src + static_cast<size_t>(inY) * rb, kTile * 4u);
    }
}

void gridWriteRow(const TileGrid& g, uint32_t rowY, const uint8_t* src) {
    if (rowY >= g.rows * kTile) {
        return;
    }
    const uint32_t ty = rowY / kTile;
    const uint32_t inY = rowY % kTile;
    for (uint32_t tx = 0; tx < g.cols; ++tx) {
        auto it = g.tiles.find(ty * g.cols + tx);
        if (it == g.tiles.end() || it->second == nullptr || it->second->pixels == nullptr) {
            continue;
        }
        uint8_t* dst = it->second->pixels->mapCpuWrite();
        if (dst == nullptr) {
            continue;
        }
        const uint32_t rb = it->second->pixels->rowBytes();
        std::memcpy(dst + static_cast<size_t>(inY) * rb,
                    src + static_cast<size_t>(tx) * kTile * 4u, kTile * 4u);
    }
}

// 深拷贝网格（保持不可变 COW 纪律；EngineBuffer 构造为零填充）
std::shared_ptr<TileGrid> copyGrid(const TileGrid& g) {
    auto copy = std::make_shared<TileGrid>();
    copy->cols = g.cols;
    copy->rows = g.rows;
    copy->revision = g.revision + 1;
    for (const auto& [idx, tile] : g.tiles) {
        auto t = std::make_shared<Tile>();
        auto* buf = new EngineBuffer(kTile, kTile);
        std::memcpy(buf->mapCpuWrite(), tile->pixels->mapCpu(),
                    static_cast<size_t>(tile->pixels->rowBytes()) * kTile);
        t->pixels.reset(buf);
        copy->tiles.emplace(idx, std::move(t));
    }
    return copy;
}

// 行数据写入 builder（按瓦片段 ensureTile；全零段跳过保持稀疏）
void builderWriteRow(TileGridBuilder& b, uint32_t rowY, const uint8_t* src, uint32_t cols) {
    for (uint32_t tx = 0; tx < cols; ++tx) {
        bool any = false;
        for (uint32_t k = 0; k < kTile; ++k) {
            if (src[(tx * kTile + k) * 4 + 3] != 0) {
                any = true;
                break;
            }
        }
        if (!any) {
            continue;
        }
        uint8_t* dst = b.ensureTile(tx, rowY / kTile).mapCpuWrite() + (rowY % kTile) * kTile * 4;
        std::memcpy(dst, src + tx * kTile * 4, kTile * 4);
    }
}

// 网格水平镜像（builder 重建：稀疏网格镜像目标瓦片可能不存在，就地写会丢数据——M8a 实证修复）
std::shared_ptr<const TileGrid> flipGridH(const TileGrid& g) {
    const uint32_t w = g.cols * kTile;
    const uint32_t h = g.rows * kTile;
    TileGridBuilder builder(g.cols, g.rows);
    std::vector<uint8_t> row(w * 4);
    for (uint32_t y = 0; y < h; ++y) {
        gridReadRow(g, y, row.data());
        std::reverse(reinterpret_cast<uint32_t*>(row.data()),
                     reinterpret_cast<uint32_t*>(row.data() + w * 4));
        builderWriteRow(builder, y, row.data(), g.cols);
    }
    return builder.publish(g.revision + 1);
}

// 网格垂直镜像（builder 重建）
std::shared_ptr<const TileGrid> flipGridV(const TileGrid& g) {
    const uint32_t w = g.cols * kTile;
    const uint32_t h = g.rows * kTile;
    TileGridBuilder builder(g.cols, g.rows);
    std::vector<uint8_t> row(w * 4);
    for (uint32_t y = 0; y < h; ++y) {
        gridReadRow(g, h - 1 - y, row.data());
        builderWriteRow(builder, y, row.data(), g.cols);
    }
    return builder.publish(g.revision + 1);
}

// 网格 90° 旋转（CW: (x,y)→(H-1-y,x)；输出 cols/rows 互换；透明像素保持稀疏）
std::shared_ptr<const TileGrid> rotateGrid90(const TileGrid& g, bool clockwise) {
    const uint32_t w = g.cols * kTile;
    const uint32_t h = g.rows * kTile;
    const uint32_t nw = clockwise ? h : w;
    const uint32_t nh = clockwise ? w : h;
    TileGridBuilder builder(nw / kTile, nh / kTile);
    std::vector<uint8_t> rowIn(w * 4);
    for (uint32_t y = 0; y < h; ++y) {
        gridReadRow(g, y, rowIn.data());
        for (uint32_t x = 0; x < w; ++x) {
            if (rowIn[x * 4 + 3] == 0) {
                continue;
            }
            uint32_t nx = 0;
            uint32_t ny = 0;
            if (clockwise) {
                nx = h - 1 - y;
                ny = x;
            } else {
                nx = y;
                ny = w - 1 - x;
            }
            uint8_t* dst = builder.ensureTile(nx / kTile, ny / kTile).mapCpuWrite() +
                           (ny % kTile) * kTile * 4 + (nx % kTile) * 4;
            std::memcpy(dst, rowIn.data() + x * 4, 4);
        }
    }
    return builder.publish(g.revision + 1);
}

// 蒙版深变换：像素核 + offset 重映射（patch 在图层局部坐标系）
void transformMask(Layer& l, bool horizontal, bool rotate, bool clockwise,
                   uint32_t oldW, uint32_t oldH) {
    if (l.mask == nullptr) {
        return;
    }
    auto m = std::make_shared<LayerMask>(*l.mask);
    if (m->pixels != nullptr) {
        if (rotate) {
            m->pixels = rotateGrid90(*m->pixels, clockwise);
        } else if (horizontal) {
            m->pixels = flipGridH(*m->pixels);
        } else {
            m->pixels = flipGridV(*m->pixels);
        }
    }
    // offset 重映射：patch 覆盖图层局部区间 [off, off+patch)
    //   翻转 H：局部 (x)→(LW-1-x) → off' = LW - off - patchW
    //   翻转 V：off' = LH - off - patchH
    //   旋转 CW：(x,y)→(LH-1-y,x) → offX' = LH - offY - patchH；offY' = offX
    //   旋转 CCW：(x,y)→(y,LW-1-x) → offX' = offY；offY' = LW - offX - patchW
    //   旋转 180 由两次 90 合成，无需单列
    // 注意：LW/LH 用旋转前的图层网格尺寸（oldW/oldH 方向的网格维）
    const auto gridDim = [&](bool width) -> int {
        const TileGrid* g = l.pixels.get();  // 已变换后的新网格
        if (g == nullptr) {
            return 0;
        }
        if (!rotate) {
            return static_cast<int>((width ? g->cols : g->rows) * kTile);
        }
        // 旋转后网格维互换：新 cols*256 = 旧 rows*256（= 旧方向的"高"）
        return static_cast<int>((width ? g->rows : g->cols) * kTile);
    };
    const int patchW = static_cast<int>(m->width);
    const int patchH = static_cast<int>(m->height);
    if (rotate) {
        // 旋转前图层网格：宽 = oldW 方向网格（= 变换后新网格的 rows 维）
        const int preW = gridDim(true);
        const int preH = gridDim(false);
        const int ox = m->offsetX;
        const int oy = m->offsetY;
        if (clockwise) {
            m->offsetX = preH - oy - patchH;
            m->offsetY = ox;
        } else {
            m->offsetX = oy;
            m->offsetY = preW - ox - patchW;
        }
    } else if (horizontal) {
        const int lw = gridDim(true);
        m->offsetX = lw - m->offsetX - patchW;
    } else {
        const int lh = gridDim(false);
        m->offsetY = lh - m->offsetY - patchH;
    }
    l.mask = std::move(m);
}

}  // namespace

bool visibleContentBBox(const Document& doc, int& x, int& y, int& w, int& h) {
    int bx0 = INT32_MAX;
    int by0 = INT32_MAX;
    int bx1 = INT32_MIN;
    int by1 = INT32_MIN;
    for (const Layer& l : doc.layers) {
        if (!l.visible || l.adjustment != nullptr) {
            continue;
        }
        const std::shared_ptr<const TileGrid> g = l.effectivePixels();
        if (g == nullptr || g->tiles.empty()) {
            continue;
        }
        uint32_t tx0 = UINT32_MAX;
        uint32_t ty0 = UINT32_MAX;
        uint32_t tx1 = 0;
        uint32_t ty1 = 0;
        for (const auto& [idx, tile] : g->tiles) {
            (void)tile;
            tx0 = std::min(tx0, idx % g->cols);
            ty0 = std::min(ty0, idx / g->cols);
            tx1 = std::max(tx1, idx % g->cols);
            ty1 = std::max(ty1, idx / g->cols);
        }
        const int lx = static_cast<int>(l.transform.originX) + static_cast<int>(tx0 * kTile);
        const int ly = static_cast<int>(l.transform.originY) + static_cast<int>(ty0 * kTile);
        const int lw = static_cast<int>((tx1 - tx0 + 1) * kTile);
        const int lh = static_cast<int>((ty1 - ty0 + 1) * kTile);
        bx0 = std::min(bx0, lx);
        by0 = std::min(by0, ly);
        bx1 = std::max(bx1, lx + lw);
        by1 = std::max(by1, ly + lh);
    }
    if (bx0 == INT32_MAX) {
        return false;
    }
    x = bx0;
    y = by0;
    w = bx1 - bx0;
    h = by1 - by0;
    return true;
}

bool resizeCanvas(Document& doc, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (x == 0 && y == 0 && static_cast<uint32_t>(w) == doc.width &&
        static_cast<uint32_t>(h) == doc.height) {
        return false;
    }
    for (Layer& l : doc.layers) {
        // origin 平移（新文档原点 = 旧 (x,y)）；蒙版 offset 是图层局部坐标，随层走不动
        l.transform.originX -= x;
        l.transform.originY -= y;
    }
    doc.width = static_cast<uint32_t>(w);
    doc.height = static_cast<uint32_t>(h);
    doc.selection = nullptr;  // 会话态语义：画布重设即清除（M8a 简化登记）
    return true;
}

bool flipDocument(Document& doc, bool horizontal) {
    if (doc.width == 0 || doc.height == 0) {
        return false;
    }
    for (Layer& l : doc.layers) {
        if (l.pixels != nullptr) {
            if (horizontal) {
                l.pixels = flipGridH(*l.pixels);
                const double gw = static_cast<double>(l.pixels->cols) * kTile;
                l.transform.originX =
                    static_cast<double>(doc.width) - l.transform.originX - gw;
            } else {
                l.pixels = flipGridV(*l.pixels);
                const double gh = static_cast<double>(l.pixels->rows) * kTile;
                l.transform.originY =
                    static_cast<double>(doc.height) - l.transform.originY - gh;
            }
        }
        transformMask(l, horizontal, false, false, doc.width, doc.height);
        if (l.mask != nullptr && l.mask->enabled && l.pixels != nullptr) {
            l.render = composeMasked(l.pixels, *l.mask);
        }
    }
    doc.selection = nullptr;
    return true;
}

bool rotateDocument90(Document& doc, bool clockwise) {
    if (doc.width == 0 || doc.height == 0) {
        return false;
    }
    const uint32_t oldW = doc.width;
    const uint32_t oldH = doc.height;
    for (Layer& l : doc.layers) {
        const double ox = l.transform.originX;
        const double oy = l.transform.originY;
        if (l.pixels != nullptr) {
            l.pixels = rotateGrid90(*l.pixels, clockwise);
            const double gh = static_cast<double>(l.pixels->rows) * kTile;
            const double gw = static_cast<double>(l.pixels->cols) * kTile;
            if (clockwise) {
                l.transform.originX = static_cast<double>(oldH) - oy - gh;
                l.transform.originY = ox;
            } else {
                l.transform.originX = oy;
                l.transform.originY = static_cast<double>(oldW) - ox - gw;
            }
        }
        transformMask(l, false, true, clockwise, oldW, oldH);
        if (l.mask != nullptr && l.mask->enabled && l.pixels != nullptr) {
            l.render = composeMasked(l.pixels, *l.mask);
        }
    }
    doc.width = oldH;
    doc.height = oldW;
    doc.selection = nullptr;
    return true;
}

bool translateSelection(Document& doc, int dx, int dy) {
    if (doc.selection == nullptr || (dx == 0 && dy == 0)) {
        return false;
    }
    const TileGrid& g = *doc.selection;
    const uint32_t w = g.cols * kTile;
    const uint32_t h = g.rows * kTile;
    TileGridBuilder builder(g.cols, g.rows);
    std::vector<uint8_t> row4(w * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const int srcY = static_cast<int>(y) - dy;
        if (srcY < 0 || srcY >= static_cast<int>(h)) {
            continue;
        }
        gridReadRow(g, static_cast<uint32_t>(srcY), row4.data());
        for (uint32_t x = 0; x < w; ++x) {
            const int sx = static_cast<int>(x) - dx;  // dest[x]=src[x-dx]：内容 +dx 平移（M8a 单测修正）
            if (sx < 0 || sx >= static_cast<int>(w)) {
                continue;
            }
            if (row4[sx * 4] == 0) {
                continue;  // 灰度 0 保持稀疏
            }
            const uint8_t gray = row4[sx * 4];
            uint8_t* dst = builder.ensureTile(x / kTile, y / kTile).mapCpuWrite() +
                           (y % kTile) * kTile * 4 + (x % kTile) * 4;
            dst[0] = dst[1] = dst[2] = gray;
            dst[3] = 255;
        }
    }
    doc.selection = builder.publish(g.revision + 1);
    return true;
}

}  // namespace montage
