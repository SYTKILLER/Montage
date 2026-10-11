#include "engine/shape_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "tiles/selection_ops.h"
#include "tiles/shape_mask.h"

namespace montage {
namespace {

void srcOver(uint8_t px[4], float sr, float sg, float sb, float sa) {
    if (sa <= 0.0f) {
        return;
    }
    const float da = px[3] / 255.0f;
    const float oa = sa + da * (1.0f - sa);
    if (oa <= 0.0001f) {
        px[0] = px[1] = px[2] = px[3] = 0;
        return;
    }
    const float srcP[3] = {sr * sa, sg * sa, sb * sa};
    for (int c = 0; c < 3; ++c) {
        const float d = px[static_cast<size_t>(c)] / 255.0f * da * (1.0f - sa);
        px[static_cast<size_t>(c)] =
            static_cast<uint8_t>(std::lround(std::min(1.0f, (srcP[c] + d) / oa) * 255.0f));
    }
    px[3] = static_cast<uint8_t>(std::lround(std::min(1.0f, oa) * 255.0f));
}

// publish 只含写入瓦片——并回原网格（未触碰瓦片保留原共享 Tile*）
std::shared_ptr<const TileGrid> mergeWithBase(const std::shared_ptr<const TileGrid>& base,
                                              std::shared_ptr<const TileGrid> written,
                                              uint32_t cols, uint32_t rows, uint64_t rev) {
    auto outGrid = std::make_shared<TileGrid>();
    outGrid->cols = cols;
    outGrid->rows = rows;
    outGrid->revision = rev;
    if (base != nullptr) {
        outGrid->tiles = base->tiles;
    }
    for (auto& kv : written->tiles) {
        outGrid->tiles[kv.first] = std::move(kv.second);
    }
    return outGrid;
}

}  // namespace

bool rasterizeShape(Document& doc, LayerId layerId, const ShapeSpec& spec,
                    const float fg[3], const float bg[3], std::string& err) {
    Layer* layer = nullptr;
    for (Layer& l : doc.layers) {
        if (l.id == layerId) {
            layer = &l;
            break;
        }
    }
    if (layer == nullptr) {
        err = "shape: layer not found";
        return false;
    }
    if (doc.width == 0 || doc.height == 0) {
        err = "shape: no document";
        return false;
    }
    if (!spec.fill && !spec.stroke && spec.kind != ShapeSpec::Kind::Line) {
        err = "shape: nothing to draw";
        return false;
    }

    // ① 填充掩码（闭合形状；直线恒描边）
    std::shared_ptr<const TileGrid> fillMask;
    if (spec.kind != ShapeSpec::Kind::Line && spec.fill) {
        const bool flip = spec.x1 < spec.x0 || spec.y1 < spec.y0;
        const double x0 = std::min(spec.x0, spec.x1);
        const double y0 = std::min(spec.y0, spec.y1);
        const double w = std::abs(spec.x1 - spec.x0);
        const double h = std::abs(spec.y1 - spec.y0);
        (void)flip;
        switch (spec.kind) {
            case ShapeSpec::Kind::Rect:
                fillMask = tiles::rectMask(x0, y0, w, h, doc.width, doc.height);
                break;
            case ShapeSpec::Kind::RoundedRect:
                fillMask = tiles::roundedRectMask(spec.x0, spec.y0, spec.x1, spec.y1,
                                                  spec.cornerRadius, doc.width, doc.height);
                break;
            case ShapeSpec::Kind::Ellipse:
                fillMask = tiles::ellipseMask(x0, y0, w, h, doc.width, doc.height);
                break;
            default:
                break;
        }
        if (fillMask == nullptr) {
            err = "shape: empty drag rect";
            return false;
        }
    }

    // ② 描边掩码：闭合形状 = 轮廓带（border 核：dilate−erode，方形 SE 方形近似登记）；
    //    直线 = 胶囊带
    std::shared_ptr<const TileGrid> strokeMask;
    const bool wantStroke = spec.stroke || spec.kind == ShapeSpec::Kind::Line;
    if (wantStroke) {
        const int r = std::max(1, static_cast<int>(std::lround(spec.strokeWidth / 2.0)));
        if (spec.kind == ShapeSpec::Kind::Line) {
            strokeMask = tiles::capsuleMask(spec.x0, spec.y0, spec.x1, spec.y1,
                                            spec.strokeWidth, doc.width, doc.height);
            if (strokeMask == nullptr) {
                err = "shape: degenerate line";
                return false;
            }
        } else {
            // borderSelection 读 doc.selection——临时换入填充掩码（调用方持 docMutex）
            std::shared_ptr<const TileGrid> saved = doc.selection;
            doc.selection = fillMask;
            strokeMask = tiles::borderSelection(doc, r);
            doc.selection = saved;
            if (strokeMask == nullptr) {
                err = "shape: stroke band empty";
                return false;
            }
        }
    }

    // ③ 着色落层：stroke ∪ fill 触碰瓦片（fill 先、stroke 后 = 描边叠填充之上），
    //    alpha × 选区灰度；未触碰瓦片 memcmp 保留共享
    std::shared_ptr<const TileGrid> base = layer->pixels;
    uint32_t cols;
    uint32_t rows;
    int originX;
    int originY;
    if (base != nullptr) {
        cols = base->cols;
        rows = base->rows;
        originX = static_cast<int>(std::lround(layer->transform.originX));
        originY = static_cast<int>(std::lround(layer->transform.originY));
    } else {
        cols = (doc.width + kTileSize - 1u) / kTileSize;
        rows = (doc.height + kTileSize - 1u) / kTileSize;
        originX = 0;
        originY = 0;
    }
    const TileGrid* selGrid = doc.selection != nullptr ? doc.selection.get() : nullptr;
    std::vector<const TileGrid*> domains;
    if (fillMask != nullptr) {
        domains.push_back(fillMask.get());
    }
    if (strokeMask != nullptr) {
        domains.push_back(strokeMask.get());
    }

    TileGridBuilder builder(cols, rows);
    std::vector<uint8_t> out(static_cast<size_t>(kTileSize) * kTileSize * 4u);
    std::vector<bool> touchedTiles(static_cast<size_t>(cols) * rows, false);
    for (const TileGrid* m : domains) {
        for (const auto& kv : m->tiles) {
            if (kv.first < touchedTiles.size()) {
                touchedTiles[kv.first] = true;
            }
        }
    }
    bool anyTile = false;
    for (size_t key = 0; key < touchedTiles.size(); ++key) {
        if (!touchedTiles[key]) {
            continue;
        }
        const uint32_t tx = static_cast<uint32_t>(key % cols);
        const uint32_t ty = static_cast<uint32_t>(key / cols);
        std::fill(out.begin(), out.end(), static_cast<uint8_t>(0));
        std::shared_ptr<const Tile> orig;
        if (base != nullptr) {
            auto it = base->tiles.find(static_cast<uint32_t>(key));
            if (it != base->tiles.end() && it->second != nullptr) {
                orig = it->second;
                const uint8_t* src = orig->pixels->mapCpu();
                if (src != nullptr) {
                    std::memcpy(out.data(), src, out.size());
                    orig->pixels->unmap();
                }
            }
        }
        for (uint32_t rr = 0; rr < kTileSize; ++rr) {
            const int docY = originY + static_cast<int>(ty * kTileSize + rr);
            for (uint32_t cc = 0; cc < kTileSize; ++cc) {
                const int docX = originX + static_cast<int>(tx * kTileSize + cc);
                float selA = 1.0f;
                if (selGrid != nullptr) {
                    selA = static_cast<float>(selectionGrayAt(*selGrid, docX, docY)) / 255.0f;
                }
                uint8_t* px = out.data() + (static_cast<size_t>(rr) * kTileSize + cc) * 4u;
                if (fillMask != nullptr) {
                    const uint8_t g = selectionGrayAt(*fillMask, docX, docY);
                    if (g > 0) {
                        srcOver(px, fg[0], fg[1], fg[2],
                                static_cast<float>(g) / 255.0f * selA);
                    }
                }
                if (strokeMask != nullptr) {
                    const uint8_t g = selectionGrayAt(*strokeMask, docX, docY);
                    if (g > 0) {
                        srcOver(px, bg[0], bg[1], bg[2],
                                static_cast<float>(g) / 255.0f * selA);
                    }
                }
            }
        }
        // 与原瓦片相同则不落（fill 掩码着色恒改变像素，此检查主要省空层无选区端）
        bool changed = orig == nullptr;
        if (orig != nullptr) {
            const uint8_t* old = orig->pixels->mapCpu();
            if (old != nullptr) {
                changed = std::memcmp(old, out.data(), out.size()) != 0;
                orig->pixels->unmap();
            }
        }
        if (changed) {
            EngineBuffer& buf = builder.ensureTile(tx, ty);
            std::memcpy(buf.mapCpuWrite(), out.data(), out.size());
            anyTile = true;
        }
    }
    if (!anyTile) {
        err = "shape: nothing written";
        return false;
    }
    const uint64_t nextRev = (base != nullptr ? base->revision : 0) + 1;
    layer->pixels = mergeWithBase(base, builder.publish(nextRev), cols, rows, nextRev);
    if (layer->mask != nullptr && layer->mask->enabled) {
        layer->render = composeMasked(layer->pixels, *layer->mask);
    }
    return true;
}

}  // namespace montage
