#include "io/psd/psd_import.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <unordered_map>
#include <unistd.h>

#include <hilog/log.h>

#include "engine/engine.h"
#include "engine/style_ops.h"
#include "engine/smart_ops.h"
#include "io/psd/psd_reader.h"

namespace montage {
namespace io {
namespace {

constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.PSD";

void notifyProgress(double progress) {
    auto& e = Engine::get();
    {
        // 锁内完成读指针+调用：与 Dispose/OnImportProgress 的 import.mtx 置空/release 互斥
        //（同 image_import.cpp 的 notifyProgress，审查修复）
        std::lock_guard<std::mutex> lk(e.import.mtx);
        e.import.progress = progress;
        if (e.progressTsfn != nullptr) {
            napi_call_threadsafe_function(
                e.progressTsfn,
                reinterpret_cast<void*>(static_cast<intptr_t>(progress * 1000.0 + 0.5)),
                napi_tsfn_nonblocking);
        }
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

bool assemblePsdLayers(psd::PsdDocument& doc, const std::function<uint64_t()>& nextId,
                       const std::function<void(double)>& progress, std::vector<Layer>& layers,
                       std::vector<std::string>* notes, std::string& err) {
    // 烘焙：蒙版 → 剪贴 → 直通 alpha；逐层切瓦片（M5b-2 起蒙版/剪贴均非破坏）
    layers.reserve(doc.layers.size());
    // M14a 真编组 + 审查修复：reader 文件序中子层/内层组记录先于其父组记录出现（组头在
    // 收尾 divider 才产出），单趟解析时 groupIdMap 未填充 → parentId 恒 0（层级全扁平），
    // 且组节点落在子段之后破坏「组节点紧跟完整子树」先序不变式（渲染/连续性检查均按此
    // 约定）。改两遍：先扫全部组记录预分配 组号→引擎 id，再按组树 DFS 前序组装。
    std::unordered_map<int, uint64_t> groupIdMap;  // M14a：组装期组号 → 引擎 LayerId
    for (const psd::Record& rec : doc.layers) {
        if (rec.isGroup && rec.groupId != 0) {
            groupIdMap[rec.groupId] = nextId();
        }
    }
    std::unordered_map<int, std::vector<size_t>> childrenOf;  // 父组号 → 记录下标（记录序）
    for (size_t ri = 0; ri < doc.layers.size(); ++ri) {
        childrenOf[doc.layers[ri].parentGroup].push_back(ri);
    }
    int done = 0;
    // 逐记录建层（parentId 由 DFS 父链给定）；返回是否产出引擎层（未映射调整类跳过）
    auto createRecord = [&](psd::Record& rec, uint64_t parent) -> bool {
        done++;
        if (progress != nullptr) {
            progress(0.15 + 0.8 * static_cast<double>(done) /
                               static_cast<double>(std::max<size_t>(doc.layers.size(), 1)));
        }
        if (rec.isGroup) {
            Layer g;
            auto git = rec.groupId != 0 ? groupIdMap.find(rec.groupId) : groupIdMap.end();
            g.id = git != groupIdMap.end() ? git->second : nextId();
            g.name = rec.name;
            g.visible = rec.visible;
            g.opacity = std::min(1.0, std::max(0.0, rec.opacity));
            g.blendMode = rec.blendMode;
            g.isGroup = true;
            g.parentId = parent;
            layers.push_back(std::move(g));
            done++;
            return true;
        }
        if (rec.kind == psd::LayerKind::SmartObject && !rec.placedRaw.empty()) {
            // M21：SoLd/PlLd + lnk2 内嵌源 → 非破坏智能对象层（源不可得时落回栅格化）
            psd::PlacedSmart placed;
            if (psd::parsePlaced(rec.placedKey, rec.placedRaw.data(), rec.placedRaw.size(),
                                 placed)) {
                const psd::LinkedFile* lf = nullptr;
                for (const psd::LinkedFile& f : doc.linkedFiles) {
                    if (!placed.idnt.empty() && f.uuid == placed.idnt) {
                        lf = &f;
                        break;
                    }
                }
                if (lf != nullptr && !lf->bytes.empty()) {
                    auto sm = std::make_shared<SmartLayerState>();
                    sm->contentsId = nextId();
                    sm->embedded = true;
                    sm->fileName = lf->fileName;
                    sm->sourceBytes = lf->bytes;
                    for (int i = 0; i < 6; ++i) {
                        sm->m[i] = placed.m[i];
                    }
                    sm->srcW = placed.srcW;
                    sm->srcH = placed.srcH;
                    sm->filters = placed.filters;
                    sm->filtersEnabled = placed.stackEnabled;
                    sm->psdRaw = rec.placedRaw;
                    sm->placedId = placed.placedId;
                    sm->uuid = placed.idnt;
                    sm->sourceHash = 0;
                    sm->psdImportHash = smart::fingerprint(*sm);
                    Layer layer;
                    layer.id = nextId();
                    layer.name = rec.name;
                    layer.visible = rec.visible;
                    layer.opacity = std::min(1.0, std::max(0.0, rec.opacity));
                    layer.blendMode = rec.blendMode;
                    layer.clipping = rec.clipping;
                    layer.parentId = parent;
                    layer.transform.width = placed.srcW;
                    layer.transform.height = placed.srcH;
                    layer.smart = std::move(sm);
                    // 蒙版像素在 FEid 缓存（v1 不读）→ 无滤镜蒙版 = 滤镜全域生效（PS
                    // filterMaskExtendWithWhite 语义）+ 提示
                    if (!rec.notes.empty()) {
                        std::vector<std::string> kept;
                        for (const std::string& n : rec.notes) {
                            if (n != "The smart object was rasterized.") {
                                kept.push_back(n);
                            }
                        }
                        rec.notes = std::move(kept);
                    }
                    if (placed.hasStack && placed.maskEnabled && !placed.filters.empty()) {
                        rec.notes.push_back(
                            "The smart filter mask was flattened to the filtered result "
                            "(FEid cache is not imported).");
                    }
                    for (const std::string& n : rec.notes) {
                        if (notes != nullptr) {
                            notes->push_back(rec.name + ": " + n);
                        }
                    }
                    layers.push_back(std::move(layer));
                    done++;
                    return true;
                }
            }
        }
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
                layer.id = nextId();
                layer.name = rec.name;
                layer.visible = rec.visible;
                layer.opacity = std::min(1.0, std::max(0.0, rec.opacity));
                layer.blendMode = rec.blendMode;
                layer.clipping = rec.clipping;
                layer.parentId = parent;  // 组内调整层须留在组子树内（平铺不变式）
                layer.adjustment = std::move(adj);
                layers.push_back(std::move(layer));
                done++;
                return true;
            }
            return false;  // 其余调整类（hue2/blnc/...）仍跳过（note 已带）
        }
        Layer layer;
        layer.id = nextId();
        layer.name = rec.name;
        layer.visible = rec.visible;
        layer.opacity = std::min(1.0, std::max(0.0, rec.opacity));
        layer.blendMode = rec.blendMode;
        layer.clipping = rec.clipping;
        layer.parentId = parent;
        layer.transform.originX = rec.left;
        layer.vectorMask = rec.vectorMask;  // M18d：vmsk 矢量蒙版非破坏导入
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
        // M17c：lrFX 效果 → LayerStyle（五类；lfx2 专属字段已在 reader 降级 notes）
        if (rec.hasStyle && !rec.style.empty()) {
            auto st = std::make_shared<LayerStyle>();
            for (const psd::Record::PsdStyleEffect& e : rec.style) {
                if (e.key == "dsdw") {
                    st->dropShadow = true;
                    st->shadowAngleDeg = e.angle;
                    st->shadowDistance = e.distance;
                    st->shadowSize = e.size;
                    st->shadowOpacity = e.opacity;
                    st->shadowColor[0] = e.color[0];
                    st->shadowColor[1] = e.color[1];
                    st->shadowColor[2] = e.color[2];
                    st->shadowColor[3] = 1.0f;
                    st->shadowUseGL = e.useGlobal;
                } else if (e.key == "isdw") {
                    st->innerShadow = true;
                    st->isAngleDeg = e.angle;
                    st->isDistance = e.distance;
                    st->isSize = e.size;
                    st->isOpacity = e.opacity;
                    st->isColor[0] = e.color[0];
                    st->isColor[1] = e.color[1];
                    st->isColor[2] = e.color[2];
                    st->isColor[3] = 1.0f;
                    st->isUseGL = e.useGlobal;
                } else if (e.key == "oglw") {
                    st->outerGlow = true;
                    st->ogSize = e.size;
                    st->ogOpacity = e.opacity;
                    st->ogColor[0] = e.color[0];
                    st->ogColor[1] = e.color[1];
                    st->ogColor[2] = e.color[2];
                    st->ogColor[3] = 1.0f;
                } else if (e.key == "iglw") {
                    st->innerGlow = true;
                    st->igSize = e.size;
                    st->igOpacity = e.opacity;
                    st->igColor[0] = e.color[0];
                    st->igColor[1] = e.color[1];
                    st->igColor[2] = e.color[2];
                    st->igColor[3] = 1.0f;
                } else if (e.key == "sofi") {
                    st->colorOverlay = true;
                    st->overlayOpacity = e.opacity;
                    st->overlayColor[0] = e.color[0];
                    st->overlayColor[1] = e.color[1];
                    st->overlayColor[2] = e.color[2];
                    st->overlayColor[3] = 1.0f;
                }
            }
            layer.style = st;
        }
        // 空像素剪贴层：无内容不渲染，无需提示（M5b-2 剪贴已非破坏）

        for (const std::string& n : rec.notes) {
            if (notes != nullptr) {
                notes->push_back(rec.name + ": " + n);
            }
        }
        layers.push_back(std::move(layer));
        return true;
    };

    // DFS 前序组装：组节点先于其子段（根序 = 记录序 = PSD 自底向上）。
    // 深度护栏：对抗文件的深嵌套不下钻，超限组子段拍平为根并登记 note。
    constexpr int kMaxGroupDepth = 64;
    bool depthClipped = false;
    std::function<void(int, int)> emitChildren = [&](int parentKey, int depth) {
        auto cit = childrenOf.find(parentKey);
        if (cit == childrenOf.end()) {
            return;
        }
        for (size_t ri : cit->second) {
            psd::Record& rec = doc.layers[ri];
            const uint64_t parent =
                parentKey != 0 && groupIdMap.count(parentKey) != 0 ? groupIdMap[parentKey] : 0;
            if (!createRecord(rec, parent)) {
                continue;
            }
            if (rec.isGroup && rec.groupId != 0) {
                if (depth < kMaxGroupDepth) {
                    emitChildren(rec.groupId, depth + 1);
                } else if (!depthClipped) {
                    depthClipped = true;
                    if (notes != nullptr) {
                        notes->push_back("Groups nested deeper than 64 levels were flattened.");
                    }
                    auto sub = childrenOf.find(rec.groupId);
                    if (sub != childrenOf.end()) {
                        for (size_t ci : sub->second) {
                            createRecord(doc.layers[ci], 0);
                        }
                    }
                }
            }
        }
    };
    emitChildren(0, 0);
    return true;
}

bool importPsd(int fd, int* outWidth, int* outHeight, std::vector<std::string>* notes,
               std::string& err) {
    // 全量读入（PSD 解析需随机访问；典型工程文件数十 MB）；512MB 上限（审查修复：
    // 原先仅查空文件即按 sz 全量分配，对抗文件可打到 OOM）
    constexpr off_t kMaxPsdBytes = 512ll * 1024 * 1024;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) {
        close(fd);
        err = "empty file";
        return false;
    }
    if (sz > kMaxPsdBytes) {
        close(fd);
        err = "psd file too large";
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

    std::vector<Layer> layers;
    if (!assemblePsdLayers(doc, [&engine]() { return engine.nextLayerId(); },
                           [](double p) { notifyProgress(p); }, layers, notes, err)) {
        return false;
    }

    // 整档替换：统一走 resetDocument（清撤销/会话态，撤销不跨文档；未持锁调用）
    engine.resetDocument();
    {
        std::lock_guard<std::mutex> lk(engine.docMutex);
        engine.doc.width = doc.width;
        engine.doc.height = doc.height;
        engine.doc.name = "PSD";
        engine.doc.layers = std::move(layers);
        engine.doc.activeId = engine.doc.layers.empty()
                                  ? 0
                                  : engine.doc.layers[engine.doc.layers.size() - 1].id;
        // M17c：带 lrFX 样式的层重建派生网格（openProject 同款收尾）
        // M18d：PSD 路径资源 → 路径面板（保存路径 + 工作路径；不入 History）
        engine.doc.paths = doc.paths;
        engine.doc.workPath = doc.workPath;
        restyleDocument(engine.doc);
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
