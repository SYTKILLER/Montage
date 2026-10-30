// M26 插件 JSVM 宿主（P8，设备侧；宿主 .debug/p8 不编译本文件，纯逻辑见 plugins.cpp）。
// 方案：docs/40-方案设计/14 §3-§4。每插件运行 = 独立 VM+Env（堆隔离 maxOld/maxYoung），
// 在调用线程上创建-执行-销毁（Env 线程受限）；无中断 API → deadline 于宿主调用点
// （tick/log/progress/band 边界）协作检查，纯 JS 死循环线程弃置 + 插件 poison（有界泄漏，
// 方案 §4.2）。语义对拍 photocraft crates/plugins runtime.rs/apply.rs。

#include "engine/plugins_jsvm.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <ark_runtime/jsvm.h>
#include <hilog/log.h>

#include "engine/filter_ops.h"
#include "engine/pixel_access.h"
#include "engine/view_ops.h"

namespace montage {
namespace plugins {

namespace {

constexpr uint32_t kDomain = 0xD0026;
constexpr const char* kTag = "Montage.M26";
// JS 堆上限（方案 §4.2）：maxOld 128MB / maxYoung 16MB。band 缓冲为宿主内存（外部记账）。
constexpr size_t kMaxOldBytes = 128u << 20;
constexpr size_t kMaxYoungBytes = 16u << 20;
constexpr int kMaxLogCalls = 200;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::once_flag g_jsvmInit;
void ensureJsvmInit() {
    std::call_once(g_jsvmInit, []() {
        // --jitless：模拟器（x86_64）V8 在 libuv 工作线程热循环 JIT 编译期 SEGV
        //（cppcrash 实证：短脚本解释执行正常、apply 热循环崩 v8_shared JIT 路径）。
        // 解释执行慢 ~10x——插件滤镜本就慢路径，可接受；真机复评登记。
        // V8 惯例：argv[0] 为程序名被跳过，flag 自 argv[1] 起生效
        static char prog[] = "montage";
        static char flag0[] = "--jitless";
        static char* argv[] = {prog, flag0};
        static int argc = 2;
        JSVM_InitOptions opt{};
        opt.argc = &argc;
        opt.argv = argv;
        OH_JSVM_Init(&opt);
    });
}

// ---- 每次运行的共享上下文（宿主回调与 runner 同线程；monitor 线程仅置 abort）----
struct RunCtx {
    std::atomic<int64_t> deadline{0};
    std::atomic<bool> abort{false};
    std::atomic<int> logCount{0};
    JSVM_CallbackStruct tickCb{};   // data = this（deadline/计费检查点）
    JSVM_CallbackStruct logCb{};
    JSVM_CallbackStruct progCb{};
};

bool deadlineHit(RunCtx& c) {
    return c.abort.load(std::memory_order_relaxed) || nowMs() >= c.deadline.load(std::memory_order_relaxed);
}

// ---- JSVM 值助手（全部单线程 Env 内使用）----
JSVM_Value jUndefined(JSVM_Env env) {
    JSVM_Value v = nullptr;
    OH_JSVM_GetUndefined(env, &v);
    return v;
}

bool jStr(JSVM_Env env, JSVM_Value v, std::string& out) {
    size_t n = 0;
    if (OH_JSVM_GetValueStringUtf8(env, v, nullptr, 0, &n) != JSVM_OK) {
        return false;
    }
    out.assign(n + 1, '\0');
    size_t copied = 0;
    if (OH_JSVM_GetValueStringUtf8(env, v, out.data(), n + 1, &copied) != JSVM_OK) {
        return false;
    }
    out.resize(copied);
    return true;
}

bool jType(JSVM_Env env, JSVM_Value v, JSVM_ValueType& t) {
    return OH_JSVM_Typeof(env, v, &t) == JSVM_OK;
}

bool jIsType(JSVM_Env env, JSVM_Value v, JSVM_ValueType t) {
    JSVM_ValueType got = JSVM_UNDEFINED;
    return jType(env, v, got) && got == t;
}

bool jGetProp(JSVM_Env env, JSVM_Value obj, const char* key, JSVM_Value* out) {
    return OH_JSVM_GetNamedProperty(env, obj, key, out) == JSVM_OK && *out != nullptr;
}

// 异常串化（清挂起异常；失败回退通用文案）。
std::string jTakeException(JSVM_Env env) {
    JSVM_Value ex = nullptr;
    std::string msg = "plugin exception";
    if (OH_JSVM_GetAndClearLastException(env, &ex) == JSVM_OK && ex != nullptr) {
        JSVM_Value strV = nullptr;
        std::string s;
        if (OH_JSVM_CoerceToString(env, ex, &strV) == JSVM_OK && jStr(env, strV, s) &&
            !s.empty()) {
            msg = s;
        }
    }
    // 截断（异常文本可能巨大）
    if (msg.size() > 512) {
        msg.resize(512);
    }
    return msg;
}

// ---- 宿主函数三枚（tick/log/progress；data = RunCtx*）----
JSVM_Value TickCb(JSVM_Env env, JSVM_CallbackInfo info) {
    void* data = nullptr;
    OH_JSVM_GetCbInfo(env, info, nullptr, nullptr, nullptr, &data);
    auto* c = static_cast<RunCtx*>(data);
    if (c != nullptr && deadlineHit(*c)) {
        OH_JSVM_ThrowError(env, nullptr, "plugin deadline exceeded");
    }
    return jUndefined(env);
}

JSVM_Value LogCb(JSVM_Env env, JSVM_CallbackInfo info) {
    void* data = nullptr;
    size_t argc = 1;
    JSVM_Value argv[1] = {nullptr};
    OH_JSVM_GetCbInfo(env, info, &argc, argv, nullptr, &data);
    auto* c = static_cast<RunCtx*>(data);
    if (c != nullptr && argc >= 1 && argv[0] != nullptr) {
        const int n = c->logCount.fetch_add(1, std::memory_order_relaxed);
        if (n < kMaxLogCalls) {
            std::string s;
            if (jStr(env, argv[0], s)) {
                if (s.size() > 256) {
                    s.resize(256);
                }
                OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "plugin: %{public}s", s.c_str());
            }
        }
    }
    return jUndefined(env);
}

JSVM_Value ProgressCb(JSVM_Env env, JSVM_CallbackInfo info) {
    void* data = nullptr;
    size_t argc = 1;
    JSVM_Value argv[1] = {nullptr};
    OH_JSVM_GetCbInfo(env, info, &argc, argv, nullptr, &data);
    if (argc >= 1 && argv[0] != nullptr) {
        double p = 0;
        if (OH_JSVM_GetValueDouble(env, argv[0], &p) == JSVM_OK && std::isfinite(p)) {
            OH_LOG_Print(LOG_APP, LOG_DEBUG, kDomain, kTag, "plugin progress %{public}f", p);
        }
    }
    return jUndefined(env);
}

// ---- VM 生命周期（单线程 RAII）----
struct VmSession {
    JSVM_VM vm = nullptr;
    JSVM_Env env = nullptr;
    JSVM_EnvScope scope = nullptr;
    bool loaded = false;

    bool create(std::string& err) {
        ensureJsvmInit();
        JSVM_CreateVMOptions opt{};
        opt.maxOldGenerationSize = kMaxOldBytes;
        opt.maxYoungGenerationSize = kMaxYoungBytes;
        if (OH_JSVM_CreateVM(&opt, &vm) != JSVM_OK || vm == nullptr) {
            err = "cannot create plugin VM";
            return false;
        }
        if (OH_JSVM_CreateEnv(vm, 0, nullptr, &env) != JSVM_OK || env == nullptr) {
            err = "cannot create plugin env";
            return false;
        }
        if (OH_JSVM_OpenEnvScope(env, &scope) != JSVM_OK) {
            err = "cannot open env scope";
            return false;
        }
        return true;
    }

    // 载入源码（编译 + 顶层执行）。异常/缺导出 → false + err。
    bool loadSource(const std::string& source, std::string& err) {
        JSVM_HandleScope hs = nullptr;
        if (OH_JSVM_OpenHandleScope(env, &hs) != JSVM_OK) {
            err = "cannot open handle scope";
            return false;
        }
        JSVM_Value srcV = nullptr;
        if (OH_JSVM_CreateStringUtf8(env, source.data(), source.size(), &srcV) != JSVM_OK) {
            err = "source string error";
            OH_JSVM_CloseHandleScope(env, hs);
            return false;
        }
        JSVM_Script script = nullptr;
        bool cacheRejected = false;
        if (OH_JSVM_CompileScript(env, srcV, nullptr, 0, false, &cacheRejected, &script) !=
                JSVM_OK ||
            script == nullptr) {
            err = "compile: " + jTakeException(env);
            OH_JSVM_CloseHandleScope(env, hs);
            return false;
        }
        JSVM_Value result = nullptr;
        if (OH_JSVM_RunScript(env, script, &result) != JSVM_OK) {
            err = "run: " + jTakeException(env);
            OH_JSVM_CloseHandleScope(env, hs);
            return false;
        }
        OH_JSVM_CloseHandleScope(env, hs);
        loaded = true;
        return true;
    }

    void destroy() {
        if (env != nullptr && scope != nullptr) {
            OH_JSVM_CloseEnvScope(env, scope);
        }
        scope = nullptr;
        env = nullptr;
        if (vm != nullptr) {
            OH_JSVM_DestroyVM(vm);
        }
        vm = nullptr;
    }

    ~VmSession() { destroy(); }
};

// ---- manifest JS 对象 → RawManifest ----
bool readManifest(JSVM_Env env, JSVM_Value obj, RawManifest& out, std::string& err) {
    JSVM_ValueType t = JSVM_UNDEFINED;
    if (!jType(env, obj, t) || (t != JSVM_OBJECT)) {
        err = "manifest: montage_manifest() must return an object";
        return false;
    }
    auto getStr = [&](const char* key, std::string& dst, bool& has) {
        JSVM_Value v = nullptr;
        has = false;
        if (jGetProp(env, obj, key, &v) && jIsType(env, v, JSVM_STRING)) {
            has = jStr(env, v, dst);
        }
    };
    getStr("id", out.id, out.hasId);
    getStr("name", out.name, out.hasName);
    getStr("version", out.version, out.hasVersion);
    getStr("description", out.description, out.hasDescription);
    getStr("author", out.author, out.hasAuthor);
    getStr("kind", out.kind, out.hasKind);
    getStr("area", out.area, out.hasArea);
    JSVM_Value ov = nullptr;
    if (jGetProp(env, obj, "overlap", &ov) && jIsType(env, ov, JSVM_NUMBER)) {
        OH_JSVM_GetValueDouble(env, ov, &out.overlap);
        out.hasOverlap = true;
    }
    // params：{ name: {type, min, max, default, options} } 有序表
    JSVM_Value params = nullptr;
    if (jGetProp(env, obj, "params", &params) && jIsType(env, params, JSVM_OBJECT)) {
        JSVM_Value names = nullptr;
        uint32_t n = 0;
        if (OH_JSVM_GetPropertyNames(env, params, &names) == JSVM_OK && names != nullptr &&
            OH_JSVM_GetArrayLength(env, names, &n) == JSVM_OK) {
            for (uint32_t i = 0; i < n && out.params.size() <= kMaxParams; ++i) {
                JSVM_Value keyV = nullptr;
                std::string key;
                if (OH_JSVM_GetElement(env, names, i, &keyV) != JSVM_OK || !jStr(env, keyV, key)) {
                    continue;
                }
                JSVM_Value spec = nullptr;
                if (!jGetProp(env, params, key.c_str(), &spec) ||
                    !jIsType(env, spec, JSVM_OBJECT)) {
                    continue;
                }
                ParamSpec ps;
                JSVM_Value typeV = nullptr;
                std::string type;
                if (!jGetProp(env, spec, "type", &typeV) || !jStr(env, typeV, type)) {
                    err = "manifest: parameter \"" + key + "\": missing type";
                    return false;
                }
                auto getNum = [&](const char* k2, double& d2) {
                    JSVM_Value v2 = nullptr;
                    if (jGetProp(env, spec, k2, &v2) && jIsType(env, v2, JSVM_NUMBER)) {
                        OH_JSVM_GetValueDouble(env, v2, &d2);
                        return true;
                    }
                    return false;
                };
                if (type == "number") {
                    ps.type = ParamSpec::Number;
                    getNum("min", ps.mn);
                    getNum("max", ps.mx);
                    getNum("default", ps.def);
                } else if (type == "int") {
                    ps.type = ParamSpec::Int;
                    getNum("min", ps.mn);
                    getNum("max", ps.mx);
                    getNum("default", ps.def);
                } else if (type == "bool") {
                    ps.type = ParamSpec::Bool;
                    JSVM_Value dv = nullptr;
                    if (jGetProp(env, spec, "default", &dv)) {
                        bool b = false;
                        if (OH_JSVM_GetValueBool(env, dv, &b) == JSVM_OK) {
                            ps.bdef = b;
                        }
                    }
                } else if (type == "choice") {
                    ps.type = ParamSpec::Choice;
                    JSVM_Value opts = nullptr;
                    uint32_t on = 0;
                    bool isArr = false;
                    if (jGetProp(env, spec, "options", &opts) &&
                        OH_JSVM_IsArray(env, opts, &isArr) == JSVM_OK && isArr) {
                        if (OH_JSVM_GetArrayLength(env, opts, &on) == JSVM_OK) {
                            for (uint32_t k = 0; k < on && ps.options.size() <= 64; ++k) {
                                JSVM_Value ov2 = nullptr;
                                std::string os;
                                if (OH_JSVM_GetElement(env, opts, k, &ov2) == JSVM_OK &&
                                    jStr(env, ov2, os)) {
                                    ps.options.push_back(os);
                                }
                            }
                        }
                    }
                    JSVM_Value dv = nullptr;
                    std::string ds;
                    if (jGetProp(env, spec, "default", &dv) && jStr(env, dv, ds)) {
                        ps.cdef = ds;
                    }
                } else {
                    err = "manifest: parameter \"" + key + "\": unknown type \"" + type + "\"";
                    return false;
                }
                out.params.emplace_back(key, ps);
            }
        }
    }
    out.complete = out.hasId && out.hasName && out.hasKind;
    return true;
}

}  // namespace


// ---- PluginHost：JSVM 执行线程收敛 ----
// 模拟器 x86_64 两例 cppcrash 同点（libv8+0x3b2680，rax=0）：libuv async-work 线程与自建
// std::thread 上跑 JSVM 热循环均崩，栈底 emutls 分配 + rwlock；主线程 register（跑 JS）恒成功。
// 结论：模拟器 musl emutls 对非主线程 V8 TLS 惰性分配不可用 → JSVM 限定主线程执行。
// 执行器由 bridge 注入（napi threadsafe function 回调 = ArkTS 主线程）；注入前直调
// （宿主测试/无 ArkTS 场景）。运行期 UI 冻结由 deadline 60s 兜底；真机复评恢复独立线程。
class PluginHost {
  public:
    static PluginHost& get() {
        static PluginHost inst;
        return inst;
    }

    using Fn = std::function<void(std::string&)>;
    using Executor = std::function<void(const Fn&, std::string&, bool&)>;

    // bridge 在 napi 注册期注入（JS 线程执行器）。线程安全：主线程串行注入。
    void setExecutor(Executor ex) {
        std::lock_guard<std::mutex> lk(mtx_);
        executor_ = std::move(ex);
    }

    bool submit(const Fn& fn, std::string& err) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (executor_ != nullptr) {
            bool ok = false;
            executor_(fn, err, ok);
            return ok;
        }
        try {
            fn(err);
            return true;
        } catch (const std::exception& e) {
            if (err.empty()) {
                err = std::string("host: ") + e.what();
            }
            return false;
        } catch (...) {
            if (err.empty()) {
                err = "host: unknown exception";
            }
            return false;
        }
    }

  private:
    std::mutex mtx_;
    Executor executor_;
};

// ---- 注册期校验运行（方案 §4.3；bridge 在 async work 线程调用）----
bool pluginValidateSourceOnHost(const std::string& source, Manifest& out, std::string& err) {
    if (source.size() > kMaxSourceBytes) {
        err = "plugin source is " + std::to_string(source.size()) + " bytes (limit 1048576)";
        return false;
    }
    VmSession s;
    if (!s.create(err)) {
        return false;
    }
    RunCtx ctx;
    ctx.deadline.store(nowMs() + kRegisterTimeMs, std::memory_order_relaxed);
    if (!s.loadSource(source, err)) {
        return false;
    }
    JSVM_HandleScope hs = nullptr;
    if (OH_JSVM_OpenHandleScope(s.env, &hs) != JSVM_OK) {
        err = "cannot open handle scope";
        return false;
    }
    JSVM_Value global = nullptr;
    OH_JSVM_GetGlobal(s.env, &global);
    // 必需导出检查：montage_manifest / montage_apply 都是函数
    JSVM_Value manFn = nullptr;
    JSVM_Value applyFn = nullptr;
    if (!jGetProp(s.env, global, "montage_manifest", &manFn) ||
        !jIsType(s.env, manFn, JSVM_FUNCTION)) {
        err = "plugin must define function montage_manifest()";
        OH_JSVM_CloseHandleScope(s.env, hs);
        return false;
    }
    if (!jGetProp(s.env, global, "montage_apply", &applyFn) ||
        !jIsType(s.env, applyFn, JSVM_FUNCTION)) {
        err = "plugin must define function montage_apply(api)";
        OH_JSVM_CloseHandleScope(s.env, hs);
        return false;
    }
    JSVM_Value manObj = nullptr;
    if (OH_JSVM_CallFunction(s.env, global, manFn, 0, nullptr, &manObj) != JSVM_OK) {
        err = "montage_manifest: " + jTakeException(s.env);
        OH_JSVM_CloseHandleScope(s.env, hs);
        return false;
    }
    if (deadlineHit(ctx)) {
        err = "plugin exceeded its time budget";
        OH_JSVM_CloseHandleScope(s.env, hs);
        return false;
    }
    RawManifest raw;
    if (!readManifest(s.env, manObj, raw, err)) {
        OH_JSVM_CloseHandleScope(s.env, hs);
        return false;
    }
    const bool ok = validateManifest(raw, out, err);
    OH_JSVM_CloseHandleScope(s.env, hs);
    return ok;
}

// ---- 运行会话（每 pluginRun 一个；bridge async work 线程内使用；pimpl 持 VmSession+RunCtx）----
struct PluginRunner::Impl {
    std::unique_ptr<VmSession> sess;
    std::unique_ptr<RunCtx> ctx;
};

PluginRunner::PluginRunner() = default;
PluginRunner::~PluginRunner() {
    stop();
}

bool PluginRunner::start(const std::string& source, int64_t timeMs, std::string& err) {
    stop();
    impl_ = std::make_unique<Impl>();
    impl_->sess = std::make_unique<VmSession>();
    impl_->ctx = std::make_unique<RunCtx>();
    impl_->ctx->deadline.store(nowMs() + timeMs, std::memory_order_relaxed);
    if (!impl_->sess->create(err)) {
        impl_.reset();
        return false;
    }
    if (!impl_->sess->loadSource(source, err)) {
        impl_.reset();
        return false;
    }
    return true;
}

void PluginRunner::stop() {
    impl_.reset();  // RAII destroy（VM 析构于 Impl 内）
}

bool PluginRunner::aborted() const {
    return impl_ == nullptr || impl_->ctx == nullptr || deadlineHit(*impl_->ctx);
}

// 单 band 调用：pixels 为 iw×ih×4 f32（原地写回）。meta 为 band 输入域信息。
bool PluginRunner::applyBand(float* pixels, int iw, int ih, const BandMeta& meta,
                             const std::vector<std::pair<std::string, PValue>>& params,
                             std::string& err) {
    if (impl_ == nullptr || impl_->sess == nullptr || impl_->ctx == nullptr) {
        err = "plugin runner not started";
        return false;
    }
    if (deadlineHit(*impl_->ctx)) {
        err = "plugin exceeded its time budget";
        return false;
    }
    JSVM_Env env = impl_->sess->env;
    JSVM_HandleScope hs = nullptr;
    if (OH_JSVM_OpenHandleScope(env, &hs) != JSVM_OK) {
        err = "cannot open handle scope";
        return false;
    }
    bool ok = false;
    do {
        JSVM_Value global = nullptr;
        OH_JSVM_GetGlobal(env, &global);
        JSVM_Value applyFn = nullptr;
        if (!jGetProp(env, global, "montage_apply", &applyFn) ||
            !jIsType(env, applyFn, JSVM_FUNCTION)) {
            err = "plugin must define function montage_apply(api)";
            break;
        }
        // api 对象
        JSVM_Value api = nullptr;
        JSVM_Value img = nullptr;
        JSVM_Value pj = nullptr;
        JSVM_Value ab = nullptr;
        void* abData = nullptr;
        if (OH_JSVM_CreateObject(env, &api) != JSVM_OK ||
            OH_JSVM_CreateObject(env, &img) != JSVM_OK ||
            OH_JSVM_CreateObject(env, &pj) != JSVM_OK) {
            err = "jsvm: create object failed";
            break;
        }
        auto setD = [&](JSVM_Value o, const char* k, double v) {
            JSVM_Value n = nullptr;
            if (OH_JSVM_CreateDouble(env, v, &n) == JSVM_OK) {
                OH_JSVM_SetNamedProperty(env, o, k, n);
            }
        };
        auto setI = [&](JSVM_Value o, const char* k, int32_t v) {
            JSVM_Value n = nullptr;
            if (OH_JSVM_CreateInt32(env, v, &n) == JSVM_OK) {
                OH_JSVM_SetNamedProperty(env, o, k, n);
            }
        };
        setI(img, "x", meta.x);
        setI(img, "y", meta.y);
        setI(img, "width", iw);
        setI(img, "height", ih);
        setI(img, "overlap", static_cast<int32_t>(meta.overlap));
        setI(img, "canvasWidth", meta.canvasW);
        setI(img, "canvasHeight", meta.canvasH);
        setI(img, "channels", 4);
        JSVM_Value alphaT = nullptr;
        OH_JSVM_GetBoolean(env, true, &alphaT);
        OH_JSVM_SetNamedProperty(env, img, "alpha", alphaT);
        JSVM_Value modeV = nullptr;
        OH_JSVM_CreateStringUtf8(env, "rgb", JSVM_AUTO_LENGTH, &modeV);
        OH_JSVM_SetNamedProperty(env, img, "mode", modeV);
        setI(img, "depth", 8);
        setI(img, "bandIndex", meta.bandIndex);
        setI(img, "bandCount", meta.bandCount);
        // pixels ArrayBuffer（iw*ih*4 f32）
        const size_t bytes = static_cast<size_t>(iw) * ih * 4u * sizeof(float);
        if (OH_JSVM_CreateArraybuffer(env, bytes, &abData, &ab) != JSVM_OK || abData == nullptr) {
            err = "jsvm: cannot allocate pixel buffer";
            break;
        }
        std::memcpy(abData, pixels, bytes);
        OH_JSVM_SetNamedProperty(env, api, "image", img);
        OH_JSVM_SetNamedProperty(env, api, "pixels", ab);
        // params（resolved 有序表 → JS 对象）
        for (const auto& kv : params) {
            const PValue& v = kv.second;
            if (v.t == PValue::Num) {
                JSVM_Value n = nullptr;
                if (OH_JSVM_CreateDouble(env, v.num, &n) == JSVM_OK) {
                    OH_JSVM_SetNamedProperty(env, pj, kv.first.c_str(), n);
                }
            } else if (v.t == PValue::Bool) {
                JSVM_Value b = nullptr;
                if (OH_JSVM_GetBoolean(env, v.b, &b) == JSVM_OK) {
                    OH_JSVM_SetNamedProperty(env, pj, kv.first.c_str(), b);
                }
            } else {
                JSVM_Value s = nullptr;
                if (OH_JSVM_CreateStringUtf8(env, v.s.data(), v.s.size(), &s) == JSVM_OK) {
                    OH_JSVM_SetNamedProperty(env, pj, kv.first.c_str(), s);
                }
            }
        }
        OH_JSVM_SetNamedProperty(env, api, "params", pj);
        // 宿主函数：tick/log/progress（JSVM_Callback = CallbackStruct*{cb,data}，与 napi 异；
        // data = RunCtx → 回调内做时限/取消检查）
        RunCtx* rc = impl_->ctx.get();
        rc->tickCb = {TickCb, rc};
        rc->logCb = {LogCb, rc};
        rc->progCb = {ProgressCb, rc};
        JSVM_Value fn = nullptr;
        if (OH_JSVM_CreateFunction(env, "tick", JSVM_AUTO_LENGTH, &rc->tickCb, &fn) == JSVM_OK) {
            OH_JSVM_SetNamedProperty(env, api, "tick", fn);
        }
        if (OH_JSVM_CreateFunction(env, "log", JSVM_AUTO_LENGTH, &rc->logCb, &fn) == JSVM_OK) {
            OH_JSVM_SetNamedProperty(env, api, "log", fn);
        }
        if (OH_JSVM_CreateFunction(env, "progress", JSVM_AUTO_LENGTH, &rc->progCb, &fn) ==
            JSVM_OK) {
            OH_JSVM_SetNamedProperty(env, api, "progress", fn);
        }
        // 调用
        JSVM_Value result = nullptr;
        if (OH_JSVM_CallFunction(env, api, applyFn, 1, &api, &result) != JSVM_OK) {
            err = "montage_apply: " + jTakeException(env);
            break;
        }
        bool pending = false;
        if (OH_JSVM_IsExceptionPending(env, &pending) == JSVM_OK && pending) {
            err = "montage_apply: " + jTakeException(env);
            break;
        }
        if (deadlineHit(*impl_->ctx)) {
            err = "plugin exceeded its time budget";
            break;
        }
        // 取回像素
        void* outData = nullptr;
        size_t outLen = 0;
        if (OH_JSVM_GetArraybufferInfo(env, ab, &outData, &outLen) != JSVM_OK ||
            outData == nullptr || outLen != bytes) {
            err = "plugin pixel buffer unavailable";
            break;
        }
        std::memcpy(pixels, outData, bytes);
        ok = true;
    } while (false);
    OH_JSVM_CloseHandleScope(env, hs);
    return ok;
}

// ---- poison 监视（弃线程路径的登记员；纯宿主线，无 JSVM 调用）----
void startPoisonMonitor(const std::string& pluginId, int64_t deadline, int64_t timeMs) {
    std::thread([pluginId, deadline, timeMs]() {
        const int64_t end = deadline + timeMs;  // start 时取的 deadline + 预算
        const int64_t wait = end - nowMs();
        if (wait > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(wait));
        }
        PluginRegistry::get().poison(pluginId);
    }).detach();
}

// ---- 滤镜执行（方案 §4.1 管线；work = 快照副本，bridge 发布段负责入账）----
namespace {

Layer* findLayerById(std::vector<Layer>& layers, LayerId id) {
    for (Layer& l : layers) {
        if (l.id == id) {
            return &l;
        }
    }
    return nullptr;
}

}  // namespace

bool pluginRunFilterOnHost(Document& work, LayerId layerId, const PluginEntry& entry,
                           const std::vector<std::pair<std::string, PValue>>& params,
                           std::string& err) {
    Layer* layer = findLayerById(work.layers, layerId);
    if (layer == nullptr) {
        err = "layer not found";
        return false;
    }
    if (layer->pixels == nullptr) {
        err = "layer is empty";
        return false;
    }
    if (layer->text != nullptr) {
        err = "rasterize text layer first";
        return false;
    }
    // 深色层垫片：U8 工作网格，析构升位写回（filter_ops 同式）
    const pixel_access::DeepEditGuard deepGuard(*layer, layer->pixels->revision + 1);
    const TileGrid& grid = *layer->pixels;
    // 层 origin（选区采样与选区 bbox 换算）
    const int orgX = static_cast<int>(std::lround(layer->transform.originX));
    const int orgY = static_cast<int>(std::lround(layer->transform.originY));
    // area（网格局部坐标）：content = 内容 bbox；canvas = 层网格全域（v1 语义，登记）
    IRect area;
    if (entry.man.canvasArea) {
        area = IRect{0, 0, static_cast<int32_t>(grid.cols * kTileSize),
                     static_cast<int32_t>(grid.rows * kTileSize)};
    } else {
        uint32_t bx = 0, by = 0, bw = 0, bh = 0;
        if (!layerContentBBox(grid, bx, by, bw, bh)) {
            err = "no content";
            return false;
        }
        area = IRect{static_cast<int32_t>(bx), static_cast<int32_t>(by),
                     static_cast<int32_t>(bx + bw), static_cast<int32_t>(by + bh)};
    }
    // 选区（文档域 bbox → 网格局部 → 交 area）
    const TileGrid* sel = work.selection.get();
    if (sel != nullptr) {
        double sx = 0, sy = 0, sw = 0, sh = 0;
        if (selectionBBox(work, sx, sy, sw, sh)) {
            const IRect selLocal{static_cast<int32_t>(std::lround(sx)) - orgX,
                                 static_cast<int32_t>(std::lround(sy)) - orgY,
                                 static_cast<int32_t>(std::lround(sx + sw)) - orgX,
                                 static_cast<int32_t>(std::lround(sy + sh)) - orgY};
            area = iIntersect(area, selLocal);
        }
    }
    if (area.empty()) {
        return true;  // 无输出域 = 无操作成功
    }
    // extent = 网格全域（读端边缘复制边界；v1 canvas=网格全域同源，登记）
    const IRect extent{0, 0, static_cast<int32_t>(grid.cols * kTileSize),
                       static_cast<int32_t>(grid.rows * kTileSize)};
    // band 划分
    std::vector<IRect> bands;
    planBands(area, bands);
    // 预算闸（v1 与滤镜族同量级：area ≤ 16MP；band 缓冲 4MB 级已限单带内存）
    const uint64_t areaPx = static_cast<uint64_t>(area.width()) * area.height();
    if (areaPx > 16ull * 1000 * 1000) {
        err = "plugin area exceeds budget";
        return false;
    }
    // 运行会话
    const int64_t startedAt = nowMs();
    PluginRunner runner;
    if (!runner.start(entry.source, kRunTimeMs, err)) {
        return false;
    }
    std::vector<uint8_t> inU8;
    std::vector<float> buf;
    std::vector<float> orig;
    std::vector<uint8_t> strip(static_cast<size_t>(area.width()) * area.height() * 4u, 0);
    int idx = 0;
    for (const IRect& band : bands) {
        const IRect in = bandInput(band, entry.man.overlap);
        readGridClamped(grid, in, extent, inU8);
        const int iw = in.width();
        const int ih = in.height();
        const size_t n = static_cast<size_t>(iw) * ih * 4u;
        buf.resize(n);
        for (size_t i = 0; i < n; ++i) {
            buf[i] = static_cast<float>(inU8[i]) * (1.0f / 255.0f);
        }
        orig = buf;
        BandMeta meta;
        meta.x = in.x0;
        meta.y = in.y0;
        meta.overlap = entry.man.overlap;
        meta.canvasW = work.width;
        meta.canvasH = work.height;
        meta.bandIndex = idx++;
        meta.bandCount = static_cast<int32_t>(bands.size());
        if (!runner.applyBand(buf.data(), iw, ih, meta, params, err)) {
            return false;
        }
        // 清洗 + 选区 lerp + 写 strip（输出域 = band 在 in 域内的偏移）
        const float* bandPx = buf.data() +
                              (static_cast<size_t>(band.y0 - in.y0) * iw + (band.x0 - in.x0)) * 4u;
        const float* bandOrig = orig.data() +
                                (static_cast<size_t>(band.y0 - in.y0) * iw + (band.x0 - in.x0)) * 4u;
        finishBand(bandPx, bandOrig, band.width(), band.height(), sel, orgX, orgY, band,
                   strip.data(), area.width(), area.x0, area.y0);
    }
    // 发布（U8 网格；DeepEditGuard 析构升位）
    layer->pixels = publishAreaStrip(grid, area, strip.data(), nextGridRevision());
    if (layer->mask != nullptr && layer->mask->enabled) {
        layer->render = composeMasked(layer->pixels, *layer->mask);
    } else {
        layer->render = nullptr;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                 "plugin %{public}s applied at (%{public}d,%{public}d) %{public}d x %{public}d "
                 "bands=%{public}zu %{public}lldms",
                 entry.man.id.c_str(), area.x0, area.y0, area.width(), area.height(),
                 bands.size(), static_cast<long long>(nowMs() - startedAt));
    return true;
}


// ---- 公开入口：JSVM 工作收敛到公司线程（模拟器 emutls 崩溃规避，见 PluginHost 注）----
void pluginSetHostExecutor(PluginHostExecutor executor) {
    PluginHost::get().setExecutor(std::move(executor));
}

bool pluginValidateSource(const std::string& source, Manifest& out, std::string& err) {
    return PluginHost::get().submit([&](std::string& e2) {
        if (!pluginValidateSourceOnHost(source, out, e2)) {
            throw std::runtime_error(e2);
        }
    }, err);
}

bool pluginRunFilter(Document& work, LayerId layerId, const PluginEntry& entry,
                     const std::vector<std::pair<std::string, PValue>>& params,
                     std::string& err) {
    return PluginHost::get().submit([&](std::string& e2) {
        if (!pluginRunFilterOnHost(work, layerId, entry, params, e2)) {
            throw std::runtime_error(e2);
        }
    }, err);
}

}  // namespace plugins
}  // namespace montage
