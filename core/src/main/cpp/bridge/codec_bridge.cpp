#include "bridge/codec_bridge.h"

#include <unistd.h>

#include <string>
#include <vector>

#include "engine/engine.h"
#include "io/codecs/codec_table.h"
#include "io/codecs/gif_codec.h"
#include "io/codecs/tiff_codec.h"
#include "io/image_export.h"
#include "bridge/psd_snap.h"

// M25 编解码批命令面（engine_bridge.cpp 之外独立文件，最小化并行批冲突面）。
// make* 辅助在 engine_bridge.cpp 匿名命名空间（内链）——此处本地复制同实现，
// Result 形态与错误码严格对齐（kErrBadParam=1/kErrInternal=3/kErrNoDoc=5）。

namespace montage {
namespace bridge {

namespace {

constexpr int kErrBadParam = 1;
constexpr int kErrInternal = 3;
constexpr int kErrNoDoc = 5;

napi_value makeOkLocal(napi_env env, napi_value data) {
    napi_value result = nullptr;
    napi_value okValue = nullptr;
    napi_create_object(env, &result);
    napi_get_boolean(env, true, &okValue);
    napi_set_named_property(env, result, "ok", okValue);
    if (data != nullptr) {
        napi_set_named_property(env, result, "data", data);
    }
    return result;
}

napi_value makeErrorLocal(napi_env env, int32_t code, const std::string& message) {
    napi_value result = nullptr;
    napi_value okValue = nullptr;
    napi_value errObj = nullptr;
    napi_value codeValue = nullptr;
    napi_value msgValue = nullptr;
    napi_create_object(env, &errObj);
    napi_create_int32(env, code, &codeValue);
    napi_create_string_utf8(env, message.c_str(), message.size(), &msgValue);
    napi_set_named_property(env, errObj, "code", codeValue);
    napi_set_named_property(env, errObj, "message", msgValue);
    napi_create_object(env, &result);
    napi_get_boolean(env, false, &okValue);
    napi_set_named_property(env, result, "ok", okValue);
    napi_set_named_property(env, result, "error", errObj);
    return result;
}

napi_value makeDoubleLocal(napi_env env, double v) {
    napi_value out = nullptr;
    napi_create_double(env, v, &out);
    return out;
}

napi_value makeUint32Local(napi_env env, uint32_t v) {
    napi_value out = nullptr;
    napi_create_uint32(env, v, &out);
    return out;
}

napi_value makeStringLocal(napi_env env, const std::string& v) {
    napi_value out = nullptr;
    napi_create_string_utf8(env, v.c_str(), v.size(), &out);
    return out;
}

// 导出异步任务上下文（ExportPng 同范式：锁内快照 → libuv 线程锁外扁平化+编码 → JS resolve）
struct CodecExportCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    io::FlattenJob job;
    io::FlattenResult result;
    int fd = -1;
    std::string format;
    int quality = 90;
    std::vector<uint8_t> encoded;
    uint64_t bytes = 0;
    // P7：分层 TIFF（tif/tiff 且文档 ≤30000px 且有层）——合成图 + 37724 Layr
    io::PsdDocSnap psdSnap;
    bool layered = false;
    // P7：GIF 多帧（≥2 可见像素层）——每层 displayPixels 全画布帧
    struct GifFrameSrc {
        std::shared_ptr<const TileGrid> pixels;
        double originX = 0.0;
        double originY = 0.0;
    };
    std::vector<GifFrameSrc> gifFrames;
    uint32_t gifW = 0;
    uint32_t gifH = 0;
};

}  // namespace

napi_value ExportImage(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeErrorLocal(env, kErrBadParam, "exportImage(fd, format[, quality]) requires 2 args");
    }
    int32_t fd = -1;
    if (napi_get_value_int32(env, argv[0], &fd) != napi_ok || fd < 0) {
        return makeErrorLocal(env, kErrBadParam, "exportImage: invalid fd");
    }
    size_t flen = 0;
    char fbuf[32] = {0};
    if (napi_get_value_string_utf8(env, argv[1], fbuf, sizeof(fbuf), &flen) != napi_ok) {
        close(fd);
        return makeErrorLocal(env, kErrBadParam, "exportImage: invalid format");
    }
    const std::string format(fbuf, flen);
    int quality = 90;
    if (argc >= 3 && argv[2] != nullptr) {
        double q = 0;
        if (napi_get_value_double(env, argv[2], &q) == napi_ok && q > 0 && q <= 100) {
            quality = static_cast<int>(q);
        }
    }
    if (!io::codecs::canExport(format)) {
        close(fd);
        return makeErrorLocal(env, kErrBadParam,
                              "exportImage: format '" + format + "' not exportable");
    }

    auto& e = Engine::get();
    auto* ctx = new CodecExportCtx();
    ctx->fd = fd;  // 全路径接管：execute 关闭（成功/失败都关）
    ctx->format = format;
    ctx->quality = quality;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc().width == 0 || e.doc().height == 0) {
            delete ctx;
            close(fd);
            return makeErrorLocal(env, kErrNoDoc, "exportImage: no document");
        }
        ctx->job = io::BuildFlattenJob(e.doc(), 0, 0, 0, 0);
        // P7：GIF 多帧判定（≥2 可见像素层 → 层=帧）
        if (format == "gif") {
            for (const Layer& l : e.doc().layers) {
                if (!l.visible || l.isGroup || l.adjustment != nullptr) {
                    continue;
                }
                auto px = l.displayPixels();
                if (px == nullptr || px->tiles.empty()) {
                    continue;
                }
                CodecExportCtx::GifFrameSrc src;
                src.pixels = std::move(px);
                src.originX = l.effectiveOriginX();
                src.originY = l.effectiveOriginY();
                ctx->gifFrames.push_back(std::move(src));
                if (ctx->gifFrames.size() >= 64) {
                    break;  // v1 帧数上限（超出回退合成图单帧）
                }
            }
            if (ctx->gifFrames.size() < 2) {
                ctx->gifFrames.clear();
            }
            ctx->gifW = e.doc().width;
            ctx->gifH = e.doc().height;
        }
        // P7：分层 TIFF 判定（tif/tiff、≤30000px、至少一个层记录——空档回退平铺）
        if (format == "tif" || format == "tiff") {
            if (e.doc().width <= 30000 && e.doc().height <= 30000) {
                bridge::BuildPsdDocSnapLocked(e, ctx->psdSnap);
                ctx->psdSnap.zipComposite = false;
                ctx->psdSnap.psb = false;
                bool anyLayer = false;
                for (const auto& pl : ctx->psdSnap.layers) {
                    if (pl.pixels != nullptr || pl.isGroup || pl.isAdjustment) {
                        anyLayer = true;
                        break;
                    }
                }
                ctx->layered = anyLayer;
            }
        }
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montageExportImage", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<CodecExportCtx*>(data);
            c->result = io::FlattenVisible(c->job, 0, 0, 0, 0);
            if (!c->result.ok) {
                return;
            }
            std::string err;
            if (!c->gifFrames.empty()) {
                // P7：层→帧（displayPixels 全画布合成，透明底；每帧独立量化）
                std::vector<std::vector<uint8_t>> bufs;
                std::vector<const uint8_t*> fptrs;
                std::vector<uint16_t> delays;
                for (const auto& src : c->gifFrames) {
                    bufs.emplace_back(static_cast<size_t>(c->gifW) * c->gifH * 4, 0);
                    std::vector<uint8_t>& canvas = bufs.back();
                    const TileGrid& g = *src.pixels;
                    const uint32_t gw = g.cols * kTileSize;
                    const uint32_t gh = g.rows * kTileSize;
                    std::vector<uint8_t> row(gw * 4);
                    for (uint32_t y = 0; y < gh; ++y) {
                        const int64_t dy = static_cast<int64_t>(src.originY) + y;
                        if (dy < 0 || dy >= static_cast<int64_t>(c->gifH)) {
                            continue;
                        }
                        for (uint32_t tx = 0; tx < g.cols; ++tx) {
                            const uint32_t x0 = tx * kTileSize;
                            auto it = g.tiles.find(static_cast<uint32_t>(y / kTileSize) * g.cols + tx);
                            if (it == g.tiles.end() || it->second == nullptr) {
                                continue;
                            }
                            const uint8_t* tp = it->second->pixels->mapCpu();
                            if (tp == nullptr) {
                                continue;
                            }
                            std::memcpy(row.data() + static_cast<size_t>(x0) * 4,
                                        tp + static_cast<size_t>(y % kTileSize) *
                                                 it->second->pixels->rowBytes(),
                                        static_cast<size_t>(kTileSize) * 4);
                            it->second->pixels->unmap();
                        }
                        uint8_t* drow = canvas.data() + static_cast<size_t>(dy) * c->gifW * 4;
                        for (uint32_t x = 0; x < gw; ++x) {
                            const int64_t dx = static_cast<int64_t>(src.originX) + x;
                            if (dx < 0 || dx >= static_cast<int64_t>(c->gifW)) {
                                continue;
                            }
                            const uint8_t* s4 = row.data() + static_cast<size_t>(x) * 4;
                            if (s4[3] != 0) {
                                uint8_t* d4 = drow + static_cast<size_t>(dx) * 4;
                                d4[0] = s4[0];
                                d4[1] = s4[1];
                                d4[2] = s4[2];
                                d4[3] = s4[3];
                            }
                        }
                    }
                    fptrs.push_back(canvas.data());
                    delays.push_back(10);  // 100ms/帧 v1 固定
                }
                if (!io::codecs::gifEncodeFrames(fptrs, c->gifW, c->gifH, delays, 256, false,
                                                 c->encoded, err)) {
                    c->encoded.clear();  // 回退合成图单帧
                }
            }
            if (c->encoded.empty()) {
            if (c->layered) {
                // P7：分层 TIFF（合成图直用扁平结果；失败回退平铺编码）
                c->psdSnap.flattenedRgba =
                    std::make_shared<std::vector<uint8_t>>(c->result.rgba);
                if (io::codecs::tiffEncodeLayers(c->psdSnap, "deflate", c->encoded, err)) {
                    // fallthrough to write
                } else {
                    c->layered = false;  // 回退平铺
                    c->encoded.clear();
                }
            }
            if (!c->layered &&
                !io::codecs::encode(c->format, c->result.rgba.data(), c->result.width,
                                    c->result.height, c->quality, c->encoded, err)) {
                c->result.ok = false;
                c->result.error = err;
                return;
            }
            }
            size_t off = 0;
            while (off < c->encoded.size()) {
                const ssize_t n = write(c->fd, c->encoded.data() + off, c->encoded.size() - off);
                if (n <= 0) {
                    c->result.ok = false;
                    c->result.error = "export write failed";
                    return;
                }
                off += static_cast<size_t>(n);
            }
            c->bytes = c->encoded.size();
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<CodecExportCtx*>(data);
            if (c->fd >= 0) {
                close(c->fd);
                c->fd = -1;
            }
            if (status == napi_ok && c->result.ok) {
                napi_value d = nullptr;
                napi_create_object(env, &d);
                napi_set_named_property(env, d, "width", makeUint32Local(env, c->result.width));
                napi_set_named_property(env, d, "height", makeUint32Local(env, c->result.height));
                napi_set_named_property(env, d, "bytes",
                                        makeDoubleLocal(env, static_cast<double>(c->bytes)));
                napi_resolve_deferred(env, c->deferred, makeOkLocal(env, d));
            } else {
                napi_resolve_deferred(env, c->deferred,
                                      makeErrorLocal(env, kErrInternal,
                                                     c->result.error.empty() ? "exportImage failed"
                                                                             : c->result.error));
            }
            napi_delete_async_work(env, c->work);
            delete c;
        },
        ctx, &ctx->work);
    if (st != napi_ok) {
        close(fd);
        delete ctx;
        return makeErrorLocal(env, kErrInternal, "create async work failed");
    }
    napi_queue_async_work(env, ctx->work);
    return promise;
}

// 能力表透出（格式×读×写×深度单一事实来源；顺序 = UI 呈现顺序）
// 返回 {ok, data:{formats:[{format,read,write,depths,exts:[],note}], openExts:[], saveExts:[]}}
napi_value GetCodecCapabilities(napi_env env, napi_callback_info /*info*/) {
    napi_value data = nullptr;
    napi_create_object(env, &data);

    napi_value formats = nullptr;
    napi_create_array(env, &formats);
    uint32_t i = 0;
    std::string openExts;
    std::string saveExts;
    for (const io::codecs::CodecCap& c : io::codecs::capabilities()) {
        napi_value o = nullptr;
        napi_create_object(env, &o);
        napi_set_named_property(env, o, "format", makeStringLocal(env, c.format));
        napi_value rd = nullptr;
        napi_get_boolean(env, c.read, &rd);
        napi_set_named_property(env, o, "read", rd);
        napi_value wr = nullptr;
        napi_get_boolean(env, c.write, &wr);
        napi_set_named_property(env, o, "write", wr);
        napi_set_named_property(env, o, "depths", makeStringLocal(env, c.depths));
        // exts: 分号拆数组
        napi_value exts = nullptr;
        napi_create_array(env, &exts);
        uint32_t ei = 0;
        std::string es(c.exts);
        size_t pos = 0;
        while (pos < es.size()) {
            const size_t semi = es.find(';', pos);
            const std::string one = es.substr(pos, semi == std::string::npos ? std::string::npos
                                                                             : semi - pos);
            if (!one.empty()) {
                napi_set_element(env, exts, ei++, makeStringLocal(env, one));
                if (c.read) {
                    if (!openExts.empty()) openExts += ';';
                    openExts += one;
                }
                if (c.write) {
                    if (!saveExts.empty()) saveExts += ';';
                    saveExts += one;
                }
            }
            if (semi == std::string::npos) {
                break;
            }
            pos = semi + 1;
        }
        napi_set_named_property(env, o, "exts", exts);
        napi_set_named_property(env, o, "note", makeStringLocal(env, c.note));
        napi_set_element(env, formats, i++, o);
    }
    napi_set_named_property(env, data, "formats", formats);
    napi_set_named_property(env, data, "openExts", makeStringLocal(env, openExts));
    napi_set_named_property(env, data, "saveExts", makeStringLocal(env, saveExts));
    return makeOkLocal(env, data);
}

}  // namespace bridge
}  // namespace montage
