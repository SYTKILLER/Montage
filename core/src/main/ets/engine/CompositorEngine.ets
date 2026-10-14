import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import native from 'libcompositor.so';
import { EngineStats, ExportPngData, HistogramData, QuickMaskState, TransformOverlayBox, ExportPsdData, FlattenData, LayerDTO, LayerThumb, OpenFileData, OpenProjectData, OpResultData, PixelProbe, PixelTestData, PsdOpenData, SamplePixelData, SaveProjectData, ViewportData, GuidesData, LayerBoundsData, HistoryListData } from '../model/EngineTypes';

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

function strArr(r: Record<string, Object>, key: string): string[] {
  const v: Object = r[key];
  const out: string[] = [];
  if (Array.isArray(v)) {
    for (const item of v as Object[]) {
      if (typeof item === 'string') {
        out.push(item);
      }
    }
  }
  return out;
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
    quickMask: boolOf(r, 'quickMask'),
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

function numArr(r: Record<string, Object>, key: string): number[] {
  const raw = r[key];
  const out: number[] = [];
  if (raw !== undefined && raw !== null) {
    const arr = raw as Array<Object>;
    for (const item of arr) {
      out.push(typeof item === 'number' ? item : 0);
    }
  }
  return out;
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
    collapsed: boolOf(r, 'collapsed'),
    linkId: num(r, 'linkId'),
    hasStyle: boolOf(r, 'hasStyle'),
    isFill: boolOf(r, 'isFill'),
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
    curveX: numArr(r, 'curveX'),
    curveY: numArr(r, 'curveY'),
    isText: boolOf(r, 'isText'),
    textContent: str(r, 'textContent'),
    textFontFamily: str(r, 'textFontFamily'),
    textFontSize: num(r, 'textFontSize'),
    textLineHeight: num(r, 'textLineHeight'),
    textTracking: num(r, 'textTracking'),
    textBold: boolOf(r, 'textBold'),
    textItalic: boolOf(r, 'textItalic'),
    textColor: str(r, 'textColor'),
    textAlign: num(r, 'textAlign'),
    textWrapWidth: num(r, 'textWrapWidth'),
    textX: num(r, 'textX'),
    textY: num(r, 'textY'),
    textW: num(r, 'textW'),
    textH: num(r, 'textH'),
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
      notes: strArr(r.data, 'notes'),
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
  // ---------- M14a 图层编组 ----------

  addGroup(): number {
    const r = narrowResult(native.addGroup() as Object);
    if (!r.ok || r.data === null) {
      hilog.error(DOMAIN, TAG, 'addGroup failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return 0;
    }
    return num(r.data, 'id');
  }

  groupLayer(id: number): number {
    const r = narrowResult(native.groupLayer(id) as Object);
    if (!r.ok || r.data === null) {
      hilog.error(DOMAIN, TAG, 'groupLayer failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return 0;
    }
    return num(r.data, 'id');
  }

  ungroupLayer(id: number): boolean {
    const r = narrowResult(native.ungroupLayer(id) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'ungroupLayer failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  setLayerParent(id: number, parent: number, index: number): boolean {
    const r = narrowResult(native.setLayerParent(id, parent, index) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setLayerParent failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  setGroupCollapsed(id: number, collapsed: boolean): boolean {
    const r = narrowResult(native.setGroupCollapsed(id, collapsed ? 1 : 0) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setGroupCollapsed failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  // ---------- M14b 合并族 / 复制 / 链接 / 对齐分布 ----------

  mergeDown(): boolean {
    const r = narrowResult(native.mergeDown() as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'mergeDown failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  mergeVisible(): boolean {
    const r = narrowResult(native.mergeVisible() as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'mergeVisible failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  flattenImage(): boolean {
    const r = narrowResult(native.flattenImage() as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'flattenImage failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  stampVisible(): number {
    const r = narrowResult(native.stampVisible() as Object);
    if (!r.ok || r.data === null) {
      hilog.error(DOMAIN, TAG, 'stampVisible failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return 0;
    }
    return num(r.data, 'id');
  }

  duplicateLayer(id: number): number {
    const r = narrowResult(native.duplicateLayer(id) as Object);
    if (!r.ok || r.data === null) {
      hilog.error(DOMAIN, TAG, 'duplicateLayer failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return 0;
    }
    return num(r.data, 'id');
  }

  setLayerLink(ids: number[], link: boolean): boolean {
    const r = narrowResult(native.setLayerLink(ids, link ? 1 : 0) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setLayerLink failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  alignLayers(ids: number[], mode: number): boolean {
    const r = narrowResult(native.alignLayers(ids, mode) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'alignLayers failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  // ---------- M14c 图层样式 / 填充图层 ----------

  setDropShadow(id: number, enabled: boolean, angle: number, dist: number, size: number,
    opacity: number, r: number, g: number, b: number): boolean {
    const r2 = narrowResult(native.setDropShadow(id, enabled ? 1 : 0, angle, dist, size, opacity,
      r, g, b) as Object);
    if (!r2.ok) {
      hilog.error(DOMAIN, TAG, 'setDropShadow failed: %{public}s',
        r2.error !== null ? r2.error.message : 'unknown');
      return false;
    }
    return true;
  }

  setLayerStroke(id: number, enabled: boolean, width: number, r: number, g: number,
    b: number): boolean {
    const r2 = narrowResult(native.setLayerStroke(id, enabled ? 1 : 0, width, r, g, b) as Object);
    if (!r2.ok) {
      hilog.error(DOMAIN, TAG, 'setLayerStroke failed: %{public}s',
        r2.error !== null ? r2.error.message : 'unknown');
      return false;
    }
    return true;
  }

  setColorOverlay(id: number, enabled: boolean, r: number, g: number, b: number,
    opacity: number): boolean {
    const r2 = narrowResult(native.setColorOverlay(id, enabled ? 1 : 0, r, g, b, opacity) as Object);
    if (!r2.ok) {
      hilog.error(DOMAIN, TAG, 'setColorOverlay failed: %{public}s',
        r2.error !== null ? r2.error.message : 'unknown');
      return false;
    }
    return true;
  }

  clearLayerStyle(id: number): boolean {
    const r2 = narrowResult(native.clearLayerStyle(id) as Object);
    if (!r2.ok) {
      hilog.error(DOMAIN, TAG, 'clearLayerStyle failed: %{public}s',
        r2.error !== null ? r2.error.message : 'unknown');
      return false;
    }
    return true;
  }

  addFillLayer(kind: number, r: number, g: number, b: number, angle: number): number {
    const r2 = narrowResult(native.addFillLayer(kind, r, g, b, angle) as Object);
    if (!r2.ok || r2.data === null) {
      hilog.error(DOMAIN, TAG, 'addFillLayer failed: %{public}s',
        r2.error !== null ? r2.error.message : 'unknown');
      return 0;
    }
    return num(r2.data, 'id');
  }

  setFillLayerParams(id: number, kind: number, r: number, g: number, b: number,
    angle: number): boolean {
    const r2 = narrowResult(native.setFillLayerParams(id, kind, r, g, b, angle) as Object);
    if (!r2.ok) {
      hilog.error(DOMAIN, TAG, 'setFillLayerParams failed: %{public}s',
        r2.error !== null ? r2.error.message : 'unknown');
      return false;
    }
    return true;
  }

  distributeLayers(ids: number[], mode: number): boolean {
    const r = narrowResult(native.distributeLayers(ids, mode) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'distributeLayers failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  // ---------- M13 滤镜 ----------

  applyFilter(layerId: number, kind: number, p0: number, p1: number, p2: number,
    p3: number, p4: number, p5: number, seed: number): boolean {
    const r = narrowResult(native.applyFilter(layerId, kind, p0, p1, p2, p3, p4, p5, seed) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'applyFilter failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  beginFilterPreview(layerId: number): boolean {
    const r = narrowResult(native.beginFilterPreview(layerId) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'beginFilterPreview failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  setFilterPreview(kind: number, p0: number, p1: number, p2: number, p3: number,
    p4: number, p5: number, seed: number): boolean {
    const r = narrowResult(native.setFilterPreview(kind, p0, p1, p2, p3, p4, p5, seed) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setFilterPreview failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  endFilterPreview(commit: boolean, kind: number, p0: number, p1: number, p2: number,
    p3: number, p4: number, p5: number, seed: number): void {
    const r = narrowResult(native.endFilterPreview(commit ? 1 : 0, kind, p0, p1, p2, p3, p4, p5,
      seed) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'endFilterPreview failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
  }

  // ---------- M12 文字层 ----------

  createTextLayer(x: number, y: number, wrapW: number): number {
    const r = narrowResult(native.createTextLayer(x, y, wrapW) as Object);
    if (!r.ok || r.data === null) {
      hilog.error(DOMAIN, TAG, 'createTextLayer failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return 0;
    }
    return num(r.data, 'id');
  }

  setTextLayerContent(id: number, content: string, txn: boolean): boolean {
    const r = narrowResult(native.setTextLayerContent(id, content, txn ? 1 : 0) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setTextLayerContent failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  setTextLayerStyle(id: number, family: string, fontSize: number, lineHeight: number,
    tracking: number, bold: boolean, italic: boolean, colorR: number, colorG: number,
    colorB: number, align: number, wrapWidth: number, txn: boolean): boolean {
    const r = narrowResult(native.setTextLayerStyle(id, family, fontSize, lineHeight, tracking,
      bold, italic, colorR, colorG, colorB, align, wrapWidth, txn ? 1 : 0) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setTextLayerStyle failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  rasterizeTextLayer(id: number): boolean {
    const r = narrowResult(native.rasterizeTextLayer(id) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'rasterizeTextLayer failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  beginTextSession(id: number): boolean {
    const r = narrowResult(native.beginTextSession(id) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'beginTextSession failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  endTextSession(): void {
    const r = narrowResult(native.endTextSession() as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'endTextSession failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
  }

  listSystemFonts(): string[] {
    try {
      const v: Object = native.listSystemFonts() as Object;
      const out: string[] = [];
      if (Array.isArray(v)) {
        for (const item of v as Object[]) {
          if (typeof item === 'string') {
            out.push(item);
          }
        }
      }
      return out;
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'listSystemFonts failed: %{public}s',
        e instanceof Error ? e.message : String(e));
      return [];
    }
  }

  registerFontFile(alias: string, path: string): boolean {
    const r = narrowResult(native.registerFontFile(alias, path) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'registerFontFile failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return false;
    }
    return true;
  }

  addAdjustmentLayer(kind?: number): number {
    const r = narrowResult(kind === undefined
      ? (native.addAdjustmentLayer() as Object)
      : (native.addAdjustmentLayer(kind) as Object));
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

  // M7c：魔棒选区；op 布尔四态（M10a）；allLayers 0/1=所有图层采样、sampleSize=N×N 均值种子（M10d）。
  // allLayers 用 number（0/1，napi_get_value_bool 不收 number）。
  setWandSelection(x: number, y: number, tolerance: number, contiguous: boolean,
    op?: number, allLayers?: number, sampleSize?: number): boolean {
    // 全 number 化参数（contiguous 0/1）做 no-structural-typing 二分
    // 统一 7 参调用（op 缺省 0、allLayers 缺省 0、sampleSize 缺省 1；native 侧可变 argc）
    // 注意：变量类型必须 EngineResult（narrowResult 返回类型；曾误注 OpResultData 触发
    // no-structural-typing——与参数个数/类型无关，纯注解笔误）
    let r: EngineResult;
    const v: Object = native.setWandSelection(x, y, tolerance, contiguous,
      op ?? 0, allLayers ?? 0, sampleSize ?? 1) as Object;
    r = narrowResult(v);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setWandSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M7a：矩形选框（文档坐标）；op 可选布尔四态（M10a）
  setRectSelection(x: number, y: number, w: number, h: number, op?: number): boolean {
    const r = narrowResult(op === undefined
      ? native.setRectSelection(x, y, w, h) as Object
      : native.setRectSelection(x, y, w, h, op) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setRectSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M10a：多边形套索（点列扁平 [x0,y0,...] 文档域；偶奇扫描线）
  setPolygonSelection(points: number[], op?: number): boolean {
    const r = narrowResult(op === undefined
      ? native.setPolygonSelection(points) as Object
      : native.setPolygonSelection(points, op) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setPolygonSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M10a：椭圆选框（内切矩形）
  setEllipseSelection(x: number, y: number, w: number, h: number, op?: number): boolean {
    const r = narrowResult(op === undefined
      ? native.setEllipseSelection(x, y, w, h) as Object
      : native.setEllipseSelection(x, y, w, h, op) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setEllipseSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M10a：全选 / 反选 / 重新选择
  selectAll(): boolean {
    const r = narrowResult(native.selectAll() as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'selectAll failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  invertSelection(): boolean {
    const r = narrowResult(native.invertSelection() as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'invertSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  reselectSelection(): boolean {
    const r = narrowResult(native.reselectSelection() as Object);
    return r.ok;
  }

  // M10b 修改族：羽化（三盒高斯近似）/ 扩展 / 收缩 / 边界 / 平滑（Replace 语义）
  featherSelection(radius: number): boolean {
    const r = narrowResult(native.featherSelection(radius) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'featherSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  expandSelection(radius: number): boolean {
    const r = narrowResult(native.expandSelection(radius) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'expandSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  contractSelection(radius: number): boolean {
    const r = narrowResult(native.contractSelection(radius) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'contractSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  borderSelection(radius: number): boolean {
    const r = narrowResult(native.borderSelection(radius) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'borderSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  smoothSelection(): boolean {
    const r = narrowResult(native.smoothSelection() as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'smoothSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M10d 快速蒙版 toggle（PS Q 键）；失败返回 null
  toggleQuickMask(): QuickMaskState | null {
    // 变量类型必须 EngineResult（no-structural-typing 注解纪律，同 setWandSelection）
    let r: EngineResult;
    const v: Object = native.toggleQuickMask() as Object;
    r = narrowResult(v);
    if (!r.ok || r.data === null) {
      hilog.error(DOMAIN, TAG, 'toggleQuickMask failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
      return null;
    }
    return { active: boolOf(r.data, 'active'), hasSelection: boolOf(r.data, 'hasSelection') };
  }

  // M11a 渐变（type 0=线性/1=径向；style 0=前景→背景/1=前景→透明；
  // target 0=像素/1=图层蒙版；拖线文档域）
  applyGradient(layerId: number, type: number, style: number, x0: number, y0: number,
    x1: number, y1: number, target: number): boolean {
    let r: EngineResult;
    const v: Object = native.applyGradient(layerId, type, style, x0, y0, x1, y1,
      target) as Object;
    r = narrowResult(v);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'applyGradient rejected: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M11a 填充（source 0=前景/1=背景/2=黑/3=白；色源引擎 fg/bg 现值）
  fillSelection(source: number): boolean {
    const r = narrowResult(native.fillSelection(source) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'fillSelection rejected: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M11a 描边（width 1..100 中心位置；前景色）
  strokeSelection(width: number): boolean {
    const r = narrowResult(native.strokeSelection(width) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'strokeSelection rejected: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M11b 油漆桶（魔棒核命中域填充前景色；allLayers 0/1 number 化同 setWandSelection）
  bucketFill(x: number, y: number, tolerance: number, contiguous: boolean,
    allLayers?: number, sampleSize?: number): boolean {
    let r: EngineResult;
    const v: Object = native.bucketFill(x, y, tolerance, contiguous, allLayers ?? 0,
      sampleSize ?? 1) as Object;
    r = narrowResult(v);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'bucketFill rejected: %{public}s', r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M11d 仿制图章：Alt 取源 / 对齐开关 / 工具激活标志
  setCloneSource(x: number, y: number): boolean {
    const r = narrowResult(native.setCloneSource(x, y) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setCloneSource rejected: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  setCloneAligned(aligned: boolean): boolean {
    const r = narrowResult(native.setCloneAligned(aligned) as Object);
    return r.ok;
  }

  setBrushClone(active: boolean): boolean {
    const r = narrowResult(native.setBrushClone(active) as Object);
    return r.ok;
  }

  // M11e dab 内核族：mode 对齐 BrushEffect（0=None 1=Blur 2=Sharpen 3=Smudge 4=Dodge 5=Burn 6=Sponge）
  setBrushEffect(mode: number): boolean {
    const r = narrowResult(native.setBrushEffect(mode) as Object);
    return r.ok;
  }

  // M11f 历史画笔：steps 0=关闭，1=最近一次编辑前快照…
  setHistoryBrush(steps: number): boolean {
    const r = narrowResult(native.setHistoryBrush(steps) as Object);
    return r.ok;
  }

  // M11c 形状栅格化（kind 0=矩形/1=圆角/2=椭圆/3=直线；fill/stroke 0/1 number；前景填充/背景描边）
  rasterizeShape(kind: number, x0: number, y0: number, x1: number, y1: number,
    cornerRadius: number, fill: boolean, stroke: boolean, strokeWidth: number): boolean {
    let r: EngineResult;
    const v: Object = native.rasterizeShape(0, kind, x0, y0, x1, y1, cornerRadius,
      fill ? 1 : 0, stroke ? 1 : 0, strokeWidth) as Object;
    r = narrowResult(v);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'rasterizeShape rejected: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  // M10c Alpha 通道：存储选区（同名覆盖）/ 载入（op 布尔四态）/ 删除 / 列表
  storeSelection(name: string): boolean {
    const r = narrowResult(native.storeSelection(name) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'storeSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  loadSelection(name: string, op?: number): boolean {
    const r = narrowResult(op === undefined
      ? native.loadSelection(name) as Object
      : native.loadSelection(name, op) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'loadSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  deleteChannel(name: string): boolean {
    const r = narrowResult(native.deleteChannel(name) as Object);
    return r.ok;
  }

  async getChannelList(): Promise<string[]> {
    const value = native.getChannelList() as string[];
    const out: string[] = [];
    if (value !== null && value !== undefined) {
      for (const v of value) {
        out.push(v);
      }
    }
    return out;
  }

  // M10c 剪贴板：拷贝/剪切（merged=合并拷贝）/粘贴/清除选区像素
  copySelection(merged: boolean): boolean {
    const r = narrowResult(native.copySelection(merged) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'copySelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  cutSelection(merged: boolean): boolean {
    const r = narrowResult(native.cutSelection(merged) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'cutSelection failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  pasteClipboard(): boolean {
    const r = narrowResult(native.pasteClipboard() as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'pasteClipboard failed: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  deleteSelectionPixels(): boolean {
    const r = narrowResult(native.deleteSelectionPixels() as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'deleteSelectionPixels failed: %{public}s',
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

  /** 前景/背景色（M9c；0..1；fg 即笔刷色）。 */
  setColors(fgR: number, fgG: number, fgB: number, bgR: number, bgG: number,
            bgB: number): boolean {
    const r = narrowResult(native.setColors(fgR, fgG, fgB, bgR, bgG, bgB) as Object);
    return r.ok;
  }

  /** 曲线控制点设置（M9b；xs/ys 0..1 归一）。 */
  setCurvePoints(id: number, xs: number[], ys: number[]): boolean {
    const r = narrowResult(native.setCurvePoints(id, xs, ys) as Object);
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

  /** 变换 overlay bbox（M8.1；进入 overlay 态）。 */
  beginTransformOverlay(): TransformOverlayBox | null {
    const r = narrowResult(native.beginTransformOverlay() as Object);
    if (!r.ok || r.data === null) {
      return null;
    }
    return {
      left: num(r.data, 'left'), top: num(r.data, 'top'),
      width: num(r.data, 'width'), height: num(r.data, 'height'),
    };
  }

  /** 拖拽实时预览矩阵（M8.1）。 */
  setTransformPreview(active: boolean, m: number[]): void {
    if (active && m.length >= 6) {
      native.setTransformPreview(active, m[0], m[1], m[2], m[3], m[4], m[5]);
    } else {
      native.setTransformPreview(active);
    }
  }

  /** 矩阵烘焙（M8.1；overlay 确认；异步 warp 防主线程 freeze）。 */
  async bakeLayerMatrix(id: number, m: number[]): Promise<boolean> {
    const value: Object = await (native.bakeLayerMatrix(id, m[0], m[1], m[2], m[3], m[4], m[5]) as Promise<Object>);
    const r = narrowResult(value);
    return r.ok && r.data !== null && boolOf(r.data, 'changed');
  }

  /** 取消 overlay（M8.1）。 */
  cancelTransformOverlay(): void {
    native.cancelTransformOverlay();
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
        lastError: 'getStats failed', canUndo: false, canRedo: false, quickMask: false,
        historyVersion: 0,
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


  // ---------- M15a 视图：参考线 + 图层包围盒 ----------

  addGuide(orient: number, pos: number): boolean {
    const r = narrowResult(native.addGuide(orient, pos) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'addGuide rejected: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  moveGuide(orient: number, index: number, pos: number): boolean {
    const r = narrowResult(native.moveGuide(orient, index, pos) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'moveGuide rejected: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  removeGuide(orient: number, index: number): boolean {
    const r = narrowResult(native.removeGuide(orient, index) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'removeGuide rejected: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  clearGuides(): boolean {
    const r = narrowResult(native.clearGuides() as Object);
    return r.ok;
  }

  setGuidesLocked(locked: boolean): boolean {
    const r = narrowResult(native.setGuidesLocked(locked ? 1 : 0) as Object);
    return r.ok;
  }

  setGuidesVisible(visible: boolean): boolean {
    const r = narrowResult(native.setGuidesVisible(visible ? 1 : 0) as Object);
    return r.ok;
  }

  getGuides(): GuidesData {
    const r = narrowResult(native.getGuides() as Object);
    if (!r.ok || r.data === null) {
      return { h: [], v: [], locked: false, visible: true };
    }
    const d = r.data;
    const hv: Object = d['h'];
    const vv: Object = d['v'];
    const h: number[] = [];
    const v: number[] = [];
    if (Array.isArray(hv)) {
      for (const item of hv as Object[]) {
        if (typeof item === 'number') {
          h.push(item);
        }
      }
    }
    if (Array.isArray(vv)) {
      for (const item of vv as Object[]) {
        if (typeof item === 'number') {
          v.push(item);
        }
      }
    }
    return { h: h, v: v, locked: boolOf(d, 'locked'), visible: boolOf(d, 'visible') };
  }

  // 精确内容包围盒（文档域 px）；id=0 = 活动层。失败（组/调整层/空层）ok=false。
  getLayerBounds(id: number): LayerBoundsData {
    const r = narrowResult(native.getLayerBounds(id) as Object);
    if (!r.ok || r.data === null) {
      return { ok: false, x: 0, y: 0, w: 0, h: 0 };
    }
    const d = r.data;
    return { ok: boolOf(d, 'ok'), x: num(d, 'x'), y: num(d, 'y'), w: num(d, 'w'), h: num(d, 'h') };
  }

  // ---------- M15b 面板：历史列表 / 历史跳转 / 选区包围盒 ----------

  getHistoryList(): HistoryListData {
    const r = narrowResult(native.getHistoryList() as Object);
    if (!r.ok || r.data === null) {
      return { labels: [''], current: 0 };
    }
    const d = r.data;
    const raw: Object = d['labels'];
    const labels: string[] = [];
    if (Array.isArray(raw)) {
      for (const item of raw as Object[]) {
        labels.push(typeof item === 'string' ? item : '');
      }
    }
    return { labels: labels, current: num(d, 'current') };
  }

  setHistoryState(index: number): boolean {
    const r = narrowResult(native.setHistoryState(index) as Object);
    if (!r.ok) {
      hilog.error(DOMAIN, TAG, 'setHistoryState rejected: %{public}s',
        r.error !== null ? r.error.message : 'unknown');
    }
    return r.ok;
  }

  getSelectionBounds(): LayerBoundsData {
    const r = narrowResult(native.getSelectionBounds() as Object);
    if (!r.ok || r.data === null) {
      return { ok: false, x: 0, y: 0, w: 0, h: 0 };
    }
    const d = r.data;
    return { ok: boolOf(d, 'ok'), x: num(d, 'x'), y: num(d, 'y'), w: num(d, 'w'), h: num(d, 'h') };
  }

  dispose(): void {
    native.dispose();
    this.revisionBound = false;
    this.importBound = false;
    hilog.info(DOMAIN, TAG, 'disposed');
  }
}
