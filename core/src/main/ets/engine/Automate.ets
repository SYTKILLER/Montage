/**
 * M24b 批处理 + Image Processor（photocraft file_cmds.rs batch/imageProcessor 语义对拍）。
 * - 输入 = 目录（fs.listFile 按扩展名过滤）或文件路径数组；
 * - 步骤 = 动作名或内联 [[id,params]…]；过滤 file.save/saveAs/view.*（shell_save/view 同义）；
 *   未知命令整批拒绝（automation.hasCommand 预检）；
 * - 逐文件 scratch 语义：openFileFromFd 整档替换 → automation.execute 逐步 → exportImage 写
 *   输出；每文件错误收集不中断（photocraft process_files 同义）；
 * - 输出命名 <stem>.<ext>（format=same 跟随输入）；OutputClaims（lowercase）检出同名覆盖；
 * - P4 D2 scratch 会话化：批处理在新会话标签内逐文件「新会话→打开→执行→导出→关闭」，
 *   原文档全程不动——撤销历史天然保留（旧 saveProject/openProject 恢复路径删除，其
 *   恢复恒丢 undo 栈即本项登记的根因）；全程 automation.suspendRecording；
 * - 异步纪律：整批 async 循环（每文件 await 让出 JS 线程），进度回调 + cancel 标志。
 */
import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { automation, ActionStep } from './Automation';
import { CompositorEngine } from './CompositorEngine';

const TAG: string = 'Montage.Automate';
const DOMAIN: number = 0x4D30;

/** 支持的批处理输入扩展名（引擎可打开的平面格式 + 工程/PSD）。 */
const INPUT_EXTS: string[] = ['png', 'jpg', 'jpeg', 'webp', 'bmp', 'gif', 'tiff', 'tif',
  'qoi', 'tga', 'psd', 'montage'];

/** 输入扩展名 → exportImage format（M25 codec 面；'same' 时跟随）。 */
function extToFormat(ext: string): string {
  if (ext === 'jpg') {
    return 'jpeg';
  }
  if (ext === 'tif') {
    return 'tiff';
  }
  return ext;
}

export interface BatchFileError {
  file: string;
  error: string;
}

export interface BatchResult {
  files: string[];
  errors: BatchFileError[];
}

export interface BatchOptions {
  inputDir: string;         // 目录输入（空则用 files）
  files: string[];          // 文件数组输入
  output: string;           // 输出目录
  actionName: string;       // 动作名（优先；空则用 steps）
  steps: ActionStep[];      // 内联步骤
  format: string;           // 'same' | png/jpeg/psd/tiff/...
  quality: number;          // 0 = 默认
}

export interface ProcessorOptions {
  inputDir: string;
  files: string[];
  output: string;
  format: string;           // 默认 jpeg
  width: number;            // 0 = 不限
  height: number;
  dontEnlarge: boolean;
  quality: number;          // 0 = 默认 85
  convertToSrgb: boolean;   // v1 接受但 no-op（文档像素恒 8-bit sRGB，登记）
}

/** OutputClaims（photocraft #420/#422）：同名输出已在本轮写过 → 报错而非静默覆盖。 */
class OutputClaims {
  private map: Map<string, string> = new Map();

  check(out: string): string {
    const key = out.toLowerCase();
    const first = this.map.get(key);
    if (first !== undefined) {
      return `未写出：${out} 本轮已被 ${first} 的结果占用（输出同名）`;
    }
    return '';
  }

  record(out: string, input: string): void {
    this.map.set(out.toLowerCase(), input);
  }
}

function stem(path: string): string {
  const base = path.substring(path.lastIndexOf('/') + 1);
  const dot = base.lastIndexOf('.');
  return dot > 0 ? base.substring(0, dot) : base;
}

function extOf(path: string): string {
  const dot = path.lastIndexOf('.');
  return dot >= 0 ? path.substring(dot + 1).toLowerCase() : '';
}

function joinDir(dir: string, name: string): string {
  if (dir.endsWith('/')) {
    return dir + name;
  }
  return dir + '/' + name;
}

/**
 * 批处理驱动。engine 经构造注入（Index 持有）；tempProjectPath = cacheDir 临时包路径
 * （原文档保护）；cancelFlag 每文件检查。
 */
export class Automate {
  private engine: CompositorEngine;
  private cancelFlag: () => boolean;
  private tempProjectPath: string;  // P4 后不再使用（scratch 会话替代存/恢复包；字段留 API 稳定）

  constructor(engine: CompositorEngine, cancelFlag: () => boolean, tempProjectPath: string) {
    this.engine = engine;
    this.cancelFlag = cancelFlag;
    this.tempProjectPath = tempProjectPath;
  }

  /** 目录输入枚举（fs.listFile + 扩展名过滤，按名排序确定性）。 */
  static listInputs(dir: string): string[] {
    const names: string[] = fs.listFileSync(dir);
    const out: string[] = [];
    for (const n of names) {
      if (INPUT_EXTS.includes(extOf(n))) {
        out.push(joinDir(dir, n));
      }
    }
    out.sort();
    return out;
  }

  /** 步骤预检（photocraft batch：未知命令整批拒绝）。返回错误串，空 = 通过。 */
  static checkSteps(actionName: string, inline: ActionStep[]): string {
    let steps: ActionStep[] = inline;
    if (actionName.length > 0) {
      const g = automation.getAction({ 'action': actionName });
      if (!g.ok || g.value === undefined) {
        return g.error;
      }
      const v = g.value as Record<string, Object>;
      steps = v['steps'] as Object as ActionStep[];
      if (steps === undefined || steps === null) {
        steps = [];
      }
    }
    for (const s of steps) {
      if (s.id === 'file.save' || s.id === 'file.saveAs' || s.id.startsWith('view.')) {
        continue;
      }
      if (!automation.hasCommand(s.id)) {
        return `动作含未知命令 ${s.id}`;
      }
    }
    return '';
  }

  private stepsFor(actionName: string, inline: ActionStep[]): ActionStep[] {
    if (actionName.length > 0) {
      const g = automation.getAction({ 'action': actionName });
      if (g.ok && g.value !== undefined) {
        const v = g.value as Record<string, Object>;
        const s = v['steps'] as Object as ActionStep[];
        return s !== undefined && s !== null ? s : [];
      }
      return [];
    }
    return inline;
  }

  private runnable(steps: ActionStep[]): ActionStep[] {
    const out: ActionStep[] = [];
    for (const s of steps) {
      if (s.id === 'file.save' || s.id === 'file.saveAs' || s.id.startsWith('view.')) {
        continue; // 输出目录接管保存；视图步不改输出（photocraft #2752 同义）
      }
      out.push(s);
    }
    return out;
  }

  /** P4 D2：当前 active 会话 id（scratch 落位前闩锁；批后切回）。 */
  private currentDocId(): number {
    for (const d of this.engine.docsList()) {
      if (d.active) {
        return d.id;
      }
    }
    return 0;
  }

  /** 共用逐文件循环（batch 与 imageProcessor；photocraft process_files 同构）。 */
  private async processFiles(inputs: string[], output: string, format: string, quality: number,
    steps: ActionStep[], onProgress: (done: number, total: number, name: string) => void,
    perFile: () => Promise<void>): Promise<BatchResult> {
    const result: BatchResult = { files: [], errors: [] };
    const claims = new OutputClaims();
    // P4 D2 scratch：原文档 id 闩锁（整个批处理原文档/撤销历史原样不动）
    const origId = this.currentDocId();
    try {
      let done = 0;
      for (const path of inputs) {
        if (this.cancelFlag()) {
          result.errors.push({ file: path, error: '已取消' });
          continue;
        }
        const ext = format === 'same' ? (extOf(path).length > 0 ? extOf(path) : 'png') : format;
        const fmt = extToFormat(ext);
        const out = joinDir(output, `${stem(path)}.${ext}`);
        // 每文件独立 scratch 会话：新会话（激活）→ openFileFromFd 空档建档 → 执行 → 导出 → 关
        const scratch = this.engine.docNew();
        const scratchId = scratch !== null ? scratch.id : 0;
        try {
          const claimErr = claims.check(out);
          if (claimErr.length > 0) {
            throw new Error(claimErr);
          }
          const fd = fs.openSync(path, fs.OpenMode.READ_ONLY).fd;
          await this.engine.openFileFromFd(fd, stem(path));
          for (const s of steps) {
            const rr = await automation.execute(s.id, s.params);
            if (!rr.ok) {
              throw new Error(`步骤 ${s.id} 失败: ${rr.error}`);
            }
          }
          await perFile();
          const wfd = fs.openSync(out, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE |
            fs.OpenMode.TRUNC).fd;
          if (fmt === 'png') {
            // PNG 走 PngRowEncoder 流式直写（codec 面无 png 写，M25 定案）
            await this.engine.exportPng(wfd);
          } else {
            await this.engine.exportImage(wfd, fmt, quality);
          }
          claims.record(out, path);
          result.files.push(out);
        } catch (e) {
          result.errors.push({ file: path, error: (e as Error).message });
        } finally {
          if (scratchId !== 0) {
            this.engine.docClose(scratchId);
          }
        }
        done++;
        onProgress(done, inputs.length, stem(path));
      }
    } finally {
      // 切回原文档（关 scratch 后 active 已回邻位，显式按 id 切回更稳）
      if (origId !== 0) {
        this.engine.docSwitch(origId);
      }
    }
    return result;
  }

  async runBatch(opts: BatchOptions,
    onProgress: (done: number, total: number, name: string) => void): Promise<BatchResult> {
    const inputs = opts.inputDir.length > 0 ? Automate.listInputs(opts.inputDir) : opts.files;
    if (inputs.length === 0) {
      return { files: [], errors: [{ file: '', error: '没有可处理的输入文件' }] };
    }
    const steps = this.runnable(this.stepsFor(opts.actionName, opts.steps));
    automation.suspendRecording();
    try {
      const r = await this.processFiles(inputs, opts.output, opts.format, opts.quality, steps,
        onProgress, async (): Promise<void> => {
        });
      hilog.info(DOMAIN, TAG, 'batch done: %{public}d ok, %{public}d errors',
        r.files.length, r.errors.length);
      return r;
    } finally {
      automation.resumeRecording();
    }
  }

  async runProcessor(opts: ProcessorOptions,
    onProgress: (done: number, total: number, name: string) => void): Promise<BatchResult> {
    const inputs = opts.inputDir.length > 0 ? Automate.listInputs(opts.inputDir) : opts.files;
    if (inputs.length === 0) {
      return { files: [], errors: [{ file: '', error: '没有可处理的输入文件' }] };
    }
    const format = opts.format.length > 0 ? opts.format : 'jpeg';
    automation.suspendRecording();
    try {
      return await this.processFiles(inputs, opts.output, format,
        opts.quality > 0 ? opts.quality : 85, [], onProgress, async (): Promise<void> => {
          // fit 尺寸（dontEnlarge：不放大）
          if (opts.width > 0 || opts.height > 0) {
            const st = this.engine.getStats();
            const w = st.docWidth;
            const h = st.docHeight;
            if (w > 0 && h > 0) {
              const boxW = opts.width > 0 ? opts.width : 1e9;
              const boxH = opts.height > 0 ? opts.height : 1e9;
              const scale = Math.min(boxW / w, boxH / h);
              if (scale < 1) {
                const tw = Math.max(1, Math.round(w * scale));
                const th = Math.max(1, Math.round(h * scale));
                const ok = await this.engine.resampleDocument(tw, th);
                if (!ok) {
                  throw new Error('缩放失败');
                }
              }
            }
          }
        });
    } finally {
      automation.resumeRecording();
    }
  }
}
