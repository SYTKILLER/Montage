/**
 * M24c H5 测试控制通道（photocraft control-protocol 子集，debug-only）。
 * - 门控：仅 debugControl=1 want 参数（EntryAbility 透传 AppStorage）或帮助›调试手动开启；
 *   loopback only + token 认证（32B hex，hilog tag Montage.Control 透出）；
 * - 帧协议 = JSON 行：request {id, method, params} → reply {id, ok, ...}（V-15 无半关闭 →
 *   on('message') 行缓冲累积；JSON.stringify 无内嵌换行保证行帧完整）；
 * - 方法：auth / engine.execute（Automation.execute 白名单 = 注册表 + actions.*；P4 job 化：
 *   params.wait=false 立返 jobId，jobs.list 轮询，jobs.cancel 取消批处理族）/ jobs.list /
 *   jobs.cancel / state.inspect / ui.screenshot（flatten→exportPng 写 cacheDir）/ ui.shotRead /
 *   ui.stroke（P4 无头笔画驱动——动作录制/回放 E2E 通路）/ fs.mkdir|read|write（限 cacheDir，
 *   拒 '..'）/ app.batch / app.imageProcessor（Automate 驱动，wait=false 起后台 job 可取消）；
 * - 大载荷分块：shotRead/fs.read 按调用方给的 offset/length 回 base64（≤48KB/块建议）。
 */
import { socket } from '@kit.NetworkKit';
import { util } from '@kit.ArkTS';
import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { automation, AutoResult } from 'core';
import { Automate, BatchOptions, ProcessorOptions, BatchResult, ActionStep } from 'core';
import { CompositorEngine } from 'core';
import { COMMAND_TABLE, cmdChecked, cmdEnabled, i18nKeys, MenuState } from './CommandRegistry';
import { coverage, missingFor, registeredLangs, t as tr } from './I18n';

const TAG: string = 'Montage.Control';
const DOMAIN: number = 0x4D30;

/** 每连接状态。 */
class CtrlConn {
  conn: socket.TCPSocketConnection;
  authed: boolean = false;
  buffer: string = '';

  constructor(conn: socket.TCPSocketConnection) {
    this.conn = conn;
  }
}

function hexToken(): string {
  const chars = '0123456789abcdef';
  let s = '';
  for (let i = 0; i < 64; i++) {
    s += chars.charAt(Math.floor(Math.random() * 16));
  }
  return s;
}

function toRecord(v: Object): Record<string, Object> {
  return v as Record<string, Object>;
}

/** P4 后台 job（photocraft #210 同义）：长命令立返 jobId，poll/list 查态，批处理族可取消。 */
class CtrlJob {
  id: number;
  method: string;
  state: string = 'running';  // running | done | failed | cancelled
  value: Object | undefined = undefined;
  error: string = '';
  started: number = 0;
  doneAt: number = 0;
  cancelFlag: (() => boolean) | null = null;  // jobs.cancel 置位（Automate 每文件检查）
  cancelFn: (() => void) | null = null;       // 支持取消的任务挂接（批处理族）
  cancelled: boolean = false;

  constructor(id: number, method: string) {
    this.id = id;
    this.method = method;
    this.started = Date.now();
  }
}

/** M-EC 编辑会话登记行（smart.list 透出；Index.smartEdits 的镜像）。 */
export interface CtrlSmartEditRec {
  sessionId: number;
  parentId: number;
  layerId: number;
  contentsId: number;
  fileName: string;
}

export class ControlServer {
  private engine: CompositorEngine | null = null;
  private server: socket.TCPSocketServer | null = null;
  private token: string = '';
  private cacheDir: string = '';
  private port: number = 0;
  private running: boolean = false;
  private shotSeq: number = 0;
  // P3-H1：菜单状态快照提供者（Index 注入 —— commands.list 使能/勾选现状求值用）
  private stateFn: (() => MenuState) | null = null;
  // P4：无头笔画驱动（Index 注入——ui.stroke 走 strokeTouch 同管线，录制/回放 E2E 通路）
  private strokeFn: ((x0: number, y0: number, x1: number, y1: number) => void) | null = null;
  // M-EC：智能对象编辑会话驱动（Index 注入——smart.edit/finish 走 UI 同管线）
  private smartEditFn: ((layerId: number) => Promise<number>) | null = null;
  private smartFinishFn: ((sessionId: number) => Promise<boolean>) | null = null;
  private smartListFn: (() => CtrlSmartEditRec[]) | null = null;
  private jobs: CtrlJob[] = [];
  private jobSeq: number = 0;

  setEngine(e: CompositorEngine): void {
    this.engine = e;
  }

  setStateProvider(fn: (() => MenuState) | null): void {
    this.stateFn = fn;
  }

  setStrokeHandler(fn: ((x0: number, y0: number, x1: number, y1: number) => void) | null): void {
    this.strokeFn = fn;
  }

  /** M-EC：编辑会话原语（smart.edit/finish/list；Index 注入 UI 同管线）。 */
  setSmartHandlers(editFn: ((layerId: number) => Promise<number>) | null,
    finishFn: ((sessionId: number) => Promise<boolean>) | null,
    listFn: (() => CtrlSmartEditRec[]) | null): void {
    this.smartEditFn = editFn;
    this.smartFinishFn = finishFn;
    this.smartListFn = listFn;
  }

  // M-EC：raw docs.close 后的登记清理钩子（引擎原语不动 UI 态——Index 注入）
  private docClosedFn: ((sessionId: number) => void) | null = null;

  setDocClosedHandler(fn: ((sessionId: number) => void) | null): void {
    this.docClosedFn = fn;
  }

  /** 后台 job 启动：立返 jobId；promise 结算入 job（done/failed；cancelled 由 cancel 路径收尾）。 */
  private startJob(method: string, run: () => Promise<Object>): CtrlJob {
    this.jobSeq++;
    const job = new CtrlJob(this.jobSeq, method);
    this.jobs.push(job);
    if (this.jobs.length > 32) {
      this.jobs.splice(0, this.jobs.length - 32);  // 滚动保留最近 32 个
    }
    run().then((value: Object): void => {
      if (job.cancelled) {
        return;  // 已取消：结算丢弃
      }
      job.state = 'done';
      job.value = value;
      job.doneAt = Date.now();
    }).catch((e: Error): void => {
      if (job.cancelled) {
        return;
      }
      job.state = 'failed';
      job.error = e.message;
      job.doneAt = Date.now();
    });
    return job;
  }

  isRunning(): boolean {
    return this.running;
  }

  getPort(): number {
    return this.port;
  }

  /** cacheDir 由 Index 注入（fs 路径白名单根）。 */
  start(engine: CompositorEngine, cacheDir: string, port: number): boolean {
    if (this.running) {
      return true;
    }
    this.engine = engine;
    this.cacheDir = cacheDir.endsWith('/') ? cacheDir : cacheDir + '/';
    this.token = hexToken();
    const srv = socket.constructTCPSocketServerInstance();
    this.server = srv;
    srv.on('connect', (client: socket.TCPSocketConnection): void => {
      hilog.info(DOMAIN, TAG, 'client connected');
      const c = new CtrlConn(client);
      client.on('message', (value: socket.SocketMessageInfo): void => {
        this.onMessage(c, value.message);
      });
      client.on('close', (): void => {
        hilog.info(DOMAIN, TAG, 'client closed');
      });
      client.on('error', (err: Error): void => {
        hilog.error(DOMAIN, TAG, 'client error: %{public}s', err.message);
      });
    });
    const addr: socket.NetAddress = { address: '127.0.0.1', port: port, family: 1 };
    srv.listen(addr).then((): void => {
      this.running = true;
      this.port = port;
      hilog.info(DOMAIN, TAG, 'control server listening port=%{public}d token=%{public}s',
        port, this.token);
    }).catch((e: Error): void => {
      hilog.error(DOMAIN, TAG, 'listen failed: %{public}s', e.message);
      this.running = false;
    });
    return true;
  }

  stop(): void {
    if (this.server !== null) {
      this.server.close().catch((_e: Error): void => {
      });
      this.server = null;
    }
    this.running = false;
    hilog.info(DOMAIN, TAG, 'control server stopped');
  }

  /** hilog 之外的第二取 token 通道（调试面板显示用）。 */
  currentToken(): string {
    return this.token;
  }

  private send(c: CtrlConn, text: string): void {
    // JSON 行帧：回包必须带行终止符（客户端按行分帧；photocraft 协议同构）
    c.conn.send({ data: text + '\n', encoding: 'utf-8' }).catch((e: Error): void => {
      hilog.error(DOMAIN, TAG, 'send failed: %{public}s', e.message);
    });
  }

  private reply(c: CtrlConn, id: string, ok: boolean, payload: Record<string, Object>): void {
    const r: Record<string, Object> = { 'id': id, 'ok': ok };
    const keys: string[] = Object.keys(payload);
    for (const k of keys) {
      r[k] = payload[k];
    }
    if (!ok) {
      r['error'] = payload['error'] !== undefined ? payload['error'] : 'unknown';
    }
    this.send(c, JSON.stringify(r));
  }

  private onMessage(c: CtrlConn, data: ArrayBuffer): void {
    const dec = util.TextDecoder.create('utf-8');
    c.buffer += dec.decodeToString(new Uint8Array(data));
    let idx = c.buffer.indexOf('\n');
    while (idx >= 0) {
      const line = c.buffer.substring(0, idx).trim();
      c.buffer = c.buffer.substring(idx + 1);
      if (line.length > 0) {
        this.dispatch(c, line);
      }
      idx = c.buffer.indexOf('\n');
    }
    if (c.buffer.length > 4 * 1024 * 1024) {
      c.buffer = ''; // 防恶意超长行
    }
  }

  private dispatch(c: CtrlConn, line: string): void {
    let req: Record<string, Object>;
    try {
      req = JSON.parse(line) as Object as Record<string, Object>;
    } catch (err) {
      this.send(c, '{"id":"?","ok":false,"error":"malformed json"}');
      return;
    }
    const id = typeof req['id'] === 'string' ? req['id'] as string : String(req['id'] ?? '?');
    const method = typeof req['method'] === 'string' ? req['method'] as string : '';
    const params: Record<string, Object> = req['params'] !== undefined && req['params'] !== null
      ? toRecord(req['params']) : {} as Record<string, Object>;
    if (!c.authed) {
      if (method === 'auth') {
        const tok = typeof params['token'] === 'string' ? params['token'] as string : '';
        if (tok === this.token && this.token.length > 0) {
          c.authed = true;
          this.reply(c, id, true, {});
        } else {
          this.reply(c, id, false, { 'error': 'auth failed' });
        }
        return;
      }
      this.reply(c, id, false, { 'error': 'not authenticated' });
      return;
    }
    try {
      this.handle(c, id, method, params);
    } catch (err) {
      this.reply(c, id, false, { 'error': `handler error: ${(err as Error).message}` });
    }
  }

  /** cacheDir 白名单：拒绝 '..' 与越根路径。 */
  private safePath(p: string): string {
    if (p.includes('..')) {
      throw new Error('path rejected');
    }
    const norm = p.startsWith('/') ? p : this.cacheDir + p;
    if (!norm.startsWith(this.cacheDir)) {
      throw new Error('path outside cacheDir');
    }
    return norm;
  }

  private handle(c: CtrlConn, id: string, method: string, params: Record<string, Object>): void {
    // P4 job 化：wait=false 抽取（不透传给命令层——Record 复制排除键）
    const waitRaw: Object = params['wait'];
    const noWait = waitRaw !== undefined && waitRaw !== null && typeof waitRaw === 'boolean' &&
      !(waitRaw as boolean);
    const cmdParams: Record<string, Object> = {} as Record<string, Object>;
    for (const k of Object.keys(params)) {
      if (k !== 'wait') {
        cmdParams[k] = params[k];
      }
    }
    if (method === 'jobs.list' || method === 'jobs.cancel') {
      this.handleJobs(c, id, method, cmdParams);
      return;
    }
    if (method === 'ui.stroke') {
      // P4 无头笔画驱动（录制窗内自动入动作——与 UI strokeTouch 同管线）
      if (this.strokeFn === null) {
        this.reply(c, id, false, { 'error': 'no stroke handler' });
        return;
      }
      const x0 = typeof cmdParams['x0'] === 'number' ? cmdParams['x0'] as number : 0;
      const y0 = typeof cmdParams['y0'] === 'number' ? cmdParams['y0'] as number : 0;
      const x1 = typeof cmdParams['x1'] === 'number' ? cmdParams['x1'] as number : x0 + 100;
      const y1 = typeof cmdParams['y1'] === 'number' ? cmdParams['y1'] as number : y0;
      this.strokeFn(x0, y0, x1, y1);
      this.reply(c, id, true, { 'strokes': 1 });
      return;
    }
    if (method.startsWith('actions.')) {
      // 动作管理命令直达（engine.execute {command} 的顶层便捷路由）
      const run = (): Promise<Object> =>
        automation.execute(method, cmdParams).then((r: AutoResult): Object => {
          if (!r.ok) {
            throw new Error(r.error);
          }
          if (r.value !== undefined) {
            return r.value;
          }
          const empty: Record<string, Object> = {};
          return empty;
        });
      if (noWait) {
        const job = this.startJob(method, run);
        this.reply(c, id, true, { 'job': job.id, 'state': 'running' });
        return;
      }
      run().then((value: Object): void => {
        this.reply(c, id, true, { 'value': value });
      }).catch((e: Error): void => {
        this.reply(c, id, false, { 'error': `actions error: ${e.message}` });
      });
      return;
    }
    if (method === 'engine.execute') {
      const cmd = typeof cmdParams['command'] === 'string' ? cmdParams['command'] as string : '';
      const execParams: Record<string, Object> = cmdParams['params'] !== undefined &&
        cmdParams['params'] !== null ? toRecord(cmdParams['params']) : {} as Record<string, Object>;
      if (cmd.length === 0) {
        this.reply(c, id, false, { 'error': 'missing "command"' });
        return;
      }
      const run = (): Promise<Object> =>
        automation.execute(cmd, execParams).then((r: AutoResult): Object => {
          if (!r.ok) {
            throw new Error(r.error);
          }
          if (r.value !== undefined) {
            return r.value;
          }
          const empty: Record<string, Object> = {};
          return empty;
        });
      if (noWait) {
        const job = this.startJob(`engine.execute:${cmd}`, run);
        this.reply(c, id, true, { 'job': job.id, 'state': 'running' });
        return;
      }
      run().then((value: Object): void => {
        this.reply(c, id, true, { 'value': value });
      }).catch((e: Error): void => {
        this.reply(c, id, false, { 'error': `execute error: ${e.message}` });
      });
      return;
    }
    if (method === 'state.inspect') {
      const payload: Record<string, Object> = {};
      if (this.engine !== null) {
        payload['stats'] = this.engine.getStats() as Object as Record<string, Object>;
      }
      const acts = automation.listActions();
      if (acts.value !== undefined) {
        payload['actions'] = acts.value;
      }
      payload['recording'] = automation.recordingName();
      payload['suspended'] = automation.isSuspended();
      payload['port'] = this.port;
      this.reply(c, id, true, payload);
      return;
    }
    if (method === 'commands.list') {
      // P3-H1 四端同源导出：命令注册表全量元数据 + 使能/勾选现状求值 + 动作注册表
      const st: MenuState | null = this.stateFn !== null ? this.stateFn() : null;
      const menu: Record<string, Object>[] = [];
      for (const s of COMMAND_TABLE) {
        const row: Record<string, Object> = {};
        row['id'] = s.id;
        row['zh'] = s.zh;
        row['en'] = s.en;
        row['menu'] = s.menu;
        row['parent'] = s.parent;
        row['shortcut'] = s.shortcut;
        row['params'] = s.params;
        row['isSub'] = s.isSub;
        row['coalesce'] = s.coalesce;
        row['when'] = s.when;
        row['chk'] = s.chk;
        if (st !== null) {
          row['label'] = s.isSub ? tr(`sub.${s.id}`, s.zh) : tr(`cmd.${s.id}`, s.zh);
          row['enabled'] = cmdEnabled(s, st);
          row['checked'] = cmdChecked(s, st);
        }
        menu.push(row);
      }
      const payload2: Record<string, Object> = {};
      payload2['menu'] = menu as Object;
      const auto = automation.specDump();
      if (auto.value !== undefined) {
        payload2['auto'] = auto.value;
      }
      payload2['count'] = menu.length;
      this.reply(c, id, true, payload2);
      return;
    }
    if (method === 'i18n.check') {
      // P3-H8 强制覆盖测试（photocraft complete_menus 同义）：每个已注册语言包对
      // 命令注册表全键核验，缺失清单非空 = 失败；随包缺键累计（missingFor）一并透出
      const keys = i18nKeys();
      const langs: Record<string, Object> = {};
      let fail = false;
      for (const lang of registeredLangs()) {
        const miss = coverage(lang, keys).concat(missingFor(lang));
        const arr: string[] = miss;
        const v: Record<string, Object> = {};
        v['missing'] = arr as Object;
        v['missingCount'] = arr.length;
        langs[lang] = v;
        if (arr.length > 0) {
          fail = true;
        }
      }
      const pl: Record<string, Object> = {};
      pl['keys'] = keys.length;
      pl['langs'] = langs as Object;
      this.reply(c, id, !fail, pl);
      return;
    }
    // M-EC：智能对象编辑会话原语（E2E 通道，docs.* 同族；不入 automation 注册表）
    if (method === 'smart.edit' || method === 'smart.finish' || method === 'smart.list') {
      if (this.engine === null) {
        this.reply(c, id, false, { 'error': 'no engine' });
        return;
      }
      if (method === 'smart.list') {
        const rows = this.smartListFn !== null ? this.smartListFn() : [];
        this.reply(c, id, true, { 'edits': rows as Object });
        return;
      }
      const sidV: Object = cmdParams['id'];
      if (typeof sidV !== 'number') {
        this.reply(c, id, false, { 'error': 'missing numeric "id"' });
        return;
      }
      const sid = sidV as number;
      if (method === 'smart.edit') {
        if (this.smartEditFn === null) {
          this.reply(c, id, false, { 'error': 'no smart edit handler' });
          return;
        }
        this.smartEditFn(sid).then((newSid: number): void => {
          if (newSid !== 0) {
            this.reply(c, id, true, { 'sessionId': newSid });
          } else {
            this.reply(c, id, false, { 'error': 'smart edit failed (see hilog)' });
          }
        }).catch((e: Error): void => {
          this.reply(c, id, false, { 'error': `smart.edit: ${e.message}` });
        });
        return;
      }
      if (this.smartFinishFn === null) {
        this.reply(c, id, false, { 'error': 'no smart finish handler' });
        return;
      }
      this.smartFinishFn(sid).then((ok: boolean): void => {
        this.reply(c, id, ok, ok ? { 'sessionId': sid } : { 'error': 'smart finish failed' });
      }).catch((e: Error): void => {
        this.reply(c, id, false, { 'error': `smart.finish: ${e.message}` });
      });
      return;
    }
    // P4 D2：多文档池驱动（E2E 通道；会话原语不入 automation 注册表——不可重放）
    if (method === 'docs.list' || method === 'docs.switch' || method === 'docs.close' ||
        method === 'docs.new') {
      if (this.engine === null) {
        this.reply(c, id, false, { 'error': 'no engine' });
        return;
      }
      if (method === 'docs.list') {
        this.reply(c, id, true, { 'docs': this.engine.docsList() as Object });
      } else if (method === 'docs.switch' || method === 'docs.close') {
        const idV: Object = cmdParams['id'];
        if (typeof idV !== 'number') {
          this.reply(c, id, false, { 'error': 'missing numeric "id"' });
          return;
        }
        const did = idV as number;
        const ok = method === 'docs.switch' ? this.engine.docSwitch(did) : this.engine.docClose(did);
        if (ok && method === 'docs.close' && this.docClosedFn !== null) {
          this.docClosedFn(did);  // M-EC：编辑会话登记同步清理
        }
        this.reply(c, id, ok, ok ? {} : { 'error': `doc ${did} not found` });
      } else {
        const dn = this.engine.docNew();
        if (dn === null) {
          this.reply(c, id, false, { 'error': 'docNew failed' });
        } else {
          // 回包键勿用 'id'——reply() 的帧 id 会被 payload 同名键覆盖（docs.new 探针实证）
          this.reply(c, id, true, { 'docId': dn.id });
        }
      }
      return;
    }
    if (method === 'ui.screenshot') {
      if (this.engine === null) {
        this.reply(c, id, false, { 'error': 'no engine' });
        return;
      }
      this.shotSeq++;
      const shotPath = `${this.cacheDir}ctrl_shot_${this.shotSeq}.png`;
      try {
        const fd = fs.openSync(shotPath, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE |
          fs.OpenMode.TRUNC).fd;
        this.engine.exportPng(fd).then((_d: Object): void => {
          const st = fs.statSync(shotPath);
          this.reply(c, id, true, { 'shot': shotPath, 'bytes': st.size });
        }).catch((e: Error): void => {
          this.reply(c, id, false, { 'error': `exportPng: ${e.message}` });
        });
      } catch (err) {
        this.reply(c, id, false, { 'error': `screenshot: ${(err as Error).message}` });
      }
      return;
    }
    if (method === 'ui.shotRead' || method === 'fs.read') {
      const p = typeof params['path'] === 'string' ? params['path'] as string
        : (typeof params['shot'] === 'string' ? params['shot'] as string : '');
      const offset = typeof params['offset'] === 'number' ? params['offset'] as number : 0;
      const length = typeof params['length'] === 'number' ? params['length'] as number : 49152;
      try {
        const path = this.safePath(p);
        const f = fs.openSync(path, fs.OpenMode.READ_ONLY);
        try {
          const size = fs.statSync(path).size;
          const buf = new ArrayBuffer(Math.min(length, Math.max(0, size - offset)));
          const n = fs.readSync(f.fd, buf, { offset: offset, length: buf.byteLength });
          const b64 = new util.Base64Helper().encodeToStringSync(new Uint8Array(buf, 0, n));
          this.reply(c, id, true, { 'b64': b64, 'size': size });
        } finally {
          fs.closeSync(f.fd);
        }
      } catch (err) {
        this.reply(c, id, false, { 'error': `fs.read: ${(err as Error).message}` });
      }
      return;
    }
    if (method === 'fs.write') {
      const p = typeof params['path'] === 'string' ? params['path'] as string : '';
      const b64 = typeof params['b64'] === 'string' ? params['b64'] as string : '';
      try {
        const path = this.safePath(p);
        const bin = new util.Base64Helper().decodeSync(b64);
        const f = fs.openSync(path, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE |
          fs.OpenMode.TRUNC);
        try {
          fs.writeSync(f.fd, bin.buffer);
        } finally {
          fs.closeSync(f.fd);
        }
        this.reply(c, id, true, { 'written': bin.byteLength });
      } catch (err) {
        this.reply(c, id, false, { 'error': `fs.write: ${(err as Error).message}` });
      }
      return;
    }
    if (method === 'fs.mkdir') {
      const p = typeof params['path'] === 'string' ? params['path'] as string : '';
      try {
        const path = this.safePath(p);
        try {
          fs.mkdirSync(path, true);
        } catch (e) {
          // 已存在视为成功
        }
        this.reply(c, id, true, {});
      } catch (err) {
        this.reply(c, id, false, { 'error': `fs.mkdir: ${(err as Error).message}` });
      }
      return;
    }
    if (method === 'app.batch' || method === 'app.imageProcessor') {
      if (this.engine === null) {
        this.reply(c, id, false, { 'error': 'no engine' });
        return;
      }
      // 路径全部经 cacheDir 白名单解析（相对路径补根；Automate 需要绝对路径）
      let inputDir = typeof params['input'] === 'string' ? params['input'] as string : '';
      if (inputDir.length > 0) {
        inputDir = this.safePath(inputDir);
      }
      let files: string[] = [];
      if (inputDir.length === 0) {
        const arr = params['files'] as Object as Object[];
        if (arr !== undefined && arr !== null) {
          for (const x of arr) {
            if (typeof x === 'string') {
              files.push(this.safePath(x));
            }
          }
        }
      }
      const output = typeof params['output'] === 'string'
        ? this.safePath(params['output'] as string) : '';
      const format = typeof params['format'] === 'string' ? params['format'] as string :
        (method === 'app.batch' ? 'same' : 'jpeg');
      const quality = typeof params['quality'] === 'number' ? params['quality'] as number : 0;
      const actionName = typeof params['action'] === 'string' ? params['action'] as string : '';
      let steps: ActionStep[] = [];
      const rawSteps = params['steps'] as Object as Object[];
      if (rawSteps !== undefined && rawSteps !== null) {
        for (const rs of rawSteps) {
          const pair = rs as Object as Object[];
          if (pair !== undefined && pair.length >= 1 && typeof pair[0] === 'string') {
            const p2 = pair.length > 1 && pair[1] !== undefined && pair[1] !== null
              ? toRecord(pair[1]) : {} as Record<string, Object>;
            steps.push({ id: pair[0] as string, params: p2 });
          }
        }
      }
      if (inputDir.length === 0 && files.length === 0) {
        this.reply(c, id, false, { 'error': 'missing "input" (folder) or "files"' });
        return;
      }
      if (output.length === 0) {
        this.reply(c, id, false, { 'error': 'missing "output"' });
        return;
      }
      // P4：wait=false → 后台可取消 job（批处理族独享 cancelFn；Automate 每文件查旗标）
      let cancelReq = false;
      const am = new Automate(this.engine, (): boolean => cancelReq,
        `${this.cacheDir}ctrl_restore.montage`);
      if (method === 'app.batch') {
        const preErr = Automate.checkSteps(actionName, steps);
        if (preErr.length > 0) {
          this.reply(c, id, false, { 'error': preErr });
          return;
        }
        const opts: BatchOptions = {
          inputDir: inputDir, files: files, output: output, actionName: actionName,
          steps: steps, format: format, quality: quality,
        };
        const run = (): Promise<Object> => am.runBatch(opts,
          (_d: number, _t: number, _n: string): void => {
          }).then((r: BatchResult): Object => {
          if (cancelReq) {
            throw new Error('cancelled');
          }
          return this.batchPayload(r) as Object;
        });
        if (noWait) {
          const job = this.startJob('app.batch', run);
          job.cancelFn = (): void => {
            cancelReq = true;
          };
          this.reply(c, id, true, { 'job': job.id, 'state': 'running' });
          return;
        }
        run().then((value: Object): void => {
          this.reply(c, id, true, value as Record<string, Object>);
        }).catch((e: Error): void => {
          this.reply(c, id, false, { 'error': `batch: ${e.message}` });
        });
      } else {
        const opts: ProcessorOptions = {
          inputDir: inputDir, files: files, output: output, format: format,
          width: typeof cmdParams['width'] === 'number' ? cmdParams['width'] as number : 0,
          height: typeof cmdParams['height'] === 'number' ? cmdParams['height'] as number : 0,
          dontEnlarge: true, quality: quality,
          convertToSrgb: typeof cmdParams['convertToSrgb'] === 'boolean'
            ? cmdParams['convertToSrgb'] as boolean : false,
        };
        const run = (): Promise<Object> => am.runProcessor(opts,
          (_d: number, _t: number, _n: string): void => {
          }).then((r: BatchResult): Object => this.batchPayload(r) as Object);
        if (noWait) {
          const job = this.startJob('app.imageProcessor', run);
          job.cancelFn = (): void => {
            cancelReq = true;
          };
          this.reply(c, id, true, { 'job': job.id, 'state': 'running' });
          return;
        }
        run().then((value: Object): void => {
          this.reply(c, id, true, value as Record<string, Object>);
        }).catch((e: Error): void => {
          this.reply(c, id, false, { 'error': `processor: ${e.message}` });
        });
      }
      return;
    }
    this.reply(c, id, false, { 'error': `unknown method ${method}` });
  }

  /** P4：jobs.list / jobs.cancel（photocraft #210 语义子集）。 */
  private handleJobs(c: CtrlConn, id: string, method: string, params: Record<string, Object>): void {
    if (method === 'jobs.list') {
      const rows: Record<string, Object>[] = [];
      const jobFilter: Object = params['job'];
      const filter = typeof jobFilter === 'number' ? jobFilter as number : 0;
      for (const j of this.jobs) {
        if (filter !== 0 && j.id !== filter) {
          continue;
        }
        const row: Record<string, Object> = {
          'id': j.id, 'method': j.method, 'state': j.state,
          'ms': j.doneAt > 0 ? j.doneAt - j.started : Date.now() - j.started,
        };
        if (j.value !== undefined) {
          row['value'] = j.value;
        }
        if (j.error.length > 0) {
          row['error'] = j.error;
        }
        rows.push(row);
      }
      this.reply(c, id, true, { 'jobs': rows as Object });
      return;
    }
    // jobs.cancel {job}
    const jobV: Object = params['job'];
    if (typeof jobV !== 'number') {
      this.reply(c, id, false, { 'error': 'jobs.cancel needs numeric "job"' });
      return;
    }
    for (const j of this.jobs) {
      if (j.id !== (jobV as number)) {
        continue;
      }
      if (j.state !== 'running') {
        this.reply(c, id, false, { 'error': `job ${j.id} already ${j.state}` });
        return;
      }
      if (j.cancelFn === null) {
        this.reply(c, id, false, { 'error': `job ${j.id} does not support cancel` });
        return;
      }
      j.cancelled = true;
      j.cancelFn();
      j.state = 'cancelled';  // 批处理循环随后以「已取消」错误收尾（结算被 cancelled 屏蔽）
      j.doneAt = Date.now();
      this.reply(c, id, true, { 'job': j.id, 'state': 'cancelled' });
      return;
    }
    this.reply(c, id, false, { 'error': 'no such job' });
  }

  private batchPayload(r: BatchResult): Record<string, Object> {
    const errs: Record<string, Object>[] = [];
    for (const e of r.errors) {
      errs.push({ 'file': e.file, 'error': e.error });
    }
    const payload: Record<string, Object> = {
      'files': r.files as Object as Object[],
      'errors': errs as Object as Object[],
    };
    return payload;
  }
}

/** 全局单例（Index 持有并挂 engine）。 */
export const controlServer: ControlServer = new ControlServer();
