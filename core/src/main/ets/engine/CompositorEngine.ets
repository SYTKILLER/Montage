import { hilog } from '@kit.PerformanceAnalysisKit';
import native from 'libcompositor.so';
import { EngineStats, PixelProbe, PixelTestData } from '../model/EngineTypes';

const DOMAIN: number = 0x4D30;
const TAG: string = 'Montage.Bridge';

type RevisionListener = (revision: number) => void;

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

function narrowStats(r: Record<string, Object>): EngineStats {
  return {
    fps: num(r, 'fps'),
    frames: num(r, 'frames'),
    bufferWidth: num(r, 'bufferWidth'),
    bufferHeight: num(r, 'bufferHeight'),
    docRevision: num(r, 'docRevision'),
    docOpen: boolOf(r, 'docOpen'),
    renderRunning: boolOf(r, 'renderRunning'),
    lastError: str(r, 'lastError'),
    callbackThread: str(r, 'callbackThread'),
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

/**
 * Compositor 引擎 ArkTS 门面：typed 命令面 + AppStorage 版本号事件面。
 * 页面侧通过 @StorageProp('docVersion') 建立刷新依赖（build 内必须真实读取）。
 */
export class CompositorEngine {
  private revisionBound: boolean = false;

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
        docOpen: false, renderRunning: false, lastError: 'getStats failed', callbackThread: '',
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
    hilog.info(DOMAIN, TAG, 'disposed');
  }
}
