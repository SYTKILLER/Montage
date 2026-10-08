import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { image } from '@kit.ImageKit';
import { window } from '@kit.ArkUI';
import { CompositorEngine, LayerDTO, LayerThumb, OpenFileData, OpenProjectData, PsdOpenData, SaveProjectData, ViewportData } from 'core';
import { AppPrefs, RecentProject, ThemeMode } from '../editor/AppPrefs';
import { leafItem, MenuGroupVM, MenuItemVM, subItem, ToolVM } from '../editor/MenuModel';
import { Toolbox } from '../editor/components/Toolbox';
import { OptionsBar } from '../editor/components/OptionsBar';
import { StatusBar } from '../editor/components/StatusBar';
import { WelcomeView } from '../editor/components/WelcomeView';
import { LayersPanel, ThumbEntry } from '../editor/components/LayersPanel';
import { ExportDialog } from '../editor/components/ExportDialog';
import { ABOUT_TEXT, InfoDialog, SHORTCUT_ROWS } from '../editor/components/InfoDialog';

const TAG: string = 'Montage.Shell';
const DOMAIN: number = 0x4D30;

// KeyCode（@ohos.multimodalInput.keyCode 已核验，2026-10-07）
const KC_B: number = 2018;
const KC_C: number = 2019;
const KC_D: number = 2020;
const KC_E: number = 2021;
const KC_H: number = 2024;
const KC_I: number = 2025;
const KC_L: number = 2028;
const KC_M: number = 2029;
const KC_N: number = 2030;
const KC_O: number = 2031;
const KC_S: number = 2035;
const KC_V: number = 2038;
const KC_W: number = 2039;
const KC_Y: number = 2041;
const KC_Z: number = 2042;
const KC_ALT_L: number = 2045;
const KC_ALT_R: number = 2046;
const KC_SHIFT_L: number = 2047;
const KC_SHIFT_R: number = 2048;
const KC_SPACE: number = 2050;
const KC_MINUS: number = 2057;
const KC_LBRACKET: number = 2059;
const KC_RBRACKET: number = 2060;
const KC_EQUALS: number = 2058;
const KC_CTRL_L: number = 2072;
const KC_CTRL_R: number = 2073;
const KC_NUM0: number = 2000;
const KC_NUM1: number = 2001;
const KC_ESC: number = 2070;
const KC_ENTER: number = 2054;
const KC_UP: number = 2012;
const KC_DOWN: number = 2013;
const KC_LEFT: number = 2014;
const KC_RIGHT: number = 2015;

/**
 * M7.5 编辑器外壳（06 规划 §4）：菜单栏 + 选项条 + 工具箱 + 画布 + 面板 + 状态栏。
 * 命令表是唯一入口：菜单/快捷键共用 runCommand；工具态驱动手势换绑与选项条。
 */
@Entry
@Component
struct Index {
  @StorageProp('docVersion') @Watch('onDocVersion') docVersion: number = 0;
  @StorageProp('importProgress') importProgress: number = 0;
  @State docOpen: boolean = false;
  @State docW: number = 0;
  @State docH: number = 0;
  @State zoom: number = 1;
  @State panX: number = 0;
  @State panY: number = 0;
  @State canvasVpW: number = 0;
  @State canvasVpH: number = 0;
  @State importing: boolean = false;
  @State busyText: string = '';
  @State layers: LayerDTO[] = [];
  @State layerOpacity: number = 100;   // 0-100 显示单位
  @State blendSel: number = 0;
  @State adjBlack: number = 0;         // 0-255
  @State adjWhite: number = 255;
  @State adjGamma: number = 1;
  @State tool: string = 'brush';
  @State spaceHeld: boolean = false;
  @State brushSize: number = 40;
  @State brushHardness: number = 80;
  @State brushOpacity: number = 100;   // 5-100 百分比（引擎单位 0-1 在 pushBrushSettings 换算）
  @State brushColor: string = '#D93025';
  @State maskTarget: boolean = false;
  @State wandTolerance: number = 32;
  @State wandContiguous: boolean = true;
  @State canUndo: boolean = false;
  @State canRedo: boolean = false;
  @State fps: number = 0;
  @State recents: RecentProject[] = [];
  @State themeMode: ThemeMode = 'system';
  @State layersVisible: boolean = true;
  @State showExport: boolean = false;
  @State showRename: boolean = false;
  @State showShortcuts: boolean = false;
  @State showAbout: boolean = false;
  @State showTheme: boolean = false;
  @State renameText: string = '';
  @State thumbVersion: number = 0;

  private engine: CompositorEngine = new CompositorEngine();
  private xcController: XComponentController = new XComponentController();
  private prefs: AppPrefs = new AppPrefs();
  private statsTimer: number = -1;
  private saveUri: string = '';
  private fittedOnce: boolean = false;
  private pinchActive: boolean = false;
  private pinchBaseZoom: number = 1;
  private pinchAx: number = 0;
  private pinchAy: number = 0;
  private panLastX: number = 0;
  private panLastY: number = 0;
  private ctrlHeld: boolean = false;
  private shiftHeld: boolean = false;
  private altHeld: boolean = false;
  private rectDrag: boolean = false;
  private rectX0: number = 0;
  private rectY0: number = 0;
  private rectX1: number = 0;
  private rectY1: number = 0;
  private moveActive: boolean = false;
  private moveLastX: number = 0;
  private moveLastY: number = 0;
  private cropDrag: boolean = false;
  private cropX0: number = 0;
  private cropY0: number = 0;
  @State cropRect: number[] | null = null;   // [x,y,w,h] 文档坐标；非空 = 待提交/取消
  @State hasSelectionFlag: boolean = false;  // 会话态镜像（引擎无此查询命令，操作点维护）
  private readonly thumbCache: Map<number, ThumbEntry> = new Map();
  private readonly thumbsInFlight: Set<number> = new Set();

  aboutToAppear(): void {
    this.prefs.init(getContext(this));
    this.themeMode = this.prefs.getThemeMode();
    this.prefs.applyTheme(this.themeMode, getContext(this));
    this.recents = this.prefs.getRecents();
    this.engine.bindRevisionListener((revision: number): void => {
      hilog.info(DOMAIN, TAG, 'docVersion → %{public}d', revision);
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

  // ---------- 状态刷新（05 §6 数据流） ----------

  private onDocVersion(): void {
    this.refreshLayers();
  }

  private refreshLayers(): void {
    this.layers = this.engine.getLayerListSnapshot();
    const active = this.activeLayer();
    if (active !== undefined) {
      this.blendSel = active.blendMode;
      this.layerOpacity = Math.round(active.opacity * 100);
      this.adjBlack = Math.round(active.adjBlack * 255);
      this.adjWhite = Math.round(active.adjWhite * 255);
      this.adjGamma = active.adjGamma;
    } else {
      this.blendSel = 0;
      this.layerOpacity = 100;
    }
    this.refreshThumbs();
  }

  private activeLayer(): LayerDTO | undefined {
    return this.layers.find((l: LayerDTO): boolean => l.isActive);
  }

  private refreshStats(): void {
    const s = this.engine.getStats();
    this.fps = s.fps;
    this.canUndo = s.canUndo;
    this.canRedo = s.canRedo;
  }

  private log(msg: string): void {
    hilog.info(DOMAIN, TAG, '%{public}s', msg);
  }

  private toast(msg: string): void {
    this.getUIContext().getPromptAction().showToast({ message: msg, duration: 2200 });
  }

  // ---------- 缩略图缓存（token 编入 key，异步到位 bump 版本） ----------

  private getThumb(id: number): Object | null {
    const e = this.thumbCache.get(id);
    return (e !== undefined && e.pm !== null) ? e.pm : null;
  }

  private refreshThumbs(): void {
    for (const l of this.layers) {
      const e = this.thumbCache.get(l.id);
      if ((!e || e.token !== l.thumbToken) && !this.thumbsInFlight.has(l.id)) {
        this.thumbsInFlight.add(l.id);
        this.engine.getLayerThumbnail(l.id).then((t: LayerThumb): void => {
          this.thumbsInFlight.delete(l.id);
          // 无论是否空内容都入缓存：撤销回空层时若保留旧图会永远不刷新（M7.5 验证期修复）
          this.thumbCache.set(l.id, { token: l.thumbToken, pm: t.pixelMap });
          this.thumbVersion++;
        }).catch((_e: BusinessError): void => {
          this.thumbsInFlight.delete(l.id);
        });
      }
    }
    const stale: number[] = [];
    this.thumbCache.forEach((_v: ThumbEntry, k: number): void => {
      const exists = this.layers.some((l: LayerDTO): boolean => l.id === k);
      if (!exists) {
        stale.push(k);
      }
    });
    for (const k of stale) {
      this.thumbCache.delete(k);
    }
  }

  // ---------- 视口 ----------

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

  private zoomAt(cxVp: number, cyVp: number, multiplier: number): void {
    const k = this.pxPerVp();
    const ax = this.panX + (cxVp * k) / this.zoom;
    const ay = this.panY + (cyVp * k) / this.zoom;
    const z = this.clampZoom(this.zoom * multiplier);
    this.applyViewport(z, ax - (cxVp * k) / z, ay - (cyVp * k) / z);
  }

  private onWheel(e: AxisEvent): void {
    if (e.scrollStep === undefined || e.scrollStep === 0) {
      return;
    }
    // M2.5 实证：模拟器 scrollStep 恒 3.0 无方向 → 单步按 3.0 归一；方向语义留真机（M16）
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

  private navActive(): boolean {
    return this.tool === 'hand' || (this.spaceHeld && this.tool !== 'hand');
  }

  // ---------- 画布触摸（按工具分发；笔画走 onTouch 原始事件） ----------

  private onTouchEvent(e: TouchEvent): void {
    if (!this.docOpen) {
      return;
    }
    if (e.touches.length < 1 && e.changedTouches.length < 1) {
      return;
    }
    const t: TouchObject = e.changedTouches.length > 0 ? e.changedTouches[0] : e.touches[0];
    const k = this.pxPerVp();
    const docX = this.panX + (t.x * k) / this.zoom;
    const docY = this.panY + (t.y * k) / this.zoom;

    if (this.navActive()) {
      return;  // 抓手/临时抓手：Pan 手势负责
    }
    switch (this.tool) {
      case 'brush':
      case 'eraser':
        this.strokeTouch(e, t, docX, docY);
        break;
      case 'move':
        this.moveTouch(e, docX, docY);
        break;
      case 'crop':
        this.cropTouch(e, docX, docY);
        break;
      case 'wand':
        if (e.type === TouchType.Down) {
          const ok = this.engine.setWandSelection(docX, docY, this.wandTolerance, this.wandContiguous);
          this.hasSelectionFlag = ok;
          this.log(`魔棒: (${docX.toFixed(0)},${docY.toFixed(0)}) tol=${this.wandTolerance} ` +
            `contig=${this.wandContiguous} → ${ok ? '选区已生成' : '失败'}`);
        }
        break;
      case 'rectSelect':
        if (this.altHeld && this.hasSelection) {
          // Alt+拖拽 = 移动选区（M7d/M8a；Up 时单事务提交）
          if (e.type === TouchType.Down) {
            this.moveActive = true;
            this.moveLastX = docX;
            this.moveLastY = docY;
          } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.moveActive) {
            this.moveActive = false;
            const dx = docX - this.moveLastX;
            const dy = docY - this.moveLastY;
            if (this.engine.translateSelection(Math.round(dx), Math.round(dy))) {
              this.log(`选区平移 (${dx.toFixed(0)},${dy.toFixed(0)})`);
            }
          }
          break;
        }
        this.rectSelectTouch(e, docX, docY);
        break;
      case 'eyedropper':
        if (e.type === TouchType.Down) {
          this.pickColor(docX, docY);
        }
        break;
      case 'zoom':
        if (e.type === TouchType.Down) {
          this.zoomAt(t.x, t.y, this.altHeld ? 0.8 : 1.25);
        }
        break;
      default:
        break;  // lasso 占位：M10 提供
    }
  }

  private moveTouch(e: TouchEvent, docX: number, docY: number): void {
    if (e.type === TouchType.Down) {
      this.engine.translateLayer(0, 0, 0, 0);
      this.moveActive = true;
      this.moveLastX = docX;
      this.moveLastY = docY;
    } else if (e.type === TouchType.Move && this.moveActive) {
      this.engine.translateLayer(0, docX - this.moveLastX, docY - this.moveLastY, 1);
      this.moveLastX = docX;
      this.moveLastY = docY;
    } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.moveActive) {
      this.moveActive = false;
      this.engine.translateLayer(0, 0, 0, 2);
    }
  }

  private cropTouch(e: TouchEvent, docX: number, docY: number): void {
    if (e.type === TouchType.Down) {
      this.cropDrag = true;
      this.cropRect = null;
      this.cropX0 = docX;
      this.cropY0 = docY;
    } else if (e.type === TouchType.Move && this.cropDrag) {
      const x = Math.min(this.cropX0, docX);
      const y = Math.min(this.cropY0, docY);
      this.cropRect = [x, y, Math.abs(docX - this.cropX0), Math.abs(docY - this.cropY0)];
    } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.cropDrag) {
      this.cropDrag = false;
      const x = Math.min(this.cropX0, docX);
      const y = Math.min(this.cropY0, docY);
      const w = Math.abs(docX - this.cropX0);
      const h = Math.abs(docY - this.cropY0);
      this.cropRect = w >= 2 && h >= 2 ? [Math.round(x), Math.round(y), Math.round(w), Math.round(h)] : null;
    }
  }

  private get hasSelection(): boolean {
    return this.hasSelectionFlag;
  }

  private strokeTouch(e: TouchEvent, t: TouchObject, docX: number, docY: number): void {
    const pressure: number = (t.pressure !== undefined && t.pressure > 0) ? t.pressure : 1.0;
    if (e.type === TouchType.Down) {
      this.pushBrushSettings();
      this.engine.beginStroke(docX, docY, pressure);
    } else if (e.type === TouchType.Move) {
      this.engine.continueStroke(docX, docY, pressure);
    } else if (e.type === TouchType.Up || e.type === TouchType.Cancel) {
      this.engine.endStroke();
    }
  }

  private rectSelectTouch(e: TouchEvent, docX: number, docY: number): void {
    if (e.type === TouchType.Down) {
      this.rectDrag = true;
      this.rectX0 = docX;
      this.rectY0 = docY;
      this.rectX1 = docX;
      this.rectY1 = docY;
    } else if (e.type === TouchType.Move && this.rectDrag) {
      this.rectX1 = docX;
      this.rectY1 = docY;
    } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.rectDrag) {
      this.rectDrag = false;
      this.rectX1 = docX;
      this.rectY1 = docY;
      const x = Math.min(this.rectX0, this.rectX1);
      const y = Math.min(this.rectY0, this.rectY1);
      const w = Math.abs(this.rectX1 - this.rectX0);
      const h = Math.abs(this.rectY1 - this.rectY0);
      if (w < 2 || h < 2) {
        this.engine.clearSelection();
        this.hasSelectionFlag = false;
        this.log('矩形选区：拖距过小，已取消选区');
        return;
      }
      const ok = this.engine.setRectSelection(x, y, w, h);
      this.hasSelectionFlag = ok;
      this.log(`矩形选区: (${x.toFixed(0)},${y.toFixed(0)}) ${w.toFixed(0)}×${h.toFixed(0)} → ${ok}`);
    }
  }

  private pickColor(docX: number, docY: number): void {
    this.engine.samplePixel(docX, docY).then((d): void => {
      if (!d.ok) {
        this.toast('该点无像素（透明）');
        return;
      }
      const hex = this.rgbToHex(d.red, d.green, d.blue);
      this.brushColor = hex;
      this.toast(`取色 ${hex}`);
      this.log(`吸管: (${docX.toFixed(0)},${docY.toFixed(0)}) → ${hex} a=${d.alpha}`);
    }).catch((e: BusinessError): void => {
      this.log(`取色失败: ${e.message}`);
    });
  }

  private rgbToHex(r: number, g: number, b: number): string {
    const h = (v: number): string => v.toString(16).padStart(2, '0');
    return `#${h(r)}${h(g)}${h(b)}`.toUpperCase();
  }

  // ---------- 画布与文档动作 ----------

  private onCanvasReady(): void {
    try {
      const id: string = this.xcController.getXComponentSurfaceId();
      this.log(`surfaceId=${id}`);
      const ok: boolean = this.engine.createSurface(id);
      this.log(`创建渲染表面: ${ok ? '成功' : '失败'}`);
    } catch (e) {
      const err: BusinessError = e as BusinessError;
      this.log(`画布初始化失败: ${err.code} ${err.message}`);
    }
  }

  private docName(): string {
    if (this.saveUri !== '') {
      const seg = this.saveUri.split('/');
      return decodeURIComponent(seg[seg.length - 1]);
    }
    return '未命名';
  }

  private updateTitle(): void {
    window.getLastWindow(getContext(this)).then((w: window.Window): void => {
      w.setWindowTitle(`${this.docName()} - Montage`).catch((): void => {
        // 自由窗口标题失败不致命
      });
    }).catch((): void => {
      // getLastWindow 失败不致命
    });
  }

  private afterDocChanged(): void {
    this.docOpen = true;
    this.fittedOnce = false;
    this.maskTarget = false;
    this.engine.setBrushTarget(false);
    this.refreshLayers();
    this.updateTitle();
    this.tryFit();
  }

  private doNewDocument(): void {
    const ok: boolean = this.engine.newDocument(4096, 4096);
    this.docW = 4096;
    this.docH = 4096;
    this.saveUri = '';
    this.log(`新建空文档 4096x4096: ${ok ? '成功' : '失败'}`);
    this.afterDocChanged();
  }

  private doCloseDocument(): void {
    const ok = this.engine.closeDocument();
    this.docOpen = false;
    this.saveUri = '';
    this.layers = [];
    this.thumbCache.clear();
    this.log(`关闭文档: ${ok ? '成功' : '失败'}`);
    this.updateTitle();
  }

  private onOpenImage(): void {
    const opt = new picker.DocumentSelectOptions();
    opt.maxSelectNumber = 1;
    // M2.5 实证：fileSuffixFilters 非图片后缀列表为空 → 不过滤 + 扩展名分发
    opt.defaultFilePathUri = 'file://docs/storage/Users/currentUser/Download';
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
    this.importing = true;
    this.busyText = 'PSD 解析中…';
    this.log(`打开 PSD fd=${f.fd}`);
    this.engine.openPsdFile(f.fd).then((d: PsdOpenData): void => {
      this.importing = false;
      this.busyText = '';
      this.docW = d.width;
      this.docH = d.height;
      this.afterDocChanged();
      this.log(`PSD 打开完成 ${d.width}x${d.height}，${d.notes.length} 条提示`);
      for (let i = 0; i < d.notes.length && i < 8; i++) {
        this.log(`提示: ${d.notes[i]}`);
      }
    }).catch((e: BusinessError): void => {
      this.importing = false;
      this.busyText = '';
      this.log(`PSD 打开失败: ${e.message}`);
    });
  }

  private openProjectByUri(uri: string): void {
    const f = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    this.importing = true;
    this.busyText = '工程打开中…';
    this.engine.openProject(f.fd).then((d: OpenProjectData): void => {
      this.importing = false;
      this.busyText = '';
      this.docW = d.width;
      this.docH = d.height;
      this.saveUri = uri;  // 已关联包：保存直达覆写
      this.afterDocChanged();
      const seg = uri.split('/');
      this.recents = this.prefs.addRecent(decodeURIComponent(seg[seg.length - 1]), uri);
      this.log(`工程已打开 ${d.width}x${d.height}，${d.layers} 层`);
    }).catch((e: BusinessError): void => {
      this.importing = false;
      this.busyText = '';
      this.log(`工程打开失败: ${e.message}`);
      this.recents = this.prefs.removeRecent(uri);
      this.toast('工程打开失败，已从最近列表移除');
    });
  }

  private openRecent(uri: string): void {
    try {
      fs.accessSync(uri);
      this.openProjectByUri(uri);
    } catch (e) {
      this.recents = this.prefs.removeRecent(uri);
      this.toast('文件不存在，已从最近列表移除');
    }
  }

  private openByUri(uri: string): void {
    const f = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    this.importing = true;
    this.engine.openFileFromFd(f.fd).then((d: OpenFileData): void => {
      // fd 所有权已移交 native（含失败路径）
      this.importing = false;
      if (d.width > this.docW || d.height > this.docH) {
        this.docW = Math.max(this.docW, d.width);
        this.docH = Math.max(this.docH, d.height);
      }
      this.log(`图层已添加 ${d.width}x${d.height} (id=${d.layerId})`);
      if (!this.docOpen) {
        this.afterDocChanged();
      } else {
        this.refreshLayers();
      }
    }).catch((e: BusinessError): void => {
      this.importing = false;
      this.log(`导入失败: ${e.message}`);
    });
  }

  private doSaveProject(saveAs: boolean): void {
    if (!this.docOpen) {
      this.toast('无文档可保存');
      return;
    }
    if (this.busyText !== '') {
      return;
    }
    if (!saveAs && this.saveUri !== '') {
      this.writeProjectTo(this.saveUri);
      return;
    }
    const opt = new picker.DocumentSaveOptions();
    opt.newFileNames = [`${this.docName().replace(/\.montage$/, '') || '未命名'}.montage`];
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
    this.busyText = '保存中…';
    // TRUNC 清旧包再整包重写（fd 移交 native）
    const f = fs.openSync(uri, fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
    this.log(`保存工程 fd=${f.fd}`);
    this.engine.saveProject(f.fd).then((d: SaveProjectData): void => {
      this.busyText = '';
      const seg = uri.split('/');
      this.recents = this.prefs.addRecent(decodeURIComponent(seg[seg.length - 1]), uri);
      this.updateTitle();
      this.toast(`已保存（${d.layers} 层）`);
      this.log(`工程已保存：${d.layers} 层，${d.bytes} 字节`);
    }).catch((e: BusinessError): void => {
      this.busyText = '';
      this.log(`保存失败: ${e.message}`);
      this.toast('保存失败');
    });
  }

  private doExport(format: string, quality: number): void {
    if (!this.docOpen) {
      this.toast('无文档可导出');
      return;
    }
    const ext = format === 'png' ? 'png' : (format === 'psd' ? 'psd' : 'jpg');
    const opt = new picker.DocumentSaveOptions();
    opt.newFileNames = [`${this.docName().replace(/\.(montage|png|jpg|jpeg)$/i, '') || '未命名'}.${ext}`];
    const dp = new picker.DocumentViewPicker();
    dp.save(opt).then((uris: string[]): void => {
      if (uris.length < 1) {
        return;
      }
      this.runExport(uris[0], format, quality);
    }).catch((e: BusinessError): void => {
      this.log(`导出选择失败: ${e.code} ${e.message}`);
    });
  }

  private runExport(uri: string, format: string, quality: number): void {
    this.showExport = false;
    this.busyText = '扁平合成中…';
    const t0 = Date.now();
    const f = fs.openSync(uri, fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
    if (format === 'psd') {
      // PSD：层/蒙版/剪贴非破坏导出（M8b）
      this.engine.exportPsd(f.fd).then((d): void => {
        this.busyText = '';
        this.log(`PSD 导出 ${d.layers} 层 ${d.bytes} 字节（${Date.now() - t0}ms）→ ${uri}`);
        for (let i = 0; i < d.notes.length && i < 5; i++) {
          this.log(`PSD 提示: ${d.notes[i]}`);
        }
        this.toast(`PSD 已导出（${d.layers} 层）`);
      }).catch((e: BusinessError): void => {
        this.busyText = '';
        this.log(`PSD 导出失败: ${e.message}`);
        this.toast('PSD 导出失败');
      });
      return;
    }
    if (format === 'png') {
      // PNG：引擎 PngRowEncoder 流式直写 fd（绕系统编解码器，M4c 教训）
      this.engine.exportPng(f.fd).then((d): void => {
        this.busyText = '';
        this.log(`PNG 导出 ${d.width}x${d.height} ${d.bytes} 字节（${Date.now() - t0}ms）→ ${uri}`);
        this.toast(`PNG 已导出（${(d.bytes / 1048576).toFixed(1)}MB）`);
      }).catch((e: BusinessError): void => {
        this.busyText = '';
        this.log(`PNG 导出失败: ${e.message}`);
        this.toast('PNG 导出失败');
      });
    } else {
      // JPEG：引擎扁平化 → PixelMap → ImagePacker（V-01）
      this.engine.flattenToPixelMap().then((d): void => {
        if (d.pixelMap === null) {
          this.busyText = '';
          this.toast('导出失败：空文档');
          fs.closeSync(f.fd);
          return;
        }
        const pm = d.pixelMap as image.PixelMap;
        const packer = image.createImagePacker();
        packer.packToFile(pm, f.fd, { format: 'image/jpeg', quality: quality }).then((): void => {
          this.busyText = '';
          pm.release();
          this.log(`JPEG 导出 q=${quality}（${Date.now() - t0}ms）→ ${uri}`);
          this.toast('JPEG 已导出');
        }).catch((e: BusinessError): void => {
          this.busyText = '';
          pm.release();
          this.log(`JPEG 编码失败: ${e.message}`);
          this.toast('JPEG 导出失败');
        });
      }).catch((e: BusinessError): void => {
        this.busyText = '';
        this.log(`扁平化失败: ${e.message}`);
        this.toast('导出失败');
      });
    }
  }

  // ---------- 工具与画笔 ----------

  private pickTool(id: string): void {
    if (id === this.tool) {
      return;
    }
    if (id === 'brush' || id === 'eraser') {
      this.maskTarget = false;  // 换笔即退出蒙版目标（安全默认）
      this.engine.setBrushTarget(false);
    }
    if (this.tool === 'crop') {
      this.cropRect = null;  // 换工具即丢弃未提交裁剪框
    }
    this.tool = id;
    this.log(`工具 → ${id}`);
  }

  private hexToRgb(hex: string): number[] {
    const r = parseInt(hex.slice(1, 3), 16) / 255;
    const g = parseInt(hex.slice(3, 5), 16) / 255;
    const b = parseInt(hex.slice(5, 7), 16) / 255;
    return [r, g, b];
  }

  private pushBrushSettings(): void {
    const rgb = this.hexToRgb(this.brushColor);
    this.engine.setBrushSettings(this.brushSize, this.brushHardness / 100, this.brushOpacity / 100,
      rgb[0], rgb[1], rgb[2], this.tool === 'eraser');
  }

  private activeLayerHasMask(): boolean {
    const a = this.activeLayer();
    return a !== undefined && a.hasMask;
  }

  private onToggleMaskTarget(): void {
    if (!this.activeLayerHasMask()) {
      this.toast('当前图层无蒙版');
      return;
    }
    this.maskTarget = !this.maskTarget;
    if (this.maskTarget && this.tool === 'eraser') {
      this.tool = 'brush';
    }
    this.engine.setBrushTarget(this.maskTarget);
    this.log(`落笔目标: ${this.maskTarget ? '蒙版' : '像素'}`);
  }

  // ---------- 命令表（菜单/快捷键唯一入口） ----------

  private runCommand(id: string): void {
    hilog.info(DOMAIN, TAG, 'cmd %{public}s', id);
    switch (id) {
      case 'file.new':
        this.doNewDocument();
        break;
      case 'file.open':
        this.onOpenImage();
        break;
      case 'file.save':
        this.doSaveProject(false);
        break;
      case 'file.saveAs':
        this.doSaveProject(true);
        break;
      case 'file.export':
        this.showExport = true;
        break;
      case 'file.close':
        this.doCloseDocument();
        break;
      case 'edit.undo':
        if (this.engine.undo()) {
          this.log('撤销');
        }
        break;
      case 'edit.redo':
        if (this.engine.redo()) {
          this.log('重做');
        }
        break;
      case 'layer.new':
        this.engine.addLayer();
        break;
      case 'layer.rename': {
        const a = this.activeLayer();
        if (a !== undefined) {
          this.renameText = a.name;
          this.showRename = true;
        }
        break;
      }
      case 'layer.delete': {
        const a = this.activeLayer();
        if (a !== undefined) {
          this.engine.removeLayer(a.id);
        }
        break;
      }
      case 'layer.moveUp':
      case 'layer.moveDown': {
        const a = this.activeLayer();
        if (a !== undefined) {
          this.onMoveLayer(a, id === 'layer.moveUp');
        }
        break;
      }
      case 'layer.clipToggle': {
        const a = this.activeLayer();
        if (a !== undefined) {
          this.engine.setLayerClipping(a.id, !a.clipping);
        }
        break;
      }
      case 'layer.maskAdd': {
        const a = this.activeLayer();
        if (a !== undefined) {
          this.engine.addLayerMask(a.id);
        }
        break;
      }
      case 'layer.maskEnableToggle': {
        const a = this.activeLayer();
        if (a !== undefined) {
          this.engine.setLayerMaskEnabled(a.id, !a.maskEnabled);
        }
        break;
      }
      case 'layer.adjustAdd':
        this.engine.addAdjustmentLayer();
        break;
      case 'select.deselect':
        this.engine.clearSelection();
        this.hasSelectionFlag = false;
        this.log('选区已取消');
        break;
      case 'image.rotate90cw':
        this.docOp((): boolean => this.engine.rotateDocument90(true), '旋转 90° 顺时针');
        break;
      case 'image.rotate90ccw':
        this.docOp((): boolean => this.engine.rotateDocument90(false), '旋转 90° 逆时针');
        break;
      case 'image.rotate180':
        this.docOp((): boolean => {
          const a = this.engine.rotateDocument90(true);
          return a && this.engine.rotateDocument90(true);
        }, '旋转 180°');
        break;
      case 'image.flipH':
        this.docOp((): boolean => this.engine.flipDocument(true), '水平翻转');
        break;
      case 'image.flipV':
        this.docOp((): boolean => this.engine.flipDocument(false), '垂直翻转');
        break;
      case 'image.trim':
        this.docOp((): boolean => this.engine.trimCanvas(), '裁切');
        break;
      case 'image.revealAll':
        this.docOp((): boolean => this.engine.revealAll(), '显示全部');
        break;
      case 'crop.commit':
        this.commitCrop();
        break;
      case 'crop.cancel':
        this.cropRect = null;
        break;
      case 'view.zoomIn':
        this.doZoomStep(1.25);
        break;
      case 'view.zoomOut':
        this.doZoomStep(0.8);
        break;
      case 'view.zoom100':
        this.doZoom100();
        break;
      case 'view.fit':
        this.tryFit();
        break;
      case 'window.toggleLayers':
        this.layersVisible = !this.layersVisible;
        break;
      case 'help.about':
        this.showAbout = true;
        break;
      case 'help.shortcuts':
        this.showShortcuts = true;
        break;
      case 'help.theme':
        this.showTheme = true;
        break;
      case 'theme.system':
      case 'theme.light':
      case 'theme.dark':
        this.applyTheme(id.slice(6) as ThemeMode);
        break;
      default:
        // 占位命令（M8/M10/M14+）：提示并登记
        this.toast('该功能在后续里程碑提供');
        this.log(`命令占位: ${id}`);
        break;
    }
  }

  private applyTheme(mode: ThemeMode): void {
    this.themeMode = mode;
    this.prefs.setThemeMode(mode);
    this.prefs.applyTheme(mode, getContext(this));
    this.showTheme = false;
  }

  /** M8a 文档级操作统一包装：执行 + 刷文档尺寸 + 清选区镜像 + 提示。 */
  private docOp(op: () => boolean, label: string): void {
    if (!this.docOpen) {
      return;
    }
    if (op()) {
      this.hasSelectionFlag = false;
      this.cropRect = null;
      this.refreshDocMeta();
      this.toast(label + '完成');
      this.log(`${label} 完成`);
    }
  }

  private refreshDocMeta(): void {
    const s = this.engine.getStats();
    if (s.docWidth > 0) {
      this.docW = s.docWidth;
      this.docH = s.docHeight;
      this.fittedOnce = false;
      this.tryFit();
    }
  }

  private commitCrop(): void {
    const r = this.cropRect;
    if (r === null || !this.docOpen) {
      return;
    }
    if (this.engine.resizeCanvas(r[0], r[1], r[2], r[3])) {
      this.hasSelectionFlag = false;
      this.cropRect = null;
      this.refreshDocMeta();
      this.toast(`画布已裁剪 ${r[2]}×${r[3]}`);
      this.log(`裁剪: (${r[0]},${r[1]}) ${r[2]}×${r[3]}`);
    }
  }

  private buildMenuGroups(): MenuGroupVM[] {
    const d = this.docOpen;
    const file: MenuItemVM[] = [
      leafItem('file.new', '新建', 'Ctrl+N', true),
      leafItem('file.open', '打开…', 'Ctrl+O', true),
      leafItem('file.save', '保存', 'Ctrl+S', d),
      leafItem('file.saveAs', '另存为…', '', d),
      subItem('file.exportSub', '导出', d, [
        leafItem('file.export', '导出图像（PNG / JPEG）…', '', d),
      ]),
      leafItem('file.close', '关闭文档', '', d),
    ];
    const edit: MenuItemVM[] = [
      leafItem('edit.undo', '撤销', 'Ctrl+Z', this.canUndo),
      leafItem('edit.redo', '重做', 'Ctrl+Shift+Z', this.canRedo),
      leafItem('edit.cut', '剪切', 'Ctrl+X', false),
      leafItem('edit.copy', '拷贝', 'Ctrl+C', false),
      leafItem('edit.paste', '粘贴', 'Ctrl+V', false),
    ];
    const imageMenu: MenuItemVM[] = [
      leafItem('image.canvasSize', '画布大小…', '', false),
      leafItem('image.imageSize', '图像大小…', '', false),
      leafItem('image.rotate90cw', '顺时针旋转 90°', '', d),
      leafItem('image.rotate90ccw', '逆时针旋转 90°', '', d),
      leafItem('image.rotate180', '旋转 180°', '', d),
      leafItem('image.flipH', '水平翻转画布', '', d),
      leafItem('image.flipV', '垂直翻转画布', '', d),
      leafItem('image.trim', '裁切（基于像素）', '', d),
      leafItem('image.revealAll', '显示全部', '', d),
    ];
    const layer: MenuItemVM[] = [
      leafItem('layer.new', '新建图层', '', d),
      leafItem('layer.rename', '重命名…', '', this.activeLayer() !== undefined),
      leafItem('layer.delete', '删除图层', '', this.activeLayer() !== undefined),
      leafItem('layer.moveUp', '上移一层', '', d),
      leafItem('layer.moveDown', '下移一层', '', d),
      leafItem('layer.clipToggle', '创建/释放剪贴蒙版', '', d),
      leafItem('layer.maskAdd', '添加图层蒙版（显示全部）', '', d),
      leafItem('layer.maskEnableToggle', '启用/停用蒙版', '', d && this.activeLayerHasMask()),
      leafItem('layer.adjustAdd', '新建调整图层（色阶）', '', d),
    ];
    const select: MenuItemVM[] = [
      leafItem('select.all', '全部选择', 'Ctrl+A', false),
      leafItem('select.deselect', '取消选择', 'Ctrl+D', d),
      leafItem('select.inverse', '反选', 'Ctrl+Shift+I', false),
    ];
    const view: MenuItemVM[] = [
      leafItem('view.zoomIn', '放大', 'Ctrl+=', d),
      leafItem('view.zoomOut', '缩小', 'Ctrl+-', d),
      leafItem('view.zoom100', '100%', 'Ctrl+1', d),
      leafItem('view.fit', '适配画布', 'Ctrl+0', d),
    ];
    const win: MenuItemVM[] = [
      leafItem('window.toggleLayers', '图层面板', '', true, this.layersVisible),
    ];
    const help: MenuItemVM[] = [
      subItem('help.themeSub', '主题', true, [
        leafItem('theme.system', '跟随系统', '', true, this.themeMode === 'system'),
        leafItem('theme.light', '浅色', '', true, this.themeMode === 'light'),
        leafItem('theme.dark', '深色', '', true, this.themeMode === 'dark'),
      ]),
      leafItem('help.shortcuts', '快捷键表', '', true),
      leafItem('help.about', '关于 Montage', '', true),
    ];
    return [
      { id: 'file', label: '文件', items: file },
      { id: 'edit', label: '编辑', items: edit },
      { id: 'image', label: '图像', items: imageMenu },
      { id: 'layer', label: '图层', items: layer },
      { id: 'select', label: '选择', items: select },
      { id: 'view', label: '视图', items: view },
      { id: 'window', label: '窗口', items: win },
      { id: 'help', label: '帮助', items: help },
    ];
  }

  private buildTools(): ToolVM[] {
    return [
      { id: 'move', label: '移动', symbol: $r('sys.symbol.move'), enabled: true },
      { id: 'rectSelect', label: '选框', symbol: $r('sys.symbol.Inverse_selection'), enabled: true },
      { id: 'lasso', label: '套索', symbol: $r('sys.symbol.smart_selection_area'), enabled: true },
      { id: 'wand', label: '魔棒', symbol: $r('sys.symbol.wand_and_stars'), enabled: true },
      { id: 'crop', label: '裁剪', symbol: $r('sys.symbol.crop'), enabled: true },
      { id: '-', label: '', symbol: $r('sys.symbol.minus'), enabled: false },
      { id: 'eyedropper', label: '吸管', symbol: $r('sys.symbol.eyedropper'), enabled: true },
      { id: 'brush', label: '画笔', symbol: $r('sys.symbol.paintbrush_pointed'), enabled: true },
      { id: 'eraser', label: '橡皮', symbol: $r('sys.symbol.eraser_line'), enabled: true },
      { id: '-', label: '', symbol: $r('sys.symbol.minus'), enabled: false },
      { id: 'hand', label: '抓手', symbol: $r('sys.symbol.hand'), enabled: true },
      { id: 'zoom', label: '缩放', symbol: $r('sys.symbol.magnifyingglass'), enabled: true },
    ];
  }

  private toolLabel(id: string): string {
    for (const t of this.buildTools()) {
      if (t.id === id) {
        return t.label;
      }
    }
    return '';
  }

  // ---------- 快捷键（05 §3；修饰键经 keyCode 维护 held 态——UI KeyEvent 无 ctrlKey 字段，2026-10-07 核验） ----------

  private onKey(e: KeyEvent): void {
    if (e.type === KeyType.Up) {
      if (e.keyCode === KC_SPACE) {
        this.spaceHeld = false;
      } else if (e.keyCode === KC_CTRL_L || e.keyCode === KC_CTRL_R) {
        this.ctrlHeld = false;
      } else if (e.keyCode === KC_SHIFT_L || e.keyCode === KC_SHIFT_R) {
        this.shiftHeld = false;
      } else if (e.keyCode === KC_ALT_L || e.keyCode === KC_ALT_R) {
        this.altHeld = false;
      }
      return;
    }
    if (e.type !== KeyType.Down) {
      return;
    }
    // 修饰键状态维护
    if (e.keyCode === KC_SPACE) {
      this.spaceHeld = true;
      return;
    } else if (e.keyCode === KC_CTRL_L || e.keyCode === KC_CTRL_R) {
      this.ctrlHeld = true;
      return;
    } else if (e.keyCode === KC_SHIFT_L || e.keyCode === KC_SHIFT_R) {
      this.shiftHeld = true;
      return;
    } else if (e.keyCode === KC_ALT_L || e.keyCode === KC_ALT_R) {
      this.altHeld = true;
      return;
    }
    // 对话框打开时：Esc 关闭，其余不抢
    if (this.showExport || this.showRename || this.showShortcuts || this.showAbout || this.showTheme) {
      if (e.keyCode === KC_ESC) {
        this.showExport = false;
        this.showRename = false;
        this.showShortcuts = false;
        this.showAbout = false;
        this.showTheme = false;
      }
      return;
    }
    // 裁剪待提交：回车提交 / Esc 取消
    if (this.cropRect !== null) {
      if (e.keyCode === KC_ENTER) {
        this.commitCrop();
      } else if (e.keyCode === KC_ESC) {
        this.cropRect = null;
      }
      return;
    }
    // 移动工具方向键微调（Shift=10px；一次按键 = 一步事务）
    if (this.tool === 'move' && this.docOpen) {
      const nudge: number = this.shiftHeld ? 10 : 1;
      let ndx: number = 0;
      let ndy: number = 0;
      if (e.keyCode === KC_LEFT) {
        ndx = 0 - nudge;
      } else if (e.keyCode === KC_RIGHT) {
        ndx = nudge;
      } else if (e.keyCode === KC_UP) {
        ndy = 0 - nudge;
      } else if (e.keyCode === KC_DOWN) {
        ndy = nudge;
      }
      if (ndx !== 0 || ndy !== 0) {
        this.engine.translateLayer(0, 0, 0, 0);
        this.engine.translateLayer(0, ndx, ndy, 1);
        this.engine.translateLayer(0, 0, 0, 2);
        return;
      }
    }
    const code = e.keyCode;
    if (this.ctrlHeld) {
      switch (code) {
        case KC_Z:
          this.runCommand(this.shiftHeld ? 'edit.redo' : 'edit.undo');
          break;
        case KC_Y:
          this.runCommand('edit.redo');
          break;
        case KC_N:
          this.runCommand('file.new');
          break;
        case KC_O:
          this.runCommand('file.open');
          break;
        case KC_S:
          this.runCommand('file.save');
          break;
        case KC_NUM0:
          this.runCommand('view.fit');
          break;
        case KC_NUM1:
          this.runCommand('view.zoom100');
          break;
        case KC_EQUALS:
          this.runCommand('view.zoomIn');
          break;
        case KC_MINUS:
          this.runCommand('view.zoomOut');
          break;
        case KC_D:
          this.runCommand('select.deselect');
          break;
        default:
          break;
      }
      return;
    }
    // 单键
    switch (code) {
      case KC_V:
        this.pickTool('move');
        break;
      case KC_M:
        this.pickTool('rectSelect');
        break;
      case KC_L:
        this.pickTool('lasso');
        break;
      case KC_W:
        this.pickTool('wand');
        break;
      case KC_C:
        this.pickTool('crop');
        break;
      case KC_I:
        this.pickTool('eyedropper');
        break;
      case KC_B:
        this.pickTool('brush');
        break;
      case KC_E:
        this.pickTool('eraser');
        break;
      case KC_H:
        this.pickTool('hand');
        break;
      case KC_Z:
        this.pickTool('zoom');
        break;
      case KC_LBRACKET:
        this.brushSize = Math.max(2, this.brushSize - 4);
        break;
      case KC_RBRACKET:
        this.brushSize = Math.min(400, this.brushSize + 4);
        break;
      default:
        break;
    }
  }

  // ---------- 图层动作 ----------

  private onMoveLayer(l: LayerDTO, up: boolean): void {
    const idx = this.layers.findIndex((x: LayerDTO): boolean => x.id === l.id);
    const target = up ? idx + 1 : idx - 1;  // 引擎栈序：上移=idx+1
    if (target < 0 || target >= this.layers.length) {
      return;
    }
    this.engine.reorderLayer(l.id, target);
  }

  private commitRename(): void {
    const a = this.activeLayer();
    const name = this.renameText.trim();
    if (a !== undefined && name !== '') {
      this.engine.renameLayer(a.id, name);
      this.log(`图层重命名 → ${name}`);
    }
    this.showRename = false;
  }

  // ---------- 菜单栏（内联 @Builder：@Prop 对象数组同步失效实证，2026-10-07；无参体内直读免疫） ----------

  @Builder
  menuLeafRow(it: MenuItemVM) {
    MenuItem({
      content: it.label,
      labelInfo: it.checked ? '✓' : it.shortcut,
    })
      .enabled(it.enabled)
      .onClick((): void => {
        this.runCommand(it.id);
      })
  }

  @Builder
  menuSub(p: MenuItemVM) {
    Menu() {
      ForEach(p.children, (c: MenuItemVM) => {
        this.menuLeafRow(c)
      }, (c: MenuItemVM): string => `${c.id}|${c.enabled}|${c.checked}`)
    }
  }

  @Builder
  menuGroup(g: MenuGroupVM) {
    Menu() {
      ForEach(g.items, (it: MenuItemVM) => {
        if (it.children.length > 0) {
          MenuItem({
            content: it.label,
            builder: (): void => this.menuSub(it),
          })
            .enabled(it.enabled)
        } else {
          this.menuLeafRow(it)
        }
      }, (it: MenuItemVM): string => `${it.id}|${it.enabled}|${it.checked}`)
    }
  }

  @Builder
  menuBar() {
    Row({ space: 2 }) {
      ForEach(this.buildMenuGroups(), (g: MenuGroupVM) => {
        Text(g.label)
          .fontSize(13)
          .fontColor($r('app.color.text_primary'))
          .padding({ left: 10, right: 10, top: 4, bottom: 4 })
          .borderRadius(6)
          .bindMenu(this.menuGroup(g))
      }, (g: MenuGroupVM): string =>
        `${g.id}|${g.items.length}|${this.docOpen ? 1 : 0}|${this.canUndo ? 1 : 0}|` +
        `${this.canRedo ? 1 : 0}|${this.layersVisible ? 1 : 0}|${this.themeMode}|` +
        `${this.activeLayer() !== undefined ? 1 : 0}${this.activeLayerHasMask() ? 1 : 0}`)
    }
    .width('100%')
    .height(36)
    .padding({ left: 8, right: 8 })
    .backgroundColor($r('app.color.bar_background'))
    .border({ width: { bottom: 1 }, color: { bottom: $r('app.color.divider') } })
  }

  build() {
    Stack() {
      Column() {
        this.menuBar()
        OptionsBar({
          tool: this.tool,
          toolLabel: this.toolLabel(this.tool),
          docOpen: this.docOpen,
          brushSize: $brushSize,
          brushHardness: $brushHardness,
          brushOpacity: $brushOpacity,
          wandTolerance: $wandTolerance,
          wandContiguous: $wandContiguous,
          brushColor: this.brushColor,
          maskTarget: this.maskTarget,
          hasMaskOnActive: this.activeLayerHasMask(),
          cropPending: this.cropRect !== null,
          onToggleMaskTarget: (): void => this.onToggleMaskTarget(),
          onColorPick: (hex: string): void => {
            this.brushColor = hex;
          },
          onZoom: (cmd: string): void => this.runCommand(`view.${cmd === 'fit' ? 'fit' : cmd === '100' ? 'zoom100' : cmd === 'in' ? 'zoomIn' : 'zoomOut'}`),
          onCropCommand: (cmd: string): void => this.runCommand(`crop.${cmd}`),
        })
        Row() {
          Toolbox({ tools: this.buildTools(), activeTool: this.tool,
            onPick: (id: string): void => this.pickTool(id) })

          // 画布区
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
            // 矩形选区拖拽预览（GL 蚂蚁线在提交后接管）
            if (this.rectDrag) {
              Column()
                .position({
                  x: (Math.min(this.rectX0, this.rectX1) - this.panX) * this.zoom / this.pxPerVp(),
                  y: (Math.min(this.rectY0, this.rectY1) - this.panY) * this.zoom / this.pxPerVp()
                })
                .width(Math.abs(this.rectX1 - this.rectX0) * this.zoom / this.pxPerVp())
                .height(Math.abs(this.rectY1 - this.rectY0) * this.zoom / this.pxPerVp())
                .border({ width: 1, color: '#FFFFFF' })
                .backgroundColor('#22000000')
            }
            // 裁剪框（拖拽中 + 待提交常显）
            if (this.cropRect !== null) {
              Column()
                .position({
                  x: (this.cropRect[0] - this.panX) * this.zoom / this.pxPerVp(),
                  y: (this.cropRect[1] - this.panY) * this.zoom / this.pxPerVp()
                })
                .width(this.cropRect[2] * this.zoom / this.pxPerVp())
                .height(this.cropRect[3] * this.zoom / this.pxPerVp())
                .border({ width: 1.5, color: '#00E5FF' })
                .backgroundColor('#14000000')
            }
            if (!this.docOpen) {
              WelcomeView({
                recents: this.recents,
                onNew: (): void => this.doNewDocument(),
                onOpen: (): void => this.onOpenImage(),
                onOpenRecent: (uri: string): void => this.openRecent(uri),
              })
            }
          }
          .layoutWeight(1)
          .height('100%')
          .alignContent(Alignment.TopStart)
          .focusable(true)
          .focusOnTouch(true)
          .backgroundColor($r('app.color.canvas_backdrop'))
          .onAreaChange((_o: Area, n: Area): void => {
            this.canvasVpW = Number(n.width);
            this.canvasVpH = Number(n.height);
            if (this.docOpen && !this.fittedOnce) {
              this.tryFit();
            }
          })
          .gesture(this.navActive() ?
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
            : PinchGesture({ fingers: 2, distance: 1 })
              .onActionStart((e: PinchGestureEvent): void => this.onPinchStart(e))
              .onActionUpdate((e: PinchGestureEvent): void => this.onPinchUpdate(e))
              .onActionEnd((): void => this.onPinchEnd())
              .onActionCancel((): void => this.onPinchEnd()))

          // 右侧面板坞
          if (this.layersVisible) {
            Column() {
              LayersPanel({
                layers: $layers,
                layerOpacity: $layerOpacity,
                blendSel: $blendSel,
                adjBlack: $adjBlack,
                adjWhite: $adjWhite,
                adjGamma: $adjGamma,
                thumbVersion: this.thumbVersion,
                docOpen: this.docOpen,
                getThumb: (id: number): Object | null => this.getThumb(id),
                onSelect: (id: number): void => {
                  this.engine.selectLayer(id);
                },
                onToggleVisible: (l: LayerDTO): void => {
                  this.engine.setLayerVisible(l.id, !l.visible);
                },
                onToggleMaskEnabled: (l: LayerDTO): void => {
                  this.engine.setLayerMaskEnabled(l.id, !l.maskEnabled);
                },
                onToggleClipping: (l: LayerDTO): void => {
                  this.engine.setLayerClipping(l.id, !l.clipping);
                },
                onMove: (l: LayerDTO, up: boolean): void => this.onMoveLayer(l, up),
                onDelete: (): void => this.runCommand('layer.delete'),
                onAdd: (): void => this.runCommand('layer.new'),
                onAddMask: (): void => this.runCommand('layer.maskAdd'),
                onAddAdjustment: (): void => this.runCommand('layer.adjustAdd'),
                onRequestRename: (_l: LayerDTO): void => this.runCommand('layer.rename'),
                onOpacityChange: (v: number): void => {
                  const a = this.activeLayer();
                  if (a !== undefined) {
                    this.engine.setLayerOpacity(a.id, v);
                  }
                },
                onBlendChange: (idx: number): void => {
                  const a = this.activeLayer();
                  if (a !== undefined) {
                    this.engine.setLayerBlendMode(a.id, idx);
                  }
                },
                onAdjustChange: (b: number, w: number, g: number): void => {
                  const a = this.activeLayer();
                  if (a !== undefined && a.isAdjustment) {
                    this.engine.setAdjustmentParams(a.id, b, w, g);
                  }
                },
              })
            }
            .width(280)
            .border({ width: { left: 1 }, color: { left: $r('app.color.divider') } })
          }
        }
        .layoutWeight(1)
        .alignItems(VerticalAlign.Top)

        StatusBar({
          docOpen: this.docOpen,
          docW: this.docW,
          docH: this.docH,
          zoom: this.zoom,
          activeLayerName: this.activeLayer()?.name ?? '',
          busyText: this.busyText,
          importing: this.importing,
          importProgress: this.importProgress,
          fps: this.fps,
        })
      }
      .width('100%')
      .height('100%')
      .backgroundColor($r('app.color.page_background'))
      .focusable(true)
      .onKeyEvent((e: KeyEvent): void => this.onKey(e))

      // ---------- 模态对话框层（05 §5：Stack 遮罩 + 居中卡片） ----------
      if (this.showExport) {
        ExportDialog({
          docW: this.docW,
          docH: this.docH,
          onConfirm: (format: string, quality: number): void => this.doExport(format, quality),
          onCancel: (): void => {
            this.showExport = false;
          },
        })
      }
      if (this.showRename) {
        Stack() {
          Column()
            .width('100%')
            .height('100%')
            .backgroundColor('#66000000')
            .onClick((): void => {
              this.showRename = false;
            })
          Column({ space: 14 }) {
            Text('重命名图层').fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            TextInput({ text: this.renameText })
              .height(40)
              .width('100%')
              .fontSize(14)
              .onChange((v: string): void => {
                this.renameText = v;
              })
              .defaultFocus(true)
            Row({ space: 12 }) {
              Button('取消').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => {
                  this.showRename = false;
                })
              Button('确定').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => this.commitRename())
            }
            .justifyContent(FlexAlign.End)
            .width('100%')
          }
          .padding(20)
          .borderRadius(14)
          .backgroundColor($r('app.color.surface'))
          .border({ width: 1, color: $r('app.color.border') })
          .width(360)
        }
        .width('100%')
        .height('100%')
      }
      if (this.showAbout || this.showShortcuts || this.showTheme) {
        InfoDialog({
          title: this.showAbout ? '关于 Montage' : (this.showShortcuts ? '快捷键' : '主题'),
          rows: this.showShortcuts ? SHORTCUT_ROWS : [],
          showThemes: this.showTheme,
          currentTheme: this.themeMode,
          onPickTheme: (m: ThemeMode): void => this.applyTheme(m),
          onClose: (): void => {
            this.showAbout = false;
            this.showShortcuts = false;
            this.showTheme = false;
          },
        })
      }
    }
    .width('100%')
    .height('100%')
  }
}
