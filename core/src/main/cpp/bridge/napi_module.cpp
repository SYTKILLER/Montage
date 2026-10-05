#include <napi/native_api.h>

#include "bridge/engine_bridge.h"

namespace {

napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor props[] = {
        {"newDocument", nullptr, montage::bridge::NewDocument, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"closeDocument", nullptr, montage::bridge::CloseDocument, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createSurface", nullptr, montage::bridge::CreateSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroySurface", nullptr, montage::bridge::DestroySurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getStats", nullptr, montage::bridge::GetStats, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"onRevisionChanged", nullptr, montage::bridge::OnRevisionChanged, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runPixelTest", nullptr, montage::bridge::RunPixelTest, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"dispose", nullptr, montage::bridge::Dispose, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
    return exports;
}

static napi_module g_montageCompositorModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "compositor",
    .nm_priv = nullptr,
    .reserved = {0},
};

}  // namespace

// .so 被加载时静态注册 NAPI 模块（HAR native 打包验证项：注册与导入方式实测）。
extern "C" __attribute__((constructor)) void MontageCompositorModuleRegister(void) {
    napi_module_register(&g_montageCompositorModule);
}
