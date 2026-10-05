#ifndef MONTAGE_BRIDGE_ENGINE_BRIDGE_H
#define MONTAGE_BRIDGE_ENGINE_BRIDGE_H

#include <napi/native_api.h>

// typed NAPI 命令面（01 文档 §6 + 02 §5 M1/M2 清单）：小参数同步命令 + Promise 异步 + 事件面。
// 禁止在此层之外再开泛型 dispatch；新增命令需在 01/02 文档登记（命令白名单制度）。
namespace montage {
namespace bridge {

// 会话
napi_value NewDocument(napi_env env, napi_callback_info info);
napi_value CloseDocument(napi_env env, napi_callback_info info);
// 导入（02 §6）：fd 所有权移交 native（含失败路径）
napi_value OpenFileFromFd(napi_env env, napi_callback_info info);
// 视口（02 §5；pan = 视口左上角文档坐标，canvasW/H = 画布组件 px 尺寸）
napi_value SetViewport(napi_env env, napi_callback_info info);
napi_value FitToWindow(napi_env env, napi_callback_info info);
// 渲染表面
napi_value CreateSurface(napi_env env, napi_callback_info info);
napi_value DestroySurface(napi_env env, napi_callback_info info);
// 状态
napi_value GetStats(napi_env env, napi_callback_info info);
// 事件面：docVersion / importProgress
napi_value OnRevisionChanged(napi_env env, napi_callback_info info);
napi_value OnImportProgress(napi_env env, napi_callback_info info);
// M0 实测遗留：像素指针读写（Promise<{ok, data}>）
napi_value RunPixelTest(napi_env env, napi_callback_info info);
// 页面销毁时释放回调与渲染线程
napi_value Dispose(napi_env env, napi_callback_info info);

}  // namespace bridge
}  // namespace montage

#endif  // MONTAGE_BRIDGE_ENGINE_BRIDGE_H
