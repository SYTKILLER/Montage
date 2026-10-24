#include "io/image_import.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>

#include <hilog/log.h>
#include <multimedia/image_framework/image/image_common.h>
#include <multimedia/image_framework/image/image_source_native.h>
#include <multimedia/image_framework/image/pixelmap_native.h>

#include "engine/engine.h"
#include "io/png_decode.h"
#include "io/dng_decode.h"
#include "io/codecs/codec_import.h"
#include "io/codecs/codec_table.h"
#include "io/codecs/tiff_codec.h"

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
        // 锁内完成读指针+调用：与 Dispose/OnImportProgress 的 import.mtx 置空/release 互斥，
        // 杜绝「读到旧指针 → 对方 release 后才调用」的 use-after-release（审查修复）
        std::lock_guard<std::mutex> lk(e.import.mtx);
        e.import.progress = progress;
        if (e.progressTsfn != nullptr) {
            // permille 走 intptr，免堆分配
            napi_call_threadsafe_function(
                e.progressTsfn,
                reinterpret_cast<void*>(static_cast<intptr_t>(progress * 1000.0 + 0.5)),
                napi_tsfn_nonblocking);
        }
    }
}

bool importCancelled() {
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.import.mtx);
    return e.import.cancel;
}

// 解出一个带并切瓦片写入 builder；失败时 err 带原因。
// actualH 返回解码实际高度——PNG/JPEG 等格式 desiredRegion 可能不生效（回退整图，M1/M16a 实证），
// 调用方据此跳过后续带，避免重复整图解码。
// forceSwapRB：mimeType 驱动的换序覆盖（M16a 实证 JPEG 请求 BGRA 如实输出 BGRA——报告=请求=实际，
// M1 的「报 3 才换」启发式在此漏换致红蓝对调；引擎瓦片约定 RGBA 字节序，BGRA 实出必须换）。
bool decodeBand(OH_ImageSourceNative* source, uint32_t imgW, uint32_t imgY, uint32_t imgH,
                TileGridBuilder& builder, uint32_t* actualH, std::string& err,
                bool forceSwapRB = false) {
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
    // M1 两轮实证（PNG，现为自研解码器前的旧径）：此解码路径的 fmt 报告与实际输出相反；
    // M16a JPEG 校准：JPEG 报告诚实（请求 BGRA=报告=实际输出 BGRA），按 M1 规则漏换致红蓝对调，
    // 故 mimeType=image/jpeg 由调用方传 forceSwapRB=true；其余格式维持 fmt 启发式（待逐一校准）。
    const bool swapRB = forceSwapRB || (pixelFormat == 3);
    // M4b 实证（.montage 保存对拍）：解码带为预乘 alpha（引擎约定直 alpha，对齐 PSD 路径
    // unpremultiply），DecodingOptions 无 alphaType 请求项（SDK 核验），只能检测后手动还原
    int32_t alphaType = 0;
    OH_PixelmapImageInfo_GetAlphaType(info, &alphaType);
    // 实证（M4b 对拍）：模拟器 PNG 解码带为预乘 alpha 且报告 UNKNOWN(0)；DecodingOptions 无
    // alphaType 请求项（SDK 核验）。引擎瓦片约定直 alpha（对齐 PSD unpremultiply 路径）：
    // 显式报 UNPREMULTIPLIED(3) 才信任报告跳过，其余（UNKNOWN/PREMULTIPLIED）按预乘还原；
    // 无 alpha 格式 a≡255，还原为恒等
    const bool unpremult = (alphaType != 3 /*PIXELMAP_ALPHA_TYPE_UNPREMULTIPLIED*/);
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "band decode: fmt=%{public}d alphaType=%{public}d swapRB=%{public}d unpremult=%{public}d",
                 pixelFormat, alphaType, swapRB ? 1 : 0, unpremult ? 1 : 0);

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
                if (!swapRB && !unpremult) {
                    std::memcpy(dstRow, srcRow, static_cast<size_t>(copyW) * 4u);
                } else {
                    for (uint32_t px = 0; px < copyW; ++px) {
                        uint8_t r8 = srcRow[px * 4u + 0u];
                        uint8_t g8 = srcRow[px * 4u + 1u];
                        uint8_t b8 = srcRow[px * 4u + 2u];
                        uint8_t a8 = srcRow[px * 4u + 3u];
                        if (unpremult && a8 > 0 && a8 < 255) {
                            // 预乘还原（四舍五入），引擎瓦片一律直 alpha
                            const uint32_t a32 = a8;
                            r8 = static_cast<uint8_t>(std::min(255u, (r8 * 255u + a32 / 2u) / a32));
                            g8 = static_cast<uint8_t>(std::min(255u, (g8 * 255u + a32 / 2u) / a32));
                            b8 = static_cast<uint8_t>(std::min(255u, (b8 * 255u + a32 / 2u) / a32));
                        }
                        dstRow[px * 4u + 0u] = swapRB ? b8 : r8;
                        dstRow[px * 4u + 1u] = g8;
                        dstRow[px * 4u + 2u] = swapRB ? r8 : b8;
                        dstRow[px * 4u + 3u] = a8;
                    }
                }
            }
        }
    }
    return true;
}

// 导入主体（fd 归属本函数）。导入 = 追加图层：无文档时以图片尺寸建文档；有文档时
// 新图层居中放置、置于栈顶并激活。返回 payload（堆）。
ImportCompletion* runImport(int fd, const std::string& fileName) {
    FdGuard fdGuard(fd);
    auto& engine = Engine::get();

    // 全量读入一次（DNG/M25 嗅探共用；512MB 上限）
    std::vector<uint8_t> fileBytes;
    {
        lseek(fd, 0, SEEK_SET);
        const off_t sz = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        if (sz > 0 && sz <= 512ll * 1024 * 1024) {
            fileBytes.resize(static_cast<size_t>(sz));
            size_t got = 0;
            while (got < fileBytes.size()) {
                const ssize_t n = read(fd, fileBytes.data() + got, fileBytes.size() - got);
                if (n <= 0) {
                    break;
                }
                got += static_cast<size_t>(n);
            }
            if (got != fileBytes.size()) {
                fileBytes.clear();
            }
        }
        lseek(fd, 0, SEEK_SET);
    }

    // M15d：DNG RAW 显影链（内容探测在系统解码器之前——ImageSource 不识别 RAW）
    if (!fileBytes.empty() && looksLikeDng(fileBytes)) {
        {
            const std::vector<uint8_t>& bytes = fileBytes;
            {
                std::shared_ptr<const TileGrid> grid;
                uint32_t w = 0, h = 0;
                std::string derr;
                if (developDngToGrid(bytes, grid, w, h, derr)) {
                    // 以显影结果为内容建层（复用引擎导入收尾语义：追加为普通像素层）
                    Layer layer;
                    layer.id = engine.nextLayerId();
                    layer.name = "RAW";
                    layer.transform.width = w;
                    layer.transform.height = h;
                    layer.pixels = grid;
                    {
                        std::lock_guard<std::mutex> lk(engine.docMutex);
                        if (engine.doc.width == 0) {
                            engine.doc.width = w;
                            engine.doc.height = h;
                        }
                        if (engine.doc.layers.empty()) {
                            engine.doc.layers.push_back(std::move(layer));
                            engine.doc.activeId = engine.doc.layers.back().id;
                        } else {
                            const LayerId nid = layer.id;
                            engine.doc.layers.push_back(std::move(layer));
                            engine.doc.activeId = nid;
                        }
                        // M15e：会话 RAW 缓存（显影参数面板数据源；resetDocument 随文档清）
                        engine.rawDevelop.bytes = bytes;
                        engine.rawDevelop.layerId = engine.doc.activeId;
                        engine.rawDevelop.w = w;
                        engine.rawDevelop.h = h;
                        engine.rawDevelop.last = io::DevelopParams{};
                        engine.rawDevelop.valid = true;
                        engine.bumpRevisionLocked();
                    }
                    engine.requestRender();
                    OH_LOG_Print(LOG_APP, LOG_INFO, 0x4D30, "Montage.Dng",
                                 "imported developed dng %ux%u", w, h);
                    return makePayload(true, w, h, engine.doc.activeId, "");
                }
                OH_LOG_Print(LOG_APP, LOG_WARN, 0x4D30, "Montage.Dng",
                             "dng develop failed: %{public}s", derr.c_str());
                return makePayload(false, 0, 0, 0, derr);
            }
        }
        lseek(fd, 0, SEEK_SET);
    }

    // M25：平面格式内容探测分发（TIFF/BigTIFF/EXR/HDR/GIF/QOI/BMP + TGA footer/扩展名）。
    // 命中即整图解码为单层追加（分层 TIFF 的图层语义走 openTiffFile 文档命令；此路径
    // 解过分层也只取合成图，保证「导入=追加图层」语义恒定）。
    std::shared_ptr<const TileGrid> codecGrid;
    if (!fileBytes.empty()) {
        std::string fmt;
        if (codecs::sniff(fileBytes.data(), fileBytes.size(), fileName, fmt)) {
            std::vector<std::string> layerNotes;
            if (fmt == "tiff") {
                uint32_t lw = 0;
                uint32_t lh = 0;
                std::vector<codecs::CodecLayerRec> lr;
                std::string lerr;
                std::vector<psd::Record> pr;
                if (codecs::tiffDecodeLayers(fileBytes.data(), fileBytes.size(), lw, lh, lr,
                                             layerNotes, lerr)) {
                    for (const std::string& n : layerNotes) {
                        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                                     "layered tiff (composite import): %{public}s", n.c_str());
                    }
                }
            }
            codecs::CodecImage img;
            std::string cerr;
            if (!codecs::decode(fileBytes.data(), fileBytes.size(), fmt, img, cerr)) {
                return makePayload(false, 0, 0, 0, fmt + " decode failed: " + cerr);
            }
            if (img.w == 0 || img.h == 0 || img.w > kMaxSide || img.h > kMaxSide ||
                static_cast<uint64_t>(img.w) * img.h > kMaxPixels) {
                return makePayload(false, img.w, img.h, 0, "codec size out of limits");
            }
            codecGrid = gridFromRgba(img.w, img.h, img.rgba);
            if (codecGrid == nullptr) {
                return makePayload(false, img.w, img.h, 0, "codec grid build failed");
            }
            OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                         "codec native decode path %{public}s %{public}ux%{public}u", fmt.c_str(),
                         img.w, img.h);
            // 短路发布：不走 ImageSource；「导入=追加图层」语义与主路径一致
            //（无文档建档、有文档居中置顶激活；分层 TIFF 只取合成图）
            const LayerId codecLayerId = engine.nextLayerId();
            Transform ct;
            {
                std::lock_guard<std::mutex> lk(engine.docMutex);
                const bool createsDoc = (engine.doc.width == 0 || engine.doc.height == 0);
                if (createsDoc) {
                    engine.doc.width = img.w;
                    engine.doc.height = img.h;
                    engine.doc.name = "Untitled";
                    ct.originX = 0.0;
                    ct.originY = 0.0;
                } else {
                    ct.originX = std::max(0.0, (static_cast<double>(engine.doc.width) - img.w) / 2.0);
                    ct.originY = std::max(0.0, (static_cast<double>(engine.doc.height) - img.h) / 2.0);
                }
                ct.width = img.w;
                ct.height = img.h;
                Layer layer;
                layer.id = codecLayerId;
                layer.name = "图片 " + std::to_string(codecLayerId);
                layer.transform = ct;
                layer.pixels = codecGrid;
                engine.doc.layers.push_back(std::move(layer));
                engine.doc.activeId = codecLayerId;
                engine.bumpRevisionLocked();
                engine.requestRender();
            }
            notifyProgress(1.0);
            OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                         "codec import done %{public}ux%{public}u layer=%{public}llu",
                         img.w, img.h, static_cast<unsigned long long>(codecLayerId));
            return makePayload(true, img.w, img.h, codecLayerId, "");
        }
    }

    OH_ImageSourceNative* rawSource = nullptr;
    if (OH_ImageSourceNative_CreateFromFd(fd, &rawSource) != IMAGE_SUCCESS || rawSource == nullptr) {
        return makePayload(false, 0, 0, 0, "CreateFromFd failed");
    }
    SourceGuard source(rawSource);

    uint32_t imgW = 0;
    uint32_t imgH = 0;
    std::string mimeType;
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
            Image_MimeType mt {nullptr, 0};
            if (OH_ImageSourceInfo_GetMimeType(info, &mt) == IMAGE_SUCCESS && mt.data != nullptr) {
                mimeType.assign(mt.data, mt.size);
            }
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

    // PNG 走自研确定性解码（直 RGBA，零预乘舍入/字节序猜谜，M4c 实证系统解码器不可信）；
    // 其余格式维持系统分带解码（swapRB 校准按 M1 实证）
    std::shared_ptr<const TileGrid> nativeGrid;
    if (mimeType == "image/png") {
        lseek(fd, 0, SEEK_SET);
        off_t sz = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        std::string err;
        // 512MB 读入上限（审查修复：仅查 sz>0 即全量分配；同文件 DNG 路径已有同款上限）
        if (sz > 0 && sz <= 512ll * 1024 * 1024) {
            std::vector<uint8_t> bytes(static_cast<size_t>(sz));
            size_t got = 0;
            while (got < bytes.size()) {
                ssize_t n = read(fd, bytes.data() + got, bytes.size() - got);
                if (n <= 0) {
                    break;
                }
                got += static_cast<size_t>(n);
            }
            if (got == bytes.size()) {
                nativeGrid = decodePngToGrid(bytes.data(), bytes.size(), err);
            }
        }
        if (nativeGrid == nullptr) {
            return makePayload(false, imgW, imgH, 0, "png decode failed: " + err);
        }
        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "png native decode path %{public}ux%{public}u", imgW, imgH);
    }

    // 图层落位：无文档 → 建文档（尺寸=图片）；有文档 → 居中放置
    // 不变式：width==0（建档）时 History 必已为空——把 doc 清到零尺寸的路径只有
    // Engine::resetDocument（新建/关闭/打开工程/PSD），其内已清撤销并作废在途笔画。
    // 此处不可调 resetDocument：会置 import.cancel 自我取消（导入线程内禁止）。
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
    // 图层入栈/瓦片快照更新（PNG native 单趟与分带路径共用）
    bool layerAppended = false;
    auto publishGrid = [&](std::shared_ptr<const TileGrid> grid) {
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
    };

    if (nativeGrid != nullptr) {
        publishGrid(nativeGrid);
        notifyProgress(1.0);
    } else {
        TileGridBuilder builder(cols, rows);
        const uint32_t bandH = kBandTileRows * kTileSize;
        const uint32_t bands = (imgH + bandH - 1u) / bandH;
        uint64_t gridRevision = 0;
        bool fullDecoded = false;

        for (uint32_t band = 0; band < bands && !fullDecoded; ++band) {
            if (importCancelled()) {
                OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "import cancelled at band %{public}u", band);
                return makePayload(false, imgW, imgH, layerId, "cancelled");
            }
            const uint32_t y0 = band * bandH;
            const uint32_t h = std::min(bandH, imgH - y0);
            std::string err;
            uint32_t actualH = 0;
            if (!decodeBand(source.get(), imgW, y0, h, builder, &actualH, err,
                            mimeType == "image/jpeg")) {
                return makePayload(false, imgW, imgH, layerId, err);
            }
            // M16a：region 生效性逐带留痕（JPEG 行为 M16 校准项；真机复跑同通道对读）
            if (band < 3) {
                OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                             "band %{public}u/%{public}u: req y=%{public}u h=%{public}u -> actual h=%{public}u region=%{public}s",
                             band + 1, bands, y0, h, actualH, actualH == h ? "honored" : "fallback");
            }
            if (band == 0 && actualH >= imgH) {
                // region 解码不生效（整图回退，M1 实证 PNG 如此）：瓦片已全部就位，发布后结束
                OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag,
                             "region decode unsupported (got full %{public}u rows), single-pass import", actualH);
                fullDecoded = true;
            }
            // 带边界发布：图层入栈（首个带）+ 图层瓦片快照更新 + 渐进渲染（02 §6.1 / R4）
            publishGrid(builder.publish(++gridRevision));
            notifyProgress(static_cast<double>(band + 1) / static_cast<double>(bands));
        }
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

void importThread(int fd, std::string fileName, napi_threadsafe_function completionTsfn) {
    ImportCompletion* payload = runImport(fd, std::move(fileName));
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

bool startImageImport(int fd, napi_threadsafe_function completionTsfn,
                      const std::string& fileName) {
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
    std::thread(&importThread, fd, fileName, completionTsfn).detach();
    return true;
}

void cancelImageImport() {
    auto& engine = Engine::get();
    std::lock_guard<std::mutex> lk(engine.import.mtx);
    engine.import.cancel = true;
}

// M21：内存栅格源解码（智能对象源；系统解码器整图单趟 + mimeType 驱动 swapRB
//（M16a 校准纪律：JPEG 报告诚实强制换序）+ 预乘还原（M4b 实证）。PNG 不走此路径
//（自研确定性解码在 smart_ops decodeSourceFlat 分发）。
bool decodeRasterSource(const std::string& fileName, const std::vector<uint8_t>& bytes,
                        uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba, std::string& err) {
    (void)fileName;
    OH_ImageSourceNative* rawSource = nullptr;
    if (OH_ImageSourceNative_CreateFromData(const_cast<uint8_t*>(bytes.data()), bytes.size(),
                                            &rawSource) != IMAGE_SUCCESS ||
        rawSource == nullptr) {
        err = "smart source: CreateFromData failed";
        return false;
    }
    SourceGuard source(rawSource);

    uint32_t imgW = 0;
    uint32_t imgH = 0;
    std::string mimeType;
    {
        OH_ImageSource_Info* info = nullptr;
        OH_ImageSourceInfo_Create(&info);
        if (info == nullptr) {
            err = "smart source: ImageSourceInfo create failed";
            return false;
        }
        const Image_ErrorCode rc = OH_ImageSourceNative_GetImageInfo(source.get(), 0, info);
        if (rc == IMAGE_SUCCESS) {
            OH_ImageSourceInfo_GetWidth(info, &imgW);
            OH_ImageSourceInfo_GetHeight(info, &imgH);
            Image_MimeType mt{nullptr, 0};
            if (OH_ImageSourceInfo_GetMimeType(info, &mt) == IMAGE_SUCCESS && mt.data != nullptr) {
                mimeType.assign(mt.data, mt.size);
            }
        }
        OH_ImageSourceInfo_Release(info);
        if (rc != IMAGE_SUCCESS) {
            err = "smart source: GetImageInfo rc=" + std::to_string(rc);
            return false;
        }
    }
    if (imgW == 0 || imgH == 0) {
        err = "smart source: empty image";
        return false;
    }

    // 整图单趟解码（BGRA 请求，M1 实证 native 解码器实出与请求同序语义按格式分治）
    OH_DecodingOptions* opts = nullptr;
    if (OH_DecodingOptions_Create(&opts) != IMAGE_SUCCESS || opts == nullptr) {
        err = "smart source: DecodingOptions create failed";
        return false;
    }
    OH_DecodingOptions_SetPixelFormat(opts, 4 /*BGRA_8888*/);
    OH_PixelmapNative* rawPm = nullptr;
    const Image_ErrorCode rc = OH_ImageSourceNative_CreatePixelmap(source.get(), opts, &rawPm);
    OH_DecodingOptions_Release(opts);
    if (rc != IMAGE_SUCCESS || rawPm == nullptr) {
        err = "smart source: pixelmap decode rc=" + std::to_string(rc);
        return false;
    }
    PixelMapGuard pm(rawPm);

    OH_Pixelmap_ImageInfo* pinfo = nullptr;
    OH_PixelmapImageInfo_Create(&pinfo);
    if (pinfo == nullptr) {
        err = "smart source: ImageInfo create failed";
        return false;
    }
    OH_PixelmapNative_GetImageInfo(pm.get(), pinfo);
    uint32_t pmW = 0;
    uint32_t pmH = 0;
    uint32_t rowStride = 0;
    int32_t pixelFormat = 0;
    int32_t alphaType = 0;
    OH_PixelmapImageInfo_GetWidth(pinfo, &pmW);
    OH_PixelmapImageInfo_GetHeight(pinfo, &pmH);
    OH_PixelmapImageInfo_GetRowStride(pinfo, &rowStride);
    OH_PixelmapImageInfo_GetPixelFormat(pinfo, &pixelFormat);
    OH_PixelmapImageInfo_GetAlphaType(pinfo, &alphaType);
    OH_PixelmapImageInfo_Release(pinfo);
    if (pmW == 0 || pmH == 0 || pmW != imgW || pmH != imgH) {
        err = "smart source: decode size mismatch";
        return false;
    }
    if (rowStride == 0) {
        rowStride = pmW * 4u;
    }
    // swapRB：mimeType 驱动（M16a：JPEG 诚实报告强制换序）；其余维持 fmt 启发式
    const bool swapRB = mimeType == "image/jpeg" || pixelFormat == 3;
    // unpremultiply：报告 UNPREMULTIPLIED(3) 才跳过（M4b 实证）
    const bool unpremult = alphaType != 3;

    void* addr = nullptr;
    if (OH_PixelmapNative_AccessPixels(pm.get(), &addr) != IMAGE_SUCCESS || addr == nullptr) {
        err = "smart source: pixel access failed";
        return false;
    }
    const uint8_t* src = static_cast<const uint8_t*>(addr);
    rgba.assign(static_cast<size_t>(pmW) * pmH * 4u, 0);
    for (uint32_t y = 0; y < pmH; ++y) {
        const uint8_t* srow = src + static_cast<size_t>(y) * rowStride;
        uint8_t* drow = rgba.data() + static_cast<size_t>(y) * pmW * 4u;
        for (uint32_t x = 0; x < pmW; ++x) {
            uint8_t px[4];
            std::memcpy(px, srow + static_cast<size_t>(x) * 4u, 4);
            if (swapRB) {
                std::swap(px[0], px[2]);
            }
            if (unpremult && px[3] != 255 && px[3] != 0) {
                const unsigned a = px[3];
                for (int c = 0; c < 3; ++c) {
                    const unsigned v = (static_cast<unsigned>(px[c]) * 255u + a / 2u) / a;
                    px[c] = static_cast<uint8_t>(v > 255u ? 255u : v);
                }
            }
            std::memcpy(drow + static_cast<size_t>(x) * 4u, px, 4);
        }
    }
    OH_PixelmapNative_UnaccessPixels(pm.get());
    w = pmW;
    h = pmH;
    return true;
}

}  // namespace io
}  // namespace montage
