#include "io/codecs/codec_import.h"

#include <cstring>
#include <unistd.h>

#include <hilog/log.h>

#include "engine/document.h"
#include "engine/engine.h"
#include "engine/style_ops.h"
#include "io/codecs/codec_table.h"
#include "io/codecs/tiff_codec.h"
#include "io/psd/psd_import.h"
#include "io/psd/psd_reader.h"

namespace montage {
namespace io {

std::shared_ptr<const TileGrid> gridFromRgba(uint32_t w, uint32_t h,
                                             const std::vector<uint8_t>& rgba) {
    if (w == 0 || h == 0 || rgba.size() < static_cast<size_t>(w) * h * 4 ||
        static_cast<uint64_t>(w) * h > 100ull * 1000 * 1000) {
        return nullptr;
    }
    TileGridBuilder builder((w + kTileSize - 1u) / kTileSize, (h + kTileSize - 1u) / kTileSize);
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t ty = y / kTileSize;
        const uint32_t inTileY = y % kTileSize;
        for (uint32_t tx = 0; tx < builder.cols(); ++tx) {
            const uint32_t x0 = tx * kTileSize;
            const uint32_t copyW = std::min(kTileSize, w - x0);
            EngineBuffer& buf = builder.ensureTile(tx, ty);
            std::memcpy(buf.mapCpuWrite() + static_cast<size_t>(buf.rowBytes()) * inTileY,
                        rgba.data() + (static_cast<size_t>(y) * w + x0) * 4u,
                        static_cast<size_t>(copyW) * 4u);
        }
    }
    return builder.publish(1);
}

bool importTiffDocument(int fd, int* outWidth, int* outHeight, std::vector<std::string>* notes,
                        std::string& err) {
    // 全量读入（512MB 上限，同 psd_import 纪律）
    constexpr off_t kMaxBytes = 512ll * 1024 * 1024;
    auto& engine = Engine::get();
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) {
        close(fd);
        err = "empty file";
        return false;
    }
    if (sz > kMaxBytes) {
        close(fd);
        err = "tiff file too large";
        return false;
    }
    lseek(fd, 0, SEEK_SET);
    std::vector<uint8_t> data(static_cast<size_t>(sz));
    size_t got = 0;
    while (got < data.size()) {
        const ssize_t n = read(fd, data.data() + got, data.size() - got);
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

    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<psd::Record> psdRecords;
    std::vector<std::string> localNotes;
    bool layered = false;
    psd::PsdDocument pdoc;
    std::vector<codecs::CodecLayerRec> recs;  // 扁平回退路径不消费，接口要求非空输出
    if (codecs::tiffDecodeLayers(data.data(), data.size(), w, h, recs, localNotes, err, &pdoc)) {
        layered = true;
    } else if (err == "no layers") {
        err.clear();  // 扁平 TIFF 走合成图
    } else {
        return false;
    }

    std::vector<Layer> layers;
    if (layered) {
        // PSD 通路组装（组嵌套/蒙版/剪贴/混合键/样式降级提示全复用）
        if (!assemblePsdLayers(pdoc, [&engine]() { return engine.nextLayerId(); },
                               nullptr, layers, &localNotes, err)) {
            return false;
        }
        // pdoc 的合成尺寸可能与主 IFD 不一致（异常文件）——以主 IFD 为准
    } else {
        codecs::CodecImage img;
        if (!codecs::decode(data.data(), data.size(), "tiff", img, err)) {
            return false;
        }
        w = img.w;
        h = img.h;
        Layer layer;
        layer.id = engine.nextLayerId();
        layer.name = "Background";
        layer.transform.width = w;
        layer.transform.height = h;
        layer.pixels = gridFromRgba(w, h, img.rgba);
        if (layer.pixels == nullptr) {
            err = "tiff: grid build failed";
            return false;
        }
        layers.push_back(std::move(layer));
    }

    // 整档替换（importPsd 同款收尾）；D2 落位：非空档新开会话（prepareOpenSlot）
    engine.prepareOpenSlot();
    engine.resetDocument();
    {
        std::lock_guard<std::mutex> lk(engine.docMutex);
        engine.doc().width = w;
        engine.doc().height = h;
        engine.doc().name = "TIFF";
        engine.doc().layers = std::move(layers);
        engine.doc().activeId = engine.doc().layers.empty()
                                  ? 0
                                  : engine.doc().layers.back().id;
        restyleDocument(engine.doc());
        engine.bumpRevisionLocked();
        engine.requestRender();
    }
    if (outWidth != nullptr) {
        *outWidth = static_cast<int>(w);
    }
    if (outHeight != nullptr) {
        *outHeight = static_cast<int>(h);
    }
    if (notes != nullptr) {
        *notes = std::move(localNotes);
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x4D30, "Montage.Codec",
                 "tiff document open %{public}ux%{public}u layered=%{public}d", w, h,
                 layered ? 1 : 0);
    return true;
}

}  // namespace io
}  // namespace montage
