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
  docOpen: boolean;
  renderRunning: boolean;
  lastError: string;
  callbackThread: string;
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
