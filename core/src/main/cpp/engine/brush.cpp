#include "engine/brush.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <cstring>

#include <hilog/log.h>

namespace montage {
namespace {
constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Brush";
constexpr int kStampLimit = 160;
constexpr float kSpacingHard = 0.015f;  // 对拍 BrushStroke.spacingFraction
constexpr float kSpacingSoft = 0.025f;
}  // namespace

bool StrokeDraft::begin(const Document& doc, LayerId layerId, const BrushSettings& settings,
                        std::string& err) {
    const Layer* layer = nullptr;
    for (const Layer& l : doc.layers) {
        if (l.id == layerId) {
            layer = &l;
            break;
        }
    }
    if (layer == nullptr) {
        err = "brush: layer not found";
        return false;
    }
    if (!(settings.diameter >= 1.0f && settings.diameter <= 2100.0f) ||
        !(settings.hardness >= 0.0f && settings.hardness <= 1.0f) ||
        !(settings.opacity >= 0.01f && settings.opacity <= 1.0f)) {
        err = "brush: invalid settings";
        return false;
    }
    settings_ = settings;
    layerId_ = layerId;
    base_ = layer->pixels;
    originX_ = static_cast<int>(std::lround(layer->transform.originX));
    originY_ = static_cast<int>(std::lround(layer->transform.originY));
    if (base_ != nullptr) {
        // 网格 = 图层像素网格（1:1，origin 仅平移）
        imgW_ = base_->cols * kTileSize;
        imgH_ = base_->rows * kTileSize;
        gridCols_ = base_->cols;
        gridRows_ = base_->rows;
    } else {
        // 空图层：网格 = 画布（对齐源 extent = pixelBounds ∪ canvas）
        imgW_ = imgH_ = 0;
        gridCols_ = (doc.width + kTileSize - 1u) / kTileSize;
        gridRows_ = (doc.height + kTileSize - 1u) / kTileSize;
        // 空图层 origin 通常 (0,0)（画布域）；防御非零 origin
        originX_ = std::min(0, originX_);
        originY_ = std::min(0, originY_);
    }
    coverage_.clear();
    work_.clear();
    touched_.clear();
    samples_.clear();
    hasPrev_ = false;
    distToNext_ = 0;
    active_ = true;

    // 软刷 1D 径向 falloff 表（对拍 falloff(): 归一化高斯跨半径衰减，rim 归零）
    radial_.clear();
    if (settings_.hardness < 1.0f) {
        const int steps = std::min(kStampLimit, static_cast<int>(std::ceil(settings_.diameter))) / 2 + 1;
        const float radius = settings_.diameter / 2.0f;
        const float hardR = radius * settings_.hardness;
        radial_.reserve(static_cast<size_t>(steps));
        for (int i = 0; i < steps; ++i) {
            const float d = static_cast<float>(i) / (steps - 1) * radius;
            float v = 0.0f;
            if (d <= hardR) {
                v = 1.0f;
            } else if (d < radius) {
                v = falloff((d - hardR) / std::max(1e-6f, radius - hardR));
            }
            radial_.push_back(v);
        }
    }
    return true;
}

void StrokeDraft::ensureWorking(uint32_t key) {
    if (work_.count(key) != 0) {
        return;
    }
    std::vector<uint8_t> px(static_cast<size_t>(kTileSize) * kTileSize * 4, 0);
    if (base_ != nullptr) {
        auto it = base_->tiles.find(key);
        if (it != base_->tiles.end() && it->second != nullptr) {
            const uint8_t* src = it->second->pixels->mapCpu();
            if (src != nullptr) {
                std::memcpy(px.data(), src, static_cast<size_t>(kTileSize) * kTileSize * 4);
            }
            it->second->pixels->unmap();
        }
    }
    work_[key] = std::move(px);
}

void StrokeDraft::dab(float gx, float gy) {
    const float radius = settings_.diameter / 2.0f;
    const float maxX = layerHadPixels_ ? static_cast<float>(imgW_) : static_cast<float>(gridCols_ * kTileSize);
    const float maxY = layerHadPixels_ ? static_cast<float>(imgH_) : static_cast<float>(gridRows_ * kTileSize);
    const float x0 = std::max(0.0f, gx - radius);
    const float y0 = std::max(0.0f, gy - radius);
    const float x1 = std::min(maxX, gx + radius);
    const float y1 = std::min(maxY, gy + radius);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    const int px0 = static_cast<int>(std::floor(x0));
    const int py0 = static_cast<int>(std::floor(y0));
    const int px1 = static_cast<int>(std::ceil(x1));
    const int py1 = static_cast<int>(std::ceil(y1));
    constexpr int kTile = static_cast<int>(kTileSize);
    const int tx0 = std::max(0, px0 / kTile);
    const int ty0 = std::max(0, py0 / kTile);
    const int tx1 = std::max(px1 - 1, px0) / kTile;
    const int ty1 = std::max(py1 - 1, py0) / kTile;
    const int radialMax = static_cast<int>(radial_.size()) - 1;

    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            if (tx < 0 || ty < 0 || tx >= static_cast<int>(gridCols_) ||
                ty >= static_cast<int>(gridRows_)) {
                continue;
            }
            const uint32_t key = static_cast<uint32_t>(ty) * gridCols_ + static_cast<uint32_t>(tx);
            ensureWorking(key);
            if (coverage_.count(key) == 0) {
                coverage_[key].assign(static_cast<size_t>(kTileSize) * kTileSize, 0);
            }
            touched_.insert(key);
            auto& cov = coverage_[key];
            // coverage 累计：硬 max（保抗锯齿轮廓）/ 软 screen（软刷笔画内 paint 累积）
            for (int r = std::max(py0, ty * kTile); r < std::min(py1, (ty + 1) * kTile); ++r) {
                uint8_t* covRow = cov.data() + static_cast<size_t>(r - ty * kTile) * kTileSize;
                for (int cx = std::max(px0, tx * kTile); cx < std::min(px1, (tx + 1) * kTile); ++cx) {
                    const float dx = cx + 0.5f - gx;
                    const float dy = r + 0.5f - gy;
                    const float d = std::sqrt(dx * dx + dy * dy);
                    if (d > radius) {
                        continue;
                    }
                    float f = 0.0f;
                    if (settings_.hardness >= 1.0f) {
                        f = 1.0f;
                    } else {
                        const int ri = std::min(radialMax,
                                                static_cast<int>(d / radius * (radialMax)));
                        f = radial_[static_cast<size_t>(std::max(0, ri))];
                        if (f <= 0.0f) {
                            continue;
                        }
                    }
                    uint8_t& c = covRow[static_cast<size_t>(cx - tx * kTile)];
                    if (settings_.hardness >= 1.0f) {
                        c = std::max(c, static_cast<uint8_t>(std::lround(f * 255.0f)));
                    } else {
                        const float cv = c / 255.0f;
                        c = static_cast<uint8_t>(
                            std::lround((1.0f - (1.0f - cv) * (1.0f - f)) * 255.0f));
                    }
                }
            }
        }
    }
}

void StrokeDraft::walkTo(float gx, float gy) {
    const float spacing =
        std::max(0.25f, settings_.diameter *
                            (settings_.hardness >= 1.0f ? kSpacingHard : kSpacingSoft));
    if (hasPrev_) {
        const float dx = gx - prevX_;
        const float dy = gy - prevY_;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length > 0.0f) {
            float distance = distToNext_;
            while (distance <= length) {
                dab(prevX_ + dx * distance / length, prevY_ + dy * distance / length);
                distance += spacing;
            }
            distToNext_ = distance - length;
        }
    } else {
        dab(gx, gy);
        distToNext_ = spacing;
    }
    prevX_ = gx;
    prevY_ = gy;
    hasPrev_ = true;
}

// 向心 Catmull-Rom（对拍 curve()：过每个样本、步长 ≤2px）
void StrokeDraft::curveTo(float gx, float gy) {
    const size_t n = samples_.size();
    if (n < 2) {
        return;
    }
    const Pt start = samples_[n - 2];
    const Pt end{gx, gy};
    const Pt before = n >= 3 ? samples_[n - 3] : start;
    const Pt after = end;
    auto knot = [](float t, const Pt& a, const Pt& b) {
        return t + std::max(0.0001f, std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y)));
    };
    auto mix = [](const Pt& a, const Pt& b, float ta, float tb, float t) {
        const float wa = (tb - t) / (tb - ta);
        const float wb = (t - ta) / (tb - ta);
        return Pt{a.x * wa + b.x * wb, a.y * wa + b.y * wb};
    };
    const float t0 = 0.0f;
    const float t1 = knot(t0, before, start);
    const float t2 = knot(t1, start, end);
    const float t3 = knot(t2, end, after);
    const float dist = std::sqrt((end.x - start.x) * (end.x - start.x) +
                                 (end.y - start.y) * (end.y - start.y));
    const int pieces = std::max(1, static_cast<int>(std::ceil(dist / 2.0f)));
    for (int i = 1; i <= pieces; ++i) {
        const float t = t1 + (t2 - t1) * static_cast<float>(i) / static_cast<float>(pieces);
        const Pt a1 = mix(before, start, t0, t1, t);
        const Pt a2 = mix(start, end, t1, t2, t);
        const Pt a3 = mix(end, after, t2, t3, t);
        const Pt b1 = mix(a1, a2, t0, t2, t);
        const Pt b2 = mix(a2, a3, t1, t3, t);
        const Pt p = i == pieces ? end : mix(b1, b2, t1, t2, t);
        walkTo(p.x, p.y);
    }
}

void StrokeDraft::append(float docX, float docY) {
    if (!active_) {
        return;
    }
    const float gx = docX - static_cast<float>(originX_);
    const float gy = docY - static_cast<float>(originY_);
    if (!std::isfinite(gx) || !std::isfinite(gy)) {
        return;
    }
    if (!samples_.empty() && samples_.back().x == gx && samples_.back().y == gy) {
        return;
    }
    samples_.push_back(Pt{gx, gy});
    if (samples_.size() > 4) {
        samples_.erase(samples_.begin());
    }
    const size_t n = samples_.size();
    if (n == 1) {
        walkTo(gx, gy);
    } else if (n >= 3) {
        curveTo(gx, gy);
    }
}

void StrokeDraft::end() {
    // 简化（M3.1）：无 provisional tail，末段随样本即时落 dab
    active_ = false;
}

void StrokeDraft::cancel() {
    active_ = false;
    coverage_.clear();
    work_.clear();
    touched_.clear();
}

bool StrokeDraft::tileFor(uint32_t tx, uint32_t ty, std::vector<uint8_t>& out) const {
    if (gridCols_ == 0) {
        return false;
    }
    auto it = work_.find(ty * gridCols_ + tx);
    if (it == work_.end()) {
        return false;
    }
    out = it->second;
    return true;
}

// publish 语义（对拍 publish()）：瓦片 = base + color × coverage × opacity；
// 橡皮 = destinationOut。直通 alpha 域换算。
void StrokeDraft::paintTile(uint32_t key) {
    auto covIt = coverage_.find(key);
    auto workIt = work_.find(key);
    if (covIt == coverage_.end() || workIt == work_.end()) {
        return;
    }
    const uint8_t* cov = covIt->second.data();
    uint8_t* px = workIt->second.data();
    const float op = settings_.opacity;
    for (size_t i = 0; i < static_cast<size_t>(kTileSize) * kTileSize; ++i) {
        const float cv = cov[i] / 255.0f;
        const float covA = cv * op;  // 笔画级 opacity 上限（覆盖层语义）
        uint8_t* p = px + i * 4;
        if (settings_.erasing) {
            // destinationOut：alpha 乘减，rgb 不动（直通域）
            p[3] = static_cast<uint8_t>(std::lround(p[3] * (1.0f - covA)));
            continue;
        }
        const float as = p[3] / 255.0f;
        const float ao = covA + as * (1.0f - covA);
        if (ao <= 0.0001f) {
            p[0] = p[1] = p[2] = p[3] = 0;
            continue;
        }
        const float cr = settings_.red;
        const float cg = settings_.green;
        const float cb = settings_.blue;
        // premult 合成后转回直通：out_rgb = (src_rgb·covA + dst_rgb·as·(1-covA)) / ao
        for (int c = 0; c < 3; ++c) {
            const float d = p[static_cast<size_t>(c)] / 255.0f;
            const float outP = cr * covA + d * as * (1.0f - covA);
            p[static_cast<size_t>(c)] =
                static_cast<uint8_t>(std::lround(std::min(1.0f, outP / ao) * 255.0f));
        }
        p[3] = static_cast<uint8_t>(std::lround(std::min(1.0f, ao) * 255.0f));
    }
}

std::shared_ptr<const TileGrid> StrokeDraft::commitGrid(const TileGrid* original) {
    if (gridCols_ == 0) {
        return nullptr;
    }
    auto grid = std::make_shared<TileGrid>();
    grid->cols = gridCols_;
    grid->rows = gridRows_;
    grid->revision = gridRevision;
    if (original != nullptr) {
        for (const auto& kv : original->tiles) {
            grid->tiles[kv.first] = kv.second;
        }
    }
    for (const uint32_t key : touched_) {
        if (coverage_.count(key) == 0) {
            continue;
        }
        paintTile(key);
        auto it = work_.find(key);
        if (it == work_.end()) {
            continue;
        }
        auto tile = std::make_shared<Tile>();
        auto buf = std::make_unique<EngineBuffer>(kTileSize, kTileSize);
        std::memcpy(buf->mapCpuWrite(), it->second.data(),
                    static_cast<size_t>(kTileSize) * kTileSize * 4);
        tile->pixels = std::move(buf);
        grid->tiles[key] = std::move(tile);
    }
    return grid;
}

}  // namespace montage
