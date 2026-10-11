#include "engine/clipboard_ops.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "io/image_export.h"

namespace montage {

namespace {

constexpr uint32_t kTile = kTileSize;

// 单像素读取（网格局部坐标；缺瓦片/出界返回 nullptr）
const uint8_t* flattenGridCell(const TileGrid& g, int gx, int gy) {
    if (gx < 0 || gy < 0) {
        return nullptr;
    }
    const uint32_t ux = static_cast<uint32_t>(gx);
    const uint32_t uy = static_cast<uint32_t>(gy);
    auto it = g.tiles.find((uy / kTile) * g.cols + (ux / kTile));
    if (it == g.tiles.end() || it->second == nullptr || it->second->pixels == nullptr) {
        return nullptr;
    }
    return it->second->pixels->mapCpu() +
           (static_cast<size_t>(uy % kTile) * kTile + ux % kTile) * 4u;
}

// 稀疏网格 → 连续 RGBA 平面（w*h*4；缺瓦片零填充）
std::vector<uint8_t> flattenGrid(const TileGrid& g, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4u, 0);
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
        const uint32_t rb = tile->pixels->rowBytes();
        for (int32_t r = 0; r < copyH; ++r) {
            std::memcpy(out.data() + (static_cast<size_t>(y0 + r) * w + x0) * 4u,
                        src + static_cast<size_t>(r) * rb, static_cast<size_t>(copyW) * 4u);
        }
    }
    return out;
}

}  // namespace

bool selectionBBox(const Document& doc, int& x, int& y, int& w, int& h) {
    if (doc.selection == nullptr) {
        return false;
    }
    const TileGrid& g = *doc.selection;
    int minX = static_cast<int>(g.cols * kTile);
    int minY = static_cast<int>(g.rows * kTile);
    int maxX = -1;
    int maxY = -1;
    for (const auto& [key, tile] : g.tiles) {
        if (tile == nullptr || tile->pixels == nullptr) {
            continue;
        }
        const uint32_t tx = key % g.cols;
        const uint32_t ty = key / g.cols;
        const uint8_t* px = tile->pixels->mapCpu();
        for (uint32_t r = 0; r < kTile; ++r) {
            for (uint32_t c = 0; c < kTile; ++c) {
                if (px[(static_cast<size_t>(r) * kTile + c) * 4u] != 0) {
                    const int gx = static_cast<int>(tx * kTile + c);
                    const int gy = static_cast<int>(ty * kTile + r);
                    minX = std::min(minX, gx);
                    minY = std::min(minY, gy);
                    maxX = std::max(maxX, gx);
                    maxY = std::max(maxY, gy);
                }
            }
        }
    }
    if (maxX < 0) {
        return false;
    }
    x = minX;
    y = minY;
    w = maxX - minX + 1;
    h = maxY - minY + 1;
    return true;
}

bool copySelectionToClipboard(const Document& doc, bool merged, ClipBuffer& out) {
    int bx = 0;
    int by = 0;
    int bw = 0;
    int bh = 0;
    if (!selectionBBox(doc, bx, by, bw, bh)) {
        return false;
    }
    const Layer* l = nullptr;
    for (const Layer& layer : doc.layers) {
        if (layer.id == doc.activeId) {
            l = &layer;
            break;
        }
    }
    if (!merged && (l == nullptr || l->effectivePixels() == nullptr)) {
        return false;
    }

    std::vector<uint8_t> region(static_cast<size_t>(bw) * bh * 4u, 0);
    if (merged) {
        // 合并拷贝：CPU flatten 区域版（直 alpha）
        const io::FlattenJob job = io::BuildFlattenJob(doc, bx, by, bw, bh);
        const io::FlattenResult res = io::FlattenVisible(job, bx, by, bw, bh);
        if (!res.ok || res.rgba.size() != region.size()) {
            return false;
        }
        region = std::move(res.rgba);
    } else {
        // 单层拷贝：effectivePixels（蒙版已合成）网格域 → 选区包围盒
        const TileGrid& grid = *l->effectivePixels();
        const int gridW = static_cast<int>(grid.cols * kTile);
        const int gridH = static_cast<int>(grid.rows * kTile);
        const double orgX = l->transform.originX;
        const double orgY = l->transform.originY;
        for (int r = 0; r < bh; ++r) {
            const int gy = by + r - static_cast<int>(orgY);
            if (gy < 0 || gy >= gridH) {
                continue;
            }
            for (int c = 0; c < bw; ++c) {
                const int gx = bx + c - static_cast<int>(orgX);
                if (gx < 0 || gx >= gridW) {
                    continue;
                }
                const uint8_t* s = flattenGridCell(grid, gx, gy);
                if (s != nullptr) {
                    std::memcpy(region.data() + (static_cast<size_t>(r) * bw + c) * 4u, s, 4u);
                }
            }
        }
    }

    // 掩码裁剪：alpha × gray/255（直 alpha 域；像素 rgb 不预乘——与引擎瓦片约定一致）
    const std::vector<uint8_t> maskPlane = flattenGrid(*doc.selection,
                                                       doc.selection->cols * kTile,
                                                       doc.selection->rows * kTile);
    const int mw = static_cast<int>(doc.selection->cols * kTile);
    for (int r = 0; r < bh; ++r) {
        const int my = by + r;
        if (my < 0 || my >= static_cast<int>(doc.selection->rows * kTile)) {
            continue;
        }
        for (int c = 0; c < bw; ++c) {
            const int mx = bx + c;
            if (mx < 0 || mx >= mw) {
                continue;
            }
            const uint8_t gray = maskPlane[(static_cast<size_t>(my) * mw + mx) * 4u];
            uint8_t* p = region.data() + (static_cast<size_t>(r) * bw + c) * 4u;
            if (gray < 255) {
                p[3] = static_cast<uint8_t>((p[3] * gray + 127) / 255);
            }
        }
    }

    out.width = static_cast<uint32_t>(bw);
    out.height = static_cast<uint32_t>(bh);
    out.docX = bx;
    out.docY = by;
    out.pixels = std::move(region);
    return true;
}

bool eraseSelectionPixels(Document& doc) {
    if (doc.selection == nullptr) {
        return false;
    }
    Layer* l = nullptr;
    for (Layer& layer : doc.layers) {
        if (layer.id == doc.activeId) {
            l = &layer;
            break;
        }
    }
    if (l == nullptr || l->pixels == nullptr) {
        return false;
    }
    const TileGrid& sel = *doc.selection;
    const TileGrid& grid = *l->pixels;
    auto out = std::make_shared<TileGrid>();
    out->cols = grid.cols;
    out->rows = grid.rows;
    out->revision = grid.revision + 1;
    bool changed = false;
    for (const auto& [key, tile] : grid.tiles) {
        if (tile == nullptr || tile->pixels == nullptr) {
            continue;
        }
        const uint32_t tx = key % grid.cols;
        const uint32_t ty = key / grid.cols;
        // 与选区掩码求交：掩码覆盖本瓦片才需要逐像素处理
        auto selIt = sel.tiles.find(ty * sel.cols + tx);
        const bool selTilePresent = tx < sel.cols && ty < sel.rows && selIt != sel.tiles.end();
        if (!selTilePresent) {
            out->tiles[key] = tile;  // 无选区覆盖：原瓦片共享
            continue;
        }
        const uint8_t* selPx = selIt->second->pixels->mapCpu();
        const uint8_t* srcPx = tile->pixels->mapCpu();
        const uint32_t rb = tile->pixels->rowBytes();
        auto buf = std::make_unique<EngineBuffer>(kTile, kTile);
        uint8_t* dst = buf->mapCpuWrite();
        bool tileChanged = false;
        for (uint32_t i = 0; i < kTile * kTile; ++i) {
            uint8_t* d = dst + i * 4u;
            const uint8_t* s = srcPx + i * 4u;
            const uint8_t gray = selPx[i * 4u];
            if (gray >= 255) {
                if (s[3] != 0) {  // 已透明像素不重复标记（no-op 语义）
                    d[0] = d[1] = d[2] = d[3] = 0;
                    tileChanged = true;
                    changed = true;
                } else {
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                    d[3] = s[3];
                }
            } else if (gray > 0) {
                // 灰度选区：alpha × (1 - gray/255)（直 alpha 域）
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = static_cast<uint8_t>((s[3] * (255 - gray) + 127) / 255);
                if (d[3] != s[3]) {
                    tileChanged = true;
                    changed = true;
                }
            } else {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = s[3];
            }
        }
        if (!tileChanged) {
            out->tiles[key] = tile;
            continue;
        }
        (void)rb;
        auto t = std::make_shared<Tile>();
        t->pixels = std::move(buf);
        out->tiles[key] = std::move(t);
    }
    l->pixels = std::move(out);
    if (l->mask != nullptr && l->mask->enabled) {
        l->render = composeMasked(l->pixels, *l->mask);
    }
    return changed;
}

LayerId pasteClipboardAsLayer(Document& doc, const ClipBuffer& clip, LayerId newId) {
    if (clip.width == 0 || clip.height == 0 || clip.pixels.empty()) {
        return 0;
    }
    Layer layer;
    layer.id = newId;
    layer.name = "图层 " + std::to_string(newId);
    layer.visible = true;
    layer.opacity = 1.0;
    layer.blendMode = BlendMode::Normal;
    layer.transform.originX = clip.docX;
    layer.transform.originY = clip.docY;

    // 像素 → 稀疏网格（alpha 0 的像素不落瓦片保持稀疏）
    const uint32_t cols = (clip.width + kTile - 1u) / kTile;
    const uint32_t rows = (clip.height + kTile - 1u) / kTile;
    TileGridBuilder builder(cols, rows);
    for (uint32_t r = 0; r < clip.height; ++r) {
        for (uint32_t c = 0; c < clip.width; ++c) {
            const uint8_t* s = clip.pixels.data() + (static_cast<size_t>(r) * clip.width + c) * 4u;
            if (s[3] == 0) {
                continue;
            }
            uint8_t* d = builder.ensureTile(c / kTile, r / kTile).mapCpuWrite() +
                         (r % kTile) * kTile * 4 + c % kTile * 4;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = s[3];
        }
    }
    layer.pixels = builder.publish(1);

    // 插入到活动层之上（栈序：active 索引 +1）
    int insertAt = static_cast<int>(doc.layers.size());
    for (size_t i = 0; i < doc.layers.size(); ++i) {
        if (doc.layers[i].id == doc.activeId) {
            insertAt = static_cast<int>(i) + 1;
            break;
        }
    }
    doc.layers.insert(doc.layers.begin() + insertAt, std::move(layer));
    doc.activeId = newId;
    return newId;
}

}  // namespace montage
