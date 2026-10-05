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

/** 图层 DTO（02 §5；M2 无组/蒙版，字段保真常量占位）。 */
export interface LayerDTO {
  id: number;
  name: string;
  visible: boolean;
  opacity: number;
  blendMode: number;
  isActive: boolean;
  isGroup: boolean;
  parentId: number;
  hasMask: boolean;
  thumbToken: number;
}

/** PSD 导入结果（openPsdFile 的 data）。 */
export interface PsdOpenData {
  width: number;
  height: number;
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
