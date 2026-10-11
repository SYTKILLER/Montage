#include "engine/gradient_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "tiles/selection_ops.h"

namespace montage {
namespace {

// stops 升序分段线性插值（直 alpha 域逐通道 lerp）；t clamp 到 [first, last]
void evalStops(const std::vector<GradientStop>& stops, float t, float out[4]) {
    t = std::min(1.0f, std::max(0.0f, t));
    if (stops.empty()) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    if (t <= stops.front().pos) {
        out[0] = stops.front().r;
        out[1] = stops.front().g;
        out[2] = stops.front().b;
        out[3] = stops.front().a;
        return;
    }
    if (t >= stops.back().pos) {
        out[0] = stops.back().r;
        out[1] = stops.back().g;
        out[2] = stops.back().b;
        out[3] = stops.back().a;
        return;
    }
    for (size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].pos) {
            const GradientStop& a = stops[i - 1];
            const GradientStop& b = stops[i];
            const float span = b.pos - a.pos;
            const float u = span > 1e-9f ? (t - a.pos) / span : 1.0f;
            out[0] = a.r + (b.r - a.r) * u;
            out[1] = a.g + (b.g - a.g) * u;
            out[2] = a.b + (b.b - a.b) * u;
            out[3] = a.a + (b.a - a.a) * u;
            return;
        }
    }
}

// 由引擎 fg/bg 构建两停（样式语义见 GradientStyle）
std::vector<GradientStop> buildStops(GradientStyle style, const float fg[3], const float bg[3]) {
    GradientStop s0;
    s0.pos = 0.0f;
    s0.r = fg[0];
    s0.g = fg[1];
    s0.b = fg[2];
    s0.a = 1.0f;
    GradientStop s1;
    s1.pos = 1.0f;
    if (style == GradientStyle::FgToBg) {
        s1.r = bg[0];
        s1.g = bg[1];
        s1.b = bg[2];
    } else {
        s1.r = fg[0];
        s1.g = fg[1];
        s1.b = fg[2];
    }
    s1.a = (style == GradientStyle::FgToBg) ? 1.0f : 0.0f;
    return {s0, s1};
}

// src-over（直 alpha 域）：结果写回 px
void srcOver(uint8_t px[4], float sr, float sg, float sb, float sa) {
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

// 输出缓冲与原瓦片（或缺席 = 全零）逐字节相同则不落（原共享瓦片/稀疏保持），否则写入 builder
void publishTileIfChanged(TileGridBuilder& builder, uint32_t tx, uint32_t ty,
                          const std::shared_ptr<const Tile>& original,
                          const std::vector<uint8_t>& out) {
    if (original != nullptr) {
        const uint8_t* old = original->pixels->mapCpu();
        if (old != nullptr) {
            const bool same = std::memcmp(old, out.data(), out.size()) == 0;
            original->pixels->unmap();
            if (same) {
                return;
            }
        }
    } else {
        const bool allZero = std::all_of(out.begin(), out.end(),
                                         [](uint8_t v) { return v == 0; });
        if (allZero) {
            return;  // 透明端不落空瓦片（保持稀疏）
        }
    }
    EngineBuffer& buf = builder.ensureTile(tx, ty);
    std::memcpy(buf.mapCpuWrite(), out.data(), out.size());
}

// publish 只含写入瓦片——必须并回原网格（未触碰瓦片保留原共享 Tile*，COW 零成本）
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

// 渐变拖线 → 像素 (docX,docY) 的参数 t
double gradientT(const GradientSpec& spec, double docX, double docY) {
    const double dx = spec.x1 - spec.x0;
    const double dy = spec.y1 - spec.y0;
    const double len2 = dx * dx + dy * dy;
    if (len2 < 1e-9) {
        return 0.0;
    }
    if (spec.type == GradientType::Linear) {
        return ((docX - spec.x0) * dx + (docY - spec.y0) * dy) / len2;
    }
    return std::hypot(docX - spec.x0, docY - spec.y0) / std::sqrt(len2);
}

}  // namespace

bool applyGradient(Document& doc, LayerId layerId, const GradientSpec& spec,
                   GradientTarget target, const float fg[3], const float bg[3],
                   std::string& err) {
    Layer* layer = nullptr;
    for (Layer& l : doc.layers) {
        if (l.id == layerId) {
            layer = &l;
            break;
        }
    }
    if (layer == nullptr) {
        err = "gradient: layer not found";
        return false;
    }
    if (doc.width == 0 || doc.height == 0) {
        err = "gradient: no document";
        return false;
    }
    const double dx = spec.x1 - spec.x0;
    const double dy = spec.y1 - spec.y0;
    if (dx * dx + dy * dy < 1e-9) {
        err = "gradient: degenerate drag line";
        return false;
    }
    const std::vector<GradientStop> stops = buildStops(spec.style, fg, bg);
    const TileGrid* selGrid = doc.selection != nullptr ? doc.selection.get() : nullptr;

    if (target == GradientTarget::LayerMask) {
        // 蒙版目标：patch 网格全域写入（灰度域 src-over：outG = lum·sA + base·(1−sA)）
        if (layer->mask == nullptr || layer->mask->pixels == nullptr) {
            err = "gradient: layer has no editable mask";
            return false;
        }
        const LayerMask& mask = *layer->mask;
        const std::shared_ptr<const TileGrid> base = mask.pixels;
        const int originX = static_cast<int>(std::lround(layer->transform.originX));
        const int originY = static_cast<int>(std::lround(layer->transform.originY));
        TileGridBuilder builder(base->cols, base->rows);
        std::vector<uint8_t> out(static_cast<size_t>(kTileSize) * kTileSize * 4u);
        for (uint32_t ty = 0; ty < base->rows; ++ty) {
            for (uint32_t tx = 0; tx < base->cols; ++tx) {
                std::fill(out.begin(), out.end(), static_cast<uint8_t>(0));
                std::shared_ptr<const Tile> orig;
                auto it = base->tiles.find(ty * base->cols + tx);
                if (it != base->tiles.end() && it->second != nullptr) {
                    orig = it->second;
                    const uint8_t* src = orig->pixels->mapCpu();
                    if (src != nullptr) {
                        std::memcpy(out.data(), src, out.size());
                        orig->pixels->unmap();
                    }
                }
                for (uint32_t r = 0; r < kTileSize; ++r) {
                    // doc 坐标 = 图层 origin + patch offset + 网格内位置
                    const int docY = originY + mask.offsetY +
                                     static_cast<int>(ty * kTileSize + r);
                    for (uint32_t c = 0; c < kTileSize; ++c) {
                        const int docX = originX + mask.offsetX +
                                         static_cast<int>(tx * kTileSize + c);
                        float col[4];
                        evalStops(stops, static_cast<float>(
                                             gradientT(spec, docX, docY)), col);
                        float sa = col[3];
                        if (selGrid != nullptr) {
                            sa *= static_cast<float>(
                                      selectionGrayAt(*selGrid, docX, docY)) / 255.0f;
                        }
                        const size_t off = (static_cast<size_t>(r) * kTileSize + c) * 4u;
                        uint8_t* px = out.data() + off;
                        const float baseG = px[0] / 255.0f;
                        const float lum = 0.3f * col[0] + 0.59f * col[1] + 0.11f * col[2];
                        const float outG = lum * sa + baseG * (1.0f - sa);
                        const uint8_t g8 =
                            static_cast<uint8_t>(std::lround(std::min(1.0f, std::max(0.0f, outG)) * 255.0f));
                        px[0] = px[1] = px[2] = g8;
                        px[3] = 255;
                    }
                }
                publishTileIfChanged(builder, tx, ty, orig, out);
            }
        }
        auto merged = std::make_shared<LayerMask>(mask);
        merged->pixels = mergeWithBase(base, builder.publish(base->revision + 1),
                                       base->cols, base->rows, base->revision + 1);
        layer->mask = std::move(merged);
        layer->render = (layer->mask->enabled && layer->pixels != nullptr)
                            ? composeMasked(layer->pixels, *layer->mask)
                            : nullptr;
        return true;
    }

    // 像素目标：网格 = 图层域（空层 = 文档域新建，origin=(0,0)）
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
    TileGridBuilder builder(cols, rows);
    std::vector<uint8_t> out(static_cast<size_t>(kTileSize) * kTileSize * 4u);
    for (uint32_t ty = 0; ty < rows; ++ty) {
        for (uint32_t tx = 0; tx < cols; ++tx) {
            std::fill(out.begin(), out.end(), static_cast<uint8_t>(0));
            std::shared_ptr<const Tile> orig;
            if (base != nullptr) {
                auto it = base->tiles.find(ty * cols + tx);
                if (it != base->tiles.end() && it->second != nullptr) {
                    orig = it->second;
                    const uint8_t* src = orig->pixels->mapCpu();
                    if (src != nullptr) {
                        std::memcpy(out.data(), src, out.size());
                        orig->pixels->unmap();
                    }
                }
            }
            for (uint32_t r = 0; r < kTileSize; ++r) {
                const int docY = originY + static_cast<int>(ty * kTileSize + r);
                for (uint32_t c = 0; c < kTileSize; ++c) {
                    const int docX = originX + static_cast<int>(tx * kTileSize + c);
                    float col[4];
                    evalStops(stops, static_cast<float>(
                                         gradientT(spec, docX, docY)), col);
                    float sa = col[3];
                    if (selGrid != nullptr) {
                        sa *= static_cast<float>(
                                  selectionGrayAt(*selGrid, docX, docY)) / 255.0f;
                    }
                    srcOver(out.data() + (static_cast<size_t>(r) * kTileSize + c) * 4u,
                            col[0], col[1], col[2], sa);
                }
            }
            publishTileIfChanged(builder, tx, ty, orig, out);
        }
    }
    const uint64_t nextRev = (base != nullptr ? base->revision : 0) + 1;
    layer->pixels = mergeWithBase(base, builder.publish(nextRev), cols, rows, nextRev);
    if (layer->mask != nullptr && layer->mask->enabled) {
        layer->render = composeMasked(layer->pixels, *layer->mask);
    }
    return true;
}

bool fillWithColor(Document& doc, LayerId layerId, FillSource source, float fr, float fg,
                   float fb, float br, float bg_, float bb, std::string& err) {
    Layer* layer = nullptr;
    for (Layer& l : doc.layers) {
        if (l.id == layerId) {
            layer = &l;
            break;
        }
    }
    if (layer == nullptr) {
        err = "fill: layer not found";
        return false;
    }
    if (doc.width == 0 || doc.height == 0) {
        err = "fill: no document";
        return false;
    }
    float col[3];
    if (source == FillSource::Foreground) {
        col[0] = fr;
        col[1] = fg;
        col[2] = fb;
    } else if (source == FillSource::Background) {
        col[0] = br;
        col[1] = bg_;
        col[2] = bb;
    } else if (source == FillSource::Black) {
        col[0] = col[1] = col[2] = 0.0f;
    } else {
        col[0] = col[1] = col[2] = 1.0f;
    }
    if (layer->pixels == nullptr) {
        // 空层填充：建文档域网格（选区约束仍生效；全透明端不落瓦片由 publish 兜底）
        const uint32_t cols = (doc.width + kTileSize - 1u) / kTileSize;
        const uint32_t rows = (doc.height + kTileSize - 1u) / kTileSize;
        TileGridBuilder builder(cols, rows);
        layer->pixels = builder.publish(1);
    }
    const std::shared_ptr<const TileGrid> base = layer->pixels;  // 持共享引用防自替换
    const TileGrid& grid = *base;
    const TileGrid* selGrid = doc.selection != nullptr ? doc.selection.get() : nullptr;
    const int originX = static_cast<int>(std::lround(layer->transform.originX));
    const int originY = static_cast<int>(std::lround(layer->transform.originY));
    TileGridBuilder builder(grid.cols, grid.rows);
    std::vector<uint8_t> out(static_cast<size_t>(kTileSize) * kTileSize * 4u);
    for (uint32_t ty = 0; ty < grid.rows; ++ty) {
        for (uint32_t tx = 0; tx < grid.cols; ++tx) {
            std::fill(out.begin(), out.end(), static_cast<uint8_t>(0));
            std::shared_ptr<const Tile> orig;
            auto it = grid.tiles.find(ty * grid.cols + tx);
            if (it != grid.tiles.end() && it->second != nullptr) {
                orig = it->second;
                const uint8_t* src = orig->pixels->mapCpu();
                if (src != nullptr) {
                    std::memcpy(out.data(), src, out.size());
                    orig->pixels->unmap();
                }
            }
            for (uint32_t r = 0; r < kTileSize; ++r) {
                const int docY = originY + static_cast<int>(ty * kTileSize + r);
                for (uint32_t c = 0; c < kTileSize; ++c) {
                    const int docX = originX + static_cast<int>(tx * kTileSize + c);
                    float sa = 1.0f;
                    if (selGrid != nullptr) {
                        sa = static_cast<float>(selectionGrayAt(*selGrid, docX, docY)) / 255.0f;
                    }
                    if (sa <= 0.0f) {
                        continue;
                    }
                    srcOver(out.data() + (static_cast<size_t>(r) * kTileSize + c) * 4u,
                            col[0], col[1], col[2], sa);
                }
            }
            publishTileIfChanged(builder, tx, ty, orig, out);
        }
    }
    layer->pixels = mergeWithBase(base, builder.publish(grid.revision + 1), grid.cols,
                                  grid.rows, grid.revision + 1);
    if (layer->mask != nullptr && layer->mask->enabled) {
        layer->render = composeMasked(layer->pixels, *layer->mask);
    }
    return true;
}

bool strokeSelectionBorder(Document& doc, LayerId layerId, int width, float fr, float fgn,
                           float fb, std::string& err) {
    if (doc.selection == nullptr) {
        err = "stroke: no active selection";
        return false;
    }
    if (width < 1 || width > 100) {
        err = "stroke: width out of range";
        return false;
    }
    // 环带（border 核，r = ceil(w/2) 中心描边语义）临时作选区 → 走填充核；
    // 调用方须持 docMutex（与选区约束同锁域）
    auto band = tiles::borderSelection(doc, (width + 1) / 2);
    if (band == nullptr) {
        err = "stroke: empty band";
        return false;
    }
    std::shared_ptr<const TileGrid> saved = doc.selection;
    doc.selection = std::move(band);
    const bool ok = fillWithColor(doc, layerId, FillSource::Foreground, fr, fgn, fb, 0, 0, 0, err);
    doc.selection = saved;
    return ok;
}

}  // namespace montage
