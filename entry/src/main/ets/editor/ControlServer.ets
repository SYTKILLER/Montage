/**
 * M24c H5 测试控制通道（photocraft control-protocol 子集，debug-only）。
 * - 门控：仅 debugControl=1 want 参数（EntryAbility 透传 AppStorage）或帮助›调试手动开启；
 *   loopback only + token 认证（32B hex，hilog tag Montage.Control 透出）；
 * - 帧协议 = JSON 行：request {id, method, params} → reply {id, ok, ...}（V-15 无半关闭 →
 *   on('message') 行缓冲累积；JSON.stringify 无内嵌换行保证行帧完整）；
 * - 方法：auth / engine.execute（Automation.execute 白名单 = 注册表 + actions.*）/ state.inspect /
 *   ui.screenshot（flatten→exportPng 写 cacheDir）/ ui.shotRead / fs.mkdir|read|write（限 cacheDir，
 *   拒 '..'）/ app.batch / app.imageProcessor（Automate 驱动）；
 * - 大载荷分块：shotRead/fs.read 按调用方给的 offset/length 回 base64（≤48KB/块建议）。
 */
import { socket } from '@kit.NetworkKit';
import { util } from '@kit.ArkTS';
import { fileIo as fs } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { automation, AutoResult } from 'core';
import { Automate, BatchOptions, ProcessorOptions, BatchResult, ActionStep } from 'core';
import { CompositorEngine } from 'core';

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

export class ControlServer {
  private engine: CompositorEngine | null = null;
  private server: socket.TCPSocketServer | null = null;
  private token: string = '';
  private cacheDir: string = '';
  private port: number = 0;
  private running: boolean = false;
  private shotSeq: number = 0;

  setEngine(e: CompositorEngine): void {
    this.engine = e;
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
    if (method.startsWith('actions.')) {
      // 动作管理命令直达（engine.execute {command} 的顶层便捷路由）
      automation.execute(method, params).then((r: AutoResult): void => {
        const payload: Record<string, Object> = {};
        if (r.value !== undefined) {
          payload['value'] = r.value;
        }
        this.reply(c, id, r.ok, r.ok ? payload : { 'error': r.error });
      }).catch((e: Error): void => {
        this.reply(c, id, false, { 'error': `actions error: ${e.message}` });
      });
      return;
    }
    if (method === 'engine.execute') {
      const cmd = typeof params['command'] === 'string' ? params['command'] as string : '';
      const execParams: Record<string, Object> = params['params'] !== undefined &&
        params['params'] !== null ? toRecord(params['params']) : {} as Record<string, Object>;
      if (cmd.length === 0) {
        this.reply(c, id, false, { 'error': 'missing "command"' });
        return;
      }
      automation.execute(cmd, execParams).then((r: AutoResult): void => {
        const payload: Record<string, Object> = {};
        if (r.value !== undefined) {
          payload['value'] = r.value;
        }
        this.reply(c, id, r.ok, r.ok ? payload : { 'error': r.error });
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
        new Automate(this.engine, (): boolean => false, `${this.cacheDir}ctrl_restore.montage`)
          .runBatch(opts, (_d: number, _t: number, _n: string): void => {
          }).then((r: BatchResult): void => {
          this.reply(c, id, true, this.batchPayload(r));
        }).catch((e: Error): void => {
          this.reply(c, id, false, { 'error': `batch: ${e.message}` });
        });
      } else {
        const opts: ProcessorOptions = {
          inputDir: inputDir, files: files, output: output, format: format,
          width: typeof params['width'] === 'number' ? params['width'] as number : 0,
          height: typeof params['height'] === 'number' ? params['height'] as number : 0,
          dontEnlarge: true, quality: quality,
          convertToSrgb: typeof params['convertToSrgb'] === 'boolean'
            ? params['convertToSrgb'] as boolean : false,
        };
        new Automate(this.engine, (): boolean => false, `${this.cacheDir}ctrl_restore.montage`)
          .runProcessor(opts, (_d: number, _t: number, _n: string): void => {
          }).then((r: BatchResult): void => {
          this.reply(c, id, true, this.batchPayload(r));
        }).catch((e: Error): void => {
          this.reply(c, id, false, { 'error': `processor: ${e.message}` });
        });
      }
      return;
    }
    this.reply(c, id, false, { 'error': `unknown method ${method}` });
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
