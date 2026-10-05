#include "bridge/engine_bridge.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#include <hilog/log.h>
#include <multimedia/image_framework/image/pixelmap_native.h>

#include "render/render_loop.h"
#include "tiles/pixel_probe.h"

namespace montage {
namespace bridge {
namespace {

constexpr LogType kLogType = LOG_APP;
constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Engine";

// 错误码：错误模型走 Result 对象（{ok,data}|{ok:false,error:{code,message}}），不跨桥抛异常。
enum : int32_t {
    kErrBadParam = 1,
    kErrSurface = 2,
    kErrInternal = 3,
};

struct Engine {
    static Engine& get() {
        static Engine inst;
        return inst;
    }

    std::mutex mtx;
    int64_t docRevision = 0;
    bool docOpen = false;
    int32_t docWidth = 0;
    int32_t docHeight = 0;
    napi_threadsafe_function revisionTsfn = nullptr;
    RenderLoop render;

    void bumpRevisionLocked() {
        docRevision++;
        if (revisionTsfn == nullptr) {
            return;
        }
        void* payload = reinterpret_cast<void*>(static_cast<intptr_t>(docRevision));
        napi_status st = napi_call_threadsafe_function(revisionTsfn, payload, napi_tsfn_nonblocking);
        if (st != napi_ok) {
            OH_LOG_Print(kLogType, LOG_WARN, kDomain, kTag, "revision tsfn call st=%{public}d",
                         static_cast<int>(st));
        }
    }
};

void revisionJsCallback(napi_env env, napi_value jsCb, void* /*context*/, void* data) {
    const int64_t rev = static_cast<int64_t>(reinterpret_cast<intptr_t>(data));
    napi_value revValue = nullptr;
    napi_value undefined = nullptr;
    napi_create_int64(env, rev, &revValue);
    napi_get_undefined(env, &undefined);
    napi_call_function(env, undefined, jsCb, 1, &revValue, nullptr);
}

napi_value makeOk(napi_env env, napi_value data) {
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

napi_value makeError(napi_env env, int32_t code, const std::string& message) {
    napi_value result = nullptr;
    napi_value okValue = nullptr;
    napi_value errObj = nullptr;
    napi_value codeValue = nullptr;
    napi_value msgValue = nullptr;
    napi_create_object(env, &errObj);
    napi_create_int32(env, code, &codeValue);
    napi_create_string_utf8(env, message.c_str(), NAPI_AUTO_LENGTH, &msgValue);
    napi_set_named_property(env, errObj, "code", codeValue);
    napi_set_named_property(env, errObj, "message", msgValue);
    napi_create_object(env, &result);
    napi_get_boolean(env, false, &okValue);
    napi_set_named_property(env, result, "ok", okValue);
    napi_set_named_property(env, result, "error", errObj);
    return result;
}

napi_value makeInt32(napi_env env, int32_t v) {
    napi_value out = nullptr;
    napi_create_int32(env, v, &out);
    return out;
}

napi_value makeUint32(napi_env env, uint32_t v) {
    napi_value out = nullptr;
    napi_create_uint32(env, v, &out);
    return out;
}

napi_value makeDouble(napi_env env, double v) {
    napi_value out = nullptr;
    napi_create_double(env, v, &out);
    return out;
}

napi_value makeBool(napi_env env, bool v) {
    napi_value out = nullptr;
    napi_get_boolean(env, v, &out);
    return out;
}

napi_value makeString(napi_env env, const std::string& v) {
    napi_value out = nullptr;
    napi_create_string_utf8(env, v.c_str(), NAPI_AUTO_LENGTH, &out);
    return out;
}

napi_value makeProbeObject(napi_env env, const tiles::ProbeResult& p) {
    napi_value obj = nullptr;
    napi_create_object(env, &obj);
    napi_set_named_property(env, obj, "allocator", makeString(env, p.allocator));
    napi_set_named_property(env, obj, "createOk", makeBool(env, p.createOk));
    napi_set_named_property(env, obj, "createRc", makeInt32(env, p.createRc));
    napi_set_named_property(env, obj, "width", makeUint32(env, p.width));
    napi_set_named_property(env, obj, "height", makeUint32(env, p.height));
    napi_set_named_property(env, obj, "rowStride", makeUint32(env, p.rowStride));
    napi_set_named_property(env, obj, "pixelFormat", makeInt32(env, p.pixelFormat));
    napi_set_named_property(env, obj, "accessOk", makeBool(env, p.accessOk));
    napi_set_named_property(env, obj, "accessRc", makeInt32(env, p.accessRc));
    napi_set_named_property(env, obj, "writeReadOk", makeBool(env, p.writeReadOk));
    napi_set_named_property(env, obj, "error", makeString(env, p.error));
    return obj;
}

// M0 像素测试异步上下文：execute 在 libuv 工作线程跑探针，complete 回 JS 线程组装结果。
struct PixelTestContext {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    tiles::ProbeResult dma;
    tiles::ProbeResult shared;
    tiles::ProbeHandle sample;
};

}  // namespace

napi_value NewDocument(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "newDocument(w, h) requires 2 args");
    }
    int32_t w = 0;
    int32_t h = 0;
    if (napi_get_value_int32(env, argv[0], &w) != napi_ok ||
        napi_get_value_int32(env, argv[1], &h) != napi_ok || w <= 0 || h <= 0) {
        return makeError(env, kErrBadParam, "newDocument: invalid size");
    }
    auto& e = Engine::get();
    int64_t rev = 0;
    {
        std::lock_guard<std::mutex> lk(e.mtx);
        e.docOpen = true;
        e.docWidth = w;
        e.docHeight = h;
        e.bumpRevisionLocked();
        rev = e.docRevision;
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "revision", makeDouble(env, static_cast<double>(rev)));
    napi_set_named_property(env, data, "width", makeInt32(env, w));
    napi_set_named_property(env, data, "height", makeInt32(env, h));
    OH_LOG_Print(kLogType, LOG_INFO, kDomain, kTag, "newDocument %{public}dx%{public}d rev=%{public}lld",
                 w, h, static_cast<long long>(rev));
    return makeOk(env, data);
}

napi_value CloseDocument(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.mtx);
        e.docOpen = false;
        e.bumpRevisionLocked();
    }
    return makeOk(env, nullptr);
}

napi_value CreateSurface(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "createSurface(surfaceId) requires 1 arg");
    }
    napi_valuetype t = napi_undefined;
    napi_typeof(env, argv[0], &t);
    if (t != napi_string) {
        return makeError(env, kErrBadParam, "createSurface: surfaceId must be string");
    }
    char buf[64] = {0};
    size_t len = 0;
    if (napi_get_value_string_utf8(env, argv[0], buf, sizeof(buf), &len) != napi_ok) {
        return makeError(env, kErrBadParam, "createSurface: read string failed");
    }
    const uint64_t surfaceId = std::strtoull(buf, nullptr, 10);
    auto& e = Engine::get();
    std::string err;
    if (!e.render.start(surfaceId, &err)) {
        OH_LOG_Print(kLogType, LOG_ERROR, kDomain, kTag, "createSurface failed: %{public}s", err.c_str());
        return makeError(env, kErrSurface, err);
    }
    OH_LOG_Print(kLogType, LOG_INFO, kDomain, kTag, "createSurface ok: %{public}s", buf);
    return makeOk(env, nullptr);
}

napi_value DestroySurface(napi_env env, napi_callback_info /*info*/) {
    Engine::get().render.stop();
    return makeOk(env, nullptr);
}

napi_value GetStats(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    const RenderLoop::Snapshot snap = e.render.snapshot();
    int64_t rev = 0;
    bool open = false;
    {
        std::lock_guard<std::mutex> lk(e.mtx);
        rev = e.docRevision;
        open = e.docOpen;
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "fps", makeDouble(env, snap.fps));
    napi_set_named_property(env, data, "frames", makeDouble(env, static_cast<double>(snap.frames)));
    napi_set_named_property(env, data, "bufferWidth", makeInt32(env, snap.width));
    napi_set_named_property(env, data, "bufferHeight", makeInt32(env, snap.height));
    napi_set_named_property(env, data, "docRevision", makeDouble(env, static_cast<double>(rev)));
    napi_set_named_property(env, data, "docOpen", makeBool(env, open));
    napi_set_named_property(env, data, "renderRunning", makeBool(env, snap.running));
    napi_set_named_property(env, data, "lastError", makeString(env, snap.lastError));
    napi_set_named_property(env, data, "callbackThread", makeString(env, snap.callbackThread));
    return makeOk(env, data);
}

napi_value OnRevisionChanged(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "onRevisionChanged(callback) requires 1 arg");
    }
    napi_valuetype t = napi_undefined;
    napi_typeof(env, argv[0], &t);
    if (t != napi_function) {
        return makeError(env, kErrBadParam, "onRevisionChanged: callback must be function");
    }
    auto& e = Engine::get();
    napi_threadsafe_function tsfn = nullptr;
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "montageRevision", NAPI_AUTO_LENGTH, &resourceName);
    napi_status st = napi_create_threadsafe_function(env, argv[0], nullptr, resourceName, 32, 1, nullptr,
                                                     nullptr, nullptr, revisionJsCallback, &tsfn);
    if (st != napi_ok) {
        return makeError(env, kErrInternal, "create threadsafe function failed");
    }
    {
        std::lock_guard<std::mutex> lk(e.mtx);
        if (e.revisionTsfn != nullptr) {
            napi_release_threadsafe_function(e.revisionTsfn, napi_tsfn_release);
        }
        e.revisionTsfn = tsfn;
    }
    return makeOk(env, nullptr);
}

napi_value RunPixelTest(napi_env env, napi_callback_info /*info*/) {
    napi_value promise = nullptr;
    napi_deferred deferred = nullptr;
    napi_create_promise(env, &deferred, &promise);

    auto* ctx = new PixelTestContext();
    ctx->deferred = deferred;
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montagePixelTest", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<PixelTestContext*>(data);
            c->dma = tiles::ProbeAllocator(IMAGE_ALLOCATOR_MODE_DMA, 1024, 1024);
            c->shared = tiles::ProbeAllocator(IMAGE_ALLOCATOR_MODE_SHARED_MEMORY, 256, 256);
            c->sample = tiles::ProbeCreateKeep(IMAGE_ALLOCATOR_MODE_SHARED_MEMORY, 64, 64);
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<PixelTestContext*>(data);
            napi_value dataObj = nullptr;
            napi_create_object(env, &dataObj);
            napi_set_named_property(env, dataObj, "dma", makeProbeObject(env, c->dma));
            napi_set_named_property(env, dataObj, "shared", makeProbeObject(env, c->shared));
            if (c->sample.pixelmap != nullptr) {
                napi_value pmValue = nullptr;
                const Image_ErrorCode crc = OH_PixelmapNative_ConvertPixelmapNativeToNapi(
                    env, static_cast<OH_PixelmapNative*>(c->sample.pixelmap), &pmValue);
                if (crc == IMAGE_SUCCESS && pmValue != nullptr) {
                    napi_set_named_property(env, dataObj, "samplePixelMap", pmValue);
                } else {
                    OH_LOG_Print(kLogType, LOG_WARN, kDomain, kTag,
                                 "ConvertPixelmapNativeToNapi rc=%{public}d", static_cast<int>(crc));
                }
            }
            tiles::ReleaseProbeHandle(c->sample);
            napi_value result = (status == napi_ok)
                                    ? makeOk(env, dataObj)
                                    : makeError(env, kErrInternal, "pixel test work failed");
            napi_resolve_deferred(env, c->deferred, result);
            napi_delete_async_work(env, c->work);
            delete c;
        },
        ctx, &ctx->work);
    if (st != napi_ok) {
        delete ctx;
        return makeError(env, kErrInternal, "create async work failed");
    }
    napi_queue_async_work(env, ctx->work);
    return promise;
}

napi_value Dispose(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    napi_threadsafe_function tsfn = nullptr;
    {
        std::lock_guard<std::mutex> lk(e.mtx);
        tsfn = e.revisionTsfn;
        e.revisionTsfn = nullptr;
    }
    if (tsfn != nullptr) {
        napi_release_threadsafe_function(tsfn, napi_tsfn_release);
    }
    e.render.stop();
    return makeOk(env, nullptr);
}

}  // namespace bridge
}  // namespace montage
