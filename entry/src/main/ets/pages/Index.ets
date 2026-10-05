import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { image } from '@kit.ImageKit';
import { CompositorEngine, OpenFileData, PixelTestData, ViewportData } from 'core';

const TAG: string = '[Montage.M1]';

/**
 * M1 图片打开与画布浏览（临时脚手架，编辑器骨架落地后替换）：
 * 1) DocumentViewPicker 选图 → fd → 分带区域解码进 TileGrid（渐进显示）
 * 2) 单层瓦片 GL 绘制（棋盘格透明底 + 按需上传 + LRU）
 * 3) 双指缩放/单指平移（GL 内视口变换，手势绑无变换宿主，05 §4 范式）
 * 4) onAxisEvent 滚轮缩放（锚点=光标，D5.4）+ fit/100%/步进缩放
 * 5) 应用内生成测试大图（6144×4096）走全链路（ImagePacker→fd→导入）
 */
@Entry
@Component
struct Index {
  @StorageProp('docVersion') docVersion: number = 0;
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
  @State pixelText: string = '未运行';
  @State statsText: string = '等待渲染…';
  @State logLines: string[] = [];
  private engine: CompositorEngine = new CompositorEngine();
  private xcController: XComponentController = new XComponentController();
  private statsTimer: number = -1;
  private surfaceReady: boolean = false;
  private docOpen: boolean = false;
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

  // ---------- 动作 ----------

  private onCanvasReady(): void {
    try {
      const id: string = this.xcController.getXComponentSurfaceId();
      this.log(`surfaceId=${id}`);
      const ok: boolean = this.engine.createSurface(id);
      this.log(`创建渲染表面: ${ok ? '成功' : '失败'}`);
      this.doNewDocument();
      this.surfaceReady = true;
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

  private doFit(): void {
    this.tryFit();
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

  // ---------- 手势与滚轮（05 §4：锚点公式 O = C − (C0 − O0)·k 等价实现） ----------

  private onWheel(e: AxisEvent): void {
    if (e.scrollStep === undefined || e.scrollStep === 0) {
      return;
    }
    const k = this.pxPerVp();
    const factor = Math.pow(1.1, -e.scrollStep);
    const ax = this.panX + (e.x * k) / this.zoom;
    const ay = this.panY + (e.y * k) / this.zoom;
    const z = this.clampZoom(this.zoom * factor);
    this.applyViewport(z, ax - (e.x * k) / z, ay - (e.y * k) / z);
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

  // ---------- 打开与测试图 ----------

  private onOpenImage(): void {
    const opt = new picker.DocumentSelectOptions();
    opt.maxSelectNumber = 1;
    opt.fileSuffixFilters = ['.png', '.jpg', '.jpeg', '.webp', '.bmp', '.gif'];
    const dp = new picker.DocumentViewPicker();
    dp.select(opt).then((uris: string[]): void => {
      if (uris.length < 1) {
        return;
      }
      this.openByUri(uris[0]);
    }).catch((e: BusinessError): void => {
      this.log(`选择文件失败: ${e.code} ${e.message}`);
    });
  }

  private openByUri(uri: string): void {
    const f = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    this.log(`开始导入 fd=${f.fd}`);
    this.importing = true;
    this.engine.openFileFromFd(f.fd).then((d: OpenFileData): void => {
      // fd 所有权已移交 native（含失败路径），此处不再 close
      this.importing = false;
      this.docW = d.width;
      this.docH = d.height;
      this.docOpen = true;
      this.fittedOnce = false;
      this.log(`导入完成 ${d.width}x${d.height}，自动适配`);
      this.tryFit();
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
    const w = 6144;
    const h = 4096;
    const t0 = Date.now();
    try {
      this.log(`生成测试图 ${w}x${h}（渐变+棋盘+圆环）…`);
      const buf = new ArrayBuffer(w * h * 4);
      const u32 = new Uint32Array(buf);
      const cx = w / 2;
      const cy = h / 2;
      for (let y = 0; y < h; y++) {
        const rowBase = y * w;
        const dy = y - cy;
        for (let x = 0; x < w; x++) {
          const dx = x - cx;
          const dist = Math.sqrt(dx * dx + dy * dy);
          let r = (x * 255 / w) | 0;
          let g = (y * 255 / h) | 0;
          let b = ((x + y) % 64 < 32) ? 255 : 60;
          if (dist > 400 && dist < 1600 && ((dist / 40) | 0) % 2 === 0) {
            r = 255 - r;  // 圆环细条纹（检接缝/摩尔纹）
            b = 255 - b;
          }
          u32[rowBase + x] = 0xFF000000 | (b << 16) | (g << 8) | r;
        }
      }
      const genMs = Date.now() - t0;
      this.log(`图案生成完成 ${genMs}ms，编码 PNG…`);
      const pm: image.PixelMap = image.createPixelMapSync(buf, {
        size: { width: w, height: h },
        pixelFormat: image.PixelMapFormat.RGBA_8888,
      });
      const path = `${getContext(this).cacheDir}/m1_test.png`;
      const f = fs.openSync(path, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      const packer = image.createImagePacker();
      await packer.packToFile(pm, f.fd, { format: 'image/png', quality: 100 });
      pm.release();
      fs.closeSync(f.fd);
      this.log(`PNG 已写入 ${path}（${Date.now() - t0}ms），走导入全链路…`);
      const rf = fs.openSync(path, fs.OpenMode.READ_ONLY);
      this.importing = true;
      await this.engine.openFileFromFd(rf.fd);
      this.importing = false;
      this.docW = w;
      this.docH = h;
      this.docOpen = true;
      this.fittedOnce = false;
      this.log(`测试图导入完成，总耗时 ${Date.now() - t0}ms，自动适配`);
      this.tryFit();
    } catch (e) {
      this.importing = false;
      const err: BusinessError = e as BusinessError;
      this.log(`测试图流程失败: ${err.code} ${err.message}`);
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
    this.statsText = `fps=${s.fps.toFixed(1)} frames=${s.frames} buf=${s.bufferWidth}x${s.bufferHeight} ` +
      `瓦片缓存渲染中 running=${s.renderRunning}` + (s.lastError === '' ? '' : ` err=${s.lastError}`);
  }

  build() {
    Column({ space: 8 }) {
      Row({ space: 12 }) {
        Text('Montage · M1 画布浏览')
          .fontSize(18)
          .fontWeight(FontWeight.Bold)
        Text(`文档版本号: ${this.docVersion}`)
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

      Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
        Button('打开图片').onClick((): void => this.onOpenImage())
        Button('生成测试大图').onClick((): void => {
          this.generateTestImage();
        }).margin({ left: 8 })
        Button('适配').onClick((): void => this.doFit()).margin({ left: 8 })
        Button('100%').onClick((): void => this.doZoom100()).margin({ left: 8 })
        Button('放大').onClick((): void => this.doZoomStep(1.25)).margin({ left: 8 })
        Button('缩小').onClick((): void => this.doZoomStep(0.8)).margin({ left: 8 })
        Button('像素测试').onClick((): void => this.doPixelTest()).margin({ left: 8 })
      }
      .width('100%')

      if (this.importing) {
        Row({ space: 8 }) {
          Text(`导入中 ${(this.importProgress * 100).toFixed(0)}%`)
            .fontSize(12)
            .fontColor($r('app.color.text_secondary'))
          Progress({ value: this.importProgress * 100, total: 100 }).width('60%')
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
      .onAxisEvent((e: AxisEvent): void => {
        this.onWheel(e);
      })
      .gesture(GestureGroup(GestureMode.Parallel,
        PanGesture({ fingers: 1, distance: 2 })
          .onActionStart((e: GestureEvent): void => this.onPanStart(e))
          .onActionUpdate((e: GestureEvent): void => this.onPanUpdate(e)),
        PinchGesture({ fingers: 2, distance: 1 })
          .onActionStart((e: PinchGestureEvent): void => this.onPinchStart(e))
          .onActionUpdate((e: PinchGestureEvent): void => this.onPinchUpdate(e))
          .onActionEnd((): void => this.onPinchEnd())
          .onActionCancel((): void => this.onPinchEnd())
      ))

      Text(`统计: ${this.statsText}`)
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
        .width('100%')
      Text(`像素测试: ${this.pixelText}`)
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
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
      .height(110)
      .scrollBar(BarState.Off)
    }
    .width('100%')
    .height('100%')
    .padding(12)
    .backgroundColor($r('app.color.page_background'))
  }
}
