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
// PSD 导入（04 §1.1 PSD-1）：解析→烘焙→整档替换文档，notes 返回降级提示
napi_value OpenPsdFile(napi_env env, napi_callback_info info);
// 工程保存（04 §1.2 M4b）：.montage zip 包流式写出；fd 所有权移交 native（含失败路径）
napi_value SaveProject(napi_env env, napi_callback_info info);
// 工程打开（04 §1.2 M4c）：zip 解析→manifest 校验→逐层解码→整档替换；fd 所有权移交 native
napi_value OpenProject(napi_env env, napi_callback_info info);
// 视口（02 §5；pan = 视口左上角文档坐标，canvasW/H = 画布组件 px 尺寸）
napi_value SetViewport(napi_env env, napi_callback_info info);
napi_value FitToWindow(napi_env env, napi_callback_info info);
// 图层（02 §5 M2 命令面；LayerDTO 见 02 §5，缩略图 = O3 跨桥实测）
napi_value AddLayer(napi_env env, napi_callback_info info);
napi_value RemoveLayer(napi_env env, napi_callback_info info);
napi_value SelectLayer(napi_env env, napi_callback_info info);
napi_value SetLayerVisible(napi_env env, napi_callback_info info);
// 蒙版（04 §1.2 / 02 §3 M5a）：启用/停用图层蒙版
napi_value SetLayerMaskEnabled(napi_env env, napi_callback_info info);
// 剪贴蒙版（M5b-2）：创建/释放剪贴（剪贴到下方最近非剪贴层）
napi_value SetLayerClipping(napi_env env, napi_callback_info info);
napi_value SetLayerOpacity(napi_env env, napi_callback_info info);
napi_value SetLayerBlendMode(napi_env env, napi_callback_info info);
napi_value ReorderLayer(napi_env env, napi_callback_info info);
napi_value GetLayerListSnapshot(napi_env env, napi_callback_info info);
napi_value GetLayerThumbnail(napi_env env, napi_callback_info info);
// 渲染表面
napi_value CreateSurface(napi_env env, napi_callback_info info);
napi_value DestroySurface(napi_env env, napi_callback_info info);
// 状态
napi_value GetStats(napi_env env, napi_callback_info info);
// 事件面：docVersion / importProgress
napi_value OnRevisionChanged(napi_env env, napi_callback_info info);
napi_value OnImportProgress(napi_env env, napi_callback_info info);
// 笔刷（01 §6 命令面：M3）
napi_value SetBrushSettings(napi_env env, napi_callback_info info);
// 笔画落笔目标（M5b）：true = 活动图层蒙版
napi_value SetBrushTarget(napi_env env, napi_callback_info info);
// 添加图层蒙版（M5b：显示全部，全 255 patch）
napi_value AddLayerMask(napi_env env, napi_callback_info info);
napi_value BeginStroke(napi_env env, napi_callback_info info);
napi_value ContinueStroke(napi_env env, napi_callback_info info);
napi_value EndStroke(napi_env env, napi_callback_info info);
// 撤销（02 §4 / 01 §6：undo/redo）
napi_value Undo(napi_env env, napi_callback_info info);
napi_value Redo(napi_env env, napi_callback_info info);
// M0 实测遗留：像素指针读写（Promise<{ok, data}>）
napi_value RunPixelTest(napi_env env, napi_callback_info info);
// 页面销毁时释放回调与渲染线程
napi_value Dispose(napi_env env, napi_callback_info info);

}  // namespace bridge
}  // namespace montage

#endif  // MONTAGE_BRIDGE_ENGINE_BRIDGE_H
