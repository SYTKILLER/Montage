import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import native from 'libcompositor.so';
import { EngineStats, ExportPngData, HistogramData, ExportPsdData, FlattenData, LayerDTO, LayerThumb, OpenFileData, OpenProjectData, OpResultData, PixelProbe, PixelTestData, PsdOpenData, SamplePixelData, SaveProjectData, ViewportData } from '../model/EngineTypes';

const DOMAIN: number = 0x4D30;
const TAG: string = 'Montage.Bridge';

type RevisionListener = (revision: number) => void;
type ImportProgressListener = (progress: number) => void;

interface EngineError {
  code: number;
  message: string;
}

/** native Result 对象的 ArkTS 窄化形态（{ok,data}|{ok:false,error}，01 文档 §6）。 */
interface EngineResult {
  ok: boolean;
  data: Record<string, Object> | null;
  error: EngineError | null;
}

function toRecord(value: Object): Record<string, Object> {
  return value as Record<string, Object>;
}

function num(r: Record<string, Object>, key: string): number {
  const v: Object = r[key];
  return typeof v === 'number' ? v : 0;
}

function boolOf(r: Record<string, Object>, key: string): boolean {
  const v: Object = r[key];
  return typeof v === 'boolean' ? v : false;
}

function str(r: Record<string, Object>, key: string): string {
  const v: Object = r[key];
  return typeof v === 'string' ? v : '';
}

function narrowResult(value: Object): EngineResult {
  const r = toRecord(value);
  const ok = boolOf(r, 'ok');
  let data: Record<string, Object> | null = null;
  if (r['data'] !== undefined && r['data'] !== null) {
    data = toRecord(r['data']);
  }
  let error: EngineError | null = null;
  if (r['error'] !== undefined && r['error'] !== null) {
    const e = toRecord(r['error']);
    error = { code: num(e, 'code'), message: str(e, 'message') };
  }
  return { ok: ok, data: data, error: error };
}

function narrowViewport(r: Record<string, Object>): ViewportData {
  return { zoom: num(r, 'zoom'), panX: num(r, 'panX'), panY: num(r, 'panY') };
}

function narrowStats(r: Record<string, Object>): EngineStats {
  return {
    fps: num(r, 'fps'),
    frames: num(r, 'frames'),
    bufferWidth: num(r, 'bufferWidth'),
    bufferHeight: num(r, 'bufferHeight'),
    docRevision: num(r, 'docRevision'),
    docWidth: num(r, 'docWidth'),
    docHeight: num(r, 'docHeight'),
    zoom: num(r, 'zoom'),
    panX: num(r, 'panX'),
    panY: num(r, 'panY'),
    importRunning: boolOf(r, 'importRunning'),
    importProgress: num(r, 'importProgress'),
    renderRunning: boolOf(r, 'renderRunning'),
    lastError: str(r, 'lastError'),
    canUndo: boolOf(r, 'canUndo'),
    canRedo: boolOf(r, 'canRedo'),
    historyVersion: num(r, 'historyVersion'),
  };
}

function narrowProbe(r: Record<string, Object>): PixelProbe {
  return {
    allocator: str(r, 'allocator'),
    createOk: boolOf(r, 'createOk'),
    createRc: num(r, 'createRc'),
    width: num(r, 'width'),
    height: num(r, 'height'),
    rowStride: num(r, 'rowStride'),
    pixelFormat: num(r, 'pixelFormat'),
    accessOk: boolOf(r, 'accessOk'),
    accessRc: num(r, 'accessRc'),
    writeReadOk: boolOf(r, 'writeReadOk'),
    error: str(r, 'error'),
  };
}

function narrowPixelData(r: Record<string, Object>): PixelTestData {
  let sample: Object | null = null;
  if (r['samplePixelMap'] !== undefined && r['samplePixelMap'] !== null) {
    sample = r['samplePixelMap'];
  }
  return {
    dma: narrowProbe(toRecord(r['dma'])),
    shared: narrowProbe(toRecord(r['shared'])),
    samplePixelMap: sample,
  };
}

function narrowLayer(r: Record<string, Object>): LayerDTO {
  return {
    id: num(r, 'id'),
    name: str(r, 'name'),
    visible: boolOf(r, 'visible'),
    opacity: num(r, 'opacity'),
    blendMode: num(r, 'blendMode'),
    isActive: boolOf(r, 'isActive'),
    isGroup: boolOf(r, 'isGroup'),
    parentId: num(r, 'parentId'),
    hasMask: boolOf(r, 'hasMask'),
    maskEnabled: boolOf(r, 'maskEnabled'),
    clipping: boolOf(r, 'clipping'),
    isAdjustment: boolOf(r, 'isAdjustment'),
    adjBlack: num(r, 'adjBlack'),
    adjWhite: num(r, 'adjWhite'),
    adjGamma: num(r, 'adjGamma'),
    adjKind: num(r, 'adjKind'),
    adjP0: num(r, 'adjP0'),
    adjP1: num(r, 'adjP1'),
    adjP2: num(r, 'adjP2'),
    adjP3: num(r, 'adjP3'),
    thumbToken: num(r, 'thumbToken'),
  };
}

/**
 * Compositor 引擎 ArkTS 门面：typed 命令面 + AppStorage 事件面（docVersion/importProgress）。
 * 页面侧通过 @StorageProp 建立刷新依赖（build 内必须真实读取）。
 */
export class CompositorEngine {
  private revisionBound: boolean = false;
  private importBound: boolean = false;

  bindRevisionListener(listener?: RevisionListener): void {
    if (this.revisionBound) {
      return;
    }
    native.onRevisionChanged((revision: number): void => {
      AppStorage.setOrCreate<number>('docVersion', revision);
      if (listener !== undefined) {
        listener(revision);
      }
    });
    this.revisionBound = true;
    hilog.info(DOMAIN, TAG, 'revision listener bound');
  }

  bindImportListener(listener?: ImportProgressListener): void {
    if (this.importBound) {
      return;
    }
    native.onImportProgress((progress: number): void => {
      AppStorage.setOrCreate<number>('importProgress', progress);
      if (listener !== undefined) {
        listener(progress);
      }
    });
    this.importBound = true;
    hilog.info(DOMAIN, TAG, 'import progress listener bound');
  }

  newDocument(width: number, height: number): boolean {
    const r = narrowResult(native.newDocument(width, height) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'newDocument failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  closeDocument(): boolean {
    const r = narrowResult(native.closeDocument() as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'closeDocument failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  /**
   * 导入图片。fd 所有权一律移交 native（含失败路径），调用方不得再 close。
   */
  openFileFromFd(fd: number): Promise<OpenFileData> {
    return new Promise<OpenFileData>((resolve: (d: OpenFileData) => void,
      reject: (e: Error) => void) => {
      (native.openFileFromFd(fd) as Promise<Object>).then((value: Object): void => {
        const r = narrowResult(value);
        if (r.ok && r.data !== null) {
          resolve({
            width: num(r.data, 'width'),
            height: num(r.data, 'height'),
            layerId: num(r.data, 'layerId'),
          });
        } else {
          reject(new Error(r.error !== null ? r.error.message : 'openFileFromFd failed'));
        }
      }).catch((e: BusinessError): void => {
        reject(new Error(`openFileFromFd: ${e.message}`));
      });
    });
  }

  async openPsdFile(fd: number): Promise<PsdOpenData> {
    const value: Object = await (native.openPsdFile(fd) as Promise<Object>);
    const r = narrowResult(value);
    if (!r.ok || r.data === null) {
      throw new Error(r.error !== null ? r.error.message : 'openPsdFile failed');
    }
    const notes: string[] = [];
    const arr: Record<string, Object> = r.data['notes'] as Record<string, Object>;
    const len = Object.keys(arr).length;
    for (let i = 0; i < len; i++) {
      const item = arr[`${i}`];
      if (item !== undefined && item !== null && typeof item === 'string') {
        notes.push(item);
      }
    }
    return { width: num(r.data, 'width'), height: num(r.data, 'height'), notes: notes };
  }

  /**
   * 工程保存（M4b）。fd 所有权移交 native（成功/失败都关闭），调用方不得再 close。
   */
  async saveProject(fd: number): Promise<SaveProjectData> {
    const value: Object = await (native.saveProject(fd) as Promise<Object>);
    const r = narrowResult(value);
    if (!r.ok || r.data === null) {
      throw new Error(r.error !== null ? r.error.message : 'saveProject failed');
    }
    return { layers: num(r.data, 'layers'), bytes: num(r.data, 'bytes') };
  }

  /**
   * 工程打开（M4c）。fd 所有权移交 native（成功/失败都关闭），调用方不得再 close。
   */
  async openProject(fd: number): Promise<OpenProjectData> {
    const value: Object = await (native.openProject(fd) as Promise<Object>);
    const r = narrowResult(value);
    if (!r.ok || r.data === null) {
      throw new Error(r.error !== null ? r.error.message : 'openProject failed');
    }
    return {
      width: num(r.data, 'width'),
      height: num(r.data, 'height'),
      layers: num(r.data, 'layers'),
    };
  }

  setViewport(zoom: number, panX: number, panY: number): ViewportData | null {
    const r = narrowResult(native.setViewport(zoom, panX, panY) as Object);
    if (!r.ok || r.data === null) {
      return null;
    }
    return narrowViewport(r.data);
  }

  fitToWindow(canvasW: number, canvasH: number): ViewportData | null {
    const r = narrowResult(native.fitToWindow(canvasW, canvasH) as Object);
    if (!r.ok || r.data === null) {
      return null;
    }
    return narrowViewport(r.data);
  }

  // ---------- 图层（02 §5 M2 命令面） ----------

  getLayerListSnapshot(): LayerDTO[] {
    const r = narrowResult(native.getLayerListSnapshot() as Object);
    const out: LayerDTO[] = [];
    if (!r.ok || r.data === null) {
      return out;
    }
    // data 是数组：Record 收窄按索引遍历
    const arr: Record<string, Object> = r.data;
    const len = Object.keys(arr).length;
    for (let i = 0; i < len; i++) {
      const item = arr[`${i}`];
      if (item !== undefined && item !== null) {
        out.push(narrowLayer(toRecord(item)));
      }
    }
    return out;
  }

  addLayer(name?: string): number {
    const r = narrowResult((name !== undefined ? native.addLayer(name) : native.addLayer()) as Object);
    if (!r.ok || r.data === null) {
      hilog.error(DOMAIN, TAG, 'addLayer failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return 0;
    }
    return num(r.data, 'id');
  }

  removeLayer(id: number): boolean {
    const r = narrowResult(native.removeLayer(id) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'removeLayer failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  selectLayer(id: number): boolean {
    const r = narrowResult(native.selectLayer(id) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'selectLayer failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  setLayerVisible(id: number, v: boolean): boolean {
    const r = narrowResult(native.setLayerVisible(id, v) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setLayerVisible failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  setLayerOpacity(id: number, v: number): boolean {
    const r = narrowResult(native.setLayerOpacity(id, v) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setLayerOpacity failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  setLayerBlendMode(id: number, mode: number): boolean {
    const r = narrowResult(native.setLayerBlendMode(id, mode) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setLayerBlendMode failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M5a：蒙版启用/停用
  setLayerMaskEnabled(id: number, v: boolean): boolean {
    const r = narrowResult(native.setLayerMaskEnabled(id, v) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setLayerMaskEnabled failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M5b-2：创建/释放剪贴蒙版
  setLayerClipping(id: number, v: boolean): boolean {
    const r = narrowResult(native.setLayerClipping(id, v) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setLayerClipping failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M6a：添加 Levels 调整层，返回新图层 id（0 = 失败）
  addAdjustmentLayer(): number {
    const r = narrowResult(native.addAdjustmentLayer() as Object);
    if (!r.ok || r.data === null) {
      hilog.error(DOMAIN, TAG, 'addAdjustmentLayer failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return 0;
    }
    return num(r.data, 'id');
  }

  // M6a：设置调整层 Levels 参数（滑条拖动结束调用）
  setAdjustmentParams(id: number, inBlack: number, inWhite: number, gamma: number): boolean {
    const r = narrowResult(native.setAdjustmentParams(id, inBlack, inWhite, gamma) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setAdjustmentParams failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M7c：魔棒选区（活动图层采样）
  setWandSelection(x: number, y: number, tolerance: number, contiguous: boolean): boolean {
    const r = narrowResult(native.setWandSelection(x, y, tolerance, contiguous) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setWandSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M7a：矩形选框（文档坐标）/ 取消选区
  setRectSelection(x: number, y: number, w: number, h: number): boolean {
    const r = narrowResult(native.setRectSelection(x, y, w, h) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setRectSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  clearSelection(): boolean {
    const r = narrowResult(native.clearSelection() as Object);
    return r.ok;
  }

  reorderLayer(id: number, toIndex: number): boolean {
    const r = narrowResult(native.reorderLayer(id, toIndex) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'reorderLayer failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // ---------- 笔刷（M3）/ 撤销（M4a） ----------

  setBrushSettings(diameter: number, hardness: number, opacity: number, red: number,
    green: number, blue: number, erasing: boolean): void {
    native.setBrushSettings(diameter, hardness, opacity, red, green, blue, erasing);
  }

  // M5b：笔画落笔目标（true = 活动图层蒙版）
  setBrushTarget(mask: boolean): boolean {
    const r = narrowResult(native.setBrushTarget(mask) as Object);
    return r.ok;
  }

  // M5b：添加显示全部图层蒙版
  addLayerMask(id: number): boolean {
    const r = narrowResult(native.addLayerMask(id) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'addLayerMask failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M3.1：pressure ∈[0,1] 调制 dab 直径；缺省/无压感设备传 1
  beginStroke(x: number, y: number, pressure?: number): void {
    // native 命令是同步返回 Result 对象（非 Promise）；拒绝要留日志（验证期：静默拒绝=无声丢笔画）
    const v: Object = pressure !== undefined
      ? (native.beginStroke(x, y, pressure) as Object)
      : (native.beginStroke(x, y) as Object);
    const r = narrowResult(v);
    if (!r.ok && r.error !== null) {
      hilog.error(DOMAIN, TAG, 'beginStroke rejected: %{public}s', r.error.message);
    }
  }

  continueStroke(x: number, y: number, pressure?: number): void {
    if (pressure !== undefined) {
      native.continueStroke(x, y, pressure);
    } else {
      native.continueStroke(x, y);
    }
  }

  endStroke(): void {
    native.endStroke();
  }

  undo(): boolean {
    const r = narrowResult(native.undo() as Object);
    return r.ok && r.data !== null && boolOf(r.data, 'performed');
  }

  redo(): boolean {
    const r = narrowResult(native.redo() as Object);
    return r.ok && r.data !== null && boolOf(r.data, 'performed');
  }

  async getLayerThumbnail(id: number): Promise<LayerThumb> {
    const value: Object = await (native.getLayerThumbnail(id) as Promise<Object>);
    const r = narrowResult(value);
    if (!r.ok || r.data === null) {
      throw new Error(r.error !== null ? r.error.message : 'getLayerThumbnail failed');
    }
    const pm = r.data['pixelMap'];
    return {
      pixelMap: pm !== undefined && pm !== null ? pm : null,
      ready: boolOf(r.data, 'ready'),
    };
  }

  /** 图层重命名（M7.5 图层面板；直接改元数据，不入撤销）。 */
  renameLayer(id: number, name: string): boolean {
    const r = narrowResult(native.renameLayer(id, name) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'renameLayer failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  /** 画布重设（M8a：裁剪/画布大小；无损，选区清除）。 */
  resizeCanvas(x: number, y: number, w: number, h: number): boolean {
    const r = narrowResult(native.resizeCanvas(x, y, w, h) as Object);
    return r.ok && r.data !== null && boolOf(r.data, 'changed');
  }

  /** 裁切（M8a：可见内容包围盒收画布）。 */
  trimCanvas(): boolean {
    const r = narrowResult(native.trimCanvas() as Object);
    return r.ok && r.data !== null && boolOf(r.data, 'changed');
  }

  /** 显示全部（M8a：内容包围盒 ∪ 当前文档域）。 */
  revealAll(): boolean {
    const r = narrowResult(native.revealAll() as Object);
    return r.ok && r.data !== null && boolOf(r.data, 'changed');
  }

  /** 90° 旋转（M8a；clockwise=true 顺时针）。 */
  rotateDocument90(clockwise: boolean): boolean {
    const r = narrowResult(native.rotateDocument90(clockwise) as Object);
    return r.ok;
  }

  /** 翻转（M8a；horizontal=true 水平）。 */
  flipDocument(horizontal: boolean): boolean {
    const r = narrowResult(native.flipDocument(horizontal) as Object);
    return r.ok;
  }

  /** 移动图层（M8a 三段事务：phase 0=begin/1=move/2=end；拖拽实时渲染、撤销粒度=一次拖拽）。 */
  translateLayer(id: number, dx: number, dy: number, phase: number): boolean {
    const r = narrowResult(native.translateLayer(id, dx, dy, phase) as Object);
    if (!r.ok && r.error !== null) {
      hilog.error(DOMAIN, TAG, 'translateLayer rejected: %{public}s', r.error.message);
    }
    return r.ok;
  }

  /** 调整层 kind 切换 + 通用参数（M9a；kind 对齐 AdjustmentKind 枚举）。 */
  setAdjustmentKind(id: number, kind: number, p0: number, p1: number, p2: number,
                   p3: number): boolean {
    const r = narrowResult(native.setAdjustmentKind(id, kind, p0, p1, p2, p3) as Object);
    if (!r.ok && r.error !== null) {
      hilog.error(DOMAIN, TAG, 'setAdjustmentKind rejected: %{public}s', r.error.message);
    }
    return r.ok;
  }

  /** 直方图（M9a；全文档扁平化，透明区不计）。 */
  async getHistogram(): Promise<HistogramData> {
    const value: Object = await (native.getHistogram() as Promise<Object>);
    const r = narrowResult(value);
    if (!r.ok || r.data === null) {
      throw new Error(r.error !== null ? r.error.message : 'getHistogram failed');
    }
    const pick = (key: string): number[] => {
      const raw = r.data![key] as Array<Object>;
      const out: number[] = [];
      for (const item of raw) {
        out.push(typeof item === 'number' ? item : 0);
      }
      return out;
    };
    return { luma: pick('luma'), r: pick('r'), g: pick('g'), b: pick('b') };
  }

  /** 图像大小重采样（M8c；双线性）。 */
  resampleDocument(w: number, h: number): boolean {
    const r = narrowResult(native.resampleDocument(w, h) as Object);
    return r.ok && r.data !== null && boolOf(r.data, 'changed');
  }

  /** 自由变换烘焙（M8d；围绕枢轴 缩放/旋转/斜切，提交式）。 */
  bakeLayerTransform(id: number, scaleX: number, scaleY: number, rotDeg: number,
                     skewDeg: number, pivotX: number, pivotY: number): boolean {
    const r = narrowResult(native.bakeLayerTransform(id, scaleX, scaleY, rotDeg,
      skewDeg, pivotX, pivotY) as Object);
    if (!r.ok && r.error !== null) {
      hilog.error(DOMAIN, TAG, 'bakeLayerTransform rejected: %{public}s', r.error.message);
    }
    return r.ok && r.data !== null && boolOf(r.data, 'changed');
  }

  /** 任意角旋转（M8c；顺时针度）。 */
  rotateDocumentArbitrary(deg: number): boolean {
    const r = narrowResult(native.rotateDocumentArbitrary(deg) as Object);
    return r.ok;
  }

  /** 选区平移（M8a/M7d）。 */
  translateSelection(dx: number, dy: number): boolean {
    const r = narrowResult(native.translateSelection(dx, dy) as Object);
    return r.ok && r.data !== null && boolOf(r.data, 'moved');
  }

  /** 取色（M7.5 吸管；合成结果区域采样，异步扁平化单区域）。 */
  async samplePixel(x: number, y: number): Promise<SamplePixelData> {
    const value: Object = await (native.samplePixel(x, y) as Promise<Object>);
    const r = narrowResult(value);
    if (!r.ok || r.data === null) {
      throw new Error(r.error !== null ? r.error.message : 'samplePixel failed');
    }
    return {
      ok: boolOf(r.data, 'ok'),
      red: num(r.data, 'red'),
      green: num(r.data, 'green'),
      blue: num(r.data, 'blue'),
      alpha: num(r.data, 'alpha'),
    };
  }

  /** PNG 导出（M7.5：CPU 扁平化 → PngRowEncoder 流式直写 fd）。 */
  async exportPng(fd: number): Promise<ExportPngData> {
    const value: Object = await (native.exportPng(fd) as Promise<Object>);
    const r = narrowResult(value);
    if (!r.ok || r.data === null) {
      throw new Error(r.error !== null ? r.error.message : 'exportPng failed');
    }
    return {
      width: num(r.data, 'width'),
      height: num(r.data, 'height'),
      bytes: num(r.data, 'bytes'),
    };
  }

  /** PSD 导出（M8b：层/蒙版/剪贴/混合键/luni 中文名；调整层跳过+提示）。 */
  async exportPsd(fd: number): Promise<ExportPsdData> {
    const value: Object = await (native.exportPsd(fd) as Promise<Object>);
    const r = narrowResult(value);
    if (!r.ok || r.data === null) {
      throw new Error(r.error !== null ? r.error.message : 'exportPsd failed');
    }
    const notesRaw = r.data['notes'];
    const notes: string[] = [];
    if (notesRaw !== undefined && notesRaw !== null) {
      const arr = notesRaw as Array<Object>;
      for (const item of arr) {
        if (typeof item === 'string') {
          notes.push(item);
        }
      }
    }
    return {
      layers: num(r.data, 'layers'),
      bytes: num(r.data, 'bytes'),
      notes: notes,
    };
  }

  /** 扁平化为 PixelMap（M7.5：JPEG 导出走 ImagePacker；RGBA_8888 直 alpha）。 */
  async flattenToPixelMap(): Promise<FlattenData> {
    const value: Object = await (native.flattenToPixelMap() as Promise<Object>);
    const r = narrowResult(value);
    if (!r.ok || r.data === null) {
      throw new Error(r.error !== null ? r.error.message : 'flattenToPixelMap failed');
    }
    const pm = r.data['pixelMap'];
    return {
      width: num(r.data, 'width'),
      height: num(r.data, 'height'),
      pixelMap: pm !== undefined && pm !== null ? pm : null,
    };
  }

  createSurface(surfaceId: string): boolean {
    const r = narrowResult(native.createSurface(surfaceId) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'createSurface failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  destroySurface(): boolean {
    const r = narrowResult(native.destroySurface() as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'destroySurface failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  getStats(): EngineStats {
    const r = narrowResult(native.getStats() as Object);
    if (!r.ok || r.data === null) {
      hilog.error(DOMAIN, TAG, 'getStats failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return {
        fps: 0, frames: 0, bufferWidth: 0, bufferHeight: 0, docRevision: 0,
        docWidth: 0, docHeight: 0, zoom: 1, panX: 0, panY: 0,
        importRunning: false, importProgress: 0, renderRunning: false,
        lastError: 'getStats failed', canUndo: false, canRedo: false, historyVersion: 0,
      };
    }
    return narrowStats(r.data);
  }

  async runPixelTest(): Promise<PixelTestData> {
    const value: Object = await (native.runPixelTest() as Promise<Object>);
    const r = narrowResult(value);
    if (!r.ok || r.data === null) {
      const msg = r.error !== null ? r.error.message : 'unknown';
      hilog.error(DOMAIN, TAG, 'runPixelTest failed: %{public}s', msg);
      throw new Error(`runPixelTest failed: ${msg}`);
    }
    return narrowPixelData(r.data);
  }

  dispose(): void {
    native.dispose();
    this.revisionBound = false;
    this.importBound = false;
    hilog.info(DOMAIN, TAG, 'disposed');
  }
}
