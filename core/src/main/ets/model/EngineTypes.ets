/**
 * 引擎 → ArkTS 的轻量 DTO 类型（01 文档 §6：位图永不过桥，只见数值/字符串/布尔）。
 * native 返回值一律经 CompositorEngine 内的 Record 收窄成本接口，禁止裸 ESObject 外泄。
 */

/** 引擎统计快照（getStats）。 */
export interface EngineStats {
  fps: number;
  frames: number;
  bufferWidth: number;
  bufferHeight: number;
  docRevision: number;
  docWidth: number;
  docHeight: number;
  zoom: number;
  panX: number;
  panY: number;
  importRunning: boolean;
  importProgress: number;
  renderRunning: boolean;
  lastError: string;
  canUndo: boolean;
  canRedo: boolean;
  quickMask: boolean;  // M10d：快速蒙版模式中（撤销跨 Q 边界后经 1s 轮询/docVersion 刷新镜像）
  historyVersion: number;
}

/** 快速蒙版 toggle 结果（toggleQuickMask 的 data，M10d）。 */
export interface QuickMaskState {
  active: boolean;        // true = 已进入快速蒙版模式
  hasSelection: boolean;  // toggle 后主选区是否存在（退出时通道内容非零）
}

/** 视口（pan = 视口左上角文档坐标，doc px）。 */
export interface ViewportData {
  zoom: number;
  panX: number;
  panY: number;
}

/** 导入完成结果（openFileFromFd 的 data，M2 起=新增图层）。 */
export interface OpenFileData {
  width: number;
  height: number;
  layerId: number;
}

/** 图层 DTO（02 §5；M2 无组，字段保真常量占位；M5a 起蒙版字段为真值）。 */
export interface LayerDTO {
  id: number;
  name: string;
  visible: boolean;
  opacity: number;
  blendMode: number;
  isActive: boolean;
  isGroup: boolean;
  parentId: number;
  collapsed: boolean;
  linkId: number;    // M14b 链接层（非零 = 同号层联动移动；会话态不持久化）
  hasStyle: boolean; // M14c 图层样式（投影/描边/颜色叠加任一）
  isFill: boolean;   // M14c 填充图层（纯色/渐变参数化内容）
  hasMask: boolean;
  maskEnabled: boolean;
  clipping: boolean;
  isAdjustment: boolean;
  adjBlack: number;
  adjWhite: number;
  adjGamma: number;
  adjKind: number;   // M9a：对齐 AdjustmentKind 枚举（0=Levels 1=Curves 2=BC 3=Invert 4=Threshold 5=Posterize 6=ColorBalance 7=HueSat）
  adjP0: number;
  adjP1: number;
  adjP2: number;
  adjP3: number;
  curveX: number[];  // Curves 控制点（0..1）
  curveY: number[];
  // M12 文本层字段（非文本层为常量缺省）
  isText: boolean;
  textContent: string;
  textFontFamily: string;
  textFontSize: number;
  textLineHeight: number;
  textTracking: number;
  textBold: boolean;
  textItalic: boolean;
  textColor: string;   // #RRGGBB
  textAlign: number;   // 0=左 1=中 2=右
  textWrapWidth: number;
  textX: number;       // 文本框左上角（文档域）
  textY: number;
  textW: number;       // 最近栅格化像素尺寸（空内容 = 0）
  textH: number;
  thumbToken: number;
}

/** PSD 导入结果（openPsdFile 的 data）。 */
export interface PsdOpenData {
  width: number;
  height: number;
  notes: string[];
}

/** 工程保存结果（saveProject 的 data，M4b）。 */
export interface SaveProjectData {
  layers: number;
  bytes: number;
}

/** 工程打开结果（openProject 的 data，M4c；M12 加 notes 文本层降级提示）。 */
export interface OpenProjectData {
  width: number;
  height: number;
  layers: number;
  notes: string[];
}

/** 缩略图结果（getLayerThumbnail 的 data；O3 跨桥实测）。 */
export interface LayerThumb {
  pixelMap: Object | null;
  ready: boolean;
}

/** 单个分配器探针结果（M0 像素实测）。 */
export interface PixelProbe {
  allocator: string;
  createOk: boolean;
  createRc: number;
  width: number;
  height: number;
  rowStride: number;
  pixelFormat: number;
  accessOk: boolean;
  accessRc: number;
  writeReadOk: boolean;
  error: string;
}

/** 像素测试结果（runPixelTest 的 data）。 */
export interface PixelTestData {
  dma: PixelProbe;
  shared: PixelProbe;
  /** ConvertPixelmapNativeToNapi 转换出的 PixelMap 样本（仅验证跨桥通路，M0 不解包使用）。 */
  samplePixelMap: Object | null;
}

/** 取色结果（samplePixel 的 data，M7.5 吸管；RGBA 0-255，合成结果采样）。 */
export interface SamplePixelData {
  ok: boolean;
  red: number;
  green: number;
  blue: number;
  alpha: number;
}

/** PNG 导出结果（exportPng 的 data，M7.5；PngRowEncoder 直写 fd）。 */
export interface ExportPngData {
  width: number;
  height: number;
  bytes: number;
}

/** 扁平化结果（flattenToPixelMap 的 data，M7.5 JPEG 导出；RGBA_8888 PixelMap）。 */
export interface FlattenData {
  width: number;
  height: number;
  pixelMap: Object | null;
}

/** 画布重设/选区平移等操作结果（M8a；changed/moved = 有实际变更）。 */
export interface OpResultData {
  changed: boolean;
  moved: boolean;
}

/** PSD 导出结果（exportPsd 的 data，M8b；notes = 跳过/降级提示）。 */
export interface ExportPsdData {
  layers: number;
  bytes: number;
  notes: string[];
}

/** 直方图（getHistogram 的 data，M9a；0..255 各 256 bin，透明区不计）。 */
export interface HistogramData {
  luma: number[];
  r: number[];
  g: number[];
  b: number[];
}

/** 变换 overlay 初始框（beginTransformOverlay 的 data，M8.1；doc 坐标）。 */
export interface TransformOverlayBox {
  left: number;
  top: number;
  width: number;
  height: number;
}
