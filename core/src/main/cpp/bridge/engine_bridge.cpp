#include "bridge/engine_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>

#include <hilog/log.h>
#include <multimedia/image_framework/image/pixelmap_native.h>

#include "engine/vector_render.h"
#include "engine/vector_raster.h"
#include "engine/vector_flatten.h"
#include "engine/vector_edit.h"
#include "engine/vector_shapes.h"
#include "engine/engine.h"
#include "engine/heal_ops.h"
#include "engine/filter_ops.h"
#include "engine/group_ops.h"
#include "engine/layer_ops.h"
#include "engine/style_ops.h"
#include "engine/filters.h"
#include "engine/gradient_ops.h"
#include "engine/shape_ops.h"
#include "engine/text_ops.h"
#include "engine/view_ops.h"
#include "engine/text_raster.h"
#include "engine/transform_ops.h"
#include "engine/resample.h"
#include "engine/resample_hq.h"
#include "engine/liquify_ops.h"
#include "engine/perspective_ops.h"
#include "engine/wideangle_ops.h"
#include "engine/puppet_ops.h"
#include "engine/warp_ops.h"
#include "tiles/magic_wand.h"
#include "tiles/quick_mask.h"
#include "tiles/selection_ops.h"
#include "io/image_export.h"
#include "io/image_import.h"
#include "io/project_open.h"
#include "io/project_save.h"
#include "io/psd/psd_import.h"
#include "io/psd_export.h"
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

// M14c：几何/重采样操作后重算带样式/填充层的派生渲染（像素已重排，styledRender 陈旧）
void restyleAllLocked(Engine& e) {
    restyleDocument(e.doc);
}

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
    // 画布总像素上限（与存档 kMaxTotalPixels 对齐）：30000² 无上限可选区修改族
    // 会按全图平面分配冲到 GB 级（P1-7 源头治理）
    constexpr uint64_t kMaxNewDocPixels = 100ull * 1000 * 1000;
    if (static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kMaxNewDocPixels) {
        return makeError(env, kErrBadParam, "newDocument: size exceeds 100MP limit");
    }
    auto& e = Engine::get();
    io::cancelImageImport();
    e.resetDocument();  // 统一入口：清撤销/会话态（撤销不许跨文档）
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.doc.width = static_cast<uint32_t>(w);
        e.doc.height = static_cast<uint32_t>(h);
        e.doc.name = "Untitled";
        // M7.5：PS 语义——新建文档自带初始图层（此前零图层，首笔直接画会 no active layer）
        Layer initial;
        initial.id = e.nextLayerId();
        initial.name = "图层 " + std::to_string(initial.id);
        e.doc.layers.push_back(std::move(initial));
        e.doc.activeId = e.doc.layers.back().id;
        e.bumpRevisionLocked();
        e.requestRender();
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "newDocument %{public}dx%{public}d", w, h);
    return makeOk(env, nullptr);
}

napi_value CloseDocument(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    io::cancelImageImport();
    e.resetDocument();  // 统一入口：清撤销/会话态
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
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
        // M12 C-3：文本层字体缺失检测（回退默认字体显示旧栅格 + 提示；不崩溃不空层）
        napi_value notes = nullptr;
        napi_create_array(env, &notes);
        uint32_t nn = 0;
        {
            auto& e = Engine::get();
            std::lock_guard<std::mutex> lk(e.docMutex);
            for (const Layer& l : e.doc.layers) {
                if (l.text == nullptr || l.text->fontFamily.empty() ||
                    fontFamilyAvailable(l.text->fontFamily)) {
                    continue;
                }
                std::string note = "文本层「" + l.name + "」字体缺失：" + l.text->fontFamily +
                                   "，已回退默认字体显示（编辑时生效）";
                napi_value s = nullptr;
                napi_create_string_utf8(env, note.c_str(), NAPI_AUTO_LENGTH, &s);
                napi_set_element(env, notes, nn++, s);
            }
        }
        napi_set_named_property(env, d, "notes", notes);
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

// M16a：surface 尺寸变化唤醒（XComponent.onSurfaceChanged → 此处）。空闲停帧后 resize 的
// 消费入口——旧 60Hz 常转循环由 doFrame 的 buffer geometry 检查顺带吸收，governor 下须显式唤醒。
napi_value NotifySurfaceChanged(napi_env env, napi_callback_info /*info*/) {
    Engine::get().requestRender();
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
    bool quickMask = false;
    bool hasRaw = false;               // M15e
    io::DevelopParams devLast;         // M15e
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        vp = e.viewport;
        rev = e.docRevision;
        dw = e.doc.width;
        dh = e.doc.height;
        quickMask = e.doc.quickMask != nullptr;  // M10d：UI 镜像（撤销跨 Q 边界后刷新）
        hasRaw = e.rawDevelop.valid;             // M15e：RAW 显影会话镜像（导入线程会写，须锁内读）
        devLast = e.rawDevelop.last;
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
    napi_set_named_property(env, data, "quickMask", makeBool(env, quickMask));
    napi_set_named_property(env, data, "historyVersion",
                            makeDouble(env, static_cast<double>(e.historyVersion.load())));
    // M16a 真机基线仪表：帧调度档位 + 2s 统计窗累计（间隔采样做差即档位帧率）
    napi_set_named_property(env, data, "paceTier", makeInt32(env, snap.paceTier));
    napi_set_named_property(env, data, "fullFrames", makeInt32(env, snap.fullFrames));
    napi_set_named_property(env, data, "overlayFrames", makeInt32(env, snap.overlayFrames));
    napi_set_named_property(env, data, "idleParks",
                            makeDouble(env, static_cast<double>(snap.idleParks)));
    napi_set_named_property(env, data, "renderCallbacks",
                            makeDouble(env, static_cast<double>(snap.callbacks)));
    // M15e：RAW 显影会话镜像（菜单使能 + 面板参数回显）
    napi_set_named_property(env, data, "hasRawDevelop", makeBool(env, hasRaw));
    napi_set_named_property(env, data, "devTemp", makeDouble(env, devLast.temp));
    napi_set_named_property(env, data, "devTint", makeDouble(env, devLast.tint));
    napi_set_named_property(env, data, "devEv", makeDouble(env, devLast.ev));
    napi_set_named_property(env, data, "devGamma", makeDouble(env, devLast.gamma));
    // M17a：样式剪贴板镜像（粘贴菜单使能；copyLayerStyle 在 docMutex 内写，锁内读）
    napi_set_named_property(env, data, "hasStyleClipboard",
                            makeBool(env, e.styleClipboard != nullptr));
    // M17d：文档图案数（「定义图案」菜单使能/图案列表刷新键）
    napi_set_named_property(env, data, "patternCount", makeDouble(env,
                            static_cast<double>(e.doc.patterns.size())));
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
        // 与 notifyProgress/Dispose 同锁（import.mtx）：换绑窗口内导入线程不会读到悬垂指针
        std::lock_guard<std::mutex> lk(e.import.mtx);
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
    napi_set_named_property(env, obj, "isGroup", makeBool(env, l.isGroup));
    napi_set_named_property(env, obj, "parentId", makeDouble(env, static_cast<double>(l.parentId)));
    napi_set_named_property(env, obj, "collapsed", makeBool(env, l.collapsed));
    napi_set_named_property(env, obj, "linkId", makeDouble(env, static_cast<double>(l.linkId)));
    napi_set_named_property(env, obj, "hasStyle", makeBool(env, l.style != nullptr));
    napi_set_named_property(env, obj, "styleOn",
                            makeBool(env, l.style != nullptr && l.style->enabled));  // M17a
    napi_set_named_property(env, obj, "isFill", makeBool(env, l.fill != nullptr));
    // M18 形状层字段（非形状层给常量缺省，DTO 形状稳定）
    napi_set_named_property(env, obj, "isShape", makeBool(env, l.shape != nullptr));
    if (l.shape != nullptr) {
        const ShapeLayerState& sh = *l.shape;
        napi_set_named_property(env, obj, "shapeLiveKind", makeInt32(env, static_cast<int32_t>(sh.live.kind)));
        napi_value rectArr = nullptr;
        napi_create_array(env, &rectArr);
        for (int i = 0; i < 4; i++) {
            napi_set_element(env, rectArr, static_cast<uint32_t>(i), makeDouble(env, sh.live.rect[i]));
        }
        napi_set_named_property(env, obj, "shapeRect", rectArr);
        napi_value radArr = nullptr;
        napi_create_array(env, &radArr);
        for (int i = 0; i < 4; i++) {
            napi_set_element(env, radArr, static_cast<uint32_t>(i), makeDouble(env, sh.live.radii[i]));
        }
        napi_set_named_property(env, obj, "shapeRadii", radArr);
        napi_set_named_property(env, obj, "shapeSides", makeDouble(env, static_cast<double>(sh.live.sides)));
        napi_set_named_property(env, obj, "shapeStar", makeDouble(env, sh.live.starRatio));
        napi_value fromArr = nullptr;
        napi_create_array(env, &fromArr);
        napi_set_element(env, fromArr, 0, makeDouble(env, sh.live.from[0]));
        napi_set_element(env, fromArr, 1, makeDouble(env, sh.live.from[1]));
        napi_set_named_property(env, obj, "shapeFrom", fromArr);
        napi_value toArr = nullptr;
        napi_create_array(env, &toArr);
        napi_set_element(env, toArr, 0, makeDouble(env, sh.live.to[0]));
        napi_set_element(env, toArr, 1, makeDouble(env, sh.live.to[1]));
        napi_set_named_property(env, obj, "shapeTo", toArr);
        napi_set_named_property(env, obj, "shapeWeight", makeDouble(env, sh.live.weight));
        napi_set_named_property(env, obj, "shapeFillKind", makeInt32(env, sh.fillKind));
        char fillHex[10] = "#FF0000";
        if (sh.fillKind == 1) {
            std::snprintf(fillHex, sizeof(fillHex), "#%02X%02X%02X%02X",
                          static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, sh.fillColor[3])) * 255.0f)),
                          static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, sh.fillColor[0])) * 255.0f)),
                          static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, sh.fillColor[1])) * 255.0f)),
                          static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, sh.fillColor[2])) * 255.0f)));
        }
        napi_set_named_property(env, obj, "shapeFill", makeString(env, fillHex));
        napi_set_named_property(env, obj, "shapeStrokeOn", makeBool(env, sh.hasStroke));
        char stHex[10] = "#000000";
        if (sh.hasStroke) {
            std::snprintf(stHex, sizeof(stHex), "#%02X%02X%02X%02X",
                          static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, sh.stroke.color[3])) * 255.0f)),
                          static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, sh.stroke.color[0])) * 255.0f)),
                          static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, sh.stroke.color[1])) * 255.0f)),
                          static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, sh.stroke.color[2])) * 255.0f)));
        }
        napi_set_named_property(env, obj, "shapeStrokeColor", makeString(env, stHex));
        napi_set_named_property(env, obj, "shapeStrokeWidth",
                                makeDouble(env, sh.hasStroke ? sh.stroke.width : 3.0));
    }
    napi_set_named_property(env, obj, "hasVectorMask",
                            makeBool(env, l.vectorMask != nullptr));  // M18d
    napi_set_named_property(env, obj, "vectorMaskOn",
                            makeBool(env, l.vectorMask != nullptr && l.vectorMask->enabled));
    napi_set_named_property(env, obj, "hasMask", makeBool(env, l.mask != nullptr));
    napi_set_named_property(env, obj, "maskEnabled", makeBool(env, l.mask != nullptr && l.mask->enabled));
    napi_set_named_property(env, obj, "clipping", makeBool(env, l.clipping));
    napi_set_named_property(env, obj, "isAdjustment", makeBool(env, l.adjustment != nullptr));
    if (l.adjustment != nullptr) {
        napi_value cxArr = nullptr;
        napi_create_array(env, &cxArr);
        for (size_t ci = 0; ci < l.adjustment->curveX.size(); ++ci) {
            napi_set_element(env, cxArr, static_cast<uint32_t>(ci),
                             makeDouble(env, l.adjustment->curveX[ci]));
        }
        napi_set_named_property(env, obj, "curveX", cxArr);
        napi_value cyArr = nullptr;
        napi_create_array(env, &cyArr);
        for (size_t ci = 0; ci < l.adjustment->curveY.size(); ++ci) {
            napi_set_element(env, cyArr, static_cast<uint32_t>(ci),
                             makeDouble(env, l.adjustment->curveY[ci]));
        }
        napi_set_named_property(env, obj, "curveY", cyArr);
        napi_set_named_property(env, obj, "adjBlack", makeDouble(env, l.adjustment->inBlack));
        napi_set_named_property(env, obj, "adjWhite", makeDouble(env, l.adjustment->inWhite));
        napi_set_named_property(env, obj, "adjGamma", makeDouble(env, l.adjustment->gamma));
        napi_set_named_property(env, obj, "adjKind", makeInt32(env, static_cast<int32_t>(l.adjustment->kind)));
        napi_set_named_property(env, obj, "adjP0", makeDouble(env, l.adjustment->p0));
        napi_set_named_property(env, obj, "adjP1", makeDouble(env, l.adjustment->p1));
        napi_set_named_property(env, obj, "adjP2", makeDouble(env, l.adjustment->p2));
        napi_set_named_property(env, obj, "adjP3", makeDouble(env, l.adjustment->p3));
    }
    // M12 文本层字段（非文本层给常量缺省，DTO 形状稳定）
    napi_set_named_property(env, obj, "isText", makeBool(env, l.text != nullptr));
    napi_set_named_property(env, obj, "textContent", makeString(env, l.text ? l.text->content : ""));
    napi_set_named_property(env, obj, "textFontFamily",
                            makeString(env, l.text ? l.text->fontFamily : ""));
    napi_set_named_property(env, obj, "textFontSize", makeDouble(env, l.text ? l.text->fontSize : 0));
    napi_set_named_property(env, obj, "textLineHeight",
                            makeDouble(env, l.text ? l.text->lineHeight : 0));
    napi_set_named_property(env, obj, "textTracking", makeDouble(env, l.text ? l.text->tracking : 0));
    napi_set_named_property(env, obj, "textBold", makeBool(env, l.text && l.text->bold));
    napi_set_named_property(env, obj, "textItalic", makeBool(env, l.text && l.text->italic));
    char textHex[8] = "#000000";
    if (l.text != nullptr) {
        std::snprintf(textHex, sizeof(textHex), "#%02X%02X%02X",
                      static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, l.text->color[0])) * 255.0f)),
                      static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, l.text->color[1])) * 255.0f)),
                      static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, l.text->color[2])) * 255.0f)));
    }
    napi_set_named_property(env, obj, "textColor", makeString(env, textHex));
    napi_set_named_property(env, obj, "textAlign", makeInt32(env, l.text ? l.text->align : 0));
    napi_set_named_property(env, obj, "textWrapWidth", makeDouble(env, l.text ? l.text->wrapWidth : 0));
    napi_set_named_property(env, obj, "textX", makeDouble(env, l.text ? l.transform.originX : 0));
    napi_set_named_property(env, obj, "textY", makeDouble(env, l.text ? l.transform.originY : 0));
    napi_set_named_property(env, obj, "textW", makeDouble(env, l.text ? l.text->rasterW : 0));
    napi_set_named_property(env, obj, "textH", makeDouble(env, l.text ? l.text->rasterH : 0));
    napi_set_named_property(env, obj, "thumbToken",
                            makeDouble(env, static_cast<double>(l.pixels != nullptr ? l.pixels->revision : 0)));
    return obj;
}
}  // namespace

// M6a：添加 Levels 调整层（插到活动层之上并激活，history 事务）
napi_value AddAdjustmentLayer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t reqKind = 0;  // 默认 Levels；M9b：可指定 kind（0..7）
    if (argc >= 1 && napi_get_value_int32(env, argv[0], &reqKind) != napi_ok) {
        reqKind = 0;
    }
    if (reqKind < 0 || reqKind > 7) {
        reqKind = 0;
    }
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
        const char* kindNames[8] = {"Levels", "Curves", "亮度/对比度", "反相", "阈值",
                                    "色调分离", "色彩平衡", "色相/饱和度"};
        layer.name = kindNames[reqKind];
        auto adj = std::make_shared<LayerAdjustment>();
        adj->kind = static_cast<AdjustmentKind>(reqKind);
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
        // M14a：组级联删除（整棵子树）
        if (e.doc.layers[at].isGroup) {
            const int end = groupChildEnd(e.doc.layers, static_cast<int>(at));
            e.doc.layers.erase(e.doc.layers.begin() + static_cast<long>(at),
                               e.doc.layers.begin() + end);
        } else {
            e.doc.layers.erase(e.doc.layers.begin() + static_cast<long>(at));
        }
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
        {
            // M18：形状层底片 = shapeCache（像素层 = pixels）
            const std::shared_ptr<const TileGrid> base =
                l->shape != nullptr ? l->shapeCache : l->pixels;
            l->render = (v && base != nullptr) ? composeMasked(base, *l->mask) : nullptr;
        }
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
        // M14a 组树不变式：平铺序必须是先序 DFS（组节点紧跟完整子树）。无检查的单节点
        // 搬移可把组段打散（子层逃逸组 blend/opacity 语义），坏顺序还会原样入存档——
        // 虚拟副本上移动 + contiguousOn 终检，破坏即拒绝（照 setLayerParent 范式）。
        std::vector<Layer> tmp = e.doc.layers;
        Layer layer = tmp[at];
        tmp.erase(tmp.begin() + static_cast<long>(at));
        long dst = std::min(std::max(0L, static_cast<long>(toIndex)),
                            static_cast<long>(tmp.size()));
        tmp.insert(tmp.begin() + dst, std::move(layer));
        if (!contiguousOn(tmp)) {
            return makeError(env, kErrBadParam, "reorderLayer: move breaks group contiguity");
        }
        e.doc.layers = std::move(tmp);
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
        e.history.beginEdit(e.doc, "add mask", static_cast<uint64_t>(e.docRevision));
        auto m = std::make_shared<LayerMask>();
        // M10c：解除"蒙版需先有像素"前提——空图层蒙版网格取文档域（全 255 显示全部；
        // composeMasked 对 null 像素层输出 null，渲染路径不变）
        const uint32_t cols = l->pixels != nullptr
            ? l->pixels->cols
            : (e.doc.width + kTileSize - 1u) / kTileSize;
        const uint32_t rows = l->pixels != nullptr
            ? l->pixels->rows
            : (e.doc.height + kTileSize - 1u) / kTileSize;
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
        {
            // M18：形状层底片 = shapeCache
            const std::shared_ptr<const TileGrid> base =
                l->shape != nullptr ? l->shapeCache : l->pixels;
            l->render = base != nullptr ? composeMasked(base, *l->mask) : nullptr;
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M7a/M10a：矩形选框（文档坐标；硬边）。第 5 可选参 = 布尔 op（0 新/1 加/2 减/3 交）。
napi_value SetRectSelection(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        return makeError(env, kErrBadParam, "setRectSelection(x, y, w, h, op?) requires 4+ args");
    }
    double x = 0, y = 0, w = 0, h = 0;
    if (napi_get_value_double(env, argv[0], &x) != napi_ok ||
        napi_get_value_double(env, argv[1], &y) != napi_ok ||
        napi_get_value_double(env, argv[2], &w) != napi_ok ||
        napi_get_value_double(env, argv[3], &h) != napi_ok) {
        return makeError(env, kErrBadParam, "setRectSelection: invalid args");
    }
    int32_t op = 0;
    if (argc >= 5 && napi_get_value_int32(env, argv[4], &op) != napi_ok) {
        return makeError(env, kErrBadParam, "setRectSelection: invalid op");
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
        auto mask = tiles::rectMask(x, y, w, h, e.doc.width, e.doc.height);
        if (mask == nullptr) {
            return makeError(env, kErrBadParam, "setRectSelection: rect outside document");
        }
        auto merged = tiles::combineSelection(e.doc, std::move(mask),
                                              static_cast<tiles::SelectionOp>(op));
        e.history.beginEdit(e.doc, "rect select", static_cast<uint64_t>(e.docRevision));
        e.doc.selection = std::move(merged);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M7a/M10a：取消选区（保存到 lastSelection 供重新选择）
napi_value ClearSelection(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.selection == nullptr) {
            return makeOk(env, nullptr);
        }
        e.history.beginEdit(e.doc, "deselect", static_cast<uint64_t>(e.docRevision));
        e.lastSelection = e.doc.selection;
        e.doc.selection = nullptr;
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M7c/M10a/M10d：魔棒选区。参数 (x, y, tolerance, contiguous, op?, allLayers?, sampleSize?)。
// allLayers=true 采样源=全部可见层合成（CPU flatten）；sampleSize=N×N 均值种子（1..101 奇数）。
// ---- M11b 油漆桶：魔棒 BFS 核复用（A-3 验收 = 两命令内核对拍一致）----
// 掩码计算提取共享（SetWandSelection / BucketFill 同源）；须持 docMutex。
std::shared_ptr<const TileGrid> computeWandMaskLocked(Engine& e, int x, int y, int tol,
                                                      bool contiguous, bool allLayers,
                                                      int sampleSize, std::string& err) {
    std::unique_ptr<tiles::WandSource> src;
    if (allLayers) {
        // 采样源 = 全部可见层合成（flatten 区域版全域；v1 登记整域一次性）
        const io::FlattenJob job = io::BuildFlattenJob(e.doc, 0, 0, 0, 0);
        io::FlattenResult res = io::FlattenVisible(job, 0, 0, 0, 0);
        if (!res.ok) {
            err = "wand: flatten failed";
            return nullptr;
        }
        auto flat = std::make_shared<std::vector<uint8_t>>(std::move(res.rgba));
        src = tiles::makeFlatSource(std::move(flat), e.doc.width, e.doc.height);
    } else {
        Layer* l = findLayerLocked(e.doc, e.doc.activeId);
        if (l == nullptr || l->effectivePixels() == nullptr) {
            err = "wand: no pixels to sample";
            return nullptr;
        }
        src = tiles::makeTileSource(*l->effectivePixels());
    }
    return tiles::wandMask(*src, e.doc.width, e.doc.height, x, y, tol, contiguous, sampleSize);
}

napi_value SetWandSelection(napi_env env, napi_callback_info info) {
    size_t argc = 7;
    napi_value argv[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        return makeError(env, kErrBadParam,
                         "setWandSelection(x, y, tolerance, contiguous, op?, allLayers?, sampleSize?) requires 4+ args");
    }
    double x = 0, y = 0, tol = 32;
    bool contiguous = true;
    if (napi_get_value_double(env, argv[0], &x) != napi_ok ||
        napi_get_value_double(env, argv[1], &y) != napi_ok ||
        napi_get_value_double(env, argv[2], &tol) != napi_ok ||
        napi_get_value_bool(env, argv[3], &contiguous) != napi_ok) {
        return makeError(env, kErrBadParam, "setWandSelection: invalid args");
    }
    int32_t op = 0;
    if (argc >= 5 && napi_get_value_int32(env, argv[4], &op) != napi_ok) {
        return makeError(env, kErrBadParam, "setWandSelection: invalid op");
    }
    // allLayers = number 0/1（ArkTS 侧 number 化，napi_get_value_bool 不收 number）
    double allLayersNum = 0;
    if (argc >= 6) {
        if (napi_get_value_double(env, argv[5], &allLayersNum) != napi_ok) {
            return makeError(env, kErrBadParam, "setWandSelection: invalid allLayers");
        }
    }
    const bool allLayers = allLayersNum != 0;
    double sampleSize = 1;
    if (argc >= 7) {
        if (napi_get_value_double(env, argv[6], &sampleSize) != napi_ok) {
            return makeError(env, kErrBadParam, "setWandSelection: invalid sampleSize");
        }
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "setWandSelection: no document");
        }
        std::string maskErr;
        auto mask = computeWandMaskLocked(e, static_cast<int>(x), static_cast<int>(y),
                                          static_cast<int>(tol), contiguous, allLayers,
                                          static_cast<int>(sampleSize), maskErr);
        if (mask == nullptr) {
            return makeError(env, kErrBadParam, "setWandSelection: " + maskErr);
        }
        auto merged = tiles::combineSelection(e.doc, std::move(mask),
                                              static_cast<tiles::SelectionOp>(op));
        if (merged == nullptr && op != 0) {
            // 减/交后清空或与现有相同：仍作为一次事务落盘（PS 允许减到无选区）
            e.history.beginEdit(e.doc, "magic wand", static_cast<uint64_t>(e.docRevision));
            e.doc.selection = nullptr;
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            e.bumpRevisionLocked();
            e.requestRender();
            return makeOk(env, nullptr);
        }
        e.history.beginEdit(e.doc, "magic wand", static_cast<uint64_t>(e.docRevision));
        e.doc.selection = std::move(merged);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M10a：多边形套索选区（点列 [x0,y0,...] 文档域；偶奇扫描线；第 2 可选参 = op）
napi_value SetPolygonSelection(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "setPolygonSelection(points, op?) requires 1+ args");
    }
    // 数组判定：ArkTS 运行时的 number[] 跨桥后 napi_is_array/napi_get_array_length 均
    // 报告失败（M10a 模拟器实证），但 napi_get_element 可正常读——逐元素探测长度。
    uint32_t n = 0;
    napi_get_array_length(env, argv[0], &n);  // 报告失败时保持 0，走元素探测
    if (n == 0) {
        const uint32_t kMaxProbe = 2 * 4096 + 1;
        for (uint32_t i = 0; i < kMaxProbe; ++i) {
            napi_value v = nullptr;
            if (napi_get_element(env, argv[0], i, &v) != napi_ok || v == nullptr) {
                break;
            }
            napi_valuetype vt = napi_undefined;
            napi_typeof(env, v, &vt);
            if (vt != napi_number) {
                break;
            }
            n = i + 1;
        }
    }
    if (n < 6 || (n % 2) != 0 || n > 2 * 4096) {
        return makeError(env, kErrBadParam, "setPolygonSelection: bad point count");
    }
    std::vector<double> xy(n);
    for (uint32_t i = 0; i < n; ++i) {
        napi_value v = nullptr;
        napi_get_element(env, argv[0], i, &v);
        double d = 0;
        if (napi_get_value_double(env, v, &d) != napi_ok) {
            return makeError(env, kErrBadParam, "setPolygonSelection: bad coord");
        }
        xy[i] = d;
    }
    int32_t op = 0;
    if (argc >= 2 && napi_get_value_int32(env, argv[1], &op) != napi_ok) {
        return makeError(env, kErrBadParam, "setPolygonSelection: invalid op");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "setPolygonSelection: no document");
        }
        auto mask = tiles::polygonMask(xy, e.doc.width, e.doc.height);
        auto merged = tiles::combineSelection(e.doc, std::move(mask),
                                              static_cast<tiles::SelectionOp>(op));
        e.history.beginEdit(e.doc, "polygon select", static_cast<uint64_t>(e.docRevision));
        e.doc.selection = std::move(merged);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M10a：椭圆选框（内切于文档域矩形；2×2 覆盖抗锯齿；第 5 可选参 = op）
napi_value SetEllipseSelection(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        return makeError(env, kErrBadParam, "setEllipseSelection(x, y, w, h, op?) requires 4+ args");
    }
    double x = 0, y = 0, w = 0, h = 0;
    if (napi_get_value_double(env, argv[0], &x) != napi_ok ||
        napi_get_value_double(env, argv[1], &y) != napi_ok ||
        napi_get_value_double(env, argv[2], &w) != napi_ok ||
        napi_get_value_double(env, argv[3], &h) != napi_ok) {
        return makeError(env, kErrBadParam, "setEllipseSelection: invalid args");
    }
    int32_t op = 0;
    if (argc >= 5 && napi_get_value_int32(env, argv[4], &op) != napi_ok) {
        return makeError(env, kErrBadParam, "setEllipseSelection: invalid op");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "setEllipseSelection: no document");
        }
        auto mask = tiles::ellipseMask(x, y, w, h, e.doc.width, e.doc.height);
        auto merged = tiles::combineSelection(e.doc, std::move(mask),
                                              static_cast<tiles::SelectionOp>(op));
        e.history.beginEdit(e.doc, "ellipse select", static_cast<uint64_t>(e.docRevision));
        e.doc.selection = std::move(merged);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M10a：全选（满铺 255 蚂蚁线边界；Ctrl+A）
napi_value SelectAll(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "selectAll: no document");
        }
        e.history.beginEdit(e.doc, "select all", static_cast<uint64_t>(e.docRevision));
        e.doc.selection = tiles::fullSelection(e.doc.width, e.doc.height);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M10a：反选（无选区 = 全选；Ctrl+Shift+I）
napi_value InvertSelection(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "invertSelection: no document");
        }
        auto inv = tiles::invertSelection(e.doc);
        if (inv == nullptr && e.doc.selection == nullptr) {
            return makeError(env, kErrBadParam, "invertSelection: nothing to invert");
        }
        e.history.beginEdit(e.doc, "invert selection", static_cast<uint64_t>(e.docRevision));
        e.doc.selection = std::move(inv);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M10a：重新选择（恢复最近取消的选区；Shift+Ctrl+D）
napi_value ReselectSelection(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "reselectSelection: no document");
        }
        if (e.lastSelection == nullptr) {
            return makeError(env, kErrBadParam, "reselectSelection: no reselectable selection");
        }
        e.history.beginEdit(e.doc, "reselect", static_cast<uint64_t>(e.docRevision));
        e.doc.selection = e.lastSelection;
        e.lastSelection = nullptr;
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M10b 修改族公共实现（Replace 语义；fn 返回 null 时保留原选区并报错）
napi_value applySelectionKernel(napi_env env, const char* label,
                                std::shared_ptr<const TileGrid> (*fn)(const Document&, int),
                                int param) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "selection op: no document");
        }
        if (e.doc.selection == nullptr) {
            return makeError(env, kErrBadParam, "selection op: no active selection");
        }
        auto next = fn(e.doc, param);
        if (next == nullptr) {
            return makeError(env, kErrBadParam, "selection op: kernel produced empty result");
        }
        e.history.beginEdit(e.doc, label, static_cast<uint64_t>(e.docRevision));
        e.doc.selection = std::move(next);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value FeatherSelection(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double r = 0;
    if (argc < 1 || napi_get_value_double(env, argv[0], &r) != napi_ok || r <= 0 || r > 250) {
        return makeError(env, kErrBadParam, "featherSelection(radius 0..250) requires radius");
    }
    return applySelectionKernel(env, "feather selection",
                                [](const Document& d, int p) { return tiles::featherSelection(d, p); },
                                static_cast<int>(std::lround(r)));
}

napi_value ExpandSelection(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double r = 0;
    if (argc < 1 || napi_get_value_double(env, argv[0], &r) != napi_ok || r <= 0 || r > 100) {
        return makeError(env, kErrBadParam, "expandSelection(radius 1..100) requires radius");
    }
    return applySelectionKernel(env, "expand selection",
                                [](const Document& d, int p) { return tiles::expandSelection(d, p); },
                                static_cast<int>(r));
}

napi_value ContractSelection(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double r = 0;
    if (argc < 1 || napi_get_value_double(env, argv[0], &r) != napi_ok || r <= 0 || r > 100) {
        return makeError(env, kErrBadParam, "contractSelection(radius 1..100) requires radius");
    }
    return applySelectionKernel(env, "contract selection",
                                [](const Document& d, int p) { return tiles::contractSelection(d, p); },
                                static_cast<int>(r));
}

napi_value BorderSelection(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double r = 0;
    if (argc < 1 || napi_get_value_double(env, argv[0], &r) != napi_ok || r <= 0 || r > 100) {
        return makeError(env, kErrBadParam, "borderSelection(radius 1..100) requires radius");
    }
    return applySelectionKernel(env, "border selection",
                                [](const Document& d, int p) { return tiles::borderSelection(d, p); },
                                static_cast<int>(r));
}

napi_value SmoothSelection(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "smoothSelection: no document");
        }
        if (e.doc.selection == nullptr) {
            return makeError(env, kErrBadParam, "smoothSelection: no active selection");
        }
        auto next = tiles::smoothSelection(e.doc);
        if (next == nullptr) {
            return makeError(env, kErrBadParam, "smoothSelection: empty result");
        }
        e.history.beginEdit(e.doc, "smooth selection", static_cast<uint64_t>(e.docRevision));
        e.doc.selection = std::move(next);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// ---- M10c Alpha 通道（存储/载入选区）与剪贴板 ----

// M10d 快速蒙版 toggle（PS Q 键）：进入 = selection→quickMask（selection 置 null）；
// 退出 = quickMask→selection（全 0 → null）。撤销粒度 = 单次 toggle（快照带两网格）。
napi_value ToggleQuickMask(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "toggleQuickMask: no document");
        }
        const bool entering = e.doc.quickMask == nullptr;
        e.history.beginEdit(e.doc, "quick mask toggle", static_cast<uint64_t>(e.docRevision));
        if (entering) {
            tiles::enterQuickMask(e.doc);
        } else {
            tiles::exitQuickMask(e.doc);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "quick mask %{public}s",
                     entering ? "enter" : "exit");
        napi_value data = nullptr;
        napi_create_object(env, &data);
        napi_set_named_property(env, data, "active", makeBool(env, e.doc.quickMask != nullptr));
        napi_set_named_property(env, data, "hasSelection", makeBool(env, e.doc.selection != nullptr));
        return makeOk(env, data);
    }
}

// ---- M11a 绘图工具第一批（渐变/填充/描边；色源 = 引擎 fg/bg 现值）----

// applyGradient(layerId, type, style, x0, y0, x1, y1, target)；撤销粒度 = 单次拖线
napi_value ApplyGradient(napi_env env, napi_callback_info info) {
    size_t argc = 8;
    napi_value argv[8] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 8) {
        return makeError(env, kErrBadParam,
                         "applyGradient(layerId,type,style,x0,y0,x1,y1,target) requires 8 args");
    }
    double id = 0, type = 0, style = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, target = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_double(env, argv[1], &type) != napi_ok ||
        napi_get_value_double(env, argv[2], &style) != napi_ok ||
        napi_get_value_double(env, argv[3], &x0) != napi_ok ||
        napi_get_value_double(env, argv[4], &y0) != napi_ok ||
        napi_get_value_double(env, argv[5], &x1) != napi_ok ||
        napi_get_value_double(env, argv[6], &y1) != napi_ok ||
        napi_get_value_double(env, argv[7], &target) != napi_ok) {
        return makeError(env, kErrBadParam, "applyGradient: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "applyGradient: no document");
        }
        GradientSpec spec;
        spec.type = static_cast<GradientType>(static_cast<int32_t>(type));
        spec.style = static_cast<GradientStyle>(static_cast<int32_t>(style));
        spec.x0 = x0;
        spec.y0 = y0;
        spec.x1 = x1;
        spec.y1 = y1;
        const auto gt = static_cast<GradientTarget>(static_cast<int32_t>(target));
        std::string err;
        e.history.beginEdit(e.doc, "gradient", static_cast<uint64_t>(e.docRevision));
        const bool ok = applyGradient(e.doc, static_cast<LayerId>(id), spec, gt,
                                      e.fgColor, e.bgColor, err);
        if (!ok) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, err);
        }
        restyleAllLocked(e);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// fillSelection(source 0=前景/1=背景/2=黑/3=白)
napi_value FillSelection(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double source = 0;
    if (argc < 1 || napi_get_value_double(env, argv[0], &source) != napi_ok) {
        return makeError(env, kErrBadParam, "fillSelection(source) requires 1 arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "fillSelection: no document");
        }
        std::string err;
        e.history.beginEdit(e.doc, "fill", static_cast<uint64_t>(e.docRevision));
        const bool ok = fillWithColor(e.doc, e.doc.activeId,
                                      static_cast<FillSource>(static_cast<int32_t>(source)),
                                      e.fgColor[0], e.fgColor[1], e.fgColor[2],
                                      e.bgColor[0], e.bgColor[1], e.bgColor[2], err);
        if (!ok) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, err);
        }
        restyleAllLocked(e);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// strokeSelection(width 1..100，中心位置语义，前景色)
napi_value StrokeSelection(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double width = 2;
    if (argc < 1 || napi_get_value_double(env, argv[0], &width) != napi_ok) {
        return makeError(env, kErrBadParam, "strokeSelection(width) requires 1 arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "strokeSelection: no document");
        }
        std::string err;
        e.history.beginEdit(e.doc, "stroke", static_cast<uint64_t>(e.docRevision));
        const bool ok = strokeSelectionBorder(e.doc, e.doc.activeId,
                                              static_cast<int>(width),
                                              e.fgColor[0], e.fgColor[1], e.fgColor[2], err);
        if (!ok) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, err);
        }
        restyleAllLocked(e);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// 油漆桶 bucketFill(x, y, tolerance, contiguous, allLayers, sampleSize)：
// 魔棒核算命中域 → ∩ 现有选区（PS 油漆桶受活动选区约束）→ 临时选区填充前景色后还原。
napi_value BucketFill(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        return makeError(env, kErrBadParam,
                         "bucketFill(x, y, tolerance, contiguous, allLayers?, sampleSize?) requires 4+ args");
    }
    double x = 0, y = 0, tol = 32, allLayersNum = 0, sampleSize = 1;
    bool contiguous = true;
    if (napi_get_value_double(env, argv[0], &x) != napi_ok ||
        napi_get_value_double(env, argv[1], &y) != napi_ok ||
        napi_get_value_double(env, argv[2], &tol) != napi_ok ||
        napi_get_value_bool(env, argv[3], &contiguous) != napi_ok) {
        return makeError(env, kErrBadParam, "bucketFill: invalid args");
    }
    if (argc >= 5 && napi_get_value_double(env, argv[4], &allLayersNum) != napi_ok) {
        return makeError(env, kErrBadParam, "bucketFill: invalid allLayers");
    }
    if (argc >= 6 && napi_get_value_double(env, argv[5], &sampleSize) != napi_ok) {
        return makeError(env, kErrBadParam, "bucketFill: invalid sampleSize");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lg(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "bucketFill: no document");
        }
        std::string err;
        auto mask = computeWandMaskLocked(e, static_cast<int>(x), static_cast<int>(y),
                                          static_cast<int>(tol), contiguous,
                                          allLayersNum != 0, static_cast<int>(sampleSize), err);
        if (mask == nullptr) {
            return makeError(env, kErrBadParam, "bucketFill: " + err);
        }
        // 活动选区约束（有选区时油漆桶只填交集——PS 语义）
        std::shared_ptr<const TileGrid> saved = e.doc.selection;
        if (saved != nullptr) {
            mask = tiles::combineSelection(e.doc, mask, tiles::SelectionOp::Intersect);
            if (mask == nullptr) {
                return makeError(env, kErrBadParam, "bucketFill: empty intersection");
            }
        }
        e.history.beginEdit(e.doc, "bucket fill", static_cast<uint64_t>(e.docRevision));
        e.doc.selection = std::move(mask);
        const bool ok = fillWithColor(e.doc, e.doc.activeId, FillSource::Foreground,
                                      e.fgColor[0], e.fgColor[1], e.fgColor[2],
                                      e.bgColor[0], e.bgColor[1], e.bgColor[2], err);
        e.doc.selection = saved;
        if (!ok) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "bucketFill: " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M11c 形状栅格化（kind 0=矩形/1=圆角矩形/2=椭圆/3=直线；fill/stroke 用 0/1 number——
// napi_get_value_bool 不收 number 的跨桥坑；撤销粒度 = 单次拖拽）
napi_value RasterizeShape(napi_env env, napi_callback_info info) {
    size_t argc = 10;
    napi_value argv[10] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 10) {
        return makeError(env, kErrBadParam,
                         "rasterizeShape(layerId,kind,x0,y0,x1,y1,cornerRadius,fill,stroke,strokeWidth) requires 10 args");
    }
    double v[10] = {0};
    for (size_t i = 0; i < 10; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "rasterizeShape: invalid args");
        }
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "rasterizeShape: no document");
        }
        ShapeSpec spec;
        spec.kind = static_cast<ShapeSpec::Kind>(static_cast<int32_t>(v[1]));
        spec.x0 = v[2];
        spec.y0 = v[3];
        spec.x1 = v[4];
        spec.y1 = v[5];
        spec.cornerRadius = v[6];
        spec.fill = v[7] != 0;
        spec.stroke = v[8] != 0;
        spec.strokeWidth = v[9];
        std::string err;
        e.history.beginEdit(e.doc, "shape", static_cast<uint64_t>(e.docRevision));
        const bool ok = rasterizeShape(e.doc, e.doc.activeId, spec, e.fgColor, e.bgColor, err);
        if (!ok) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, err);
        }
        restyleAllLocked(e);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// ---- M11d 仿制图章 ----

// Alt+点击取源：记录源点 = 当前活动层（v1 采样源层原像素）
napi_value SetCloneSource(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "setCloneSource(x, y) requires 2 args");
    }
    double x = 0, y = 0;
    if (napi_get_value_double(env, argv[0], &x) != napi_ok ||
        napi_get_value_double(env, argv[1], &y) != napi_ok) {
        return makeError(env, kErrBadParam, "setCloneSource: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "setCloneSource: no document");
        }
        if (e.doc.activeId == 0) {
            return makeError(env, kErrBadParam, "setCloneSource: no active layer");
        }
        e.cloneSource.active = true;
        e.cloneSource.layerId = e.doc.activeId;
        e.cloneSource.srcX = x;
        e.cloneSource.srcY = y;
        e.cloneSource.hasOffset = false;  // 新源点：对齐偏移重新锁定
        e.cloneSource.lockOffsetX = 0;
        e.cloneSource.lockOffsetY = 0;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "clone source set (%.0f, %.0f)", x, y);
    return makeOk(env, nullptr);
}

napi_value SetCloneAligned(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "setCloneAligned(aligned) requires 1 arg");
    }
    bool aligned = true;
    if (napi_get_value_bool(env, argv[0], &aligned) != napi_ok) {
        return makeError(env, kErrBadParam, "setCloneAligned: invalid arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.cloneSource.aligned = aligned;
        e.cloneSource.hasOffset = false;  // 切换模式：偏移重锁
    }
    return makeOk(env, nullptr);
}

napi_value SetBrushClone(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "setBrushClone(active) requires 1 arg");
    }
    bool active = false;
    if (napi_get_value_bool(env, argv[0], &active) != napi_ok) {
        return makeError(env, kErrBadParam, "setBrushClone: invalid arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.brushIsClone = active;
    }
    return makeOk(env, nullptr);
}

// M11e：效果工具切换（mode 对齐 BrushEffect；非 0 时与克隆/蒙版目标互斥由 UI 侧保证，
// 引擎在 begin 处再兜底降级）
napi_value SetBrushEffect(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "setBrushEffect(mode) requires 1 arg");
    }
    double mode = 0;
    if (napi_get_value_double(env, argv[0], &mode) != napi_ok) {
        return makeError(env, kErrBadParam, "setBrushEffect: invalid arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        const int m = static_cast<int>(mode);
        e.brushEffect = (m >= 0 && m <= 6) ? static_cast<BrushEffect>(m) : BrushEffect::None;
        if (e.brushEffect != BrushEffect::None) {
            e.brushIsClone = false;  // 效果与克隆互斥（工具切换互斥的引擎侧兜底）
        }
    }
    return makeOk(env, nullptr);
}

// M19a：修复族工具切换（kind 对齐 HealKind：0 无/1 修复画笔/2-4 污点三模式；非 0 时与
// 克隆/效果/历史画笔互斥——工具切换互斥的引擎侧兜底）
napi_value SetBrushHeal(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "setBrushHeal(kind) requires 1 arg");
    }
    double kind = 0;
    if (napi_get_value_double(env, argv[0], &kind) != napi_ok) {
        return makeError(env, kErrBadParam, "setBrushHeal: invalid arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        const int k = static_cast<int>(kind);
        e.brushHealKind = (k >= 0 && k <= 4) ? k : 0;
        if (e.brushHealKind != 0) {
            e.brushIsClone = false;
            e.brushEffect = BrushEffect::None;
            e.historyBrushSteps = 0;
        }
    }
    return makeOk(env, nullptr);
}

// M19a：修补工具（选区 = 目标，(dx,dy) 平移取源，泊松融入选区；一步撤销事务）
napi_value PatchSelection(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "patchSelection(dx, dy) requires 2 args");
    }
    double dx = 0, dy = 0;
    if (napi_get_value_double(env, argv[0], &dx) != napi_ok ||
        napi_get_value_double(env, argv[1], &dy) != napi_ok) {
        return makeError(env, kErrBadParam, "patchSelection: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lg(e.docMutex);
        if (e.doc.width == 0) {
            return makeError(env, kErrNoDoc, "patchSelection: no document");
        }
        e.history.beginEdit(e.doc, "patch", static_cast<uint64_t>(e.docRevision));
        std::string err;
        const bool ok = patchSelection(e.doc, static_cast<int>(std::lround(dx)),
                                       static_cast<int>(std::lround(dy)), err);
        if (!ok) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "patchSelection: " + err);
        }
        restyleDocument(e.doc);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    e.historyVersion.fetch_add(1);
    return makeOk(env, nullptr);
}

// M19b：内容感知填充/移动——异步 Promise（PatchMatch 秒级，同步会 ANR；
// DocGeomAsync 同范式：锁内快照 → 锁外求解 → 发布段 docEpoch+docRevision 复核）
struct CaOpCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    Document docSnap;
    uint64_t epoch = 0;
    int64_t revision = 0;
    int op = 0;  // 0=fill 1=move
    int dx = 0, dy = 0;
    bool extend = false;
    bool changed = false;
    std::string err;
};

napi_value CaOpAsync(napi_env env, napi_callback_info info, int op) {
    auto& e = Engine::get();
    auto* ctx = new CaOpCtx();
    ctx->op = op;
    if (op == 1) {
        size_t argc = 3;
        napi_value argv[3] = {nullptr, nullptr, nullptr};
        napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
        if (argc < 2) {
            delete ctx;
            return makeError(env, kErrBadParam, "contentAwareMove(dx, dy, extend?) requires 2+ args");
        }
        double dx = 0, dy = 0, extendNum = 0;
        if (napi_get_value_double(env, argv[0], &dx) != napi_ok ||
            napi_get_value_double(env, argv[1], &dy) != napi_ok) {
            delete ctx;
            return makeError(env, kErrBadParam, "contentAwareMove: invalid args");
        }
        if (argc >= 3 && napi_get_value_double(env, argv[2], &extendNum) != napi_ok) {
            delete ctx;
            return makeError(env, kErrBadParam, "contentAwareMove: invalid extend");
        }
        ctx->dx = static_cast<int>(std::lround(dx));
        ctx->dy = static_cast<int>(std::lround(dy));
        ctx->extend = extendNum != 0;
    }
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0) {
            delete ctx;
            return makeError(env, kErrNoDoc, op == 0 ? "contentAwareFill: no document"
                                                     : "contentAwareMove: no document");
        }
        ctx->docSnap = e.doc;
        ctx->epoch = e.docEpoch;
        ctx->revision = e.docRevision;
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montageCaOp", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<CaOpCtx*>(data);
            Document work = c->docSnap;
            if (c->op == 0) {
                c->changed = contentAwareFill(work, c->err);
            } else {
                c->changed = contentAwareMove(work, c->dx, c->dy, c->extend, c->err);
            }
            if (c->changed) {
                restyleDocument(work);
            }
            auto& eg = Engine::get();
            std::lock_guard<std::mutex> lk(eg.docMutex);
            if (c->changed && eg.docEpoch == c->epoch && eg.docRevision == c->revision) {
                eg.history.beginEdit(eg.doc, c->op == 0 ? "content-aware fill" : "content-aware move",
                                     static_cast<uint64_t>(eg.docRevision));
                eg.doc = std::move(work);
                eg.history.endEdit(eg.doc, static_cast<uint64_t>(eg.docRevision));
                eg.bumpRevisionLocked();
                eg.requestRender();
            } else {
                c->changed = false;
            }
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<CaOpCtx*>(data);
            auto& eg = Engine::get();
            if (status == napi_ok && c->changed) {
                eg.historyVersion.fetch_add(1);
            }
            napi_value dataObj = nullptr;
            napi_create_object(env, &dataObj);
            napi_set_named_property(env, dataObj, "changed",
                                    makeBool(env, status == napi_ok && c->changed));
            napi_resolve_deferred(env, c->deferred, makeOk(env, dataObj));
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

// ---------- M20b 液化（photocraft liquify.rs 命令面）：一笔一步异步事务（CaOp 同范式：
// 锁内快照 → 锁外重放笔画+渲染 → 发布段 epoch/revision 复核）。笔画=纯数据
//（工具/笔刷/点列 doc 坐标），TS 侧已转层局部坐标。 ----------
struct LiquifyCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    Document docSnap;
    uint64_t epoch = 0;
    int64_t revision = 0;
    LayerId layerId = 0;
    std::vector<liquify::Stroke> strokes;
    bool changed = false;
    std::string err;
};

// napi strokes 数组 → liquify::Stroke
bool parseLiquifyStrokes(napi_env env, napi_value arr, std::vector<liquify::Stroke>& out) {
    bool isArr = false;
    if (napi_is_array(env, arr, &isArr) != napi_ok || !isArr) {
        return false;
    }
    uint32_t n = 0;
    napi_get_array_length(env, arr, &n);
    out.reserve(n);
    for (uint32_t k = 0; k < n; ++k) {
        napi_value obj = nullptr;
        if (napi_get_element(env, arr, k, &obj) != napi_ok) {
            return false;
        }
        liquify::Stroke s;
        auto num = [&](const char* key, double& v) -> bool {
            napi_value f = nullptr;
            if (napi_get_named_property(env, obj, key, &f) != napi_ok || f == nullptr) {
                return false;
            }
            return napi_get_value_double(env, f, &v) == napi_ok;
        };
        double tool = 0, size = 0, density = 50, pressure = 100, rate = 80;
        if (!num("tool", tool) || !num("size", size)) {
            return false;
        }
        if (!num("density", density) || !num("pressure", pressure) || !num("rate", rate)) {
            return false;
        }
        s.tool = static_cast<liquify::Tool>(static_cast<int32_t>(tool));
        s.size = size;
        s.density = density;
        s.pressure = pressure;
        s.rate = rate;
        napi_value pts = nullptr;
        double amount = 0;
        if (napi_get_named_property(env, obj, "amount", &pts) == napi_ok && pts != nullptr) {
            if (napi_get_value_double(env, pts, &amount) == napi_ok) {
                s.hasAmount = true;
                s.amount = amount;
            }
        }
        pts = nullptr;
        if (napi_get_named_property(env, obj, "points", &pts) != napi_ok || pts == nullptr) {
            return false;
        }
        bool ptsArr = false;
        if (napi_is_array(env, pts, &ptsArr) != napi_ok || !ptsArr) {
            return false;
        }
        uint32_t pn = 0;
        napi_get_array_length(env, pts, &pn);
        for (uint32_t i = 0; i < pn; ++i) {
            napi_value pt = nullptr;
            if (napi_get_element(env, pts, i, &pt) != napi_ok) {
                return false;
            }
            bool ptArr = false;
            if (napi_is_array(env, pt, &ptArr) != napi_ok || !ptArr) {
                return false;
            }
            uint32_t dn = 0;
            napi_get_array_length(env, pt, &dn);
            if (dn < 2) {
                return false;
            }
            double x = 0, y = 0, pr = 1.0;
            napi_value xv = nullptr, yv = nullptr, pv = nullptr;
            if (napi_get_element(env, pt, 0, &xv) != napi_ok ||
                napi_get_element(env, pt, 1, &yv) != napi_ok ||
                napi_get_value_double(env, xv, &x) != napi_ok ||
                napi_get_value_double(env, yv, &y) != napi_ok) {
                return false;
            }
            if (dn >= 3 && napi_get_element(env, pt, 2, &pv) == napi_ok) {
                napi_get_value_double(env, pv, &pr);
            }
            s.points.push_back({x, y, pr});
        }
        if (!s.points.empty()) {
            out.push_back(std::move(s));
        }
    }
    return true;
}

napi_value ApplyLiquify(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "applyLiquify(layerId, strokes) requires 2 args");
    }
    double layerId = 0;
    if (napi_get_value_double(env, argv[0], &layerId) != napi_ok || layerId < 1) {
        return makeError(env, kErrBadParam, "applyLiquify: invalid layerId");
    }
    auto* ctx = new LiquifyCtx();
    ctx->layerId = static_cast<LayerId>(layerId);
    if (!parseLiquifyStrokes(env, argv[1], ctx->strokes) || ctx->strokes.empty()) {
        delete ctx;
        return makeError(env, kErrBadParam, "applyLiquify: invalid strokes");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0) {
            delete ctx;
            return makeError(env, kErrNoDoc, "applyLiquify: no document");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            delete ctx;
            return makeError(env, kErrBadParam, "applyLiquify: edit/preview session open");
        }
        ctx->docSnap = e.doc;
        ctx->epoch = e.docEpoch;
        ctx->revision = e.docRevision;
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montageLiquify", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<LiquifyCtx*>(data);
            Document work = c->docSnap;
            Layer* layer = nullptr;
            for (Layer& l : work.layers) {
                if (l.id == c->layerId) {
                    layer = &l;
                    break;
                }
            }
            if (layer == nullptr || layer->pixels == nullptr) {
                c->err = "no raster layer";
                return;
            }
            // 区域化（M20b-2 交互路径）：场界=笔画 bbox 外扩（笔刷半径+余量），钳到网格。
            // 只转换/渲染区域行（调试构建整网格 f32 每笔数十秒的教训），触区瓦片 COW。
            const int gw = static_cast<int>(layer->pixels->cols * kTileSize);
            const int gh = static_cast<int>(layer->pixels->rows * kTileSize);
            double pad = 16.0;
            for (const liquify::Stroke& s : c->strokes) {
                pad = std::max(pad, s.size * 0.5 + 16.0);
            }
            double bx0 = 1e308, by0 = 1e308, bx1 = -1e308, by1 = -1e308;
            for (const liquify::Stroke& s : c->strokes) {
                for (const std::array<double, 3>& pt : s.points) {
                    bx0 = std::min(bx0, pt[0]);
                    by0 = std::min(by0, pt[1]);
                    bx1 = std::max(bx1, pt[0]);
                    by1 = std::max(by1, pt[1]);
                }
            }
            RectI region;
            region.x0 = std::max(0, static_cast<int>(std::floor(bx0 - pad)));
            region.y0 = std::max(0, static_cast<int>(std::floor(by0 - pad)));
            region.x1 = std::min(gw, static_cast<int>(std::ceil(bx1 + pad)));
            region.y1 = std::min(gh, static_cast<int>(std::ceil(by1 + pad)));
            if (region.empty()) {
                return;
            }
            const RectI fieldBounds{0, 0, region.width(), region.height()};
            liquify::Field field(fieldBounds, liquify::autoCell(fieldBounds));
            for (liquify::Stroke s : c->strokes) {
                for (std::array<double, 3>& pt : s.points) {
                    pt[0] -= layer->transform.originX + region.x0;
                    pt[1] -= layer->transform.originY + region.y0;
                }
                field.applyStroke(s);
            }
            if (field.isIdentity()) {
                return;  // 零效果笔画（零压等）：不出事务
            }
            auto warped = applyLiquifyGridRegion(*layer->pixels, region, field);
            if (warped == nullptr) {
                c->err = "liquify render failed";
                return;
            }
            layer->pixels = warped;
            if (layer->mask != nullptr && layer->mask->enabled) {
                layer->render = composeMasked(layer->pixels, *layer->mask);
            }
            c->changed = true;
            restyleDocument(work);
            auto& eg = Engine::get();
            std::lock_guard<std::mutex> lk(eg.docMutex);
            if (eg.docEpoch == c->epoch && eg.docRevision == c->revision) {
                eg.history.beginEdit(eg.doc, "æ¶²å", static_cast<uint64_t>(eg.docRevision));  // 液化
                eg.doc = std::move(work);
                eg.history.endEdit(eg.doc, static_cast<uint64_t>(eg.docRevision));
                eg.bumpRevisionLocked();
                eg.requestRender();
            } else {
                c->changed = false;
                c->err = "doc changed during liquify";
            }
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<LiquifyCtx*>(data);
            auto& eg = Engine::get();
            if (status == napi_ok && c->changed) {
                eg.historyVersion.fetch_add(1);
            }
            napi_value dataObj = nullptr;
            napi_create_object(env, &dataObj);
            napi_set_named_property(env, dataObj, "changed",
                                    makeBool(env, status == napi_ok && c->changed));
            napi_resolve_deferred(env, c->deferred, makeOk(env, dataObj));
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

// ---------- M20c 操控变形命令面：钉集一步异步事务（CaOp 同范式）。钉 doc 坐标，
// 引擎侧转层局部；网格重定由 applyPuppetGrid 的 outOrigin 回写层 origin。 ----------
struct PuppetCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    Document docSnap;
    uint64_t epoch = 0;
    int64_t revision = 0;
    LayerId layerId = 0;
    puppet::Warp warp;
    bool changed = false;
    std::string err;
};

bool parsePuppetPins(napi_env env, napi_value arr, puppet::Warp& w) {
    bool isArr = false;
    if (napi_is_array(env, arr, &isArr) != napi_ok || !isArr) {
        return false;
    }
    uint32_t n = 0;
    napi_get_array_length(env, arr, &n);
    w.pins.reserve(n);
    for (uint32_t k = 0; k < n; ++k) {
        napi_value obj = nullptr;
        if (napi_get_element(env, arr, k, &obj) != napi_ok) {
            return false;
        }
        puppet::Pin p;
        auto num = [&](const char* key, double& v) -> bool {
            napi_value f = nullptr;
            if (napi_get_named_property(env, obj, key, &f) != napi_ok || f == nullptr) {
                return false;
            }
            return napi_get_value_double(env, f, &v) == napi_ok;
        };
        double sx = 0, sy = 0, dx = 0, dy = 0;
        if (!num("sx", sx) || !num("sy", sy) || !num("dx", dx) || !num("dy", dy)) {
            return false;
        }
        p.src[0] = sx;
        p.src[1] = sy;
        p.dst[0] = dx;
        p.dst[1] = dy;
        double rot = -1;
        if (num("rotate", rot) && rot >= 0.0) {
            p.hasRotate = true;
            p.rotateDeg = rot;
        }
        double depth = 0;
        if (num("depth", depth)) {
            p.depth = static_cast<int>(depth);
        }
        w.pins.push_back(p);
    }
    return true;
}

napi_value ApplyPuppet(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "applyPuppet(layerId, pins, mode?, density?) requires 2+ args");
    }
    double layerId = 0;
    if (napi_get_value_double(env, argv[0], &layerId) != napi_ok || layerId < 1) {
        return makeError(env, kErrBadParam, "applyPuppet: invalid layerId");
    }
    auto* ctx = new PuppetCtx();
    ctx->layerId = static_cast<LayerId>(layerId);
    if (!parsePuppetPins(env, argv[1], ctx->warp) || ctx->warp.pins.empty()) {
        delete ctx;
        return makeError(env, kErrBadParam, "applyPuppet: invalid pins");
    }
    if (argc >= 3) {
        double mode = 1;
        if (napi_get_value_double(env, argv[2], &mode) == napi_ok) {
            ctx->warp.mode = static_cast<puppet::Mode>(static_cast<int32_t>(mode));
        }
    }
    if (argc >= 4) {
        double density = 1;
        if (napi_get_value_double(env, argv[3], &density) == napi_ok) {
            ctx->warp.density = static_cast<puppet::Density>(static_cast<int32_t>(density));
        }
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0) {
            delete ctx;
            return makeError(env, kErrNoDoc, "applyPuppet: no document");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            delete ctx;
            return makeError(env, kErrBadParam, "applyPuppet: edit/preview session open");
        }
        ctx->docSnap = e.doc;
        ctx->epoch = e.docEpoch;
        ctx->revision = e.docRevision;
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montagePuppet", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<PuppetCtx*>(data);
            Document work = c->docSnap;
            Layer* layer = nullptr;
            for (Layer& l : work.layers) {
                if (l.id == c->layerId) {
                    layer = &l;
                    break;
                }
            }
            if (layer == nullptr || layer->pixels == nullptr) {
                c->err = "no raster layer";
                return;
            }
            const RectI bounds{0, 0, static_cast<int>(layer->pixels->cols * kTileSize),
                               static_cast<int>(layer->pixels->rows * kTileSize)};
            // 钉 doc → 层局部
            for (puppet::Pin& p : c->warp.pins) {
                p.src[0] -= layer->transform.originX;
                p.src[1] -= layer->transform.originY;
                p.dst[0] -= layer->transform.originX;
                p.dst[1] -= layer->transform.originY;
            }
            double origin[2] = {0.0, 0.0};
            auto warped = applyPuppetGrid(*layer->pixels, bounds, c->warp, origin);
            if (warped == nullptr) {
                c->err = "puppet refused (span limit / empty mesh)";
                return;
            }
            // 恒等快路径：网格尺寸与 origin 未变且像素 revision 相同则不出事务
            if (warped->cols == layer->pixels->cols && warped->rows == layer->pixels->rows &&
                std::fabs(origin[0]) < 1e-9 && std::fabs(origin[1]) < 1e-9 &&
                puppet::warpIsIdentity(c->warp)) {
                return;
            }
            layer->pixels = warped;
            layer->transform.originX += origin[0];
            layer->transform.originY += origin[1];
            if (layer->mask != nullptr && layer->mask->enabled) {
                layer->render = composeMasked(layer->pixels, *layer->mask);
            }
            c->changed = true;
            restyleDocument(work);
            auto& eg = Engine::get();
            std::lock_guard<std::mutex> lk(eg.docMutex);
            if (eg.docEpoch == c->epoch && eg.docRevision == c->revision) {
                eg.history.beginEdit(eg.doc, "\xe6\x93\x8d\xe6\x8e\xa7\xe5\x8f\x98\xe5\xbd\xa2",
                                     static_cast<uint64_t>(eg.docRevision));  // 操控变形
                eg.doc = std::move(work);
                eg.history.endEdit(eg.doc, static_cast<uint64_t>(eg.docRevision));
                eg.bumpRevisionLocked();
                eg.requestRender();
            } else {
                c->changed = false;
                c->err = "doc changed during puppet";
            }
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<PuppetCtx*>(data);
            auto& eg = Engine::get();
            if (status == napi_ok && c->changed) {
                eg.historyVersion.fetch_add(1);
            }
            napi_value dataObj = nullptr;
            napi_create_object(env, &dataObj);
            napi_set_named_property(env, dataObj, "changed",
                                    makeBool(env, status == napi_ok && c->changed));
            napi_resolve_deferred(env, c->deferred, makeOk(env, dataObj));
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

// ---------- M20d 透视变形命令面：单平面 src/dst 四角一步异步事务（CaOp 同范式）。
// 渲染=warp_ops warpMeshImg 以 PerspectiveMap::map（doc→doc 分段连续）为正向映射，
// 4px 网格细分+双三次+收缩预滤波。输出网格重定（origin=渲染 bbox）。 ----------
struct PerspectiveCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    Document docSnap;
    uint64_t epoch = 0;
    int64_t revision = 0;
    LayerId layerId = 0;
    perspective::Plane plane;
    bool changed = false;
    std::string err;
};

napi_value ApplyPerspective(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "applyPerspective(layerId, quad8) requires 2 args");
    }
    double layerId = 0;
    if (napi_get_value_double(env, argv[0], &layerId) != napi_ok || layerId < 1) {
        return makeError(env, kErrBadParam, "applyPerspective: invalid layerId");
    }
    bool isArr = false;
    if (napi_is_array(env, argv[1], &isArr) != napi_ok || !isArr) {
        return makeError(env, kErrBadParam, "applyPerspective: quad must be array");
    }
    uint32_t n = 0;
    napi_get_array_length(env, argv[1], &n);
    if (n != 16) {
        return makeError(env, kErrBadParam, "applyPerspective: quad8 = [src4*2, dst4*2]");
    }
    auto* ctx = new PerspectiveCtx();
    ctx->layerId = static_cast<LayerId>(layerId);
    for (uint32_t i = 0; i < 8; ++i) {
        napi_value v = nullptr;
        double d = 0;
        if (napi_get_element(env, argv[1], i, &v) != napi_ok ||
            napi_get_value_double(env, v, &d) != napi_ok) {
            delete ctx;
            return makeError(env, kErrBadParam, "applyPerspective: bad quad");
        }
        ctx->plane.src[i / 2][i % 2] = d;
    }
    for (uint32_t i = 0; i < 8; ++i) {
        napi_value v = nullptr;
        double d = 0;
        if (napi_get_element(env, argv[1], 8 + i, &v) != napi_ok ||
            napi_get_value_double(env, v, &d) != napi_ok) {
            delete ctx;
            return makeError(env, kErrBadParam, "applyPerspective: bad quad");
        }
        ctx->plane.dst[i / 2][i % 2] = d;
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0) {
            delete ctx;
            return makeError(env, kErrNoDoc, "applyPerspective: no document");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            delete ctx;
            return makeError(env, kErrBadParam, "applyPerspective: edit/preview session open");
        }
        ctx->docSnap = e.doc;
        ctx->epoch = e.docEpoch;
        ctx->revision = e.docRevision;
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montagePerspective", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<PerspectiveCtx*>(data);
            Document work = c->docSnap;
            Layer* layer = nullptr;
            for (Layer& l : work.layers) {
                if (l.id == c->layerId) {
                    layer = &l;
                    break;
                }
            }
            if (layer == nullptr || layer->pixels == nullptr) {
                c->err = "no raster layer";
                return;
            }
            uint32_t bx = 0, by = 0, bw = 0, bh = 0;
            if (!layerContentBBox(*layer->pixels, bx, by, bw, bh)) {
                c->err = "empty layer";
                return;
            }
            // 内容 doc 域 bbox
            const RectI content{static_cast<int>(layer->transform.originX + bx),
                                static_cast<int>(layer->transform.originY + by),
                                static_cast<int>(layer->transform.originX + bx + bw),
                                static_cast<int>(layer->transform.originY + by + bh)};
            // 平面 doc → 局部（网格坐标）
            perspective::Plane local = c->plane;
            for (int k = 0; k < 4; ++k) {
                local.src[k][0] -= layer->transform.originX;
                local.src[k][1] -= layer->transform.originY;
                local.dst[k][0] -= layer->transform.originX;
                local.dst[k][1] -= layer->transform.originY;
            }
            perspective::PerspectiveMap map;
            if (!map.build({local})) {
                c->err = "degenerate quad";
                return;
            }
            if (perspective::PerspectiveMap::isIdentity({local})) {
                return;  // 无变形不出事务
            }
            // 读内容区为 f32
            warp::Img src;
            src.w = content.width();
            src.h = content.height();
            src.px.resize(static_cast<size_t>(src.w) * src.h * 4u);
            std::vector<uint8_t> row(static_cast<size_t>(src.w) * 4u);
            for (int y = 0; y < src.h; ++y) {
                const uint32_t gy = content.y0 + y;
                const uint32_t ty = gy / kTileSize;
                const uint32_t inY = gy % kTileSize;
                std::memset(row.data(), 0, static_cast<size_t>(src.w) * 4u);
                for (uint32_t tx = static_cast<uint32_t>(content.x0) / kTileSize;
                     tx <= static_cast<uint32_t>(content.x1 - 1) / kTileSize; ++tx) {
                    auto it = layer->pixels->tiles.find(ty * layer->pixels->cols + tx);
                    if (it == layer->pixels->tiles.end() || it->second == nullptr ||
                        it->second->pixels == nullptr) {
                        continue;
                    }
                    const uint8_t* sp = it->second->pixels->mapCpu();
                    if (sp == nullptr) {
                        continue;
                    }
                    const int x0 = std::max(content.x0, static_cast<int>(tx * kTileSize));
                    const int x1 = std::min(content.x1, static_cast<int>((tx + 1) * kTileSize));
                    std::memcpy(row.data() + static_cast<size_t>(x0 - content.x0) * 4u,
                                sp + static_cast<size_t>(inY) * it->second->pixels->rowBytes() +
                                    static_cast<size_t>(x0 - static_cast<int>(tx * kTileSize)) * 4u,
                                static_cast<size_t>(x1 - x0) * 4u);
                }
                std::memcpy(&src.px[static_cast<size_t>(y) * src.w * 4u], row.data(),
                            static_cast<size_t>(src.w) * 4u);
            }
            // 正向映射渲染（doc→doc；4px 细分逼近分段单应）
            const double ox = static_cast<double>(layer->transform.originX);
            const double oy = static_cast<double>(layer->transform.originY);
            const warp::TriRender res = warp::warpMeshImg(
                src, RectI{0, 0, src.w, src.h},
                [&map, ox, oy](double x, double y) {
                    const perspective::Pt d = map.map(x + ox, y + oy);
                    return std::array<double, 2>{d[0] - ox, d[1] - oy};
                },
                warp::Interp::Bicubic);
            if (res.out.w <= 0 || res.out.h <= 0) {
                c->err = "render failed";
                return;
            }
            // 发布网格
            const uint32_t cols = std::max(
                1u, (static_cast<uint32_t>(res.out.w) + kTileSize - 1) / kTileSize);
            const uint32_t rows = std::max(
                1u, (static_cast<uint32_t>(res.out.h) + kTileSize - 1) / kTileSize);
            TileGridBuilder builder(cols, rows);
            for (int y = 0; y < res.out.h; ++y) {
                std::vector<uint8_t> outRow(static_cast<size_t>(res.out.w) * 4u);
                bool any = false;
                for (int x = 0; x < res.out.w; ++x) {
                    const float* sp = &res.out.px[(static_cast<size_t>(y) * res.out.w + x) * 4u];
                    uint8_t* d = &outRow[static_cast<size_t>(x) * 4u];
                    for (int c2 = 0; c2 < 4; ++c2) {
                        d[c2] = static_cast<uint8_t>(std::lround(std::clamp(sp[c2], 0.0f, 1.0f) * 255.0f));
                    }
                    if (d[3] != 0) {
                        any = true;
                    }
                }
                if (!any) {
                    continue;
                }
                for (uint32_t tx = 0; tx < cols; ++tx) {
                    bool tileAny = false;
                    for (uint32_t k = 0; k < kTileSize; ++k) {
                        const uint32_t x = tx * kTileSize + k;
                        if (x < static_cast<uint32_t>(res.out.w) &&
                            outRow[static_cast<size_t>(x) * 4u + 3u] != 0) {
                            tileAny = true;
                            break;
                        }
                    }
                    if (!tileAny) {
                        continue;
                    }
                    uint8_t* dst = builder.ensureTile(tx, static_cast<uint32_t>(y) / kTileSize)
                                       .mapCpuWrite() +
                                   (y % kTileSize) * kTileSize * 4u;
                    const uint32_t span = std::min<uint32_t>(kTileSize, static_cast<uint32_t>(res.out.w) - tx * kTileSize);
                    std::memcpy(dst, &outRow[static_cast<size_t>(tx) * kTileSize * 4u],
                                static_cast<size_t>(span) * 4u);
                }
            }
            auto grid = builder.publish(layer->pixels->revision + 1);
            layer->pixels = grid;
            // 内容 doc 位置=origin+gridLocal；渲染锚定 res.origin（层局部）→ origin 重定
            layer->transform.originX += res.originX;
            layer->transform.originY += res.originY;
            if (layer->mask != nullptr && layer->mask->enabled) {
                layer->render = composeMasked(layer->pixels, *layer->mask);
            }
            c->changed = true;
            restyleDocument(work);
            auto& eg = Engine::get();
            std::lock_guard<std::mutex> lk(eg.docMutex);
            if (eg.docEpoch == c->epoch && eg.docRevision == c->revision) {
                eg.history.beginEdit(eg.doc, "\xe9\x80\x8f\xe8\xa7\x86\xe5\x8f\x98\xe5\xbd\xa2",
                                     static_cast<uint64_t>(eg.docRevision));  // 透视变形
                eg.doc = std::move(work);
                eg.history.endEdit(eg.doc, static_cast<uint64_t>(eg.docRevision));
                eg.bumpRevisionLocked();
                eg.requestRender();
            } else {
                c->changed = false;
                c->err = "doc changed during perspective";
            }
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<PerspectiveCtx*>(data);
            auto& eg = Engine::get();
            if (status == napi_ok && c->changed) {
                eg.historyVersion.fetch_add(1);
            }
            napi_value dataObj = nullptr;
            napi_create_object(env, &dataObj);
            napi_set_named_property(env, dataObj, "changed",
                                    makeBool(env, status == napi_ok && c->changed));
            napi_resolve_deferred(env, c->deferred, makeOk(env, dataObj));
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

// ---------- M20d 自适应广角命令面：约束线+镜头模型一步异步事务（CaOp 同范式）。
// 渲染=solve 网格 → warp_ops warpTrianglesImg（约束曲线顶点对 (out,src)）。
// 无约束 = 恒等（拒绝不出事务）。 ----------
struct WideAngleCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    Document docSnap;
    uint64_t epoch = 0;
    int64_t revision = 0;
    LayerId layerId = 0;
    wideangle::Params params;
    bool changed = false;
    std::string err;
};

napi_value ApplyWideAngle(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 5) {
        return makeError(env, kErrBadParam,
                         "applyWideAngle(layerId, model, focal, crop, scale, constraints?) requires 5+ args");
    }
    auto* ctx = new WideAngleCtx();
    auto num = [&](napi_value v, double& out) -> bool {
        return napi_get_value_double(env, v, &out) == napi_ok;
    };
    double layerId = 0, model = 0, focal = 0, crop = 1, scale = 100;
    if (!num(argv[0], layerId) || !num(argv[1], model) || !num(argv[2], focal) ||
        !num(argv[3], crop) || !num(argv[4], scale) || layerId < 1) {
        delete ctx;
        return makeError(env, kErrBadParam, "applyWideAngle: invalid args");
    }
    ctx->layerId = static_cast<LayerId>(layerId);
    ctx->params.model = static_cast<wideangle::Model>(static_cast<int32_t>(model));
    ctx->params.focalLength = focal;
    ctx->params.cropFactor = crop;
    ctx->params.scale = scale;
    if (argc >= 6 && argv[5] != nullptr) {
        bool isArr = false;
        if (napi_is_array(env, argv[5], &isArr) == napi_ok && isArr) {
            uint32_t n = 0;
            napi_get_array_length(env, argv[5], &n);
            for (uint32_t k = 0; k < n; ++k) {
                napi_value obj = nullptr;
                if (napi_get_element(env, argv[5], k, &obj) != napi_ok) {
                    continue;
                }
                wideangle::Constraint c;
                auto fld = [&](const char* key, double& v) -> bool {
                    napi_value f = nullptr;
                    return napi_get_named_property(env, obj, key, &f) == napi_ok && f != nullptr &&
                           napi_get_value_double(env, f, &v) == napi_ok;
                };
                double ax = 0, ay = 0, bx = 0, by = 0, orient = 0;
                if (!fld("ax", ax) || !fld("ay", ay) || !fld("bx", bx) || !fld("by", by)) {
                    continue;
                }
                fld("orientation", orient);
                c.a = {ax, ay};
                c.b = {bx, by};
                c.orientation = static_cast<wideangle::Orientation>(static_cast<int32_t>(orient));
                ctx->params.constraints.push_back(c);
            }
        }
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0) {
            delete ctx;
            return makeError(env, kErrNoDoc, "applyWideAngle: no document");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            delete ctx;
            return makeError(env, kErrBadParam, "applyWideAngle: edit/preview session open");
        }
        ctx->docSnap = e.doc;
        ctx->epoch = e.docEpoch;
        ctx->revision = e.docRevision;
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montageWideAngle", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<WideAngleCtx*>(data);
            Document work = c->docSnap;
            Layer* layer = nullptr;
            for (Layer& l : work.layers) {
                if (l.id == c->layerId) {
                    layer = &l;
                    break;
                }
            }
            if (layer == nullptr || layer->pixels == nullptr) {
                c->err = "no raster layer";
                return;
            }
            uint32_t bx = 0, by = 0, bw = 0, bh = 0;
            if (!layerContentBBox(*layer->pixels, bx, by, bw, bh)) {
                c->err = "empty layer";
                return;
            }
            // 约束 doc → 层局部（帧=内容 bbox）
            const RectI frame{static_cast<int>(bx), static_cast<int>(by),
                              static_cast<int>(bx + bw), static_cast<int>(by + bh)};
            for (wideangle::Constraint& cst : c->params.constraints) {
                cst.a[0] -= bx;
                cst.a[1] -= by;
                cst.b[0] -= bx;
                cst.b[1] -= by;
            }
            // 读内容区
            warp::Img src;
            src.w = frame.width();
            src.h = frame.height();
            src.px.resize(static_cast<size_t>(src.w) * src.h * 4u);
            std::vector<uint8_t> row(static_cast<size_t>(src.w) * 4u);
            for (int y = 0; y < src.h; ++y) {
                const uint32_t gy = frame.y0 + y;
                const uint32_t ty = gy / kTileSize;
                const uint32_t inY = gy % kTileSize;
                std::memset(row.data(), 0, static_cast<size_t>(src.w) * 4u);
                for (uint32_t tx = static_cast<uint32_t>(frame.x0) / kTileSize;
                     tx <= static_cast<uint32_t>(frame.x1 - 1) / kTileSize; ++tx) {
                    auto it = layer->pixels->tiles.find(ty * layer->pixels->cols + tx);
                    if (it == layer->pixels->tiles.end() || it->second == nullptr ||
                        it->second->pixels == nullptr) {
                        continue;
                    }
                    const uint8_t* sp = it->second->pixels->mapCpu();
                    if (sp == nullptr) {
                        continue;
                    }
                    const int x0 = std::max(frame.x0, static_cast<int>(tx * kTileSize));
                    const int x1 = std::min(frame.x1, static_cast<int>((tx + 1) * kTileSize));
                    std::memcpy(row.data() + static_cast<size_t>(x0 - frame.x0) * 4u,
                                sp + static_cast<size_t>(inY) * it->second->pixels->rowBytes() +
                                    static_cast<size_t>(x0 - static_cast<int>(tx * kTileSize)) * 4u,
                                static_cast<size_t>(x1 - x0) * 4u);
                }
                std::memcpy(&src.px[static_cast<size_t>(y) * src.w * 4u], row.data(),
                            static_cast<size_t>(src.w) * 4u);
            }
            // 求解 + 渲染（(out, src) 顶点对直接给 warpTrianglesImg）
            const wideangle::Mesh mesh = wideangle::solve(c->params, frame);
            if (mesh.verts.empty()) {
                c->err = "solve failed";
                return;
            }
            bool identity = true;
            for (const auto& v : mesh.verts) {
                if (std::fabs(v.first[0] - v.second[0]) > 1e-9 ||
                    std::fabs(v.first[1] - v.second[1]) > 1e-9) {
                    identity = false;
                    break;
                }
            }
            if (identity) {
                return;  // 无约束/无缩放 = 恒等
            }
            const auto tris = mesh.triangles();
            const double originX = frame.x0;
            const double originY = frame.y0;
            const warp::TriRender res = warp::warpTrianglesImg(
                src, RectI{0, 0, src.w, src.h}, mesh.verts, tris, warp::Interp::Bicubic,
                0.0, 0.0, frame.width(), frame.height());
            if (res.out.w <= 0) {
                c->err = "render failed";
                return;
            }
            // 发布（帧原点不变：网格 (out,src) 都是 doc 域，输出=帧 bbox 同位）
            const uint32_t cols = std::max(
                1u, (static_cast<uint32_t>(res.out.w) + kTileSize - 1) / kTileSize);
            const uint32_t rows = std::max(
                1u, (static_cast<uint32_t>(res.out.h) + kTileSize - 1) / kTileSize);
            TileGridBuilder builder(cols, rows);
            for (int y = 0; y < res.out.h; ++y) {
                std::vector<uint8_t> outRow(static_cast<size_t>(res.out.w) * 4u);
                bool any = false;
                for (int x = 0; x < res.out.w; ++x) {
                    const float* sp = &res.out.px[(static_cast<size_t>(y) * res.out.w + x) * 4u];
                    uint8_t* d = &outRow[static_cast<size_t>(x) * 4u];
                    for (int c2 = 0; c2 < 4; ++c2) {
                        d[c2] = static_cast<uint8_t>(std::lround(std::clamp(sp[c2], 0.0f, 1.0f) * 255.0f));
                    }
                    if (d[3] != 0) {
                        any = true;
                    }
                }
                if (!any) {
                    continue;
                }
                for (uint32_t tx = 0; tx < cols; ++tx) {
                    bool tileAny = false;
                    for (uint32_t k = 0; k < kTileSize; ++k) {
                        const uint32_t x = tx * kTileSize + k;
                        if (x < static_cast<uint32_t>(res.out.w) &&
                            outRow[static_cast<size_t>(x) * 4u + 3u] != 0) {
                            tileAny = true;
                            break;
                        }
                    }
                    if (!tileAny) {
                        continue;
                    }
                    uint8_t* dst = builder.ensureTile(tx, static_cast<uint32_t>(y) / kTileSize)
                                       .mapCpuWrite() +
                                   (y % kTileSize) * kTileSize * 4u;
                    const uint32_t span = std::min<uint32_t>(kTileSize, static_cast<uint32_t>(res.out.w) - tx * kTileSize);
                    std::memcpy(dst, &outRow[static_cast<size_t>(tx) * kTileSize * 4u],
                                static_cast<size_t>(span) * 4u);
                }
            }
            layer->pixels = builder.publish(layer->pixels->revision + 1);
            if (layer->mask != nullptr && layer->mask->enabled) {
                layer->render = composeMasked(layer->pixels, *layer->mask);
            }
            c->changed = true;
            restyleDocument(work);
            auto& eg = Engine::get();
            std::lock_guard<std::mutex> lk(eg.docMutex);
            if (eg.docEpoch == c->epoch && eg.docRevision == c->revision) {
                eg.history.beginEdit(eg.doc, "\xe8\x87\xaa\xe9\x80\x82\xe5\xba\x94\xe5\xb9\xbf\xe8\xa7\x92",
                                     static_cast<uint64_t>(eg.docRevision));  // 自适应广角
                eg.doc = std::move(work);
                eg.history.endEdit(eg.doc, static_cast<uint64_t>(eg.docRevision));
                eg.bumpRevisionLocked();
                eg.requestRender();
            } else {
                c->changed = false;
                c->err = "doc changed during wide angle";
            }
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<WideAngleCtx*>(data);
            auto& eg = Engine::get();
            if (status == napi_ok && c->changed) {
                eg.historyVersion.fetch_add(1);
            }
            napi_value dataObj = nullptr;
            napi_create_object(env, &dataObj);
            napi_set_named_property(env, dataObj, "changed",
                                    makeBool(env, status == napi_ok && c->changed));
            napi_resolve_deferred(env, c->deferred, makeOk(env, dataObj));
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

napi_value ContentAwareFill(napi_env env, napi_callback_info info) {
    return CaOpAsync(env, info, 0);
}

napi_value ContentAwareMove(napi_env env, napi_callback_info info) {
    return CaOpAsync(env, info, 1);
}

// M11f：历史画笔源（steps 0=关闭；1=最近一次编辑前快照…；笔画时实时取快照）
napi_value SetHistoryBrush(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "setHistoryBrush(steps) requires 1 arg");
    }
    double steps = 0;
    if (napi_get_value_double(env, argv[0], &steps) != napi_ok) {
        return makeError(env, kErrBadParam, "setHistoryBrush: invalid arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.historyBrushSteps = static_cast<int>(steps);
        if (e.historyBrushSteps > 0) {
            e.brushIsClone = false;      // 与克隆互斥（引擎侧兜底）
            e.brushEffect = BrushEffect::None;
        }
    }
    return makeOk(env, nullptr);
}

napi_value StoreSelection(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    char name[128] = {0};
    size_t nameLen = 0;
    if (argc < 1 || napi_get_value_string_utf8(env, argv[0], name, sizeof(name) - 1, &nameLen) != napi_ok ||
        nameLen == 0) {
        return makeError(env, kErrBadParam, "storeSelection(name) requires name");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "storeSelection: no document");
        }
        if (e.doc.selection == nullptr) {
            return makeError(env, kErrBadParam, "storeSelection: no active selection");
        }
        e.history.beginEdit(e.doc, "store selection", static_cast<uint64_t>(e.docRevision));
        AlphaChannel ch;
        ch.name = name;
        ch.mask = e.doc.selection;
        bool replaced = false;
        for (AlphaChannel& c : e.doc.channels) {
            if (c.name == ch.name) {
                c.mask = ch.mask;  // PS：同名覆盖
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            e.doc.channels.push_back(std::move(ch));
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value LoadSelection(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    char name[128] = {0};
    size_t nameLen = 0;
    int32_t op = 0;
    if (argc < 1 || napi_get_value_string_utf8(env, argv[0], name, sizeof(name) - 1, &nameLen) != napi_ok ||
        nameLen == 0) {
        return makeError(env, kErrBadParam, "loadSelection(name, op?) requires name");
    }
    if (argc >= 2 && napi_get_value_int32(env, argv[1], &op) != napi_ok) {
        return makeError(env, kErrBadParam, "loadSelection: invalid op");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "loadSelection: no document");
        }
        const TileGrid* mask = nullptr;
        for (const AlphaChannel& c : e.doc.channels) {
            if (c.name == name && c.mask != nullptr) {
                mask = c.mask.get();
                break;
            }
        }
        if (mask == nullptr) {
            return makeError(env, kErrBadParam, "loadSelection: channel not found");
        }
        // 通道掩码（shared_ptr 共享零拷贝）→ 布尔合并走既有 combineSelection
        std::shared_ptr<const TileGrid> next;
        for (const AlphaChannel& c : e.doc.channels) {
            if (c.name == name) {
                next = c.mask;
                break;
            }
        }
        auto merged = tiles::combineSelection(e.doc, next, static_cast<tiles::SelectionOp>(op));
        e.history.beginEdit(e.doc, "load selection", static_cast<uint64_t>(e.docRevision));
        e.doc.selection = std::move(merged);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value DeleteChannel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    char name[128] = {0};
    size_t nameLen = 0;
    if (argc < 1 || napi_get_value_string_utf8(env, argv[0], name, sizeof(name) - 1, &nameLen) != napi_ok ||
        nameLen == 0) {
        return makeError(env, kErrBadParam, "deleteChannel(name) requires name");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.history.beginEdit(e.doc, "delete channel", static_cast<uint64_t>(e.docRevision));
        const size_t before = e.doc.channels.size();
        for (size_t i = 0; i < e.doc.channels.size(); ++i) {
            if (e.doc.channels[i].name == name) {
                e.doc.channels.erase(e.doc.channels.begin() + static_cast<long>(i));
                break;
            }
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        if (e.doc.channels.size() != before) {
            e.bumpRevisionLocked();
            e.requestRender();
        }
    }
    return makeOk(env, nullptr);
}

// 通道列表（名字数组；v1 通道面板只读）
napi_value GetChannelList(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    napi_value arr = nullptr;
    napi_create_array_with_length(env, e.doc.channels.size(), &arr);
    for (size_t i = 0; i < e.doc.channels.size(); ++i) {
        napi_value s = nullptr;
        napi_create_string_utf8(env, e.doc.channels[i].name.c_str(), NAPI_AUTO_LENGTH, &s);
        napi_set_element(env, arr, static_cast<uint32_t>(i), s);
    }
    return arr;
}

napi_value CopySelection(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    bool merged = false;
    if (argc >= 1) {
        napi_get_value_bool(env, argv[0], &merged);
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "copySelection: no document");
        }
        if (e.doc.selection == nullptr) {
            return makeError(env, kErrBadParam, "copySelection: no active selection");
        }
        if (!copySelectionToClipboard(e.doc, merged, e.clipboard)) {
            return makeError(env, kErrBadParam, "copySelection: nothing to copy");
        }
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x4D30, "Montage.Clipboard",
                     "copy %{public}s %{public}ux%{public}u @(%{public}d,%{public}d)",
                     merged ? "merged" : "layer", e.clipboard.width, e.clipboard.height,
                     e.clipboard.docX, e.clipboard.docY);
    }
    return makeOk(env, nullptr);
}

napi_value CutSelection(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    bool merged = false;
    if (argc >= 1) {
        napi_get_value_bool(env, argv[0], &merged);
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "cutSelection: no document");
        }
        if (e.doc.selection == nullptr) {
            return makeError(env, kErrBadParam, "cutSelection: no active selection");
        }
        if (!copySelectionToClipboard(e.doc, merged, e.clipboard)) {
            return makeError(env, kErrBadParam, "cutSelection: nothing to cut");
        }
        e.history.beginEdit(e.doc, "cut", static_cast<uint64_t>(e.docRevision));
        eraseSelectionPixels(e.doc);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value DeleteSelectionPixels(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "deleteSelectionPixels: no document");
        }
        if (e.doc.selection == nullptr) {
            return makeError(env, kErrBadParam, "deleteSelectionPixels: no active selection");
        }
        e.history.beginEdit(e.doc, "clear", static_cast<uint64_t>(e.docRevision));
        const bool changed = eraseSelectionPixels(e.doc);
        if (changed) {
            restyleAllLocked(e);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        if (changed) {
            e.bumpRevisionLocked();
            e.requestRender();
        }
    }
    return makeOk(env, nullptr);
}

napi_value PasteClipboard(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "pasteClipboard: no document");
        }
        if (e.clipboard.width == 0) {
            return makeError(env, kErrBadParam, "pasteClipboard: clipboard empty");
        }
        e.history.beginEdit(e.doc, "paste", static_cast<uint64_t>(e.docRevision));
        const LayerId newId = e.nextLayerId();
        pasteClipboardAsLayer(e.doc, e.clipboard, newId);
        restyleAllLocked(e);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x4D30, "Montage.Clipboard",
                     "paste layer=%{public}llu %{public}ux%{public}u @(%{public}d,%{public}d)",
                     static_cast<unsigned long long>(newId), e.clipboard.width, e.clipboard.height,
                     e.clipboard.docX, e.clipboard.docY);
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
        // M12（PS 语义）：在文本层上作画自动栅格化（一步事务），此后按普通层走
        {
            Layer* al = findLayerLocked(e.doc, e.doc.activeId);
            if (al != nullptr && al->text != nullptr) {
                e.history.beginEdit(e.doc, "rasterize text (paint)",
                                    static_cast<uint64_t>(e.docRevision));
                rasterizeTextLayerToPixels(*al);
                e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
                e.bumpRevisionLocked();
                OH_LOG_Print(LOG_APP, LOG_INFO, 0x4D30, "Montage.Text",
                             "text layer auto rasterized (paint)");
            }
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
        e.strokeCv.notify_all();  // 唤醒笔画线程（此前缺失：线程 cv.wait 睡死、笔画不提交，M7.5 实证修复）
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
        e.strokeCv.notify_all();
    }
    return makeOk(env, nullptr);
}

napi_value EndStroke(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> sk(e.strokeMtx);
        e.strokeQueue.push_back({0.0f, 0.0f, 1.0f, false, true});
        e.ensureStrokeThreadLocked();
        e.strokeCv.notify_all();
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
            e.fireRevisionLocked();  // 恢复快照版本只广播（bump 的自增会让值恰好不变、@Watch 不触发）
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
            e.fireRevisionLocked();  // 同 undo：恢复快照版本只广播
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
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        revision = e.revisionTsfn;
        e.revisionTsfn = nullptr;
    }
    if (revision != nullptr) {
        napi_release_threadsafe_function(revision, napi_tsfn_release);
    }
    // progress TSFN 必须与导入线程同锁（import.mtx）销毁：notifyProgress 持该锁读指针并
    // 调用，此处锁内置空+release → 不存在「线程已读到旧指针、release 后才调用」的
    // use-after-release 窗口（原先读 docMutex 写、无锁读，两侧互斥不成立）
    {
        std::lock_guard<std::mutex> lk(e.import.mtx);
        if (e.progressTsfn != nullptr) {
            napi_release_threadsafe_function(e.progressTsfn, napi_tsfn_release);
            e.progressTsfn = nullptr;
        }
    }
    e.render.stop();
    return makeOk(env, nullptr);
}

// ---------- M8a 无损文档操作（06 规划 §4）：resizeCanvas / trim / revealAll / rotate / flip / translate ----------

napi_value ResizeCanvas(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        return makeError(env, kErrBadParam, "resizeCanvas(x, y, w, h) requires 4 args");
    }
    double x = 0, y = 0, w = 0, h = 0;
    if (napi_get_value_double(env, argv[0], &x) != napi_ok ||
        napi_get_value_double(env, argv[1], &y) != napi_ok ||
        napi_get_value_double(env, argv[2], &w) != napi_ok ||
        napi_get_value_double(env, argv[3], &h) != napi_ok) {
        return makeError(env, kErrBadParam, "resizeCanvas: invalid args");
    }
    auto& e = Engine::get();
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "resizeCanvas: no document");
        }
        e.history.beginEdit(e.doc, "canvas size", static_cast<uint64_t>(e.docRevision));
        changed = resizeCanvas(e.doc, static_cast<int>(x), static_cast<int>(y),
                                 static_cast<int>(w), static_cast<int>(h));
        if (changed) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            e.bumpRevisionLocked();
            e.requestRender();
        } else {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        }
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "changed", makeBool(env, changed));
    return makeOk(env, data);
}

// ---------- M8a/M8c 文档级几何命令（审查修复：原在 JS 线程同步持 docMutex 整档搬像素 +
// restyleAllLocked，大图 appfreeze 与 M8.1 warp 同型——统一 async work，重活锁外作用于
// 快照副本，发布段 docEpoch+docRevision 复核（防并发编辑/关档后被旧快照覆盖）） ----------
struct DocGeomCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    Document docSnap;   // 锁内整档拷贝（shared_ptr 共享瓦片，O(层数)）
    uint64_t epoch = 0;
    int64_t revision = 0;
    int op = 0;         // 0=rot90 1=flip 2=trim 3=revealAll 4=resample 5=rotArb 6=caScale
    bool flag = true;   // clockwise / horizontal
    double deg = 0;     // rotArb
    uint32_t w = 0, h = 0;  // resample / caScale
    int method = 2;         // resample 重采样方法（M20a，2=Bicubic 默认）
    bool changed = false;
    std::string err;    // caScale 失败原因
};

napi_value DocGeomAsync(napi_env env, napi_callback_info info, int op) {
    auto& e = Engine::get();
    auto* ctx = new DocGeomCtx();
    ctx->op = op;
    if (op == 0 || op == 1) {
        size_t argc = 1;
        napi_value argv[1] = {nullptr};
        napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
        if (argc >= 1) {
            napi_get_value_bool(env, argv[0], &ctx->flag);
        }
    } else if (op == 4 || op == 6) {
        size_t argc = 3;
        napi_value argv[3] = {nullptr, nullptr, nullptr};
        napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
        if (argc < 2) {
            delete ctx;
            return makeError(env, kErrBadParam,
                             op == 4 ? "resampleDocument(w, h, method?) requires 2+ args"
                                     : "contentAwareScale(w, h) requires 2 args");
        }
        double w = 0, h = 0;
        if (napi_get_value_double(env, argv[0], &w) != napi_ok ||
            napi_get_value_double(env, argv[1], &h) != napi_ok || w < 1 || h < 1) {
            delete ctx;
            return makeError(env, kErrBadParam,
                             op == 4 ? "resampleDocument: invalid size"
                                     : "contentAwareScale: invalid size");
        }
        ctx->w = static_cast<uint32_t>(w);
        ctx->h = static_cast<uint32_t>(h);
        if (op == 4 && argc >= 3 && argv[2] != nullptr) {
            double m = 0;
            if (napi_get_value_double(env, argv[2], &m) == napi_ok && m >= 0 && m <= 7) {
                ctx->method = static_cast<int>(m);
            }
        }
    } else if (op == 5) {
        size_t argc = 1;
        napi_value argv[1] = {nullptr};
        napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
        if (argc < 1) {
            delete ctx;
            return makeError(env, kErrBadParam, "rotateDocumentArbitrary(deg) requires 1 arg");
        }
        if (napi_get_value_double(env, argv[0], &ctx->deg) != napi_ok) {
            delete ctx;
            return makeError(env, kErrBadParam, "rotateDocumentArbitrary: invalid deg");
        }
    }
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            delete ctx;
            return makeError(env, kErrNoDoc, "docGeom: no document");
        }
        ctx->docSnap = e.doc;
        ctx->epoch = e.docEpoch;
        ctx->revision = e.docRevision;
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montageDocGeom", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<DocGeomCtx*>(data);
            // 重活在锁外：整档像素搬运 + restyleDocument（纯函数）作用于本地副本
            Document work = c->docSnap;
            switch (c->op) {
                case 0:
                    rotateDocument90(work, c->flag);
                    c->changed = true;
                    break;
                case 1:
                    flipDocument(work, c->flag);
                    c->changed = true;
                    break;
                case 2: {
                    int bx = 0, by = 0, bw = 0, bh = 0;
                    if (visibleContentBBox(work, bx, by, bw, bh)) {
                        c->changed = resizeCanvas(work, bx, by, bw, bh);
                    }
                    break;
                }
                case 3: {
                    int bx = 0, by = 0, bw = 0, bh = 0;
                    int x = 0;
                    int y = 0;
                    int w = static_cast<int>(work.width);
                    int h = static_cast<int>(work.height);
                    if (visibleContentBBox(work, bx, by, bw, bh)) {
                        const int x1 = std::max(x + w, bx + bw);
                        const int y1 = std::max(y + h, by + bh);
                        x = std::min(x, bx);
                        y = std::min(y, by);
                        w = x1 - x;
                        h = y1 - y;
                    }
                    c->changed = resizeCanvas(work, x, y, w, h);
                    break;
                }
                case 4:
                    // M20a：method 化 HQ 重采样（缺省 2=Bicubic；旧双线性=1）
                    c->changed = resampleDocumentHQ(work, c->w, c->h,
                                                    static_cast<ResampleMethod>(c->method));
                    break;
                case 5:
                    rotateDocumentArbitrary(work, c->deg);
                    c->changed = true;
                    break;
                case 6:
                    c->changed = contentAwareScale(work, c->w, c->h, c->err);
                    break;
                default:
                    break;
            }
            if (c->changed) {
                restyleDocument(work);
            }
            // 发布段（持锁）：代次/版本失配 = 快照后有并发编辑/关档 → 整体作废
            auto& eg = Engine::get();
            std::lock_guard<std::mutex> lk(eg.docMutex);
            if (c->changed && eg.docEpoch == c->epoch && eg.docRevision == c->revision) {
                const char* label = c->op == 0
                                        ? (c->flag ? "rotate 90 CW" : "rotate 90 CCW")
                                    : c->op == 1
                                        ? (c->flag ? "flip horizontal" : "flip vertical")
                                    : c->op == 2 ? "trim"
                                    : c->op == 3 ? "reveal all"
                                    : c->op == 4 ? "image size"
                                    : c->op == 6 ? "content-aware scale"
                                                 : "rotate arbitrary";
                eg.history.beginEdit(eg.doc, label, static_cast<uint64_t>(eg.docRevision));
                eg.doc = std::move(work);
                eg.history.endEdit(eg.doc, static_cast<uint64_t>(eg.docRevision));
                eg.bumpRevisionLocked();
                eg.requestRender();
            } else {
                c->changed = false;
            }
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<DocGeomCtx*>(data);
            napi_value dataObj = nullptr;
            napi_create_object(env, &dataObj);
            napi_set_named_property(env, dataObj, "changed",
                                    makeBool(env, status == napi_ok && c->changed));
            napi_resolve_deferred(env, c->deferred, makeOk(env, dataObj));
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

napi_value TrimCanvas(napi_env env, napi_callback_info info) {
    return DocGeomAsync(env, info, 2);
}

napi_value RevealAll(napi_env env, napi_callback_info info) {
    return DocGeomAsync(env, info, 3);
}

napi_value RotateDocument90(napi_env env, napi_callback_info info) {
    return DocGeomAsync(env, info, 0);
}

napi_value FlipDocument(napi_env env, napi_callback_info info) {
    return DocGeomAsync(env, info, 1);
}

// 移动工具：三段事务（0=begin 快照 / 1=move 实时平移 / 2=end 提交），拖拽过程实时渲染、
// 撤销粒度 = 一次拖拽。id=0 表示活动图层。M14b：活动层 linkId 非零时联动平移同组全部层。
napi_value TranslateLayer(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        return makeError(env, kErrBadParam, "translateLayer(id, dx, dy, phase) requires 4 args");
    }
    double id = 0, dx = 0, dy = 0, phase = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_double(env, argv[1], &dx) != napi_ok ||
        napi_get_value_double(env, argv[2], &dy) != napi_ok ||
        napi_get_value_double(env, argv[3], &phase) != napi_ok) {
        return makeError(env, kErrBadParam, "translateLayer: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "translateLayer: no document");
        }
        Layer* l = id > 0 ? findLayerLocked(e.doc, static_cast<LayerId>(id)) : nullptr;
        if (l == nullptr) {
            for (Layer& li : e.doc.layers) {
                if (li.id == e.doc.activeId) {
                    l = &li;
                    break;
                }
            }
        }
        if (l == nullptr) {
            return makeError(env, kErrNoDoc, "translateLayer: no active layer");
        }
        // M14b 链接联动：拖拽目标 linkId 非零 → 同组全部层一起动（每组拖拽一次事务）
        std::vector<LayerId> linked = collectLinkedIds(e.doc, l->id);
        const int p = static_cast<int>(phase);
        if (p == 0) {
            e.history.beginEdit(e.doc, linked.size() > 1 ? "move linked layers" : "move layer",
                                static_cast<uint64_t>(e.docRevision));
        }
        for (LayerId lid : linked) {
            Layer* t = findLayerLocked(e.doc, lid);
            if (t == nullptr) {
                continue;
            }
            shiftLayerOrigins(*t, dx, dy);  // M14c：styledOrigin 随层平移
        }
        if (p == 2) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            e.bumpRevisionLocked();
        }
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value TranslateSelection(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "translateSelection(dx, dy) requires 2 args");
    }
    double dx = 0, dy = 0;
    if (napi_get_value_double(env, argv[0], &dx) != napi_ok ||
        napi_get_value_double(env, argv[1], &dy) != napi_ok) {
        return makeError(env, kErrBadParam, "translateSelection: invalid args");
    }
    auto& e = Engine::get();
    bool moved = false;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "translateSelection: no document");
        }
        e.history.beginEdit(e.doc, "move selection", static_cast<uint64_t>(e.docRevision));
        moved = translateSelection(e.doc, static_cast<int>(dx), static_cast<int>(dy));
        if (moved) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            e.bumpRevisionLocked();
            e.requestRender();
        } else {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        }
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "moved", makeBool(env, moved));
    return makeOk(env, data);
}

// ---------- M7.5 导出管线（06 规划 §4）：renameLayer / samplePixel / exportPng / flattenToPixelMap ----------

napi_value RenameLayer(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "renameLayer(id, name) requires 2 args");
    }
    double id = 0;
    char buf[256] = {0};
    size_t len = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_string_utf8(env, argv[1], buf, sizeof(buf), &len) != napi_ok) {
        return makeError(env, kErrBadParam, "renameLayer: invalid args");
    }
    auto& e = Engine::get();
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l != nullptr) {
            l->name = std::string(buf, len);
            e.bumpRevisionLocked();
            e.requestRender();
            ok = true;
        }
    }
    if (!ok) {
        return makeError(env, kErrBadParam, "renameLayer: layer not found");
    }
    return makeOk(env, nullptr);
}

// 扁平化异步任务上下文（锁内建快照 → libuv 线程锁外计算 → JS 线程 resolve）
struct ExportCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    io::FlattenJob job;
    io::FlattenResult result;
    double sampleX = 0;          // samplePixel 用
    double sampleY = 0;
    int fd = -1;                 // exportPng：fd 全路径接管（execute 内关闭）
    uint64_t pngBytes = 0;
    io::PsdDocSnap psdSnap;      // exportPsd 用
    io::PsdExportResult psdResult;
};

void finalizeExportCtx(napi_env /*env*/, void* data, void* /*hint*/) {
    delete static_cast<ExportCtx*>(data);
}

napi_value SamplePixel(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "samplePixel(x, y) requires 2 args");
    }
    double x = 0;
    double y = 0;
    if (napi_get_value_double(env, argv[0], &x) != napi_ok ||
        napi_get_value_double(env, argv[1], &y) != napi_ok) {
        return makeError(env, kErrBadParam, "samplePixel: invalid args");
    }
    auto& e = Engine::get();
    auto* ctx = new ExportCtx();
    ctx->sampleX = x;
    ctx->sampleY = y;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            delete ctx;
            return makeError(env, kErrNoDoc, "samplePixel: no document");
        }
        ctx->job = io::BuildFlattenJob(e.doc, 0, 0, 0, 0);
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montageSamplePixel", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<ExportCtx*>(data);
            const int px = static_cast<int>(std::floor(c->sampleX));
            const int py = static_cast<int>(std::floor(c->sampleY));
            c->result = io::FlattenVisible(c->job, px, py, 1, 1);
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<ExportCtx*>(data);
            napi_value dataObj = nullptr;
            napi_create_object(env, &dataObj);
            if (status == napi_ok && c->result.ok && c->result.rgba.size() >= 4) {
                napi_set_named_property(env, dataObj, "ok", makeBool(env, true));
                napi_set_named_property(env, dataObj, "red", makeUint32(env, c->result.rgba[0]));
                napi_set_named_property(env, dataObj, "green", makeUint32(env, c->result.rgba[1]));
                napi_set_named_property(env, dataObj, "blue", makeUint32(env, c->result.rgba[2]));
                napi_set_named_property(env, dataObj, "alpha", makeUint32(env, c->result.rgba[3]));
            } else {
                napi_set_named_property(env, dataObj, "ok", makeBool(env, false));
                napi_set_named_property(env, dataObj, "red", makeUint32(env, 0));
                napi_set_named_property(env, dataObj, "green", makeUint32(env, 0));
                napi_set_named_property(env, dataObj, "blue", makeUint32(env, 0));
                napi_set_named_property(env, dataObj, "alpha", makeUint32(env, 0));
            }
            napi_resolve_deferred(env, c->deferred, makeOk(env, dataObj));
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

napi_value ExportPng(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "exportPng(fd) requires 1 arg");
    }
    int32_t fd = -1;
    if (napi_get_value_int32(env, argv[0], &fd) != napi_ok || fd < 0) {
        return makeError(env, kErrBadParam, "exportPng: invalid fd");
    }
    auto& e = Engine::get();
    auto* ctx = new ExportCtx();
    ctx->fd = fd;  // 全路径接管：execute 关闭（成功/失败都关）
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            delete ctx;
            close(fd);
            return makeError(env, kErrNoDoc, "exportPng: no document");
        }
        ctx->job = io::BuildFlattenJob(e.doc, 0, 0, 0, 0);
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montageExportPng", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<ExportCtx*>(data);
            io::FlattenResult r = io::FlattenVisible(c->job, 0, 0, 0, 0);
            c->result = std::move(r);
            if (c->result.ok) {
                std::string err;
                if (!io::WritePngToFd(c->fd, c->result.width, c->result.height, c->result.rgba,
                                      &c->pngBytes, &err)) {
                    c->result.ok = false;
                    c->result.error = err;
                }
            }
            close(c->fd);
            c->fd = -1;
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<ExportCtx*>(data);
            if (c->fd >= 0) {
                close(c->fd);  // 兜底（create 失败路径不会到这，但防御）
                c->fd = -1;
            }
            if (status == napi_ok && c->result.ok) {
                napi_value dataObj = nullptr;
                napi_create_object(env, &dataObj);
                napi_set_named_property(env, dataObj, "width", makeUint32(env, c->result.width));
                napi_set_named_property(env, dataObj, "height", makeUint32(env, c->result.height));
                napi_set_named_property(env, dataObj, "bytes", makeDouble(env, static_cast<double>(c->pngBytes)));
                napi_resolve_deferred(env, c->deferred, makeOk(env, dataObj));
            } else {
                napi_resolve_deferred(env, c->deferred,
                                      makeError(env, kErrInternal, c->result.error.empty()
                                          ? "exportPng failed" : c->result.error));
            }
            napi_delete_async_work(env, c->work);
            delete c;
        },
        ctx, &ctx->work);
    if (st != napi_ok) {
        close(fd);
        delete ctx;
        return makeError(env, kErrInternal, "create async work failed");
    }
    napi_queue_async_work(env, ctx->work);
    return promise;
}

// M8d：自由变换烘焙（围绕枢轴 缩放/旋转/斜切；history 事务）
napi_value BakeLayerTransform(napi_env env, napi_callback_info info) {
    size_t argc = 8;
    napi_value argv[8] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 8) {
        return makeError(env, kErrBadParam,
                         "bakeLayerTransform(id,sx,sy,rot,skew,px,py) requires 8 args");
    }
    double p[7] = {0};
    for (int i = 1; i < 8; ++i) {
        if (napi_get_value_double(env, argv[i], &p[i - 1]) != napi_ok) {
            return makeError(env, kErrBadParam, "bakeLayerTransform: invalid args");
        }
    }
    double id = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "bakeLayerTransform: invalid id");
    }
    auto& e = Engine::get();
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "bakeLayerTransform: no document");
        }
        e.history.beginEdit(e.doc, "free transform", static_cast<uint64_t>(e.docRevision));
        ok = bakeLayerTransform(e.doc, static_cast<LayerId>(id), p[0], p[1], p[2], p[3], p[4], p[5]);
        if (ok) {
            restyleAllLocked(e);
        }
        if (ok) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            e.bumpRevisionLocked();
            e.requestRender();
        } else {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        }
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "changed", makeBool(env, ok));
    return makeOk(env, data);
}

// M8.1：自由变换 overlay——进入（返回活动层内容 bbox；返回后 overlay 显示框）
napi_value BeginTransformOverlay(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    int bx = 0, by = 0, bw = 0, bh = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "beginTransformOverlay: no document");
        }
        const Layer* active = nullptr;
        for (const Layer& l : e.doc.layers) {
            if (l.id == e.doc.activeId) {
                active = &l;
                break;
            }
        }
        if (active == nullptr || active->effectivePixels() == nullptr) {
            return makeError(env, kErrBadParam, "beginTransformOverlay: empty active layer");
        }
        if (!visibleContentBBox(e.doc, bx, by, bw, bh)) {
            return makeError(env, kErrBadParam, "beginTransformOverlay: no content");
        }
        Engine::TransformPreview tp;
        tp.layerId = active->id;
        tp.active = true;  // 单位阵（无变换）
        e.transformPreview = tp;
        e.requestRender();
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "left", makeInt32(env, bx));
    napi_set_named_property(env, data, "top", makeInt32(env, by));
    napi_set_named_property(env, data, "width", makeInt32(env, bw));
    napi_set_named_property(env, data, "height", makeInt32(env, bh));
    return makeOk(env, data);
}

// M8.1：拖拽实时预览矩阵（active=false 取消预览）
napi_value SetTransformPreview(napi_env env, napi_callback_info info) {
    size_t argc = 8;
    napi_value argv[8] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "setTransformPreview requires args");
    }
    bool active = false;
    if (napi_get_value_bool(env, argv[0], &active) != napi_ok) {
        return makeError(env, kErrBadParam, "setTransformPreview: invalid active");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.transformPreview.active = active;
        if (active && argc >= 7) {
            for (int i = 0; i < 6; ++i) {
                double v = 0;
                if (napi_get_value_double(env, argv[i + 1], &v) == napi_ok) {
                    e.transformPreview.m[i] = v;
                }
            }
        }
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M8.1：矩阵烘焙（overlay 确认；异步 warp 防主线程 freeze；一步撤销；清除预览态）
struct BakeCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    Document docSnap;   // 锁内整档拷贝（shared_ptr 共享瓦片，O(层数)）
    double id = 0;
    double m[6] = {1, 0, 0, 0, 1, 0};
    bool ok = false;
    // 发布护栏（审查修复）：快照到 worker 执行之间文档可能被并发编辑/关闭，
    // 代次+版本失配即作废，防旧 docSnap 整档覆盖丢编辑/复活已关档（照 developRaw 复核范式）
    uint64_t epoch = 0;
    int64_t revision = 0;
};

napi_value BakeLayerMatrix(napi_env env, napi_callback_info info) {
    size_t argc = 7;
    napi_value argv[7] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 7) {
        return makeError(env, kErrBadParam, "bakeLayerMatrix(id,m0..m5) requires 7 args");
    }
    auto* ctx = new BakeCtx();
    // id 只取 argv[0]（审查修复 P2-6：原空转循环对 i=0..6 每次成功解析都覆写 ctx->id，
    // 最终 id = argv[6] = m[5]（Y 平移分量）→ 烘焙找错层/静默失败，M8.1 起即坏）
    double idVal = 0;
    if (napi_get_value_double(env, argv[0], &idVal) != napi_ok || idVal < 0) {
        delete ctx;
        return makeError(env, kErrBadParam, "bakeLayerMatrix: invalid layer id");
    }
    ctx->id = idVal;
    {
        double m6[6] = {0};
        for (int i = 1; i < 7; ++i) {
            if (napi_get_value_double(env, argv[i], &m6[i - 1]) != napi_ok) {
                delete ctx;
                return makeError(env, kErrBadParam, "bakeLayerMatrix: invalid args");
            }
            ctx->m[i - 1] = m6[i - 1];
        }
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            delete ctx;
            return makeError(env, kErrNoDoc, "bakeLayerMatrix: no document");
        }
        ctx->docSnap = e.doc;
        ctx->epoch = e.docEpoch;
        ctx->revision = e.docRevision;
        // 预览态即清（渲染回到未变换；烘焙结果异步发布）
        e.transformPreview.active = false;
        e.requestRender();
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montageBakeXform", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<BakeCtx*>(data);
            auto& e = Engine::get();
            // warp 在锁外跑（此前持 docMutex 整段烘焙，大图长阻塞所有命令）；
            // 纯函数作用于快照副本，历史事务只在发布段挂 e.doc——作废路径零污染
            Document work = c->docSnap;
            const bool baked = bakeLayerMatrix(work, static_cast<LayerId>(c->id), c->m);
            std::lock_guard<std::mutex> lk(e.docMutex);
            if (!baked || e.docEpoch != c->epoch || e.docRevision != c->revision) {
                c->ok = false;  // 烘焙失败或快照已过期 → 整体作废，不发布不进历史
                return;
            }
            e.history.beginEdit(e.doc, "free transform", static_cast<uint64_t>(e.docRevision));
            e.doc = std::move(work);
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            c->ok = true;
            restyleAllLocked(e);
            e.bumpRevisionLocked();
            e.requestRender();
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<BakeCtx*>(data);
            napi_value dataObj = nullptr;
            napi_create_object(env, &dataObj);
            napi_set_named_property(env, dataObj, "changed", makeBool(env, status == napi_ok && c->ok));
            napi_resolve_deferred(env, c->deferred, makeOk(env, dataObj));
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

// M8.1 取消（清预览态）
napi_value CancelTransformOverlay(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.transformPreview.active = false;
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M9b：曲线控制点设置（id, n, xs[], ys[]；0..1 归一；history 事务）
napi_value SetCurvePoints(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 3) {
        return makeError(env, kErrBadParam, "setCurvePoints(id, xs, ys) requires 3 args");
    }
    double id = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "setCurvePoints: invalid id");
    }
    bool isArr1 = false;
    bool isArr2 = false;
    napi_is_array(env, argv[1], &isArr1);
    napi_is_array(env, argv[2], &isArr2);
    if (!isArr1 || !isArr2) {
        return makeError(env, kErrBadParam, "setCurvePoints: xs/ys must be arrays");
    }
    uint32_t nx = 0;
    uint32_t ny = 0;
    napi_get_array_length(env, argv[1], &nx);
    napi_get_array_length(env, argv[2], &ny);
    if (nx < 2 || nx != ny || nx > 64) {
        return makeError(env, kErrBadParam, "setCurvePoints: bad point count");
    }
    std::vector<float> xs(nx);
    std::vector<float> ys(nx);
    for (uint32_t i = 0; i < nx; ++i) {
        napi_value xv = nullptr;
        napi_value yv = nullptr;
        napi_get_element(env, argv[1], i, &xv);
        napi_get_element(env, argv[2], i, &yv);
        double vx = 0;
        double vy = 0;
        if (napi_get_value_double(env, xv, &vx) != napi_ok ||
            napi_get_value_double(env, yv, &vy) != napi_ok || vx < 0 || vx > 1 || vy < -1 ||
            vy > 2) {
            return makeError(env, kErrBadParam, "setCurvePoints: bad point value");
        }
        xs[i] = static_cast<float>(vx);
        ys[i] = static_cast<float>(vy);
    }
    auto& e = Engine::get();
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr || l->adjustment == nullptr) {
            return makeError(env, kErrBadParam, "setCurvePoints: not an adjustment layer");
        }
        e.history.beginEdit(e.doc, "curves points", static_cast<uint64_t>(e.docRevision));
        // COW（照 setAdjustmentParams 范式）：History 快照浅拷贝 shared_ptr，原地改 pointee
        // 会同步污染撤销前快照 → undo 恢复不出旧曲线
        auto adj = std::make_shared<LayerAdjustment>(*l->adjustment);
        adj->kind = AdjustmentKind::Curves;
        adj->curveX = xs;
        adj->curveY = ys;
        l->adjustment = std::move(adj);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
        ok = true;
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "performed", makeBool(env, ok));
    return makeOk(env, data);
}

// M9c：前景/背景色（fg 即笔刷色——PS 语义；X 交换/D 重置由 UI 侧调）
napi_value SetColors(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 6) {
        return makeError(env, kErrBadParam, "setColors(fgR,fgG,fgB,bgR,bgG,bgB) requires 6 args");
    }
    double v[6] = {0};
    for (int i = 0; i < 6; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok || v[i] < 0 || v[i] > 1) {
            return makeError(env, kErrBadParam, "setColors: invalid color");
        }
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.fgColor[0] = static_cast<float>(v[0]);
        e.fgColor[1] = static_cast<float>(v[1]);
        e.fgColor[2] = static_cast<float>(v[2]);
        e.bgColor[0] = static_cast<float>(v[3]);
        e.bgColor[1] = static_cast<float>(v[4]);
        e.bgColor[2] = static_cast<float>(v[5]);
        e.brush.red = e.fgColor[0];
        e.brush.green = e.fgColor[1];
        e.brush.blue = e.fgColor[2];
    }
    return makeOk(env, nullptr);
}

// M9a：调整层 kind 切换 + 通用参数（id, kind, p0..p3；kind 对齐 AdjustmentKind 枚举）
napi_value SetAdjustmentKind(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 6) {
        return makeError(env, kErrBadParam, "setAdjustmentKind(id,kind,p0..p3) requires 6 args");
    }
    double v[6] = {0};
    for (int i = 0; i < 6; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setAdjustmentKind: invalid args");
        }
    }
    auto& e = Engine::get();
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(v[0]));
        if (l == nullptr || l->adjustment == nullptr) {
            return makeError(env, kErrBadParam, "setAdjustmentKind: not an adjustment layer");
        }
        const int kind = static_cast<int>(v[1]);
        if (kind < 0 || kind > 7) {
            return makeError(env, kErrBadParam, "setAdjustmentKind: bad kind");
        }
        e.history.beginEdit(e.doc, "adjustment kind", static_cast<uint64_t>(e.docRevision));
        // COW：同 setCurvePoints——kind/参数原地改会污染 History 浅拷贝的撤销前快照
        auto adj = std::make_shared<LayerAdjustment>(*l->adjustment);
        adj->kind = static_cast<AdjustmentKind>(kind);
        adj->p0 = static_cast<float>(v[2]);
        adj->p1 = static_cast<float>(v[3]);
        adj->p2 = static_cast<float>(v[4]);
        adj->p3 = static_cast<float>(v[5]);
        l->adjustment = std::move(adj);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
        ok = true;
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "performed", makeBool(env, ok));
    return makeOk(env, data);
}

// M9a：直方图（全文档扁平化的亮度/R/G/B 4×256；异步）
struct HistogramCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    io::FlattenJob job;
    io::FlattenResult result;
    uint32_t luma[256] = {0};
    uint32_t r[256] = {0};
    uint32_t g[256] = {0};
    uint32_t b[256] = {0};
};

napi_value GetHistogram(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    auto* ctx = new HistogramCtx();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            delete ctx;
            return makeError(env, kErrNoDoc, "getHistogram: no document");
        }
        ctx->job = io::BuildFlattenJob(e.doc, 0, 0, 0, 0);
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montageHistogram", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<HistogramCtx*>(data);
            io::FlattenResult fr = io::FlattenVisible(c->job, 0, 0, 0, 0);
            c->result = std::move(fr);
            const size_t n = static_cast<size_t>(c->result.width) * c->result.height;
            for (size_t i = 0; i < n; ++i) {
                const uint8_t R = c->result.rgba[i * 4];
                const uint8_t G = c->result.rgba[i * 4 + 1];
                const uint8_t B = c->result.rgba[i * 4 + 2];
                const uint8_t A = c->result.rgba[i * 4 + 3];
                if (A == 0) {
                    continue;  // 透明区不计（PS 直方图行为）
                }
                c->r[R]++;
                c->g[G]++;
                c->b[B]++;
                c->luma[(R * 77 + G * 150 + B * 29) >> 8]++;
            }
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<HistogramCtx*>(data);
            if (status == napi_ok && c->result.ok) {
                napi_value dataObj = nullptr;
                napi_create_object(env, &dataObj);
                napi_value arrs[4];
                const char* names[4] = {"luma", "r", "g", "b"};
                uint32_t* srcs[4] = {c->luma, c->r, c->g, c->b};
                for (int k = 0; k < 4; ++k) {
                    napi_create_array(env, &arrs[k]);
                    for (int i = 0; i < 256; ++i) {
                        napi_set_element(env, arrs[k], i, makeUint32(env, srcs[k][i]));
                    }
                    napi_set_named_property(env, dataObj, names[k], arrs[k]);
                }
                napi_resolve_deferred(env, c->deferred, makeOk(env, dataObj));
            } else {
                napi_resolve_deferred(env, c->deferred, makeError(env, kErrInternal, "histogram failed"));
            }
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

// M8c：图像大小重采样（history 事务；异步 work，同 DocGeomAsync）
napi_value ResampleDocument(napi_env env, napi_callback_info info) {
    return DocGeomAsync(env, info, 4);
}

// M19c：内容感知比例（活动图层缝雕刻到 w×h，选区灰度作保护图；画布随缩放；异步 work）
napi_value ContentAwareScale(napi_env env, napi_callback_info info) {
    return DocGeomAsync(env, info, 6);
}

// M8c：任意角旋转（度，顺时针；history 事务；异步 work，同 DocGeomAsync）
napi_value RotateDocumentArbitrary(napi_env env, napi_callback_info info) {
    return DocGeomAsync(env, info, 5);
}

// M8b：PSD 导出（锁内拍快照 + 扁平化任务，锁外编码写出；fd 全路径接管）
napi_value ExportPsd(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "exportPsd(fd) requires 1 arg");
    }
    int32_t fd = -1;
    if (napi_get_value_int32(env, argv[0], &fd) != napi_ok || fd < 0) {
        return makeError(env, kErrBadParam, "exportPsd: invalid fd");
    }
    auto& e = Engine::get();
    auto* ctx = new ExportCtx();
    ctx->fd = fd;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            delete ctx;
            close(fd);
            return makeError(env, kErrNoDoc, "exportPsd: no document");
        }
        io::PsdDocSnap snap;
        snap.workPath = e.doc.workPath;  // M18d：路径资源导出
        snap.paths = e.doc.paths;
        snap.width = e.doc.width;
        snap.height = e.doc.height;
        snap.name = e.doc.name;
        for (const Layer& l : e.doc.layers) {
            io::PsdLayerSnap pl;
            pl.name = l.name;
            pl.visible = l.visible;
            pl.opacity = l.opacity;
            pl.blendMode = l.blendMode;
            pl.clipping = l.clipping;
            pl.pixels = l.effectivePixels();
            // M15c：样式派生域 origin 独立于 transform（投影/描边扩展），导出须对齐像素
            pl.originX = l.effectiveOriginX();
            pl.originY = l.effectiveOriginY();
            if (l.mask != nullptr) {
                pl.maskGrid = l.mask->pixels;
                pl.maskW = l.mask->width;
                pl.maskH = l.mask->height;
                pl.maskOffsetX = l.mask->offsetX;
                pl.maskOffsetY = l.mask->offsetY;
                pl.maskOutside = l.mask->outside;
                pl.maskEnabled = l.mask->enabled;
            }
            pl.isAdjustment = l.adjustment != nullptr;
            pl.vectorMask = l.vectorMask;  // M18d：vmsk 导出
            // M15c（PSD-2）：组树 + 降级标记 + 调整层参数
            pl.id = l.id;
            pl.parentId = l.parentId;
            pl.isGroup = l.isGroup;
            pl.collapsed = l.collapsed;
            pl.isText = l.text != nullptr;
            pl.isFill = l.fill != nullptr;
            // M17c：样式 → lrFX 导出。仅当已启用效果全部可由 lrFX 五类表示（无光泽/斜面/
            // 渐变叠加/spread）才走「原始像素 + lrFX」；否则维持烘焙像素 + 降级 note，
            // 避免 PS 端像素已含效果再叠 lrFX 双重应用。
            if (l.style != nullptr && l.style->enabled) {
                const LayerStyle& st = *l.style;
                // M17d：自定义等高线/非默认 Range/图案叠加同样不可由 lrFX PS5 布局承载
                //（photocraft parse_lrfx 对 contour/range 硬编码 Linear/0.5）→ 走烘焙分流。
                const bool contourCustom =
                    st.shadowContour.custom || st.isContour.custom || st.ogContour.custom ||
                    st.igContour.custom || st.satinContour.custom || st.bevelContour.custom ||
                    st.bevelGlossContour.custom;
                const bool allRepresentable =
                    !st.satin && !st.bevel && !st.gradientOverlay && !st.patternOverlay &&
                    !contourCustom && st.shadowSpread == 0.0 && st.isSpread == 0.0 &&
                    st.ogSpread == 0.0 && st.igSpread == 0.0 && st.ogRange == 0.5 &&
                    st.igRange == 0.5;
                if (allRepresentable) {
                    pl.style = l.style;
                    pl.pixels = l.pixels;  // 原始像素（效果由 lrFX 承载，PS 端渲染）
                    pl.originX = l.transform.originX;
                    pl.originY = l.transform.originY;
                }
            }
            pl.hasStyle = l.style != nullptr && pl.style == nullptr;
            if (l.adjustment != nullptr) {
                pl.adjKind = static_cast<int32_t>(l.adjustment->kind);
                pl.adjInBlack = l.adjustment->inBlack;
                pl.adjInWhite = l.adjustment->inWhite;
                pl.adjGamma = l.adjustment->gamma;
                pl.adjCurveX = l.adjustment->curveX;
                pl.adjCurveY = l.adjustment->curveY;
            }
            snap.layers.push_back(std::move(pl));
        }
        ctx->psdSnap = std::move(snap);
        ctx->job = io::BuildFlattenJob(e.doc, 0, 0, 0, 0);
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montageExportPsd", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<ExportCtx*>(data);
            io::FlattenResult r = io::FlattenVisible(c->job, 0, 0, 0, 0);
            if (r.ok && !r.rgba.empty()) {
                c->psdSnap.flattenedRgba = std::make_shared<std::vector<uint8_t>>(std::move(r.rgba));
            }
            io::PsdExportResult pr = io::exportPsd(c->fd, c->psdSnap);
            c->fd = -1;
            c->psdResult = std::move(pr);
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<ExportCtx*>(data);
            if (c->fd >= 0) {
                close(c->fd);
                c->fd = -1;
            }
            if (status == napi_ok && c->psdResult.ok) {
                napi_value dataObj = nullptr;
                napi_create_object(env, &dataObj);
                napi_set_named_property(env, dataObj, "layers", makeUint32(env, c->psdResult.layers));
                napi_set_named_property(env, dataObj, "bytes", makeDouble(env, static_cast<double>(c->psdResult.bytes)));
                napi_value arr = nullptr;
                napi_create_array(env, &arr);
                uint32_t n = 0;
                for (const std::string& note : c->psdResult.notes) {
                    napi_set_element(env, arr, n++, makeString(env, note));
                }
                napi_set_named_property(env, dataObj, "notes", arr);
                napi_resolve_deferred(env, c->deferred, makeOk(env, dataObj));
            } else {
                napi_resolve_deferred(env, c->deferred,
                                      makeError(env, kErrInternal, c->psdResult.error.empty()
                                          ? "exportPsd failed" : c->psdResult.error));
            }
            napi_delete_async_work(env, c->work);
            delete c;
        },
        ctx, &ctx->work);
    if (st != napi_ok) {
        close(fd);
        delete ctx;
        return makeError(env, kErrInternal, "create async work failed");
    }
    napi_queue_async_work(env, ctx->work);
    return promise;
}

napi_value FlattenToPixelMap(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    auto* ctx = new ExportCtx();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            delete ctx;
            return makeError(env, kErrNoDoc, "flattenToPixelMap: no document");
        }
        ctx->job = io::BuildFlattenJob(e.doc, 0, 0, 0, 0);
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montageFlatten", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<ExportCtx*>(data);
            c->result = io::FlattenVisible(c->job, 0, 0, 0, 0);
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<ExportCtx*>(data);
            if (status != napi_ok || !c->result.ok || c->result.rgba.empty()) {
                napi_resolve_deferred(env, c->deferred,
                                      makeError(env, kErrInternal, c->result.error.empty()
                                          ? "flatten failed" : c->result.error));
                napi_delete_async_work(env, c->work);
                delete c;
                return;
            }
            // RGBA_8888 直 alpha → PixelMap（显式声明格式与 alphaType，M4b 教训）
            OH_Pixelmap_InitializationOptions* opts = nullptr;
            OH_PixelmapInitializationOptions_Create(&opts);
            if (opts != nullptr) {
                OH_PixelmapInitializationOptions_SetWidth(opts, c->result.width);
                OH_PixelmapInitializationOptions_SetHeight(opts, c->result.height);
                OH_PixelmapInitializationOptions_SetPixelFormat(opts, PIXEL_FORMAT_RGBA_8888);
                OH_PixelmapInitializationOptions_SetAlphaType(opts, PIXELMAP_ALPHA_TYPE_UNPREMULTIPLIED);
            }
            OH_PixelmapNative* pm = nullptr;
            Image_ErrorCode crc = OH_PixelmapNative_CreatePixelmap(
                const_cast<uint8_t*>(c->result.rgba.data()), c->result.rgba.size(), opts, &pm);
            if (opts != nullptr) {
                OH_PixelmapInitializationOptions_Release(opts);
            }
            if (crc != IMAGE_SUCCESS || pm == nullptr) {
                napi_resolve_deferred(env, c->deferred,
                                      makeError(env, kErrInternal, "createPixelmap rc=" +
                                          std::to_string(static_cast<int>(crc))));
                napi_delete_async_work(env, c->work);
                delete c;
                return;
            }
            napi_value pmValue = nullptr;
            crc = OH_PixelmapNative_ConvertPixelmapNativeToNapi(env, pm, &pmValue);
            OH_PixelmapNative_Release(pm);  // napi 值持独立引用（M0 runPixelTest 同款）
            if (crc != IMAGE_SUCCESS || pmValue == nullptr) {
                napi_resolve_deferred(env, c->deferred,
                                      makeError(env, kErrInternal, "convert napi rc=" +
                                          std::to_string(static_cast<int>(crc))));
                napi_delete_async_work(env, c->work);
                delete c;
                return;
            }
            napi_value dataObj = nullptr;
            napi_create_object(env, &dataObj);
            napi_set_named_property(env, dataObj, "width", makeUint32(env, c->result.width));
            napi_set_named_property(env, dataObj, "height", makeUint32(env, c->result.height));
            napi_set_named_property(env, dataObj, "pixelMap", pmValue);
            napi_resolve_deferred(env, c->deferred, makeOk(env, dataObj));
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

// ---------- M12 文字层 ----------

namespace {
bool getDoubleArg(napi_env env, napi_value v, double* out) {
    return napi_get_value_double(env, v, out) == napi_ok;
}
}  // namespace

// M12：创建文本层（x, y, wrapW；wrapW>0 = 段落文字换行宽）。颜色取前景色（PS 语义），
// 内容空（无像素），插入活动层之上并激活；本身是一步撤销（"新增文本层"）。
napi_value CreateTextLayer(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "createTextLayer(x, y, wrapW?) requires 2+ args");
    }
    double x = 0, y = 0, wrapW = 0;
    if (!getDoubleArg(env, argv[0], &x) || !getDoubleArg(env, argv[1], &y) ||
        (argc >= 3 && !getDoubleArg(env, argv[2], &wrapW))) {
        return makeError(env, kErrBadParam, "createTextLayer: invalid args");
    }
    auto& e = Engine::get();
    double newId = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "createTextLayer: no document");
        }
        if (e.textEditOpen) {
            return makeError(env, kErrBadParam, "createTextLayer: text edit session open");
        }
        e.history.beginEdit(e.doc, "add text", static_cast<uint64_t>(e.docRevision));
        Layer layer;
        layer.id = e.nextLayerId();
        layer.name = "文本";
        auto t = std::make_shared<TextLayerState>();
        t->color[0] = e.fgColor[0];
        t->color[1] = e.fgColor[1];
        t->color[2] = e.fgColor[2];
        t->wrapWidth = wrapW > 0.0 ? wrapW : 0.0;
        layer.text = std::move(t);
        layer.transform.originX = x;
        layer.transform.originY = y;
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

// M12：设置文本内容（id, content, txn）。txn=1 独立撤销事务；txn=0 编辑会话内调用
//（begin/endTextSession 包裹，整会话一步撤销）。层名跟随内容（PS 语义）。
napi_value SetTextLayerContent(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam,
                         "setTextLayerContent(id, content, txn?) requires 2+ args");
    }
    double id = 0;
    size_t len = 0;
    int32_t txn = 1;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_string_utf8(env, argv[1], nullptr, 0, &len) != napi_ok ||
        (argc >= 3 && napi_get_value_int32(env, argv[2], &txn) != napi_ok)) {
        return makeError(env, kErrBadParam, "setTextLayerContent: invalid args");
    }
    std::string content(len, '\0');
    if (len > 0 &&
        napi_get_value_string_utf8(env, argv[1], &content[0], len + 1, &len) != napi_ok) {
        return makeError(env, kErrBadParam, "setTextLayerContent: read string failed");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr || l->text == nullptr) {
            return makeError(env, kErrBadParam, "setTextLayerContent: text layer not found");
        }
        if (l->text->content == content) {
            return makeOk(env, nullptr);  // 无变化（debounce 抖动）
        }
        if (txn != 0) {
            e.history.beginEdit(e.doc, "text edit", static_cast<uint64_t>(e.docRevision));
        }
        auto nt = std::make_shared<TextLayerState>(*l->text);  // COW
        nt->content = content;
        l->text = std::move(nt);
        std::string err;
        relayoutTextLayer(*l, err);
        l->name = textLayerDisplayName(*l->text);
        if (txn != 0) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        }
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M12：设置文本样式全量（id, fontFamily, fontSize, lineHeight, tracking, bold, italic,
// r, g, b, align, wrapWidth, txn）。字符/段落面板变更入口；txn 语义同 setContent。
napi_value SetTextLayerStyle(napi_env env, napi_callback_info info) {
    size_t argc = 13;
    napi_value argv[13];
    for (size_t i = 0; i < 13; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 12) {
        return makeError(env, kErrBadParam,
                         "setTextLayerStyle(id, family, size, lh, tr, bold, italic, r, g, b, "
                         "align, wrapW, txn?) requires 12+ args");
    }
    double id = 0, fontSize = 0, lineHeight = 0, tracking = 0;
    double r = 0, g = 0, b = 0, align = 0, wrapW = 0;
    size_t famLen = 0;
    bool bold = false, italic = false;
    int32_t txn = 1;
    bool ok = getDoubleArg(env, argv[0], &id) &&
              (napi_get_value_string_utf8(env, argv[1], nullptr, 0, &famLen) == napi_ok) &&
              getDoubleArg(env, argv[2], &fontSize) && getDoubleArg(env, argv[3], &lineHeight) &&
              getDoubleArg(env, argv[4], &tracking) &&
              napi_get_value_bool(env, argv[5], &bold) == napi_ok &&
              napi_get_value_bool(env, argv[6], &italic) == napi_ok &&
              getDoubleArg(env, argv[7], &r) && getDoubleArg(env, argv[8], &g) &&
              getDoubleArg(env, argv[9], &b) && getDoubleArg(env, argv[10], &align) &&
              getDoubleArg(env, argv[11], &wrapW);
    if (argc >= 13) {
        ok = ok && napi_get_value_int32(env, argv[12], &txn) == napi_ok;
    }
    if (!ok) {
        return makeError(env, kErrBadParam, "setTextLayerStyle: invalid args");
    }
    std::string family(famLen, '\0');
    if (famLen > 0 &&
        napi_get_value_string_utf8(env, argv[1], &family[0], famLen + 1, &famLen) != napi_ok) {
        return makeError(env, kErrBadParam, "setTextLayerStyle: read family failed");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr || l->text == nullptr) {
            return makeError(env, kErrBadParam, "setTextLayerStyle: text layer not found");
        }
        if (txn != 0) {
            e.history.beginEdit(e.doc, "text style", static_cast<uint64_t>(e.docRevision));
        }
        auto nt = std::make_shared<TextLayerState>(*l->text);  // COW
        nt->fontFamily = family;
        nt->fontSize = std::min(800.0, std::max(4.0, fontSize));
        nt->lineHeight = std::min(4.0, std::max(0.5, lineHeight));
        nt->tracking = std::min(500.0, std::max(-500.0, tracking));
        nt->bold = bold;
        nt->italic = italic;
        nt->color[0] = static_cast<float>(std::min(1.0, std::max(0.0, r)));
        nt->color[1] = static_cast<float>(std::min(1.0, std::max(0.0, g)));
        nt->color[2] = static_cast<float>(std::min(1.0, std::max(0.0, b)));
        nt->align = static_cast<int32_t>(std::min(2.0, std::max(0.0, align)));
        nt->wrapWidth = wrapW > 0.0 ? wrapW : 0.0;
        l->text = std::move(nt);
        std::string err;
        relayoutTextLayer(*l, err);
        if (txn != 0) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        }
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M12：栅格化文本层 → 普通像素层（一步撤销；此后不可再编辑）
napi_value RasterizeTextLayer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double id = 0;
    if (argc < 1 || napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "rasterizeTextLayer(id) requires 1 arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr || l->text == nullptr) {
            return makeError(env, kErrBadParam, "rasterizeTextLayer: text layer not found");
        }
        if (e.textEditOpen && e.textEditLayerId == l->id) {
            return makeError(env, kErrBadParam, "rasterizeTextLayer: edit session open");
        }
        e.history.beginEdit(e.doc, "rasterize text", static_cast<uint64_t>(e.docRevision));
        rasterizeTextLayerToPixels(*l);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M12：编辑会话（PS 语义：整个编辑会话 = 一步撤销）。begin 持 history pending 槽，
// 期间内容/样式走 txn=0；end 提交（before==after 自动丢弃）。
napi_value BeginTextSession(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double id = 0;
    if (argc < 1 || napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "beginTextSession(id) requires 1 arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr || l->text == nullptr) {
            return makeError(env, kErrBadParam, "beginTextSession: text layer not found");
        }
        if (e.textEditOpen) {
            return makeError(env, kErrBadParam, "beginTextSession: already open");
        }
        e.textEditOpen = true;
        e.textEditLayerId = l->id;
        e.history.beginEdit(e.doc, "text edit", static_cast<uint64_t>(e.docRevision));
        e.requestRender();  // 编辑层从 GL 隐藏（渲染快照按 textEditOpen 跳过）
    }
    return makeOk(env, nullptr);
}

napi_value EndTextSession(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.textEditOpen) {
            e.textEditOpen = false;
            e.textEditLayerId = 0;
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            e.historyVersion.fetch_add(1);
            e.requestRender();  // 编辑层回 GL 显示
        }
    }
    return makeOk(env, nullptr);
}

// M12：系统字体族名列表（去重排序；FontParser 系统字体列表，与排版解析同源）
napi_value ListSystemFonts(napi_env env, napi_callback_info /*info*/) {
    std::vector<std::string> fonts;
    std::string err;
    if (!systemFontFamilies(fonts, err)) {
        return makeError(env, kErrInternal, "listSystemFonts: " + err);
    }
    napi_value arr = nullptr;
    napi_create_array_with_length(env, fonts.size(), &arr);
    for (size_t i = 0; i < fonts.size(); ++i) {
        napi_value s = nullptr;
        napi_create_string_utf8(env, fonts[i].c_str(), NAPI_AUTO_LENGTH, &s);
        napi_set_element(env, arr, static_cast<uint32_t>(i), s);
    }
    return arr;
}

// M12：注册用户字体文件（alias = 工程内引用族名，path = 沙箱 ttf/otf 路径）。
// 注册成功后全文档文本层重栅格（缺字体层即时恢复正确字形）。
napi_value RegisterFontFile(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    size_t aliasLen = 0, pathLen = 0;
    if (argc < 2 ||
        napi_get_value_string_utf8(env, argv[0], nullptr, 0, &aliasLen) != napi_ok ||
        napi_get_value_string_utf8(env, argv[1], nullptr, 0, &pathLen) != napi_ok) {
        return makeError(env, kErrBadParam, "registerFontFile(alias, path) requires 2 args");
    }
    std::string alias(aliasLen, '\0');
    std::string path(pathLen, '\0');
    if ((aliasLen > 0 &&
         napi_get_value_string_utf8(env, argv[0], &alias[0], aliasLen + 1, &aliasLen) != napi_ok) ||
        (pathLen > 0 &&
         napi_get_value_string_utf8(env, argv[1], &path[0], pathLen + 1, &pathLen) != napi_ok)) {
        return makeError(env, kErrBadParam, "registerFontFile: read string failed");
    }
    std::string err;
    if (!registerUserFont(alias, path, err)) {
        return makeError(env, kErrInternal, "registerFontFile: " + err);
    }
    auto& e = Engine::get();
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        std::string relayoutErr;
        n = relayoutDocumentText(e.doc, relayoutErr);
        if (n > 0) {
            e.bumpRevisionLocked();
            e.requestRender();
        }
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "relayouted", makeInt32(env, n));
    return makeOk(env, data);
}

// ---------- M13 滤镜 ----------

namespace {
// argv[0]=kind, argv[1..6]=p0..p5, argv[7]=seed（相对偏移由调用侧决定）
bool readFilterArgs(napi_env env, napi_value* argv, size_t argc, filters::FilterParams& p) {
    double kind = 0, seed = 0;
    if (argc < 1 || napi_get_value_double(env, argv[0], &kind) != napi_ok) {
        return false;
    }
    for (int i = 0; i < 6 && 1 + i < static_cast<int>(argc); ++i) {
        double v = 0;
        if (napi_get_value_double(env, argv[1 + i], &v) != napi_ok) {
            return false;
        }
        (&p.p0)[i] = static_cast<float>(v);
    }
    if (argc >= 8 && napi_get_value_double(env, argv[7], &seed) != napi_ok) {
        return false;
    }
    p.kind = static_cast<int32_t>(kind);
    p.seed = static_cast<uint32_t>(seed);
    return p.kind >= 0 && p.kind < filters::kFilterKindCount;
}
}  // namespace

// M13：应用滤镜（layerId, kind, p0..p5, seed；一步 History 事务；选区约束自动）
napi_value ApplyFilter(napi_env env, napi_callback_info info) {
    size_t argc = 9;
    napi_value argv[9];
    for (size_t i = 0; i < 9; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double id = 0;
    if (argc < 2 || napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "applyFilter(layerId, kind, p0..p5, seed?) invalid");
    }
    filters::FilterParams p;
    if (!readFilterArgs(env, argv + 1, argc - 1, p)) {
        return makeError(env, kErrBadParam, "applyFilter: bad params");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0) {
            return makeError(env, kErrNoDoc, "applyFilter: no document");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            return makeError(env, kErrBadParam, "applyFilter: edit/preview session open");
        }
        e.history.beginEdit(e.doc, "filter", static_cast<uint64_t>(e.docRevision));
        std::string err;
        if (!applyFilterToLayer(e.doc, static_cast<LayerId>(id), p, err)) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));  // before==after 丢弃
            return makeError(env, kErrBadParam, "applyFilter: " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M13：开始预览（layerId）——建 1/4 proxy 源（4×4 块平均降采样）
napi_value BeginFilterPreview(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double id = 0;
    if (argc < 1 || napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "beginFilterPreview(layerId) invalid");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0) {
            return makeError(env, kErrNoDoc, "beginFilterPreview: no document");
        }
        Layer* layer = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (layer == nullptr || layer->pixels == nullptr) {
            return makeError(env, kErrBadParam, "beginFilterPreview: empty layer");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            return makeError(env, kErrBadParam, "beginFilterPreview: session open");
        }
        uint32_t gx = 0, gy = 0, w = 0, h = 0;
        if (!layerContentBBox(*layer->pixels, gx, gy, w, h)) {
            return makeError(env, kErrBadParam, "beginFilterPreview: no content");
        }
        std::vector<uint8_t> strip;
        filterGridToStrip(*layer->pixels, gx, gy, w, h, strip);
        auto& fp = e.filterPreview;
        fp = Engine::FilterPreview{};
        fp.active = true;
        fp.layerId = static_cast<LayerId>(id);
        fp.fullW = w;
        fp.fullH = h;
        fp.originX = layer->transform.originX + gx;
        fp.originY = layer->transform.originY + gy;
        const uint32_t qw = (w + 3u) / 4u;
        const uint32_t qh = (h + 3u) / 4u;
        fp.srcW = qw;
        fp.srcH = qh;
        fp.srcStrip.assign(static_cast<size_t>(qw) * qh * 4u, 0);
        for (uint32_t y = 0; y < qh; ++y) {
            for (uint32_t x = 0; x < qw; ++x) {
                uint32_t acc[4] = {0, 0, 0, 0};
                uint32_t cnt = 0;
                for (uint32_t sy = y * 4u; sy < std::min(h, (y + 1u) * 4u); ++sy) {
                    for (uint32_t sx = x * 4u; sx < std::min(w, (x + 1u) * 4u); ++sx) {
                        const uint8_t* px = strip.data() + (static_cast<size_t>(sy) * w + sx) * 4u;
                        for (int c = 0; c < 4; ++c) {
                            acc[c] += px[c];
                        }
                        ++cnt;
                    }
                }
                uint8_t* d = fp.srcStrip.data() + (static_cast<size_t>(y) * qw + x) * 4u;
                for (int c = 0; c < 4; ++c) {
                    d[c] = cnt > 0 ? static_cast<uint8_t>(acc[c] / cnt) : 0;
                }
            }
        }
        fp.scale = 4.0;
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x4D30, "Montage.Filter",
                     "preview begin: layer %{public}llu domain (%{public}u,%{public}u) %{public}u x %{public}u proxy %{public}u x %{public}u",
                     static_cast<unsigned long long>(fp.layerId), gx, gy, w, h, fp.srcW, fp.srcH);
    }
    return makeOk(env, nullptr);
}

// M13：预览跑核（kind, p0..p5, seed；proxy 网格发布 → GL 替换该层显示）
napi_value SetFilterPreview(napi_env env, napi_callback_info info) {
    size_t argc = 8;
    napi_value argv[8];
    for (size_t i = 0; i < 8; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    filters::FilterParams p;
    if (!readFilterArgs(env, argv, argc, p)) {
        return makeError(env, kErrBadParam, "setFilterPreview: bad params");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Engine::FilterPreview& fp = e.filterPreview;
        if (!fp.active || fp.srcStrip.empty()) {
            return makeError(env, kErrBadParam, "setFilterPreview: no preview session");
        }
        p.colorFg[0] = e.fgColor[0];
        p.colorFg[1] = e.fgColor[1];
        p.colorFg[2] = e.fgColor[2];
        p.colorBg[0] = e.bgColor[0];
        p.colorBg[1] = e.bgColor[1];
        p.colorBg[2] = e.bgColor[2];
        std::vector<uint8_t> out(fp.srcStrip.size(), 0);
        std::string err;
        if (!filters::applyFilter(fp.srcStrip.data(), out.data(), fp.srcW, fp.srcH, p, err)) {
            return makeError(env, kErrBadParam, "setFilterPreview: " + err);
        }
        fp.proxy = filterStripToGrid(out, fp.srcW, fp.srcH,
                                     fp.proxy != nullptr ? fp.proxy->revision + 1 : 1);
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x4D30, "Montage.Filter",
                     "preview proxy published: %{public}u x %{public}u", fp.srcW, fp.srcH);
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M13：结束预览（commit=true → 全分辨率核 + 一步事务；false → 丢弃）
napi_value EndFilterPreview(napi_env env, napi_callback_info info) {
    size_t argc = 10;
    napi_value argv[10];
    for (size_t i = 0; i < 10; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t commit = 0;
    if (argc >= 1 && napi_get_value_int32(env, argv[0], &commit) != napi_ok) {
        commit = 0;
    }
    // M20-UI：可选第 10 参 = 历史标签（滤镜名；缺省 "filter"）
    std::string histLabel = "filter";
    if (argc >= 10 && argv[9] != nullptr) {
        size_t len = 0;
        if (napi_get_value_string_utf8(env, argv[9], nullptr, 0, &len) == napi_ok && len > 0 &&
            len < 128) {
            char buf[128];
            if (napi_get_value_string_utf8(env, argv[9], buf, sizeof(buf), &len) == napi_ok) {
                histLabel = buf;
            }
        }
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Engine::FilterPreview& fp = e.filterPreview;
        if (fp.active) {
            if (commit != 0) {
                filters::FilterParams p;
                if (readFilterArgs(env, argv + 1, argc - 1, p)) {
                    p.colorFg[0] = e.fgColor[0];
                    p.colorFg[1] = e.fgColor[1];
                    p.colorFg[2] = e.fgColor[2];
                    p.colorBg[0] = e.bgColor[0];
                    p.colorBg[1] = e.bgColor[1];
                    p.colorBg[2] = e.bgColor[2];
                    e.history.beginEdit(e.doc, histLabel, static_cast<uint64_t>(e.docRevision));
                    std::string err;
                    if (applyFilterToLayer(e.doc, fp.layerId, p, err)) {
                        restyleAllLocked(e);
                        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
                        e.bumpRevisionLocked();
                    } else {
                        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
                        OH_LOG_Print(LOG_APP, LOG_WARN, 0x4D30, "Montage.Filter",
                                     "commit failed: %{public}s", err.c_str());
                    }
                }
            }
            fp = Engine::FilterPreview{};
            e.requestRender();
        }
    }
    return makeOk(env, nullptr);
}

// ---------- M15e RAW 显影参数面板 ----------

// RAW 重显影异步任务上下文（锁内快照 RAW 字节 → libuv 线程锁外显影 → JS 线程 COW 提交）
struct DevelopRawCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::vector<uint8_t> bytes;
    LayerId layerId = 0;
    io::DevelopParams params;
    bool ok = false;
    uint32_t w = 0;
    uint32_t h = 0;
    std::string err;
    std::shared_ptr<const TileGrid> grid;
};

// developRaw(temp, tint, ev, gamma)：从会话 RAW 缓存全分辨率重显影（恒从 RAW 出发，
// 会话内非破坏），COW 换层像素 + 一步 history 事务。v1 无实时预览（登记后续）。
napi_value DevelopRaw(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        return makeError(env, kErrBadParam, "developRaw(temp,tint,ev,gamma) requires 4 args");
    }
    double temp = 0;
    double tint = 0;
    double ev = 0;
    double gamma = 1;
    if (napi_get_value_double(env, argv[0], &temp) != napi_ok ||
        napi_get_value_double(env, argv[1], &tint) != napi_ok ||
        napi_get_value_double(env, argv[2], &ev) != napi_ok ||
        napi_get_value_double(env, argv[3], &gamma) != napi_ok) {
        return makeError(env, kErrBadParam, "developRaw: invalid args");
    }
    auto& e = Engine::get();
    auto* ctx = new DevelopRawCtx();
    ctx->params.temp = static_cast<float>(std::min(100.0, std::max(-100.0, temp)));
    ctx->params.tint = static_cast<float>(std::min(100.0, std::max(-100.0, tint)));
    ctx->params.ev = static_cast<float>(std::min(2.0, std::max(-2.0, ev)));
    ctx->params.gamma = static_cast<float>(std::min(2.0, std::max(0.5, gamma)));
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (!e.rawDevelop.valid || e.rawDevelop.bytes.empty()) {
            delete ctx;
            return makeError(env, kErrNoDoc, "developRaw: no RAW session (import a DNG first)");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            delete ctx;
            return makeError(env, kErrBadParam, "developRaw: session open");
        }
        ctx->bytes = e.rawDevelop.bytes;
        ctx->layerId = e.rawDevelop.layerId;
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value workName = nullptr;
    napi_create_string_utf8(env, "montageDevelopRaw", NAPI_AUTO_LENGTH, &workName);
    napi_status st = napi_create_async_work(
        env, nullptr, workName,
        [](napi_env /*env*/, void* data) {
            auto* c = static_cast<DevelopRawCtx*>(data);
            c->ok = io::developDngToGrid(c->bytes, c->params, c->grid, c->w, c->h, c->err);
        },
        [](napi_env env, napi_status status, void* data) {
            auto* c = static_cast<DevelopRawCtx*>(data);
            auto& e = Engine::get();
            if (status == napi_ok && c->ok) {
                bool applied = false;
                {
                    std::lock_guard<std::mutex> lk(e.docMutex);
                    for (Layer& l : e.doc.layers) {
                        if (l.id != c->layerId) {
                            continue;
                        }
                        // 同一 RAW 同尺寸：transform 不动，仅 COW 换像素 + 一步事务
                        e.history.beginEdit(e.doc, "RAW 显影",
                                            static_cast<uint64_t>(e.docRevision));
                        l.pixels = c->grid;
                        restyleAllLocked(e);
                        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
                        e.bumpRevisionLocked();
                        e.rawDevelop.last = c->params;
                        applied = true;
                        break;
                    }
                    if (!applied) {
                        c->err = "developRaw: RAW layer no longer exists";
                    }
                }
                if (applied) {
                    e.requestRender();
                    napi_value dataObj = nullptr;
                    napi_create_object(env, &dataObj);
                    napi_set_named_property(env, dataObj, "width", makeUint32(env, c->w));
                    napi_set_named_property(env, dataObj, "height", makeUint32(env, c->h));
                    napi_set_named_property(env, dataObj, "layerId",
                                            makeDouble(env, static_cast<double>(c->layerId)));
                    napi_resolve_deferred(env, c->deferred, makeOk(env, dataObj));
                } else {
                    napi_resolve_deferred(env, c->deferred,
                                          makeError(env, kErrInternal, c->err));
                }
            } else {
                napi_resolve_deferred(env, c->deferred,
                                      makeError(env, kErrInternal,
                                                c->err.empty() ? "developRaw failed" : c->err));
            }
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

// ---------- M14a 图层编组 ----------
// M14a：新建空组（插活动层上方，激活）
napi_value AddGroup(napi_env env, napi_callback_info info) {
    auto& e = Engine::get();
    double newId = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0) {
            return makeError(env, kErrNoDoc, "addGroup: no document");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            return makeError(env, kErrBadParam, "addGroup: session open");
        }
        e.history.beginEdit(e.doc, "new group", static_cast<uint64_t>(e.docRevision));
        std::string err;
        const LayerId id = createGroup(e.doc, e.nextLayerId(), "", err);
        if (id == 0) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "addGroup: " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        newId = static_cast<double>(id);
        e.bumpRevisionLocked();
        e.requestRender();
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "id", makeDouble(env, newId));
    return makeOk(env, data);
}

// M14a：活动层包进新建组（PS Ctrl+G 单层语义）
napi_value GroupLayer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double id = 0;
    if (argc < 1 || napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "groupLayer(id) requires 1 arg");
    }
    auto& e = Engine::get();
    double newId = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.history.beginEdit(e.doc, "group layer", static_cast<uint64_t>(e.docRevision));
        std::string err;
        const LayerId gid = groupLayer(e.doc, e.nextLayerId(), static_cast<LayerId>(id), err);
        if (gid == 0) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "groupLayer: " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        newId = static_cast<double>(gid);
        e.bumpRevisionLocked();
        e.requestRender();
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "id", makeDouble(env, newId));
    return makeOk(env, data);
}

// M14a：解散组（子层上提）
napi_value UngroupLayer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double id = 0;
    if (argc < 1 || napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "ungroupLayer(id) requires 1 arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.history.beginEdit(e.doc, "ungroup", static_cast<uint64_t>(e.docRevision));
        std::string err;
        if (!ungroupLayer(e.doc, static_cast<LayerId>(id), err)) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "ungroupLayer: " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M14a：移动层到父/索引（拖拽重排与入组/出组）
napi_value SetLayerParent(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double id = 0, parent = 0, index = 0;
    if (argc < 3 || napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_double(env, argv[1], &parent) != napi_ok ||
        napi_get_value_double(env, argv[2], &index) != napi_ok) {
        return makeError(env, kErrBadParam, "setLayerParent(id, parent, index) requires 3 args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.history.beginEdit(e.doc, "reorder layer", static_cast<uint64_t>(e.docRevision));
        std::string err;
        if (!setLayerParent(e.doc, static_cast<LayerId>(id), static_cast<LayerId>(parent),
                            static_cast<int>(index), err)) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "setLayerParent: " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// M14a：组折叠
napi_value SetGroupCollapsed(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double id = 0;
    int32_t collapsed = 0;
    if (argc < 2 || napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_int32(env, argv[1], &collapsed) != napi_ok) {
        return makeError(env, kErrBadParam, "setGroupCollapsed(id, collapsed) requires 2 args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        std::string err;
        if (!setGroupCollapsed(e.doc, static_cast<LayerId>(id), collapsed != 0, err)) {
            return makeError(env, kErrBadParam, "setGroupCollapsed: " + err);
        }
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// ---------- M14b 合并族 / 复制 / 链接 / 对齐分布 ----------

// id 数组参数（@State 代理数组跨桥 is_array 失败——M10a 实证，逐元素探测长度）
bool readLayerIdArray(napi_env env, napi_value v, std::vector<LayerId>& out) {
    uint32_t n = 0;
    napi_get_array_length(env, v, &n);
    if (n == 0) {
        const uint32_t kMaxProbe = 4096;
        for (uint32_t i = 0; i < kMaxProbe; ++i) {
            napi_value e = nullptr;
            if (napi_get_element(env, v, i, &e) != napi_ok || e == nullptr) {
                break;
            }
            napi_valuetype vt = napi_undefined;
            napi_typeof(env, e, &vt);
            if (vt != napi_number) {
                break;
            }
            n = i + 1;
        }
    }
    out.clear();
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        napi_value e = nullptr;
        if (napi_get_element(env, v, i, &e) != napi_ok) {
            return false;
        }
        double d = 0;
        if (napi_get_value_double(env, e, &d) != napi_ok) {
            return false;
        }
        out.push_back(static_cast<LayerId>(d));
    }
    return true;
}

// 合并族共用骨架：session 守卫 + history 事务 + 刷新
napi_value runLayerMergeCommand(napi_env env, const char* what,
                                bool (*fn)(Document&, std::string&)) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, std::string(what) + ": no document");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            return makeError(env, kErrBadParam, std::string(what) + ": session open");
        }
        e.history.beginEdit(e.doc, what, static_cast<uint64_t>(e.docRevision));
        std::string err;
        if (!fn(e.doc, err)) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, std::string(what) + ": " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value MergeDown(napi_env env, napi_callback_info /*info*/) {
    return runLayerMergeCommand(env, "merge down", mergeDown);
}

napi_value MergeVisible(napi_env env, napi_callback_info /*info*/) {
    return runLayerMergeCommand(env, "merge visible", mergeVisible);
}

napi_value FlattenImage(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "flattenImage: no document");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            return makeError(env, kErrBadParam, "flattenImage: session open");
        }
        e.history.beginEdit(e.doc, "flatten image", static_cast<uint64_t>(e.docRevision));
        std::string err;
        if (!flattenImage(e.doc, e.nextLayerId(), err)) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "flattenImage: " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value StampVisible(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    double newId = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "stampVisible: no document");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            return makeError(env, kErrBadParam, "stampVisible: session open");
        }
        e.history.beginEdit(e.doc, "stamp visible", static_cast<uint64_t>(e.docRevision));
        std::string err;
        const LayerId id = e.nextLayerId();
        const std::string name = "图层 " + std::to_string(id);
        const LayerId got = stampVisible(e.doc, id, name, err);
        if (got == 0) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "stampVisible: " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        newId = static_cast<double>(got);
        e.bumpRevisionLocked();
        e.requestRender();
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "id", makeDouble(env, newId));
    return makeOk(env, data);
}

napi_value DuplicateLayer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double id = 0;
    if (argc >= 1 && napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "duplicateLayer(id?) invalid");
    }
    auto& e = Engine::get();
    double newId = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "duplicateLayer: no document");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            return makeError(env, kErrBadParam, "duplicateLayer: session open");
        }
        LayerId src = static_cast<LayerId>(id);
        if (src == 0) {
            src = e.doc.activeId;
        }
        e.history.beginEdit(e.doc, "duplicate layer", static_cast<uint64_t>(e.docRevision));
        std::string err;
        const LayerId got = duplicateLayer(e.doc, src, e.nextLayerId(),
                                           [&e] { return e.nextLayerId(); }, err);
        if (got == 0) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "duplicateLayer: " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        newId = static_cast<double>(got);
        e.bumpRevisionLocked();
        e.requestRender();
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "id", makeDouble(env, newId));
    return makeOk(env, data);
}

napi_value SetLayerLink(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t link = 0;
    if (argc < 2 || napi_get_value_int32(env, argv[1], &link) != napi_ok) {
        return makeError(env, kErrBadParam, "setLayerLink(ids, link) requires 2 args");
    }
    std::vector<LayerId> ids;
    if (!readLayerIdArray(env, argv[0], ids)) {
        return makeError(env, kErrBadParam, "setLayerLink: bad ids");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "setLayerLink: no document");
        }
        e.history.beginEdit(e.doc, link ? "link layers" : "unlink layers",
                            static_cast<uint64_t>(e.docRevision));
        std::string err;
        if (!setLayerLink(e.doc, ids, link != 0, err)) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "setLayerLink: " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value AlignLayers(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t mode = 0;
    if (argc < 2 || napi_get_value_int32(env, argv[1], &mode) != napi_ok) {
        return makeError(env, kErrBadParam, "alignLayers(ids, mode) requires 2 args");
    }
    std::vector<LayerId> ids;
    if (!readLayerIdArray(env, argv[0], ids)) {
        return makeError(env, kErrBadParam, "alignLayers: bad ids");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "alignLayers: no document");
        }
        e.history.beginEdit(e.doc, "align layers", static_cast<uint64_t>(e.docRevision));
        std::string err;
        if (!alignLayers(e.doc, ids, mode, err)) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "alignLayers: " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value DistributeLayers(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t mode = 0;
    if (argc < 2 || napi_get_value_int32(env, argv[1], &mode) != napi_ok) {
        return makeError(env, kErrBadParam, "distributeLayers(ids, mode) requires 2 args");
    }
    std::vector<LayerId> ids;
    if (!readLayerIdArray(env, argv[0], ids)) {
        return makeError(env, kErrBadParam, "distributeLayers: bad ids");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "distributeLayers: no document");
        }
        e.history.beginEdit(e.doc, "distribute layers", static_cast<uint64_t>(e.docRevision));
        std::string err;
        if (!distributeLayers(e.doc, ids, mode, err)) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "distributeLayers: " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// ---------- M14c 图层样式 / 填充图层 ----------

// 样式命令共用：定位层（0=活动）+ session 守卫 + 事务 + restyle + 刷新
napi_value runStyleCommand(napi_env env, const char* what, LayerId id,
                           const std::function<bool(Document&, Layer&, std::string&)>& apply) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, std::string(what) + ": no document");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            return makeError(env, kErrBadParam, std::string(what) + ": session open");
        }
        Layer* l = id > 0 ? findLayerLocked(e.doc, id) : nullptr;
        if (l == nullptr) {
            for (Layer& li : e.doc.layers) {
                if (li.id == e.doc.activeId) {
                    l = &li;
                    break;
                }
            }
        }
        if (l == nullptr) {
            return makeError(env, kErrNoDoc, std::string(what) + ": no active layer");
        }
        if (l->isGroup) {
            return makeError(env, kErrBadParam, std::string(what) + ": group has no style");
        }
        e.history.beginEdit(e.doc, what, static_cast<uint64_t>(e.docRevision));
        std::string err;
        if (!apply(e.doc, *l, err)) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, std::string(what) + ": " + err);
        }
        if (!restyleLayer(e.doc, *l, nextGridRevision(), err)) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, std::string(what) + ": " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// 投影：setDropShadow(id, enabled, angleDeg, dist, size, opacity, r, g, b)（色 0..255）
napi_value SetDropShadow(napi_env env, napi_callback_info info) {
    size_t argc = 11;
    napi_value argv[11];
    for (size_t i = 0; i < 11; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double v[11] = {0};
    if (argc < 11) {
        return makeError(env, kErrBadParam,
                         "setDropShadow(id,enabled,angle,dist,size,opacity,r,g,b,spread,useGL) requires 11");
    }
    for (size_t i = 0; i < 11; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setDropShadow: invalid args");
        }
    }
    const LayerId id = static_cast<LayerId>(v[0]);
    return runStyleCommand(env, "drop shadow", id,
                           [v](Document&, Layer& l, std::string&) -> bool {
        if (l.style == nullptr) {
            l.style = std::make_shared<LayerStyle>();
        } else {
            l.style = std::make_shared<LayerStyle>(*l.style);  // COW：快照共享指针不可原地改
        }
        l.style->dropShadow = v[1] != 0;
        l.style->shadowAngleDeg = v[2];
        l.style->shadowDistance = v[3];
        l.style->shadowSize = v[4];
        l.style->shadowOpacity = std::clamp(v[5], 0.0, 1.0);
        l.style->shadowColor[0] = static_cast<float>(v[6] / 255.0);
        l.style->shadowColor[1] = static_cast<float>(v[7] / 255.0);
        l.style->shadowColor[2] = static_cast<float>(v[8] / 255.0);
        l.style->shadowSpread = std::clamp(v[9], 0.0, 1.0);   // M17b
        l.style->shadowUseGL = v[10] != 0;                    // M17b
        l.style->shadowColor[3] = 1.0f;
        return true;
    });
}

// 描边：setLayerStroke(id, enabled, width, r, g, b)
napi_value SetLayerStroke(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6];
    for (size_t i = 0; i < 6; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double v[6] = {0};
    if (argc < 6) {
        return makeError(env, kErrBadParam, "setLayerStroke(id,enabled,width,r,g,b) requires 6");
    }
    for (size_t i = 0; i < 6; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setLayerStroke: invalid args");
        }
    }
    const LayerId id = static_cast<LayerId>(v[0]);
    return runStyleCommand(env, "layer stroke", id,
                           [v](Document&, Layer& l, std::string&) -> bool {
        if (l.style == nullptr) {
            l.style = std::make_shared<LayerStyle>();
        } else {
            l.style = std::make_shared<LayerStyle>(*l.style);  // COW：快照共享指针不可原地改
        }
        l.style->stroke = v[1] != 0;
        l.style->strokeWidth = v[2];
        l.style->strokeColor[0] = static_cast<float>(v[3] / 255.0);
        l.style->strokeColor[1] = static_cast<float>(v[4] / 255.0);
        l.style->strokeColor[2] = static_cast<float>(v[5] / 255.0);
        l.style->strokeColor[3] = 1.0f;
        return true;
    });
}

// 颜色叠加：setColorOverlay(id, enabled, r, g, b, opacity)
napi_value SetColorOverlay(napi_env env, napi_callback_info info) {
    size_t argc = 7;
    napi_value argv[7];
    for (size_t i = 0; i < 7; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double v[7] = {0};
    if (argc < 7) {
        return makeError(env, kErrBadParam,
                         "setColorOverlay(id,enabled,r,g,b,opacity) requires 7");
    }
    for (size_t i = 0; i < 7; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setColorOverlay: invalid args");
        }
    }
    const LayerId id = static_cast<LayerId>(v[0]);
    return runStyleCommand(env, "color overlay", id,
                           [v](Document&, Layer& l, std::string&) -> bool {
        if (l.style == nullptr) {
            l.style = std::make_shared<LayerStyle>();
        } else {
            l.style = std::make_shared<LayerStyle>(*l.style);  // COW：快照共享指针不可原地改
        }
        l.style->colorOverlay = v[1] != 0;
        l.style->overlayColor[0] = static_cast<float>(v[2] / 255.0);
        l.style->overlayColor[1] = static_cast<float>(v[3] / 255.0);
        l.style->overlayColor[2] = static_cast<float>(v[4] / 255.0);
        l.style->overlayColor[3] = 1.0f;
        l.style->overlayOpacity = std::clamp(v[5], 0.0, 1.0);
        return true;
    });
}

// 清样式：clearLayerStyle(id)
napi_value ClearLayerStyle(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double id = 0;
    if (argc >= 1 && napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "clearLayerStyle(id?) invalid");
    }
    return runStyleCommand(env, "clear style", static_cast<LayerId>(id),
                           [](Document&, Layer& l, std::string&) -> bool {
        l.style = nullptr;
        l.styledRender = nullptr;
        return true;
    });
}

// ---------- M17a 图层效果扩展：内阴影/外发光/内发光/渐变叠加 + 样式剪贴板 + 效果开关 ----------

// setInnerShadow(id,enabled,angle,dist,size,opacity,r,g,b)
napi_value SetInnerShadow(napi_env env, napi_callback_info info) {
    size_t argc = 11;  // 审查修复：原 9 < 所需 11，napi_get_cb_info 只拷 9 个 → argv[9..10]
                       // 恒 nullptr，spread/useGL 两参解析必败，内阴影整体不可用
    napi_value argv[11];
    for (size_t i = 0; i < 11; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double v[11] = {0};
    if (argc < 11) {
        return makeError(env, kErrBadParam,
                         "setInnerShadow(id,enabled,angle,dist,size,opacity,r,g,b,spread,useGL) requires 11");
    }
    for (size_t i = 0; i < 11; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setInnerShadow: invalid args");
        }
    }
    const LayerId id = static_cast<LayerId>(v[0]);
    return runStyleCommand(env, "inner shadow", id,
                           [v](Document&, Layer& l, std::string&) -> bool {
        if (l.style == nullptr) {
            l.style = std::make_shared<LayerStyle>();
        } else {
            l.style = std::make_shared<LayerStyle>(*l.style);
        }
        l.style->innerShadow = v[1] != 0;
        l.style->isAngleDeg = v[2];
        l.style->isDistance = std::max(0.0, v[3]);
        l.style->isSize = std::max(0.0, v[4]);
        l.style->isOpacity = std::clamp(v[5], 0.0, 1.0);
        l.style->isColor[0] = static_cast<float>(v[6] / 255.0);
        l.style->isColor[1] = static_cast<float>(v[7] / 255.0);
        l.style->isColor[2] = static_cast<float>(v[8] / 255.0);
        l.style->isColor[3] = 1.0f;
        l.style->isSpread = std::clamp(v[9], 0.0, 1.0);   // M17b
        l.style->isUseGL = v[10] != 0;                    // M17b
        return true;
    });
}

// setOuterGlow(id,enabled,size,opacity,r,g,b)
napi_value SetOuterGlow(napi_env env, napi_callback_info info) {
    size_t argc = 8;
    napi_value argv[8];
    for (size_t i = 0; i < 8; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double v[8] = {0};
    if (argc < 8) {
        return makeError(env, kErrBadParam,
                         "setOuterGlow(id,enabled,size,opacity,r,g,b,spread) requires 8");
    }
    for (size_t i = 0; i < 8; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setOuterGlow: invalid args");
        }
    }
    const LayerId id = static_cast<LayerId>(v[0]);
    return runStyleCommand(env, "outer glow", id,
                           [v](Document&, Layer& l, std::string&) -> bool {
        if (l.style == nullptr) {
            l.style = std::make_shared<LayerStyle>();
        } else {
            l.style = std::make_shared<LayerStyle>(*l.style);
        }
        l.style->outerGlow = v[1] != 0;
        l.style->ogSize = std::max(0.0, v[2]);
        l.style->ogOpacity = std::clamp(v[3], 0.0, 1.0);
        l.style->ogColor[0] = static_cast<float>(v[4] / 255.0);
        l.style->ogColor[1] = static_cast<float>(v[5] / 255.0);
        l.style->ogColor[2] = static_cast<float>(v[6] / 255.0);
        l.style->ogColor[3] = 1.0f;
        l.style->ogSpread = std::clamp(v[7], 0.0, 1.0);   // M17b
        return true;
    });
}

// setInnerGlow(id,enabled,size,opacity,r,g,b)
napi_value SetInnerGlow(napi_env env, napi_callback_info info) {
    size_t argc = 8;
    napi_value argv[8];
    for (size_t i = 0; i < 8; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double v[8] = {0};
    if (argc < 8) {
        return makeError(env, kErrBadParam,
                         "setInnerGlow(id,enabled,size,opacity,r,g,b,spread) requires 8");
    }
    for (size_t i = 0; i < 8; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setInnerGlow: invalid args");
        }
    }
    const LayerId id = static_cast<LayerId>(v[0]);
    return runStyleCommand(env, "inner glow", id,
                           [v](Document&, Layer& l, std::string&) -> bool {
        if (l.style == nullptr) {
            l.style = std::make_shared<LayerStyle>();
        } else {
            l.style = std::make_shared<LayerStyle>(*l.style);
        }
        l.style->innerGlow = v[1] != 0;
        l.style->igSize = std::max(0.0, v[2]);
        l.style->igOpacity = std::clamp(v[3], 0.0, 1.0);
        l.style->igColor[0] = static_cast<float>(v[4] / 255.0);
        l.style->igColor[1] = static_cast<float>(v[5] / 255.0);
        l.style->igColor[2] = static_cast<float>(v[6] / 255.0);
        l.style->igColor[3] = 1.0f;
        l.style->igSpread = std::clamp(v[7], 0.0, 1.0);   // M17b
        return true;
    });
}

// setGradientOverlay(id,enabled,angle,opacity,fr,fg,fb,tr,tg,tb)
napi_value SetGradientOverlay(napi_env env, napi_callback_info info) {
    size_t argc = 10;
    napi_value argv[10];
    for (size_t i = 0; i < 10; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double v[10] = {0};
    if (argc < 10) {
        return makeError(env, kErrBadParam,
                         "setGradientOverlay(id,enabled,angle,opacity,fr,fg,fb,tr,tg,tb) requires 10");
    }
    for (size_t i = 0; i < 10; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setGradientOverlay: invalid args");
        }
    }
    const LayerId id = static_cast<LayerId>(v[0]);
    return runStyleCommand(env, "gradient overlay", id,
                           [v](Document&, Layer& l, std::string&) -> bool {
        if (l.style == nullptr) {
            l.style = std::make_shared<LayerStyle>();
        } else {
            l.style = std::make_shared<LayerStyle>(*l.style);
        }
        l.style->gradientOverlay = v[1] != 0;
        l.style->gdAngleDeg = v[2];
        l.style->gdOpacity = std::clamp(v[3], 0.0, 1.0);
        l.style->gdFrom[0] = static_cast<float>(v[4] / 255.0);
        l.style->gdFrom[1] = static_cast<float>(v[5] / 255.0);
        l.style->gdFrom[2] = static_cast<float>(v[6] / 255.0);
        l.style->gdFrom[3] = 1.0f;
        l.style->gdTo[0] = static_cast<float>(v[7] / 255.0);
        l.style->gdTo[1] = static_cast<float>(v[8] / 255.0);
        l.style->gdTo[2] = static_cast<float>(v[9] / 255.0);
        l.style->gdTo[3] = 1.0f;
        return true;
    });
}

// ---------- M17b 光泽 / 斜面浮雕 / 全局光 ----------

// setSatin(id,enabled,angle,dist,size,opacity,r,g,b,invert)
napi_value SetSatin(napi_env env, napi_callback_info info) {
    size_t argc = 10;
    napi_value argv[10];
    for (size_t i = 0; i < 10; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double v[10] = {0};
    if (argc < 10) {
        return makeError(env, kErrBadParam,
                         "setSatin(id,enabled,angle,dist,size,opacity,r,g,b,invert) requires 10");
    }
    for (size_t i = 0; i < 10; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setSatin: invalid args");
        }
    }
    const LayerId id = static_cast<LayerId>(v[0]);
    return runStyleCommand(env, "satin", id,
                           [v](Document&, Layer& l, std::string&) -> bool {
        if (l.style == nullptr) {
            l.style = std::make_shared<LayerStyle>();
        } else {
            l.style = std::make_shared<LayerStyle>(*l.style);
        }
        l.style->satin = v[1] != 0;
        l.style->satinAngleDeg = v[2];
        l.style->satinDistance = std::max(0.0, v[3]);
        l.style->satinSize = std::max(0.0, v[4]);
        l.style->satinOpacity = std::clamp(v[5], 0.0, 1.0);
        l.style->satinColor[0] = static_cast<float>(v[6] / 255.0);
        l.style->satinColor[1] = static_cast<float>(v[7] / 255.0);
        l.style->satinColor[2] = static_cast<float>(v[8] / 255.0);
        l.style->satinColor[3] = 1.0f;
        l.style->satinInvert = v[9] != 0;
        return true;
    });
}

// setBevel(id,enabled,style,depth,up,size,angle,altitude,hiOp,hr,hg,hb,shOp,sr,sg,sb)
napi_value SetBevel(napi_env env, napi_callback_info info) {
    size_t argc = 16;
    napi_value argv[16];
    for (size_t i = 0; i < 16; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double v[16] = {0};
    if (argc < 16) {
        return makeError(env, kErrBadParam,
                         "setBevel(id,enabled,style,depth,up,size,angle,altitude,"
                         "hiOp,hr,hg,hb,shOp,sr,sg,sb) requires 16");
    }
    for (size_t i = 0; i < 16; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setBevel: invalid args");
        }
    }
    const LayerId id = static_cast<LayerId>(v[0]);
    return runStyleCommand(env, "bevel emboss", id,
                           [v](Document&, Layer& l, std::string&) -> bool {
        if (l.style == nullptr) {
            l.style = std::make_shared<LayerStyle>();
        } else {
            l.style = std::make_shared<LayerStyle>(*l.style);
        }
        l.style->bevel = v[1] != 0;
        l.style->bevelStyle = v[2] != 0 ? 1 : 0;  // 0=Inner 1=Outer（Emboss 族挂 M17c）
        l.style->bevelDepth = std::clamp(v[3], 0.0, 10.0);
        l.style->bevelUp = v[4] != 0;
        l.style->bevelSize = std::max(0.0, v[5]);
        l.style->bevelAngleDeg = v[6];
        l.style->bevelAltitudeDeg = std::clamp(v[7], 1.0, 89.0);
        l.style->bevelHiOpacity = std::clamp(v[8], 0.0, 1.0);
        l.style->bevelHiColor[0] = static_cast<float>(v[9] / 255.0);
        l.style->bevelHiColor[1] = static_cast<float>(v[10] / 255.0);
        l.style->bevelHiColor[2] = static_cast<float>(v[11] / 255.0);
        l.style->bevelHiColor[3] = 1.0f;
        l.style->bevelShOpacity = std::clamp(v[12], 0.0, 1.0);
        l.style->bevelShColor[0] = static_cast<float>(v[13] / 255.0);
        l.style->bevelShColor[1] = static_cast<float>(v[14] / 255.0);
        l.style->bevelShColor[2] = static_cast<float>(v[15] / 255.0);
        l.style->bevelShColor[3] = 1.0f;
        return true;
    });
}

// setGlobalLight(angle)：文档级全局光角度（无 History 事务——PS 同语义；全文档重样式）
napi_value SetGlobalLight(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double angle = 120.0;
    if (argc >= 1 && napi_get_value_double(env, argv[0], &angle) != napi_ok) {
        return makeError(env, kErrBadParam, "setGlobalLight(angle) invalid");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0) {
            return makeError(env, kErrNoDoc, "setGlobalLight: no document");
        }
        e.doc.globalLightAngle = std::clamp(angle, 0.0, 360.0);
        std::string err;
        for (Layer& l : e.doc.layers) {
            if (l.style != nullptr || l.fill != nullptr) {
                restyleLayer(e.doc, l, nextGridRevision(), err);
            }
        }
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

// 样式剪贴板（会话态，跨文档保留——M10c ClipBuffer 同语义）
napi_value CopyLayerStyle(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double id = 0;
    if (argc >= 1 && napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "copyLayerStyle(id?) invalid");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0) {
            return makeError(env, kErrNoDoc, "copyLayerStyle: no document");
        }
        Layer* l = nullptr;
        for (Layer& li : e.doc.layers) {
            if (li.id == (id > 0 ? static_cast<LayerId>(id) : e.doc.activeId)) {
                l = &li;
                break;
            }
        }
        if (l == nullptr || l->style == nullptr) {
            return makeError(env, kErrBadParam, "copyLayerStyle: layer has no style");
        }
        e.styleClipboard = std::make_shared<LayerStyle>(*l->style);
    }
    return makeOk(env, nullptr);
}

// pasteLayerStyle(id)：剪贴板深拷贝 COW 替换 + 一步事务
napi_value PasteLayerStyle(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double id = 0;
    if (argc >= 1 && napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "pasteLayerStyle(id?) invalid");
    }
    auto& e = Engine::get();
    std::shared_ptr<LayerStyle> snap;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.styleClipboard == nullptr) {
            return makeError(env, kErrBadParam, "pasteLayerStyle: style clipboard empty");
        }
        snap = e.styleClipboard;
    }
    return runStyleCommand(env, "paste layer style", static_cast<LayerId>(id),
                           [&snap](Document&, Layer& l, std::string&) -> bool {
        l.style = std::make_shared<LayerStyle>(*snap);
        l.styledRender = nullptr;  // runStyleCommand 内 restyleLayer 重算
        return true;
    });
}

// setLayerEffectsEnabled(id,enabled)：fx 总开关（PS「隐藏全部效果」，COW 翻 enabled）
napi_value SetLayerEffectsEnabled(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double v[2] = {0, 1};
    for (size_t i = 0; i < 2; ++i) {
        if (argc >= i + 1 && napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setLayerEffectsEnabled: invalid args");
        }
    }
    return runStyleCommand(env, v[1] != 0 ? "show all effects" : "hide all effects",
                           static_cast<LayerId>(v[0]),
                           [v](Document&, Layer& l, std::string& err) -> bool {
        if (l.style == nullptr) {
            err = "layer has no style";
            return false;
        }
        l.style = std::make_shared<LayerStyle>(*l.style);
        l.style->enabled = v[1] != 0;
        return true;
    });
}

// 新建填充图层：addFillLayer(kind, r, g, b, angle)（kind 0=纯色 1=线性渐变 fg→bg）
napi_value AddFillLayer(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5];
    for (size_t i = 0; i < 5; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double v[5] = {0};
    if (argc < 5) {
        return makeError(env, kErrBadParam, "addFillLayer(kind,r,g,b,angle) requires 5");
    }
    for (size_t i = 0; i < 5; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "addFillLayer: invalid args");
        }
    }
    auto& e = Engine::get();
    double newId = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "addFillLayer: no document");
        }
        if (e.textEditOpen || e.filterPreview.active) {
            return makeError(env, kErrBadParam, "addFillLayer: session open");
        }
        e.history.beginEdit(e.doc, "new fill layer", static_cast<uint64_t>(e.docRevision));
        Layer layer;
        layer.id = e.nextLayerId();
        const int kind = static_cast<int>(v[0]);
        layer.name = kind == 1 ? "渐变填充 " + std::to_string(layer.id)
                               : "纯色填充 " + std::to_string(layer.id);
        layer.fill = std::make_shared<FillLayerState>();
        layer.fill->kind = kind == 1 ? 1 : 0;
        layer.fill->color[0] = static_cast<float>(v[1] / 255.0);
        layer.fill->color[1] = static_cast<float>(v[2] / 255.0);
        layer.fill->color[2] = static_cast<float>(v[3] / 255.0);
        layer.fill->color[3] = 1.0f;
        layer.fill->angleDeg = v[4];
        layer.transform.originX = 0.0;
        layer.transform.originY = 0.0;
        std::string err;
        if (!restyleLayer(e.doc, layer, nextGridRevision(), err)) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "addFillLayer: " + err);
        }
        // 插到活动层上方（addLayer 同语义）
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

// 填充层改参：setFillLayerParams(id, kind, r, g, b, angle)
napi_value SetFillLayerParams(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6];
    for (size_t i = 0; i < 6; ++i) {
        argv[i] = nullptr;
    }
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double v[6] = {0};
    if (argc < 6) {
        return makeError(env, kErrBadParam, "setFillLayerParams(id,kind,r,g,b,angle) requires 6");
    }
    for (size_t i = 0; i < 6; ++i) {
        if (napi_get_value_double(env, argv[i], &v[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setFillLayerParams: invalid args");
        }
    }
    const LayerId id = static_cast<LayerId>(v[0]);
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "setFillLayerParams: no document");
        }
        Layer* l = id > 0 ? findLayerLocked(e.doc, id) : nullptr;
        if (l == nullptr) {
            for (Layer& li : e.doc.layers) {
                if (li.id == e.doc.activeId) {
                    l = &li;
                    break;
                }
            }
        }
        if (l == nullptr || l->fill == nullptr) {
            return makeError(env, kErrBadParam, "setFillLayerParams: not a fill layer");
        }
        e.history.beginEdit(e.doc, "fill layer params", static_cast<uint64_t>(e.docRevision));
        // COW：fill 为 shared_ptr 且 History 浅拷贝，原地改会污染撤销前快照（同 adjustment）
        auto fill = std::make_shared<FillLayerState>(*l->fill);
        fill->kind = static_cast<int>(v[1]) == 1 ? 1 : 0;
        fill->color[0] = static_cast<float>(v[2] / 255.0);
        fill->color[1] = static_cast<float>(v[3] / 255.0);
        fill->color[2] = static_cast<float>(v[4] / 255.0);
        fill->color[3] = 1.0f;
        fill->angleDeg = v[5];
        l->fill = std::move(fill);
        std::string err;
        if (!restyleLayer(e.doc, *l, nextGridRevision(), err)) {
            e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
            return makeError(env, kErrBadParam, "setFillLayerParams: " + err);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}


// ---------- M15a 视图：参考线（会话态，无历史事务）+ 图层内容包围盒 ----------

// addGuide(orient, pos)：orient 0 = 水平（pos=y）、1 = 垂直（pos=x）；文档域 px
napi_value AddGuide(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t orient = 0;
    double pos = 0;
    if (argc < 2 || napi_get_value_int32(env, argv[0], &orient) != napi_ok ||
        napi_get_value_double(env, argv[1], &pos) != napi_ok) {
        return makeError(env, kErrBadParam, "addGuide(orient, pos) requires 2 args");
    }
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    std::string err;
    if (!addGuide(e.doc, orient, pos, err)) {
        return makeError(env, kErrBadParam, "addGuide: " + err);
    }
    return makeOk(env, nullptr);
}

// moveGuide(orient, index, pos)
napi_value MoveGuide(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t orient = 0, index = 0;
    double pos = 0;
    if (argc < 3 || napi_get_value_int32(env, argv[0], &orient) != napi_ok ||
        napi_get_value_int32(env, argv[1], &index) != napi_ok ||
        napi_get_value_double(env, argv[2], &pos) != napi_ok) {
        return makeError(env, kErrBadParam, "moveGuide(orient, index, pos) requires 3 args");
    }
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    std::string err;
    if (!moveGuide(e.doc, orient, index, pos, err)) {
        return makeError(env, kErrBadParam, "moveGuide: " + err);
    }
    return makeOk(env, nullptr);
}

// removeGuide(orient, index)
napi_value RemoveGuide(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t orient = 0, index = 0;
    if (argc < 2 || napi_get_value_int32(env, argv[0], &orient) != napi_ok ||
        napi_get_value_int32(env, argv[1], &index) != napi_ok) {
        return makeError(env, kErrBadParam, "removeGuide(orient, index) requires 2 args");
    }
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    std::string err;
    if (!removeGuide(e.doc, orient, index, err)) {
        return makeError(env, kErrBadParam, "removeGuide: " + err);
    }
    return makeOk(env, nullptr);
}

// clearGuides()：锁定态也允许（显式菜单命令）
napi_value ClearGuides(napi_env env, napi_callback_info info) {
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    clearGuides(e.doc);
    return makeOk(env, nullptr);
}

// setGuidesLocked(locked)
napi_value SetGuidesLocked(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t locked = 0;
    if (argc < 1 || napi_get_value_int32(env, argv[0], &locked) != napi_ok) {
        return makeError(env, kErrBadParam, "setGuidesLocked(locked) requires 1 arg");
    }
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    setGuidesLocked(e.doc, locked != 0);
    return makeOk(env, nullptr);
}

// setGuidesVisible(visible)
napi_value SetGuidesVisible(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t visible = 0;
    if (argc < 1 || napi_get_value_int32(env, argv[0], &visible) != napi_ok) {
        return makeError(env, kErrBadParam, "setGuidesVisible(visible) requires 1 arg");
    }
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    setGuidesVisible(e.doc, visible != 0);
    return makeOk(env, nullptr);
}

// getGuides() → data {h:number[], v:number[], locked:bool, visible:bool}
napi_value GetGuides(napi_env env, napi_callback_info info) {
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    napi_value data = nullptr;
    napi_create_object(env, &data);
    const auto pushArr = [&](napi_env env2, napi_value obj, const char* key,
                             const std::vector<float>& v) -> void {
        napi_value arr = nullptr;
        napi_create_array_with_length(env2, v.size(), &arr);
        for (size_t i = 0; i < v.size(); ++i) {
            napi_set_element(env2, arr, static_cast<uint32_t>(i), makeDouble(env2, v[i]));
        }
        napi_set_named_property(env2, obj, key, arr);
    };
    pushArr(env, data, "h", e.doc.guidesH);
    pushArr(env, data, "v", e.doc.guidesV);
    napi_set_named_property(env, data, "locked", makeBool(env, e.doc.guidesLocked));
    napi_set_named_property(env, data, "visible", makeBool(env, e.doc.guidesVisible));
    return makeOk(env, data);
}

// getLayerBounds(id) → data {ok,x,y,w,h}（文档域精确 alpha 包围盒；id=0 = 活动层）
napi_value GetLayerBounds(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double id = 0;
    if (argc >= 1 && napi_get_value_double(env, argv[0], &id) != napi_ok) {
        id = 0;
    }
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    LayerId lid = static_cast<LayerId>(id);
    if (lid == 0) {
        lid = e.doc.activeId;
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    double x = 0, y = 0, w = 0, h = 0;
    std::string err;
    const bool ok = layerContentBBox(e.doc, lid, x, y, w, h, err);
    napi_set_named_property(env, data, "ok", makeBool(env, ok));
    napi_set_named_property(env, data, "x", makeDouble(env, x));
    napi_set_named_property(env, data, "y", makeDouble(env, y));
    napi_set_named_property(env, data, "w", makeDouble(env, w));
    napi_set_named_property(env, data, "h", makeDouble(env, h));
    return makeOk(env, data);
}


// ---------- M15b 面板：历史列表 / 历史跳转 / 选区包围盒 ----------

// getHistoryList() → data {labels: string[], current: number}
napi_value GetHistoryList(napi_env env, napi_callback_info info) {
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    const History::Info hist = e.history.getInfo();
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_value arr = nullptr;
    napi_create_array_with_length(env, hist.labels.size(), &arr);
    for (size_t i = 0; i < hist.labels.size(); ++i) {
        napi_set_element(env, arr, static_cast<uint32_t>(i), makeString(env, hist.labels[i]));
    }
    napi_set_named_property(env, data, "labels", arr);
    napi_set_named_property(env, data, "current", makeDouble(env, static_cast<double>(hist.current)));
    return makeOk(env, data);
}

// setHistoryState(index)：跳转到线性历史索引（连续 undo/redo；一步或多步整快照替换）
napi_value SetHistoryState(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double idx = -1;
    if (argc < 1 || napi_get_value_double(env, argv[0], &idx) != napi_ok || idx < 0) {
        return makeError(env, kErrBadParam, "setHistoryState(index) requires index >= 0");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        std::string label;
        uint64_t rev = static_cast<uint64_t>(e.docRevision);
        if (!e.history.jumpTo(e.doc, rev, static_cast<size_t>(idx), label)) {
            return makeError(env, kErrBadParam, "setHistoryState: index out of range");
        }
        e.docRevision = static_cast<int64_t>(rev);
        e.fireRevisionLocked();  // 广播版本（不递增——撤销族同语义）
        e.requestRender();
    }
    e.historyVersion.fetch_add(1);
    return makeOk(env, nullptr);
}

// ---------- M17d 等高线 / 图案资源 ----------

// setStyleContour(id, effect, xs[], ys[], corners[], range)：效果等高线（0=投影 1=内阴影
// 2=外发光 3=内发光 4=光泽 5=斜面轮廓 6=斜面光泽；xs/ys 0..1 各 2..32 点，corners 与点
// 一一对应 0/1；点数 <2 = 清回 Linear；range 仅 2/3 生效，0.01..1）。
napi_value SetStyleContour(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 5) {
        return makeError(env, kErrBadParam,
                         "setStyleContour(id,effect,xs,ys,corners,range) requires 6 args");
    }
    double id = 0;
    double effect = 0;
    double range = 0.5;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_double(env, argv[1], &effect) != napi_ok) {
        return makeError(env, kErrBadParam, "setStyleContour: invalid args");
    }
    if (argc >= 6 && napi_get_value_double(env, argv[5], &range) != napi_ok) {
        return makeError(env, kErrBadParam, "setStyleContour: invalid range");
    }
    if (effect < 0 || effect > 6) {
        return makeError(env, kErrBadParam, "setStyleContour: bad effect index");
    }
    const auto readF32Array = [&](const napi_value& arr, std::vector<float>& out,
                                  double lo, double hi) -> bool {
        bool isArr = false;
        napi_is_array(env, arr, &isArr);
        if (!isArr) {
            return false;
        }
        uint32_t n = 0;
        if (napi_get_array_length(env, arr, &n) != napi_ok) {
            return false;  // @State 代理数组（M10a）：长度探测失败按无效处理
        }
        if (n > 32) {
            return false;
        }
        out.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            napi_value ev = nullptr;
            double v = 0;
            if (napi_get_element(env, arr, i, &ev) != napi_ok ||
                napi_get_value_double(env, ev, &v) != napi_ok || v < lo || v > hi) {
                return false;
            }
            out[i] = static_cast<float>(v);
        }
        return true;
    };
    std::vector<float> xs, ys;
    if (!readF32Array(argv[2], xs, 0.0, 1.0) || !readF32Array(argv[3], ys, 0.0, 1.0) ||
        xs.size() != ys.size()) {
        return makeError(env, kErrBadParam, "setStyleContour: bad xs/ys");
    }
    std::vector<float> cf;
    bool cornersOk = readF32Array(argv[4], cf, 0.0, 1.0);
    if (cornersOk && !cf.empty() && cf.size() != xs.size()) {
        return makeError(env, kErrBadParam, "setStyleContour: corners size mismatch");
    }
    std::vector<uint8_t> corners;
    if (cornersOk && !cf.empty()) {
        corners.resize(cf.size());
        for (size_t i = 0; i < cf.size(); ++i) {
            corners[i] = cf[i] != 0.0f ? 1 : 0;
        }
    }
    const LayerId lid = static_cast<LayerId>(id);
    const int fx = static_cast<int>(effect);
    return runStyleCommand(env, "style contour", lid,
                           [fx, xs, ys, corners, range](Document&, Layer& l,
                                                        std::string&) -> bool {
        if (l.style == nullptr) {
            l.style = std::make_shared<LayerStyle>();
        } else {
            l.style = std::make_shared<LayerStyle>(*l.style);  // COW：快照共享指针不可原地改
        }
        StyleContour* target = nullptr;
        switch (fx) {
            case 0: target = &l.style->shadowContour; break;
            case 1: target = &l.style->isContour; break;
            case 2: target = &l.style->ogContour; break;
            case 3: target = &l.style->igContour; break;
            case 4: target = &l.style->satinContour; break;
            case 5: target = &l.style->bevelContour; break;
            default: target = &l.style->bevelGlossContour; break;
        }
        if (xs.empty() && ys.empty()) {
            // range-only 更新（发光「范围」滑条）：不动点列
        } else if (xs.size() < 2) {
            *target = StyleContour{};  // 清回 Linear（恒等）
        } else {
            target->custom = true;
            target->pts.clear();
            for (size_t i = 0; i < xs.size(); ++i) {
                target->pts.push_back(xs[i]);
                target->pts.push_back(ys[i]);
            }
            target->corners = corners;
        }
        if (fx == 2) {
            l.style->ogRange = std::clamp(range, 0.01, 1.0);
        } else if (fx == 3) {
            l.style->igRange = std::clamp(range, 0.01, 1.0);
        }
        return true;
    });
}

// setPatternOverlay(id, enabled, patternId, scale, opacity)：图案叠加（patternId 空 = 不渲染）。
napi_value SetPatternOverlay(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 5) {
        return makeError(env, kErrBadParam,
                         "setPatternOverlay(id,enabled,patternId,scale,opacity) requires 5");
    }
    double id = 0;
    double enabled = 0;
    double scale = 1.0;
    double opacity = 1.0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_double(env, argv[1], &enabled) != napi_ok ||
        napi_get_value_double(env, argv[3], &scale) != napi_ok ||
        napi_get_value_double(env, argv[4], &opacity) != napi_ok) {
        return makeError(env, kErrBadParam, "setPatternOverlay: invalid args");
    }
    char buf[128];
    size_t len = 0;
    if (napi_get_value_string_utf8(env, argv[2], buf, sizeof(buf), &len) != napi_ok) {
        return makeError(env, kErrBadParam, "setPatternOverlay: invalid patternId");
    }
    const std::string patternId(buf, len);
    const LayerId lid = static_cast<LayerId>(id);
    return runStyleCommand(env, "pattern overlay", lid,
                           [enabled, patternId, scale, opacity](Document& doc, Layer& l,
                                                                std::string& err) -> bool {
        if (enabled != 0) {
            bool found = false;
            for (const DocumentPattern& p : doc.patterns) {
                if (p.id == patternId) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                err = "pattern not found: " + patternId;
                return false;
            }
        }
        if (l.style == nullptr) {
            l.style = std::make_shared<LayerStyle>();
        } else {
            l.style = std::make_shared<LayerStyle>(*l.style);  // COW：快照共享指针不可原地改
        }
        l.style->patternOverlay = enabled != 0;
        l.style->patternId = patternId;
        l.style->poScale = std::clamp(scale, 0.01, 10.0);
        l.style->poOpacity = std::clamp(opacity, 0.0, 1.0);
        return true;
    });
}

// definePattern()：编辑 → 定义图案（选区 ∩ 活动层原始像素，alpha×选区灰度；无 History
// ——PS 定义图案不在历史面板，setGlobalLight 同语义；返回 data {id,name,w,h}）。
napi_value DefinePattern(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    napi_value data = nullptr;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "definePattern: no document");
        }
        if (e.doc.selection == nullptr) {
            return makeError(env, kErrBadParam, "definePattern: no active selection");
        }
        Layer* l = nullptr;
        for (Layer& li : e.doc.layers) {
            if (li.id == e.doc.activeId) {
                l = &li;
                break;
            }
        }
        if (l == nullptr || l->isGroup || l->pixels == nullptr) {
            return makeError(env, kErrBadParam, "definePattern: no active layer pixels");
        }
        double bx = 0, by = 0, bw = 0, bh = 0;
        if (!selectionBBox(e.doc, bx, by, bw, bh) || bw < 1 || bh < 1) {
            return makeError(env, kErrBadParam, "definePattern: empty selection");
        }
        if (bw > 4096 || bh > 4096) {
            return makeError(env, kErrBadParam, "definePattern: selection too large");
        }
        const int x0 = static_cast<int>(bx);
        const int y0 = static_cast<int>(by);
        const uint32_t w = static_cast<uint32_t>(bw);
        const uint32_t h = static_cast<uint32_t>(bh);
        const TileGrid& grid = *l->pixels;
        TileGridBuilder builder((w + kTileSize - 1) / kTileSize,
                                (h + kTileSize - 1) / kTileSize);
        const int gOX = static_cast<int>(std::lround(l->transform.originX));
        const int gOY = static_cast<int>(std::lround(l->transform.originY));
        bool anyTile = false;
        for (uint32_t yy = 0; yy < h; ++yy) {
            const int docY = y0 + static_cast<int>(yy);
            const int gy = docY - gOY;
            for (uint32_t xx = 0; xx < w; ++xx) {
                const int docX = x0 + static_cast<int>(xx);
                const int gx = docX - gOX;
                if (gx < 0 || gy < 0 || gy >= static_cast<int>(grid.rows * kTileSize) ||
                    gx >= static_cast<int>(grid.cols * kTileSize)) {
                    continue;
                }
                const uint32_t sty2 = static_cast<uint32_t>(gy) / kTileSize;
                const uint32_t stx = static_cast<uint32_t>(gx) / kTileSize;
                auto it = grid.tiles.find(sty2 * grid.cols + stx);
                if (it == grid.tiles.end() || it->second == nullptr ||
                    it->second->pixels == nullptr) {
                    continue;
                }
                const uint8_t* ps = it->second->pixels->mapCpu();
                if (ps == nullptr) {
                    continue;
                }
                const uint8_t* sp =
                    ps + static_cast<size_t>(static_cast<uint32_t>(gy) % kTileSize) *
                             it->second->pixels->rowBytes() +
                         static_cast<size_t>(static_cast<uint32_t>(gx) % kTileSize) * 4;
                const uint8_t sg = selectionGrayAt(*e.doc.selection, docX, docY);
                const uint32_t a = static_cast<uint32_t>(sp[3]) * sg / 255u;
                if (a == 0 && sp[0] == 0 && sp[1] == 0 && sp[2] == 0) {
                    continue;
                }
                const uint32_t tx = xx / kTileSize;
                const uint32_t ty = yy / kTileSize;
                EngineBuffer& tile = builder.ensureTile(tx, ty);
                uint8_t* dp = tile.mapCpuWrite() +
                              static_cast<size_t>(yy % kTileSize) * tile.rowBytes() +
                              static_cast<size_t>(xx % kTileSize) * 4;
                dp[0] = sp[0];
                dp[1] = sp[1];
                dp[2] = sp[2];
                dp[3] = static_cast<uint8_t>(std::min<uint32_t>(255u, a));
                anyTile = true;
            }
        }
        if (!anyTile) {
            return makeError(env, kErrBadParam, "definePattern: nothing under selection");
        }
        DocumentPattern p;
        e.patternSeq += 1;
        p.id = "pat_" + std::to_string(e.patternSeq);
        p.name = "图案 " + std::to_string(e.patternSeq);
        p.w = w;
        p.h = h;
        p.pixels = builder.publish(nextGridRevision());
        e.doc.patterns.push_back(std::move(p));
        e.bumpRevisionLocked();  // 图案列表刷新（docVersion 事件；不入 History）
        napi_create_object(env, &data);
        napi_value sid = nullptr;
        napi_create_string_utf8(env, e.doc.patterns.back().id.c_str(), NAPI_AUTO_LENGTH, &sid);
        napi_set_named_property(env, data, "id", sid);
        napi_value sname = nullptr;
        napi_create_string_utf8(env, e.doc.patterns.back().name.c_str(), NAPI_AUTO_LENGTH,
                                &sname);
        napi_set_named_property(env, data, "name", sname);
        napi_set_named_property(env, data, "w", makeDouble(env, w));
        napi_set_named_property(env, data, "h", makeDouble(env, h));
        OH_LOG_Print(LOG_APP, LOG_INFO, 0x4D30, "Montage.Pattern",
                     "define %{public}s %{public}ux%{public}u", e.doc.patterns.back().id.c_str(),
                     w, h);
    }
    e.requestRender();
    return makeOk(env, data);
}

// getPatterns() → [{id,name,w,h}, …]（文档图案列表，样式对话框图案选择器用）
napi_value GetPatterns(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    napi_value arr = nullptr;
    napi_create_array_with_length(env, e.doc.patterns.size(), &arr);
    for (size_t i = 0; i < e.doc.patterns.size(); ++i) {
        const DocumentPattern& p = e.doc.patterns[i];
        napi_value obj = nullptr;
        napi_create_object(env, &obj);
        napi_value sid = nullptr;
        napi_create_string_utf8(env, p.id.c_str(), NAPI_AUTO_LENGTH, &sid);
        napi_set_named_property(env, obj, "id", sid);
        napi_value sname = nullptr;
        napi_create_string_utf8(env, p.name.c_str(), NAPI_AUTO_LENGTH, &sname);
        napi_set_named_property(env, obj, "name", sname);
        napi_set_named_property(env, obj, "w", makeDouble(env, p.w));
        napi_set_named_property(env, obj, "h", makeDouble(env, p.h));
        napi_set_element(env, arr, static_cast<uint32_t>(i), obj);
    }
    return arr;
}

// getSelectionBounds() → data {ok,x,y,w,h}（文档域精确 alpha 包围盒）
napi_value GetSelectionBounds(napi_env env, napi_callback_info info) {
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    napi_value data = nullptr;
    napi_create_object(env, &data);
    double x = 0, y = 0, w = 0, h = 0;
    const bool ok = selectionBBox(e.doc, x, y, w, h);
    napi_set_named_property(env, data, "ok", makeBool(env, ok));
    napi_set_named_property(env, data, "x", makeDouble(env, x));
    napi_set_named_property(env, data, "y", makeDouble(env, y));
    napi_set_named_property(env, data, "w", makeDouble(env, w));
    napi_set_named_property(env, data, "h", makeDouble(env, h));
    return makeOk(env, data);
}


// ---------- M18 矢量与形状层 ----------

// 路径 → 扁平跨桥数组：[子路径数, (closed, op, knot数, knot×7)×n]
napi_value pathToFlatArray(napi_env env, const VectorPath& p) {
    napi_value arr = nullptr;
    napi_create_array(env, &arr);
    uint32_t k = 0;
    auto pushD = [&](double v) { napi_set_element(env, arr, k++, makeDouble(env, v)); };
    pushD(static_cast<double>(p.subpaths.size()));
    for (const Subpath& sp : p.subpaths) {
        pushD(sp.closed ? 1.0 : 0.0);
        pushD(static_cast<double>(static_cast<int32_t>(sp.op)));
        pushD(static_cast<double>(sp.knots.size()));
        for (const Knot& kn : sp.knots) {
            pushD(kn.anchor.x);
            pushD(kn.anchor.y);
            pushD(kn.inCtrl.x);
            pushD(kn.inCtrl.y);
            pushD(kn.outCtrl.x);
            pushD(kn.outCtrl.y);
            pushD(kn.smooth ? 1.0 : 0.0);
        }
    }
    return arr;
}

// 扁平数组 → 路径（长度/结构校验；脏数据返回 false）。ArkTS 代理数组 is_array 失败兜底
// 同 readLayerIdArray（逐元素探测，M10a 实证）。
bool flatArrayToPath(napi_env env, napi_value v, VectorPath& out) {
    uint32_t n = 0;
    napi_get_array_length(env, v, &n);
    if (n == 0) {
        const uint32_t kMaxProbe = 65536;
        for (uint32_t i = 0; i < kMaxProbe; ++i) {
            napi_value e = nullptr;
            if (napi_get_element(env, v, i, &e) != napi_ok || e == nullptr) {
                break;
            }
            napi_valuetype vt = napi_undefined;
            napi_typeof(env, e, &vt);
            if (vt != napi_number) {
                break;
            }
            n = i + 1;
        }
    }
    out = VectorPath{};
    size_t idx = 0;
    auto next = [&](double& d) {
        if (idx >= n) {
            return false;
        }
        napi_value e = nullptr;
        if (napi_get_element(env, v, static_cast<uint32_t>(idx), &e) != napi_ok) {
            return false;
        }
        if (napi_get_value_double(env, e, &d) != napi_ok) {
            return false;
        }
        idx++;
        return true;
    };
    double subCount = 0;
    if (!next(subCount) || subCount < 0.0 || subCount > 4096.0) {
        return false;
    }
    for (size_t si = 0; si < static_cast<size_t>(subCount); si++) {
        double closed = 0, op = 0, knotCount = 0;
        if (!next(closed) || !next(op) || !next(knotCount)) {
            return false;
        }
        if (knotCount < 0.0 || knotCount > 4096.0 || op < -1.0 || op > 4.0) {
            return false;
        }
        Subpath sp;
        sp.closed = closed > 0.5;
        sp.op = static_cast<PathOp>(static_cast<int32_t>(op));
        for (size_t ki = 0; ki < static_cast<size_t>(knotCount); ki++) {
            double vals[7];
            for (double& val : vals) {
                if (!next(val)) {
                    return false;
                }
                if (!std::isfinite(val)) {
                    return false;
                }
            }
            Knot kn;
            kn.anchor = vpt(vals[0], vals[1]);
            kn.inCtrl = vpt(vals[2], vals[3]);
            kn.outCtrl = vpt(vals[4], vals[5]);
            kn.smooth = vals[6] > 0.5;
            sp.knots.push_back(kn);
        }
        out.subpaths.push_back(std::move(sp));
    }
    return true;
}

napi_value GetShapePath(napi_env env, napi_callback_info info) {
    // getShapePath(id) → 扁平路径（live 形状返回生成路径——直接选择拖拽后经 setShapePath
    // 提交会清 live，PS 同语义）。非形状层返回空数组。
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "getShapePath(id) requires 1 arg");
    }
    double id = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "getShapePath: invalid id");
    }
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    const Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
    VectorPath out;
    if (l != nullptr && l->shape != nullptr) {
        out = l->shape->live.kind == ShapeLiveKind::None ? l->shape->path
                                                         : liveShapePath(l->shape->live);
    }
    return makeOk(env, pathToFlatArray(env, out));
}
napi_value CreateShapeLayer(napi_env env, napi_callback_info info) {
    // createShapeLayer(kind, x0, y0, x1, y1, radius, sides, starRatio, weight,
    //                  fillKind, fr, fg, fb, fa, hasStroke, sr, sg, sb, sa, strokeWidth)
    size_t argc = 20;
    napi_value argv[20] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 20) {
        return makeError(env, kErrBadParam, "createShapeLayer requires 20 args");
    }
    double a[20] = {0};
    for (size_t i = 0; i < 20; i++) {
        if (napi_get_value_double(env, argv[i], &a[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "createShapeLayer: invalid args");
        }
    }
    auto& e = Engine::get();
    double newId = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "createShapeLayer: no document");
        }
        auto state = std::make_shared<ShapeLayerState>();
        LiveShape& lv = state->live;
        lv.kind = static_cast<ShapeLiveKind>(static_cast<int32_t>(a[0]));
        if (lv.kind < ShapeLiveKind::Rect || lv.kind > ShapeLiveKind::Line) {
            return makeError(env, kErrBadParam, "createShapeLayer: bad kind");
        }
        const double x0 = a[1], y0 = a[2], x1 = a[3], y1 = a[4];
        if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) {
            return makeError(env, kErrBadParam, "createShapeLayer: bad rect");
        }
        switch (lv.kind) {
            case ShapeLiveKind::Rect:
                lv.rect[0] = std::min(x0, x1);
                lv.rect[1] = std::min(y0, y1);
                lv.rect[2] = std::fabs(x1 - x0);
                lv.rect[3] = std::fabs(y1 - y0);
                lv.radii[0] = lv.radii[1] = lv.radii[2] = lv.radii[3] = std::max(0.0, a[5]);
                break;
            case ShapeLiveKind::Ellipse:
                lv.rect[0] = std::min(x0, x1);
                lv.rect[1] = std::min(y0, y1);
                lv.rect[2] = std::fabs(x1 - x0);
                lv.rect[3] = std::fabs(y1 - y0);
                break;
            case ShapeLiveKind::Polygon: {
                lv.rect[0] = std::min(x0, x1);
                lv.rect[1] = std::min(y0, y1);
                lv.rect[2] = std::fabs(x1 - x0);
                lv.rect[3] = std::fabs(y1 - y0);
                const double sides = std::min(std::max(a[6], 3.0), 100.0);
                lv.sides = static_cast<uint32_t>(sides);
                lv.starRatio = std::min(std::max(a[7], 0.0), 1.0);
                if (lv.starRatio >= 1.0) {
                    lv.starRatio = 1.0;
                }
                break;
            }
            case ShapeLiveKind::Line:
                lv.from[0] = x0;
                lv.from[1] = y0;
                lv.to[0] = x1;
                lv.to[1] = y1;
                lv.weight = std::max(1.0, a[8]);
                break;
            default:
                break;
        }
        state->path = liveShapePath(lv);
        state->fillKind = static_cast<int32_t>(a[9]);
        state->fillColor[0] = static_cast<float>(a[10]);
        state->fillColor[1] = static_cast<float>(a[11]);
        state->fillColor[2] = static_cast<float>(a[12]);
        state->fillColor[3] = static_cast<float>(a[13]);
        state->hasStroke = a[14] > 0.5;
        if (state->hasStroke) {
            state->stroke.width = std::max(1.0, a[19]);
            state->stroke.color[0] = static_cast<float>(a[15]);
            state->stroke.color[1] = static_cast<float>(a[16]);
            state->stroke.color[2] = static_cast<float>(a[17]);
            state->stroke.color[3] = static_cast<float>(a[18]);
        }
        std::string err;
        if (!validateShape(state->path, state->hasStroke ? &state->stroke : nullptr, err)) {
            return makeError(env, kErrBadParam, ("createShapeLayer: " + err).c_str());
        }
        Layer layer;
        layer.id = e.nextLayerId();
        layer.name = shapeLayerDisplayName(*state);
        layer.shape = std::move(state);
        size_t at = e.doc.layers.size();
        for (size_t i = 0; i < e.doc.layers.size(); ++i) {
            if (e.doc.layers[i].id == e.doc.activeId) {
                at = i + 1;
                break;
            }
        }
        e.history.beginEdit(e.doc, layer.name, static_cast<uint64_t>(e.docRevision));
        e.doc.layers.insert(e.doc.layers.begin() + static_cast<long>(at), std::move(layer));
        Layer& inserted = e.doc.layers[at];
        e.doc.activeId = inserted.id;
        if (!shapeRenderLayer(e.doc, inserted, err)) {
            { uint64_t rv = 0; std::string lb; e.history.undo(e.doc, rv, lb); (void)rv; (void)lb; }  // 渲染失败回滚（预算拒绝等）
            return makeError(env, kErrBadParam, ("createShapeLayer: " + err).c_str());
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        newId = static_cast<double>(inserted.id);
        e.bumpRevisionLocked();
        e.requestRender();
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "id", makeDouble(env, newId));
    return makeOk(env, data);
}

napi_value SetShapeLive(napi_env env, napi_callback_info info) {
    // setShapeLive(id, kind, x0, y0, x1, y1, radius, sides, starRatio, weight)
    size_t argc = 10;
    napi_value argv[10] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 10) {
        return makeError(env, kErrBadParam, "setShapeLive requires 10 args");
    }
    double a[10] = {0};
    for (size_t i = 0; i < 10; i++) {
        if (napi_get_value_double(env, argv[i], &a[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setShapeLive: invalid args");
        }
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(a[0]));
        if (l == nullptr || l->shape == nullptr) {
            return makeError(env, kErrBadParam, "setShapeLive: shape layer not found");
        }
        auto next = std::make_shared<ShapeLayerState>(*l->shape);  // COW
        LiveShape& lv = next->live;
        lv.kind = static_cast<ShapeLiveKind>(static_cast<int32_t>(a[1]));
        const double x0 = a[2], y0 = a[3], x1 = a[4], y1 = a[5];
        switch (lv.kind) {
            case ShapeLiveKind::Rect:
                lv.rect[0] = std::min(x0, x1);
                lv.rect[1] = std::min(y0, y1);
                lv.rect[2] = std::fabs(x1 - x0);
                lv.rect[3] = std::fabs(y1 - y0);
                lv.radii[0] = lv.radii[1] = lv.radii[2] = lv.radii[3] = std::max(0.0, a[6]);
                break;
            case ShapeLiveKind::Ellipse:
                lv.rect[0] = std::min(x0, x1);
                lv.rect[1] = std::min(y0, y1);
                lv.rect[2] = std::fabs(x1 - x0);
                lv.rect[3] = std::fabs(y1 - y0);
                break;
            case ShapeLiveKind::Polygon:
                lv.rect[0] = std::min(x0, x1);
                lv.rect[1] = std::min(y0, y1);
                lv.rect[2] = std::fabs(x1 - x0);
                lv.rect[3] = std::fabs(y1 - y0);
                lv.sides = static_cast<uint32_t>(std::min(std::max(a[7], 3.0), 100.0));
                lv.starRatio = std::min(std::max(a[8], 0.0), 1.0);
                if (lv.starRatio >= 1.0) {
                    lv.starRatio = 1.0;
                }
                break;
            case ShapeLiveKind::Line:
                lv.from[0] = x0;
                lv.from[1] = y0;
                lv.to[0] = x1;
                lv.to[1] = y1;
                lv.weight = std::max(1.0, a[9]);
                break;
            default:
                return makeError(env, kErrBadParam, "setShapeLive: bad kind");
        }
        next->path = liveShapePath(lv);
        std::string err;
        if (!validateShape(next->path, next->hasStroke ? &next->stroke : nullptr, err)) {
            return makeError(env, kErrBadParam, ("setShapeLive: " + err).c_str());
        }
        e.history.beginEdit(e.doc, "编辑形状", static_cast<uint64_t>(e.docRevision));
        l->shape = std::move(next);
        shapeRenderLayer(e.doc, *l, err);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value SetShapeFill(napi_env env, napi_callback_info info) {
    // setShapeFill(id, kind, r, g, b, a)
    size_t argc = 6;
    napi_value argv[6] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 6) {
        return makeError(env, kErrBadParam, "setShapeFill requires 6 args");
    }
    double a[6] = {0};
    for (size_t i = 0; i < 6; i++) {
        if (napi_get_value_double(env, argv[i], &a[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setShapeFill: invalid args");
        }
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(a[0]));
        if (l == nullptr || l->shape == nullptr) {
            return makeError(env, kErrBadParam, "setShapeFill: shape layer not found");
        }
        auto next = std::make_shared<ShapeLayerState>(*l->shape);
        next->fillKind = static_cast<int32_t>(a[1]);
        next->fillColor[0] = static_cast<float>(a[2]);
        next->fillColor[1] = static_cast<float>(a[3]);
        next->fillColor[2] = static_cast<float>(a[4]);
        next->fillColor[3] = static_cast<float>(a[5]);
        e.history.beginEdit(e.doc, "形状填充", static_cast<uint64_t>(e.docRevision));
        l->shape = std::move(next);
        std::string err;
        shapeRenderLayer(e.doc, *l, err);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value SetShapeStroke(napi_env env, napi_callback_info info) {
    // setShapeStroke(id, on, r, g, b, a, width, align?, cap?, join?)
    size_t argc = 10;
    napi_value argv[10] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 7) {
        return makeError(env, kErrBadParam, "setShapeStroke requires 7+ args");
    }
    double a[10] = {0};
    for (size_t i = 0; i < argc && i < 10; i++) {
        if (napi_get_value_double(env, argv[i], &a[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "setShapeStroke: invalid args");
        }
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(a[0]));
        if (l == nullptr || l->shape == nullptr) {
            return makeError(env, kErrBadParam, "setShapeStroke: shape layer not found");
        }
        auto next = std::make_shared<ShapeLayerState>(*l->shape);
        next->hasStroke = a[1] > 0.5;
        next->stroke.color[0] = static_cast<float>(a[2]);
        next->stroke.color[1] = static_cast<float>(a[3]);
        next->stroke.color[2] = static_cast<float>(a[4]);
        next->stroke.color[3] = static_cast<float>(a[5]);
        next->stroke.width = std::max(1.0, a[6]);
        if (argc >= 8) {
            next->stroke.align = static_cast<StrokeAlign>(
                static_cast<int32_t>(std::min(std::max(a[7], 0.0), 2.0)));
        }
        if (argc >= 9) {
            next->stroke.cap =
                static_cast<LineCap>(static_cast<int32_t>(std::min(std::max(a[8], 0.0), 2.0)));
        }
        if (argc >= 10) {
            next->stroke.join =
                static_cast<LineJoin>(static_cast<int32_t>(std::min(std::max(a[9], 0.0), 2.0)));
        }
        std::string err;
        if (next->hasStroke && !validateShape(next->path, &next->stroke, err)) {
            return makeError(env, kErrBadParam, ("setShapeStroke: " + err).c_str());
        }
        e.history.beginEdit(e.doc, "形状描边", static_cast<uint64_t>(e.docRevision));
        l->shape = std::move(next);
        shapeRenderLayer(e.doc, *l, err);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value RasterizeShapeLayer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "rasterizeShapeLayer(id) requires 1 arg");
    }
    double id = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "rasterizeShapeLayer: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr || l->shape == nullptr) {
            return makeError(env, kErrBadParam, "rasterizeShapeLayer: shape layer not found");
        }
        e.history.beginEdit(e.doc, "栅格化形状", static_cast<uint64_t>(e.docRevision));
        rasterizeShapeLayerToPixels(*l);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value SetShapePath(napi_env env, napi_callback_info info) {
    // setShapePath(id, flat[])：路径编辑提交（live 清 None）
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "setShapePath(id, flat) requires 2 args");
    }
    double id = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "setShapePath: invalid args");
    }
    VectorPath path;
    if (!flatArrayToPath(env, argv[1], path)) {
        return makeError(env, kErrBadParam, "setShapePath: bad path array");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr || l->shape == nullptr) {
            return makeError(env, kErrBadParam, "setShapePath: shape layer not found");
        }
        auto next = std::make_shared<ShapeLayerState>(*l->shape);
        std::string err;
        if (!validateShape(path, next->hasStroke ? &next->stroke : nullptr, err)) {
            return makeError(env, kErrBadParam, ("setShapePath: " + err).c_str());
        }
        next->path = std::move(path);
        next->live = LiveShape{};  // 路径编辑后不再是参数形状
        e.history.beginEdit(e.doc, "编辑路径", static_cast<uint64_t>(e.docRevision));
        l->shape = std::move(next);
        shapeRenderLayer(e.doc, *l, err);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value SetWorkPath(napi_env env, napi_callback_info info) {
    // setWorkPath(flat[])：会话态（无历史事务，PS 路径面板同语义）
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 1) {
        return makeError(env, kErrBadParam, "setWorkPath(flat) requires 1 arg");
    }
    VectorPath path;
    if (!flatArrayToPath(env, argv[0], path)) {
        return makeError(env, kErrBadParam, "setWorkPath: bad path array");
    }
    std::string err;
    if (!path.empty() && !validateShape(path, nullptr, err)) {
        return makeError(env, kErrBadParam, ("setWorkPath: " + err).c_str());
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        e.doc.workPath = std::move(path);
        e.bumpRevisionLocked();
    }
    return makeOk(env, nullptr);
}

napi_value GetWorkPath(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    return makeOk(env, pathToFlatArray(env, e.doc.workPath));
}

napi_value CommitWorkPathAsShape(napi_env env, napi_callback_info info) {
    // commitWorkPathAsShape(fillKind,fr,fg,fb,fa, hasStroke, sr,sg,sb,sa, sw) → 新形状层
    size_t argc = 11;
    napi_value argv[11] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 11) {
        return makeError(env, kErrBadParam, "commitWorkPathAsShape requires 11 args");
    }
    double a[11] = {0};
    for (size_t i = 0; i < 11; i++) {
        if (napi_get_value_double(env, argv[i], &a[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "commitWorkPathAsShape: invalid args");
        }
    }
    auto& e = Engine::get();
    double newId = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "commitWorkPathAsShape: no document");
        }
        if (e.doc.workPath.empty()) {
            return makeError(env, kErrBadParam, "commitWorkPathAsShape: no work path");
        }
        auto state = std::make_shared<ShapeLayerState>();
        state->path = e.doc.workPath;
        state->fillKind = static_cast<int32_t>(a[0]);
        state->fillColor[0] = static_cast<float>(a[1]);
        state->fillColor[1] = static_cast<float>(a[2]);
        state->fillColor[2] = static_cast<float>(a[3]);
        state->fillColor[3] = static_cast<float>(a[4]);
        state->hasStroke = a[5] > 0.5;
        if (state->hasStroke) {
            state->stroke.width = std::max(1.0, a[10]);
            state->stroke.color[0] = static_cast<float>(a[6]);
            state->stroke.color[1] = static_cast<float>(a[7]);
            state->stroke.color[2] = static_cast<float>(a[8]);
            state->stroke.color[3] = static_cast<float>(a[9]);
        }
        std::string err;
        if (!validateShape(state->path, state->hasStroke ? &state->stroke : nullptr, err)) {
            return makeError(env, kErrBadParam, ("commitWorkPathAsShape: " + err).c_str());
        }
        Layer layer;
        layer.id = e.nextLayerId();
        layer.name = shapeLayerDisplayName(*state);
        layer.shape = std::move(state);
        size_t at = e.doc.layers.size();
        for (size_t i = 0; i < e.doc.layers.size(); ++i) {
            if (e.doc.layers[i].id == e.doc.activeId) {
                at = i + 1;
                break;
            }
        }
        e.history.beginEdit(e.doc, layer.name, static_cast<uint64_t>(e.docRevision));
        e.doc.layers.insert(e.doc.layers.begin() + static_cast<long>(at), std::move(layer));
        Layer& inserted = e.doc.layers[at];
        e.doc.activeId = inserted.id;
        if (!shapeRenderLayer(e.doc, inserted, err)) {
            { uint64_t rv = 0; std::string lb; e.history.undo(e.doc, rv, lb); (void)rv; (void)lb; }
            return makeError(env, kErrBadParam, ("commitWorkPathAsShape: " + err).c_str());
        }
        e.doc.workPath = VectorPath{};  // PS 语义：成形状层后工作路径清空
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        newId = static_cast<double>(inserted.id);
        e.bumpRevisionLocked();
        e.requestRender();
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "id", makeDouble(env, newId));
    return makeOk(env, data);
}

napi_value PathToSelection(napi_env env, napi_callback_info info) {
    // pathToSelection(op)——工作路径（空则活动形状层路径）→ 选区
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int32_t op = 0;
    if (argc >= 1 && napi_get_value_int32(env, argv[0], &op) != napi_ok) {
        return makeError(env, kErrBadParam, "pathToSelection: invalid op");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "pathToSelection: no document");
        }
        VectorPath source = e.doc.workPath;
        if (source.empty()) {
            Layer* l = findLayerLocked(e.doc, e.doc.activeId);
            if (l != nullptr && l->shape != nullptr) {
                source = l->shape->live.kind == ShapeLiveKind::None
                             ? l->shape->path
                             : liveShapePath(l->shape->live);
            }
        }
        if (source.empty()) {
            return makeError(env, kErrBadParam, "pathToSelection: no path");
        }
        auto mask = pathToSelectionMask(source, e.doc.width, e.doc.height);
        if (mask == nullptr) {
            return makeError(env, kErrBadParam, "pathToSelection: empty result");
        }
        auto merged = tiles::combineSelection(e.doc, std::move(mask),
                                              static_cast<tiles::SelectionOp>(op));
        e.history.beginEdit(e.doc, "路径转选区", static_cast<uint64_t>(e.docRevision));
        e.doc.selection = std::move(merged);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value FillPath(napi_env env, napi_callback_info info) {
    // fillPath(r,g,b,a)：工作路径域 × 颜色填充到活动层（与既有选区相交，PS 语义；
    // 临时 swap 选区走 M11a fillWithColor 核——半透明覆盖度自动获得）
    size_t argc = 4;
    napi_value argv[4] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 4) {
        return makeError(env, kErrBadParam, "fillPath(r,g,b,a) requires 4 args");
    }
    double c[4] = {0};
    for (size_t i = 0; i < 4; i++) {
        if (napi_get_value_double(env, argv[i], &c[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "fillPath: invalid args");
        }
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "fillPath: no document");
        }
        Layer* l = findLayerLocked(e.doc, e.doc.activeId);
        if (l == nullptr || l->shape != nullptr || l->fill != nullptr) {
            return makeError(env, kErrBadParam, "fillPath: active layer must be a pixel layer");
        }
        if (e.doc.workPath.empty()) {
            return makeError(env, kErrBadParam, "fillPath: no work path");
        }
        auto mask = pathToSelectionMask(e.doc.workPath, e.doc.width, e.doc.height);
        if (mask == nullptr) {
            return makeError(env, kErrBadParam, "fillPath: empty result");
        }
        std::shared_ptr<const TileGrid> saved = e.doc.selection;
        if (saved != nullptr) {
            e.doc.selection = mask;
            mask = tiles::combineSelection(e.doc, saved, tiles::SelectionOp::Intersect);
            if (mask == nullptr) {
                e.doc.selection = saved;
                return makeOk(env, nullptr);  // 不相交 = 无事发生
            }
        }
        e.history.beginEdit(e.doc, "填充路径", static_cast<uint64_t>(e.docRevision));
        std::shared_ptr<const TileGrid> savedNow = e.doc.selection;
        e.doc.selection = mask;
        std::string err;
        const bool ok = fillWithColor(e.doc, l->id, FillSource::Foreground,
                                      static_cast<float>(c[0]), static_cast<float>(c[1]),
                                      static_cast<float>(c[2]), 0, 0, 0, err);
        e.doc.selection = savedNow;
        if (!ok) {
            { uint64_t rv = 0; std::string lb; e.history.undo(e.doc, rv, lb); (void)rv; (void)lb; }
            e.doc.selection = saved;
            return makeError(env, kErrBadParam, ("fillPath: " + err).c_str());
        }
        e.doc.selection = saved;
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value StrokePath(napi_env env, napi_callback_info info) {
    // strokePath(r,g,b,a,width)：工作路径轮廓带 × 颜色描边到活动层（覆盖度→灰度掩码→
    // 临时 swap 选区走 fillWithColor 核）
    size_t argc = 5;
    napi_value argv[5] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 5) {
        return makeError(env, kErrBadParam, "strokePath(r,g,b,a,width) requires 5 args");
    }
    double c[5] = {0};
    for (size_t i = 0; i < 5; i++) {
        if (napi_get_value_double(env, argv[i], &c[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "strokePath: invalid args");
        }
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "strokePath: no document");
        }
        Layer* l = findLayerLocked(e.doc, e.doc.activeId);
        if (l == nullptr || l->shape != nullptr || l->fill != nullptr) {
            return makeError(env, kErrBadParam, "strokePath: active layer must be a pixel layer");
        }
        if (e.doc.workPath.empty()) {
            return makeError(env, kErrBadParam, "strokePath: no work path");
        }
        const double width = std::min(std::max(1.0, c[4]), 250.0);
        VectorRasterizer r = strokeRasterizer(e.doc.workPath, width, LineCap::Butt, LineJoin::Miter,
                                              4.0, {}, 0.0, kVectorTolerance);
        int32_t bx0, by0, bx1, by1;
        if (!r.pixelBounds(bx0, by0, bx1, by1)) {
            return makeOk(env, nullptr);
        }
        int32_t x0 = std::max(0, bx0);
        int32_t y0 = std::max(0, by0);
        int32_t x1 = std::min(static_cast<int32_t>(e.doc.width), bx1);
        int32_t y1 = std::min(static_cast<int32_t>(e.doc.height), by1);
        if (x0 >= x1 || y0 >= y1) {
            return makeOk(env, nullptr);
        }
        auto cov = r.render(x0, y0, x1, y1);
        auto band = coverageToMaskGrid(cov, x0, y0, x1 - x0, y1 - y0, e.doc.width, e.doc.height);
        if (band == nullptr) {
            return makeError(env, kErrBadParam, "strokePath: empty band");
        }
        std::shared_ptr<const TileGrid> saved = e.doc.selection;
        if (saved != nullptr) {
            e.doc.selection = band;
            band = tiles::combineSelection(e.doc, saved, tiles::SelectionOp::Intersect);
            if (band == nullptr) {
                e.doc.selection = saved;
                return makeOk(env, nullptr);
            }
        }
        e.history.beginEdit(e.doc, "描边路径", static_cast<uint64_t>(e.docRevision));
        std::shared_ptr<const TileGrid> savedNow = e.doc.selection;
        e.doc.selection = band;
        std::string err;
        const bool ok = fillWithColor(e.doc, l->id, FillSource::Foreground,
                                      static_cast<float>(c[0]), static_cast<float>(c[1]),
                                      static_cast<float>(c[2]), 0, 0, 0, err);
        e.doc.selection = savedNow;
        if (!ok) {
            { uint64_t rv = 0; std::string lb; e.history.undo(e.doc, rv, lb); (void)rv; (void)lb; }
            e.doc.selection = saved;
            return makeError(env, kErrBadParam, ("strokePath: " + err).c_str());
        }
        e.doc.selection = saved;
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value SaveWorkPath(napi_env env, napi_callback_info info) {
    // saveWorkPath(name?)：工作路径 → 保存路径（同名覆盖）
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    char name[96] = {0};
    if (argc >= 1) {
        size_t len = 0;
        napi_get_value_string_utf8(env, argv[0], name, sizeof(name), &len);
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.workPath.empty()) {
            return makeError(env, kErrBadParam, "saveWorkPath: no work path");
        }
        std::string nm =
            name[0] != '\0' ? std::string(name) : ("路径 " + std::to_string(e.doc.paths.size() + 1));
        bool replaced = false;
        for (NamedPathEntry& p : e.doc.paths) {
            if (p.name == nm) {
                p.path = e.doc.workPath;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            NamedPathEntry entry;
            entry.name = nm;
            entry.path = e.doc.workPath;
            e.doc.paths.push_back(std::move(entry));
        }
        e.doc.workPath = VectorPath{};
        e.bumpRevisionLocked();
    }
    return makeOk(env, nullptr);
}

napi_value DeleteSavedPath(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double idx = -1;
    if (argc < 1 || napi_get_value_double(env, argv[0], &idx) != napi_ok) {
        return makeError(env, kErrBadParam, "deleteSavedPath(i) requires 1 arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (idx < 0 || static_cast<size_t>(idx) >= e.doc.paths.size()) {
            return makeError(env, kErrBadParam, "deleteSavedPath: bad index");
        }
        e.doc.paths.erase(e.doc.paths.begin() + static_cast<long>(idx));
        e.bumpRevisionLocked();
    }
    return makeOk(env, nullptr);
}

napi_value GetSavedPaths(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    napi_value arr = nullptr;
    napi_create_array(env, &arr);
    for (size_t i = 0; i < e.doc.paths.size(); i++) {
        napi_value obj = nullptr;
        napi_create_object(env, &obj);
        napi_set_named_property(env, obj, "name", makeString(env, e.doc.paths[i].name));
        napi_set_named_property(env, obj, "flat", pathToFlatArray(env, e.doc.paths[i].path));
        napi_set_element(env, arr, static_cast<uint32_t>(i), obj);
    }
    return makeOk(env, arr);
}

napi_value LoadSavedPath(napi_env env, napi_callback_info info) {
    // loadSavedPath(i)：保存路径 → 工作路径
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    double idx = -1;
    if (argc < 1 || napi_get_value_double(env, argv[0], &idx) != napi_ok) {
        return makeError(env, kErrBadParam, "loadSavedPath(i) requires 1 arg");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (idx < 0 || static_cast<size_t>(idx) >= e.doc.paths.size()) {
            return makeError(env, kErrBadParam, "loadSavedPath: bad index");
        }
        e.doc.workPath = e.doc.paths[static_cast<size_t>(idx)].path;
        e.bumpRevisionLocked();
    }
    return makeOk(env, nullptr);
}

napi_value SelectionToWorkPath(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.selection == nullptr) {
            return makeError(env, kErrBadParam, "selectionToWorkPath: no selection");
        }
        VectorPath traced;
        if (!traceSelectionToPath(*e.doc.selection, traced, 1024)) {
            return makeError(env, kErrBadParam, "selectionToWorkPath: empty trace");
        }
        e.doc.workPath = std::move(traced);
        e.bumpRevisionLocked();
    }
    return makeOk(env, nullptr);
}

napi_value DefineCustomShape(napi_env env, napi_callback_info info) {
    // defineCustomShape(name?)：工作路径（空则活动形状层路径）→ 自定义形状库
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    char name[96] = {0};
    if (argc >= 1) {
        size_t len = 0;
        napi_get_value_string_utf8(env, argv[0], name, sizeof(name), &len);
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        VectorPath source = e.doc.workPath;
        if (source.empty()) {
            Layer* l = findLayerLocked(e.doc, e.doc.activeId);
            if (l != nullptr && l->shape != nullptr) {
                source = l->shape->live.kind == ShapeLiveKind::None
                             ? l->shape->path
                             : liveShapePath(l->shape->live);
            }
        }
        if (source.empty()) {
            return makeError(env, kErrBadParam, "defineCustomShape: no path");
        }
        // 归一化到以原点为中心、最长边 = 1 的规范域（放置时按目标尺寸缩放）
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!source.controlBounds(x0, y0, x1, y1)) {
            return makeError(env, kErrBadParam, "defineCustomShape: empty bounds");
        }
        const double w = x1 - x0;
        const double h = y1 - y0;
        const double scale = 1.0 / std::max(1e-9, std::max(w, h));
        const double cx = (x0 + x1) / 2.0;
        const double cy = (y0 + y1) / 2.0;
        for (Subpath& sp : source.subpaths) {
            for (Knot& kn : sp.knots) {
                kn.anchor = vpt((kn.anchor.x - cx) * scale, (kn.anchor.y - cy) * scale);
                kn.inCtrl = vpt((kn.inCtrl.x - cx) * scale, (kn.inCtrl.y - cy) * scale);
                kn.outCtrl = vpt((kn.outCtrl.x - cx) * scale, (kn.outCtrl.y - cy) * scale);
            }
        }
        CustomShapeEntry entry;
        e.doc.customShapeSeq++;
        entry.id = "csh_" + std::to_string(e.doc.customShapeSeq);
        entry.name =
            name[0] != '\0' ? std::string(name) : ("自定形状 " + std::to_string(e.doc.customShapeSeq));
        entry.path = std::move(source);
        e.doc.customShapes.push_back(std::move(entry));
        e.bumpRevisionLocked();
    }
    return makeOk(env, nullptr);
}

napi_value GetCustomShapes(napi_env env, napi_callback_info /*info*/) {
    auto& e = Engine::get();
    std::lock_guard<std::mutex> lk(e.docMutex);
    napi_value arr = nullptr;
    napi_create_array(env, &arr);
    for (size_t i = 0; i < e.doc.customShapes.size(); i++) {
        const CustomShapeEntry& c = e.doc.customShapes[i];
        napi_value obj = nullptr;
        napi_create_object(env, &obj);
        napi_set_named_property(env, obj, "id", makeString(env, c.id));
        napi_set_named_property(env, obj, "name", makeString(env, c.name));
        napi_set_named_property(env, obj, "flat", pathToFlatArray(env, c.path));
        napi_set_element(env, arr, static_cast<uint32_t>(i), obj);
    }
    return makeOk(env, arr);
}

napi_value DeleteCustomShape(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    char id[64] = {0};
    if (argc >= 1) {
        size_t len = 0;
        napi_get_value_string_utf8(env, argv[0], id, sizeof(id), &len);
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        for (size_t i = 0; i < e.doc.customShapes.size(); i++) {
            if (e.doc.customShapes[i].id == id) {
                e.doc.customShapes.erase(e.doc.customShapes.begin() + static_cast<long>(i));
                e.bumpRevisionLocked();
                return makeOk(env, nullptr);
            }
        }
        return makeError(env, kErrBadParam, "deleteCustomShape: id not found");
    }
}

napi_value PlaceCustomShape(napi_env env, napi_callback_info info) {
    // placeCustomShape(id, cx, cy, w, h)：自定义形状 → 新形状层（以 (cx,cy) 为中心、
    // 内切 w×h；v1 等比取 min(w,h)）。
    size_t argc = 5;
    napi_value argv[5] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 5) {
        return makeError(env, kErrBadParam, "placeCustomShape requires 5 args");
    }
    char id[64] = {0};
    size_t len = 0;
    napi_get_value_string_utf8(env, argv[0], id, sizeof(id), &len);
    double a[4] = {0};
    for (size_t i = 0; i < 4; i++) {
        if (napi_get_value_double(env, argv[i + 1], &a[i]) != napi_ok) {
            return makeError(env, kErrBadParam, "placeCustomShape: invalid args");
        }
    }
    auto& e = Engine::get();
    double newId = 0;
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        if (e.doc.width == 0 || e.doc.height == 0) {
            return makeError(env, kErrNoDoc, "placeCustomShape: no document");
        }
        const CustomShapeEntry* found = nullptr;
        for (const CustomShapeEntry& c : e.doc.customShapes) {
            if (c.id == id) {
                found = &c;
                break;
            }
        }
        if (found == nullptr) {
            return makeError(env, kErrBadParam, "placeCustomShape: id not found");
        }
        const double cx = a[0];
        const double cy = a[1];
        const double half = std::max(1.0, std::min(std::fabs(a[2]), std::fabs(a[3]))) / 2.0;
        auto state = std::make_shared<ShapeLayerState>();
        state->path = found->path;
        // 规范域 [-0.5,0.5]² → 目标域（half 缩放 + 中心平移）
        for (Subpath& sp : state->path.subpaths) {
            for (Knot& kn : sp.knots) {
                kn.anchor = vpt(cx + kn.anchor.x * half * 2.0, cy + kn.anchor.y * half * 2.0);
                kn.inCtrl = vpt(cx + kn.inCtrl.x * half * 2.0, cy + kn.inCtrl.y * half * 2.0);
                kn.outCtrl = vpt(cx + kn.outCtrl.x * half * 2.0, cy + kn.outCtrl.y * half * 2.0);
            }
        }
        state->fillKind = 1;
        state->fillColor[0] = e.fgColor[0];
        state->fillColor[1] = e.fgColor[1];
        state->fillColor[2] = e.fgColor[2];
        state->fillColor[3] = 1.0f;
        std::string err;
        if (!validateShape(state->path, nullptr, err)) {
            return makeError(env, kErrBadParam, ("placeCustomShape: " + err).c_str());
        }
        Layer layer;
        layer.id = e.nextLayerId();
        layer.name = found->name;
        layer.shape = std::move(state);
        size_t at = e.doc.layers.size();
        for (size_t i = 0; i < e.doc.layers.size(); ++i) {
            if (e.doc.layers[i].id == e.doc.activeId) {
                at = i + 1;
                break;
            }
        }
        e.history.beginEdit(e.doc, layer.name, static_cast<uint64_t>(e.docRevision));
        e.doc.layers.insert(e.doc.layers.begin() + static_cast<long>(at), std::move(layer));
        Layer& inserted = e.doc.layers[at];
        e.doc.activeId = inserted.id;
        if (!shapeRenderLayer(e.doc, inserted, err)) {
            { uint64_t rv = 0; std::string lb; e.history.undo(e.doc, rv, lb); (void)rv; (void)lb; }
            return makeError(env, kErrBadParam, ("placeCustomShape: " + err).c_str());
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        newId = static_cast<double>(inserted.id);
        e.bumpRevisionLocked();
        e.requestRender();
    }
    napi_value data = nullptr;
    napi_create_object(env, &data);
    napi_set_named_property(env, data, "id", makeDouble(env, newId));
    return makeOk(env, data);
}

napi_value SetLayerVectorMask(napi_env env, napi_callback_info info) {
    // setLayerVectorMask(id, flat[])——flat 空数组 = 删除矢量蒙版
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "setLayerVectorMask(id, flat) requires 2 args");
    }
    double id = 0;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok) {
        return makeError(env, kErrBadParam, "setLayerVectorMask: invalid id");
    }
    VectorPath path;
    if (!flatArrayToPath(env, argv[1], path)) {
        return makeError(env, kErrBadParam, "setLayerVectorMask: bad path array");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr) {
            return makeError(env, kErrBadParam, "setLayerVectorMask: layer not found");
        }
        e.history.beginEdit(e.doc, "矢量蒙版", static_cast<uint64_t>(e.docRevision));
        if (path.empty() && !path.inverted) {
            l->vectorMask = nullptr;
        } else {
            auto m = std::make_shared<VectorMaskData>();
            m->path = std::move(path);
            l->vectorMask = std::move(m);
        }
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        restyleDocument(e.doc);  // M18d：矢量蒙版设置后立即重派生
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

napi_value SetVectorMaskEnabled(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        return makeError(env, kErrBadParam, "setVectorMaskEnabled(id, v) requires 2 args");
    }
    double id = 0;
    bool v = false;
    if (napi_get_value_double(env, argv[0], &id) != napi_ok ||
        napi_get_value_bool(env, argv[1], &v) != napi_ok) {
        return makeError(env, kErrBadParam, "setVectorMaskEnabled: invalid args");
    }
    auto& e = Engine::get();
    {
        std::lock_guard<std::mutex> lk(e.docMutex);
        Layer* l = findLayerLocked(e.doc, static_cast<LayerId>(id));
        if (l == nullptr || l->vectorMask == nullptr) {
            return makeError(env, kErrBadParam, "setVectorMaskEnabled: no vector mask");
        }
        auto m = std::make_shared<VectorMaskData>(*l->vectorMask);
        m->enabled = v;
        e.history.beginEdit(e.doc, "矢量蒙版开关", static_cast<uint64_t>(e.docRevision));
        l->vectorMask = std::move(m);
        e.history.endEdit(e.doc, static_cast<uint64_t>(e.docRevision));
        restyleDocument(e.doc);  // M18d：矢量蒙版变更立即重派生
        e.bumpRevisionLocked();
        e.requestRender();
    }
    return makeOk(env, nullptr);
}

}  // namespace bridge
}  // namespace montage
