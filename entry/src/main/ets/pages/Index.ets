import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { image } from '@kit.ImageKit';
import { CompositorEngine, LayerDTO, OpenFileData, OpenProjectData, PixelTestData, PsdOpenData, SaveProjectData, ViewportData } from 'core';

const TAG: string = '[Montage.M2]';

const BLEND_LABELS: string[] = [
  '正常',
  '变暗', '正片叠底', '颜色加深', '线性加深',
  '变亮', '滤色', '颜色减淡', '线性减淡(添加)',
  '叠加', '柔光', '强光', '亮光', '线性光', '点光', '实色混合',
  '差值', '排除', '减去', '划分',
  '色相', '饱和度', '颜色', '明度',
];

/**
 * M2 图层栈合成（临时脚手架，编辑器骨架落地后替换）：
 * 1) 多图层文档（自底向上）+ 两趟合成（assemble + blend，W3C 可分离混合公式，03 §4）
 * 2) 图层面板（05 §6 数据流：docVersion → getLayerListSnapshot → @State → ForEach）
 * 3) 选中/显隐/不透明度/混合模式（normal·multiply·screen·overlay）/上移下移
 * 4) 打开图片 = 追加图层（无文档则建文档）；双图层叠放验证混合
 * 5) 滚轮缩放加固：onAxisEvent 挂 XComponent 本体 + 原始值日志 + 步长钳制
 */
@Entry
@Component
struct Index {
  @StorageProp('docVersion') @Watch('onDocVersion') docVersion: number = 0;
  @StorageProp('importProgress') importProgress: number = 0;
  @State zoom: number = 1;
  @State panX: number = 0;
  @State panY: number = 0;
  @State docW: number = 0;
  @State docH: number = 0;
  @State canvasVpW: number = 0;
  @State canvasVpH: number = 0;
  @State importing: boolean = false;
  @State generating: boolean = false;
  @State layers: LayerDTO[] = [];
  @State blendSel: number = 0;
  @State opacityVal: number = 1;
  @State tool: string = 'brush';       // brush | eraser | none
  @State maskTarget: boolean = false;  // M5b：落笔目标 = 活动图层蒙版
  @State brushSize: number = 40;
  @State brushHardness: number = 80;
  @State brushColor: string = '#D93025';
  @State canUndo: boolean = false;
  @State canRedo: boolean = false;
  @State painting: boolean = false;
  @State saving: boolean = false;
  private saveUri: string = '';  // 已关联工程包 URI（保存直达覆写，04 §2）
  @State pixelText: string = '未运行';
  @State statsText: string = '等待渲染…';
  @State logLines: string[] = [];
  private engine: CompositorEngine = new CompositorEngine();
  private xcController: XComponentController = new XComponentController();
  private statsTimer: number = -1;
  @State docOpen: boolean = false;
  private fittedOnce: boolean = false;
  private pinchActive: boolean = false;
  private pinchBaseZoom: number = 1;
  private pinchAx: number = 0;
  private pinchAy: number = 0;
  private panLastX: number = 0;
  private panLastY: number = 0;


  aboutToAppear(): void {
    this.engine.bindRevisionListener((revision: number): void => {
      this.log(`docVersion → ${revision}`);
    });
    this.engine.bindImportListener((progress: number): void => {
      if (progress >= 1) {
        this.importing = false;
      }
    });
    this.statsTimer = setInterval((): void => {
      this.refreshStats();
    }, 1000);
  }

  aboutToDisappear(): void {
    if (this.statsTimer >= 0) {
      clearInterval(this.statsTimer);
      this.statsTimer = -1;
    }
    this.engine.dispose();
  }

  private onDocVersion(): void {
    this.refreshLayers();
  }

  private refreshLayers(): void {
    this.layers = this.engine.getLayerListSnapshot();
    const active = this.activeLayer();
    if (active !== undefined) {
      this.blendSel = active.blendMode;
      this.opacityVal = active.opacity;
    } else {
      this.blendSel = 0;
      this.opacityVal = 1;
    }
  }

  private activeLayer(): LayerDTO | undefined {
    return this.layers.find((l: LayerDTO): boolean => l.isActive);
  }

  // 面板自顶向下显示（PS 惯例：顶部=最上层）
  private reversedLayers(): LayerDTO[] {
    const out: LayerDTO[] = [];
    for (let i = this.layers.length - 1; i >= 0; i--) {
      out.push(this.layers[i]);
    }
    return out;
  }

  private log(msg: string): void {
    this.logLines.splice(0, 0, msg);
    if (this.logLines.length > 24) {
      this.logLines.splice(24);
    }
    hilog.info(0x4D30, TAG, '%{public}s', msg);
  }

  private pxPerVp(): number {
    return this.getUIContext().vp2px(1);
  }

  private clampZoom(z: number): number {
    return Math.min(32, Math.max(1 / 32, z));
  }

  private applyViewport(z: number, panX: number, panY: number): void {
    const zz = this.clampZoom(z);
    this.zoom = zz;
    this.panX = panX;
    this.panY = panY;
    this.engine.setViewport(zz, panX, panY);
  }

  private canvasPxW(): number {
    return this.canvasVpW * this.pxPerVp();
  }

  private canvasPxH(): number {
    return this.canvasVpH * this.pxPerVp();
  }

  private screenToDoc(xVp: number, yVp: number): number[] {
    const k = this.pxPerVp();
    return [this.panX + (xVp * k) / this.zoom, this.panY + (yVp * k) / this.zoom];
  }

  // ---------- 画布动作 ----------

  private onCanvasReady(): void {
    try {
      const id: string = this.xcController.getXComponentSurfaceId();
      this.log(`surfaceId=${id}`);
      const ok: boolean = this.engine.createSurface(id);
      this.log(`创建渲染表面: ${ok ? '成功' : '失败'}`);
      this.doNewDocument();
    } catch (e) {
      const err: BusinessError = e as BusinessError;
      this.log(`画布初始化失败: ${err.code} ${err.message}`);
    }
  }

  private doNewDocument(): void {
    const ok: boolean = this.engine.newDocument(4096, 4096);
    this.docOpen = ok;
    this.docW = 4096;
    this.docH = 4096;
    this.fittedOnce = false;
    this.saveUri = '';  // 换文档即解除旧包关联
    this.log(`新建空文档 4096x4096: ${ok ? '成功' : '失败'}`);
    this.tryFit();
  }

  private tryFit(): void {
    if (!this.docOpen || this.canvasVpW <= 0 || this.canvasVpH <= 0) {
      return;
    }
    const vp: ViewportData | null = this.engine.fitToWindow(this.canvasPxW(), this.canvasPxH());
    if (vp !== null) {
      this.zoom = vp.zoom;
      this.panX = vp.panX;
      this.panY = vp.panY;
      this.fittedOnce = true;
      this.log(`适配视口: zoom=${(vp.zoom * 100).toFixed(1)}%`);
    }
  }

  private doZoom100(): void {
    const c: number[] = this.screenToDoc(this.canvasVpW / 2, this.canvasVpH / 2);
    const k = this.pxPerVp();
    this.applyViewport(1, c[0] - (this.canvasVpW / 2) * k, c[1] - (this.canvasVpH / 2) * k);
  }

  private doZoomStep(multiplier: number): void {
    const c: number[] = this.screenToDoc(this.canvasVpW / 2, this.canvasVpH / 2);
    const z = this.clampZoom(this.zoom * multiplier);
    const k = this.pxPerVp();
    this.applyViewport(z, c[0] - (this.canvasVpW / 2) * k / z, c[1] - (this.canvasVpH / 2) * k / z);
  }

  // ---------- 滚轮（D5.4 滚轮直接缩放，锚点=光标） ----------

  private onWheel(e: AxisEvent): void {
    if (e.scrollStep === undefined || e.scrollStep === 0) {
      return;
    }
    // hilog 不支持 %{public}f（实证：格式被吞成 "}f"），浮点转 int 再打
    hilog.info(0x4D30, TAG, 'axis x=%{public}d y=%{public}d raw100=%{public}d action=%{public}d',
      Math.round(e.x), Math.round(e.y), Math.round(e.scrollStep * 100), e.action);
    // M2.5 实证（模拟器）：AxisEvent.x/y 是 px 非 vp（4350 = 1460vp × 密度），锚点直接用 px；
    // scrollStep 恒 3.0（模拟器转发丢方向，真机待回归）→ 按 3.0 = 一档归一，单步 1.1
    const factor = Math.pow(1.1, e.scrollStep / 3.0);
    const ax = this.panX + e.x / this.zoom;
    const ay = this.panY + e.y / this.zoom;
    const z = this.clampZoom(this.zoom * factor);
    this.applyViewport(z, ax - e.x / z, ay - e.y / z);
  }

  private onPinchStart(e: PinchGestureEvent): void {
    this.pinchActive = true;
    this.pinchBaseZoom = this.zoom;
    const a: number[] = this.screenToDoc(e.pinchCenterX, e.pinchCenterY);
    this.pinchAx = a[0];
    this.pinchAy = a[1];
  }

  private onPinchUpdate(e: PinchGestureEvent): void {
    const z = this.clampZoom(this.pinchBaseZoom * e.scale);
    const k = this.pxPerVp();
    this.applyViewport(z, this.pinchAx - (e.pinchCenterX * k) / z,
      this.pinchAy - (e.pinchCenterY * k) / z);
  }

  private onPinchEnd(): void {
    this.pinchActive = false;
  }

  private onPanStart(e: GestureEvent): void {
    if (this.pinchActive || e.fingerList.length < 1) {
      return;
    }
    this.panLastX = e.fingerList[0].localX;
    this.panLastY = e.fingerList[0].localY;
  }

  private onPanUpdate(e: GestureEvent): void {
    if (this.pinchActive || e.fingerList.length < 1) {
      return;
    }
    const k = this.pxPerVp();
    const dx = (e.fingerList[0].localX - this.panLastX) * k;
    const dy = (e.fingerList[0].localY - this.panLastY) * k;
    this.panLastX = e.fingerList[0].localX;
    this.panLastY = e.fingerList[0].localY;
    this.applyViewport(this.zoom, this.panX - dx / this.zoom, this.panY - dy / this.zoom);
  }

  // ---------- 导入（= 追加图层）与测试图 ----------

  private onOpenImage(): void {
    const opt = new picker.DocumentSelectOptions();
    opt.maxSelectNumber = 1;
    // M2.5 实测：fileSuffixFilters 传 .psd 时列表异常为空（过滤实现疑似仅认首个图片类后缀），
    // 改为不过滤 + 扩展名分发
    // 用户测试文件所在目录（picker 直达）
    opt.defaultFilePathUri = 'file://docs/storage/Users/currentUser/Download/com.huawei.hmos.browser';
    const dp = new picker.DocumentViewPicker();
    dp.select(opt).then((uris: string[]): void => {
      if (uris.length < 1) {
        return;
      }
      const uri = uris[0].toLowerCase();
      if (uri.endsWith('.psd')) {
        this.openPsdByUri(uris[0]);
      } else if (uri.endsWith('.montage')) {
        this.openProjectByUri(uris[0]);
      } else {
        this.openByUri(uris[0]);
      }
    }).catch((e: BusinessError): void => {
      this.log(`选择文件失败: ${e.code} ${e.message}`);
    });
  }

  private openPsdByUri(uri: string): void {
    const f = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    this.log(`打开 PSD fd=${f.fd}`);
    this.importing = true;
    this.engine.openPsdFile(f.fd).then((d: PsdOpenData): void => {
      this.importing = false;
      this.docW = d.width;
      this.docH = d.height;
      this.docOpen = true;
      this.fittedOnce = false;
      this.saveUri = '';  // 换文档即解除旧包关联
      this.log(`PSD 打开完成 ${d.width}x${d.height}，${d.notes.length} 条提示，自动适配`);
      this.refreshLayers();
      for (let i = 0; i < d.notes.length && i < 8; i++) {
        this.log(`提示: ${d.notes[i]}`);
      }
      this.tryFit();
    }).catch((e: BusinessError): void => {
      this.importing = false;
      this.log(`PSD 打开失败: ${e.message}`);
    });
  }

  // ---------- 工程保存（M4b） ----------

  private doSaveProject(): void {
    if (!this.docOpen) {
      this.log('无文档可保存');
      return;
    }
    if (this.saving) {
      return;
    }
    if (this.saveUri !== '') {
      this.writeProjectTo(this.saveUri);
      return;
    }
    const opt = new picker.DocumentSaveOptions();
    opt.newFileNames = ['未命名.montage'];
    const dp = new picker.DocumentViewPicker();
    dp.save(opt).then((uris: string[]): void => {
      if (uris.length < 1) {
        return;
      }
      this.saveUri = uris[0];
      this.writeProjectTo(uris[0]);
    }).catch((e: BusinessError): void => {
      this.log(`保存选择失败: ${e.code} ${e.message}`);
    });
  }

  private writeProjectTo(uri: string): void {
    this.saving = true;
    // TRUNC 清空旧包再整包重写（非原子替换，O4-7 登记）；fd 移交 native（全路径关闭）
    const f = fs.openSync(uri, fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
    this.log(`保存工程 fd=${f.fd} → ${uri}`);
    this.engine.saveProject(f.fd).then((d: SaveProjectData): void => {
      this.saving = false;
      this.log(`工程已保存：${d.layers} 层，${d.bytes} 字节`);
    }).catch((e: BusinessError): void => {
      this.saving = false;
      this.log(`保存失败: ${e.message}`);
    });
  }

  private openProjectByUri(uri: string): void {
    const f = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    this.log(`打开工程 fd=${f.fd}`);
    this.importing = true;
    this.engine.openProject(f.fd).then((d: OpenProjectData): void => {
      this.importing = false;
      this.docW = d.width;
      this.docH = d.height;
      this.docOpen = true;
      this.fittedOnce = false;
      this.saveUri = uri;  // 已关联包：后续保存直接覆写（04 §2）
      this.log(`工程已打开 ${d.width}x${d.height}，${d.layers} 层`);
      this.refreshLayers();
      this.tryFit();
    }).catch((e: BusinessError): void => {
      this.importing = false;
      this.log(`工程打开失败: ${e.message}`);
    });
  }

  private openByUri(uri: string): void {
    const f = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    this.log(`导入图层 fd=${f.fd}`);
    this.importing = true;
    this.engine.openFileFromFd(f.fd).then((d: OpenFileData): void => {
      // fd 所有权已移交 native（含失败路径），此处不再 close
      this.importing = false;
      if (d.width > this.docW || d.height > this.docH) {
        this.docW = Math.max(this.docW, d.width);
        this.docH = Math.max(this.docH, d.height);
      }
      this.docOpen = true;
      this.log(`图层已添加 ${d.width}x${d.height} (id=${d.layerId})`);
    }).catch((e: BusinessError): void => {
      this.importing = false;
      this.log(`导入失败: ${e.message}`);
    });
  }

  private async generateTestImage(): Promise<void> {
    if (this.generating) {
      return;
    }
    this.generating = true;
    const w = 2048;
    const h = 1536;
    const t0 = Date.now();
    try {
      const colorShift = (this.layers.length % 2) * 120;
      this.log(`生成测试图层 ${w}x${h} #${this.layers.length + 1}…`);
      const buf = new ArrayBuffer(w * h * 4);
      const u32 = new Uint32Array(buf);
      const cx = w / 2;
      const cy = h / 2;
      for (let y = 0; y < h; y++) {
        const rowBase = y * w;
        for (let x = 0; x < w; x++) {
          const dx = x - cx;
          const dy = y - cy;
          const dist = Math.sqrt(dx * dx + dy * dy);
          const r = ((x * 255 / w) + colorShift) % 256 | 0;
          const g = (y * 255 / h) | 0;
          const b = ((x + y) % 64 < 32 ? 255 : 60);
          const alpha = dist < 640 ? 255 : 90;  // 圆形软边界：下层可透出（验证混合）
          u32[rowBase + x] = ((alpha << 24) | (b << 16) | (g << 8) | r) >>> 0;
        }
      }
      const pm: image.PixelMap = image.createPixelMapSync(buf, {
        size: { width: w, height: h },
        pixelFormat: image.PixelMapFormat.RGBA_8888,
      });
      const path = `${getContext(this).cacheDir}/m2_layer_${this.layers.length}.png`;
      const f = fs.openSync(path, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      const packer = image.createImagePacker();
      await packer.packToFile(pm, f.fd, { format: 'image/png', quality: 100 });
      pm.release();
      fs.closeSync(f.fd);
      this.importing = true;
      await this.engine.openFileFromFd(fs.openSync(path, fs.OpenMode.READ_ONLY).fd);
      this.importing = false;
      this.docOpen = true;
      this.log(`测试图层导入完成 ${Date.now() - t0}ms（含透明区，验证混合）`);
      this.tryFit();
    } catch (e) {
      this.importing = false;
      const err: BusinessError = e as BusinessError;
      this.log(`测试图层流程失败: ${err.code} ${err.message}`);
    }
    this.generating = false;
  }

  private doPixelTest(): void {
    this.pixelText = '运行中…';
    this.engine.runPixelTest().then((data: PixelTestData): void => {
      this.pixelText = `DMA 读写=${data.dma.writeReadOk ? '通过' : '不通过'}  ` +
        `共享内存读写=${data.shared.writeReadOk ? '通过' : '不通过'}`;
    }).catch((e: BusinessError): void => {
      this.pixelText = `失败: ${e.message}`;
    });
  }

  private refreshStats(): void {
    const s = this.engine.getStats();
    this.statsText = `fps=${s.fps.toFixed(1)} frames=${s.frames} running=${s.renderRunning}` +
      (s.lastError === '' ? '' : ` err=${s.lastError}`);
    this.canUndo = s.canUndo;
    this.canRedo = s.canRedo;
  }

  private hexToRgb(hex: string): number[] {
    const r = parseInt(hex.slice(1, 3), 16) / 255;
    const g = parseInt(hex.slice(3, 5), 16) / 255;
    const b = parseInt(hex.slice(5, 7), 16) / 255;
    return [r, g, b];
  }

  private pushBrushSettings(): void {
    const rgb = this.hexToRgb(this.brushColor);
    this.engine.setBrushSettings(this.brushSize, this.brushHardness / 100, 1.0,
      rgb[0], rgb[1], rgb[2], this.tool === 'eraser');
  }

  private doUndo(): void {
    if (this.engine.undo()) {
      this.log('撤销');
    }
  }

  private doRedo(): void {
    if (this.engine.redo()) {
      this.log('重做');
    }
  }

  // 触摸事件流（05 §4：笔画走 onTouch 原始事件，逐事件坐标）
  private onTouchEvent(e: TouchEvent): void {
    if (this.tool !== 'brush' && this.tool !== 'eraser') {
      return;
    }
    if (e.touches.length < 1 && e.changedTouches.length < 1) {
      return;
    }
    const t: TouchObject = e.changedTouches.length > 0 ? e.changedTouches[0] : e.touches[0];
    // M3.1 压感：SDK 约定 0.0/undefined = 设备不支持 → 归一为 1（行为同无压感）
    const pressure: number = (t.pressure !== undefined && t.pressure > 0) ? t.pressure : 1.0;
    if (e.type === TouchType.Down) {
      hilog.info(0x4D30, TAG, 'stroke down (%{public}f,%{public}f) p=%{public}f', t.x, t.y, pressure);
    }
    const k = this.pxPerVp();
    const docX = this.panX + (t.x * k) / this.zoom;
    const docY = this.panY + (t.y * k) / this.zoom;
    switch (e.type) {
      case TouchType.Down:
        this.pushBrushSettings();
        this.painting = true;
        this.engine.beginStroke(docX, docY, pressure);
        break;
      case TouchType.Move:
        if (this.painting) {
          this.engine.continueStroke(docX, docY, pressure);
        }
        break;
      case TouchType.Up:
      case TouchType.Cancel:
        if (this.painting) {
          this.painting = false;
          this.engine.endStroke();
        }
        break;
      default:
        break;
    }
  }

  // ---------- 图层面板动作 ----------

  // M5b：蒙版落笔目标切换（无蒙版图层不可开；开启时强制画笔工具）
  private activeLayerHasMask(): boolean {
    for (const l of this.layers) {
      if (l.isActive) {
        return l.hasMask;
      }
    }
    return false;
  }

  private onToggleMaskTarget(): void {
    if (!this.activeLayerHasMask()) {
      this.log('当前图层无蒙版');
      return;
    }
    this.maskTarget = !this.maskTarget;
    if (this.maskTarget && this.tool === 'eraser') {
      this.tool = 'brush';
    }
    this.engine.setBrushTarget(this.maskTarget);
    this.log(`落笔目标: ${this.maskTarget ? '蒙版' : '像素'}`);
  }

  private onAddMask(): void {
    const active = this.layers.find((l: LayerDTO): boolean => l.isActive);
    if (active === undefined) {
      return;
    }
    if (this.engine.addLayerMask(active.id)) {
      this.log(`已为图层 ${active.name} 添加蒙版`);
    }
  }

  // M5b-2：创建/释放剪贴蒙版
  private onToggleClipping(): void {
    const active = this.activeLayer();
    if (active === undefined) {
      return;
    }
    if (this.engine.setLayerClipping(active.id, !active.clipping)) {
      this.log(`剪贴蒙版: ${!active.clipping ? '创建' : '释放'}（${active.name}）`);
    }
  }

  // M6a：添加 Levels 调整层
  private onAddAdjustment(): void {
    const id: number = this.engine.addAdjustmentLayer();
    if (id !== 0) {
      this.log(`已添加 Levels 调整层 (id=${id})`);
    }
  }

  // M6a：调整层参数（滑条拖动结束）
  private onAdjustParamChange(inBlack: number, inWhite: number, gamma: number): void {
    const active = this.activeLayer();
    if (active === undefined || !active.isAdjustment) {
      return;
    }
    this.engine.setAdjustmentParams(active.id, inBlack, inWhite, gamma);
  }

  private onToggleVisible(l: LayerDTO): void {
    this.engine.setLayerVisible(l.id, !l.visible);
  }

  // M5a：蒙版启用/停用
  private maskEnabled(l: LayerDTO): boolean {
    return l.maskEnabled;
  }

  private onToggleMask(l: LayerDTO): void {
    this.engine.setLayerMaskEnabled(l.id, !l.maskEnabled);
  }

  private onSelectLayer(l: LayerDTO): void {
    if (!l.isActive) {
      this.engine.selectLayer(l.id);
    }
  }

  private onOpacityChange(v: number): void {
    const active = this.activeLayer();
    if (active !== undefined) {
      this.engine.setLayerOpacity(active.id, v);
    }
  }

  private onBlendChange(sel: number): void {
    const active = this.activeLayer();
    if (active !== undefined) {
      this.engine.setLayerBlendMode(active.id, sel);
    }
  }

  private onMoveLayer(l: LayerDTO, up: boolean): void {
    const idx = this.layers.findIndex((x: LayerDTO): boolean => x.id === l.id);
    const target = up ? idx + 1 : idx - 1;  // UI 倒序：上=更高层
    if (target < 0 || target >= this.layers.length) {
      return;
    }
    this.engine.reorderLayer(l.id, target);
  }

  build() {
    Column({ space: 8 }) {
      Row({ space: 12 }) {
        Text('Montage · M2 图层合成')
          .fontSize(18)
          .fontWeight(FontWeight.Bold)
        Text(`v${this.docVersion}`)
          .fontSize(13)
          .fontColor($r('app.color.text_primary'))
        Blank()
        Text(this.docOpen ? `${this.docW}×${this.docH}` : '无文档')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
        Text(`${(this.zoom * 100).toFixed(1)}%`)
          .fontSize(14)
          .fontColor($r('app.color.text_primary'))
      }
      .width('100%')

      Row({ space: 8 }) {
        // 画布区
        Column({ space: 6 }) {
          Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
            Button('打开图片').onClick((): void => this.onOpenImage())
            Button('生成测试图层').onClick((): void => {
              this.generateTestImage();
            }).margin({ left: 8 })
            Button('适配').onClick((): void => this.tryFit()).margin({ left: 8 })
            Button('100%').onClick((): void => this.doZoom100()).margin({ left: 8 })
            Button('放大').onClick((): void => this.doZoomStep(1.25)).margin({ left: 8 })
            Button('缩小').onClick((): void => this.doZoomStep(0.8)).margin({ left: 8 })
            Button('撤销').onClick((): void => this.doUndo()).margin({ left: 8 }).opacity(this.canUndo ? 1 : 0.4)
            Button('重做').onClick((): void => this.doRedo()).margin({ left: 8 }).opacity(this.canRedo ? 1 : 0.4)
            Button('保存').onClick((): void => this.doSaveProject()).margin({ left: 8 })
              .opacity(this.docOpen ? 1 : 0.4)
          }
          .width('100%')

          Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
            Button('画笔').fontSize(12)
              .backgroundColor(this.tool === 'brush' ? '#1A6BD9' : '#888888')
              .onClick((): void => {
                this.tool = 'brush';
              })
            Button('橡皮').fontSize(12).margin({ left: 6 })
              .backgroundColor(this.tool === 'eraser' ? '#1A6BD9' : '#888888')
              .onClick((): void => {
                this.tool = 'eraser';
              })
            Button('导航').fontSize(12).margin({ left: 6 })
              .backgroundColor(this.tool === 'none' ? '#1A6BD9' : '#888888')
              .onClick((): void => {
                this.tool = 'none';
              })
            Button('蒙版笔').fontSize(12).margin({ left: 6 })
              .backgroundColor(this.maskTarget ? '#D93025' : '#888888')
              .opacity(this.activeLayerHasMask() ? 1 : 0.4)
              .onClick((): void => this.onToggleMaskTarget())
            Text('直径').fontSize(11).margin({ left: 10 })
            Slider({ value: this.brushSize, min: 2, max: 200, step: 1 })
              .width(120).height(26)
              .onChange((v: number, mode: SliderChangeMode): void => {
                if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                  this.brushSize = v;
                }
              })
            Text('硬度').fontSize(11).margin({ left: 6 })
            Slider({ value: this.brushHardness, min: 0, max: 100, step: 1 })
              .width(100).height(26)
              .onChange((v: number, mode: SliderChangeMode): void => {
                if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                  this.brushHardness = v;
                }
              })
            ForEach(['#D93025', '#1A6BD9', '#188038', '#F9AB00', '#111111'], (c: string) => {
              Column()
                .width(22).height(22).borderRadius(11)
                .backgroundColor(c)
                .border({ width: this.brushColor === c ? 3 : 1, color: this.brushColor === c ? '#111111' : '#CCCCCC' })
                .margin({ left: 6 })
                .onClick((): void => {
                  this.brushColor = c;
                })
            }, (c: string): string => c)
            Text(this.tool === 'eraser' ? '橡皮' : this.tool === 'brush' ? '画笔' : '导航')
              .fontSize(11).fontColor($r('app.color.text_secondary')).margin({ left: 8 })
          }
          .width('100%')

          if (this.importing) {
            Row({ space: 8 }) {
              Text(`导入中 ${(this.importProgress * 100).toFixed(0)}%`)
                .fontSize(12)
                .fontColor($r('app.color.text_secondary'))
              Progress({ value: this.importProgress * 100, total: 100 }).width('50%')
            }
            .width('100%')
          }

          Stack() {
            XComponent({ id: 'montage-canvas', type: XComponentType.SURFACE, controller: this.xcController })
              .width('100%')
              .height('100%')
              .onLoad((): void => {
                this.onCanvasReady();
              })
              .onAxisEvent((e: AxisEvent): void => {
                this.onWheel(e);
              })
              .onTouch((e: TouchEvent): void => {
                this.onTouchEvent(e);
              })
          }
          .width('100%')
          .layoutWeight(1)
          .onAreaChange((oldValue: Area, newValue: Area): void => {
            this.canvasVpW = Number(newValue.width);
            this.canvasVpH = Number(newValue.height);
            if (this.docOpen && !this.fittedOnce) {
              this.tryFit();
            }
          })
          .gesture(this.tool === 'none' ?
            GestureGroup(GestureMode.Parallel,
              PanGesture({ fingers: 1, distance: 2 })
                .onActionStart((e: GestureEvent): void => this.onPanStart(e))
                .onActionUpdate((e: GestureEvent): void => this.onPanUpdate(e)),
              PinchGesture({ fingers: 2, distance: 1 })
                .onActionStart((e: PinchGestureEvent): void => this.onPinchStart(e))
                .onActionUpdate((e: PinchGestureEvent): void => this.onPinchUpdate(e))
                .onActionEnd((): void => this.onPinchEnd())
                .onActionCancel((): void => this.onPinchEnd())
            )
            : TapGesture())

          Text(`${this.statsText} | 像素测试: ${this.pixelText}`)
            .fontSize(11)
            .fontColor($r('app.color.text_secondary'))
            .width('100%')
            .maxLines(1)
        }
        .layoutWeight(1)
        .alignItems(HorizontalAlign.Start)

        // 图层面板（05 §1：280vp 右置）
        Column({ space: 8 }) {
          Text('图层').fontSize(14).fontWeight(FontWeight.Medium)
          Row({ space: 8 }) {
            Text('不透明度').fontSize(12).fontColor($r('app.color.text_secondary'))
            Slider({ value: this.opacityVal * 100, min: 0, max: 100, step: 1 })
              .layoutWeight(1)
              .onChange((v: number, mode: SliderChangeMode): void => {
                if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                  this.onOpacityChange(v / 100);
                }
              })
          }
          .width('100%')
          Row({ space: 8 }) {
            Text('混合').fontSize(12).fontColor($r('app.color.text_secondary'))
            Select([{ value: BLEND_LABELS[0] }, { value: BLEND_LABELS[1] },
              { value: BLEND_LABELS[2] }, { value: BLEND_LABELS[3] }] as SelectOption[])
              .selected(this.blendSel)
              .value(BLEND_LABELS[this.blendSel] ?? '正常')
              .onSelect((index: number): void => {
                this.onBlendChange(index);
              })
          }
          .width('100%')

          // M6a：调整层参数（活动层为 Levels 时显示）
          if (this.activeLayer()?.isAdjustment === true) {
            Row({ space: 6 }) {
              Text('黑').fontSize(11).fontColor($r('app.color.text_secondary'))
              Slider({ value: this.activeLayer()!.adjBlack * 255, min: 0, max: 250, step: 1 })
                .width(90).height(24)
                .onChange((v: number, mode: SliderChangeMode): void => {
                  if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                    const a = this.activeLayer()!;
                    this.onAdjustParamChange(v / 255, a.adjWhite, a.adjGamma);
                  }
                })
              Text('白').fontSize(11).fontColor($r('app.color.text_secondary'))
              Slider({ value: this.activeLayer()!.adjWhite * 255, min: 5, max: 255, step: 1 })
                .width(90).height(24)
                .onChange((v: number, mode: SliderChangeMode): void => {
                  if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                    const a = this.activeLayer()!;
                    this.onAdjustParamChange(a.adjBlack, v / 255, a.adjGamma);
                  }
                })
              Text('γ').fontSize(11).fontColor($r('app.color.text_secondary'))
              Slider({ value: this.activeLayer()!.adjGamma, min: 0.1, max: 10, step: 0.05 })
                .width(90).height(24)
                .onChange((v: number, mode: SliderChangeMode): void => {
                  if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                    const a = this.activeLayer()!;
                    this.onAdjustParamChange(a.adjBlack, a.adjWhite, v);
                  }
                })
            }
            .width('100%')
          }

          List({ space: 4 }) {
            ForEach(this.reversedLayers(), (l: LayerDTO) => {
              ListItem() {
                Row({ space: 8 }) {
                  SymbolGlyph($r('sys.symbol.eye'))
                    .fontSize(18)
                    .fontColor([l.visible ? '#1A6BD9' : '#999999'])
                    .opacity(l.visible ? 1 : 0.5)
                    .onClick((): void => this.onToggleVisible(l))
                  if (l.hasMask) {
                    // M5a 蒙版开关：实心 = 启用（渲染走合成结果），灰 = 停用
                    Text('蒙')
                      .fontSize(10)
                      .width(20).height(20).borderRadius(4)
                      .textAlign(TextAlign.Center)
                      .fontColor('#FFFFFF')
                      .backgroundColor(this.maskEnabled(l) ? '#1A6BD9' : '#999999')
                      .onClick((): void => this.onToggleMask(l))
                  }
                  Column({ space: 2 }) {
                    Text(l.name)
                      .fontSize(12)
                      .maxLines(1)
                      .textOverflow({ overflow: TextOverflow.Ellipsis })
                    Text(`${BLEND_LABELS[l.blendMode] ?? '正常'} · ${(l.opacity * 100).toFixed(0)}%`)
                      .fontSize(10)
                      .fontColor($r('app.color.text_secondary'))
                  }
                  .alignItems(HorizontalAlign.Start)
                  .layoutWeight(1)
                  .onClick((): void => this.onSelectLayer(l))
                  Column() {
                    Text('↑').fontSize(12).onClick((): void => this.onMoveLayer(l, true))
                    Text('↓').fontSize(12).onClick((): void => this.onMoveLayer(l, false))
                  }
                }
                .width('100%')
                .padding(8)
                .borderRadius(8)
                .backgroundColor(l.isActive ? '#1A6BD91A' : '#00000000')
                .border({ width: 1, color: l.isActive ? '#1A6BD9' : '#00000000' })
              }
            }, (l: LayerDTO): string =>
              `${l.id}|${l.visible}|${l.isActive}|${l.blendMode}|${Math.round(l.opacity * 100)}|${l.thumbToken}|${l.hasMask}|${l.maskEnabled}`)
          }
          .layoutWeight(1)
          .scrollBar(BarState.Off)

          Row({ space: 8 }) {
            Button('新建').fontSize(12).onClick((): void => {
              this.engine.addLayer();
            })
            Button('加蒙版').fontSize(12).onClick((): void => this.onAddMask())
            Button('剪贴').fontSize(12)
              .backgroundColor(this.activeLayer()?.clipping ? '#D93025' : '#888888')
              .onClick((): void => this.onToggleClipping())
            Button('调整层').fontSize(12).onClick((): void => this.onAddAdjustment())
            Button('删除').fontSize(12).onClick((): void => {
              const a = this.activeLayer();
              if (a !== undefined) {
                this.engine.removeLayer(a.id);
              }
            })
          }
          .width('100%')
          .justifyContent(FlexAlign.Center)
        }
        .width(270)
        .padding(10)
        .borderRadius(12)
        .backgroundColor($r('app.color.page_background'))
      }
      .layoutWeight(1)
      .width('100%')

      List({ space: 2 }) {
        ForEach(this.logLines, (line: string) => {
          ListItem() {
            Text(line)
              .fontSize(10)
              .fontColor($r('app.color.text_primary'))
              .width('100%')
          }
        }, (line: string, index: number): string => `${index}|${line}`)
      }
      .width('100%')
      .height(80)
      .scrollBar(BarState.Off)
    }
    .width('100%')
    .height('100%')
    .padding(12)
    .backgroundColor($r('app.color.page_background'))
  }
}
