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

    // 烘焙：蒙版 → 剪贴 → 直通 alpha；逐层切瓦片（M5b-2 起蒙版/剪贴均非破坏）
    std::vector<Layer> layers;
    layers.reserve(doc.layers.size());
    int done = 0;
    for (psd::Record& rec : doc.layers) {
        done++;
        notifyProgress(0.15 + 0.8 * static_cast<double>(done) /
                                   static_cast<double>(std::max<size_t>(doc.layers.size(), 1)));
        const uint64_t parent = 0;  // M2 无组层级：全部视作根（组已扁平化）
        if (rec.kind == psd::LayerKind::Adjustment) {
            // M9d：levl/curv/nvrt → 非破坏调整层（无像素内容）；其余仍跳过
            auto adj = std::make_shared<LayerAdjustment>();
            bool mapped = false;
            if (rec.adjKey == "levl" && rec.adjPayload.size() >= 14) {
                // ver u32 + inB i16 + inW i16 + gamma i16(/100) + outB i16 + outW i16
                auto rd16 = [&rec](size_t o) -> int {
                    return static_cast<int16_t>(rec.adjPayload[o] << 8 | rec.adjPayload[o + 1]);
                };
                const int inB = rd16(4);
                const int inW = rd16(6);
                const int gm = rd16(8);
                adj->kind = AdjustmentKind::Levels;
                adj->inBlack = std::min(1.0f, std::max(0.0f, inB / 255.0f));
                adj->inWhite = std::min(1.0f, std::max(0.001f, inW / 255.0f));
                adj->gamma = std::min(10.0f, std::max(0.1f, gm / 100.0f));
                mapped = true;
            } else if (rec.adjKey == "curv" && rec.adjPayload.size() >= 6) {
                // ver u32 + count u16 + count×(x,y) 字节
                const uint32_t ver =
                    static_cast<uint32_t>(rec.adjPayload[0]) << 24 |
                    static_cast<uint32_t>(rec.adjPayload[1]) << 16 |
                    static_cast<uint32_t>(rec.adjPayload[2]) << 8 | rec.adjPayload[3];
                if (ver == 1) {
                    const uint32_t count = static_cast<uint32_t>(rec.adjPayload[4]) << 8 |
                                           rec.adjPayload[5];
                    if (count >= 2 && rec.adjPayload.size() >= 6 + count * 2) {
                        adj->kind = AdjustmentKind::Curves;
                        for (uint32_t p = 0; p < count; ++p) {
                            adj->curveX.push_back(rec.adjPayload[6 + p * 2] / 255.0f);
                            adj->curveY.push_back(rec.adjPayload[7 + p * 2] / 255.0f);
                        }
                        mapped = true;
                    }
                }
            } else if (rec.adjKey == "nvrt") {
                adj->kind = AdjustmentKind::Invert;
                mapped = true;
            }
            if (mapped) {
                Layer layer;
                layer.id = engine.nextLayerId();
                layer.name = rec.name;
                layer.visible = rec.visible;
                layer.opacity = std::min(1.0, std::max(0.0, rec.opacity));
                layer.blendMode = rec.blendMode;
                layer.clipping = rec.clipping;
                layer.adjustment = std::move(adj);
                layers.push_back(std::move(layer));
                done++;
                continue;
            }
            continue;  // 其余调整类（hue2/blnc/...）仍跳过（note 已带）
        }
        Layer layer;
        layer.id = engine.nextLayerId();
        layer.name = rec.name;
        layer.visible = rec.visible;
        layer.opacity = std::min(1.0, std::max(0.0, rec.opacity));
        layer.blendMode = rec.blendMode;
        layer.clipping = rec.clipping;
        layer.transform.originX = rec.left;
        layer.transform.originY = rec.top;
        layer.transform.width = rec.width;
        layer.transform.height = rec.height;

        if (!rec.isGroup && rec.width > 0 && rec.height > 0 && !rec.rgba.empty()) {
            // M5b-2：剪贴非破坏——保留标志渲染期生效；栈底剪贴（无基）给提示
            if (rec.clipping && layers.empty()) {
                rec.notes.push_back("This clipping mask has no base below it, so clipping was skipped.");
            }
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
        }
        // 空像素剪贴层：无内容不渲染，无需提示（M5b-2 剪贴已非破坏）

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
