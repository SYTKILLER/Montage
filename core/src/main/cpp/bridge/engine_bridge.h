#ifndef MONTAGE_BRIDGE_ENGINE_BRIDGE_H
#define MONTAGE_BRIDGE_ENGINE_BRIDGE_H

#include <napi/native_api.h>

// typed NAPI 命令面（01 文档 §6）：小参数同步命令 + Promise 异步 + 版本号事件。
// 禁止在此层之外再开泛型 dispatch；新增命令需在 01 文档登记（命令白名单制度）。
namespace montage {
namespace bridge {

// 会话
napi_value NewDocument(napi_env env, napi_callback_info info);
napi_value CloseDocument(napi_env env, napi_callback_info info);
// 渲染表面
napi_value CreateSurface(napi_env env, napi_callback_info info);
napi_value DestroySurface(napi_env env, napi_callback_info info);
// 状态
napi_value GetStats(napi_env env, napi_callback_info info);
// 事件面：版本号回调
napi_value OnRevisionChanged(napi_env env, napi_callback_info info);
// M0 实测：像素指针读写（Promise<{ok, data}>）
napi_value RunPixelTest(napi_env env, napi_callback_info info);
// 页面销毁时释放回调与渲染线程
napi_value Dispose(napi_env env, napi_callback_info info);

}  // namespace bridge
}  // namespace montage

#endif  // MONTAGE_BRIDGE_ENGINE_BRIDGE_H
