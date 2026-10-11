#include "tiles/quick_mask.h"

namespace montage {
namespace tiles {

bool maskHasSelection(const TileGrid& mask) {
    for (const auto& kv : mask.tiles) {
        if (kv.second == nullptr) {
            continue;
        }
        const uint8_t* px = kv.second->pixels->mapCpu();
        if (px == nullptr) {
            continue;
        }
        const size_t n = static_cast<size_t>(kTileSize) * kTileSize * 4u;
        bool hit = false;
        for (size_t i = 0; i < n; i += 4) {
            if (px[i] != 0) {
                hit = true;
                break;
            }
        }
        kv.second->pixels->unmap();
        if (hit) {
            return true;
        }
    }
    return false;
}

bool enterQuickMask(Document& doc) {
    if (doc.width == 0 || doc.height == 0 || doc.quickMask != nullptr) {
        return false;
    }
    if (doc.selection != nullptr) {
        doc.quickMask = doc.selection;  // 不可变网格共享（通道 = 选区克隆语义）
    } else {
        // 空网格 = 缺瓦片语义 0 = 全图未选中（红 overlay 满铺），画白笔建选区
        auto g = std::make_shared<TileGrid>();
        g->cols = (doc.width + kTileSize - 1u) / kTileSize;
        g->rows = (doc.height + kTileSize - 1u) / kTileSize;
        doc.quickMask = std::move(g);
    }
    doc.selection = nullptr;
    return true;
}

bool exitQuickMask(Document& doc) {
    if (doc.quickMask == nullptr) {
        return false;
    }
    // 全 0 通道 → null（PS：全红退出 = 无选区，不落全零网格）
    doc.selection = maskHasSelection(*doc.quickMask) ? doc.quickMask : nullptr;
    doc.quickMask = nullptr;
    return true;
}

}  // namespace tiles
}  // namespace montage
