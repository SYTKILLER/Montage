#include "io/image_import.h"

#include <cmath>
#include <cstring>
#include <thread>
#include <unistd.h>

#include <hilog/log.h>
#include <multimedia/image_framework/image/image_common.h>
#include <multimedia/image_framework/image/image_source_native.h>
#include <multimedia/image_framework/image/pixelmap_native.h>

#include "engine/engine.h"

namespace montage {
namespace io {
namespace {

constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Import";
constexpr uint32_t kBandTileRows = 8;  // 02 §6.1：带 = 8×tile 行
constexpr uint32_t kMaxSide = 16384;   // 02 §7 Limits
constexpr uint64_t kMaxPixels = 100ull * 1000 * 1000;

// R10：RAII Guard——任何异常/早退路径保证 release（PixelMap/ImageSource/fd）。
class PixelMapGuard {
  public:
    explicit PixelMapGuard(OH_PixelmapNative* pm) : pm_(pm) {}
    ~PixelMapGuard() {
        if (pm_ != nullptr) {
            OH_PixelmapNative_Release(pm_);
        }
    }
    PixelMapGuard(const PixelMapGuard&) = delete;
    PixelMapGuard& operator=(const PixelMapGuard&) = delete;
    OH_PixelmapNative* get() const { return pm_; }

  private:
    OH_PixelmapNative* pm_ = nullptr;
};

class SourceGuard {
  public:
    explicit SourceGuard(OH_ImageSourceNative* s) : s_(s) {}
    ~SourceGuard() {
        if (s_ != nullptr) {
            OH_ImageSourceNative_Release(s_);
        }
    }
    SourceGuard(const SourceGuard&) = delete;
    SourceGuard& operator=(const SourceGuard&) = delete;
    OH_ImageSourceNative* get() const { return s_; }

  private:
    OH_ImageSourceNative* s_ = nullptr;
};

class FdGuard {
  public:
    explicit FdGuard(int fd) : fd_(fd) {}
    ~FdGuard() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    int get() const { return fd_; }

  private:
    int fd_;
};

// 完成回调 payload（堆分配，TSFN 送 JS 线程消费后由消费方 delete）
struct ImportCompletion {
    bool ok = false;
    uint32_t width = 0;
    uint32_t height = 0;
    LayerId layerId = 0;
    char error[192] = {0};
};

ImportCompletion* makePayload(bool ok, uint32_t w, uint32_t h, LayerId layerId, const std::string& err) {
    auto* p = new ImportCompletion();
    p->ok = ok;
    p->width = w;
    p->height = h;
    p->layerId = layerId;
    std::strncpy(p->error, err.c_str(), sizeof(p->error) - 1);
    return p;
}

void notifyProgress(double progress) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.import.mtx);
        e.import.progress = progress;
    }
    if (e.progressTsfn != nullptr) {
        // permille 走 intptr，免堆分配
        napi_call_threadsafe_function(
            e.progressTsfn,
            reinterpret_cast<void*>(static_cast<intptr_t>(progress * 1000.0 + 0.5)),
            napi_tsfn_nonblocking);
    }
}

bool importCancelled() {
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.import.mtx);
    return e.import.cancel;
}

// 解出一个带并切瓦片写入 builder；失败时 err 带原因。
// actualH 返回解码实际高度——PNG 等格式 desiredRegion 可能不生效（回退整图，M1 实证），
// 调用方据此跳过后续带，避免重复整图解码。
bool decodeBand(OH_ImageSourceNative* source, uint32_t imgW, uint32_t imgY, uint32_t imgH,
                TileGridBuilder& builder, uint32_t* actualH, std::string& err) {
    OH_DecodingOptions* opts = nullptr;
    if (OH_DecodingOptions_Create(&opts) != IMAGE_SUCCESS || opts == nullptr) {
        err = "DecodingOptions create failed";
        return false;
    }
    // R8：直出请求 BGRA_8888（M1 实证：native 解码器实际输出与请求相反，见 swapRB 校准注释）；
    // R7：分带区域解码（PNG 实证可能整图回退，actualH 兜底）
    OH_DecodingOptions_SetPixelFormat(opts, 4 /*PIXEL_FORMAT_BGRA_8888*/);
    Image_Region region{};
    region.x = 0;
    region.y = imgY;
    region.width = imgW;
    region.height = imgH;
    OH_DecodingOptions_SetDesiredRegion(opts, &region);

    OH_PixelmapNative* rawPm = nullptr;
    const Image_ErrorCode rc = OH_ImageSourceNative_CreatePixelmap(source, opts, &rawPm);
    OH_DecodingOptions_Release(opts);
    if (rc != IMAGE_SUCCESS || rawPm == nullptr) {
        err = "band decode rc=" + std::to_string(rc);
        return false;
    }
    PixelMapGuard pm(rawPm);

    // 格式核验（R8 兜底）+ 行带 stride 读取
    OH_Pixelmap_ImageInfo* info = nullptr;
    OH_PixelmapImageInfo_Create(&info);
    if (info == nullptr) {
        err = "ImageInfo create failed";
        return false;
    }
    OH_PixelmapNative_GetImageInfo(pm.get(), info);
    uint32_t bandW = 0;
    uint32_t bandH = 0;
    uint32_t rowStride = 0;
    int32_t pixelFormat = 0;
    OH_PixelmapImageInfo_GetWidth(info, &bandW);
    OH_PixelmapImageInfo_GetHeight(info, &bandH);
    OH_PixelmapImageInfo_GetRowStride(info, &rowStride);
    OH_PixelmapImageInfo_GetPixelFormat(info, &pixelFormat);
    OH_PixelmapImageInfo_Release(info);
    if (bandW == 0 || bandH == 0) {
        err = "band empty";
        return false;
    }
    if (rowStride == 0) {
        rowStride = bandW * 4u;
    }
    *actualH = bandH;
    // M1 两轮实证：此解码路径的 fmt 报告与实际输出相反（报 RGBA 实出 BGRA，反之亦然）；
    // 请求 BGRA(4) → 实出 RGBA → 不换；报 3 → 实出 BGRA → 换 R/B。JPEG 待复验。
    const bool swapRB = (pixelFormat == 3);

    void* addr = nullptr;
    if (OH_PixelmapNative_AccessPixels(pm.get(), &addr) != IMAGE_SUCCESS || addr == nullptr) {
        err = "band access failed";
        return false;
    }
    const uint8_t* src = static_cast<const uint8_t*>(addr);

    // 切瓦片：带覆盖行带 [imgY, imgY+bandH)；瓦片整片分配、按行拷贝，越界区保持透明
    const uint32_t ty0 = imgY / kTileSize;
    const uint32_t ty1 = (imgY + bandH - 1u) / kTileSize;
    for (uint32_t ty = ty0; ty <= ty1; ++ty) {
        const uint32_t tileY0 = ty * kTileSize;
        const uint32_t copyH = std::min(kTileSize, imgY + bandH - tileY0);
        for (uint32_t tx = 0; tx < builder.cols(); ++tx) {
            const uint32_t tileX0 = tx * kTileSize;
            const uint32_t copyW = std::min(kTileSize, imgW - tileX0);
            if (copyW == 0 || copyH == 0) {
                continue;
            }
            EngineBuffer& buf = builder.ensureTile(tx, ty);
            uint8_t* dstBase = buf.mapCpuWrite();
            const uint32_t inTileY = tileY0 - imgY;  // 带内起始行
            for (uint32_t r = 0; r < copyH; ++r) {
                const uint8_t* srcRow =
                    src + static_cast<size_t>(rowStride) * (inTileY + r) + static_cast<size_t>(tileX0) * 4u;
                uint8_t* dstRow = dstBase + static_cast<size_t>(buf.rowBytes()) * r;
                if (!swapRB) {
                    std::memcpy(dstRow, srcRow, static_cast<size_t>(copyW) * 4u);
                } else {
                    for (uint32_t px = 0; px < copyW; ++px) {
                        dstRow[px * 4u + 0u] = srcRow[px * 4u + 2u];
                        dstRow[px * 4u + 1u] = srcRow[px * 4u + 1u];
                        dstRow[px * 4u + 2u] = srcRow[px * 4u + 0u];
                        dstRow[px * 4u + 3u] = srcRow[px * 4u + 3u];
                    }
                }
            }
        }
    }
    return true;
}

// 导入主体（fd 归属本函数）。导入 = 追加图层：无文档时以图片尺寸建文档；有文档时
// 新图层居中放置、置于栈顶并激活。返回 payload（堆）。
ImportCompletion* runImport(int fd) {
    FdGuard fdGuard(fd);
    auto& engine = Engine::get();

    OH_ImageSourceNative* rawSource = nullptr;
    if (OH_ImageSourceNative_CreateFromFd(fd, &rawSource) != IMAGE_SUCCESS || rawSource == nullptr) {
        return makePayload(false, 0, 0, 0, "CreateFromFd failed");
    }
    SourceGuard source(rawSource);

    uint32_t imgW = 0;
    uint32_t imgH = 0;
    {
        OH_ImageSource_Info* info = nullptr;
        OH_ImageSourceInfo_Create(&info);
        if (info == nullptr) {
            return makePayload(false, 0, 0, 0, "ImageSourceInfo create failed");
        }
        const Image_ErrorCode rc = OH_ImageSourceNative_GetImageInfo(source.get(), 0, info);
        if (rc == IMAGE_SUCCESS) {
            OH_ImageSourceInfo_GetWidth(info, &imgW);
            OH_ImageSourceInfo_GetHeight(info, &imgH);
        }
        OH_ImageSourceInfo_Release(info);
        if (rc != IMAGE_SUCCESS) {
            return makePayload(false, 0, 0, 0, "GetImageInfo rc=" + std::to_string(rc));
        }
    }
    if (imgW == 0 || imgH == 0 || imgW > kMaxSide || imgH > kMaxSide ||
        static_cast<uint64_t>(imgW) * imgH > kMaxPixels) {
        return makePayload(false, imgW, imgH, 0, "image size out of limits(16384/100MP)");
    }

    // 图层落位：无文档 → 建文档（尺寸=图片）；有文档 → 居中放置
    const LayerId layerId = engine.nextLayerId();
    bool createsDoc = false;
    Transform transform;
    {
        std::lock_guard<std::mutex> lk(engine.docMutex);
        createsDoc = (engine.doc.width == 0 || engine.doc.height == 0);
        if (createsDoc) {
            transform.originX = 0.0;
            transform.originY = 0.0;
        } else {
            transform.originX = std::max(0.0, (static_cast<double>(engine.doc.width) - imgW) / 2.0);
            transform.originY = std::max(0.0, (static_cast<double>(engine.doc.height) - imgH) / 2.0);
        }
        transform.width = imgW;
        transform.height = imgH;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                 "import start %{public}ux%{public}u layer=%{public}llu createsDoc=%{public}d",
                 imgW, imgH, static_cast<unsigned long long>(layerId), createsDoc ? 1 : 0);

    const uint32_t cols = (imgW + kTileSize - 1u) / kTileSize;
    const uint32_t rows = (imgH + kTileSize - 1u) / kTileSize;
    TileGridBuilder builder(cols, rows);
    const uint32_t bandH = kBandTileRows * kTileSize;
    const uint32_t bands = (imgH + bandH - 1u) / bandH;
    uint64_t gridRevision = 0;
    bool fullDecoded = false;
    bool layerAppended = false;

    for (uint32_t band = 0; band < bands && !fullDecoded; ++band) {
        if (importCancelled()) {
            OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "import cancelled at band %{public}u", band);
            return makePayload(false, imgW, imgH, layerId, "cancelled");
        }
        const uint32_t y0 = band * bandH;
        const uint32_t h = std::min(bandH, imgH - y0);
        std::string err;
        uint32_t actualH = 0;
        if (!decodeBand(source.get(), imgW, y0, h, builder, &actualH, err)) {
            return makePayload(false, imgW, imgH, layerId, err);
        }
        if (band == 0 && actualH >= imgH) {
            // region 解码不生效（整图回退，M1 实证 PNG 如此）：瓦片已全部就位，发布后结束
            OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag,
                         "region decode unsupported (got full %{public}u rows), single-pass import", actualH);
            fullDecoded = true;
        }
        // 带边界发布：图层入栈（首个带）+ 图层瓦片快照更新 + 渐进渲染（02 §6.1 / R4）
        {
            auto grid = builder.publish(++gridRevision);
            std::lock_guard<std::mutex> lk(engine.docMutex);
            if (createsDoc) {
                engine.doc.width = imgW;
                engine.doc.height = imgH;
                engine.doc.name = "Untitled";
            }
            if (!layerAppended) {
                Layer layer;
                layer.id = layerId;
                layer.name = "图片 " + std::to_string(layerId);
                layer.transform = transform;
                layer.pixels = grid;
                engine.doc.layers.push_back(std::move(layer));
                engine.doc.activeId = layerId;
                layerAppended = true;
            } else {
                for (Layer& l : engine.doc.layers) {
                    if (l.id == layerId) {
                        l.pixels = grid;
                        break;
                    }
                }
            }
            engine.requestRender();
        }
        notifyProgress(static_cast<double>(band + 1) / static_cast<double>(bands));
    }

    {
        std::lock_guard<std::mutex> lk(engine.docMutex);
        engine.bumpRevisionLocked();
    }
    {
        std::lock_guard<std::mutex> lk(engine.import.mtx);
        engine.import.running = false;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                 "import done: grid %{public}ux%{public}u tiles layer=%{public}llu",
                 cols, rows, static_cast<unsigned long long>(layerId));
    return makePayload(true, imgW, imgH, layerId, "");
}

void importThread(int fd, napi_threadsafe_function completionTsfn) {
    ImportCompletion* payload = runImport(fd);
    auto& engine = Engine::get();
    {
        std::lock_guard<std::mutex> lk(engine.import.mtx);
        engine.import.running = false;
    }
    if (completionTsfn != nullptr) {
        napi_call_threadsafe_function(completionTsfn, payload, napi_tsfn_nonblocking);
    } else {
        delete payload;
    }
}

}  // namespace

bool startImageImport(int fd, napi_threadsafe_function completionTsfn) {
    auto& engine = Engine::get();
    {
        std::lock_guard<std::mutex> lk(engine.import.mtx);
        if (engine.import.running) {
            close(fd);  // fd 所有权一律接管：忙时直接关闭
            return false;
        }
        engine.import.cancel = false;
        engine.import.progress = 0.0;
        engine.import.running = true;
    }
    std::thread(&importThread, fd, completionTsfn).detach();
    return true;
}

void cancelImageImport() {
    auto& engine = Engine::get();
    std::lock_guard<std::mutex> lk(engine.import.mtx);
    engine.import.cancel = true;
}

}  // namespace io
}  // namespace montage
