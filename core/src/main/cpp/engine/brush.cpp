#include "engine/brush.h"
#include "engine/pixel_access.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <cmath>
#include <cstring>

#include <hilog/log.h>

#include "engine/inpaint_ops.h"
#include "engine/poisson_ops.h"

namespace montage {
namespace {
constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Brush";
constexpr int kStampLimit = 160;
constexpr float kSpacingHard = 0.015f;  // 对拍 BrushStroke.spacingFraction
constexpr float kSpacingSoft = 0.025f;
}  // namespace

bool StrokeDraft::begin(const Document& doc, LayerId layerId, const BrushSettings& settings,
                        bool onMask, bool onQuickMask, const CloneParams* cloneSrc,
                        std::string& err, BrushEffect effect, HealKind heal) {
    const Layer* layer = nullptr;
    for (const Layer& l : doc.layers) {
        if (l.id == layerId) {
            layer = &l;
            break;
        }
    }
    if (layer == nullptr && !onQuickMask) {
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
    maskTarget_ = false;
    qmTarget_ = false;
    cloneMode_ = false;
    // P6 D1-b：笔画工作深度 = 目标网格深度（像素目标=层深度/空层=文档深度；QM/蒙版恒 U8）
    depth_ = (onMask || onQuickMask) ? PixelDepth::U8
             : (layer != nullptr && layer->pixels != nullptr ? layer->depth : doc.depth);
    workDeep_.clear();
    deepU8Fb_.clear();
    patternMode_ = false;
    srcGrid_ = nullptr;
    srcCacheBase_ = nullptr;
    srcCacheKey_ = 0;
    baseCacheBase_ = nullptr;
    baseCacheKey_ = 0;
    smudgeHas_ = false;
    smudgeR_ = smudgeG_ = smudgeB_ = smudgeA_ = 0.0f;
    // M19a：修复族状态（QM/蒙版灰度目标不可修复——engine 侧已过滤；Heal 需克隆源）
    healKind_ = heal;
    healRgb_.clear();
    healRx_ = healRy_ = healRw_ = healRh_ = 0;
    canvasW_ = static_cast<int>(doc.width);
    canvasH_ = static_cast<int>(doc.height);
    // M11e：效果族仅像素目标生效（QM/蒙版灰度域退化普通笔，登记）
    effect_ = (cloneSrc == nullptr && !onMask && !onQuickMask) ? effect : BrushEffect::None;
    if (healKind_ != HealKind::None) {
        effect_ = BrushEffect::None;  // 修复与效果族互斥
    }
    if (cloneSrc != nullptr && cloneSrc->srcGrid != nullptr) {
        // M11d 仿制图章：目标 = 当前层像素网格；源 = 克隆源网格快照（不可变）
        if (layer == nullptr) {
            err = "clone: layer not found";
            return false;
        }
        maskLum_ = 0.0f;
        base_ = layer->pixels;
        originX_ = static_cast<int>(std::lround(layer->transform.originX));
        originY_ = static_cast<int>(std::lround(layer->transform.originY));
        if (base_ != nullptr) {
            imgW_ = base_->cols * kTileSize;
            imgH_ = base_->rows * kTileSize;
            gridCols_ = base_->cols;
            gridRows_ = base_->rows;
        } else {
            imgW_ = imgH_ = 0;
            gridCols_ = (doc.width + kTileSize - 1u) / kTileSize;
            gridRows_ = (doc.height + kTileSize - 1u) / kTileSize;
            originX_ = std::min(0, originX_);
            originY_ = std::min(0, originY_);
        }
        layerHadPixels_ = true;
        srcGrid_ = cloneSrc->srcGrid;
        srcOriginX_ = cloneSrc->srcOriginX;
        srcOriginY_ = cloneSrc->srcOriginY;
        srcOffsetX_ = cloneSrc->offsetX;
        srcOffsetY_ = cloneSrc->offsetY;
        restoreMode_ = cloneSrc->restoreMode;
        patternMode_ = cloneSrc->patternMode;
        patW_ = cloneSrc->patW;
        patH_ = cloneSrc->patH;
        patScale_ = std::clamp(cloneSrc->patScale, 0.01, 10.0);
        cloneMode_ = true;
    } else if (onQuickMask) {
        // M10d 快速蒙版：绑定文档域通道网格（灰度落笔；网格 = 通道网格，origin=(0,0)）
        if (doc.quickMask == nullptr) {
            err = "brush: quick mask not active";
            return false;
        }
        maskLum_ = std::min(1.0f, std::max(0.0f, (settings.red + settings.green + settings.blue) / 3.0f));
        base_ = doc.quickMask;
        originX_ = 0;
        originY_ = 0;
        imgW_ = base_->cols * kTileSize;
        imgH_ = base_->rows * kTileSize;
        gridCols_ = base_->cols;
        gridRows_ = base_->rows;
        layerHadPixels_ = true;
        qmTarget_ = true;
    } else if (onMask) {
        // M5b 蒙版模式：绑定 patch 网格（灰度落笔）；网格原点 = 图层 origin + patch offset
        if (layer->mask == nullptr || layer->mask->pixels == nullptr) {
            err = "brush: layer has no editable mask";
            return false;
        }
        maskTarget_ = true;
        maskLum_ = std::min(1.0f, std::max(0.0f, (settings.red + settings.green + settings.blue) / 3.0f));
        base_ = layer->mask->pixels;
        originX_ = static_cast<int>(std::lround(layer->transform.originX)) + layer->mask->offsetX;
        originY_ = static_cast<int>(std::lround(layer->transform.originY)) + layer->mask->offsetY;
        imgW_ = layer->mask->width;
        imgH_ = layer->mask->height;
        gridCols_ = base_->cols;
        gridRows_ = base_->rows;
        layerHadPixels_ = true;
    } else {
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
    }
    selMask_ = doc.selection != nullptr ? doc.selection.get() : nullptr;
    coverage_.clear();
    work_.clear();
    workRev_.clear();
    touched_.clear();
    tailBackup_.clear();
    samples_.clear();
    hasPrev_ = false;
    distToNext_ = 0;
    pressure_ = 1.0f;
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
    // P6：深色层平行深域瓦片（base 深字节直拷；空瓦片 = 透明零）
    if (depth_ != PixelDepth::U8) {
        const size_t bpp = static_cast<size_t>(kTileSize) * kTileSize *
                           static_cast<size_t>(bytesPerPel(depth_));
        std::vector<uint8_t> dp(bpp, 0);
        if (base_ != nullptr) {
            auto it = base_->tiles.find(key);
            if (it != base_->tiles.end() && it->second != nullptr) {
                const uint8_t* src = it->second->pixels->mapCpu();
                if (src != nullptr) {
                    std::memcpy(dp.data(), src, bpp);
                }
                it->second->pixels->unmap();
            }
        }
        workDeep_[key] = std::move(dp);
    }
}

// 当前段有效 dab 半径：压感线性调制直径（clamp 到微小下限，pressure=0 仍留针尖）
float StrokeDraft::effRadius() const {
    constexpr float kMinPressure = 0.02f;
    const float p = std::min(1.0f, std::max(kMinPressure, pressure_));
    return settings_.diameter / 2.0f * p;
}

void StrokeDraft::dab(float gx, float gy) {
    const float radius = effRadius();
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
                    if (selMask_ != nullptr) {
                        // M7a：笔画约束在选区内（文档域采样；cx/r 为图层局部像素坐标）
                        const uint8_t sg = selectionGrayAt(*selMask_,
                                                           static_cast<int>(cx) + originX_,
                                                           static_cast<int>(r) + originY_);
                        if (sg == 0) {
                            continue;
                        }
                        f *= static_cast<float>(sg) / 255.0f;
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
    if (effect_ == BrushEffect::Smudge) {
        // M11e 涂抹：dab 中心取色混入 held（首 dab 抓取、后续 40% 混入——沿笔拖颜料）
        const uint8_t* s = baseAt(static_cast<int>(gx), static_cast<int>(gy));
        const float sr = s != nullptr ? s[0] / 255.0f : 0.0f;
        const float sg = s != nullptr ? s[1] / 255.0f : 0.0f;
        const float sb = s != nullptr ? s[2] / 255.0f : 0.0f;
        const float sa = s != nullptr ? s[3] / 255.0f : 0.0f;
        if (!smudgeHas_) {
            // v1 单色涂抹：held = 起笔区采样色（沿途混入会随 dab 覆盖互相抹除，
            // 位移混合模型登记 M19/后续批）
            smudgeR_ = sr;
            smudgeG_ = sg;
            smudgeB_ = sb;
            smudgeA_ = sa;
            smudgeHas_ = true;
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

// 向心 Catmull-Rom 曲线段（对拍 curve()：过每个样本、步长 ≤2px；显式四控制点）
void StrokeDraft::curvePiece(const Pt& from, const Pt& to, const Pt& before, const Pt& after) {
    pressure_ = to.pressure;
    auto knot = [](float t, const Pt& a, const Pt& b) {
        return t + std::max(0.0001f, std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y)));
    };
    auto mix = [](const Pt& a, const Pt& b, float ta, float tb, float t) {
        const float wa = (tb - t) / (tb - ta);
        const float wb = (t - ta) / (tb - ta);
        return Pt{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.pressure * wa + b.pressure * wb};
    };
    const float t0 = 0.0f;
    const float t1 = knot(t0, before, from);
    const float t2 = knot(t1, from, to);
    const float t3 = knot(t2, to, after);
    const float dist = std::sqrt((to.x - from.x) * (to.x - from.x) +
                                 (to.y - from.y) * (to.y - from.y));
    const int pieces = std::max(1, static_cast<int>(std::ceil(dist / 2.0f)));
    for (int i = 1; i <= pieces; ++i) {
        const float t = t1 + (t2 - t1) * static_cast<float>(i) / static_cast<float>(pieces);
        const Pt a1 = mix(before, from, t0, t1, t);
        const Pt a2 = mix(from, to, t1, t2, t);
        const Pt a3 = mix(to, after, t2, t3, t);
        const Pt b1 = mix(a1, a2, t0, t2, t);
        const Pt b2 = mix(a2, a3, t1, t3, t);
        const Pt p = i == pieces ? to : mix(b1, b2, t1, t2, t);
        pressure_ = p.pressure;
        walkTo(p.x, p.y);
    }
}

// provisional tail（对拍 drawTail）：备份可能触及瓦片的整份 coverage + dab 步进状态，
// 直线走到光标（笔画不滞后），随后还原步进状态——下一样本/收尾时 removeTail 还原 coverage
// 再落最终曲线段
void StrokeDraft::drawTail(const Pt& from, const Pt& to) {
    const float reach = settings_.diameter / 2.0f + 2.0f;
    const float maxX = layerHadPixels_ ? static_cast<float>(imgW_) : static_cast<float>(gridCols_ * kTileSize);
    const float maxY = layerHadPixels_ ? static_cast<float>(imgH_) : static_cast<float>(gridRows_ * kTileSize);
    const float x0 = std::max(0.0f, std::min(from.x, to.x) - reach);
    const float y0 = std::max(0.0f, std::min(from.y, to.y) - reach);
    const float x1 = std::min(maxX, std::max(from.x, to.x) + reach);
    const float y1 = std::min(maxY, std::max(from.y, to.y) + reach);
    if (x1 > x0 && y1 > y0) {
        constexpr int kTile = static_cast<int>(kTileSize);
        const int tx0 = std::max(0, static_cast<int>(x0) / kTile);
        const int ty0 = std::max(0, static_cast<int>(y0) / kTile);
        const int tx1 = std::max(static_cast<int>(x1) - 1, static_cast<int>(x0)) / kTile;
        const int ty1 = std::max(static_cast<int>(y1) - 1, static_cast<int>(y0)) / kTile;
        for (int ty = ty0; ty <= ty1; ++ty) {
            for (int tx = tx0; tx <= tx1; ++tx) {
                if (tx >= static_cast<int>(gridCols_) || ty >= static_cast<int>(gridRows_)) {
                    continue;
                }
                const uint32_t key = static_cast<uint32_t>(ty) * gridCols_ + static_cast<uint32_t>(tx);
                auto covIt = coverage_.find(key);
                if (covIt != coverage_.end()) {
                    tailBackup_[key] = covIt->second;
                } else if (tailBackup_.find(key) == tailBackup_.end()) {
                    tailBackup_[key] = std::vector<uint8_t>();  // 空标记：尾段前无 coverage
                }
            }
        }
    }
    // 直线走到光标；步进状态还原（落盘状态不指向光标，最终曲线段从 from 续 spacing）
    const bool savedHasPrev = hasPrev_;
    const float savedX = prevX_;
    const float savedY = prevY_;
    const float savedDist = distToNext_;
    pressure_ = to.pressure;
    walkTo(to.x, to.y);
    hasPrev_ = savedHasPrev;
    prevX_ = savedX;
    prevY_ = savedY;
    distToNext_ = savedDist;
}

// 还原尾段（对拍 removeTail）：coverage 按备份还原（空标记 = 清零并撤出工作集），
// 工作瓦片重合成；返回无——渲染叠加按 touched_ 逐帧取数
void StrokeDraft::removeTail() {
    for (auto& [key, backup] : tailBackup_) {
        auto covIt = coverage_.find(key);
        if (covIt == coverage_.end()) {
            continue;
        }
        if (backup.empty()) {
            // 尾段前无 coverage：现值全为尾段所画 → 整瓦撤出（提交/渲染均忽略）
            coverage_.erase(covIt);
            work_.erase(key);
            workRev_.erase(key);
            touched_.erase(key);
        } else {
            covIt->second = backup;
            paintTile(key);
        }
    }
    tailBackup_.clear();
}

void StrokeDraft::append(float docX, float docY, float pressure) {
    if (!active_) {
        return;
    }
    const float gx = docX - static_cast<float>(originX_);
    const float gy = docY - static_cast<float>(originY_);
    const float pr = std::min(1.0f, std::max(0.0f, pressure));
    if (!std::isfinite(gx) || !std::isfinite(gy) || !std::isfinite(pr)) {
        return;
    }
    if (!samples_.empty() && samples_.back().x == gx && samples_.back().y == gy) {
        return;
    }
    removeTail();
    samples_.push_back(Pt{gx, gy, pr});
    if (samples_.size() > 4) {
        samples_.erase(samples_.begin());
    }
    const size_t n = samples_.size();
    if (n == 1) {
        pressure_ = pr;
        walkTo(gx, gy);
    } else if (effect_ == BrushEffect::Smudge) {
        // M11e 涂抹：禁用 provisional tail（tail 重画会用稀释后的 held 全程覆盖直写
        // 痕迹——颜色历史按 dab 演化无法精确重放）。直线段直连，曲线平滑度降为折线、
        // 尾段滞后一拍，登记 v1 取舍。
        walkTo(gx, gy);
    } else {
        if (n >= 3) {
            // 上一段现已拿到真实后置样本 → 落最终曲线段
            curvePiece(samples_[n - 3], samples_[n - 2],
                       samples_[n >= 4 ? n - 4 : n - 3], samples_[n - 1]);
        }
        if (n >= 2) {
            drawTail(samples_[n - 2], samples_[n - 1]);
        }
    }
}

void StrokeDraft::end() {
    // 收尾（对拍 flush()）：还原尾段，末段以真实端点为后置样本落最终曲线段
    if (!active_) {
        return;
    }
    removeTail();
    const size_t n = samples_.size();
    // M11e 涂抹：末段已在 append 的 walkTo 直连中落墨（禁 tail 同理——重放会用
    // 终点稀释后的 held 覆盖整段直写），跳过 curvePiece
    if (n >= 2 && effect_ != BrushEffect::Smudge) {
        curvePiece(samples_[n - 2], samples_[n - 1],
                   samples_[n >= 3 ? n - 3 : n - 2], samples_[n - 1]);
    }
    samples_.clear();
    hasPrev_ = false;
    active_ = false;
}

void StrokeDraft::cancel() {
    active_ = false;
    coverage_.clear();
    work_.clear();
    workRev_.clear();
    touched_.clear();
    tailBackup_.clear();
    srcGrid_ = nullptr;      // 源网格为借用指针，笔画终止即弃用（缓存随清）
    srcCacheBase_ = nullptr;
    srcCacheKey_ = 0;
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

uint32_t StrokeDraft::tileRev(uint32_t tx, uint32_t ty) const {
    if (gridCols_ == 0) {
        return 0;
    }
    auto it = workRev_.find(ty * gridCols_ + tx);
    return it == workRev_.end() ? 0u : it->second;
}

// M11d：源网格局部采样（越界 = 全透明）；单瓦片缓存（克隆采样逐像素随机访问）
const uint8_t* StrokeDraft::srcAt(int gx, int gy) const {
    if (srcGrid_ == nullptr || gx < 0 || gy < 0 ||
        gx >= static_cast<int>(srcGrid_->cols * kTileSize) ||
        gy >= static_cast<int>(srcGrid_->rows * kTileSize)) {
        return nullptr;
    }
    const uint32_t key = static_cast<uint32_t>(gy / static_cast<int>(kTileSize)) * srcGrid_->cols +
                         static_cast<uint32_t>(gx / static_cast<int>(kTileSize));
    if (key != srcCacheKey_ || srcCacheBase_ == nullptr) {
        auto it = srcGrid_->tiles.find(key);
        if (it == srcGrid_->tiles.end() || it->second == nullptr) {
            srcCacheBase_ = nullptr;
            srcCacheKey_ = key;
            return nullptr;
        }
        srcCacheBase_ = it->second->pixels->mapCpu();
        srcCacheStride_ = it->second->pixels->rowBytes();
        srcCacheKey_ = key;
    }
    if (srcCacheBase_ == nullptr) {
        return nullptr;
    }
    return srcCacheBase_ + static_cast<size_t>(gy % static_cast<int>(kTileSize)) * srcCacheStride_ +
           static_cast<size_t>(gx % static_cast<int>(kTileSize)) * 4u;
}

// M11e：base_ 网格局部采样（涂抹 dab 取色；结构同 srcAt、独立缓存）
const uint8_t* StrokeDraft::baseAt(int gx, int gy) const {
    if (base_ == nullptr || gx < 0 || gy < 0 ||
        gx >= static_cast<int>(base_->cols * kTileSize) ||
        gy >= static_cast<int>(base_->rows * kTileSize)) {
        return nullptr;
    }
    const uint32_t key = static_cast<uint32_t>(gy / static_cast<int>(kTileSize)) * base_->cols +
                         static_cast<uint32_t>(gx / static_cast<int>(kTileSize));
    if (key != baseCacheKey_ || baseCacheBase_ == nullptr) {
        auto it = base_->tiles.find(key);
        if (it == base_->tiles.end() || it->second == nullptr) {
            baseCacheBase_ = nullptr;
            baseCacheKey_ = key;
            return nullptr;
        }
        baseCacheBase_ = it->second->pixels->mapCpu();
        baseCacheStride_ = it->second->pixels->rowBytes();
        baseCacheKey_ = key;
    }
    if (baseCacheBase_ == nullptr) {
        return nullptr;
    }
    return baseCacheBase_ + static_cast<size_t>(gy % static_cast<int>(kTileSize)) * baseCacheStride_ +
           static_cast<size_t>(gx % static_cast<int>(kTileSize)) * 4u;
}

// publish 语义（对拍 publish()）：瓦片 = base + color × coverage × opacity；
// 橡皮 = destinationOut。直通 alpha 域换算。
void StrokeDraft::paintTile(uint32_t key) {
    auto covIt = coverage_.find(key);
    auto workIt = work_.find(key);
    if (covIt == coverage_.end() || workIt == work_.end()) {
        return;
    }
    if (healKind_ == HealKind::SpotContentAware || healKind_ == HealKind::SpotTexture ||
        healKind_ == HealKind::SpotProximity) {
        // M19a 污点修复：live 无预览（photocraft 同——非 LiveRetouch 族），coverage 只记账，
        // 提交时在采样窗口跑填充核 + 泊松融合（commitHeal）。work 保持 base 拷贝。
        ++workRev_[key];
        return;
    }
    ++workRev_[key];  // 发布版本递增（渲染快照增量判变依据）
    const uint8_t* cov = covIt->second.data();
    uint8_t* px = workIt->second.data();
    const float op = settings_.opacity;
    if (effect_ != BrushEffect::None) {
        if (depth_ != PixelDepth::U8) {
            deepU8Fb_[key] = true;  // P6：效果核 v1 无深域版本（提交 U8 升位，登记精度）
        }
        // M11e dab 内核族：像素域逐像素 × coverage（orig 副本供邻域/混合基准）
        constexpr size_t kN = static_cast<size_t>(kTileSize) * kTileSize;
        std::vector<uint8_t> orig(px, px + kN * 4u);
        constexpr float kStrength = 0.5f;  // 效果强度 v1 固定（曝光/流量滑条登记）
        for (size_t i = 0; i < kN; ++i) {
            const float covA = (cov[i] / 255.0f) * op;
            if (covA <= 0.0f) {
                continue;
            }
            uint8_t* p = px + i * 4u;
            if (effect_ == BrushEffect::Blur || effect_ == BrushEffect::Sharpen) {
                // 3×3 盒卷积（work_ 瓦片内 clamp 边缘）；sharpen = 反锐化掩模
                const int cx = static_cast<int>(i % kTileSize);
                const int cy = static_cast<int>(i / kTileSize);
                float acc[4] = {0, 0, 0, 0};
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = std::min(static_cast<int>(kTileSize) - 1,
                                                std::max(0, cx + dx));
                        const int ny = std::min(static_cast<int>(kTileSize) - 1,
                                                std::max(0, cy + dy));
                        const uint8_t* q = orig.data() +
                            (static_cast<size_t>(ny) * kTileSize + static_cast<size_t>(nx)) * 4u;
                        for (int ch = 0; ch < 4; ++ch) {
                            acc[ch] += q[static_cast<size_t>(ch)];
                        }
                    }
                }
                for (int ch = 0; ch < 4; ++ch) {
                    float v = acc[ch] / 9.0f;
                    if (effect_ == BrushEffect::Sharpen) {
                        const float o = orig[i * 4u + static_cast<size_t>(ch)];
                        v = o + (o - v) * 1.0f;  // amount=1（锐化差放大的钝核 v1）
                    }
                    v = std::min(255.0f, std::max(0.0f, v));
                    const float o = orig[i * 4u + static_cast<size_t>(ch)];
                    p[static_cast<size_t>(ch)] =
                        static_cast<uint8_t>(std::lround(v * covA + o * (1.0f - covA)));
                }
            } else if (effect_ == BrushEffect::Smudge) {
                // held 颜色 src-over（held alpha 参与；尾段残影登记）
                const float sa = smudgeA_ * covA;
                if (sa > 0.0f) {
                    const float da = orig[i * 4u + 3] / 255.0f;
                    const float oa = sa + da * (1.0f - sa);
                    const float srcP[3] = {smudgeR_ * sa, smudgeG_ * sa, smudgeB_ * sa};
                    for (int ch = 0; ch < 3; ++ch) {
                        const float d = orig[i * 4u + static_cast<size_t>(ch)] / 255.0f * da *
                                        (1.0f - sa);
                        p[static_cast<size_t>(ch)] = static_cast<uint8_t>(std::lround(
                            std::min(1.0f, oa > 0.0001f ? (srcP[ch] + d) / oa : 0.0f) * 255.0f));
                    }
                    p[3] = static_cast<uint8_t>(std::lround(std::min(1.0f, oa) * 255.0f));
                }
            } else if (effect_ == BrushEffect::Dodge) {
                for (int ch = 0; ch < 3; ++ch) {
                    const float c = orig[i * 4u + static_cast<size_t>(ch)];
                    const float v = c + (255.0f - c) * covA * kStrength;
                    p[static_cast<size_t>(ch)] =
                        static_cast<uint8_t>(std::lround(std::min(255.0f, v)));
                }
            } else if (effect_ == BrushEffect::Burn) {
                for (int ch = 0; ch < 3; ++ch) {
                    const float c = orig[i * 4u + static_cast<size_t>(ch)];
                    p[static_cast<size_t>(ch)] =
                        static_cast<uint8_t>(std::lround(c * (1.0f - covA * kStrength)));
                }
            } else {  // Sponge（去饱和）
                const float r = orig[i * 4u];
                const float g = orig[i * 4u + 1];
                const float b = orig[i * 4u + 2];
                const float lum = 0.3f * r + 0.59f * g + 0.11f * b;
                const float k = 1.0f - covA * kStrength;
                p[0] = static_cast<uint8_t>(std::lround(lum + (r - lum) * k));
                p[1] = static_cast<uint8_t>(std::lround(lum + (g - lum) * k));
                p[2] = static_cast<uint8_t>(std::lround(lum + (b - lum) * k));
            }
        }
        return;
    }
    if (cloneMode_) {
        if (depth_ != PixelDepth::U8) {
            deepU8Fb_[key] = true;  // P6：克隆/恢复域 v1 U8（提交升位，登记精度）
        }
        // M11d 仿制图章：out = src × covA + dst × (1−covA)（src 含自身 alpha；
        // 源恒读笔画开始快照——本笔已画像素在 work_，绝不重采）。
        // M11f 历史画笔 restoreMode：RGBA 直混（含 alpha 还原——快照透明也要恢复），
        // 且源越界/透明不再跳过（透明即恢复目标）
        const float tx = static_cast<float>(key % gridCols_ * kTileSize);
        const float ty = static_cast<float>(key / gridCols_ * kTileSize);
        for (uint32_t r = 0; r < kTileSize; ++r) {
            for (uint32_t c = 0; c < kTileSize; ++c) {
                const size_t i = (static_cast<size_t>(r) * kTileSize + c) * 4u;
                const float covA = (cov[i / 4u] / 255.0f) * op;
                if (covA <= 0.0f) {
                    continue;
                }
                // 源网格局部坐标 = 目标网格局部 − 偏移（两网格 origin 同为层 origin，同层恒成立；
                // 跨层 v1 约定双方 origin≈0，登记跨 origin 蒙版/位移层后续批）。
                // P1 图案图章：采样 = 文档坐标 ÷ scale 对 patW×patH 取模（相位锚定文档原点），
                // srcAt 收到的已是图案网格局部坐标（wrap 后恒在界内）。
                const uint8_t* src = nullptr;
                if (patternMode_) {
                    const double docX = originX_ + tx + static_cast<double>(c) + 0.5;
                    const double docY = originY_ + ty + static_cast<double>(r) + 0.5;
                    long long sxF = static_cast<long long>(std::floor(docX / patScale_));
                    long long syF = static_cast<long long>(std::floor(docY / patScale_));
                    int sx = static_cast<int>(sxF % patW_);
                    if (sx < 0) sx += patW_;
                    int sy = static_cast<int>(syF % patH_);
                    if (sy < 0) sy += patH_;
                    src = srcAt(sx, sy);
                } else {
                    src = srcAt(
                        static_cast<int>(tx + static_cast<float>(c) - static_cast<float>(srcOffsetX_)),
                        static_cast<int>(ty + static_cast<float>(r) - static_cast<float>(srcOffsetY_)));
                }
                uint8_t* p = px + i;
                float srcP[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (src != nullptr) {
                    for (int ch = 0; ch < 4; ++ch) {
                        srcP[ch] = src[static_cast<size_t>(ch)] / 255.0f;
                    }
                } else if (!restoreMode_) {
                    continue;  // 图章：源越界 = 无源像素（底不变）；恢复：越界 = 透明恢复
                }
                if (restoreMode_) {
                    // RGBA 直混：out = src × covA + dst × (1−covA)
                    for (int ch = 0; ch < 4; ++ch) {
                        p[static_cast<size_t>(ch)] = static_cast<uint8_t>(
                            std::lround((srcP[ch] * covA + p[static_cast<size_t>(ch)] / 255.0f *
                                         (1.0f - covA)) * 255.0f));
                    }
                } else {
                    const float srcA = srcP[3] * covA;
                    if (srcA <= 0.0f) {
                        continue;
                    }
                    const float da = p[3] / 255.0f;
                    const float oa = srcA + da * (1.0f - srcA);
                    const float sP[3] = {srcP[0] * srcA, srcP[1] * srcA, srcP[2] * srcA};
                    for (int ch = 0; ch < 3; ++ch) {
                        const float d = p[static_cast<size_t>(ch)] / 255.0f * da * (1.0f - srcA);
                        p[static_cast<size_t>(ch)] =
                            static_cast<uint8_t>(std::lround(
                                std::min(1.0f, oa > 0.0001f ? (sP[ch] + d) / oa : 0.0f) * 255.0f));
                    }
                    p[3] = static_cast<uint8_t>(std::lround(std::min(1.0f, oa) * 255.0f));
                }
            }
        }
        return;
    }
    if (maskTarget_ || qmTarget_) {
        // 蒙版/快速蒙版灰度落笔：out = L×covA + base×(1-covA)（base 恒灰，RGB 同值）；erasing 忽略
        for (size_t i = 0; i < static_cast<size_t>(kTileSize) * kTileSize; ++i) {
            const float covA = (cov[i] / 255.0f) * op;
            uint8_t* p = px + i * 4;
            const float outg = maskLum_ * covA + (p[0] / 255.0f) * (1.0f - covA);
            const uint8_t g = static_cast<uint8_t>(std::lround(outg * 255.0f));
            p[0] = p[1] = p[2] = g;
            p[3] = 255;
        }
        return;
    }
    // P6 D1-b：深色层双写——U8 work 照常（live 预览域），workDeep_ 同步 f32 真合成
    //（提交即真深域，笔触精度无损）。公式与 U8 路径逐项一致。
    uint8_t* dpx = nullptr;
    size_t dbpp = 0;
    if (depth_ != PixelDepth::U8) {
        auto dit = workDeep_.find(key);
        if (dit == workDeep_.end()) {
            ensureWorking(key);
            dit = workDeep_.find(key);
        }
        if (dit != workDeep_.end()) {
            dpx = dit->second.data();
            dbpp = static_cast<size_t>(bytesPerPel(depth_));
        } else {
            deepU8Fb_[key] = true;  // 防御：深域瓦片缺失 → 提交升位回退
        }
    }
    for (size_t i = 0; i < static_cast<size_t>(kTileSize) * kTileSize; ++i) {
        const float cv = cov[i] / 255.0f;
        const float covA = cv * op;  // 笔画级 opacity 上限（覆盖层语义）
        uint8_t* p = px + i * 4;
        if (settings_.erasing) {
            // destinationOut：alpha 乘减，rgb 不动（直通域）
            p[3] = static_cast<uint8_t>(std::lround(p[3] * (1.0f - covA)));
            if (dpx != nullptr) {
                float d4[4];
                pixel_access::pixelDepthReadF32(depth_, dpx + i * dbpp, d4);
                d4[3] *= 1.0f - covA;
                pixel_access::pixelDepthWriteF32(depth_, dpx + i * dbpp, d4);
            }
            continue;
        }
        const float as = p[3] / 255.0f;
        const float ao = covA + as * (1.0f - covA);
        if (ao <= 0.0001f) {
            p[0] = p[1] = p[2] = p[3] = 0;
            if (dpx != nullptr) {
                const float z[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                pixel_access::pixelDepthWriteF32(depth_, dpx + i * dbpp, z);
            }
            continue;
        }
        const float srcRgb[3] = {settings_.red, settings_.green, settings_.blue};
        // premult 合成后转回直通：out_rgb = (src_rgb·covA + dst_rgb·as·(1-covA)) / ao
        // （M7.5 验证期修复：原实现三通道全用 cr，任何笔色都画成灰白）
        for (int c = 0; c < 3; ++c) {
            const float d = p[static_cast<size_t>(c)] / 255.0f;
            const float outP = srcRgb[c] * covA + d * as * (1.0f - covA);
            p[static_cast<size_t>(c)] =
                static_cast<uint8_t>(std::lround(std::min(1.0f, outP / ao) * 255.0f));
        }
        p[3] = static_cast<uint8_t>(std::lround(std::min(1.0f, ao) * 255.0f));
        if (dpx != nullptr) {
            float d4[4];
            pixel_access::pixelDepthReadF32(depth_, dpx + i * dbpp, d4);
            const float das = d4[3];
            const float dao = covA + das * (1.0f - covA);
            if (dao > 0.0001f) {
                for (int c = 0; c < 3; ++c) {
                    const float outP = srcRgb[c] * covA + d4[c] * das * (1.0f - covA);
                    d4[c] = std::min(1.0f, dao > 0.0001f ? outP / dao : 0.0f);
                }
            }
            d4[3] = std::min(1.0f, dao);
            pixel_access::pixelDepthWriteF32(depth_, dpx + i * dbpp, d4);
        }
    }
}

std::shared_ptr<const TileGrid> StrokeDraft::commitGrid(const TileGrid* original) {
    if (gridCols_ == 0) {
        return nullptr;
    }
    if (healKind_ != HealKind::None) {
        commitHeal();  // M19a：失败时 healRgb_ 为空 → paintTileHeal 退化为 base 原色（无操作提交）
    }
    auto grid = std::make_shared<TileGrid>();
    grid->cols = gridCols_;
    grid->rows = gridRows_;
    grid->revision = gridRevision;
    grid->depth = depth_;
    if (original != nullptr) {
        for (const auto& kv : original->tiles) {
            grid->tiles[kv.first] = kv.second;
        }
    }
    for (const uint32_t key : touched_) {
        if (coverage_.count(key) == 0) {
            continue;
        }
        if (healKind_ != HealKind::None) {
            // 重建式：work 从 base 重拷再合成（live 期 clone 纹理/尾段残留不二次混色）
            auto workIt2 = work_.find(key);
            if (workIt2 != work_.end()) {
                std::vector<uint8_t> fresh(static_cast<size_t>(kTileSize) * kTileSize * 4u, 0);
                if (base_ != nullptr) {
                    auto bit = base_->tiles.find(key);
                    if (bit != base_->tiles.end() && bit->second != nullptr) {
                        const uint8_t* src = bit->second->pixels->mapCpu();
                        if (src != nullptr) {
                            std::memcpy(fresh.data(), src, fresh.size());
                        }
                        bit->second->pixels->unmap();
                    }
                }
                workIt2->second = std::move(fresh);
            }
            paintTileHeal(key);
        } else {
            paintTile(key);
        }
        auto it = work_.find(key);
        if (it == work_.end()) {
            continue;
        }
        auto tile = std::make_shared<Tile>();
        // P6 D1-b：深色层产出真深格式瓦片——workDeep_ 有深域合成结果（普通/橡皮路径）
        // 直拷；效果/克隆/修复族（deepU8Fb_）从 U8 work 升位覆盖（v1 精度登记）。
        auto buf = std::make_unique<EngineBuffer>(kTileSize, kTileSize, depth_);
        uint8_t* dst = buf->mapCpuWrite();
        const auto fb = deepU8Fb_.find(key);
        const auto dit = workDeep_.find(key);
        if (depth_ != PixelDepth::U8) {
            // 深色层：fallback（效果/克隆/修复族）或深域瓦片缺失 → U8 work 升位；否则真深直拷
            const bool u8Fb = (fb != deepU8Fb_.end() && fb->second) || dit == workDeep_.end();
            const size_t bpp = static_cast<size_t>(bytesPerPel(depth_));
            if (u8Fb) {
                for (size_t i = 0; i < static_cast<size_t>(kTileSize) * kTileSize; ++i) {
                    const uint8_t* u = it->second.data() + i * 4u;
                    const float in4[4] = {u[0] * (1.0f / 255.0f), u[1] * (1.0f / 255.0f),
                                          u[2] * (1.0f / 255.0f), u[3] * (1.0f / 255.0f)};
                    pixel_access::pixelDepthWriteF32(depth_, dst + i * bpp, in4);
                }
            } else {
                std::memcpy(dst, dit->second.data(),
                            static_cast<size_t>(kTileSize) * kTileSize * bpp);
            }
        } else {
            std::memcpy(dst, it->second.data(),
                        static_cast<size_t>(kTileSize) * kTileSize * 4);
        }
        tile->pixels = std::move(buf);
        grid->tiles[key] = std::move(tile);
    }
    return grid;
}

// ---- M19a 修复族提交 ----

namespace {

// 网格局部矩形
struct GridRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
};

// 掩码方形膨胀（photocraft retouch_cmds::dilate 同式）
void dilateMask(int w, int h, std::vector<bool>& m, int r) {
    if (r <= 0) {
        return;
    }
    std::vector<bool> tmp(m.size(), false);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (m[static_cast<size_t>(y) * w + x]) {
                for (int xx = std::max(0, x - r); xx < std::min(w, x + r + 1); ++xx) {
                    tmp[static_cast<size_t>(y) * w + xx] = true;
                }
            }
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (tmp[static_cast<size_t>(y) * w + x]) {
                for (int yy = std::max(0, y - r); yy < std::min(h, y + r + 1); ++yy) {
                    m[static_cast<size_t>(yy) * w + x] = true;
                }
            }
        }
    }
}

}  // namespace

bool StrokeDraft::commitHeal() {
    // 笔画覆盖包围盒（网格局部坐标）
    GridRect bbox{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    for (const auto& [key, cov] : coverage_) {
        const int tx = static_cast<int>(key % gridCols_) * static_cast<int>(kTileSize);
        const int ty = static_cast<int>(key / gridCols_) * static_cast<int>(kTileSize);
        for (uint32_t r = 0; r < kTileSize; ++r) {
            for (uint32_t c = 0; c < kTileSize; ++c) {
                if (cov[static_cast<size_t>(r) * kTileSize + c] > 0) {
                    bbox.x0 = std::min(bbox.x0, tx + static_cast<int>(c));
                    bbox.y0 = std::min(bbox.y0, ty + static_cast<int>(r));
                    bbox.x1 = std::max(bbox.x1, tx + static_cast<int>(c) + 1);
                    bbox.y1 = std::max(bbox.y1, ty + static_cast<int>(r) + 1);
                }
            }
        }
    }
    if (bbox.x1 <= bbox.x0) {
        return false;
    }
    const int gw = static_cast<int>(gridCols_ * kTileSize);
    const int gh = static_cast<int>(gridRows_ * kTileSize);
    if (healKind_ == HealKind::Heal) {
        // 修复画笔：bbox+2 域 seamlessClone（源 = 克隆偏移采样，目标 = 笔画前像素）。
        // RGBA 四通道同解（alpha 参与膜解——透明环境的修复结果透明，photocraft 同）。
        const GridRect region{std::max(0, bbox.x0 - 2), std::max(0, bbox.y0 - 2),
                              std::min(gw, bbox.x1 + 2), std::min(gh, bbox.y1 + 2)};
        const int w = region.w(), h = region.h();
        if (w <= 0 || h <= 0 || base_ == nullptr) {
            return false;
        }
        std::vector<float> src(static_cast<size_t>(w) * h * 4), dst(static_cast<size_t>(w) * h * 4);
        std::vector<bool> mask(static_cast<size_t>(w) * h, false);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t k = static_cast<size_t>(y) * w + x;
                const int gx = region.x0 + x, gy = region.y0 + y;
                const uint8_t* d = baseAt(gx, gy);
                if (d != nullptr) {
                    for (int c = 0; c < 4; ++c) {
                        dst[k * 4 + c] = d[c] / 255.0f;
                    }
                }
                const uint8_t* s = srcAt(gx - static_cast<int>(srcOffsetX_),
                                         gy - static_cast<int>(srcOffsetY_));
                if (s != nullptr) {
                    for (int c = 0; c < 4; ++c) {
                        src[k * 4 + c] = s[c] / 255.0f;
                    }
                }
                // 源越界/缺瓦片 = 透明（稀疏网格语义：那里本来就是透明像素；
                // 回退 dst 会零化边界失配使修复变 no-op——E2E 实证）
                const auto covIt = coverage_.find(
                    static_cast<uint32_t>(gy / static_cast<int>(kTileSize)) * gridCols_ +
                    static_cast<uint32_t>(gx / static_cast<int>(kTileSize)));
                if (covIt != coverage_.end()) {
                    const uint8_t cv = covIt->second[static_cast<size_t>(gy % static_cast<int>(kTileSize)) *
                                                     kTileSize + static_cast<uint32_t>(gx % static_cast<int>(kTileSize))];
                    mask[k] = cv > 0;
                }
            }
        }
        const std::vector<float> healed = poisson::seamlessClone(
            static_cast<size_t>(w), static_cast<size_t>(h), 4, src, dst, mask);
        healRgb_.assign(static_cast<size_t>(w) * h * 4, 0);
        for (size_t i = 0; i < static_cast<size_t>(w) * h * 4; ++i) {
            healRgb_[i] = static_cast<uint8_t>(
                std::lround(std::min(1.0f, std::max(0.0f, healed[i])) * 255.0f));
        }
        healRx_ = region.x0;
        healRy_ = region.y0;
        healRw_ = w;
        healRh_ = h;
        return true;
    }
    // 污点修复三模式：采样窗口（PS 语义）内填充 + seamlessClone。
    const inpaint::RectI holeDoc{bbox.x0 + originX_, bbox.y0 + originY_,
                                 bbox.x1 + originX_, bbox.y1 + originY_};
    const inpaint::RectI canvas{0, 0, canvasW_ > 0 ? canvasW_ : gw, canvasH_ > 0 ? canvasH_ : gh};
    inpaint::RectI win = inpaint::samplingWindow(holeDoc, canvas);
    // 预算护栏：窗口 ≤16MP（photocraft 修复族上限），超限向孔收缩
    while (static_cast<double>(win.width()) * win.height() > 16e6 &&
           (win.width() > holeDoc.width() * 2 || win.height() > holeDoc.height() * 2)) {
        const int cx = (win.x0 + win.x1) / 2, cy = (win.y0 + win.y1) / 2;
        const int nw = std::max((win.x1 - win.x0) / 2, holeDoc.width());
        const int nh = std::max((win.y1 - win.y0) / 2, holeDoc.height());
        win = inpaint::RectI{std::max(win.x0, cx - nw / 2), std::max(win.y0, cy - nh / 2),
                             std::min(win.x1, cx - nw / 2 + nw), std::min(win.y1, cy - nh / 2 + nh)};
    }
    // 转网格局部并夹到网格
    const GridRect region{std::max(0, win.x0 - originX_), std::max(0, win.y0 - originY_),
                          std::min(gw, win.x1 - originX_), std::min(gh, win.y1 - originY_)};
    const int w = region.w(), h = region.h();
    if (w <= 0 || h <= 0 || base_ == nullptr) {
        return false;
    }
    std::vector<float> img(static_cast<size_t>(w) * h * 4);
    std::vector<bool> hole(static_cast<size_t>(w) * h, false);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t k = static_cast<size_t>(y) * w + x;
            const int gx = region.x0 + x, gy = region.y0 + y;
            const uint8_t* d = baseAt(gx, gy);
            if (d != nullptr) {
                for (int c = 0; c < 4; ++c) {
                    img[k * 4 + c] = d[c] / 255.0f;
                }
            }
            const auto covIt = coverage_.find(
                static_cast<uint32_t>(gy / static_cast<int>(kTileSize)) * gridCols_ +
                static_cast<uint32_t>(gx / static_cast<int>(kTileSize)));
            if (covIt != coverage_.end()) {
                hole[k] = covIt->second[static_cast<size_t>(gy % static_cast<int>(kTileSize)) *
                                        kTileSize + static_cast<uint32_t>(gx % static_cast<int>(kTileSize))] > 0;
            }
        }
    }
    inpaint::CompleteParams p;
    const float sizeF = settings_.diameter;
    auto fillContentAware = [&](const std::vector<float>& base) {
        inpaint::CompleteResult r =
            inpaint::complete(static_cast<size_t>(w), static_cast<size_t>(h), 4, base, hole, p);
        if (r.status == inpaint::CompleteStatus::Ok) {
            return r.img;
        }
        return poisson::membraneFill(static_cast<size_t>(w), static_cast<size_t>(h), 4, base, hole);
    };
    std::vector<float> filled;
    switch (healKind_) {
        case HealKind::SpotTexture:
            filled = inpaint::synthesize(static_cast<size_t>(w), static_cast<size_t>(h), 4, img,
                                         hole,
                                         static_cast<size_t>(std::min(32.0f, std::max(6.0f, sizeF / 4.0f))),
                                         0x5eedull);
            break;
        case HealKind::SpotProximity: {
            const int ring = static_cast<int>(std::min(16.0f, std::max(3.0f, sizeF / 8.0f)));
            const int margin = std::max(std::max(w, h) / 2, 1);
            auto off = inpaint::bestOffset(static_cast<size_t>(w), static_cast<size_t>(h), 4, img,
                                           hole, static_cast<size_t>(ring), margin);
            if (off.has_value()) {
                // 预修复像素的平移拷贝（重叠位移不重复采已写像素——photocraft 同语义）
                filled.assign(img.size(), 0.0f);
                bool ok = true;
                for (int y = 0; y < h && ok; ++y) {
                    for (int x = 0; x < w; ++x) {
                        const size_t k = static_cast<size_t>(y) * w + x;
                        if (!hole[k]) {
                            for (int c = 0; c < 4; ++c) {
                                filled[k * 4 + c] = img[k * 4 + c];
                            }
                            continue;
                        }
                        const int sx = x + off->first, sy = y + off->second;
                        if (sx < 0 || sy < 0 || sx >= w || sy >= h) {
                            ok = false;
                            break;
                        }
                        const size_t s = static_cast<size_t>(sy) * w + sx;
                        for (int c = 0; c < 4; ++c) {
                            filled[k * 4 + c] = img[s * 4 + c];
                        }
                    }
                }
                if (!ok) {
                    filled = fillContentAware(img);
                }
            } else {
                filled = fillContentAware(img);
            }
            break;
        }
        default:
            filled = fillContentAware(img);
            break;
    }
    // 融合域 = 孔外扩 1（合成值与未动环境的失配即膜要修的量——photocraft 同注）
    std::vector<bool> healMask = hole;
    dilateMask(w, h, healMask, 1);
    const std::vector<float> healed = poisson::seamlessClone(static_cast<size_t>(w),
                                                             static_cast<size_t>(h), 4, filled,
                                                             img, healMask);
    healRgb_.assign(static_cast<size_t>(w) * h * 4, 0);
    for (size_t i = 0; i < static_cast<size_t>(w) * h * 4; ++i) {
        healRgb_[i] = static_cast<uint8_t>(
            std::lround(std::min(1.0f, std::max(0.0f, healed[i])) * 255.0f));
    }
    healRx_ = region.x0;
    healRy_ = region.y0;
    healRw_ = w;
    healRh_ = h;
    return true;
}

// healed 色源（RGBA，alpha 参与膜解）经 coverage 单次「替换式」合成：
// out = healed × covA + base × (1 − covA)（RGBA 四通道直混——透明修复结果必须能删除
// 目标墨迹，src-over 会保留原污点，M11f restoreMode 同一语义教训）；色源区外 = base 原样。
void StrokeDraft::paintTileHeal(uint32_t key) {
    if (depth_ != PixelDepth::U8) {
        deepU8Fb_[key] = true;  // P6：heal 色源/泊松域 v1 U8（提交升位，登记精度）
    }
    auto covIt = coverage_.find(key);
    auto workIt = work_.find(key);
    if (covIt == coverage_.end() || workIt == work_.end()) {
        return;
    }
    ++workRev_[key];
    const int tx = static_cast<int>(key % gridCols_) * static_cast<int>(kTileSize);
    const int ty = static_cast<int>(key / gridCols_) * static_cast<int>(kTileSize);
    uint8_t* px = workIt->second.data();
    const uint8_t* cov = covIt->second.data();
    const float op = settings_.opacity;
    for (uint32_t r = 0; r < kTileSize; ++r) {
        for (uint32_t c = 0; c < kTileSize; ++c) {
            const size_t i = (static_cast<size_t>(r) * kTileSize + c) * 4u;
            const float covA = (cov[i / 4u] / 255.0f) * op;
            if (covA <= 0.0f) {
                continue;
            }
            const int gx = tx + static_cast<int>(c), gy = ty + static_cast<int>(r);
            float srcP[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (gx >= healRx_ && gy >= healRy_ && gx < healRx_ + healRw_ && gy < healRy_ + healRh_) {
                const size_t k = (static_cast<size_t>(gy - healRy_) * healRw_ + static_cast<size_t>(gx - healRx_)) * 4u;
                for (int ch = 0; ch < 4; ++ch) {
                    srcP[ch] = healRgb_[k + static_cast<size_t>(ch)] / 255.0f;
                }
            } else {
                // 色源区外（heal 计算失败回退）：base 原样 = 无操作
                const uint8_t* d = baseAt(gx, gy);
                if (d == nullptr) {
                    continue;
                }
                for (int ch = 0; ch < 4; ++ch) {
                    srcP[ch] = d[ch] / 255.0f;
                }
            }
            // RGBA 直混（含 alpha 还原/删除）
            uint8_t* p = px + i;
            for (int ch = 0; ch < 4; ++ch) {
                p[static_cast<size_t>(ch)] = static_cast<uint8_t>(std::lround(
                    (srcP[ch] * covA + p[static_cast<size_t>(ch)] / 255.0f * (1.0f - covA)) *
                    255.0f));
            }
        }
    }
}

}  // namespace montage
