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
// 调整层（02 §3 M6a）：添加 Levels 调整层 / 设置参数
napi_value AddAdjustmentLayer(napi_env env, napi_callback_info info);
napi_value SetAdjustmentParams(napi_env env, napi_callback_info info);
// M9a：调整层 kind 切换 + 通用参数 / 直方图（4x256）
napi_value SetAdjustmentKind(napi_env env, napi_callback_info info);
napi_value GetHistogram(napi_env env, napi_callback_info info);
// M9b：曲线控制点设置
napi_value SetCurvePoints(napi_env env, napi_callback_info info);
// M9c：前景/背景色（fg 即笔刷色）
napi_value SetColors(napi_env env, napi_callback_info info);
// 选区（02 D2.3 M7a）：矩形选框（文档坐标）+ 取消选区
napi_value SetRectSelection(napi_env env, napi_callback_info info);
napi_value ClearSelection(napi_env env, napi_callback_info info);
// 魔棒（M7c）：点击容差选区
napi_value SetWandSelection(napi_env env, napi_callback_info info);
// M10a 选区增强：多边形套索（点列偶奇扫描线）/ 椭圆选框 / 全选 / 反选 / 重新选择。
// 布尔四态 op：0=Replace 1=Add 2=Subtract 3=Intersect（setRect/setWand 第 5 可选参同语义）。
napi_value SetPolygonSelection(napi_env env, napi_callback_info info);
napi_value SetEllipseSelection(napi_env env, napi_callback_info info);
napi_value SelectAll(napi_env env, napi_callback_info info);
napi_value InvertSelection(napi_env env, napi_callback_info info);
napi_value ReselectSelection(napi_env env, napi_callback_info info);
// M10b 修改族：羽化（半径=px，三盒高斯近似）/ 扩展 / 收缩 / 边界 / 平滑（Replace 语义）
napi_value FeatherSelection(napi_env env, napi_callback_info info);
napi_value ExpandSelection(napi_env env, napi_callback_info info);
napi_value ContractSelection(napi_env env, napi_callback_info info);
napi_value BorderSelection(napi_env env, napi_callback_info info);
napi_value SmoothSelection(napi_env env, napi_callback_info info);
// M10d 快速蒙版：toggle（选区 ↔ 临时通道；返回 {active, hasSelection}）
napi_value ToggleQuickMask(napi_env env, napi_callback_info info);
// M11a 绘图工具第一批：渐变（layerId,type,style,x0,y0,x1,y1,target）/ 填充（source）/
// 描边（width）；色源一律引擎 fg/bg 现值
napi_value ApplyGradient(napi_env env, napi_callback_info info);
napi_value FillSelection(napi_env env, napi_callback_info info);
napi_value StrokeSelection(napi_env env, napi_callback_info info);
// M11b 油漆桶：魔棒核命中域填充前景色（x,y,tol,contiguous,allLayers,sampleSize）
napi_value BucketFill(napi_env env, napi_callback_info info);
// M11c 形状栅格化（layerId,kind,x0,y0,x1,y1,cornerRadius,fill,stroke,strokeWidth；全 number）
napi_value RasterizeShape(napi_env env, napi_callback_info info);
// M11d 仿制图章：setCloneSource(x,y) Alt 取源 / setCloneAligned(aligned) 对齐开关 /
// setBrushClone(active) 图章工具激活标志
napi_value SetCloneSource(napi_env env, napi_callback_info info);
napi_value SetCloneAligned(napi_env env, napi_callback_info info);
napi_value SetBrushClone(napi_env env, napi_callback_info info);
// M11e dab 内核族：setBrushEffect(mode 0..6，对齐 BrushEffect 枚举)
napi_value SetBrushEffect(napi_env env, napi_callback_info info);
// M11f 历史画笔：setHistoryBrush(steps 0=关闭，1=最近一次编辑前快照…)
napi_value SetHistoryBrush(napi_env env, napi_callback_info info);
// M10c Alpha 通道：存储/载入/删除 + 列表（v1 通道面板只读）
napi_value StoreSelection(napi_env env, napi_callback_info info);
napi_value LoadSelection(napi_env env, napi_callback_info info);
napi_value DeleteChannel(napi_env env, napi_callback_info info);
napi_value GetChannelList(napi_env env, napi_callback_info info);
// M10c 剪贴板（引擎内存内部剪贴板）：拷贝/剪切（merged=合并拷贝）/粘贴/清除
napi_value CopySelection(napi_env env, napi_callback_info info);
napi_value CutSelection(napi_env env, napi_callback_info info);
napi_value PasteClipboard(napi_env env, napi_callback_info info);
napi_value DeleteSelectionPixels(napi_env env, napi_callback_info info);
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
// M7.5 导出管线：重命名（同步，不入撤销）/ 取色（合成结果 1×1）/ PNG 直写 fd / 扁平化 PixelMap
napi_value RenameLayer(napi_env env, napi_callback_info info);
napi_value SamplePixel(napi_env env, napi_callback_info info);
napi_value ExportPng(napi_env env, napi_callback_info info);
// M8b：PSD 导出（8-bit RGB，层/蒙版/剪贴/混合键/luni 中文名）
napi_value ExportPsd(napi_env env, napi_callback_info info);
napi_value FlattenToPixelMap(napi_env env, napi_callback_info info);
// M8a 无损文档操作：画布重设（裁剪/画布大小）/ 裁切 / 显示全部 / 90°旋转 / 翻转 /
// 移动图层（三段事务）/ 选区平移
napi_value ResizeCanvas(napi_env env, napi_callback_info info);
napi_value TrimCanvas(napi_env env, napi_callback_info info);
napi_value RevealAll(napi_env env, napi_callback_info info);
napi_value RotateDocument90(napi_env env, napi_callback_info info);
napi_value FlipDocument(napi_env env, napi_callback_info info);
napi_value TranslateLayer(napi_env env, napi_callback_info info);
napi_value TranslateSelection(napi_env env, napi_callback_info info);
// M8c：图像大小重采样 / 任意角旋转
napi_value ResampleDocument(napi_env env, napi_callback_info info);
napi_value RotateDocumentArbitrary(napi_env env, napi_callback_info info);
// M8d：自由变换烘焙（id, scaleX, scaleY, rotDeg, skewDeg, pivotX, pivotY）
napi_value BakeLayerTransform(napi_env env, napi_callback_info info);
// M8.1：overlay 进入（返回内容 bbox）/ 实时预览矩阵 / 矩阵烘焙 / 取消
napi_value BeginTransformOverlay(napi_env env, napi_callback_info info);
napi_value SetTransformPreview(napi_env env, napi_callback_info info);
napi_value BakeLayerMatrix(napi_env env, napi_callback_info info);
napi_value CancelTransformOverlay(napi_env env, napi_callback_info info);
// 页面销毁时释放回调与渲染线程
napi_value Dispose(napi_env env, napi_callback_info info);

}  // namespace bridge
}  // namespace montage

#endif  // MONTAGE_BRIDGE_ENGINE_BRIDGE_H
