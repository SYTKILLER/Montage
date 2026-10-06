#include "bridge/engine_bridge.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>

#include <hilog/log.h>
#include <multimedia/image_framework/image/pixelmap_native.h>

#include "engine/engine.h"
#include "tiles/magic_wand.h"
#include "io/image_import.h"
#include "io/project_open.h"
#include "io/project_save.h"
#include "io/psd/psd_import.h"
#include "render/render_loop.h"
#include "tiles/pixel_probe.h"

namespace montage {
namespace bridge {
namespace {

constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Engine";

// 错误码：错误模型走 Result 对象（{ok,data}|{ok:false,error:{code,message}}），不跨桥抛异常。
enum : int32_t {
    kErrBadParam = 1,
    kErrSurface = 2,
    kErrInternal = 3,
    kErrBusy = 4,
    kErrNoDoc = 5,
};

// ---------- Result 对象工具 ----------

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

napi_value makeViewportData(napi_env env, const Viewport& vp) {
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "zoom", makeDouble(env, vp.zoom));
    napi_set_named_property(env, data, "panX", makeDouble(env, vp.panX));
    napi_set_named_property(env, data, "panY", makeDouble(env, vp.panY));
    return data;
}

// ---------- 事件面 TSFN 回调 ----------

void revisionJsCallback(napi_env env, napi_value jsCb, void* /*context*/, void* data) {
    const int64_t rev = static_cast<int64_t>(reinterpret_cast<intptr_t>(data));
    napi_value revValue = nullptr;
    napi_value undefined = nullptr;
    napi_create_int64(env, rev, &revValue);
    napi_get_undefined(env, &undefined);
    napi_call_function(env, undefined, jsCb, 1, &revValue, nullptr);
}

void progressJsCallback(napi_env env, napi_value jsCb, void* /*context*/, void* data) {
    const double progress = static_cast<double>(reinterpret_cast<intptr_t>(data)) / 1000.0;
    napi_value v = nullptr;
    napi_value undefined = nullptr;
    napi_create_double(env, progress, &v);
    napi_get_undefined(env, &undefined);
    napi_call_function(env, undefined, jsCb, 1, &v, nullptr);
}

// 导入完成 → resolve Promise（openFileFromFd 的 deferred 挂在 context 上）
struct OpenCtx {
    napi_deferred deferred = nullptr;
    napi_threadsafe_function tsfn = nullptr;
};

void importDoneJsCallback(napi_env env, napi_value /*jsCb*/, void* context, void* data) {
    auto* ctx = static_cast<OpenCtx*>(context);
    auto* p = static_cast<io::ImportCompletion*>(data);
    napi_value result;
    if (p != nullptr && p->ok) {
        napi_value d = nullptr;
        napi_create_object(env, &d);
        napi_set_named_property(env, d, "width", makeUint32(env, p->width));
        napi_set_named_property(env, d, "height", makeUint32(env, p->height));
        napi_set_named_property(env, d, "layerId", makeDouble(env, static_cast<double>(p->layerId)));
        result = makeOk(env, d);
    } else {
        result = makeError(env, kErrInternal, p != nullptr ? std::string(p->error) : "import failed");
    }
    napi_resolve_deferred(env, ctx->deferred, result);
    delete p;
    napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);  // finalize 删 ctx
}

void finalizeOpenCtx(napi_env /*env*/, void* data, void* /*hint*/) {
    delete static_cast<OpenCtx*>(data);
}

napi_value noopJsCallback(napi_env /*env*/, napi_callback_info /*info*/) {
  return nullptr;
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

// 替换/登记 TSFN 的通用流程：创建 → 换入 Engine（旧的 release）
napi_status bindTsfn(napi_env env, napi_callback_info info, const char* name, int32_t maxQueue,
                     napi_threadsafe_function_call_js cb, napi_threadsafe_function* out) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return napi_invalid_arg;
    }
    napi_valuetype t = napi_undefined;
    napi_typeof(env, argv[0], &t);
    if (t != napi_function) {
        return napi_invalid_arg;
    }
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &resourceName);
    napi_status st = napi_create_threadsafe_function(env, argv[0], nullptr, resourceName, maxQueue, 1,
                                                     nullptr, nullptr, nullptr, cb, out);
    return st;
}

}  // namespace

// ---------- 会话 ----------

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
    io::cancelImageImport();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.doc = Document{};
        e.doc.width = static_cast<uint32_t>(w);
        e.doc.height = static_cast<uint32_t>(h);
        e.doc.name = "Untitled";
        e.viewport = Viewport{};
        e.bumpRevisionLocked();
        e.requestRender();
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "newDocument %{public}dx%{public}d", w, h);
    return makeOk(env, nullptr);
}

napi_value CloseDocument(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    io::cancelImageImport();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.doc = Document{};
        e.viewport = Viewport{};
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// ---------- 导入 ----------

napi_value OpenFileFromFd(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "openFileFromFd(fd) requires 1 arg");
    }
    int32_t fd = -1;
    if (napi_get_value_int32(env, argv[0], &fd) != napi_ok || fd < 0) {
        return makeError(env, kErrBadParam, "openFileFromFd: invalid fd");
    }
    napi_value promise = nullptr;
    napi_deferred deferred = nullptr;
    napi_create_promise(env, &deferred, &promise);

    auto* ctx = new OpenCtx();
    ctx->deferred = deferred;
    napi_value fn = nullptr;
    napi_value name = nullptr;
    napi_create_string_utf8(env, "importDone", NAPI_AUTO_LENGTH, &name);
    napi_create_function(env, "onImportDone", NAPI_AUTO_LENGTH, noopJsCallback, nullptr, &fn);
    napi_status st = napi_create_threadsafe_function(env, fn, nullptr, name, 4, 1, ctx,
                                                     finalizeOpenCtx, ctx, importDoneJsCallback,
                                                     &ctx->tsfn);
    if (st != napi_ok) {
        finalizeOpenCtx(env, ctx, nullptr);
        napi_resolve_deferred(env, deferred, makeError(env, kErrInternal, "create tsfn failed"));
        return promise;
    }
    if (!io::startImageImport(fd, ctx->tsfn)) {
        // fd 所有权已被 native 接管并关闭（忙路径）
        napi_resolve_deferred(env, deferred, makeError(env, kErrBusy, "another import is running"));
        napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
    }
    return promise;
}

// ---------- PSD 导入（04 §1.1 PSD-1） ----------

struct PsdPayload {
    bool ok = false;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<std::string> notes;
    std::string error;
};

void psdDoneJsCallback(napi_env env, napi_value /*jsCb*/, void* context, void* data) {
    auto* ctx = static_cast<OpenCtx*>(context);
    auto* p = static_cast<PsdPayload*>(data);
    napi_value result;
    if (p != nullptr && p->ok) {
        napi_value d = nullptr;
        napi_create_object(env, &d);
        napi_set_named_property(env, d, "width", makeInt32(env, p->width));
        napi_set_named_property(env, d, "height", makeInt32(env, p->height));
        napi_value arr = nullptr;
        napi_create_array(env, &arr);
        uint32_t i = 0;
        for (const std::string& n : p->notes) {
            napi_set_element(env, arr, i++, makeString(env, n));
        }
        napi_set_named_property(env, d, "notes", arr);
        result = makeOk(env, d);
    } else {
        result = makeError(env, kErrInternal, p != nullptr ? p->error : "psd import failed");
    }
    delete p;
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
}

napi_value OpenPsdFile(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "openPsdFile(fd) requires 1 arg");
    }
    int32_t fd = -1;
    if (napi_get_value_int32(env, argv[0], &fd) != napi_ok || fd < 0) {
        return makeError(env, kErrBadParam, "openPsdFile: invalid fd");
    }
    napi_value promise = nullptr;
    napi_deferred deferred = nullptr;
    napi_create_promise(env, &deferred, &promise);

    auto* ctx = new OpenCtx();
    ctx->deferred = deferred;
    napi_value fn = nullptr;
    napi_value name = nullptr;
    napi_create_string_utf8(env, "psdDone", NAPI_AUTO_LENGTH, &name);
    napi_create_function(env, "onPsdDone", NAPI_AUTO_LENGTH, noopJsCallback, nullptr, &fn);
    napi_status st = napi_create_threadsafe_function(env, fn, nullptr, name, 4, 1, ctx,
                                                     finalizeOpenCtx, ctx, psdDoneJsCallback,
                                                     &ctx->tsfn);
    if (st != napi_ok) {
        finalizeOpenCtx(env, ctx, nullptr);
        napi_resolve_deferred(env, deferred, makeError(env, kErrInternal, "create tsfn failed"));
        return promise;
    }
    // 与图片导入互斥（共用 import 状态）
    {
        auto& e = Engine::get();
        std::lock_guard<std::mutex> lk(e.import.mtx);
        if (e.import.running) {
            close(fd);
            napi_resolve_deferred(env, deferred,
                                  makeError(env, kErrBusy, "another import is running"));
            napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
            return promise;
        }
        e.import.cancel = false;
        e.import.progress = 0.0;
        e.import.running = true;
    }
    std::thread([fd, tsfn = ctx->tsfn]() {
        auto* payload = new PsdPayload();
        std::string err;
        payload->ok = io::importPsd(fd, &payload->width, &payload->height, &payload->notes, err);
        if (!payload->ok) {
            payload->error = err;
        }
        auto& e = Engine::get();
        {
            std::lock_guard<std::mutex> lk(e.import.mtx);
            e.import.running = false;
        }
        napi_call_threadsafe_function(tsfn, payload, napi_tsfn_nonblocking);
    }).detach();
    return promise;
}

// ---------- 工程保存（04 §1.2 M4b） ----------

struct SavePayload {
    bool ok = false;
    size_t layers = 0;
    uint64_t bytes = 0;
    char error[192] = {0};
};

void saveDoneJsCallback(napi_env env, napi_value /*jsCb*/, void* context, void* data) {
    auto* ctx = static_cast<OpenCtx*>(context);
    auto* p = static_cast<SavePayload*>(data);
    napi_value result;
    if (p != nullptr && p->ok) {
        napi_value d = nullptr;
        napi_create_object(env, &d);
        napi_set_named_property(env, d, "layers", makeUint32(env, static_cast<uint32_t>(p->layers)));
        napi_set_named_property(env, d, "bytes", makeDouble(env, static_cast<double>(p->bytes)));
        result = makeOk(env, d);
    } else {
        result = makeError(env, kErrInternal, p != nullptr ? std::string(p->error) : "save failed");
    }
    delete p;
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
}

napi_value SaveProject(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "saveProject(fd) requires 1 arg");
    }
    int32_t fd = -1;
    if (napi_get_value_int32(env, argv[0], &fd) != napi_ok || fd < 0) {
        return makeError(env, kErrBadParam, "saveProject: invalid fd");
    }
    {
        auto& e = Engine::get();
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            close(fd);
            return makeError(env, kErrNoDoc, "saveProject: no document");
        }
    }
    napi_value promise = nullptr;
    napi_deferred deferred = nullptr;
    napi_create_promise(env, &deferred, &promise);

    auto* ctx = new OpenCtx();
    ctx->deferred = deferred;
    napi_value fn = nullptr;
    napi_value name = nullptr;
    napi_create_string_utf8(env, "saveDone", NAPI_AUTO_LENGTH, &name);
    napi_create_function(env, "onSaveDone", NAPI_AUTO_LENGTH, noopJsCallback, nullptr, &fn);
    napi_status st = napi_create_threadsafe_function(env, fn, nullptr, name, 4, 1, ctx,
                                                     finalizeOpenCtx, ctx, saveDoneJsCallback,
                                                     &ctx->tsfn);
    if (st != napi_ok) {
        close(fd);
        finalizeOpenCtx(env, ctx, nullptr);
        napi_resolve_deferred(env, deferred, makeError(env, kErrInternal, "create tsfn failed"));
        return promise;
    }
    // saveProject 全路径接管 fd（成功/失败都关闭）
    std::thread([fd, tsfn = ctx->tsfn]() {
        auto* payload = new SavePayload();
        std::string err;
        payload->ok = io::saveProject(fd, &payload->layers, &payload->bytes, err);
        if (!payload->ok) {
            std::strncpy(payload->error, err.c_str(), sizeof(payload->error) - 1);
        }
        napi_call_threadsafe_function(tsfn, payload, napi_tsfn_nonblocking);
    }).detach();
    return promise;
}

// ---------- 视口 ----------

// ---------- 工程打开（04 §1.2 M4c） ----------

struct OpenProjectPayload {
    bool ok = false;
    uint32_t width = 0;
    uint32_t height = 0;
    size_t layers = 0;
    char error[192] = {0};
};

void openProjectDoneJsCallback(napi_env env, napi_value /*jsCb*/, void* context, void* data) {
    auto* ctx = static_cast<OpenCtx*>(context);
    auto* p = static_cast<OpenProjectPayload*>(data);
    napi_value result;
    if (p != nullptr && p->ok) {
        napi_value d = nullptr;
        napi_create_object(env, &d);
        napi_set_named_property(env, d, "width", makeUint32(env, p->width));
        napi_set_named_property(env, d, "height", makeUint32(env, p->height));
        napi_set_named_property(env, d, "layers", makeUint32(env, static_cast<uint32_t>(p->layers)));
        result = makeOk(env, d);
    } else {
        result = makeError(env, kErrInternal, p != nullptr ? std::string(p->error) : "open project failed");
    }
    delete p;
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
}

napi_value OpenProject(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "openProject(fd) requires 1 arg");
    }
    int32_t fd = -1;
    if (napi_get_value_int32(env, argv[0], &fd) != napi_ok || fd < 0) {
        return makeError(env, kErrBadParam, "openProject: invalid fd");
    }
    napi_value promise = nullptr;
    napi_deferred deferred = nullptr;
    napi_create_promise(env, &deferred, &promise);

    auto* ctx = new OpenCtx();
    ctx->deferred = deferred;
    napi_value fn = nullptr;
    napi_value name = nullptr;
    napi_create_string_utf8(env, "openProjectDone", NAPI_AUTO_LENGTH, &name);
    napi_create_function(env, "onOpenProjectDone", NAPI_AUTO_LENGTH, noopJsCallback, nullptr, &fn);
    napi_status st = napi_create_threadsafe_function(env, fn, nullptr, name, 4, 1, ctx,
                                                     finalizeOpenCtx, ctx, openProjectDoneJsCallback,
                                                     &ctx->tsfn);
    if (st != napi_ok) {
        close(fd);
        finalizeOpenCtx(env, ctx, nullptr);
        napi_resolve_deferred(env, deferred, makeError(env, kErrInternal, "create tsfn failed"));
        return promise;
    }
    // openProject 全路径接管 fd（成功/失败都关闭）
    std::thread([fd, tsfn = ctx->tsfn]() {
        auto* payload = new OpenProjectPayload();
        std::string err;
        io::ProjectOpenResult res;
        payload->ok = io::openProject(fd, &res, err);
        if (payload->ok) {
            payload->width = res.width;
            payload->height = res.height;
            payload->layers = res.layers;
        } else {
            std::strncpy(payload->error, err.c_str(), sizeof(payload->error) - 1);
        }
        napi_call_threadsafe_function(tsfn, payload, napi_tsfn_nonblocking);
    }).detach();
    return promise;
}

napi_value SetViewport(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) {
        return makeError(env, kErrBadParam, "setViewport(zoom, panX, panY) requires 3 args");
    }
    double zoom = 0;
    double panX = 0;
    double panY = 0;
    if (napi_get_value_double(env, argv[0], &zoom) != napi_ok ||
        napi_get_value_double(env, argv[1], &panX) != napi_ok ||
        napi_get_value_double(env, argv[2], &panY) != napi_ok) {
        return makeError(env, kErrBadParam, "setViewport: numbers required");
    }
    auto& e = Engine::get();
    Viewport vp;
    vp.zoom = clampZoom(zoom);
    vp.panX = panX;
    vp.panY = panY;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.viewport = vp;
        e.requestRender();
    }
    return makeOk(env, makeViewportData(env, vp));
}

napi_value FitToWindow(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "fitToWindow(canvasW, canvasH) requires 2 args");
    }
    double cw = 0;
    double ch = 0;
    if (napi_get_value_double(env, argv[0], &cw) != napi_ok ||
        napi_get_value_double(env, argv[1], &ch) != napi_ok || cw <= 0 || ch <= 0) {
        return makeError(env, kErrBadParam, "fitToWindow: invalid canvas size");
    }
    auto& e = Engine::get();
    Viewport vp;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "fitToWindow: no document");
        }
        vp.zoom = clampZoom(std::min(cw / e.doc.width, ch / e.doc.height));
        vp.panX = (static_cast<double>(e.doc.width) - cw / vp.zoom) / 2.0;
        vp.panY = (static_cast<double>(e.doc.height) - ch / vp.zoom) / 2.0;
        e.viewport = vp;
        e.requestRender();
    }
    return makeOk(env, makeViewportData(env, vp));
}

// ---------- 渲染表面 ----------

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
        OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, "createSurface failed: %{public}s", err.c_str());
        return makeError(env, kErrSurface, err);
    }
    // 表面就绪后按需出首帧
    e.requestRender();
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "createSurface ok: %{public}s", buf);
    return makeOk(env, nullptr);
}

napi_value DestroySurface(napi_env env, napi_callback_info /*info*/) {
    Engine::get().render.stop();
    return makeOk(env, nullptr);
}

// ---------- 状态 ----------

napi_value GetStats(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    const RenderLoop::Snapshot snap = e.render.snapshot();
    Viewport vp;
    int64_t rev = 0;
    uint32_t dw = 0;
    uint32_t dh = 0;
    double progress = 0.0;
    bool importing = false;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        vp = e.viewport;
        rev = e.docRevision;
        dw = e.doc.width;
        dh = e.doc.height;
    }
    {
        std::lock_guard<std::mutex> lk(e.import.mtx);
        progress = e.import.progress;
        importing = e.import.running;
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "fps", makeDouble(env, snap.fps));
    napi_set_named_property(env, data, "frames", makeDouble(env, static_cast<double>(snap.frames)));
    napi_set_named_property(env, data, "bufferWidth", makeInt32(env, snap.width));
    napi_set_named_property(env, data, "bufferHeight", makeInt32(env, snap.height));
    napi_set_named_property(env, data, "docRevision", makeDouble(env, static_cast<double>(rev)));
    napi_set_named_property(env, data, "docWidth", makeUint32(env, dw));
    napi_set_named_property(env, data, "docHeight", makeUint32(env, dh));
    napi_set_named_property(env, data, "zoom", makeDouble(env, vp.zoom));
    napi_set_named_property(env, data, "panX", makeDouble(env, vp.panX));
    napi_set_named_property(env, data, "panY", makeDouble(env, vp.panY));
    napi_set_named_property(env, data, "importRunning", makeBool(env, importing));
    napi_set_named_property(env, data, "importProgress", makeDouble(env, progress));
    napi_set_named_property(env, data, "renderRunning", makeBool(env, snap.running));
    napi_set_named_property(env, data, "lastError", makeString(env, snap.lastError));
    napi_set_named_property(env, data, "canUndo", makeBool(env, e.history.canUndo()));
    napi_set_named_property(env, data, "canRedo", makeBool(env, e.history.canRedo()));
    napi_set_named_property(env, data, "historyVersion",
                            makeDouble(env, static_cast<double>(e.historyVersion.load())));
    return makeOk(env, data);
}

// ---------- 事件面 ----------

napi_value OnRevisionChanged(napi_env env, napi_callback_info info) {
    napi_threadsafe_function tsfn = nullptr;
    if (bindTsfn(env, info, "montageRevision", 32, revisionJsCallback, &tsfn) != napi_ok) {
        return makeError(env, kErrBadParam, "onRevisionChanged(callback) requires function arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.revisionTsfn != nullptr) {
            napi_release_threadsafe_function(e.revisionTsfn, napi_tsfn_release);
        }
        e.revisionTsfn = tsfn;
    }
    return makeOk(env, nullptr);
}

napi_value OnImportProgress(napi_env env, napi_callback_info info) {
    napi_threadsafe_function tsfn = nullptr;
    if (bindTsfn(env, info, "montageImportProgress", 64, progressJsCallback, &tsfn) != napi_ok) {
        return makeError(env, kErrBadParam, "onImportProgress(callback) requires function arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.progressTsfn != nullptr) {
            napi_release_threadsafe_function(e.progressTsfn, napi_tsfn_release);
        }
        e.progressTsfn = tsfn;
    }
    return makeOk(env, nullptr);
}

// ---------- 图层（02 §5 M2 命令面）----------

namespace {
Layer* findLayerLocked(Document& doc, LayerId id) {
    for (Layer& l : doc.layers) {
        if (l.id == id) {
            return &l;
        }
    }
    return nullptr;
}

napi_value makeLayerDto(napi_env env, const Layer& l, bool isActive) {
    napi_value obj = nullptr;
    napi_create_object(env, &obj);
    napi_set_named_property(env, obj, "id", makeDouble(env, static_cast<double>(l.id)));
    napi_set_named_property(env, obj, "name", makeString(env, l.name));
    napi_set_named_property(env, obj, "visible", makeBool(env, l.visible));
    napi_set_named_property(env, obj, "opacity", makeDouble(env, l.opacity));
    napi_set_named_property(env, obj, "blendMode", makeInt32(env, static_cast<int32_t>(l.blendMode)));
    napi_set_named_property(env, obj, "isActive", makeBool(env, isActive));
    // 02 §5 DTO 形状保真：M2 无组，常量占位；hasMask 自 M5a 起为真值
    napi_set_named_property(env, obj, "isGroup", makeBool(env, false));
    napi_set_named_property(env, obj, "parentId", makeDouble(env, 0.0));
    napi_set_named_property(env, obj, "hasMask", makeBool(env, l.mask != nullptr));
    napi_set_named_property(env, obj, "maskEnabled", makeBool(env, l.mask != nullptr && l.mask->enabled));
    napi_set_named_property(env, obj, "clipping", makeBool(env, l.clipping));
    napi_set_named_property(env, obj, "isAdjustment", makeBool(env, l.adjustment != nullptr));
    if (l.adjustment != nullptr) {
        napi_set_named_property(env, obj, "adjBlack", makeDouble(env, l.adjustment->inBlack));
        napi_set_named_property(env, obj, "adjWhite", makeDouble(env, l.adjustment->inWhite));
        napi_set_named_property(env, obj, "adjGamma", makeDouble(env, l.adjustment->gamma));
    }
    napi_set_named_property(env, obj, "thumbToken",
                            makeDouble(env, static_cast<double>(l.pixels != nullptr ? l.pixels->revision : 0)));
    return obj;
}
}  // namespace

// M6a：添加 Levels 调整层（插到活动层之上并激活，history 事务）
napi_value AddAdjustmentLayer(napi_env env, napi_callback_info info) {
    auto& e = Engine::get();
    double newId = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "addAdjustmentLayer: no document");
        }
        e.history.beginEdit(e.doc, "add adjustment", static_cast<uint64_t>(e.docRevision));
        Layer layer;
        layer.id = e.nextLayerId();
        layer.name = "Levels";
        auto adj = std::make_shared<LayerAdjustment>();
        adj->kind = AdjustmentKind::Levels;
        layer.adjustment = std::move(adj);
        size_t at = e.doc.layers.size();
        for (size_t i = 0; i < e.doc.layers.size(); ++i) {
            if (e.doc.layers[i].id == e.doc.activeId) {
                at = i + 1;
                break;
            }
        }
        e.doc.layers.insert(e.doc.layers.begin() + static_cast<long>(at), std::move(layer));
        e.doc.activeId = e.doc.layers[at].id;
        newId = static_cast<double>(e.doc.layers[at].id);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "id", makeDouble(env, newId));
    return makeOk(env, data);
}

// M6a：设置调整层参数（COW + history 事务；滑条拖动结束调用）
napi_value SetAdjustmentParams(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        return makeError(env, kErrBadParam, "setAdjustmentParams(id, inBlack, inWhite, gamma) requires 4 args");
    }
    double id = 0, inBlack = 0, inWhite = 1, gamma = 1;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_double(env, argv[1], &inBlack) != napi_ok ||
        napi_get_value_double(env, argv[2], &inWhite) != napi_ok ||
        napi_get_value_double(env, argv[3], &gamma) != napi_ok) {
        return makeError(env, kErrBadParam, "setAdjustmentParams: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr || l->adjustment == nullptr) {
            return makeError(env, kErrBadParam, "setAdjustmentParams: adjustment layer not found");
        }
        if (l->adjustment->inBlack == static_cast<float>(inBlack) &&
            l->adjustment->inWhite == static_cast<float>(inWhite) &&
            l->adjustment->gamma == static_cast<float>(gamma)) {
            return makeOk(env, nullptr);
        }
        e.history.beginEdit(e.doc, "adjust params", static_cast<uint64_t>(e.docRevision));
        auto adj = std::make_shared<LayerAdjustment>(*l->adjustment);  // COW
        adj->inBlack = static_cast<float>(std::min(0.98, std::max(0.0, inBlack)));
        adj->inWhite = static_cast<float>(std::min(1.0, std::max(0.02, inWhite)));
        adj->gamma = static_cast<float>(std::min(10.0, std::max(0.1, gamma)));
        l->adjustment = std::move(adj);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value AddLayer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    char name[96] = {0};
    if (argc >= 1) {
        size_t len = 0;
        napi_get_value_string_utf8(env, argv[0], name, sizeof(name), &len);
    }
    auto& e = Engine::get();
    double newId = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "addLayer: no document");
        }
        Layer layer;
        layer.id = e.nextLayerId();
        layer.name = name[0] != '\0' ? std::string(name) : ("图层 " + std::to_string(layer.id));
        // 插到 active 之上（源 addLayer 语义：新图层出现在当前图层上方并激活）
        size_t at = e.doc.layers.size();
        for (size_t i = 0; i < e.doc.layers.size(); ++i) {
            if (e.doc.layers[i].id == e.doc.activeId) {
                at = i + 1;
                break;
            }
        }
        e.doc.layers.insert(e.doc.layers.begin() + static_cast<long>(at), std::move(layer));
        e.doc.activeId = e.doc.layers[at].id;
        newId = static_cast<double>(e.doc.layers[at].id);
        e.bumpRevisionLocked();
        e.requestRender();
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "id", makeDouble(env, newId));
    return makeOk(env, data);
}

napi_value RemoveLayer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "removeLayer(id) requires 1 arg");
    }
    double id = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "removeLayer: invalid id");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        size_t at = e.doc.layers.size();
        for (size_t i = 0; i < e.doc.layers.size(); ++i) {
            if (e.doc.layers[i].id == static_cast<LayerId>(id)) {
                at = i;
                break;
            }
        }
        if (at == e.doc.layers.size()) {
            return makeError(env, kErrBadParam, "removeLayer: layer not found");
        }
        e.doc.layers.erase(e.doc.layers.begin() + static_cast<long>(at));
        if (e.doc.activeId == static_cast<LayerId>(id)) {
            e.doc.activeId =
                e.doc.layers.empty() ? 0 : e.doc.layers[std::min(at, e.doc.layers.size() - 1)].id;
        }
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value SelectLayer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "selectLayer(id) requires 1 arg");
    }
    double id = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "selectLayer: invalid id");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (findLayerLocked(e.doc, static_cast<LayerId>(id)) == nullptr) {
            return makeError(env, kErrBadParam, "selectLayer: layer not found");
        }
        e.doc.activeId = static_cast<LayerId>(id);
        e.bumpRevisionLocked();
    }
    return makeOk(env, nullptr);
}

// M5a：蒙版启用/停用（COW 换 LayerMask + 重合成派生网格；撤销粒度 = 单次切换）
napi_value SetLayerMaskEnabled(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "setLayerMaskEnabled(id, v) requires 2 args");
    }
    double id = 0;
    bool v = false;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_bool(env, argv[1], &v) != napi_ok) {
        return makeError(env, kErrBadParam, "setLayerMaskEnabled: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr || l->mask == nullptr) {
            return makeError(env, kErrBadParam, "setLayerMaskEnabled: layer/mask not found");
        }
        if (l->mask->enabled == v) {
            return makeOk(env, nullptr);
        }
        e.history.beginEdit(e.doc, "mask enable", static_cast<uint64_t>(e.docRevision));
        auto m = std::make_shared<LayerMask>(*l->mask);  // COW：快照安全
        m->enabled = v;
        l->mask = std::move(m);
        l->render = (v && l->pixels != nullptr) ? composeMasked(l->pixels, *l->mask) : nullptr;
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M5b-2：创建/释放剪贴蒙版（渲染期生效，无需派生网格重算；撤销粒度 = 单次切换）
napi_value SetLayerClipping(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "setLayerClipping(id, v) requires 2 args");
    }
    double id = 0;
    bool v = false;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_bool(env, argv[1], &v) != napi_ok) {
        return makeError(env, kErrBadParam, "setLayerClipping: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr) {
            return makeError(env, kErrBadParam, "setLayerClipping: layer not found");
        }
        if (l->clipping == v) {
            return makeOk(env, nullptr);
        }
        e.history.beginEdit(e.doc, "clipping", static_cast<uint64_t>(e.docRevision));
        l->clipping = v;
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value SetLayerVisible(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "setLayerVisible(id, v) requires 2 args");
    }
    double id = 0;
    bool v = false;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_bool(env, argv[1], &v) != napi_ok) {
        return makeError(env, kErrBadParam, "setLayerVisible: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr) {
            return makeError(env, kErrBadParam, "setLayerVisible: layer not found");
        }
        l->visible = v;
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value SetLayerOpacity(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "setLayerOpacity(id, v) requires 2 args");
    }
    double id = 0;
    double v = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_double(env, argv[1], &v) != napi_ok) {
        return makeError(env, kErrBadParam, "setLayerOpacity: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr) {
            return makeError(env, kErrBadParam, "setLayerOpacity: layer not found");
        }
        l->opacity = std::min(1.0, std::max(0.0, v));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value SetLayerBlendMode(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "setLayerBlendMode(id, mode) requires 2 args");
    }
    double id = 0;
    int32_t mode = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_int32(env, argv[1], &mode) != napi_ok) {
        return makeError(env, kErrBadParam, "setLayerBlendMode: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr) {
            return makeError(env, kErrBadParam, "setLayerBlendMode: layer not found");
        }
        l->blendMode = (mode >= 0 && mode < kBlendModeCount) ? static_cast<BlendMode>(mode) : BlendMode::Normal;
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value ReorderLayer(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "reorderLayer(id, toIndex) requires 2 args");
    }
    double id = 0;
    int32_t toIndex = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_int32(env, argv[1], &toIndex) != napi_ok) {
        return makeError(env, kErrBadParam, "reorderLayer: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        size_t at = e.doc.layers.size();
        for (size_t i = 0; i < e.doc.layers.size(); ++i) {
            if (e.doc.layers[i].id == static_cast<LayerId>(id)) {
                at = i;
                break;
            }
        }
        if (at == e.doc.layers.size()) {
            return makeError(env, kErrBadParam, "reorderLayer: layer not found");
        }
        Layer layer = e.doc.layers[at];
        e.doc.layers.erase(e.doc.layers.begin() + static_cast<long>(at));
        long dst = std::min(std::max(0L, static_cast<long>(toIndex)),
                            static_cast<long>(e.doc.layers.size()));
        e.doc.layers.insert(e.doc.layers.begin() + dst, std::move(layer));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value GetLayerListSnapshot(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    napi_value arr = nullptr;
    napi_create_array(env, &arr);
    uint32_t n = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        for (const Layer& l : e.doc.layers) {
            napi_set_element(env, arr, n++, makeLayerDto(env, l, l.id == e.doc.activeId));
        }
    }
    return makeOk(env, arr);
}

// 缩略图（02 §5：64px 经 ConvertPixelmapNativeToNapi；O3 生命周期实测项）。
// execute 在 libuv 线程对瓦片最近邻采样成 64×64 RGBA；complete 回 JS 线程建 PixelMap。
struct ThumbContext {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    LayerId layerId = 0;
    std::shared_ptr<const TileGrid> pixels;  // 锁内拷贝，锁外采样
    uint32_t layerW = 0;
    uint32_t layerH = 0;
    std::vector<uint8_t> buf;                // 64×64×4 RGBA
    bool ok = false;
};

napi_value GetLayerThumbnail(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "getLayerThumbnail(id) requires 1 arg");
    }
    double id = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "getLayerThumbnail: invalid id");
    }
    napi_value promise = nullptr;
    napi_deferred deferred = nullptr;
    napi_create_promise(env, &deferred, &promise);

    auto* ctx = new ThumbContext();
    ctx->deferred = deferred;
    ctx->layerId = static_cast<LayerId>(id);
    {
        auto& e = Engine::get();
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, ctx->layerId);
        if (l != nullptr) {
            ctx->pixels = l->effectivePixels();
        }
    }
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montageLayerThumb", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<ThumbContext*>(data);
            if (c->pixels == nullptr || c->pixels->tiles.empty() || c->pixels->cols == 0) {
                return;  // 空图层：complete 侧给占位
            }
            constexpr uint32_t kThumb = 64;
            const uint32_t lw = c->pixels->cols * kTileSize;
            const uint32_t lh = c->pixels->rows * kTileSize;
            c->layerW = lw;
            c->layerH = lh;
            c->buf.assign(static_cast<size_t>(kThumb) * kThumb * 4, 0);
            const double scale = std::min(static_cast<double>(kThumb) / lw,
                                          static_cast<double>(kThumb) / lh);
            const double dw = lw * scale;
            const double dh = lh * scale;
            const int offX = static_cast<int>((kThumb - dw) / 2);
            const int offY = static_cast<int>((kThumb - dh) / 2);
            const uint32_t stride = lw * 4;  // 瓦片拼装虚拟大图（按整图层坐标采样）
            (void)stride;
            for (uint32_t v = 0; v < kThumb; ++v) {
                for (uint32_t u = 0; u < kThumb; ++u) {
                    if (u < offX || v < offY || static_cast<double>(u) >= offX + dw ||
                        static_cast<double>(v) >= offY + dh) {
                        continue;
                    }
                    const uint32_t gx = std::min(
                        static_cast<uint32_t>((u - offX) / scale), lw - 1);
                    const uint32_t gy = std::min(
                        static_cast<uint32_t>((v - offY) / scale), lh - 1);
                    const uint32_t tIdx =
                        (gy / kTileSize) * c->pixels->cols + (gx / kTileSize);
                    auto it = c->pixels->tiles.find(tIdx);
                    if (it == c->pixels->tiles.end() || it->second == nullptr) {
                        continue;
                    }
                    const PixelSource* ps = it->second->pixels.get();
                    const uint8_t* tp = ps->mapCpu();
                    if (tp == nullptr) {
                        continue;
                    }
                    const uint32_t inTileX = gx % kTileSize;
                    const uint32_t inTileY = gy % kTileSize;
                    const uint8_t* px = tp + static_cast<size_t>(inTileY) * ps->rowBytes() +
                                        static_cast<size_t>(inTileX) * 4u;
                    uint8_t* out =
                        c->buf.data() + (static_cast<size_t>(v) * kThumb + u) * 4u;
                    out[0] = px[0];
                    out[1] = px[1];
                    out[2] = px[2];
                    out[3] = px[3];
                }
            }
            c->ok = true;
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<ThumbContext*>(data);
            napi_value d = nullptr;
            napi_create_object(env, &d);
            napi_value pmVal = nullptr;
            if (status == napi_ok && c->ok) {
                OH_Pixelmap_InitializationOptions* opts = nullptr;
                OH_PixelmapInitializationOptions_Create(&opts);
                if (opts != nullptr) {
                    OH_PixelmapInitializationOptions_SetWidth(opts, 64);
                    OH_PixelmapInitializationOptions_SetHeight(opts, 64);
                    OH_PixelmapInitializationOptions_SetPixelFormat(opts, 3 /*RGBA_8888*/);
                    OH_PixelmapInitializationOptions_SetSrcPixelFormat(opts, 3 /*RGBA_8888*/);
                }
                OH_PixelmapNative* pm = nullptr;
                const Image_ErrorCode rc = OH_PixelmapNative_CreatePixelmap(
                    const_cast<uint8_t*>(c->buf.data()), c->buf.size(), opts, &pm);
                if (opts != nullptr) {
                    OH_PixelmapInitializationOptions_Release(opts);
                }
                if (rc == IMAGE_SUCCESS && pm != nullptr) {
                    OH_PixelmapNative_ConvertPixelmapNativeToNapi(env, pm, &pmVal);
                    OH_PixelmapNative_Release(pm);
                }
            }
            napi_set_named_property(env, d, "pixelMap", pmVal);
            napi_set_named_property(env, d, "ready", makeBool(env, pmVal != nullptr));
            napi_resolve_deferred(env, c->deferred, makeOk(env, d));
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

// ---------- 笔刷（M3）/ 撤销（M4a） ----------

// argv[0..6]：diameter, hardness, opacity, red, green, blue, erasing
bool parseBrush(napi_env env, size_t argc, const napi_value* argv, BrushSettings& out) {
    if (argc < 7) {
        return false;
    }
    double d = 0, h = 0, o = 0, r = 0, g = 0, b = 0;
    bool erasing = false;
    if (napi_get_value_double(env, argv[0], &d) != napi_ok ||
        napi_get_value_double(env, argv[1], &h) != napi_ok ||
        napi_get_value_double(env, argv[2], &o) != napi_ok ||
        napi_get_value_double(env, argv[3], &r) != napi_ok ||
        napi_get_value_double(env, argv[4], &g) != napi_ok ||
        napi_get_value_double(env, argv[5], &b) != napi_ok ||
        napi_get_value_bool(env, argv[6], &erasing) != napi_ok) {
        return false;
    }
    out.diameter = static_cast<float>(d);
    out.hardness = static_cast<float>(h);
    out.opacity = static_cast<float>(o);
    out.red = static_cast<float>(r);
    out.green = static_cast<float>(g);
    out.blue = static_cast<float>(b);
    out.erasing = erasing;
    return true;
}

napi_value SetBrushSettings(napi_env env, napi_callback_info info) {
    size_t argc = 7;
    napi_value argv[7] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    BrushSettings s;
    if (!parseBrush(env, argc, argv, s)) {
        return makeError(env, kErrBadParam,
                         "setBrushSettings(d,h,o,r,g,b,erasing) requires 7 args");
    }
    {
        std::lock_guard<std::mutex> lk(Engine::get().docMutex);
        Engine::get().brush = s;
    }
    return makeOk(env, nullptr);
}

napi_value SetBrushTarget(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "setBrushTarget(mask) requires 1 arg");
    }
    bool mask = false;
    if (napi_get_value_bool(env, argv[0], &mask) != napi_ok) {
        return makeError(env, kErrBadParam, "setBrushTarget: invalid arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.brushOnMask = mask;
    }
    return makeOk(env, nullptr);
}

// M5b：添加显示全部图层蒙版（patch = 图层像素网格同维全 255）；撤销粒度 = 单次添加
napi_value AddLayerMask(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "addLayerMask(id) requires 1 arg");
    }
    double id = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "addLayerMask: invalid id");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr) {
            return makeError(env, kErrBadParam, "addLayerMask: layer not found");
        }
        if (l->mask != nullptr) {
            return makeError(env, kErrBadParam, "addLayerMask: layer already has mask");
        }
        if (l->pixels == nullptr) {
            return makeError(env, kErrBadParam, "addLayerMask: empty layer");
        }
        e.history.beginEdit(e.doc, "add mask", static_cast<uint64_t>(e.docRevision));
        auto m = std::make_shared<LayerMask>();
        const uint32_t cols = l->pixels->cols;
        const uint32_t rows = l->pixels->rows;
        TileGridBuilder builder(cols, rows);
        for (uint32_t ty = 0; ty < rows; ++ty) {
            for (uint32_t tx = 0; tx < cols; ++tx) {
                EngineBuffer& buf = builder.ensureTile(tx, ty);
                uint8_t* p = buf.mapCpuWrite();
                for (size_t i = 0; i < static_cast<size_t>(kTileSize) * kTileSize * 4u; i += 4) {
                    p[i] = p[i + 1] = p[i + 2] = 255;
                    p[i + 3] = 255;
                }
            }
        }
        m->pixels = builder.publish(1);
        m->width = cols * kTileSize;
        m->height = rows * kTileSize;
        m->offsetX = 0;
        m->offsetY = 0;
        m->outside = 255;
        m->enabled = true;
        m->linked = true;
        l->mask = std::move(m);
        l->render = composeMasked(l->pixels, *l->mask);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M7a：矩形选框（文档坐标；硬边 v1）。掩码 = 文档域网格，255 在框内。
napi_value SetRectSelection(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        return makeError(env, kErrBadParam, "setRectSelection(x, y, w, h) requires 4 args");
    }
    double x = 0, y = 0, w = 0, h = 0;
    if (napi_get_value_double(env, argv[0], &x) != napi_ok ||
        napi_get_value_double(env, argv[1], &y) != napi_ok ||
        napi_get_value_double(env, argv[2], &w) != napi_ok ||
        napi_get_value_double(env, argv[3], &h) != napi_ok) {
        return makeError(env, kErrBadParam, "setRectSelection: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "setRectSelection: no document");
        }
        if (w <= 0 || h <= 0) {
            return makeError(env, kErrBadParam, "setRectSelection: empty rect");
        }
        e.history.beginEdit(e.doc, "rect select", static_cast<uint64_t>(e.docRevision));
        const uint32_t cols = (e.doc.width + kTileSize - 1u) / kTileSize;
        const uint32_t rows = (e.doc.height + kTileSize - 1u) / kTileSize;
        TileGridBuilder builder(cols, rows);
        for (uint32_t ty = 0; ty < rows; ++ty) {
            for (uint32_t tx = 0; tx < cols; ++tx) {
                EngineBuffer& buf = builder.ensureTile(tx, ty);
                uint8_t* px = buf.mapCpuWrite();
                const int32_t y0 = static_cast<int32_t>(ty * kTileSize);
                const int32_t x0 = static_cast<int32_t>(tx * kTileSize);
                for (uint32_t r = 0; r < kTileSize; ++r) {
                    uint8_t* row = px + static_cast<size_t>(r) * kTileSize * 4u;
                    const int32_t dy = y0 + static_cast<int32_t>(r);
                    const bool inY = dy >= static_cast<int32_t>(y) &&
                                     dy < static_cast<int32_t>(y + h);
                    for (uint32_t c = 0; c < kTileSize; ++c) {
                        const int32_t dx = x0 + static_cast<int32_t>(c);
                        const uint8_t v = (inY && dx >= static_cast<int32_t>(x) &&
                                           dx < static_cast<int32_t>(x + w))
                                              ? 255
                                              : 0;
                        // 选区掩码灰度存 R 通道（约定与蒙版 patch 一致）
                        row[c * 4u] = v;
                        row[c * 4u + 1u] = v;
                        row[c * 4u + 2u] = v;
                        row[c * 4u + 3u] = 255;
                    }
                }
            }
        }
        e.doc.selection = builder.publish(1);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M7a：取消选区
napi_value ClearSelection(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.selection == nullptr) {
            return makeOk(env, nullptr);
        }
        e.history.beginEdit(e.doc, "deselect", static_cast<uint64_t>(e.docRevision));
        e.doc.selection = nullptr;
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M7c：魔棒选区（活动图层有效像素；点击文档坐标 + 容差 + 连通性；history 事务）
napi_value SetWandSelection(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        return makeError(env, kErrBadParam, "setWandSelection(x, y, tolerance, contiguous) requires 4 args");
    }
    double x = 0, y = 0, tol = 32;
    bool contiguous = true;
    if (napi_get_value_double(env, argv[0], &x) != napi_ok ||
        napi_get_value_double(env, argv[1], &y) != napi_ok ||
        napi_get_value_double(env, argv[2], &tol) != napi_ok ||
        napi_get_value_bool(env, argv[3], &contiguous) != napi_ok) {
        return makeError(env, kErrBadParam, "setWandSelection: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "setWandSelection: no document");
        }
        Layer* l = findLayerLocked(e.doc, e.doc.activeId);
        if (l == nullptr || l->effectivePixels() == nullptr) {
            return makeError(env, kErrBadParam, "setWandSelection: no pixels to sample");
        }
        auto mask = tiles::wandMask(*l->effectivePixels(), e.doc.width, e.doc.height,
                                    static_cast<int>(x), static_cast<int>(y),
                                    static_cast<int>(tol), contiguous);
        if (mask == nullptr) {
            return makeError(env, kErrBadParam, "setWandSelection: nothing matched");
        }
        e.history.beginEdit(e.doc, "magic wand", static_cast<uint64_t>(e.docRevision));
        e.doc.selection = std::move(mask);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value BeginStroke(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "beginStroke(x, y, pressure?) requires 2+ args");
    }
    double x = 0, y = 0;
    if (napi_get_value_double(env, argv[0], &x) != napi_ok ||
        napi_get_value_double(env, argv[1], &y) != napi_ok) {
        return makeError(env, kErrBadParam, "beginStroke: invalid coords");
    }
    double pressure = 1.0;
    if (argc >= 3 && napi_get_value_double(env, argv[2], &pressure) != napi_ok) {
        return makeError(env, kErrBadParam, "beginStroke: invalid pressure");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0) {
            return makeError(env, kErrNoDoc, "beginStroke: no document");
        }
        if (e.doc.activeId == 0) {
            return makeError(env, kErrNoDoc, "beginStroke: no active layer");
        }
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x4D30, "Montage.Brush",
                     "begin stroke at (%{public}f, %{public}f) layer=%{public}llu d=%{public}f erasing=%{public}d",
                     x, y, static_cast<unsigned long long>(e.doc.activeId),
                     static_cast<double>(e.brush.diameter), e.brush.erasing ? 1 : 0);
    }
    {
        // begin 标记入队（draft 创建在笔画线程按序进行；陈旧点由 draft==null 丢弃）
        std::lock_guard<std::mutex> sk(e.strokeMtx);
        e.strokeQueue.push_back({static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(pressure), true, false});
        e.ensureStrokeThreadLocked();
    }
    e.requestRender();
    return makeOk(env, nullptr);
}

napi_value ContinueStroke(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "continueStroke(x, y, pressure?) requires 2+ args");
    }
    double x = 0, y = 0;
    if (napi_get_value_double(env, argv[0], &x) != napi_ok ||
        napi_get_value_double(env, argv[1], &y) != napi_ok) {
        return makeError(env, kErrBadParam, "continueStroke: invalid coords");
    }
    double pressure = 1.0;
    if (argc >= 3 && napi_get_value_double(env, argv[2], &pressure) != napi_ok) {
        return makeError(env, kErrBadParam, "continueStroke: invalid pressure");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> sk(e.strokeMtx);
        e.strokeQueue.push_back({static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(pressure), false, false});
        e.ensureStrokeThreadLocked();
    }
    return makeOk(env, nullptr);
}

napi_value EndStroke(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> sk(e.strokeMtx);
        e.strokeQueue.push_back({0.0f, 0.0f, 1.0f, false, true});
        e.ensureStrokeThreadLocked();
    }
    return makeOk(env, nullptr);
}

napi_value Undo(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    bool performed = false;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        std::string label;
        uint64_t rev = static_cast<uint64_t>(e.docRevision);
        performed = e.history.undo(e.doc, rev, label);
        if (performed) {
            e.docRevision = static_cast<int64_t>(rev);
            e.bumpRevisionLocked();
            e.requestRender();
        }
    }
    if (performed) {
        e.historyVersion.fetch_add(1);
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "performed", makeBool(env, performed));
    return makeOk(env, data);
}

napi_value Redo(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    bool performed = false;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        std::string label;
        uint64_t rev = static_cast<uint64_t>(e.docRevision);
        performed = e.history.redo(e.doc, rev, label);
        if (performed) {
            e.docRevision = static_cast<int64_t>(rev);
            e.bumpRevisionLocked();
            e.requestRender();
        }
    }
    if (performed) {
        e.historyVersion.fetch_add(1);
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "performed", makeBool(env, performed));
    return makeOk(env, data);
}

// ---------- M0 像素测试（保留） ----------

// ---------- M0 像素测试（保留） ----------

// ---------- M0 像素测试（保留） ----------

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
    {
        std::lock_guard<std::mutex> lk(e.strokeMtx);
        e.strokeQuit = true;
        e.strokeCv.notify_all();
    }
    io::cancelImageImport();
    napi_threadsafe_function revision = nullptr;
    napi_threadsafe_function progress = nullptr;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        revision = e.revisionTsfn;
        e.revisionTsfn = nullptr;
        progress = e.progressTsfn;
        e.progressTsfn = nullptr;
    }
    if (revision != nullptr) {
        napi_release_threadsafe_function(revision, napi_tsfn_release);
    }
    if (progress != nullptr) {
        napi_release_threadsafe_function(progress, napi_tsfn_release);
    }
    e.render.stop();
    return makeOk(env, nullptr);
}

}  // namespace bridge
}  // namespace montage
