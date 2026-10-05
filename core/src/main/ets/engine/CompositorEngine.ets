import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import native from 'libcompositor.so';
import { EngineStats, LayerDTO, LayerThumb, OpenFileData, PixelProbe, PixelTestData, PsdOpenData, SaveProjectData, ViewportData } from '../model/EngineTypes';

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

  beginStroke(x: number, y: number): void {
    native.beginStroke(x, y);
  }

  continueStroke(x: number, y: number): void {
    native.continueStroke(x, y);
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
