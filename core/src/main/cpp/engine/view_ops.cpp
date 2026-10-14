#include "engine/view_ops.h"

#include <algorithm>
#include <cmath>

namespace montage {

namespace {
constexpr size_t kMaxGuides = 256;

std::vector<float>* guideList(Document& doc, int orient) {
    return orient == 0 ? &doc.guidesH : &doc.guidesV;
}

const std::vector<float>* guideListConst(const Document& doc, int orient) {
    return orient == 0 ? &doc.guidesH : &doc.guidesV;
}

// 网格墨迹精确包围盒（alpha > 0 逐像素扫描；瓦片恒 kTileSize 见方、行距 rowBytes）
bool gridInkBBox(const TileGrid& grid, uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h) {
    uint32_t bx0 = UINT32_MAX, by0 = UINT32_MAX, bx1 = 0, by1 = 0;
    for (const auto& [key, tile] : grid.tiles) {
        const uint32_t tx = key % grid.cols;
        const uint32_t ty = key / grid.cols;
        const uint8_t* p = tile->pixels->mapCpu();
        if (p == nullptr) {
            continue;
        }
        const uint32_t rb = tile->pixels->rowBytes();
        uint32_t lx0 = kTileSize, ly0 = kTileSize, lx1 = 0, ly1 = 0;
        for (uint32_t py = 0; py < kTileSize; ++py) {
            const uint8_t* row = p + static_cast<size_t>(py) * rb;
            for (uint32_t px = 0; px < kTileSize; ++px) {
                if (row[px * 4 + 3] != 0) {
                    lx0 = std::min(lx0, px);
                    ly0 = std::min(ly0, py);
                    lx1 = std::max(lx1, px);
                    ly1 = std::max(ly1, py);
                }
            }
        }
        if (lx0 == kTileSize) {
            continue;  // 瓦片全透明
        }
        bx0 = std::min(bx0, tx * kTileSize + lx0);
        by0 = std::min(by0, ty * kTileSize + ly0);
        bx1 = std::max(bx1, tx * kTileSize + lx1);
        by1 = std::max(by1, ty * kTileSize + ly1);
    }
    if (bx0 == UINT32_MAX) {
        return false;
    }
    x = bx0;
    y = by0;
    w = bx1 - bx0 + 1;
    h = by1 - by0 + 1;
    return true;
}
}  // namespace

bool addGuide(Document& doc, int orient, double pos, std::string& err) {
    if (doc.guidesLocked) {
        err = "guides locked";
        return false;
    }
    const double limit = orient == 0 ? doc.height : doc.width;
    if (!(pos >= 0.0 && pos <= limit)) {
        err = "guide out of document";
        return false;
    }
    std::vector<float>* g = guideList(doc, orient);
    if (g->size() >= kMaxGuides) {
        err = "too many guides";
        return false;
    }
    const float v = static_cast<float>(std::lround(pos));
    auto it = std::lower_bound(g->begin(), g->end(), v);
    if (it != g->end() && *it == v) {
        err = "duplicate guide";
        return false;
    }
    g->insert(it, v);
    return true;
}

bool moveGuide(Document& doc, int orient, int index, double pos, std::string& err) {
    if (doc.guidesLocked) {
        err = "guides locked";
        return false;
    }
    std::vector<float>* g = guideList(doc, orient);
    if (index < 0 || static_cast<size_t>(index) >= g->size()) {
        err = "guide index out of range";
        return false;
    }
    const double limit = orient == 0 ? doc.height : doc.width;
    const float v = static_cast<float>(std::clamp(std::lround(pos), 0L, static_cast<long>(limit)));
    float old = (*g)[static_cast<size_t>(index)];
    g->erase(g->begin() + index);
    auto it = std::lower_bound(g->begin(), g->end(), v);
    if (it != g->end() && *it == v) {
        // 拖到已有线上：还原回原位（升序中 old 的槽位）
        auto rit = std::lower_bound(g->begin(), g->end(), old);
        g->insert(rit, old);
        return true;
    }
    g->insert(it, v);
    return true;
}

bool removeGuide(Document& doc, int orient, int index, std::string& err) {
    if (doc.guidesLocked) {
        err = "guides locked";
        return false;
    }
    std::vector<float>* g = guideList(doc, orient);
    if (index < 0 || static_cast<size_t>(index) >= g->size()) {
        err = "guide index out of range";
        return false;
    }
    g->erase(g->begin() + index);
    return true;
}

void clearGuides(Document& doc) {
    doc.guidesH.clear();
    doc.guidesV.clear();
}

void setGuidesLocked(Document& doc, bool locked) {
    doc.guidesLocked = locked;
}

void setGuidesVisible(Document& doc, bool visible) {
    doc.guidesVisible = visible;
}

bool layerContentBBox(const Document& doc, LayerId id,
                      double& x, double& y, double& w, double& h, std::string& err) {
    const Layer* found = nullptr;
    for (const Layer& l : doc.layers) {
        if (l.id == id) {
            found = &l;
            break;
        }
    }
    if (found == nullptr) {
        err = "layer not found";
        return false;
    }
    if (found->isGroup) {
        err = "group has no bounds";
        return false;
    }
    if (found->adjustment != nullptr) {
        err = "adjustment layer has no bounds";
        return false;
    }
    const std::shared_ptr<const TileGrid> px = found->effectivePixels();
    if (px == nullptr) {
        err = "layer has no pixels";
        return false;
    }
    uint32_t bx = 0, by = 0, bw = 0, bh = 0;
    if (!gridInkBBox(*px, bx, by, bw, bh)) {
        err = "layer is empty";
        return false;
    }
    x = found->effectiveOriginX() + bx;
    y = found->effectiveOriginY() + by;
    w = bw;
    h = bh;
    return true;
}

bool selectionBBox(const Document& doc, double& x, double& y, double& w, double& h) {
    if (doc.selection == nullptr) {
        return false;
    }
    uint32_t bx = 0, by = 0, bw = 0, bh = 0;
    if (!gridInkBBox(*doc.selection, bx, by, bw, bh)) {
        return false;
    }
    x = bx;
    y = by;
    w = bw;
    h = bh;
    return true;
}

}  // namespace montage
