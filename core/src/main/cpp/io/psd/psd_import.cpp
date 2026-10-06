#include "io/psd/psd_import.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unistd.h>

#include <hilog/log.h>

#include "engine/engine.h"
#include "io/psd/psd_reader.h"

namespace montage {
namespace io {
namespace {

constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.PSD";

void notifyProgress(double progress) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.import.mtx);
        e.import.progress = progress;
    }
    if (e.progressTsfn != nullptr) {
        napi_call_threadsafe_function(
            e.progressTsfn,
            reinterpret_cast<void*>(static_cast<intptr_t>(progress * 1000.0 + 0.5)),
            napi_tsfn_nonblocking);
    }
}

// 预乘 → 直通 alpha（引擎瓦片存储约定）
void unpremultiply(std::vector<uint8_t>& rgba) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const uint8_t a = rgba[i + 3];
        if (a == 255 || a == 0) {
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            rgba[i + static_cast<size_t>(c)] =
                static_cast<uint8_t>(std::min(255, (rgba[i + static_cast<size_t>(c)] * 255 + a / 2) / a));
        }
    }
}

// 蒙版网格构建（M5a 非破坏，对拍 bakeMask 的 maskOnLayerGrid 语义）：patch 原样入网格
// （RGBA 灰度），offset = maskRect - layerRect，patch 外取 maskDefault。返回 null = 无蒙版。
std::shared_ptr<LayerMask> buildMask(const psd::Record& rec) {
    if (!rec.hasMask || rec.mask.empty() || rec.maskW <= 0 || rec.maskH <= 0) {
        return nullptr;
    }
    const uint32_t cols = (static_cast<uint32_t>(rec.maskW) + kTileSize - 1u) / kTileSize;
    const uint32_t rows = (static_cast<uint32_t>(rec.maskH) + kTileSize - 1u) / kTileSize;
    TileGridBuilder builder(cols, rows);
    for (uint32_t ty = 0; ty < rows; ++ty) {
        const uint32_t y0 = ty * kTileSize;
        const uint32_t copyH = std::min(kTileSize, static_cast<uint32_t>(rec.maskH) - y0);
        for (uint32_t tx = 0; tx < cols; ++tx) {
            const uint32_t x0 = tx * kTileSize;
            const uint32_t copyW = std::min(kTileSize, static_cast<uint32_t>(rec.maskW) - x0);
            EngineBuffer& buf = builder.ensureTile(tx, ty);
            uint8_t* dst = buf.mapCpuWrite();
            for (uint32_t r = 0; r < copyH; ++r) {
                uint8_t* dstRow = dst + static_cast<size_t>(buf.rowBytes()) * r;
                const uint8_t* srcRow =
                    rec.mask.data() + static_cast<size_t>(y0 + r) * static_cast<uint32_t>(rec.maskW) + x0;
                for (uint32_t x = 0; x < copyW; ++x) {
                    const uint8_t g = srcRow[x];
                    dstRow[x * 4u + 0u] = g;
                    dstRow[x * 4u + 1u] = g;
                    dstRow[x * 4u + 2u] = g;
                    dstRow[x * 4u + 3u] = 255;
                }
            }
        }
    }
    auto m = std::make_shared<LayerMask>();
    m->pixels = builder.publish(1);
    m->width = static_cast<uint32_t>(rec.maskW);
    m->height = static_cast<uint32_t>(rec.maskH);
    m->offsetX = rec.maskLeft - rec.left;
    m->offsetY = rec.maskTop - rec.top;
    m->outside = rec.maskDefault;
    m->enabled = rec.maskEnabled;
    m->linked = true;
    return m;
}

// 剪贴烘焙：clip 像素 × base alpha（文档坐标对齐，预乘域等比缩放）
void bakeClipping(psd::Record& rec, const psd::Record& base) {
    if (base.width <= 0 || base.height <= 0 || rec.width <= 0 || rec.height <= 0) {
        return;
    }
    for (int y = 0; y < rec.height; ++y) {
        const int by = rec.top + y - base.top;
        if (by < 0 || by >= base.height) {
            continue;
        }
        for (int x = 0; x < rec.width; ++x) {
            const int bx = rec.left + x - base.left;
            if (bx < 0 || bx >= base.width) {
                continue;
            }
            const size_t baseAt = (static_cast<size_t>(by) * base.width + bx) * 4 + 3;
            const uint16_t k = base.rgba[baseAt];
            if (k == 255) {
                continue;
            }
            uint8_t* px = rec.rgba.data() + (static_cast<size_t>(y) * rec.width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                px[c] = static_cast<uint8_t>((px[c] * k + 127) / 255);
            }
        }
    }
    rec.notes.push_back("Clipping applied by baking into pixels (layer stack editing arrives later).");
}

}  // namespace

bool importPsd(int fd, int* outWidth, int* outHeight, std::vector<std::string>* notes,
               std::string& err) {
    // 全量读入（PSD 解析需随机访问；典型工程文件数十 MB）
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) {
        close(fd);
        err = "empty file";
        return false;
    }
    lseek(fd, 0, SEEK_SET);
    std::vector<uint8_t> data(static_cast<size_t>(sz));
    size_t got = 0;
    while (got < data.size()) {
        ssize_t n = read(fd, data.data() + got, data.size() - got);
        if (n <= 0) {
            break;
        }
        got += static_cast<size_t>(n);
    }
    close(fd);
    if (got != data.size()) {
        err = "read failed";
        return false;
    }

    auto& engine = Engine::get();
    psd::PsdDocument doc;
    if (!psd::readPsd(data.data(), data.size(), doc, err)) {
        return false;
    }

    // 烘焙：蒙版 → 剪贴 → 直通 alpha；逐层切瓦片
    std::map<uint64_t, psd::Record*> baked;  // id → 已烘焙记录（剪贴 base 引用）
    struct Built {
        Layer layer;
    };
    std::vector<Layer> layers;
    layers.reserve(doc.layers.size());
    std::map<uint64_t, uint64_t> baseForParent;  // parentId(0=根) → base layerId
    int done = 0;
    for (psd::Record& rec : doc.layers) {
        done++;
        notifyProgress(0.15 + 0.8 * static_cast<double>(done) /
                                   static_cast<double>(std::max<size_t>(doc.layers.size(), 1)));
        const uint64_t parent = 0;  // M2 无组层级：全部视作根（组已扁平化）
        if (rec.kind == psd::LayerKind::Adjustment) {
            continue;  // 跳过调整层（note 已带）
        }
        Layer layer;
        layer.id = engine.nextLayerId();
        layer.name = rec.name;
        layer.visible = rec.visible;
        layer.opacity = std::min(1.0, std::max(0.0, rec.opacity));
        layer.blendMode = rec.blendMode;
        layer.transform.originX = rec.left;
        layer.transform.originY = rec.top;
        layer.transform.width = rec.width;
        layer.transform.height = rec.height;

        if (!rec.isGroup && rec.width > 0 && rec.height > 0 && !rec.rgba.empty()) {
            if (rec.clipping) {
                auto baseIt = baseForParent.find(parent);
                if (baseIt != baseForParent.end()) {
                    auto b = baked.find(baseIt->second);
                    if (b != baked.end()) {
                        bakeClipping(rec, *b->second);
                    } else {
                        rec.notes.push_back("This clipping mask's base isn't supported, so clipping was skipped.");
                    }
                } else {
                    rec.notes.push_back("This clipping mask's base isn't supported, so clipping was skipped.");
                }
            }
            baked[layer.id] = &rec;
            baseForParent[parent] = layer.id;
            unpremultiply(rec.rgba);

            const uint32_t cols =
                (static_cast<uint32_t>(rec.width) + kTileSize - 1u) / kTileSize;
            const uint32_t rows =
                (static_cast<uint32_t>(rec.height) + kTileSize - 1u) / kTileSize;
            TileGridBuilder builder(cols, rows);
            for (uint32_t ty = 0; ty < rows; ++ty) {
                const uint32_t tileY0 = ty * kTileSize;
                const uint32_t copyH = std::min(kTileSize, static_cast<uint32_t>(rec.height) - tileY0);
                for (uint32_t tx = 0; tx < cols; ++tx) {
                    const uint32_t tileX0 = tx * kTileSize;
                    const uint32_t copyW =
                        std::min(kTileSize, static_cast<uint32_t>(rec.width) - tileX0);
                    EngineBuffer& buf = builder.ensureTile(tx, ty);
                    uint8_t* dstBase = buf.mapCpuWrite();
                    for (uint32_t r = 0; r < copyH; ++r) {
                        const uint8_t* src =
                            rec.rgba.data() +
                            (static_cast<size_t>(tileY0 + r) * static_cast<uint32_t>(rec.width) + tileX0) * 4u;
                        std::memcpy(dstBase + static_cast<size_t>(buf.rowBytes()) * r, src,
                                    static_cast<size_t>(copyW) * 4u);
                    }
                }
            }
            layer.pixels = builder.publish(1);
            // M5a 非破坏蒙版：patch 独立成网格 + 合成派生网格（原 bakeMask 改为不改像素）
            layer.mask = buildMask(rec);
            if (layer.mask != nullptr && layer.mask->enabled) {
                layer.render = composeMasked(layer.pixels, *layer.mask);
            }
        } else if (rec.clipping) {
            rec.notes.push_back("This clipping mask's base isn't supported, so clipping was skipped.");
        }

        for (const std::string& n : rec.notes) {
            if (notes != nullptr) {
                notes->push_back(rec.name + ": " + n);
            }
        }
        layers.push_back(std::move(layer));
    }

    {
        std::lock_guard<std::mutex> lk(engine.docMutex);
        engine.doc = Document{};
        engine.doc.width = doc.width;
        engine.doc.height = doc.height;
        engine.doc.name = "PSD";
        engine.doc.layers = std::move(layers);
        engine.doc.activeId = engine.doc.layers.empty()
                                  ? 0
                                  : engine.doc.layers[engine.doc.layers.size() - 1].id;
        engine.history.clear();  // 整档替换：撤销不跨文档
        engine.bumpRevisionLocked();
        engine.requestRender();
    }
    if (outWidth != nullptr) {
        *outWidth = doc.width;
    }
    if (outHeight != nullptr) {
        *outHeight = doc.height;
    }
    if (notes != nullptr) {
        if (doc.resolution != 72.0) {
            notes->insert(notes->begin(),
                          "PSD: " + std::to_string(doc.width) + "x" + std::to_string(doc.height) +
                              " @ " + std::to_string(static_cast<int>(doc.resolution)) + "dpi");
        } else {
            notes->insert(notes->begin(), "PSD: " + std::to_string(doc.width) + "x" +
                                              std::to_string(doc.height));
        }
    }
    notifyProgress(1.0);
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "imported %{public}dx%{public}d layers=%{public}zu",
                 doc.width, doc.height, doc.layers.size());
    return true;
}

}  // namespace io
}  // namespace montage
