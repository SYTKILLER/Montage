import { hilog } from '@kit.PerformanceAnalysisKit';
import { util } from '@kit.ArkTS';
import { BusinessError } from '@kit.BasicServicesKit';
import { common } from '@kit.AbilityKit';
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { image } from '@kit.ImageKit';
import { window } from '@kit.ArkUI';
import { CompositorEngine, LayerDTO, LayerThumb, OpenFileData, TransformOverlayBox, OpenProjectData, PsdOpenData, RawDevelopData, SaveProjectData, ViewportData, GuidesData, LayerBoundsData, HistoryListData, PatternInfo, LiquifyStrokeDTO, PuppetPinDTO, SmartInfoDTO, SmartFilterDTO, LookupPresetDTO, LookupImportData } from 'core';
import { AppPrefs, RecentProject, ThemeMode } from '../editor/AppPrefs';
import { leafItem, MenuGroupVM, MenuItemVM, subItem, ToolVM } from '../editor/MenuModel';
import { SWATCHES } from '../editor/components/OptionsBar';
import { Toolbox } from '../editor/components/Toolbox';
import { OptionsBar } from '../editor/components/OptionsBar';
import { StatusBar } from '../editor/components/StatusBar';
import { WelcomeView } from '../editor/components/WelcomeView';
import { LayersPanel, ThumbEntry } from '../editor/components/LayersPanel';
import { ExportDialog } from '../editor/components/ExportDialog';
import { TransformOverlay } from '../editor/components/TransformOverlay';
import { AdjustPanel } from '../editor/components/AdjustPanel';
import { ChannelsPanel } from '../editor/components/ChannelsPanel';
import { FILTER_CATALOG, FilterDef, FilterParamDef } from '../editor/FilterCatalog';
import { TextPanel } from '../editor/components/TextPanel';
import { ABOUT_TEXT, InfoDialog, SHORTCUT_ROWS } from '../editor/components/InfoDialog';
import { RulerStrip, GridGuidesOverlay, RULER_BG, RULER_VP } from '../editor/components/ViewOverlays';
import { PathsPanel } from '../editor/components/PathsPanel';
import { HistoryPanel } from '../editor/components/HistoryPanel';
import { HistogramPanel } from '../editor/components/HistogramPanel';
import { InfoPanel } from '../editor/components/InfoPanel';
import { ActionsPanel } from '../editor/components/ActionsPanel';
import { AutomateDialog } from '../editor/components/AutomateDialog';
import { automation, ActionStep } from 'core';
import { Automate, BatchOptions, ProcessorOptions, BatchResult, BatchFileError } from 'core';
import { controlServer } from '../editor/ControlServer';

const TAG: string = 'Montage.Shell';
const DOMAIN: number = 0x4D30;

// M16a JPEG 分带校准采样点（debugJpegCalibration 用；期望色为生成图案的段色）
interface JpegCalibCheck {
  x: number;
  y: number;
  label: string;
  r: number;
  g: number;
  b: number;
}

// M16a：surface 尺寸变化回调（XComponentController 子类覆写，API 12 起）。旧 60Hz 常转
// 循环由帧内 geometry 检查顺带吸收 resize，空闲停帧 governor 下必须显式唤醒渲染。
class CanvasSurfaceController extends XComponentController {
  onChanged: ((rect: SurfaceRect) => void) | null = null;

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    if (this.onChanged !== null) {
      this.onChanged(rect);
    }
  }
}

// KeyCode（@ohos.multimodalInput.keyCode 已核验，2026-10-07）
const KC_B: number = 2018;
const KC_A: number = 2017;
const KC_X: number = 2040;  // KEYCODE_X（M8.1 核验记录）
const KC_C: number = 2019;
const KC_D: number = 2020;
const KC_E: number = 2021;
const KC_H: number = 2024;
const KC_I: number = 2025;
const KC_L: number = 2028;
const KC_M: number = 2029;
const KC_N: number = 2030;
const KC_O: number = 2031;
const KC_Q: number = 2033;  // KEYCODE_Q（SDK keyCode.d.ts 核验，2026-10-11）
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
const KC_F6: number = 2095;  // @ohos.multimodalInput.keyCode KEYCODE_F6（SDK 核验）
const KC_F5: number = 2094;  // KEYCODE_F5（SDK 核验，2026-10-11）
const KC_G: number = 2023;   // KEYCODE_G（SDK 核验，2026-10-11）
const KC_J: number = 2026;   // KEYCODE_J（A=2017 起序列；KC_X=2040/KC_G=2023 双锚点核对，2026-10-13）
const KC_T_KEY: number = 2036;
const KC_U: number = 2037;  // KEYCODE_U（SDK 核验，2026-10-11）
const KC_R: number = 2034;  // KEYCODE_R（SDK 核验，2026-10-11；KC_O=2031 见上）
const KC_P: number = 2032;  // KEYCODE_P（M18 钢笔工具，PS 惯例）
const KC_X_KEY: number = 2040;
const KC_ENTER: number = 2054;
const KC_UP: number = 2012;
const KC_DOWN: number = 2013;
const KC_LEFT: number = 2014;
const KC_RIGHT: number = 2015;
const KC_F_KEY: number = 2022;         // KEYCODE_F（SDK keyCode.d.ts 核验，2026-10-13）
const KC_SEMICOLON: number = 2062;     // KEYCODE_SEMICOLON（SDK 核验，2026-10-13）
const KC_APOSTROPHE: number = 2063;    // KEYCODE_APOSTROPHE（SDK 核验，2026-10-13）

/**
 * M7.5 编辑器外壳（06 规划 §4）：菜单栏 + 选项条 + 工具箱 + 画布 + 面板 + 状态栏。
 * 命令表是唯一入口：菜单/快捷键共用 runCommand；工具态驱动手势换绑与选项条。
 */
@Entry
@Component
struct Index {
  @StorageProp('docVersion') @Watch('onDocVersion') docVersion: number = 0;
  // M23c：分段 want 通道的 flush 消费（flush 设 debugOpenB64 后本位被写触发）。
  @StorageProp('debugOpenB64') @Watch('onDebugOpenB64') debugOpenB64Prop: string = '';
  // M22：调试命令直通（want debugCmd → runCommand；uitest 注入退化的 E2E 旁路）
  @StorageProp('debugCmd') @Watch('onDebugCmd') debugCmdProp: string = '';
  @StorageProp('importProgress') importProgress: number = 0;
  @State docOpen: boolean = false;
  @State docW: number = 0;
  @State docH: number = 0;
  @State zoom: number = 1;
  @State panX: number = 0;
  @State panY: number = 0;
  @State canvasVpW: number = 0;
  @State canvasVpH: number = 0;
  // M15a 视图状态（标尺/网格/吸附/屏幕模式；除参考线镜像外均为应用偏好，重启恢复）
  @State rulersVisible: boolean = true;
  @State rulerUnit: number = 0;         // 0 = px、1 = 百分比（A-2）
  @State gridVisible: boolean = false;
  @State gridSpacing: number = 100;
  @State snapGuidesOn: boolean = true;
  @State snapGridOn: boolean = false;
  @State screenMode: number = 0;        // 0 = 标准、1 = 带菜单栏全屏、2 = 全屏
  @State guidesMirror: GuidesData = { h: [], v: [], locked: false, visible: true };
  @State guidePreview: number[] = [];   // 标尺拖拽预览 [orient, posDocPx]；空 = 无
  // M15b 面板（历史/直方图/信息）：可见性偏好持久化（C-1）+ 数据镜像
  @State historyVisible: boolean = true;
  @State histogramVisible: boolean = false;
  @State infoVisible: boolean = false;
  // M24a 动作面板
  @State actionsVisible: boolean = false;
  @State actionsRev: number = 0;
  @State actionsSel: number = -1;
  @State showActionRename: boolean = false;
  @State actionRenameText: string = '';
  // M24b 批处理 / 图像处理器
  @State showAutomate: boolean = false;
  @State automateMode: number = 0;
  @State automateBusy: boolean = false;
  @State automateProgress: string = '';
  @State automateResult: string = '';
  private automateCancel: boolean = false;
  private actionNamesMirror: string[] = [];
  @State historyLabels: string[] = [''];
  @State historyCurrent: number = 0;
  @State infoPointerX: number = -1;
  @State infoPointerY: number = -1;
  @State infoColor: string = '';
  @State infoSelOk: boolean = false;
  @State infoSelW: number = 0;
  @State infoSelH: number = 0;
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
  @State brushColor: string = '#D93025';   // 前景色（= 笔刷色）
  @State bgColor: string = '#FFFFFF';      // 背景色
  @State maskTarget: boolean = false;
  @State quickMaskActive: boolean = false;  // M10d 快速蒙版模式（引擎镜像；Q/菜单 toggle）
  // M11a 渐变工具：类型/样式/目标 + 拖线预览（文档域 [x0,y0,x1,y1]）
  @State gradientType: number = 0;      // 0=线性 1=径向
  @State gradientStyle: number = 0;     // 0=前景→背景 1=前景→透明
  @State gradientTarget: number = 0;    // 0=像素 1=图层蒙版
  @State gradientLine: number[] | null = null;  // 非空 = 拖线中（overlay 预览）
  @State showFill: boolean = false;     // M11a 填充对话框（Shift+F5）
  @State showStyle: boolean = false;    // M14c 图层样式对话框（投影/描边/颜色叠加 + 填充层参数）
  @State styShadowOn: boolean = true;
  @State styAngle: number = 120;
  @State styDist: number = 10;
  @State stySize: number = 8;
  @State styShadowOp: number = 50;
  @State styShadowColor: string = '#000000';
  @State styStrokeOn: boolean = true;
  @State styStrokeW: number = 3;
  @State styStrokeColor: string = '#D93025';
  @State styOverlayOn: boolean = false;
  @State styOverlayOp: number = 100;
  @State styOverlayColor: string = '#1A6BD9';
  // M17a 四新效果（对话框 last-applied 会话态，同 M14c 惯例不读回引擎）
  @State styInnerShadowOn: boolean = false;
  @State styIsAngle: number = 135;
  @State styIsDist: number = 5;
  @State styIsSize: number = 5;
  @State styIsOp: number = 75;
  @State styIsColor: string = '#000000';
  @State styOuterGlowOn: boolean = false;
  @State styOgSize: number = 10;
  @State styOgOp: number = 50;
  @State styOgColor: string = '#FFE470';
  @State styInnerGlowOn: boolean = false;
  @State styIgSize: number = 10;
  @State styIgOp: number = 50;
  @State styIgColor: string = '#FFE470';
  @State styGradientOn: boolean = false;
  @State styGdAngle: number = 90;
  @State styGdOp: number = 100;
  @State styGdFrom: string = '#000000';
  @State styGdTo: string = '#FFFFFF';
  // M17b spread/全局光/光泽/斜面（对话框 last-applied 会话态）
  @State styShadowSpread: number = 0;
  @State styIsSpread: number = 0;
  @State styOgSpread: number = 0;
  @State styIgSpread: number = 0;
  @State styShadowGL: boolean = true;
  @State styIsGL: boolean = true;
  @State stySatinOn: boolean = false;
  @State stySatinAngle: number = 120;
  @State stySatinDist: number = 8;
  @State stySatinSize: number = 8;
  @State stySatinOp: number = 100;
  @State stySatinColor: string = '#808080';
  @State stySatinInvert: boolean = false;
  @State styBevelOn: boolean = false;
  @State styBevelOuter: boolean = false;
  @State styBevelDepth: number = 2;
  @State styBevelUp: boolean = true;
  @State styBevelSize: number = 5;
  @State styBevelAngle: number = 120;
  @State styBevelAlt: number = 30;
  @State styBevelHiOp: number = 100;
  @State styBevelHiColor: string = '#FFFFFF';
  @State styBevelShOp: number = 100;
  @State styBevelShColor: string = '#000000';
  // M17d 等高线/Range/图案叠加（对话框 last-applied 会话态；等高线编辑器共享画布）
  @State styOgRange: number = 50;       // 发光「范围」PS 默认 50%
  @State styIgRange: number = 50;
  @State showContour: boolean = false;  // 等高线编辑器（独立模态，七类目标共用）
  @State ctTarget: number = 0;          // 0=投影 1=内阴影 2=外发光 3=内发光 4=光泽 5=斜面轮廓 6=斜面光泽
  @State ctPts: number[] = [0, 0, 1, 1];  // x,y 交错 0..1
  @State ctCorners: number[] = [0, 0];    // 与点一一对应（1=角点）
  @State ctRange: number = 50;
  @State ctDrag: number = -1;
  @State styPatternOn: boolean = false;
  @State styPatternId: string = '';
  @State styPatternScale: number = 100;
  @State styPatternOp: number = 100;
  @State patternList: PatternInfo[] = [];  // 文档图案（getPatterns 镜像）
  private contourCtx: CanvasRenderingContext2D = new CanvasRenderingContext2D(
    new RenderingContextSettings(true));
  @State showGlobalLight: boolean = false;
  @State globalLightAngle: number = 120;
  @State fillKind: number = 0;          // 填充层参数（活动层 isFill 时在样式对话框编辑）
  @State fillColor: string = '#D93025';
  @State fillAngle: number = 90;
  @State showFilter: boolean = false;   // M13 滤镜对话框（proxy 预览）
  @State lookupPresets: LookupPresetDTO[] = [];  // M22 颜色查找预设（内置 + 会话导入）
  @State viewXformLut: boolean = false;   // M22 显示变换镜像
  @State viewXformGamut: boolean = false;
  @State viewXformDesc: string = '';
  @State filterDef: FilterDef | null = null;
  @State showSmartFilters: boolean = false;  // M21 智能滤镜管理对话框
  @State smartInfo: SmartInfoDTO | null = null;
  @State smartEditIndex: number = -1;        // >=0 = 滤镜对话框编辑模式（预填+setSmartFilter）
  @State filterP: number[] = [];        // 当前参数值（schema 序）
  @State filterSeed: number = 1;
  private filterTargetLayer: number = 0; // M24a：滤镜预览目标层（提交时录 filter.apply）
  private filterDebounce: number = -1;
  @State fillSource: number = 0;        // 0=前景 1=背景 2=黑 3=白
  private gradDragging: boolean = false;
  // M15e RAW 显影参数面板（会话内导入过 DNG 才可用；确定=一步 history 事务）
  @State hasRawDevelop: boolean = false;
  @State hasStyleClipboard: boolean = false;  // M17a：粘贴样式菜单使能
  @State showDevelop: boolean = false;
  @State devTemp: number = 0;           // -100..100（正=暖）
  @State devTint: number = 0;           // -100..100（正=品红）
  @State devEv: number = 0;             // -2..+2 EV
  @State devGamma: number = 1;          // 0.5..2.0
  // M23b：Camera Raw 链开关 + Basic/细节/效果滑杆（camera_raw.rs 语义；开着时
  // v1 四参中 temp/tint/ev 由链接管、伽马保留）。曲线/HSL/分级留 M23c 编辑器。
  @State devCameraOn: boolean = false;
  @State devExposure: number = 0;       // -100..100（Camera Raw 单位，0.25/级）
  @State devContrast: number = 0;       // -100..100
  @State devHighlights: number = 0;     // -100..100
  @State devShadows: number = 0;        // -100..100
  @State devWhites: number = 0;         // -100..100
  @State devBlacks: number = 0;         // -100..100
  @State devTexture: number = 0;        // -100..100
  @State devClarity: number = 0;        // -100..100
  @State devDehaze: number = 0;         // -100..100
  @State devVibrance: number = 0;       // -100..100
  @State devSaturation: number = 0;     // -100..100
  @State devSharpen: number = 0;        // 0..300（ACR 锐化量）
  @State devNoiseLum: number = 0;       // 0..100
  @State devNoiseColor: number = 0;     // 0..100
  @State devVignette: number = 0;       // -100..100
  @State devGrain: number = 0;          // 0..100
  // M23c：曲线/HSL/分级编辑器 + 实时预览 + 层滤镜模式
  @State devCrvHl: number = 0;          // 参数曲线四区（-100..100）
  @State devCrvLg: number = 0;
  @State devCrvDk: number = 0;
  @State devCrvSh: number = 0;
  @State devCurvePts: number[] = [];    // 点曲线扁平 [x,y,…]（0..255；空=线性）
  @State devHslBand: number = 0;        // 0红1橙2黄3绿4青5蓝6紫7洋红
  @State devHslH: number[] = [0, 0, 0, 0, 0, 0, 0, 0];
  @State devHslS: number[] = [0, 0, 0, 0, 0, 0, 0, 0];
  @State devHslL: number[] = [0, 0, 0, 0, 0, 0, 0, 0];
  @State devGradeWheel: number = 0;     // 0阴影1中间调2高光3全局
  @State devGradeH: number[] = [0, 0, 0, 0];
  @State devGradeS: number[] = [0, 0, 0, 0];
  @State devGradeL: number[] = [0, 0, 0, 0];
  @State devGradeBlend: number = 50;
  @State devGradeBal: number = 0;
  @State devLayerMode: boolean = false;   // 滤镜模式：目标=活动层（滤镜菜单入口）
  @State devLayerTarget: number = 0;
  @State devPreviewLive: boolean = false; // 预览会话已开（取消需还原）
  private devPreviewTimer: number = -1;
  private devCurveCtx: CanvasRenderingContext2D = new CanvasRenderingContext2D(
    new RenderingContextSettings(true));
  private devCurveDrag: number = -1;
  private devCurveReady: boolean = false;
  // M11c 形状工具：种类/样式 + 拖拽预览（视口 [x,y,w,h]）
  @State shapeKind: number = 0;         // 0=矩形 1=圆角矩形 2=椭圆 3=直线
  @State shapeFill: boolean = true;
  @State shapeStroke: boolean = false;
  @State shapeStrokeWidth: number = 4;
  @State shapePreview: number[] | null = null;  // 非空 = 拖拽中
  @State shapeLiveRadius: number = 0;   // M18：活动 live 矩形圆角（0 = 无 live 矩形活动层）
  private shapeDragging: boolean = false;
  // M18c 钢笔工具：进行中锚点（文档域扁平 [x,y,…]）+ 闭合标志 + 已闭合提交缓存
  @State penPoints: number[] = [];
  @State penClosed: boolean = false;
  @State workPathNonEmpty: number = 0;  // M18：菜单使能镜像（引擎工作路径非空）
  @State hasCustomShapes: boolean = false;  // M18：自定义形状库非空
  // M18.1 直接选择：工作副本扁平路径 + 选中集 [sub,knot,…] + 单选手柄
  @State dsPath: number[] = [];
  @State dsSelected: number[] = [];
  @State dsHandleOf: number[] = [];  // 单选锚点 [sub,knot] 显示其手柄；空 = 不显示
  @State dsPreviewPts: number[][] = [];  // 视口折线（路径抽样）
  @State dsAnchorPts: number[][] = [];   // 视口锚点 [x,y,sel]
  @State dsHandlePts: number[][] = [];   // 视口手柄 [x,y,stemX,stemY]
  @State dsMarqueeRect: number[] | null = null;  // 视口 [x,y,w,h]
  private dsDragKind: number = 0;  // 0=无 1=锚点 2=手柄 3=框选 4=段
  private dsDragTargets: number[] = [];   // 拖动涉及的锚点 [sub,knot,…]
  private dsDragHandle: number[] = [];    // [sub,knot,isIn]
  private dsDragSeg: number[] = [];       // [sub,knot(段序),t]
  private dsDragStartDoc: number[] = [0, 0];
  private dsDragLastDoc: number[] = [0, 0];
  private dsLoadedRev: number = -1;  // 镜像装载时的 docVersion（undo/提交后强制重载）
  private shapeStartDocX: number = 0;
  private shapeStartDocY: number = 0;
  @State cloneAligned: boolean = true;   // M11d 仿制图章对齐开关
  @State cloneSourceSet: boolean = false; // 已取源镜像（选项条提示态）
  @State clonePicking: boolean = false;   // 取源模式：下一次画布点击 = 取源（触屏兜底）
  // M11e 效果工具（R 组：blur/sharpen/smudge；O 组：dodge/burn/sponge）
  @State effectTool: string = 'blur';
  @State historyBrushSteps: number = 1;  // M11f 历史画笔源步数（1=最近一次编辑前）
  // M12 文字工具：拖框预览（视口 [x,y,w,h]）+ 编辑会话 + 字符/段落面板镜像
  @State textDrag: number[] | null = null;   // 非空 = 拖框中
  @State editingTextId: number = 0;          // 非 0 = overlay 编辑会话中（RichEditor 挂载）
  @State panelFontList: string[] = [];       // 系统字体列表（懒加载一次）
  @State userFonts: string[] = [];           // M12e 已导入用户字体（会话级；重启需重注册）
  @State panelFontSel: number = -1;
  @State textFontSize: number = 40;
  @State textLineHeightPct: number = 120;    // %（引擎值 = /100）
  @State textTracking: number = 0;           // 千分比（PS 语义）
  @State textBold: boolean = false;
  @State textItalic: boolean = false;
  @State textColor: string = '#000000';
  @State textAlignIdx: number = 0;
  @State textPanelContent: string = '';      // 面板内容框镜像（blur 提交）
  private textDragging: boolean = false;
  private textStartDocX: number = 0;
  private textStartDocY: number = 0;
  private textEditTimer: number = -1;        // overlay 输入 debounce
  private richController: RichEditorController = new RichEditorController();

  private effectModeOf(id: string): number {
    if (id === 'blur') {
      return 1;
    } else if (id === 'sharpen') {
      return 2;
    } else if (id === 'smudge') {
      return 3;
    } else if (id === 'dodge') {
      return 4;
    } else if (id === 'burn') {
      return 5;
    } else if (id === 'sponge') {
      return 6;
    }
    return 0;
  }
  @State wandTolerance: number = 32;
  @State wandContiguous: boolean = true;
  @State wandAllLayers: boolean = false;
  @State wandSampleSize: number = 1;
  @State selOp: number = 0;          // M10a 布尔四态：0 新/1 加/2 减/3 交（Shift=加 Alt=减 临时修饰）
  @State selShape: string = 'rect';  // M10a 选框形状：rect | ellipse
  @State polyPendingPts: number[] = [];  // M10a 多边形套索已落点（文档域扁平），非空 = 待闭合
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
  @State showImageSize: boolean = false;
  // 尺寸对话框模式（0 重采样/1 内容感知比例——M19c 复用同对话框）
  @State imageSizeMode: number = 0;
  @State imgWText: string = '';
  @State imgHText: string = '';
  @State imgResampleMethod: number = 2;  // M20a 重采样方法（默认两次立方）
  // M20b 液化（工具态；笔画收集即发即弃，每笔一步异步事务）
  @State liquifyTool: number = 0;        // 0 向前变形/1 重建/2 平滑/3 顺时针/4 逆时针/5 褶皱/6 膨胀/7 左推
  @State liquifySize: number = 120;
  @State liquifyPressure: number = 80;
  private liquifyPoints: number[][] = [];
  private liquifyDrawing: boolean = false;
  // M20c 操控变形（钉集即数据；应用=一步异步事务）
  @State puppetMode: number = 1;     // 0 刚体/1 正常/2 扭曲
  @State puppetDensity: number = 1;  // 0 更少/1 正常/2 较多
  @State puppetPins: PuppetPinDTO[] = [];
  @State puppetDragIdx: number = -1;
  // M20d 透视变形（单平面 v1；四角 doc 坐标 [x0,y0,x1,y1,x2,y2,x3,y3] 顺时针自左上）
  @State perspectiveQuad: number[] = [];
  @State perspectiveDragIdx: number = -1;
  private readonly perspectiveSrcBackup: number[] = [];
  // M20d 自适应广角（v1 对话框：模型/焦距/裁切/缩放；约束线 overlay 登记）
  // M20d 消失点透视克隆（4 击定平面 → 取源 → 点按克隆一笔一事务）
  @State vpQuad: number[] = [];
  @State vpSource: number[] = [];
  @State vpSourceSet: boolean = false;
  @State vpSize: number = 60;
  private vpPickingSource: boolean = false;
  @State showWideAngle: boolean = false;
  @State waModel: number = 2;   // 0 自动/1 鱼眼/2 透视/3 全球形
  @State waFocal: number = 15;
  @State waCrop: number = 1;
  @State waScale: number = 100;
  @State channelsCount: number = 0;       // M10c Alpha 通道数（镜像，驱动菜单使能）
  @State showStoreSel: boolean = false;   // 存储选区名字对话框
  @State storeSelText: string = '';
  @State showLoadSel: boolean = false;    // 载入选区名字对话框
  @State loadSelText: string = '';
  @State showModify: boolean = false;     // M10b 羽化/修改族数值对话框
  @State modifyKind: string = 'feather';  // feather | border | expand | contract
  @State modifyText: string = '';
  @State showRotateAny: boolean = false;
  @State angleText: string = '';
  @State histLuma: number[] = [];  // M9 直方图（节流拉取）
  @State curveXs: number[] = [0, 1];
  @State curveYs: number[] = [0, 1];
  @State showAdjCreate: boolean = true;  // 非调整层时显示 kind 网格（点击新建）
  @State adjP0: number = 0;
  @State adjP1: number = 0;
  @State adjP2: number = 0;
  @State panelVersion: number = 0;  // M14a 面板树序版本（强制 ForEach 重建）
  @State multiSelected: number[] = [];  // M14b 图层多选（面板 Ctrl/Shift+点击；对齐/分布/链接驱动）
  private histTimer: number = -1;
  @State showTransform: boolean = false;
  @State overlayBox: TransformOverlayBox | null = null;  // M8.1 手柄 overlay 态
  @State overlayMat: number[] = [1, 0, 0, 0, 1, 0];
  @State tfScale: number = 100;   // %
  @State tfRotate: number = 0;    // 度
  @State tfSkew: number = 0;      // 度
  @State thumbVersion: number = 0;

  private engine: CompositorEngine = new CompositorEngine();
  private xcController: CanvasSurfaceController = new CanvasSurfaceController();
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
  private lassoActive: boolean = false;  // M10a 套索拖拽中
  private lassoPts: number[] = [];       // M10a 套索拖拽点集（文档域扁平）
  @State lassoPreview: number[] = [];    // 套索/多边形 overlay 折线（文档域扁平；驱动重绘）
  private polyLastDownTime: number = 0;  // 多边形套索双击检测（屏幕坐标）
  private polyLastDownScrX: number = 0;
  private polyLastDownScrY: number = 0;
  private moveActive: boolean = false;
  private moveLastX: number = 0;
  private moveLastY: number = 0;
  // M15a 吸附基准与累计位移（移动工具；吸附修正几何全在 UI 层算，引擎收增量）
  private moveSnapBase: LayerBoundsData | null = null;
  private moveAccX: number = 0;
  private moveAccY: number = 0;
  private moveAppliedX: number = 0;
  private moveAppliedY: number = 0;
  // 笔画进行中（Down→Up）。期间暂停信息面板取色：samplePixel 是同步 NAPI，会阻塞
  // UI 线程等 docMutex——笔画线程的提交/重样式（样式层含高斯模糊）和渲染线程的
  // 逐帧快照都长持该锁，撞上即秒级冻结（appfreeze 实证，冻结栈＝SamplePixel←mutex::lock）
  private strokeDrawing: boolean = false;
  // 收笔护栏（修复③）：Up 后到提交完成（docVersion 事件）之间的取色仍会撞笔画线程
  // commit/重样式临界区。护栏期 800ms 兜底，收到 revision 广播即提前解除；
  // Up 事件本身的取色因 strokeDrawing 尚未复位天然跳过，此处只管收笔后的第一波 Move/悬停
  private strokeEndGuardUntil: number = 0;
  // M19a 修补工具拖拽态（选区内按下 → 拖拽向量 = 源位移，Up 提交）
  private patchDragging: boolean = false;
  private patchStartX: number = 0;
  private patchStartY: number = 0;
  // M19b 内容感知移动（move 工具选项条三态：0 关闭/1 移动/2 扩展）
  @State caMoveMode: number = 0;
  private caDragging: boolean = false;
  private caStartX: number = 0;
  private caStartY: number = 0;
  private caCurX: number = 0;
  private caCurY: number = 0;
  // M19a 污点修复模式（0 内容识别/1 创建纹理/2 接近匹配；引擎 kind = 2 + 模式）
  @State spotHealMode: number = 0;
  // M15a 参考线交互态（画布内拖动 + 标尺拖拽创建）
  private guideMoveOrient: number = -2;  // >=0 = 画布内拖动中
  private guideMoveIndex: number = -1;
  private rulerDragOrient: number = -2;  // >=0 = 标尺拖拽中（0 = 水平、1 = 垂直）
  private rulerDragIndex: number = -1;
  private rulerDragMove: boolean = false;
  private lastInfoSampleMs: number = 0;  // 信息面板取色节流
  private cropDrag: boolean = false;
  private cropX0: number = 0;
  private cropY0: number = 0;
  @State cropRect: number[] | null = null;   // [x,y,w,h] 文档坐标；非空 = 待提交/取消
  @State hasSelectionFlag: boolean = false;  // 会话态镜像（引擎无此查询命令，操作点维护）
  private readonly thumbCache: Map<number, ThumbEntry> = new Map();
  private readonly thumbsInFlight: Set<number> = new Set();

  // getContext（API 18 废弃，审查修复 MEDIUM）替代：UIContext.getHostContext。
  // 页面挂载后恒有宿主上下文；undefined 时跳过偏好/主题（不致命）
  private hostCtx(): common.Context | undefined {
    return this.getUIContext().getHostContext();
  }

  private cachePath(name: string): string {
    const c = this.getUIContext().getHostContext();
    return (c !== undefined ? c.cacheDir : '') + '/' + name;
  }

  aboutToAppear(): void {
    const hc = this.hostCtx();
    if (hc !== undefined) {
      this.prefs.init(hc);
    }
    this.themeMode = this.prefs.getThemeMode();
    if (hc !== undefined) {
      this.prefs.applyTheme(this.themeMode, hc);
    }
    this.recents = this.prefs.getRecents();
    // M15a：视图偏好恢复（C-1）
    this.rulersVisible = this.prefs.getBool('view_rulers', true);
    this.rulerUnit = this.prefs.getNumber('view_ruler_unit', 0);
    this.gridVisible = this.prefs.getBool('view_grid', false);
    this.gridSpacing = this.prefs.getNumber('view_grid_spacing', 100);
    this.snapGuidesOn = this.prefs.getBool('view_snap_guides', true);
    this.snapGridOn = this.prefs.getBool('view_snap_grid', false);
    // 屏幕模式为会话态（启动恒标准）：1/2 是查看模式而非工作区状态，持久化会造成
    // 「重启即无工具箱/菜单栏」的陷阱；C-1 面板布局持久化由标尺/网格/吸附/面板开关承担
    // M15b：面板布局持久化（C-1）
    this.layersVisible = this.prefs.getBool('panel_layers', true);
    this.historyVisible = this.prefs.getBool('panel_history', true);
    this.histogramVisible = this.prefs.getBool('panel_histogram', false);
    this.infoVisible = this.prefs.getBool('panel_info', false);
    // M24a：动作内核挂载 + 持久化（Preferences actions_json；变更即写，量小）
    automation.setEngine(this.engine);
    automation.loadFromJson(this.prefs.getString('actions_json', ''));
    automation.setDirtyCb((): void => {
      this.actionsRev++;
      this.prefs.setString('actions_json', automation.serialize());
    });
    this.actionsVisible = this.prefs.getBool('panel_actions', false);
    // M24c：控制通道（want debugControl=1 或调试菜单手动开启）
    controlServer.setEngine(this.engine);
    this.consumeDebugControl();
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
    // M23a：want 参数调试直通打开（EntryAbility.onCreate/onNewWant 写入；
    // 模拟器嵌套菜单注入被吞的替代 E2E 通道）。debugOpenB64 = 文件字节 base64
    // （app 沙箱读不了 shell 的 /data/local/tmp，直接传字节），写 cacheDir 后打开。
    this.lookupPresets = this.engine.getLookupPresets();
    this.consumeDebugOpenB64();
  }

  // M23c：debugOpenB64 消费抽方法（分段 flush 在 aboutToAppear 之后到达，
  // 由 @StorageProp @Watch 驱动重入）。
  private refreshViewXformMirror(): void {
    const vt = this.engine.getViewTransform();
    this.viewXformLut = vt.lutOn;
    this.viewXformGamut = vt.gamutOn;
    this.viewXformDesc = vt.desc;
  }

  private onDebugOpenB64(): void {
    this.consumeDebugOpenB64();
  }

  private onDebugCmd(): void {
    const cmd = AppStorage.get<string>('debugCmd');
    if (cmd !== undefined && cmd.length > 0) {
      AppStorage.setOrCreate('debugCmd', '');
      this.log(`debugCmd: ${cmd}`);
      this.runCommand(cmd);
    }
  }

  /** M24c：want debugControl=1 → 启动控制通道（debug-only；hilog 透出 token）。 */
  private consumeDebugControl(): void {
    const flag = AppStorage.get<string>('debugControl');
    if (flag === '1') {
      AppStorage.setOrCreate('debugControl', '');
      const portStr = AppStorage.get<string>('debugControlPort') ?? '';
      const port = parseInt(portStr.length > 0 ? portStr : '18787');
      if (controlServer.start(this.engine, this.cacheDir(), isNaN(port) ? 18787 : port)) {
        this.log(`控制通道已启动（端口 ${controlServer.getPort()}，token 见 hilog Montage.Control）`);
      }
    }
  }

  /** 帮助›调试：手动开关控制通道（与 want 门控同源）。 */
  private toggleControlServer(): void {
    if (controlServer.isRunning()) {
      controlServer.stop();
      this.log('控制通道已停止');
      this.toast('控制通道已停止');
    } else {
      if (controlServer.start(this.engine, this.cacheDir(), 18787)) {
        this.log(`控制通道已启动（端口 18787，token 见 hilog Montage.Control）`);
        this.toast('控制通道已启动');
      }
    }
  }

  private cacheDir(): string {
    const c = this.getUIContext().getHostContext();
    return c !== undefined ? c.cacheDir : '';
  }

  private consumeDebugOpenB64(): void {
    const dbgB64 = AppStorage.get<string>('debugOpenB64');
    if (dbgB64 !== undefined && dbgB64.length > 0) {
      AppStorage.setOrCreate('debugOpenB64', '');
      try {
        const bin: Uint8Array = new util.Base64Helper().decodeSync(dbgB64);
        const pd = this.cachePath('debug_open.bin');
        const fw = fs.openSync(pd, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
        fs.writeSync(fw.fd, bin.buffer);
        fs.closeSync(fw);
        const f = fs.openSync(pd, fs.OpenMode.READ_ONLY);
        // M22：8BPS 魔数 → PSD 命令（PSD 不在 openFileFromFd 通用链上，独立 openPsdFile）
        const head = String.fromCharCode(bin[0], bin[1], bin[2], bin[3]);
        if (head === '8BPS') {
          this.engine.openPsdFile(f.fd).then((dd: PsdOpenData): void => {
            this.docW = dd.width;
            this.docH = dd.height;
            this.docOpen = true;
            this.log(`调试直通打开(PSD): ${dd.width}x${dd.height} notes=${dd.notes.length}`);
            for (const nt of dd.notes) {
              this.log(`  note: ${nt}`);
            }
          }).catch((e: BusinessError): void => {
            this.log(`调试直通打开失败(PSD): ${e.message}`);
          });
          return;
        }
        this.engine.openFileFromFd(f.fd).then((dd: OpenFileData): void => {
          this.docW = dd.width;
          this.docH = dd.height;
          this.docOpen = true;
          this.log(`调试直通打开: ${dd.width}x${dd.height}`);
        }).catch((e: BusinessError): void => {
          this.log(`调试直通打开失败: ${e.message}`);
        });
      } catch (e) {
        const be = e as BusinessError;
        this.log(`调试直通打开失败: ${be.code} ${be.message}`);
      }
    }
  }

  aboutToDisappear(): void {
    if (this.statsTimer >= 0) {
      clearInterval(this.statsTimer);
      this.statsTimer = -1;
    }
    if (this.histTimer >= 0) {
      clearTimeout(this.histTimer);
      this.histTimer = -1;
    }
    // 审查修复：防抖/文字提交定时器跨 dispose 回调（引擎已释放后仍发命令）
    if (this.filterDebounce >= 0) {
      clearTimeout(this.filterDebounce);
      this.filterDebounce = -1;
    }
    if (this.textEditTimer >= 0) {
      clearTimeout(this.textEditTimer);
      this.textEditTimer = -1;
    }
    this.engine.dispose();
  }

  // ---------- 状态刷新（05 §6 数据流） ----------

  private onDocVersion(): void {
    this.strokeEndGuardUntil = 0;  // 修复③：提交/撤销完成（含重样式发布），取色护栏解除
    this.refreshLayers();
    this.refreshStats();  // M10d：撤销跨 Q 边界时同步快速蒙版镜像（canUndo/Redo 同步受益）
    if (this.tool === 'directSelect') {
      this.dsReloadPath();  // M18.1：撤销/换文档后路径镜像跟随
    }
    this.scheduleHistogram();
    this.refreshGuides();  // M15a：打开工程/撤销到其他文档状态时同步参考线镜像
    this.refreshHistory();      // M15b：历史面板镜像（撤销/重做/跳转/编辑后同步）
    this.refreshSelectionInfo();  // M15b：选区尺寸镜像
  }

  // ---------- M15b 面板 ----------

  private refreshHistory(): void {
    const h: HistoryListData = this.engine.getHistoryList();
    this.historyLabels = h.labels;
    this.historyCurrent = h.current;
  }

  private pickHistoryState(idx: number): void {
    if (this.engine.setHistoryState(idx)) {
      this.refreshHistory();
      this.log(`历史跳转 → ${idx === 0 ? '初始' : this.historyLabels[idx]}`);
    }
  }

  private refreshSelectionInfo(): void {
    const b = this.engine.getSelectionBounds();
    this.infoSelOk = b.ok;
    this.infoSelW = b.w;
    this.infoSelH = b.h;
  }

  // 指针信息推送（触摸 Move 节流 150ms、Down/Up 与鼠标悬停即时）
  private updatePointerInfo(docX: number, docY: number, final: boolean): void {
    this.infoPointerX = docX;
    this.infoPointerY = docY;
    if (this.strokeDrawing) {
      return;  // 坐标读数照常，取色延到笔画结束（原因见 strokeDrawing 声明处）
    }
    if (Date.now() < this.strokeEndGuardUntil) {
      return;  // 修复③：收笔护栏期内跳过取色（docVersion 到达即解除，见 onDocVersion）
    }
    const now = Date.now();
    if (!final && now - this.lastInfoSampleMs < 150) {
      return;
    }
    this.lastInfoSampleMs = now;
    this.engine.samplePixel(Math.round(docX), Math.round(docY)).then((s): void => {
      if (s.ok) {
        const hex = (v: number): string => {
          const t = Math.max(0, Math.min(255, Math.round(v)));
          const h16 = t.toString(16);
          return h16.length === 1 ? `0${h16}` : h16;
        };
        this.infoColor = `#${hex(s.red)}${hex(s.green)}${hex(s.blue)}`;
      }
    }).catch((_e: BusinessError): void => {
      // 采样失败静默（空文档/越界）
    });
  }

  // ---------- M15a 视图：参考线 / 标尺 / 网格 / 吸附 ----------

  private refreshGuides(): void {
    this.guidesMirror = this.engine.getGuides();
  }

  // 命中测试：最近参考线（文档域容差），返回 [orient, index] 或 null
  private hitGuide(docX: number, docY: number, tolDoc: number): number[] | null {
    let best: number[] | null = null;
    let bestD = tolDoc;
    const hs = this.guidesMirror.h;
    const vs = this.guidesMirror.v;
    for (let i = 0; i < hs.length; ++i) {
      const d = Math.abs(hs[i] - docY);
      if (d <= bestD) {
        bestD = d;
        best = [0, i];
      }
    }
    for (let i = 0; i < vs.length; ++i) {
      const d = Math.abs(vs[i] - docX);
      if (d <= bestD) {
        bestD = d;
        best = [1, i];
      }
    }
    return best;
  }

  // 标尺拖拽（标尺组件 onTouch）：近邻抓已有参考线 / 空白处拖出创建；拖回标尺 = 删除/取消
  private onRulerTouch(e: TouchEvent, horizontal: boolean): void {
    if (!this.docOpen || (e.touches.length < 1 && e.changedTouches.length < 1)) {
      return;
    }
    const t: TouchObject = e.changedTouches.length > 0 ? e.changedTouches[0] : e.touches[0];
    const k = this.pxPerVp();
    const orient = horizontal ? 0 : 1;
    // 标尺组件局部 cross 坐标 − 标尺宽 = 画布内坐标（负 = 仍在标尺上）
    const docAlong = orient === 0
      ? this.panY + ((t.y - RULER_VP) * k) / this.zoom
      : this.panX + ((t.x - RULER_VP) * k) / this.zoom;
    const limit = orient === 0 ? this.docH : this.docW;
    if (e.type === TouchType.Down) {
      const list = orient === 0 ? this.guidesMirror.h : this.guidesMirror.v;
      const tolDoc = 8 * k / this.zoom;
      let idx = -1;
      for (let i = 0; i < list.length; ++i) {
        if (Math.abs(list[i] - docAlong) <= tolDoc) {
          idx = i;
          break;
        }
      }
      this.rulerDragOrient = orient;
      this.rulerDragIndex = idx;
      this.rulerDragMove = idx >= 0;
      this.guidePreview = [orient, Math.max(0, Math.min(docAlong, limit))];
      hilog.info(DOMAIN, TAG, 'ruler drag start orient=%{public}d grab=%{public}s',
        orient, idx >= 0 ? 'move' : 'create');
    } else if (e.type === TouchType.Move && this.rulerDragOrient >= 0) {
      const pos = Math.max(0, Math.min(docAlong, limit));
      if (this.rulerDragMove) {
        this.engine.moveGuide(this.rulerDragOrient, this.rulerDragIndex, pos);
        this.refreshGuides();
      }
      this.guidePreview = [this.rulerDragOrient, pos];
    } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.rulerDragOrient >= 0) {
      const backOnRuler = (horizontal ? t.y : t.x) < RULER_VP - 2;
      if (backOnRuler) {
        if (this.rulerDragMove) {
          this.engine.removeGuide(this.rulerDragOrient, this.rulerDragIndex);
          this.log('参考线已删除（拖回标尺）');
        }
      } else if (!this.rulerDragMove) {
        const pos = Math.round(Math.max(0, Math.min(docAlong, limit)));
        if (!this.engine.addGuide(this.rulerDragOrient, pos)) {
          this.toast('参考线创建失败（越界/重复/锁定）');
        } else {
          this.log(`参考线已创建 ${horizontal ? 'y' : 'x'}=${pos}`);
        }
      }
      this.rulerDragOrient = -2;
      this.rulerDragIndex = -1;
      this.rulerDragMove = false;
      this.guidePreview = [];
      this.refreshGuides();
    }
  }

  // 移动工具吸附修正量 [sx, sy]（文档域）：参考线优先、网格兜底；±4vp 容差（A-1）
  private computeMoveSnap(): number[] {
    const b = this.moveSnapBase;
    if (b === null || !b.ok) {
      return [0, 0];
    }
    const tolDoc = 4 * this.pxPerVp() / this.zoom;
    let sx = 0;
    let sy = 0;
    const candX: number[] = [b.x + this.moveAccX, b.x + b.w + this.moveAccX, b.x + b.w / 2 + this.moveAccX];
    const candY: number[] = [b.y + this.moveAccY, b.y + b.h + this.moveAccY, b.y + b.h / 2 + this.moveAccY];
    if (this.snapGuidesOn) {
      let bestD = tolDoc;
      for (const g of this.guidesMirror.v) {
        for (const c of candX) {
          const d = c - g;
          if (Math.abs(d) <= bestD) {
            bestD = Math.abs(d);
            sx = 0 - d;
          }
        }
      }
      bestD = tolDoc;
      for (const g of this.guidesMirror.h) {
        for (const c of candY) {
          const d = c - g;
          if (Math.abs(d) <= bestD) {
            bestD = Math.abs(d);
            sy = 0 - d;
          }
        }
      }
    }
    if (this.snapGridOn && this.gridSpacing > 0) {
      const g = this.gridSpacing;
      if (sx === 0) {
        for (const c of candX) {
          const d = c - Math.round(c / g) * g;
          if (Math.abs(d) <= tolDoc) {
            sx = 0 - d;
            break;
          }
        }
      }
      if (sy === 0) {
        for (const c of candY) {
          const d = c - Math.round(c / g) * g;
          if (Math.abs(d) <= tolDoc) {
            sy = 0 - d;
            break;
          }
        }
      }
    }
    return [sx, sy];
  }

  private refreshLayers(): void {
    this.layers = this.engine.getLayerListSnapshot();
    hilog.info(DOMAIN, TAG, 'layers: %{public}s', this.layers.map((l: LayerDTO): string => `${l.id}(${l.parentId})${l.isGroup ? "G" : ""}`).join(", "));
    // M14b：多选集清理（层被合并/删除后剔除失效 id）
    const liveIds = new Set<number>();
    for (const l of this.layers) {
      liveIds.add(l.id);
    }
    const pruned: number[] = [];
    for (const mid of this.multiSelected) {
      if (liveIds.has(mid)) {
        pruned.push(mid);
      }
    }
    if (pruned.length !== this.multiSelected.length) {
      this.multiSelected = pruned;
    }
    const active = this.activeLayer();
    if (active !== undefined) {
      this.blendSel = active.blendMode;
      this.layerOpacity = Math.round(active.opacity * 100);
      // M18：活动形状层 live 圆角镜像（撤销/换层后回退；非 live 矩形 = 0 隐藏滑条）
      this.shapeLiveRadius = (active.isShape && active.shapeLiveKind === 1) ? active.shapeRadii[0] : 0;
      this.hasCustomShapes = this.engine.getCustomShapes().length > 0;
      this.adjBlack = Math.round(active.adjBlack * 255);
      this.adjWhite = Math.round(active.adjWhite * 255);
      this.adjGamma = active.adjGamma;
      this.adjP0 = Math.round(active.adjP0);
      this.adjP1 = Math.round(active.adjP1);
      this.adjP2 = Math.round(active.adjP2);
      this.curveXs = active.curveX.length >= 2 ? active.curveX : [0, 1];
      this.curveYs = active.curveY.length >= 2 ? active.curveY : [0, 1];
    } else {
      this.blendSel = 0;
      this.layerOpacity = 100;
    }
    // M12：文本面板内容镜像跟随活动层（撤销/重做后回退，防旧内容残留）
    if (this.editingTextId === 0) {
      const at = this.layers.find((l: LayerDTO): boolean => l.isActive);
      this.textPanelContent = (at !== undefined && at.isText) ? at.textContent : '';
    }
    this.panelVersion++;  // M14a：面板树序变化强制重建（同 key 复用不移位实测坑）
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
    this.quickMaskActive = s.quickMask;  // M10d
    this.hasRawDevelop = s.hasRawDevelop;  // M15e：RAW 显影菜单使能镜像
    this.hasStyleClipboard = s.hasStyleClipboard;  // M17a：粘贴样式菜单使能镜像
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

  // 落墨族工具（Down/Move/Up 走 strokeTouch；stamp 仅 Alt+点击取源时 Down 不入笔）
  private isStrokeTool(): boolean {
    switch (this.tool) {
      case 'brush':
      case 'pencil':
      case 'eraser':
      case 'blur':
      case 'sharpen':
      case 'smudge':
      case 'dodge':
      case 'burn':
      case 'sponge':
      case 'historyBrush':
      case 'stamp':
      case 'heal':      // M19a 修复画笔（克隆式 live 预览 + 提交泊松融合）
      case 'spotHeal':  // M19a 污点修复（live 无预览，提交填充）
        return true;
      default:
        return false;
    }
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
    // 笔画工具按下即置位（在取色之前）：连笔时上一笔的提交/重样式仍持 docMutex，
    // Down 即时取色会同步等锁冻住 UI 线程
    if (e.type === TouchType.Down && this.isStrokeTool()) {
      this.strokeDrawing = true;
    }
    // M15b 信息面板：指针坐标/颜色（Up 即时采样，Move 节流；笔画进行中取色暂停）
    this.updatePointerInfo(docX, docY, e.type === TouchType.Up || e.type === TouchType.Down);

    if (this.navActive()) {
      return;  // 抓手/临时抓手：Pan 手势负责
    }
    // M15a：参考线拖动/Alt+删除（画布内命中截获，优先于工具分发；锁定时跳过）
    if (this.guidesMirror.visible && !this.guidesMirror.locked) {
      if (e.type === TouchType.Down && this.guideMoveOrient < 0) {
        const hit = this.hitGuide(docX, docY, 8 * k / this.zoom);
        if (hit !== null) {
          if (this.altHeld) {
            this.engine.removeGuide(hit[0], hit[1]);
            this.refreshGuides();
            this.log('参考线已删除（Alt+点击）');
          } else {
            this.guideMoveOrient = hit[0];
            this.guideMoveIndex = hit[1];
          }
          return;
        }
      } else if (this.guideMoveOrient >= 0) {
        if (e.type === TouchType.Move) {
          const pos = this.guideMoveOrient === 0
            ? Math.max(0, Math.min(docY, this.docH))
            : Math.max(0, Math.min(docX, this.docW));
          this.engine.moveGuide(this.guideMoveOrient, this.guideMoveIndex, pos);
          this.refreshGuides();
        } else if (e.type === TouchType.Up || e.type === TouchType.Cancel) {
          this.guideMoveOrient = -2;
          this.guideMoveIndex = -1;
        }
        return;
      }
    }
    switch (this.tool) {
      case 'brush':
      case 'pencil':
      case 'eraser':
      case 'blur':
      case 'sharpen':
      case 'smudge':
      case 'dodge':
      case 'burn':
      case 'sponge':
      case 'historyBrush':
        this.strokeTouch(e, t, docX, docY);
        break;
      case 'heal':
      case 'spotHeal':
        // M19a 修复族：修复画笔同图章取源（Alt+点击/取源模式）；污点无源直画
        if (this.tool === 'heal' && e.type === TouchType.Down &&
          (this.altHeld || this.clonePicking)) {
          if (this.engine.setCloneSource(docX, docY)) {
            this.cloneSourceSet = true;
            this.clonePicking = false;
            this.log(`修复源已设 (${docX.toFixed(0)},${docY.toFixed(0)})`);
          }
        } else {
          this.strokeTouch(e, t, docX, docY);
        }
        break;
      case 'patch':
        this.patchTouch(e, docX, docY);  // M19a 修补：选区内拖拽 → 位移取源
        break;
      case 'liquify':
        this.liquifyTouch(e, docX, docY);  // M20b 液化：拖拽收集一笔，松手即异步事务
        break;
      case 'puppet':
        this.puppetTouch(e, docX, docY);  // M20c 操控变形：点击=钉，拖钉=移动
        break;
      case 'perspective':
        this.perspectiveTouch(e, docX, docY);  // M20d 透视：拖四角
        break;
      case 'vanishing':
        this.vanishingTouch(e, docX, docY);
        break;
      case 'gradient':
        this.gradientTouch(e, docX, docY);
        break;
      case 'shape':
        this.shapeTouch(e, docX, docY);
        break;
      case 'pen':
        this.penTouch(e, docX, docY);
        break;
      case 'directSelect':
        this.dsTouch(e, docX, docY);
        break;
      case 'text':
        this.textTouch(e, docX, docY);
        break;
      case 'stamp':
        if (e.type === TouchType.Down && (this.altHeld || this.clonePicking)) {
          // Alt+点击 / 取源模式点击 = 取克隆源（PS Alt 语义 + 触屏兜底）
          if (this.engine.setCloneSource(docX, docY)) {
            this.cloneSourceSet = true;
            this.clonePicking = false;
            this.log(`克隆源已设 (${docX.toFixed(0)},${docY.toFixed(0)})`);
          }
        } else {
          this.strokeTouch(e, t, docX, docY);
        }
        break;
      case 'bucket':
        if (e.type === TouchType.Down) {
          const ok = this.engine.bucketFill(docX, docY, this.wandTolerance,
            this.wandContiguous, this.wandAllLayers ? 1 : 0, this.wandSampleSize);
          this.log(`油漆桶: (${docX.toFixed(0)},${docY.toFixed(0)}) tol=${this.wandTolerance} ` +
            `→ ${ok ? '已填充' : '失败'}`);
        }
        break;
      case 'move':
        this.moveTouch(e, docX, docY);
        break;
      case 'crop':
        this.cropTouch(e, docX, docY);
        break;
      case 'wand':
        if (e.type === TouchType.Down) {
          const ok = this.engine.setWandSelection(docX, docY, this.wandTolerance,
            this.wandContiguous, this.effectiveSelOp(), this.wandAllLayers ? 1 : 0,
            this.wandSampleSize);
          this.hasSelectionFlag = ok;
          this.log(`魔棒: (${docX.toFixed(0)},${docY.toFixed(0)}) tol=${this.wandTolerance} ` +
            `contig=${this.wandContiguous} op=${this.effectiveSelOp()} → ${ok ? '选区已生成' : '失败'}`);
        }
        break;
      case 'lasso':
        this.lassoTouch(e, docX, docY);
        break;
      case 'polyLasso':
        this.polyLassoTouch(e, docX, docY, t.x, t.y);
        break;
      case 'rectSelect':
        if (this.altHeld && this.hasSelectionFlag) {
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
        break;
    }
  }

  // M10a：布尔 op（选项条四态为基准；Shift 临时加 / Alt 临时减，PS 语义）
  private effectiveSelOp(): number {
    if (this.shiftHeld) {
      return 1;
    }
    if (this.altHeld) {
      return 2;
    }
    return this.selOp;
  }

  // M10a：选区命令后的会话态镜像维护（Replace 以结果为准；Add 成功必有选区）
  private updateSelFlag(op: number, ok: boolean): void {
    if (op === 0) {
      this.hasSelectionFlag = ok;
    } else if (ok) {
      this.hasSelectionFlag = true;
    }
  }

  // M10a：自由套索（拖拽收集折线，抬起自动闭合；偶奇规则）
  private lassoTouch(e: TouchEvent, docX: number, docY: number): void {
    if (e.type === TouchType.Down) {
      this.lassoActive = true;
      this.lassoPts = [docX, docY];
      this.lassoPreview = [docX, docY];
    } else if (e.type === TouchType.Move && this.lassoActive) {
      const n = this.lassoPts.length;
      const dx = docX - this.lassoPts[n - 2];
      const dy = docY - this.lassoPts[n - 1];
      if (dx * dx + dy * dy >= 4) {  // ≥2px 才记点（文档域）
        this.lassoPts.push(docX, docY);
        this.lassoPreview = this.lassoPts;
      }
    } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.lassoActive) {
      this.lassoActive = false;
      this.lassoPreview = [];
      if (this.lassoPts.length >= 6) {
        const op = this.effectiveSelOp();
        const ok = this.engine.setPolygonSelection(this.lassoPts, op);
        this.updateSelFlag(op, ok);
        this.log(`套索选区: ${this.lassoPts.length / 2} 点 op=${op} → ${ok}`);
      } else {
        this.log('套索：拖距过小，未生成选区');
      }
      this.lassoPts = [];
    }
  }

  // M10a：多边形套索（点击加点；点起点附近/双击闭合；Esc 取消）。
  // nearStart/双击判定用屏幕像素（文档域阈值随 zoom 缩小会不可命中，M10a 验证期修正）。
  private polyLassoTouch(e: TouchEvent, docX: number, docY: number, scrX: number, scrY: number): void {
    if (e.type !== TouchType.Down) {
      return;
    }
    const now = Date.now();
    const dbl = (now - this.polyLastDownTime < 400) &&
      Math.hypot(scrX - this.polyLastDownScrX, scrY - this.polyLastDownScrY) < 12;
    this.polyLastDownTime = now;
    this.polyLastDownScrX = scrX;
    this.polyLastDownScrY = scrY;
    const n = this.polyPendingPts.length;
    if (n >= 6) {
      // 点首点附近（屏幕 14px）或双击 → 闭合
      const k = this.pxPerVp();
      const fx = (this.polyPendingPts[0] - this.panX) * this.zoom / k;
      const fy = (this.polyPendingPts[1] - this.panY) * this.zoom / k;
      const nearStart = Math.hypot(scrX - fx, scrY - fy) < 14;
      if (nearStart || dbl) {
        this.commitPolygonSelection();
        return;
      }
    }
    if (dbl) {
      return;  // 双击但点数不足：忽略（避免误加点）
    }
    this.polyPendingPts.push(docX, docY);
    this.lassoPreview = this.polyPendingPts;
  }

  private commitPolygonSelection(): void {
    const pts = this.polyPendingPts;
    this.polyPendingPts = [];
    this.lassoPreview = [];
    if (pts.length >= 6) {
      const op = this.effectiveSelOp();
      const ok = this.engine.setPolygonSelection(pts, op);
      this.updateSelFlag(op, ok);
      this.log(`多边形套索: ${pts.length / 2} 点 op=${op} → ${ok}`);
    } else {
      this.log(`多边形套索提交取消：仅 ${pts.length / 2} 点（需≥3）`);
    }
  }

  private cancelPolygonSelection(): void {
    if (this.polyPendingPts.length > 0) {
      this.polyPendingPts = [];
      this.lassoPreview = [];
      this.log('多边形套索已取消');
    }
  }

  // M20c：钉 doc → 视口坐标
  private pinVpX(docX: number): number {
    return (docX - this.panX) * this.zoom / this.pxPerVp();
  }

  private pinVpY(docY: number): number {
    return (docY - this.panY) * this.zoom / this.pxPerVp();
  }

  // M20d：透视四角 doc → 视口闭合折线点列
  private perspectiveViewportPts(): number[] {
    return this.perspectiveViewportPtsOf(this.perspectiveQuad.slice(8));
  }

  private perspectiveViewportPtsOf(quad: number[]): number[] {
    const pts: number[] = [];
    for (let i = 0; i < 4; ++i) {
      pts.push(this.pinVpX(quad[i * 2]));
      pts.push(this.pinVpY(quad[i * 2 + 1]));
    }
    pts.push(this.pinVpX(quad[0]));
    pts.push(this.pinVpY(quad[1]));
    return pts;
  }

  // M10a：折线预览文档域 → 视口 points（Polyline 组件坐标 = vp）
  private lassoViewportPts(): number[][] {
    const k = this.pxPerVp();
    const out: number[][] = [];
    for (let i = 0; i + 1 < this.lassoPreview.length; i += 2) {
      out.push([(this.lassoPreview[i] - this.panX) * this.zoom / k,
                (this.lassoPreview[i + 1] - this.panY) * this.zoom / k]);
    }
    return out;
  }

  // M11a 渐变拖线视口换算（Polyline 两点）
  private gradientViewportPts(): number[][] {
    const line = this.gradientLine;
    const k = this.pxPerVp();
    if (line === null) {
      return [];
    }
    return [
      [(line[0] - this.panX) * this.zoom / k, (line[1] - this.panY) * this.zoom / k],
      [(line[2] - this.panX) * this.zoom / k, (line[3] - this.panY) * this.zoom / k],
    ];
  }

  private gradientViewportHandles(): number[][] {
    return this.gradientViewportPts();
  }

  private moveTouch(e: TouchEvent, docX: number, docY: number): void {
    // M19b 内容感知移动（caMoveMode 1=移动 2=扩展）：拖拽只累计位移，松手一步事务
    //（无实时预览——感知补全是秒级异步量级，登记 v1 取舍）
    if (this.caMoveMode > 0) {
      if (e.type === TouchType.Down) {
        if (!this.hasSelectionFlag) {
          this.log('感知移动：需先用选区工具建立选区');
          return;
        }
        this.caDragging = true;
        this.caStartX = docX;
        this.caStartY = docY;
      } else if (e.type === TouchType.Move && this.caDragging) {
        this.caCurX = docX;
        this.caCurY = docY;
      } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.caDragging) {
        this.caDragging = false;
        const dx = Math.round(this.caCurX - this.caStartX);
        const dy = Math.round(this.caCurY - this.caStartY);
        if (dx === 0 && dy === 0) {
          this.log('感知移动：位移为 0，已取消');
          return;
        }
        this.toast('感知移动计算中…');
        this.engine.contentAwareMove(dx, dy, this.caMoveMode === 2, (changed: boolean): void => {
          this.log(`感知移动(${this.caMoveMode === 2 ? '扩展' : '移动'}): ` +
            `(${dx},${dy}) → ${changed ? '已应用' : '失败（见日志）'}`);
        });
      }
      return;
    }
    if (e.type === TouchType.Down) {
      this.engine.translateLayer(0, 0, 0, 0);
      this.moveActive = true;
      this.moveLastX = docX;
      this.moveLastY = docY;
      // M15a 吸附基准：活动层视觉包围盒（含蒙版合成/样式派生 origin）
      this.moveSnapBase = this.engine.getLayerBounds(0);
      this.moveAccX = 0;
      this.moveAccY = 0;
      this.moveAppliedX = 0;
      this.moveAppliedY = 0;
    } else if (e.type === TouchType.Move && this.moveActive) {
      this.moveAccX += docX - this.moveLastX;
      this.moveAccY += docY - this.moveLastY;
      this.moveLastX = docX;
      this.moveLastY = docY;
      const snap = this.computeMoveSnap();
      const dx = this.moveAccX + snap[0] - this.moveAppliedX;
      const dy = this.moveAccY + snap[1] - this.moveAppliedY;
      if (Math.abs(dx) >= 0.5 || Math.abs(dy) >= 0.5) {
        this.engine.translateLayer(0, dx, dy, 1);
        this.moveAppliedX += dx;
        this.moveAppliedY += dy;
      }
    } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.moveActive) {
      this.moveActive = false;
      this.moveSnapBase = null;
      this.engine.translateLayer(0, 0, 0, 2);
      // M15a 验证日志：acc = 光标累计位移、applied = 吸附修正后实际位移（两者差 = 吸附量）
      this.log(`移动结束 acc=(${this.moveAccX.toFixed(1)},${this.moveAccY.toFixed(1)}) ` +
        `applied=(${this.moveAppliedX.toFixed(1)},${this.moveAppliedY.toFixed(1)})`);
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

  private strokeTouch(e: TouchEvent, t: TouchObject, docX: number, docY: number): void {
    const pressure: number = (t.pressure !== undefined && t.pressure > 0) ? t.pressure : 1.0;
    if (e.type === TouchType.Down) {
      this.pushBrushSettings();
      this.engine.beginStroke(docX, docY, pressure);
    } else if (e.type === TouchType.Move) {
      this.engine.continueStroke(docX, docY, pressure);
    } else if (e.type === TouchType.Up || e.type === TouchType.Cancel) {
      // 修复③：先立护栏再复位标志（Up 的取色因 strokeDrawing 未复位天然跳过；
      // 护栏覆盖收笔后第一波 Move/悬停，防撞笔画线程 commit/重样式临界区）
      this.strokeEndGuardUntil = Date.now() + 800;
      this.strokeDrawing = false;  // 提交在笔画线程异步进行，此刻取色仍可能撞长临界区，故先复位再收笔
      this.engine.endStroke();
    }
  }

  // M11c 形状拖拽：Down 定对角起点 / Move 更新预览 / Up 提交单事务
  private shapeTouch(e: TouchEvent, docX: number, docY: number): void {
    const k = this.pxPerVp();
    if (e.type === TouchType.Down) {
      this.shapeDragging = true;
      this.shapeStartDocX = docX;
      this.shapeStartDocY = docY;
      this.shapePreview = [(docX - this.panX) * this.zoom / k, (docY - this.panY) * this.zoom / k, 0, 0];
    } else if (e.type === TouchType.Move && this.shapeDragging) {
      const x0 = (this.shapeStartDocX - this.panX) * this.zoom / k;
      const y0 = (this.shapeStartDocY - this.panY) * this.zoom / k;
      const x1 = (docX - this.panX) * this.zoom / k;
      const y1 = (docY - this.panY) * this.zoom / k;
      this.shapePreview = [Math.min(x0, x1), Math.min(y0, y1),
        Math.abs(x1 - x0), Math.abs(y1 - y0)];
    } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.shapeDragging) {
      this.shapeDragging = false;
      this.shapePreview = null;
      const w = Math.abs(docX - this.shapeStartDocX);
      const h = Math.abs(docY - this.shapeStartDocY);
      if (w < 1 && h < 1) {
        this.toast('拖拽过短');
        return;
      }
      // M18：形状工具默认建形状层（非破坏矢量；live 可再编辑）
      const f = this.hexToRgb(this.brushColor);
      const b = this.hexToRgb(this.bgColor);
      const kind = this.engineShapeKind();  // 1=Rect 2=Ellipse 3=Polygon 4=Line
      const radius = this.shapeKind === 1 ? Math.min(w, h) * 0.25 : 0;
      const sides = this.shapeKind === 4 ? 5 : 6;
      const star = this.shapeKind === 4 ? 0.5 : 1.0;
      const weight = Math.max(4, this.shapeStrokeWidth);
      const newId = this.engine.createShapeLayer(kind, this.shapeStartDocX,
        this.shapeStartDocY, docX, docY, radius, sides, star, weight,
        this.shapeFill ? 1 : 0, f[0], f[1], f[2], 1.0,
        this.shapeStroke ? 1 : 0, b[0], b[1], b[2], 1.0, this.shapeStrokeWidth);
      this.log(`形状层(${this.shapeKindLabel()}): ${newId > 0 ? `已创建 #${newId}` : '失败'}`);
      if (newId > 0) {
        this.refreshLayers();
      }
    }
  }

  /** M18：live 矩形圆角再编辑（选项条滑条；一步撤销）。 */
  private applyShapeRadius(v: number): void {
    const a = this.activeLayer();
    if (a === undefined || !a.isShape || a.shapeLiveKind !== 1) {
      return;
    }
    const x0 = a.shapeRect[0];
    const y0 = a.shapeRect[1];
    const ok = this.engine.setShapeLive(a.id, 1, x0, y0, x0 + a.shapeRect[2], y0 + a.shapeRect[3],
      Math.max(0, v), a.shapeSides, a.shapeStar, a.shapeWeight);
    this.log(`圆角=${v.toFixed(0)}: ${ok ? '已更新' : '失败'}`);
  }

  // ---------- M18c 钢笔工具 ----------

  // 点击加角点锚点；点击首锚点附近闭合；选项条按钮提交（成形状层/存为路径/取消）
  private penTouch(e: TouchEvent, docX: number, docY: number): void {
    if (e.type !== TouchType.Down) {
      return;
    }
    if (this.penClosed) {
      return;  // 已闭合待提交
    }
    const k = this.pxPerVp();
    const tol = 12 / this.zoom * k / this.zoom;  // 视口 12vp 对应文档距离
    const n = this.penPoints.length;
    if (n >= 6) {
      const dx = docX - this.penPoints[0];
      const dy = docY - this.penPoints[1];
      if (Math.sqrt(dx * dx + dy * dy) <= 12 * k / this.zoom) {
        this.penClosed = true;
        this.pushWorkPath();
        this.log(`钢笔: 路径已闭合（${n / 2} 锚点）`);
        return;
      }
    }
    this.penPoints.push(docX);
    this.penPoints.push(docY);
    this.pushWorkPath();
  }

  /** 进行中锚点 → 引擎工作路径（扁平 op 格式；会话态无历史事务）。 */
  private pushWorkPath(): void {
    const n = this.penPoints.length / 2;
    if (n === 0) {
      this.engine.setWorkPath([]);
      this.workPathNonEmpty = 0;
      return;
    }
    const flat: number[] = [1, this.penClosed ? 1 : 0, 0, n];
    for (let i = 0; i + 1 < this.penPoints.length; i += 2) {
      const x = this.penPoints[i];
      const y = this.penPoints[i + 1];
      flat.push(x, y, x, y, x, y, 0);  // 角点：锚点=双柄
    }
    this.engine.setWorkPath(flat);
    this.workPathNonEmpty = 1;
  }

  // ---------- M18.1 直接选择（几何镜像内核 vector_edit 公式——宿主 test_vector 为 oracle） ----------

  /** 工作路径重载（工具切入/文档版本变更/换层）。非形状层清空编辑态。 */
  private dsReloadPath(): void {
    const a = this.activeLayer();
    if (a === undefined || !a.isShape) {
      this.dsPath = [];
      this.dsSelected = [];
      this.dsHandleOf = [];
      this.dsRefreshPreview();
      return;
    }
    this.dsPath = this.engine.getShapePath(a.id);
    this.dsSelected = [];
    this.dsHandleOf = [];
    this.dsLoadedRev = this.docVersion;
    this.dsRefreshPreview();
  }

  /** 扁平路径 → 可变结构（knots = [ax,ay,ix,iy,ox,oy,smooth]×n）。 */
  private dsParse(flat: number[]): number[][][] {
    const subs: number[][][] = [];
    if (flat.length < 1) {
      return subs;
    }
    const subCount = Math.floor(flat[0]);
    let idx = 1;
    for (let si = 0; si < subCount && idx + 3 <= flat.length; si++) {
      const closed = flat[idx] > 0.5;
      const op = flat[idx + 1];
      const knotCount = Math.floor(flat[idx + 2]);
      idx += 3;
      const sub: number[][] = [closed ? [1] : [0], [op]];
      const knots: number[] = [];
      for (let k = 0; k < knotCount && idx + 7 <= flat.length; k++) {
        for (let v = 0; v < 7; v++) {
          knots.push(flat[idx + v]);
        }
        idx += 7;
      }
      sub.push(knots);
      subs.push(sub);
    }
    return subs;
  }

  /** 可变结构 → 扁平路径（setShapePath 跨桥格式）。 */
  private dsSerialize(subs: number[][][]): number[] {
    const flat: number[] = [subs.length];
    for (const sub of subs) {
      const knots = sub[2];
      flat.push(sub[0][0], sub[1][0], knots.length / 7);
      for (const v of knots) {
        flat.push(v);
      }
    }
    return flat;
  }

  /** 三次贝塞尔求值（seg = [p0,c1,c2,p1] 扁平 8 数）。 */
  private dsEval(seg: number[], t: number): number[] {
    const mt = 1.0 - t;
    const a = mt * mt * mt;
    const b = 3.0 * mt * mt * t;
    const c = 3.0 * mt * t * t;
    const d = t * t * t;
    return [a * seg[0] + b * seg[2] + c * seg[4] + d * seg[6],
      a * seg[1] + b * seg[3] + c * seg[5] + d * seg[7]];
  }

  /** 子路径段列表 [p0,c1,c2,p1]（每段 8 数，含段序）。 */
  private dsSegments(sub: number[][]): number[][] {
    const knots = sub[2];
    const n = knots.length / 7;
    const out: number[][] = [];
    if (n < 2) {
      return out;
    }
    const count = sub[0][0] > 0.5 ? n : n - 1;
    for (let i = 0; i < count; i++) {
      const a = i * 7;
      const b = ((i + 1) % n) * 7;
      out.push([knots[a], knots[a + 1], knots[a + 3], knots[a + 4],
        knots[b + 2], knots[b + 3], knots[b], knots[b + 1]]);
    }
    return out;
  }

  /** 平滑 knot 保持对侧柄共线等长（内核 keepSmooth 同式）。 */
  private dsKeepSmooth(knots: number[], ki: number, movedIn: boolean): void {
    if (knots[ki + 6] < 0.5) {
      return;
    }
    const ax = knots[ki], ay = knots[ki + 1];
    const mx = movedIn ? knots[ki + 2] : knots[ki + 4];
    const my = movedIn ? knots[ki + 3] : knots[ki + 5];
    const oi = movedIn ? ki + 4 : ki + 2;
    const len = Math.hypot(knots[oi] - ax, knots[oi + 1] - ay);
    const d = Math.hypot(mx - ax, my - ay);
    if (len > 1e-9 && d > 1e-9) {
      knots[oi] = ax - (mx - ax) / d * len;
      knots[oi + 1] = ay - (my - ay) / d * len;
    }
  }

  /** 预览重建：路径抽样折线 + 锚点 + 单选手柄（视口坐标）。 */
  private dsRefreshPreview(): void {
    const k = this.pxPerVp();
    const toVp = (x: number, y: number): number[] =>
      [(x - this.panX) * this.zoom / k, (y - this.panY) * this.zoom / k];
    const pts: number[][] = [];
    const anchors: number[][] = [];
    const handles: number[][] = [];
    const subs = this.dsParse(this.dsPath);
    for (let si = 0; si < subs.length; si++) {
      const segs = this.dsSegments(subs[si]);
      for (const seg of segs) {
        for (let j = 0; j <= 12; j++) {
          const p = this.dsEval(seg, j / 12);
          pts.push(toVp(p[0], p[1]));
        }
      }
      const knots = subs[si][2];
      const n = knots.length / 7;
      for (let ki = 0; ki < n; ki++) {
        let sel = 0;
        for (let s2 = 0; s2 + 1 < this.dsSelected.length; s2 += 2) {
          if (this.dsSelected[s2] === si && this.dsSelected[s2 + 1] === ki) {
            sel = 1;
          }
        }
        const vp = toVp(knots[ki * 7], knots[ki * 7 + 1]);
        anchors.push([vp[0], vp[1], sel]);
      }
    }
    if (this.dsHandleOf.length === 2) {
      const si = this.dsHandleOf[0];
      const ki = this.dsHandleOf[1];
      if (si < subs.length) {
        const knots = subs[si][2];
        if ((ki + 1) * 7 <= knots.length) {
          const vp = toVp(knots[ki * 7], knots[ki * 7 + 1]);
          for (const isIn of [true, false]) {
            const hx = isIn ? knots[ki * 7 + 2] : knots[ki * 7 + 4];
            const hy = isIn ? knots[ki * 7 + 3] : knots[ki * 7 + 5];
            const hv = toVp(hx, hy);
            handles.push([hv[0], hv[1], vp[0], vp[1]]);
          }
        }
      }
    }
    this.dsPreviewPts = pts;
    this.dsAnchorPts = anchors;
    this.dsHandlePts = handles;
  }

  /** 命中测试：单选手柄 → 选中锚点 → 任意锚点 → 段（64 采样）。返回 [kind,sub,knot,extra]。 */
  private dsHitTest(docX: number, docY: number, tol: number): number[] {
    const subs = this.dsParse(this.dsPath);
    // 手柄最上层（仅单选时显示）
    if (this.dsHandleOf.length === 2) {
      const si = this.dsHandleOf[0];
      const ki = this.dsHandleOf[1];
      if (si < subs.length) {
        const knots = subs[si][2];
        if ((ki + 1) * 7 <= knots.length) {
          for (const isIn of [true, false]) {
            const hx = isIn ? knots[ki * 7 + 2] : knots[ki * 7 + 4];
            const hy = isIn ? knots[ki * 7 + 3] : knots[ki * 7 + 5];
            if (Math.hypot(hx - docX, hy - docY) <= tol) {
              return [2, si, ki, isIn ? 1 : 0];
            }
          }
        }
      }
    }
    // 选中锚点 → 任意锚点
    for (let s2 = 0; s2 + 1 < this.dsSelected.length; s2 += 2) {
      const si = this.dsSelected[s2];
      const ki = this.dsSelected[s2 + 1];
      if (si < subs.length) {
        const knots = subs[si][2];
        if ((ki + 1) * 7 <= knots.length &&
          Math.hypot(knots[ki * 7] - docX, knots[ki * 7 + 1] - docY) <= tol) {
          return [1, si, ki, 0];
        }
      }
    }
    let bestAnchor: number[] = [];
    let bestD = tol;
    for (let si = 0; si < subs.length; si++) {
      const knots = subs[si][2];
      const n = knots.length / 7;
      for (let ki = 0; ki < n; ki++) {
        const d = Math.hypot(knots[ki * 7] - docX, knots[ki * 7 + 1] - docY);
        if (d <= bestD) {
          bestD = d;
          bestAnchor = [1, si, ki, 0];
        }
      }
    }
    if (bestAnchor.length > 0) {
      return bestAnchor;
    }
    // 段（64 采样最近，含参数 t）
    let bestSeg: number[] = [];
    let segD = tol;
    for (let si = 0; si < subs.length; si++) {
      const segs = this.dsSegments(subs[si]);
      for (let k = 0; k < segs.length; k++) {
        for (let j = 0; j <= 64; j++) {
          const t = j / 64;
          const pt = this.dsEval(segs[k], t);
          const d = Math.hypot(pt[0] - docX, pt[1] - docY);
          if (d <= segD) {
            segD = d;
            bestSeg = [4, si, k, t];
          }
        }
      }
    }
    return bestSeg;
  }

  /** 位移应用到选中锚点集（每个只动一次）。 */
  private dsMoveSelected(subs: number[][][], dx: number, dy: number): void {
    for (let s2 = 0; s2 + 1 < this.dsDragTargets.length; s2 += 2) {
      const si = this.dsDragTargets[s2];
      const ki = this.dsDragTargets[s2 + 1];
      if (si >= subs.length) {
        continue;
      }
      const knots = subs[si][2];
      if ((ki + 1) * 7 > knots.length) {
        continue;
      }
      for (const off of [0, 2, 4]) {
        knots[ki * 7 + off] += dx;
        knots[ki * 7 + off + 1] += dy;
      }
    }
  }

  /** 直接选择触控（文档域）。 */
  private dsTouch(e: TouchEvent, docX: number, docY: number): void {
    const k = this.pxPerVp();
    const tol = 12 * k / this.zoom;
    if (e.type === TouchType.Down) {
      if (this.docVersion !== this.dsLoadedRev) {
        this.dsReloadPath();  // 引擎侧已变（撤销/提交/外部编辑）→ 镜像以引擎为准
      }
      if (this.dsPath.length === 0) {
        if (this.dsPath.length === 0) {
          this.toast('直接选择：先选中形状层');
          return;
        }
      }
      const hit = this.dsHitTest(docX, docY, tol);
      this.dsDragStartDoc = [docX, docY];
      this.dsDragLastDoc = [docX, docY];
      if (hit.length === 0) {
        this.dsDragKind = 3;  // 框选
        this.dsMarqueeRect = [0, 0, 0, 0];
        return;
      }
      const kind = hit[0];
      if (kind === 2) {
        this.dsDragKind = 2;
        this.dsDragHandle = [hit[1], hit[2], hit[3]];
        return;
      }
      if (kind === 1) {
        // Shift+点击切换选中；点中未选锚点 = 单选它（含清空旧选）
        let already = false;
        for (let p2 = 0; p2 + 1 < this.dsSelected.length; p2 += 2) {
          if (this.dsSelected[p2] === hit[1] && this.dsSelected[p2 + 1] === hit[2]) {
            already = true;
          }
        }
        if (this.shiftHeld) {
          if (already) {
            const at = this.dsSelected.indexOf(hit[1]);
            this.dsSelected.splice(at, 2);
          } else {
            this.dsSelected.push(hit[1], hit[2]);
          }
        } else if (!already) {
          this.dsSelected = [hit[1], hit[2]];
        }
        this.dsHandleOf = this.dsSelected.length === 2 ? [hit[1], hit[2]] : [];
        this.dsDragKind = 1;
        this.dsDragTargets = this.dsSelected.slice();
        this.dsRefreshPreview();
        return;
      }
      if (kind === 4) {
        this.dsDragKind = 4;
        this.dsDragSeg = [hit[1], hit[2], hit[3]];
        return;
      }
    } else if (e.type === TouchType.Move && this.dsDragKind !== 0) {
      const dx = docX - this.dsDragLastDoc[0];
      const dy = docY - this.dsDragLastDoc[1];
      this.dsDragLastDoc = [docX, docY];
      const subs = this.dsParse(this.dsPath);
      if (this.dsDragKind === 1) {
        this.dsMoveSelected(subs, dx, dy);
      } else if (this.dsDragKind === 2) {
        const si = this.dsDragHandle[0];
        const ki = this.dsDragHandle[1];
        if (si < subs.length) {
          const knots = subs[si][2];
          if ((ki + 1) * 7 <= knots.length) {
            const isIn = this.dsDragHandle[2] > 0.5;
            const off = isIn ? 2 : 4;
            knots[ki * 7 + off] += dx;
            knots[ki * 7 + off + 1] += dy;
            if (this.altHeld) {
              knots[ki * 7 + 6] = 0;  // Alt 拖柄 = 断开平滑（内核 independent）
            }
            this.dsKeepSmooth(knots, ki * 7, isIn);
          }
        }
      } else if (this.dsDragKind === 4) {
        // 拖段：权重公式（内核 bendSegment；t 钳 0.05..0.95）
        const si = this.dsDragSeg[0];
        const segIdx = this.dsDragSeg[1];
        if (si < subs.length) {
          const knots = subs[si][2];
          const n = knots.length / 7;
          const closed = subs[si][0][0] > 0.5;
          const a = segIdx * 7;
          const bIdx = (segIdx + 1) % n;
          const b = bIdx * 7;
          const outRetracted = Math.hypot(knots[a + 4] - knots[a], knots[a + 5] - knots[a + 1]) < 1e-9;
          const inRetracted = Math.hypot(knots[b + 2] - knots[b], knots[b + 3] - knots[b + 1]) < 1e-9;
          if (outRetracted && inRetracted) {
            for (const off of [0, 2, 4]) {
              knots[a + off] += dx;
              knots[a + off + 1] += dy;
              knots[b + off] += dx;
              knots[b + off + 1] += dy;
            }
          } else {
            const t = Math.min(Math.max(this.dsDragSeg[2], 0.05), 0.95);
            const u = 1.0 - t;
            const sc = 1.0 / (3.0 * t * u * (u * u + t * t));
            const w1 = u * sc;
            const w2 = t * sc;
            knots[a + 4] += dx * w1;
            knots[a + 5] += dy * w1;
            this.dsKeepSmooth(knots, a, false);
            knots[b + 2] += dx * w2;
            knots[b + 3] += dy * w2;
            this.dsKeepSmooth(knots, b, true);
          }
        }
      } else if (this.dsDragKind === 3) {
        const x0v = (Math.min(this.dsDragStartDoc[0], docX) - this.panX) * this.zoom / k;
        const y0v = (Math.min(this.dsDragStartDoc[1], docY) - this.panY) * this.zoom / k;
        this.dsMarqueeRect = [x0v, y0v,
          Math.abs(docX - this.dsDragStartDoc[0]) * this.zoom / k,
          Math.abs(docY - this.dsDragStartDoc[1]) * this.zoom / k];
        return;
      }
      this.dsPath = this.dsSerialize(subs);
      this.dsRefreshPreview();
    } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.dsDragKind !== 0) {
      if (this.dsDragKind === 3) {
        // 框选收口：盒内锚点全选
        const x0 = Math.min(this.dsDragStartDoc[0], docX);
        const x1 = Math.max(this.dsDragStartDoc[0], docX);
        const y0 = Math.min(this.dsDragStartDoc[1], docY);
        const y1 = Math.max(this.dsDragStartDoc[1], docY);
        const sel: number[] = [];
        const subs = this.dsParse(this.dsPath);
        for (let si = 0; si < subs.length; si++) {
          const knots = subs[si][2];
          const n = knots.length / 7;
          for (let ki = 0; ki < n; ki++) {
            const ax = knots[ki * 7];
            const ay = knots[ki * 7 + 1];
            if (ax >= x0 && ax <= x1 && ay >= y0 && ay <= y1) {
              sel.push(si, ki);
            }
          }
        }
        this.dsSelected = sel;
        this.dsHandleOf = sel.length === 2 ? [sel[0], sel[1]] : [];
        this.dsMarqueeRect = null;
        this.dsRefreshPreview();
      } else if (this.dsDragKind !== 0) {
        // 提交（一步「编辑路径」事务；live 清 None——PS 直接选择同语义）
        const a = this.activeLayer();
        if (a !== undefined && a.isShape) {
          const ok = this.engine.setShapePath(a.id, this.dsPath);
          this.log(`编辑路径: ${ok ? '已提交' : '失败'}`);
        }
      }
      this.dsDragKind = 0;
    }
  }

  /** 删除选中锚点（内核 deleteAnchor 语义：子路径末锚删子路径；全路径末锚拒绝）。 */
  private dsDeleteSelected(): void {
    if (this.dsSelected.length === 0) {
      return;
    }
    const subs = this.dsParse(this.dsPath);
    let total = 0;
    for (const sub of subs) {
      total += sub[2].length / 7;
    }
    if (total <= this.dsSelected.length / 2) {
      this.toast('不能删除路径的全部锚点');
      return;
    }
    // 从后往前删（索引不漂移）
    const order: number[] = this.dsSelected.slice();
    const pairs: number[][] = [];
    for (let i = 0; i + 1 < order.length; i += 2) {
      pairs.push([order[i], order[i + 1]]);
    }
    pairs.sort((p, q): number => q[1] - p[1] !== 0 ? q[1] - p[1] : q[0] - p[0]);
    for (const pr of pairs) {
      if (pr[0] >= subs.length) {
        continue;
      }
      const knots = subs[pr[0]][2];
      if (knots.length / 7 <= 1) {
        subs.splice(pr[0], 1);
      } else {
        knots.splice(pr[1] * 7, 7);
      }
    }
    this.dsPath = this.dsSerialize(subs);
    this.dsSelected = [];
    this.dsHandleOf = [];
    this.dsRefreshPreview();
    const a = this.activeLayer();
    if (a !== undefined && a.isShape) {
      const ok = this.engine.setShapePath(a.id, this.dsPath);
      this.log(`删除锚点: ${ok ? '已提交' : '失败'}`);
    }
  }

  /** 视口锚点/手柄/折线 → 触摸事件前的路径预览换算（同 pen 预览）。 */
  private dsViewportPts(): number[][] {
    return this.dsPreviewPts;
  }
  private penCancel(): void {
    this.penPoints = [];
    this.penClosed = false;
    this.engine.setWorkPath([]);
    this.workPathNonEmpty = 0;
    this.log('钢笔: 已取消');
  }

  private penCommitShape(): void {
    if (this.penPoints.length < 6) {
      this.toast('至少 3 个锚点');
      return;
    }
    if (!this.penClosed) {
      this.penClosed = true;
      this.pushWorkPath();
    }
    const f = this.hexToRgb(this.brushColor);
    const newId = this.engine.commitWorkPathAsShape(1, f[0], f[1], f[2], 1.0,
      0, 0, 0, 0, 0, 3);
    this.log(`钢笔→形状层: ${newId > 0 ? `#${newId}` : '失败'}`);
    this.penPoints = [];
    this.penClosed = false;
    if (newId > 0) {
      this.refreshLayers();
    }
  }

  private penSavePath(): void {
    if (this.penPoints.length < 6) {
      this.toast('至少 3 个锚点');
      return;
    }
    if (!this.penClosed) {
      this.penClosed = true;
      this.pushWorkPath();
    }
    if (this.engine.saveWorkPath('')) {
      this.penPoints = [];
      this.penClosed = false;
      this.log('钢笔: 已存为路径');
    }
  }

  /** 钢笔预览视口点列（Polyline）。 */
  private penViewportPts(): number[][] {
    const k = this.pxPerVp();
    const out: number[][] = [];
    for (let i = 0; i + 1 < this.penPoints.length; i += 2) {
      out.push([(this.penPoints[i] - this.panX) * this.zoom / k,
                (this.penPoints[i + 1] - this.panY) * this.zoom / k]);
    }
    return out;
  }

  /** UI shapeKind（0=矩形 1=圆角 2=椭圆 3=多边形 4=星形 5=直线）→ 引擎 LiveShape kind。 */
  private engineShapeKind(): number {
    switch (this.shapeKind) {
      case 2:
        return 2;  // Ellipse
      case 3:
      case 4:
        return 3;  // Polygon/Star
      case 5:
        return 4;  // Line
      default:
        return 1;  // Rect/RoundedRect
    }
  }

  // ---------- M12 文字工具 ----------

  // 点击 = 点文字（点击处建层进编辑）；拖框 = 段落文字（拖宽 = 换行宽）
  private textTouch(e: TouchEvent, docX: number, docY: number): void {
    const k = this.pxPerVp();
    if (e.type === TouchType.Down) {
      this.textDragging = true;
      this.textStartDocX = docX;
      this.textStartDocY = docY;
      this.textDrag = [(docX - this.panX) * this.zoom / k, (docY - this.panY) * this.zoom / k, 0, 0];
    } else if (e.type === TouchType.Move && this.textDragging) {
      const x0 = (this.textStartDocX - this.panX) * this.zoom / k;
      const y0 = (this.textStartDocY - this.panY) * this.zoom / k;
      const x1 = (docX - this.panX) * this.zoom / k;
      const y1 = (docY - this.panY) * this.zoom / k;
      this.textDrag = [Math.min(x0, x1), Math.min(y0, y1), Math.abs(x1 - x0), Math.abs(y1 - y0)];
    } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.textDragging) {
      this.textDragging = false;
      this.textDrag = null;
      const wDoc = Math.abs(docX - this.textStartDocX);
      const hDoc = Math.abs(docY - this.textStartDocY);
      const dragVp = Math.hypot(wDoc, hDoc) * this.zoom / k;
      if (dragVp < 8) {
        this.enterTextEditingAt(this.textStartDocX, this.textStartDocY, 0);
      } else {
        this.enterTextEditingAt(Math.min(this.textStartDocX, docX),
          Math.min(this.textStartDocY, docY), wDoc);
      }
    }
  }

  private enterTextEditingAt(docX: number, docY: number, wrapW: number): void {
    if (!this.docOpen) {
      return;
    }
    if (this.editingTextId !== 0) {
      this.commitTextEditing();  // 已在编辑：先提交旧层
    }
    const id = this.engine.createTextLayer(docX, docY, wrapW);
    if (id === 0) {
      this.toast('创建文本层失败');
      return;
    }
    this.refreshLayers();
    this.log(`文本层已建 #${id} (${docX.toFixed(0)},${docY.toFixed(0)}) wrap=${wrapW.toFixed(0)}`);
    this.beginTextEditing(id);
  }

  // 进入文本编辑会话（引擎 begin 持 history pending 槽；GL 隐藏该层，overlay 接管视觉）
  private beginTextEditing(id: number): void {
    const a = this.layers.find((l: LayerDTO): boolean => l.id === id);
    if (a === undefined || !a.isText) {
      return;
    }
    if (!this.engine.beginTextSession(id)) {
      this.toast('进入文本编辑失败');
      return;
    }
    this.ensureFontList();
    this.syncTextPanel(a);
    this.textPanelContent = a.textContent;
    this.editingTextId = id;
    this.log(`文本编辑开始 #${id}`);
  }

  // 提交编辑会话（endEdit：before==after 自动丢弃）
  private commitTextEditing(): void {
    if (this.editingTextId === 0) {
      return;
    }
    if (this.textEditTimer >= 0) {
      clearTimeout(this.textEditTimer);
      this.textEditTimer = -1;
    }
    this.editingTextId = 0;
    this.engine.endTextSession();
    this.refreshLayers();
    this.log('文本编辑提交');
  }

  // 面板镜像 ← 活动文本层 DTO
  private syncTextPanel(a: LayerDTO): void {
    if (!a.isText) {
      return;
    }
    this.panelFontSel = a.textFontFamily === ''
      ? -1 : this.fontOptions().indexOf(a.textFontFamily);
    this.textFontSize = Math.round(a.textFontSize > 0 ? a.textFontSize : 40);
    this.textLineHeightPct = Math.round((a.textLineHeight > 0 ? a.textLineHeight : 1.2) * 100);
    this.textTracking = Math.round(a.textTracking);
    this.textBold = a.textBold;
    this.textItalic = a.textItalic;
    this.textColor = a.textColor;
    this.textAlignIdx = a.textAlign;
  }

  // 面板/样式改动 → 引擎（编辑会话中 txn=0，否则独立事务）
  private pushTextStyle(txnOverride?: boolean): void {
    const a = this.activeLayer();
    if (a === undefined || !a.isText) {
      return;
    }
    const txn = txnOverride !== undefined ? txnOverride : this.editingTextId === 0;
    const rgb = this.hexToRgb(this.textColor);
    const ok = this.engine.setTextLayerStyle(a.id, this.panelFontSel >= 0
      ? this.fontOptions()[this.panelFontSel] : '', this.textFontSize,
      this.textLineHeightPct / 100, this.textTracking, this.textBold, this.textItalic,
      rgb[0], rgb[1], rgb[2], this.textAlignIdx,
      a.textWrapWidth, txn);
    this.log(`文本样式: ${ok ? '已应用' : '失败'} (size=${this.textFontSize} lh=${this.textLineHeightPct}%)`);
  }

  // 面板内容提交（编辑会话中 = 会话内改动 txn=0；否则独立事务 txn=1）
  private pushTextContent(text: string): void {
    const a = this.activeLayer();
    if (a === undefined || !a.isText) {
      return;
    }
    const txn = this.editingTextId !== a.id;
    this.engine.setTextLayerContent(a.id, text, txn);
    this.log(`文本内容提交: ${text.length} 字符 (txn=${txn ? 1 : 0})`);
  }

  // 字体列表懒加载（面板首次可见时拉一次）
  private ensureFontList(): void {
    if (this.panelFontList.length > 0) {
      return;
    }
    this.panelFontList = this.engine.listSystemFonts();
    this.log(`系统字体: ${this.panelFontList.length} 族`);
  }

  // M12e：字体下拉选项 = 系统 + 已导入用户字体
  private fontOptions(): string[] {
    return this.panelFontList.concat(this.userFonts);
  }

  // M12e：导入用户字体（picker 选 ttf/otf → 拷沙箱 fonts/ → 注册 FontCollection）
  private doImportFont(): void {
    const opt = new picker.DocumentSelectOptions();
    opt.maxSelectNumber = 1;
    opt.defaultFilePathUri = 'file://docs/storage/Users/currentUser/Download';
    const dp = new picker.DocumentViewPicker();
    dp.select(opt).then((uris: string[]): void => {
      if (uris.length < 1) {
        return;
      }
      const uri = uris[0];
      const lower = uri.toLowerCase();
      if (!lower.endsWith('.ttf') && !lower.endsWith('.otf') && !lower.endsWith('.ttc')) {
        this.toast('请选择 .ttf / .otf / .ttc 字体文件');
        return;
      }
      const seg = decodeURIComponent(uri.split('/').pop() ?? 'font.ttf');
      const alias = seg.replace(/\.(ttf|otf|ttc)$/i, '');
      const fontsDir = this.cachePath('fonts');
      if (!fs.accessSync(fontsDir)) {
        fs.mkdirSync(fontsDir, true);
      }
      const dest = `${fontsDir}/${seg}`;
      fs.copyFile(uri, dest).then((): void => {
        if (this.engine.registerFontFile(alias, dest)) {
          if (!this.userFonts.includes(alias)) {
            this.userFonts = this.userFonts.concat([alias]);
          }
          this.toast(`字体已导入：${alias}`);
          this.log(`用户字体注册: ${alias} <- ${dest}`);
        } else {
          this.toast('字体注册失败');
        }
      }).catch((e: BusinessError): void => {
        this.log(`字体拷贝失败: ${e.message}`);
        this.toast('字体文件拷贝失败');
      });
    }).catch((e: BusinessError): void => {
      this.log(`字体 picker 失败: ${e.message}`);
    });
  }

  // M22：载入查找表（picker 选 .cube/.3dl/.look → importLookupTable 进程槽 →
  // 颜色查找滤镜 chips 可选；util.Base64Helper 编码跨桥）
  private doImportLookupTable(): void {
    const opt = new picker.DocumentSelectOptions();
    opt.maxSelectNumber = 1;
    opt.defaultFilePathUri = 'file://docs/storage/Users/currentUser/Download';
    const dp = new picker.DocumentViewPicker();
    dp.select(opt).then((uris: string[]): void => {
      if (uris.length < 1) {
        return;
      }
      const uri = uris[0];
      const lower = uri.toLowerCase();
      if (!lower.endsWith('.cube') && !lower.endsWith('.3dl') && !lower.endsWith('.look')) {
        this.toast('请选择 .cube / .3dl / .look 查找表文件');
        return;
      }
      const seg = decodeURIComponent(uri.split('/').pop() ?? 'lookup.cube');
      try {
        const f = fs.openSync(uri, fs.OpenMode.READ_ONLY);
        const stat = fs.statSync(f.fd);
        const buf = new ArrayBuffer(stat.size);
        fs.readSync(f.fd, buf);
        fs.closeSync(f);
        const b64 = new util.Base64Helper().encodeToStringSync(new Uint8Array(buf));
        this.engine.importLookupTable(seg, b64).then((d: LookupImportData): void => {
          this.lookupPresets = this.lookupPresets.concat([{
            id: `user${d.slot}`, label: d.title.length > 0 ? d.title : seg, param: d.slot,
          }]);
          this.toast(`查找表已载入：${d.title.length > 0 ? d.title : seg}`);
          this.log(`查找表导入: ${seg} -> p0=${d.slot}`);
        }).catch((e: BusinessError): void => {
          this.log(`查找表解析失败: ${e.message}`);
          this.toast(`查找表解析失败: ${e.message}`);
        });
      } catch (e) {
        const be = e as BusinessError;
        this.log(`查找表读取失败: ${be.message}`);
        this.toast('查找表文件读取失败');
      }
    }).catch((e: BusinessError): void => {
      this.log(`查找表 picker 失败: ${e.message}`);
    });
  }

  // overlay RichEditor → 纯文本（span 值拼接；图片 span 无 value 字段自然跳过）
  private collectOverlayText(): string {
    let out = '';
    for (const sp of this.richController.getSpans()) {
      const ts = sp as RichEditorTextSpanResult;
      if (typeof ts.value === 'string') {
        out += ts.value;
      }
    }
    return out;
  }

  // ---------- M13 滤镜对话框 ----------

  private openFilterDialog(def: FilterDef): void {
    const a = this.activeLayer();
    if (!this.docOpen || a === undefined) {
      this.toast('无文档或无活动图层');
      return;
    }
    if (a.isText) {
      this.toast('文字层请先栅格化再应用滤镜');
      return;
    }
    if (a.isSmart) {
      // M21：SO 层 = 智能滤镜化（无实时预览；编辑模式预填当前参数）
      this.filterDef = def;
      if (this.smartEditIndex >= 0 && this.smartInfo !== null &&
          this.smartEditIndex < this.smartInfo.filters.length) {
        const f: SmartFilterDTO = this.smartInfo.filters[this.smartEditIndex];
        this.filterP = def.params.map((d: FilterParamDef, i: number): number =>
          f.p[i] ?? d.def);
        this.filterSeed = f.seed > 0 ? f.seed : 1;
      } else {
        this.filterP = def.params.map((d: FilterParamDef): number => d.def);
        this.filterSeed = 1;
      }
      this.showFilter = true;
      this.log(`智能滤镜对话框: ${def.label}${this.smartEditIndex >= 0 ? '（编辑）' : ''}`);
      return;
    }
    if (!this.engine.beginFilterPreview(a.id)) {
      this.toast('滤镜预览创建失败（空图层？）');
      return;
    }
    this.filterTargetLayer = a.id;
    this.filterDef = def;
    this.filterP = def.params.map((d: FilterParamDef): number => d.def);
    this.filterSeed = 1;
    this.showFilter = true;
    this.log(`滤镜预览: ${def.label}`);
    this.pushFilterPreview();
  }

  private pushFilterPreview(): void {
    if (!this.showFilter || this.filterDef === null) {
      return;
    }
    const p = this.filterP;
    const vals: number[] = [p[0] ?? 0, p[1] ?? 0, p[2] ?? 0, p[3] ?? 0, p[4] ?? 0, p[5] ?? 0];
    this.engine.setFilterPreview(this.filterDef.kind, vals[0], vals[1], vals[2], vals[3],
      vals[4], vals[5], this.filterSeed);
  }

  // 滑条拖动 debounce 150ms → proxy 跑核（D-2 预算）
  private onFilterParamChange(idx: number, v: number): void {
    if (idx >= this.filterP.length) {
      return;
    }
    this.filterP = this.filterP.map((old: number, i: number): number => i === idx ? v : old);
    if (this.filterDebounce >= 0) {
      clearTimeout(this.filterDebounce);
    }
    this.filterDebounce = setTimeout((): void => {
      this.filterDebounce = -1;
      this.pushFilterPreview();
    }, 150);
  }

  private commitFilterDialog(): void {
    if (!this.showFilter || this.filterDef === null) {
      return;
    }
    const def = this.filterDef;
    const p = this.filterP;
    const vals: number[] = [p[0] ?? 0, p[1] ?? 0, p[2] ?? 0, p[3] ?? 0, p[4] ?? 0, p[5] ?? 0];
    if (this.filterDebounce >= 0) {
      clearTimeout(this.filterDebounce);
      this.filterDebounce = -1;
    }
    this.showFilter = false;
    const a = this.activeLayer();
    if (a !== undefined && a.isSmart) {
      // M21：智能滤镜化——新滤镜走 addSmartFilter（选区自动成滤镜蒙版，PS 语义），
      // 编辑模式走 setSmartFilter；无 1/4 代理预览管线（登记）
      if (this.smartEditIndex >= 0) {
        this.engine.setSmartFilter(a.id, this.smartEditIndex, vals, this.filterSeed, 0, 1.0,
          true, def.label).then((ok: boolean): void => {
            this.log(`智能滤镜已更新: ${def.label} (${ok})`);
          });
      } else {
        this.engine.addSmartFilter(a.id, def.kind, vals, this.filterSeed, 0, 1.0, def.label)
          .then((ok: boolean): void => {
            this.log(`智能滤镜已应用: ${def.label} (${ok})`);
            this.toast('已作为智能滤镜应用');
          });
      }
      this.smartEditIndex = -1;
      this.filterDef = null;
      return;
    }
    this.engine.endFilterPreview(true, def.kind, vals[0], vals[1], vals[2], vals[3], vals[4],
      vals[5], this.filterSeed, def.label);
    // M24a：滤镜提交 = begin+end 两段会话原语的组合命令，动作步存单条 filter.apply
    if (this.filterTargetLayer !== 0) {
      automation.note('filter.apply', {
        'layerId': this.filterTargetLayer, 'kind': def.kind, 'p0': vals[0], 'p1': vals[1],
        'p2': vals[2], 'p3': vals[3], 'p4': vals[4], 'p5': vals[5], 'seed': this.filterSeed,
        'label': def.label,
      });
    }
    this.log(`滤镜已应用: ${def.label}`);
    this.filterDef = null;
  }

  private cancelFilterDialog(): void {
    if (!this.showFilter) {
      return;
    }
    if (this.filterDebounce >= 0) {
      clearTimeout(this.filterDebounce);
      this.filterDebounce = -1;
    }
    this.showFilter = false;
    const def = this.filterDef;
    const a = this.activeLayer();
    if (def !== null && a !== undefined && !a.isSmart) {
      const p = this.filterP;
      const vals: number[] = [p[0] ?? 0, p[1] ?? 0, p[2] ?? 0, p[3] ?? 0, p[4] ?? 0, p[5] ?? 0];
      this.engine.endFilterPreview(false, def.kind, vals[0], vals[1], vals[2], vals[3], vals[4],
        vals[5], this.filterSeed);
    }
    this.smartEditIndex = -1;
    this.filterDef = null;
    this.log('滤镜已取消');
  }

  // ---------- M21 智能滤镜管理对话框 ----------

  private openSmartFiltersDialog(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const info = this.engine.getSmartInfo(a.id);
    if (!info.isSmart) {
      this.toast('活动图层不是智能对象');
      return;
    }
    this.smartInfo = info;
    this.showSmartFilters = true;
    this.log(`智能滤镜对话框: ${info.filters.length} 个滤镜`);
  }

  private refreshSmartFiltersDialog(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    this.smartInfo = this.engine.getSmartInfo(a.id);
  }

  private onSmartFilterVisible(idx: number, vis: boolean): void {
    const a = this.activeLayer();
    const info = this.smartInfo;
    if (a === undefined || info === null || idx >= info.filters.length) {
      return;
    }
    const f: SmartFilterDTO = info.filters[idx];
    this.engine.setSmartFilter(a.id, idx, f.p, f.seed, f.blend, f.opacity, vis, f.name)
      .then((ok: boolean): void => {
        this.log(`智能滤镜显隐 #${idx}: ${ok}`);
        this.refreshSmartFiltersDialog();
      });
  }

  private onSmartFilterEdit(idx: number): void {
    const info = this.smartInfo;
    if (info === null || idx >= info.filters.length) {
      return;
    }
    const f: SmartFilterDTO = info.filters[idx];
    if (f.unsupported || f.kind < 0) {
      this.toast('PSD 未实现滤镜不支持参数编辑（可显隐/删除）');
      return;
    }
    const def = FILTER_CATALOG.find((d: FilterDef): boolean => d.kind === f.kind);
    if (def === undefined) {
      this.toast('滤镜目录未收录该核');
      return;
    }
    this.smartEditIndex = idx;
    this.showSmartFilters = false;
    this.openFilterDialog(def);
  }

  private onSmartFilterRemove(idx: number): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    this.engine.removeSmartFilter(a.id, idx).then((ok: boolean): void => {
      this.log(`智能滤镜已删除 #${idx}: ${ok}`);
      this.refreshSmartFiltersDialog();
    });
  }

  private onSmartFilterMove(idx: number, up: boolean): void {
    const a = this.activeLayer();
    const info = this.smartInfo;
    if (a === undefined || info === null) {
      return;
    }
    const to = up ? idx - 1 : idx + 1;
    if (to < 0 || to >= info.filters.length) {
      return;
    }
    this.engine.moveSmartFilter(a.id, idx, to).then((ok: boolean): void => {
      this.log(`智能滤镜重排 #${idx}→${to}: ${ok}`);
      this.refreshSmartFiltersDialog();
    });
  }

  private onSmartFiltersEnabled(v: boolean): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    this.engine.setSmartFiltersEnabled(a.id, v).then((ok: boolean): void => {
      this.log(`智能滤镜整栈开关: ${v} (${ok})`);
      this.refreshSmartFiltersDialog();
    });
  }

  private onSmartFilterMask(op: number, v: boolean): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    this.engine.setSmartFilterMask(a.id, op, v).then((ok: boolean): void => {
      this.log(`滤镜蒙版操作 ${op}: ${ok}`);
      this.refreshSmartFiltersDialog();
    });
  }

  // ---------- M15e RAW 显影参数面板 ----------
  // v1 模态对话框 + 确定后异步全分辨率重显影（恒从会话 RAW 缓存出发，一步 history 事务）；
  // 实时预览登记后续（需异步显影管线）。

  private resetDevEditors(): void {
    this.devCrvHl = 0;
    this.devCrvLg = 0;
    this.devCrvDk = 0;
    this.devCrvSh = 0;
    this.devCurvePts = [];
    this.devHslBand = 0;
    this.devHslH = [0, 0, 0, 0, 0, 0, 0, 0];
    this.devHslS = [0, 0, 0, 0, 0, 0, 0, 0];
    this.devHslL = [0, 0, 0, 0, 0, 0, 0, 0];
    this.devGradeWheel = 0;
    this.devGradeH = [0, 0, 0, 0];
    this.devGradeS = [0, 0, 0, 0];
    this.devGradeL = [0, 0, 0, 0];
    this.devGradeBlend = 50;
    this.devGradeBal = 0;
    this.devPreviewLive = false;
  }

  private resetDevSliders(): void {
    this.devExposure = 0;
    this.devContrast = 0;
    this.devHighlights = 0;
    this.devShadows = 0;
    this.devWhites = 0;
    this.devBlacks = 0;
    this.devTexture = 0;
    this.devClarity = 0;
    this.devDehaze = 0;
    this.devVibrance = 0;
    this.devSaturation = 0;
    this.devSharpen = 0;
    this.devNoiseLum = 0;
    this.devNoiseColor = 0;
    this.devVignette = 0;
    this.devGrain = 0;
    this.devTemp = 0;
    this.devTint = 0;
    this.devEv = 0;
  }

  private openDevelopPanel(): void {
    if (!this.hasRawDevelop) {
      this.toast('会话内没有 RAW（先打开 DNG）');
      return;
    }
    const st = this.engine.getStats();
    this.devTemp = st.devTemp;
    this.devTint = st.devTint;
    this.devEv = st.devEv;
    this.devGamma = st.devGamma === 0 ? 1 : st.devGamma;
    this.devLayerMode = false;
    this.resetDevEditors();
    this.showDevelop = true;
    this.log('显影参数面板: 打开');
  }

  // M23c：滤镜菜单「Camera Raw…」入口——同一对话框作用于活动像素层（非 RAW 会话）。
  private openCameraRawFilterDialog(): void {
    const a = this.activeLayer();
    if (!this.docOpen || a === undefined) {
      this.toast('无文档或无活动图层');
      return;
    }
    if (a.isText || a.isShape || a.isSmart || a.isGroup) {
      this.toast('Camera Raw 滤镜需要像素图层（文字/形状/智能对象/组请先栅格化）');
      return;
    }
    this.devLayerMode = true;
    this.devLayerTarget = a.id;
    this.devCameraOn = true;
    this.resetDevSliders();
    this.resetDevEditors();
    this.devGamma = 1;
    this.showDevelop = true;
    this.log(`Camera Raw 滤镜面板: 层${a.id}`);
  }

  // M23c：当前 UI 状态 → CameraRaw JSON（camelCase；全字段）。
  private buildCameraJson(): string {
    const cam: Record<string, number | number[]> = {
      'temperature': this.devTemp,
      'tint': this.devTint,
      'exposure': this.devExposure / 100.0 * 2.0,
      'contrast': this.devContrast,
      'highlights': this.devHighlights,
      'shadows': this.devShadows,
      'whites': this.devWhites,
      'blacks': this.devBlacks,
      'texture': this.devTexture,
      'clarity': this.devClarity,
      'dehaze': this.devDehaze,
      'vibrance': this.devVibrance,
      'saturation': this.devSaturation,
      'curveHighlights': this.devCrvHl,
      'curveLights': this.devCrvLg,
      'curveDarks': this.devCrvDk,
      'curveShadows': this.devCrvSh,
      'hslHue': this.devHslH,
      'hslSat': this.devHslS,
      'hslLum': this.devHslL,
      'gradeShadowsHue': this.devGradeH[0],
      'gradeShadowsSat': this.devGradeS[0],
      'gradeShadowsLum': this.devGradeL[0],
      'gradeMidtonesHue': this.devGradeH[1],
      'gradeMidtonesSat': this.devGradeS[1],
      'gradeMidtonesLum': this.devGradeL[1],
      'gradeHighlightsHue': this.devGradeH[2],
      'gradeHighlightsSat': this.devGradeS[2],
      'gradeHighlightsLum': this.devGradeL[2],
      'gradeGlobalHue': this.devGradeH[3],
      'gradeGlobalSat': this.devGradeS[3],
      'gradeGlobalLum': this.devGradeL[3],
      'gradeBlending': this.devGradeBlend,
      'gradeBalance': this.devGradeBal,
      'sharpenAmount': this.devSharpen,
      'noiseLuminance': this.devNoiseLum,
      'noiseColor': this.devNoiseColor,
      'vignetteAmount': this.devVignette,
      'grainAmount': this.devGrain,
    };
    if (this.devCurvePts.length >= 4) {
      cam['pointCurve'] = this.devCurvePts;
    }
    return JSON.stringify(cam);
  }

  // M23c：mode 0=立即提交 / 1=实时预览 / 2=终提交 / 3=取消还原。
  private runDevDevelop(mode: number): void {
    if (this.devLayerMode) {
      this.engine.cameraRawFilter(this.devLayerTarget, this.buildCameraJson(), mode)
        .then((d: RawDevelopData): void => {
          if (mode === 1) {
            this.devPreviewLive = true;
          } else if (mode === 2) {
            this.devPreviewLive = false;
            this.log(`Camera Raw 滤镜: ${d.width}x${d.height} 层${d.layerId}`);
            this.toast('Camera Raw 滤镜已应用');
          }
        })
        .catch((e: BusinessError): void => {
          if (mode !== 3) {
            this.log(`Camera Raw 失败: ${e.message}`);
            this.toast(`Camera Raw 失败: ${e.message}`);
          }
        });
      return;
    }
    if (this.devCameraOn) {
      this.engine.developRaw(0, 0, 0, this.devGamma, this.buildCameraJson(), mode)
        .then((d: RawDevelopData): void => {
          if (mode === 1) {
            this.devPreviewLive = true;
          } else if (mode === 2) {
            this.devPreviewLive = false;
            this.log(`RAW 重显影(Camera Raw): ${d.width}x${d.height} 层${d.layerId}`);
            this.toast('Camera Raw 显影已应用');
          }
        })
        .catch((e: BusinessError): void => {
          if (mode !== 3) {
            this.log(`RAW 重显影失败: ${e.message}`);
            this.toast(`显影失败: ${e.message}`);
          }
        });
      return;
    }
    this.engine.developRaw(this.devTemp, this.devTint, this.devEv, this.devGamma, '', mode)
      .then((d: RawDevelopData): void => {
        if (mode === 1) {
          this.devPreviewLive = true;
        } else if (mode === 2) {
          this.devPreviewLive = false;
          this.log(`RAW 重显影: ${d.width}x${d.height} 层${d.layerId}`);
          this.toast('显影已应用');
        }
      })
      .catch((e: BusinessError): void => {
        if (mode !== 3) {
          this.log(`RAW 重显影失败: ${e.message}`);
          this.toast(`显影失败: ${e.message}`);
        }
      });
  }

  // 滑条/曲线提交 → debounce 450ms → 异步预览（代次防旧覆盖）。
  private scheduleDevPreview(): void {
    if (!this.devLayerMode && !this.hasRawDevelop) {
      return;
    }
    if (this.devPreviewTimer >= 0) {
      clearTimeout(this.devPreviewTimer);
    }
    this.devPreviewTimer = setTimeout((): void => {
      this.devPreviewTimer = -1;
      this.runDevDevelop(1);
    }, 450);
  }

  private onDevSlider(mode: SliderChangeMode): void {
    if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
      this.scheduleDevPreview();
    }
  }

  private commitDevelop(): void {
    if (!this.showDevelop) {
      return;
    }
    this.showDevelop = false;
    if (this.devPreviewTimer >= 0) {
      clearTimeout(this.devPreviewTimer);
      this.devPreviewTimer = -1;
    }
    this.runDevDevelop(2);
  }

  private cancelDevelop(): void {
    if (!this.showDevelop) {
      return;
    }
    this.showDevelop = false;
    if (this.devPreviewTimer >= 0) {
      clearTimeout(this.devPreviewTimer);
      this.devPreviewTimer = -1;
    }
    this.runDevDevelop(3);  // 无会话时报错被吞（幂等取消）
    this.devPreviewLive = false;
    this.log('显影已取消');
  }

  // ---- M23c 点曲线编辑器（Canvas；域 0..255，与 CameraRaw.pointCurve 同约定） ----

  private devCurveToCanvas(x: number, y: number): number[] {
    return [24 + x / 255 * 152, 24 + (1 - y / 255) * 152];
  }

  private devCurveFromCanvas(cx: number, cy: number): number[] {
    const x = Math.min(255, Math.max(0, (cx - 24) / 152 * 255));
    const y = Math.min(255, Math.max(0, (1 - (cy - 24) / 152) * 255));
    return [x, y];
  }

  private devDrawCurve(): void {
    const c = this.devCurveCtx;
    c.clearRect(0, 0, 200, 200);
    c.strokeStyle = '#333333';
    c.lineWidth = 1;
    for (let i = 1; i < 4; ++i) {
      const p = 24 + 152 * i / 4;
      c.beginPath();
      c.moveTo(p, 24);
      c.lineTo(p, 24 + 152);
      c.stroke();
      c.beginPath();
      c.moveTo(24, p);
      c.lineTo(24 + 152, p);
      c.stroke();
    }
    c.strokeStyle = '#555555';
    c.beginPath();
    c.moveTo(24, 24 + 152);
    c.lineTo(24 + 152, 24);
    c.stroke();
    c.strokeStyle = '#00E5FF';
    c.lineWidth = 2;
    c.beginPath();
    const pts = this.devCurvePts;
    if (pts.length >= 4) {
      const p0 = this.devCurveToCanvas(pts[0], pts[1]);
      c.moveTo(p0[0], p0[1]);
      for (let k = 2; k + 1 < pts.length; k += 2) {
        const pk = this.devCurveToCanvas(pts[k], pts[k + 1]);
        c.lineTo(pk[0], pk[1]);
      }
    } else {
      c.moveTo(24, 24 + 152);
      c.lineTo(24 + 152, 24);
    }
    c.stroke();
    c.fillStyle = '#FFFFFF';
    for (let k = 0; k + 1 < pts.length; k += 2) {
      const pk = this.devCurveToCanvas(pts[k], pts[k + 1]);
      c.beginPath();
      c.arc(pk[0], pk[1], 5, 0, 6.283);
      c.fill();
    }
  }

  private devCurveTouch(e: TouchEvent): void {
    if (e.touches.length < 1 && e.changedTouches.length < 1) {
      return;
    }
    const t: TouchObject = e.changedTouches.length > 0 ? e.changedTouches[0] : e.touches[0];
    if (e.type === TouchType.Down) {
      const pts = this.devCurvePts;
      let hit = -1;
      let best = 14;
      for (let k = 0; k + 1 < pts.length; k += 2) {
        const pk = this.devCurveToCanvas(pts[k], pts[k + 1]);
        const dist = Math.hypot(pk[0] - t.x, pk[1] - t.y);
        if (dist < best) {
          best = dist;
          hit = k / 2;
        }
      }
      if (hit >= 0) {
        this.devCurveDrag = hit;
        return;
      }
      if (pts.length >= 32) {
        return;  // 引擎上限 16 点
      }
      const pv = this.devCurveFromCanvas(t.x, t.y);
      const np: number[] = pts.slice();
      np.push(pv[0], pv[1]);
      // x 单调排序
      const order: number[] = [];
      for (let k = 0; k < np.length / 2; ++k) {
        order.push(k);
      }
      order.sort((a: number, b: number): number => np[a * 2] - np[b * 2]);
      const sorted: number[] = [];
      for (const k of order) {
        sorted.push(np[k * 2], np[k * 2 + 1]);
      }
      this.devCurvePts = sorted;
      this.devCurveDrag = -1;
      this.devDrawCurve();
      this.scheduleDevPreview();
    } else if (e.type === TouchType.Move && this.devCurveDrag >= 0) {
      const pv = this.devCurveFromCanvas(t.x, t.y);
      const pts = this.devCurvePts.slice();
      const idx = this.devCurveDrag * 2;
      const lo = this.devCurveDrag > 0 ? pts[idx - 2] + 4 : 0;
      const hi = this.devCurveDrag < pts.length / 2 - 1 ? pts[idx + 2] - 4 : 255;
      pts[idx] = Math.min(hi, Math.max(lo, pv[0]));
      pts[idx + 1] = pv[1];
      this.devCurvePts = pts;
      this.devDrawCurve();
    } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) &&
               this.devCurveDrag >= 0) {
      this.devCurveDrag = -1;
      this.devDrawCurve();
      this.scheduleDevPreview();
    }
  }

  // 编辑 overlay 视口框 [x,y,w,h]（vp）；非编辑态返回空数组
  private editingBoxVp(): number[] {
    if (this.editingTextId === 0) {
      return [];
    }
    const a = this.layers.find((l: LayerDTO): boolean => l.id === this.editingTextId);
    if (a === undefined || !a.isText) {
      return [];
    }
    const k = this.pxPerVp();
    const x = (a.textX - this.panX) * this.zoom / k;
    const y = (a.textY - this.panY) * this.zoom / k;
    const wDoc = a.textWrapWidth > 0 ? a.textWrapWidth : Math.max(a.textW, 320);
    return [x, y, Math.max(140, wDoc * this.zoom / k), Math.max(48, (a.textH + 24) * this.zoom / k)];
  }

  // overlay 输入 → debounce 250ms → 引擎（会话内无事务，end 统一一步撤销）
  private onOverlayChanged(): void {
    if (this.editingTextId === 0) {
      return;
    }
    if (this.textEditTimer >= 0) {
      clearTimeout(this.textEditTimer);
    }
    const id = this.editingTextId;
    this.textEditTimer = setTimeout((): void => {
      this.textEditTimer = -1;
      if (this.editingTextId !== id) {
        return;
      }
      const text = this.collectOverlayText();
      if (this.engine.setTextLayerContent(id, text, false)) {
        this.textPanelContent = text;
      }
    }, 250);
  }

  private shapeKindLabel(): string {
    const labels: string[] = ['矩形', '圆角矩形', '椭圆', '多边形', '星形', '直线'];
    return labels[this.shapeKind] ?? '形状';
  }

  // M19a 修补工具：选区内按下拖拽，释放时以拖拽向量为源位移提交修补
  //（PS 语义：拖向的区域即纹理源，选区原位接受泊松融合）
  private patchTouch(e: TouchEvent, docX: number, docY: number): void {
    if (e.type === TouchType.Down) {
      if (!this.hasSelectionFlag) {
        this.log('修补：需先用选区工具建立选区');
        return;
      }
      this.patchDragging = true;
      this.patchStartX = docX;
      this.patchStartY = docY;
    } else if (e.type === TouchType.Up || e.type === TouchType.Cancel) {
      if (!this.patchDragging) {
        return;
      }
      this.patchDragging = false;
      const dx = Math.round(docX - this.patchStartX);
      const dy = Math.round(docY - this.patchStartY);
      if (dx === 0 && dy === 0) {
        this.log('修补：位移为 0，已取消');
        return;
      }
      const ok = this.engine.patchSelection(dx, dy);
      this.log(`修补: 源位移(${dx},${dy}) → ${ok ? '已应用' : '失败（见日志）'}`);
    }
  }

  // M11a 渐变拖线：Down 起线 / Move 更新终点（overlay 预览）/ Up 提交单事务
  private gradientTouch(e: TouchEvent, docX: number, docY: number): void {
    if (e.type === TouchType.Down) {
      if (this.gradientTarget === 1 && !this.activeLayerHasMask()) {
        this.toast('当前图层无蒙版');
        return;
      }
      this.gradDragging = true;
      this.gradientLine = [docX, docY, docX, docY];
    } else if (e.type === TouchType.Move && this.gradDragging) {
      const line = this.gradientLine;
      if (line !== null) {
        this.gradientLine = [line[0], line[1], docX, docY];
      }
    } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.gradDragging) {
      this.gradDragging = false;
      const line = this.gradientLine;
      this.gradientLine = null;
      const a = this.activeLayer();
      if (line === null || a === undefined) {
        return;
      }
      const dx = line[2] - line[0];
      const dy = line[3] - line[1];
      if (dx * dx + dy * dy < 1) {
        this.toast('拖线过短');
        return;
      }
      const ok = this.engine.applyGradient(a.id, this.gradientType, this.gradientStyle,
        line[0], line[1], line[2], line[3], this.gradientTarget);
      this.log(`渐变(${this.gradientType === 0 ? '线性' : '径向'}/` +
        `${this.gradientTarget === 0 ? '像素' : '蒙版'}): ${ok ? '已应用' : '失败'}`);
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
        this.log('选框：拖距过小，已取消选区');
        return;
      }
      const op = this.effectiveSelOp();
      const ok = this.selShape === 'ellipse'
        ? this.engine.setEllipseSelection(x, y, w, h, op)
        : this.engine.setRectSelection(x, y, w, h, op);
      this.updateSelFlag(op, ok);
      this.log(`${this.selShape === 'ellipse' ? '椭圆' : '矩形'}选区: (${x.toFixed(0)},${y.toFixed(0)}) ` +
        `${w.toFixed(0)}×${h.toFixed(0)} op=${op} → ${ok}`);
    }
  }

  private pickColor(docX: number, docY: number): void {
    this.engine.samplePixel(docX, docY).then((d): void => {
      if (!d.ok) {
        this.toast('该点无像素（透明）');
        return;
      }
      const hex = this.rgbToHex(d.red, d.green, d.blue);
      this.applyColors(hex, this.bgColor);
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
      this.xcController.onChanged = (_rect: SurfaceRect): void => {
        // M16a：空闲停帧后窗口 resize 的唯一消费入口
        this.engine.notifySurfaceChanged();
      };
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
    const hc = this.hostCtx();
    if (hc === undefined) {
      return;
    }
    window.getLastWindow(hc).then((w: window.Window): void => {
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
    this.quickMaskActive = false;
    this.strokeDrawing = false;  // 换文档作废在途笔画状态（引擎侧 resetDocument 同步清队列/draft）
    // M16a：换文档（新建/打开工程/PSD/图片导入共用此口）关滤镜对话框并作废在途防抖——
    // 引擎 filterPreview 已由 resetDocument 清（审查修复批一），UI 侧对话框/定时器在此收口，
    // 否则旧参数预览会打到新文档活动层
    if (this.showFilter) {
      this.showFilter = false;
      this.filterDef = null;
    }
    if (this.filterDebounce >= 0) {
      clearTimeout(this.filterDebounce);
      this.filterDebounce = -1;
    }
    this.engine.setBrushTarget(false);
    this.refreshChannelCount();
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
    if (this.editingTextId !== 0) {
      this.editingTextId = 0;  // 引擎 resetDocument 已清会话，UI 镜像同步
    }
    if (this.showFilter) {
      this.showFilter = false;  // M13：预览会话随文档清空（引擎 resetDocument 已清）
      this.filterDef = null;
    }
    const ok = this.engine.closeDocument();
    this.docOpen = false;
    this.saveUri = '';
    this.layers = [];
    this.thumbCache.clear();
    this.log(`关闭文档: ${ok ? '成功' : '失败'}`);
    this.updateTitle();
  }

  // ---------- M16a：JPEG 分带/swapRB 校准通道 ----------
  // 程序生成已知图案 JPEG（1024×4608 = 引擎分带 2048/2048/512；上 1/3 红、中 1/3 绿、下 1/3 蓝），
  // 走 openFileFromFd 系统分带解码路径，采样 7 点：三段段色（swapRB 符号：R/B 互换会红蓝对调）
  // + 两处跨带缝合行（2047/2048、4095/4096 同色跨带，断带必露邻段色）。分带是否真实生效
  // 看 hilog [Montage.Import] band 行（req vs actual）。模拟器自证 + 真机复跑同通道。
  private async debugJpegCalibration(): Promise<void> {
    const W: number = 1024;
    const H: number = 4608;
    try {
      const buf: ArrayBuffer = new ArrayBuffer(W * H * 4);
      const px: Uint8Array = new Uint8Array(buf);
      for (let y = 0; y < H; y++) {
        let r = 255;
        let g = 0;
        let b = 0;
        if (y >= 3072) {
          r = 0;
          g = 0;
          b = 255;
        } else if (y >= 1536) {
          r = 0;
          g = 255;
          b = 0;
        }
        const rowStart = y * W * 4;
        for (let x = 0; x < W; x++) {
          const o = rowStart + x * 4;
          px[o] = r;
          px[o + 1] = g;
          px[o + 2] = b;
          px[o + 3] = 255;
        }
      }
      const pm = image.createPixelMapSync(buf, {
        size: { width: W, height: H },
        srcPixelFormat: image.PixelMapFormat.RGBA_8888,
      });
      const path = this.cachePath('band_test.jpg');
      const fw = fs.openSync(path, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      const packer = image.createImagePacker();
      await packer.packToFile(pm, fw.fd, { format: 'image/jpeg', quality: 100 });
      fs.closeSync(fw);
      pm.release();
      packer.release();
      this.log(`校准 JPEG 已生成: ${path}`);
      this.doCloseDocument();  // 清场：无文档 → 导入建档（层落 0,0，文档坐标=图像坐标）
      const fd = fs.openSync(path, fs.OpenMode.READ_ONLY);
      const dd = await this.engine.openFileFromFd(fd.fd);
      this.docW = dd.width;
      this.docH = dd.height;
      this.docOpen = true;
      this.afterDocChanged();
      this.log(`JPEG 导入: ${dd.width}x${dd.height}（预期 ${W}x${H}）`);
      const pts: JpegCalibCheck[] = [
        { x: 512, y: 800, label: '上段红', r: 255, g: 0, b: 0 },
        { x: 512, y: 2047, label: '带0缝合行', r: 0, g: 255, b: 0 },
        { x: 512, y: 2048, label: '带1首行', r: 0, g: 255, b: 0 },
        { x: 512, y: 2300, label: '中段绿', r: 0, g: 255, b: 0 },
        { x: 512, y: 4095, label: '带1缝合行', r: 0, g: 0, b: 255 },
        { x: 512, y: 4096, label: '带2首行', r: 0, g: 0, b: 255 },
        { x: 512, y: 4300, label: '下段蓝', r: 0, g: 0, b: 255 },
      ];
      let pass = 0;
      for (const c of pts) {
        const s = await this.engine.samplePixel(c.x, c.y);
        const ok = s.ok && Math.abs(s.red - c.r) <= 16 && Math.abs(s.green - c.g) <= 16 &&
          Math.abs(s.blue - c.b) <= 16 && s.alpha === 255;
        if (ok) {
          pass++;
        }
        const msg = `校准点 ${c.label}(${c.x},${c.y}): rgb(${s.red},${s.green},${s.blue}) a${s.alpha} ` +
          `${ok ? '✓' : `期望(${c.r},${c.g},${c.b})`}`;
        this.log(msg);
        hilog.info(DOMAIN, TAG, 'jpeg calib: %{public}s', msg);
      }
      const verdict = pass === pts.length ? 'PASS' : 'FAIL';
      this.log(`JPEG 分带校准 ${verdict}（${pass}/${pts.length}）——分带生效性看 [Montage.Import] band 行`);
      this.toast(`JPEG 校准 ${verdict} ${pass}/${pts.length}`);
    } catch (e) {
      const be = e as BusinessError;
      const msg = `${be.code} ${be.message}`;
      this.log(`JPEG 校准失败: ${msg}`);
      hilog.error(DOMAIN, TAG, 'jpeg calib failed: %{public}s', msg);
    }
  }

  // ---------- M21 置入/替换/导出智能对象内容 ----------

  private pickSmartSource(replace: boolean): void {
    const a = this.activeLayer();
    if (replace && (a === undefined || !a.isSmart)) {
      this.toast(replace ? '活动图层不是智能对象' : '无活动图层');
      return;
    }
    if (!replace && !this.docOpen) {
      this.toast('无文档（先新建或打开）');
      return;
    }
    const opt = new picker.DocumentSelectOptions();
    opt.maxSelectNumber = 1;
    // M2.5 实证：后缀过滤非图片类为空 → 不过滤 + 应用侧分发
    opt.defaultFilePathUri = 'file://docs/storage/Users/currentUser/Download';
    const dp = new picker.DocumentViewPicker();
    dp.select(opt).then((uris: string[]): void => {
      if (uris.length < 1) {
        return;
      }
      const uri = uris[0];
      const seg = decodeURIComponent(uri.split('/').pop() ?? 'source.bin');
      let f: fs.File;
      try {
        f = fs.openSync(uri, fs.OpenMode.READ_ONLY);
      } catch (e) {
        const be = e as BusinessError;
        this.log(`置入打开失败: ${be.code} ${be.message}`);
        this.toast('文件打开失败');
        return;
      }
      try {
        const st = fs.statSync(f.fd);
        const buf = new ArrayBuffer(st.size);
        fs.readSync(f.fd, buf);
        fs.closeSync(f);
        if (replace && a !== undefined) {
          this.busyText = '替换内容中…';
          this.engine.replaceSmartContents(a.id, seg, buf).then((ok: boolean): void => {
            this.busyText = '';
            this.toast(ok ? '内容已替换（共享实例同步重渲）' : '替换失败');
            this.log(`替换内容 #${a.id} <- ${seg}: ${ok}`);
          }).catch((e2: BusinessError): void => {
            this.busyText = '';
            this.log(`替换失败: ${e2.message}`);
          });
        } else {
          this.busyText = '置入中…';
          this.engine.placeEmbedded(seg, buf).then((newId: number): void => {
            this.busyText = '';
            if (newId !== 0) {
              this.toast(`已置入 ${seg}`);
              this.log(`置入智能对象 #${newId} <- ${seg}`);
            } else {
              this.toast('置入失败（源不可解码？）');
            }
          }).catch((e2: BusinessError): void => {
            this.busyText = '';
            this.log(`置入失败: ${e2.message}`);
          });
        }
      } catch (e) {
        fs.closeSync(f);
        const be = e as BusinessError;
        this.log(`置入读取失败: ${be.code} ${be.message}`);
        this.toast('文件读取失败');
      }
    }).catch((e: BusinessError): void => {
      this.log(`置入 picker 失败: ${e.message}`);
    });
  }

  private exportSmartContents(): void {
    const a = this.activeLayer();
    if (a === undefined || !a.isSmart) {
      this.toast('活动图层不是智能对象');
      return;
    }
    const info = this.engine.getSmartInfo(a.id);
    if (!info.embedded) {
      this.toast('链接智能对象请直接使用源文件');
      return;
    }
    const opt = new picker.DocumentSaveOptions();
    opt.newFileNames = [info.fileName];
    const dp = new picker.DocumentViewPicker();
    dp.save(opt).then((uris: string[]): void => {
      if (uris.length < 1) {
        return;
      }
      const f = fs.openSync(uris[0], fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
      const n = this.engine.exportSmartContents(a.id, f.fd);
      fs.closeSync(f);
      if (n >= 0) {
        this.toast(`内容已导出（${n} 字节）`);
        this.log(`导出内容 #${a.id} -> ${uris[0]}: ${n}B`);
      } else {
        this.toast('导出失败');
      }
    }).catch((e: BusinessError): void => {
      this.log(`导出内容 picker 失败: ${e.message}`);
    });
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
      } else if (uri.endsWith('.tif') || uri.endsWith('.tiff')) {
        this.openTiffByUri(uris[0]);
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

  private openTiffByUri(uri: string): void {
    const f = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    this.importing = true;
    this.busyText = 'TIFF 解析中…';
    this.engine.openTiffFile(f.fd).then((d: PsdOpenData): void => {
      this.importing = false;
      this.busyText = '';
      this.docW = d.width;
      this.docH = d.height;
      this.afterDocChanged();
      this.log(`TIFF 打开完成 ${d.width}x${d.height}，${d.notes.length} 条提示`);
      for (let i = 0; i < d.notes.length && i < 8; i++) {
        this.log(`提示: ${d.notes[i]}`);
      }
    }).catch((e: BusinessError): void => {
      this.importing = false;
      this.busyText = '';
      this.log(`TIFF 打开失败: ${e.message}`);
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
      // M12 C-3：文本层缺字体降级提示（引擎已回退默认字体显示旧栅格）
      if (d.notes.length > 0) {
        for (const n of d.notes) {
          this.log(`降级提示: ${n}`);
        }
        this.toast(d.notes[0] + (d.notes.length > 1 ? `（等 ${d.notes.length} 条）` : ''));
      }
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
    // M25：文件名透传（TGA 等无魔数格式的嗅探兜底；native 侧魔数优先）
    const dot = uri.lastIndexOf('.');
    const baseName = dot >= 0 ? uri.slice(dot) : '';
    this.engine.openFileFromFd(f.fd, baseName).then((d: OpenFileData): void => {
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
    // TRUNC 清旧包再整包重写（fd 移交 native）；openSync 同步抛 BusinessError，
    // 不捕获会卡死忙态（审查修复）
    let f: fs.File | null = null;
    try {
      f = fs.openSync(uri, fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
      this.log(`保存工程 fd=${f.fd}`);
    } catch (e) {
      this.busyText = '';
      const be = e as BusinessError;
      this.log(`保存打开失败: ${be.code} ${be.message}`);
      this.toast('保存失败（无法写入目标文件）');
      return;
    }
    const file: fs.File = f;
    this.engine.saveProject(file.fd).then((d: SaveProjectData): void => {
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

  // ---------- M24b 批处理 / 图像处理器 ----------

  private refreshActionNames(): void {
    const r = automation.listActions();
    this.actionNamesMirror = [];
    if (r.ok && r.value !== undefined) {
      const v = r.value as Record<string, Object>;
      const arr = v['actions'] as Object as Record<string, Object>[];
      if (arr !== undefined && arr !== null) {
        for (const row of arr) {
          const nm = row['name'];
          if (typeof nm === 'string') {
            this.actionNamesMirror.push(nm);
          }
        }
      }
    }
  }

  private makeAutomate(): Automate {
    return new Automate(this.engine, (): boolean => this.automateCancel,
      this.cachePath('automate_restore.montage'));
  }

  private runAutomate(params: Record<string, Object>): void {
    const inputDir = params['inputDir'] as string;
    const output = params['output'] as string;
    if (inputDir.length === 0 || output.length === 0) {
      this.toast('输入/输出目录不能为空');
      return;
    }
    this.automateBusy = true;
    this.automateCancel = false;
    this.automateProgress = '准备中…';
    this.automateResult = '';
    const fmt = params['format'] as string;
    if (this.automateMode === 0) {
      const opts: BatchOptions = {
        inputDir: inputDir, files: [], output: output,
        actionName: params['actionName'] as string, steps: [], format: fmt, quality: 0,
      };
      const preErr = Automate.checkSteps(opts.actionName, opts.steps);
      if (preErr.length > 0) {
        this.automateBusy = false;
        this.automateResult = preErr;
        return;
      }
      this.makeAutomate().runBatch(opts, (done: number, total: number, name: string): void => {
        this.automateProgress = `${done}/${total} ${name}`;
      }).then((r: BatchResult): void => {
        this.automateBusy = false;
        this.automateProgress = '';
        this.automateResult = `完成 ${r.files.length} 个` +
          (r.errors.length > 0 ? `，失败 ${r.errors.length} 个：` +
            r.errors.slice(0, 5).map((e: BatchFileError): string => `${e.file} ${e.error}`)
              .join('；') : '');
        this.log(`批处理: ${this.automateResult}`);
      }).catch((e: BusinessError): void => {
        this.automateBusy = false;
        this.automateResult = `批处理异常: ${e.message}`;
      });
    } else {
      const opts: ProcessorOptions = {
        inputDir: inputDir, files: [], output: output, format: fmt,
        width: params['width'] as number, height: params['height'] as number,
        dontEnlarge: true, quality: params['quality'] as number, convertToSrgb: true,
      };
      this.makeAutomate().runProcessor(opts, (done: number, total: number, name: string): void => {
        this.automateProgress = `${done}/${total} ${name}`;
      }).then((r: BatchResult): void => {
        this.automateBusy = false;
        this.automateProgress = '';
        this.automateResult = `完成 ${r.files.length} 个` +
          (r.errors.length > 0 ? `，失败 ${r.errors.length} 个：` +
            r.errors.slice(0, 5).map((e: BatchFileError): string => `${e.file} ${e.error}`)
              .join('；') : '');
        this.log(`图像处理器: ${this.automateResult}`);
      }).catch((e: BusinessError): void => {
        this.automateBusy = false;
        this.automateResult = `图像处理器异常: ${e.message}`;
      });
    }
  }

  private doExport(format: string, quality: number): void {
    if (!this.docOpen) {
      this.toast('无文档可导出');
      return;
    }
    const extMap = new Map<string, string>();
    extMap.set('png', 'png');
    extMap.set('jpeg', 'jpg');
    extMap.set('psd', 'psd');
    extMap.set('tiff', 'tif');
    extMap.set('gif', 'gif');
    extMap.set('bmp', 'bmp');
    extMap.set('tga', 'tga');
    extMap.set('qoi', 'qoi');
    extMap.set('webp', 'webp');
    const ext = extMap.get(format) ?? 'png';
    const opt = new picker.DocumentSaveOptions();
    opt.newFileNames = [`${this.docName().replace(/\.(montage|png|jpg|jpeg|tif|tiff|gif|bmp|tga|qoi|webp)$/i, '') || '未命名'}.${ext}`];
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
    // openSync 同步抛 BusinessError，不捕获会卡死忙态（审查修复）
    let f: fs.File;
    try {
      f = fs.openSync(uri, fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
    } catch (e) {
      this.busyText = '';
      const be = e as BusinessError;
      this.log(`导出打开失败: ${be.code} ${be.message}`);
      this.toast('导出失败（无法写入目标文件）');
      return;
    }
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
    } else if (format === 'jpeg' || format === 'webp') {
      // JPEG/WebP：引擎扁平化 → PixelMap → ImagePacker（V-01；webp M25）
      const mime = format === 'webp' ? 'image/webp' : 'image/jpeg';
      const label = format === 'webp' ? 'WebP' : 'JPEG';
      this.engine.flattenToPixelMap().then((d): void => {
        if (d.pixelMap === null) {
          this.busyText = '';
          this.toast('导出失败：空文档');
          fs.closeSync(f.fd);
          return;
        }
        const pm = d.pixelMap as image.PixelMap;
        const packer = image.createImagePacker();
        packer.packToFile(pm, f.fd, { format: mime, quality: quality }).then((): void => {
          this.busyText = '';
          pm.release();
          this.log(`${label} 导出 q=${quality}（${Date.now() - t0}ms）→ ${uri}`);
          this.toast(`${label} 已导出`);
        }).catch((e: BusinessError): void => {
          this.busyText = '';
          pm.release();
          this.log(`${label} 编码失败: ${e.message}`);
          this.toast(`${label} 导出失败`);
        });
      }).catch((e: BusinessError): void => {
        this.busyText = '';
        this.log(`扁平化失败: ${e.message}`);
        this.toast('导出失败');
      });
    } else {
      // M25 平面格式（TIFF/GIF/BMP/TGA/QOI）：引擎扁平化 → 编码器直写 fd
      const label = format.toUpperCase();
      this.engine.exportImage(f.fd, format, quality).then((d): void => {
        this.busyText = '';
        this.log(`${label} 导出 ${d.width}x${d.height} ${d.bytes} 字节（${Date.now() - t0}ms）→ ${uri}`);
        this.toast(`${label} 已导出（${(d.bytes / 1048576).toFixed(1)}MB）`);
      }).catch((e: BusinessError): void => {
        this.busyText = '';
        this.log(`${label} 导出失败: ${e.message}`);
        this.toast(`${label} 导出失败`);
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
    if (id !== 'stamp' && this.tool === 'stamp') {
      this.engine.setBrushClone(false);  // 离开图章工具即清除克隆标志
    }
    if (id === 'historyBrush') {
      // M11f 历史画笔：互斥克隆/效果/蒙版目标
      this.maskTarget = false;
      this.engine.setBrushTarget(false);
      this.engine.setBrushClone(false);
      this.engine.setBrushEffect(0);
      this.engine.setHistoryBrush(this.historyBrushSteps);
    } else if (this.tool === 'historyBrush') {
      this.engine.setHistoryBrush(0);  // 离开历史画笔归零
    }
    if (id === 'heal' || id === 'spotHeal') {
      // M19a 修复族：互斥克隆/效果/蒙版目标/历史画笔；修复画笔 = kind 1，
      // 污点修复 = 2 + 模式（内容识别/创建纹理/接近匹配）
      this.maskTarget = false;
      this.engine.setBrushTarget(false);
      this.engine.setBrushClone(false);
      this.engine.setBrushEffect(0);
      this.engine.setHistoryBrush(0);
      this.engine.setBrushHeal(id === 'heal' ? 1 : 2 + this.spotHealMode);
    } else if (this.tool === 'heal' || this.tool === 'spotHeal') {
      this.engine.setBrushHeal(0);  // 离开修复族归零
    }
    if (this.effectModeOf(id) > 0) {
      // M11e 效果工具：互斥克隆/蒙版目标，激活对应内核
      this.maskTarget = false;
      this.engine.setBrushTarget(false);
      this.engine.setBrushClone(false);
      this.engine.setBrushEffect(this.effectModeOf(id));
      this.effectTool = id;
    } else if (this.effectModeOf(this.tool) > 0 && id !== 'stamp') {
      this.engine.setBrushEffect(0);  // 离开效果组归零
    }
    if (this.gradDragging) {
      this.gradDragging = false;  // 换工具丢弃未完成拖线
      this.gradientLine = null;
    }
    if (this.penPoints.length > 0) {
      this.penPoints = [];  // 换工具丢弃未完成路径
      this.penClosed = false;
      this.engine.setWorkPath([]);
    }
    if (this.shapeDragging) {
      this.shapeDragging = false;  // 换工具丢弃未完成形状
      this.shapePreview = null;
    }
    if (this.tool === 'crop') {
      this.cropRect = null;  // 换工具即丢弃未提交裁剪框
    }
    if (this.textDragging) {
      this.textDragging = false;  // 换工具丢弃未完成拖框
      this.textDrag = null;
    }
    if (this.editingTextId !== 0) {
      this.commitTextEditing();  // 换工具退出文本编辑会话（PS 语义：提交）
    }
    if (this.tool === 'polyLasso' && id !== 'polyLasso') {
      this.cancelPolygonSelection();  // 换工具丢弃未闭合多边形
    }
    this.tool = id;
    if (id === 'text') {
      this.ensureFontList();  // M12：字体列表懒加载（面板可见前就绪）
    }
    if (id === 'stamp') {
      this.maskTarget = false;  // 图章与蒙版目标互斥（克隆优先走像素）
      this.engine.setBrushTarget(false);
      this.engine.setBrushClone(true);
    }
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
    // 铅笔 = 硬度 100 画笔别名（PS 语义；选项条硬度滑条被忽略）
    const hardness = this.tool === 'pencil' ? 1.0 : this.brushHardness / 100;
    this.engine.setBrushSettings(this.brushSize, hardness, this.brushOpacity / 100,
      rgb[0], rgb[1], rgb[2], this.tool === 'eraser');
  }

  /** M9c：颜色变更统一入口（引擎 fg/bg + UI 镜像同步）。 */
  private applyColors(fg: string, bg: string): void {
    this.brushColor = fg;
    this.bgColor = bg;
    const f = this.hexToRgb(fg);
    const b = this.hexToRgb(bg);
    this.engine.setColors(f[0], f[1], f[2], b[0], b[1], b[2]);
  }

  // ---------- M14c 图层样式/填充层参数应用（滑条/色板实时生效，一步撤销/命令） ----------

  private applyDropShadow(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const c = this.hexToRgb(this.styShadowColor);
    const ok = this.engine.setDropShadow(a.id, this.styShadowOn, this.styAngle, this.styDist,
      this.stySize, this.styShadowOp / 100, c[0], c[1], c[2], this.styShadowSpread / 100,
      this.styShadowGL);
    this.log(`drop shadow on=${this.styShadowOn}: ${ok}`);
  }

  private applyStroke(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const c = this.hexToRgb(this.styStrokeColor);
    const ok = this.engine.setLayerStroke(a.id, this.styStrokeOn, this.styStrokeW,
      c[0], c[1], c[2]);
    this.log(`stroke on=${this.styStrokeOn}: ${ok}`);
  }

  private applyOverlay(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const c = this.hexToRgb(this.styOverlayColor);
    const ok = this.engine.setColorOverlay(a.id, this.styOverlayOn, c[0], c[1], c[2],
      this.styOverlayOp / 100);
    this.log(`overlay on=${this.styOverlayOn}: ${ok}`);
  }

  // ---------- M17a 四新效果 ----------
  private applyInnerShadow(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const c = this.hexToRgb(this.styIsColor);
    const ok = this.engine.setInnerShadow(a.id, this.styInnerShadowOn, this.styIsAngle,
      this.styIsDist, this.styIsSize, this.styIsOp / 100, c[0], c[1], c[2],
      this.styIsSpread / 100, this.styIsGL);
    this.log(`inner shadow on=${this.styInnerShadowOn}: ${ok}`);
  }

  private applyOuterGlow(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const c = this.hexToRgb(this.styOgColor);
    const ok = this.engine.setOuterGlow(a.id, this.styOuterGlowOn, this.styOgSize,
      this.styOgOp / 100, c[0], c[1], c[2], this.styOgSpread / 100);
    this.log(`outer glow on=${this.styOuterGlowOn}: ${ok}`);
  }

  private applyInnerGlow(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const c = this.hexToRgb(this.styIgColor);
    const ok = this.engine.setInnerGlow(a.id, this.styInnerGlowOn, this.styIgSize,
      this.styIgOp / 100, c[0], c[1], c[2], this.styIgSpread / 100);
    this.log(`inner glow on=${this.styInnerGlowOn}: ${ok}`);
  }

  // ---------- M17d 等高线 / 图案叠加 / Range ----------

  private applyPatternOverlay(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const ok = this.engine.setPatternOverlay(a.id, this.styPatternOn, this.styPatternId,
      this.styPatternScale / 100, this.styPatternOp / 100);
    this.log(`pattern overlay on=${this.styPatternOn}: ${ok}`);
  }

  // 发光「范围」滑条：range-only 更新（xs/ys 空数组 = 桥接约定不动点列）
  private applyGlowRange(target: number, pct: number): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const ok = this.engine.setStyleContour(a.id, target, [], [], [], pct / 100);
    this.log(`glow range t=${target} ${Math.round(pct)}%: ${ok}`);
  }

  private openContour(target: number): void {
    this.ctTarget = target;
    this.ctPts = [0, 0, 1, 1];
    this.ctCorners = [0, 0];
    this.ctRange = target === 2 ? this.styOgRange : (target === 3 ? this.styIgRange : 50);
    this.ctDrag = -1;
    this.showContour = true;
  }

  private ctLabel(): string {
    switch (this.ctTarget) {
      case 0:
        return '投影';
      case 1:
        return '内阴影';
      case 2:
        return '外发光';
      case 3:
        return '内发光';
      case 4:
        return '光泽';
      case 5:
        return '斜面轮廓';
      default:
        return '斜面光泽等高线';
    }
  }

  // 自然样条二阶导（镜像 style_ops contourCurveLut：三对角追赶解 m2）
  private ctSplineM2(xs: number[], ys: number[]): number[] {
    const n = xs.length;
    const m2: number[] = new Array<number>(n).fill(0);
    if (n < 3) {
      return m2;
    }
    const c: number[] = new Array<number>(n).fill(0);
    const d: number[] = new Array<number>(n).fill(0);
    for (let i = 1; i + 1 < n; ++i) {
      const h0 = xs[i] - xs[i - 1];
      const h1 = xs[i + 1] - xs[i];
      const a = h0 / 6;
      const b = (h0 + h1) / 3;
      const cc = h1 / 6;
      const r = (ys[i + 1] - ys[i]) / h1 - (ys[i] - ys[i - 1]) / h0;
      const den = b - a * c[i - 1];
      c[i] = cc / den;
      d[i] = (r - a * d[i - 1]) / den;
    }
    for (let i = n - 2; i >= 1; --i) {
      m2[i] = d[i] - c[i] * m2[i + 1];
    }
    return m2;
  }

  private ctSplineEval(xs: number[], ys: number[], x: number): number {
    const n = xs.length;
    if (n < 2) {
      return x;
    }
    if (x <= xs[0]) {
      return ys[0];
    }
    if (x >= xs[n - 1]) {
      return ys[n - 1];
    }
    let i = n - 2;
    for (let k = 0; k + 1 < n; ++k) {
      if (x <= xs[k + 1]) {
        i = k;
        break;
      }
    }
    const m2 = this.ctSplineM2(xs, ys);
    const h = xs[i + 1] - xs[i];
    const A = (xs[i + 1] - x) / h;
    const B = (x - xs[i]) / h;
    const y = A * ys[i] + B * ys[i + 1] +
      ((A * A * A - A) * m2[i] + (B * B * B - B) * m2[i + 1]) * h * h / 6;
    return Math.min(1, Math.max(0, y));
  }

  // 预览采样（镜像 buildContourLut：角点分段各自成样条，段间平台 = 前段末值）
  private ctSample(x: number): number {
    const n = this.ctPts.length / 2;
    if (n < 2) {
      return x;
    }
    const idx: number[] = [];
    for (let k = 0; k < n; ++k) {
      idx.push(k);
    }
    idx.sort((a: number, b: number): number => this.ctPts[a * 2] - this.ctPts[b * 2]);
    let anyCorner = false;
    for (const cv of this.ctCorners) {
      if (cv !== 0) {
        anyCorner = true;
      }
    }
    if (!anyCorner) {
      const xs: number[] = [];
      const ys: number[] = [];
      for (const k of idx) {
        xs.push(this.ctPts[k * 2]);
        ys.push(this.ctPts[k * 2 + 1]);
      }
      return this.ctSplineEval(xs, ys, x);
    }
    let start = 0;
    const segs: number[][] = [];
    for (let i = 1; i < idx.length; ++i) {
      const ci = idx[i];
      const isCorner = ci < this.ctCorners.length && this.ctCorners[ci] !== 0;
      if (isCorner || i === idx.length - 1) {
        const seg: number[] = [];
        for (let j = start; j <= i; ++j) {
          seg.push(this.ctPts[idx[j] * 2]);
          seg.push(this.ctPts[idx[j] * 2 + 1]);
        }
        segs.push(seg);
        start = i;
      }
    }
    let prevEndY = 0;
    for (const g of segs) {
      const m = g.length / 2;
      if (x < g[0]) {
        return prevEndY;
      }
      if (x <= g[(m - 1) * 2]) {
        const xs: number[] = [];
        const ys: number[] = [];
        for (let k = 0; k < m; ++k) {
          xs.push(g[k * 2]);
          ys.push(g[k * 2 + 1]);
        }
        return this.ctSplineEval(xs, ys, x);
      }
      prevEndY = g[(m - 1) * 2 + 1];
    }
    return prevEndY;
  }

  private drawContour(): void {
    const c = this.contourCtx;
    const P = 12;
    const SZ = 120;
    c.clearRect(0, 0, 220, 150);
    c.strokeStyle = '#333333';
    c.lineWidth = 1;
    for (let i = 0; i <= 4; ++i) {
      const p = P + SZ * i / 4;
      c.beginPath();
      c.moveTo(p, P);
      c.lineTo(p, P + SZ);
      c.stroke();
      c.beginPath();
      c.moveTo(P, p);
      c.lineTo(P + SZ, p);
      c.stroke();
    }
    c.strokeStyle = '#555555';
    c.beginPath();
    c.moveTo(P, P + SZ);
    c.lineTo(P + SZ, P);
    c.stroke();
    c.strokeStyle = '#00E5FF';
    c.lineWidth = 2;
    c.beginPath();
    for (let i = 0; i <= 64; ++i) {
      const x = i / 64;
      const y = this.ctSample(x);
      const cx = P + x * SZ;
      const cy = P + (1 - y) * SZ;
      if (i === 0) {
        c.moveTo(cx, cy);
      } else {
        c.lineTo(cx, cy);
      }
    }
    c.stroke();
    const n = this.ctPts.length / 2;
    for (let k = 0; k < n; ++k) {
      const px = P + this.ctPts[k * 2] * SZ;
      const py = P + (1 - this.ctPts[k * 2 + 1]) * SZ;
      const corner = k < this.ctCorners.length && this.ctCorners[k] !== 0;
      if (k === this.ctDrag) {
        c.fillStyle = '#FFB300';
      } else {
        c.fillStyle = '#FFFFFF';
      }
      if (corner) {
        c.fillRect(px - 4, py - 4, 8, 8);
      } else {
        c.beginPath();
        c.arc(px, py, 4.5, 0, 6.283);
        c.fill();
      }
    }
  }

  private ctTouch(e: TouchEvent): void {
    if (e.touches.length < 1 && e.changedTouches.length < 1) {
      return;
    }
    const t: TouchObject = e.changedTouches.length > 0 ? e.changedTouches[0] : e.touches[0];
    const P = 12;
    const SZ = 120;
    if (e.type === TouchType.Down) {
      let hit = -1;
      let best = 14;
      for (let k = 0; k < this.ctPts.length / 2; ++k) {
        const d = Math.hypot(P + this.ctPts[k * 2] * SZ - t.x,
          P + (1 - this.ctPts[k * 2 + 1]) * SZ - t.y);
        if (d < best) {
          best = d;
          hit = k;
        }
      }
      if (hit >= 0) {
        this.ctDrag = hit;
      } else {
        const x = Math.min(1, Math.max(0, (t.x - P) / SZ));
        const y = Math.min(1, Math.max(0, 1 - (t.y - P) / SZ));
        let ins = this.ctPts.length / 2;
        for (let k = 0; k < this.ctPts.length / 2; ++k) {
          if (this.ctPts[k * 2] > x) {
            ins = k;
            break;
          }
        }
        this.ctPts.splice(ins * 2, 0, x, y);
        this.ctCorners.splice(ins, 0, 0);
        this.ctDrag = ins;
        this.ctCommit();
        this.drawContour();
      }
      this.drawContour();
    } else if (e.type === TouchType.Move && this.ctDrag >= 0) {
      const x = Math.min(1, Math.max(0, (t.x - P) / SZ));
      const y = Math.min(1, Math.max(0, 1 - (t.y - P) / SZ));
      let nx = x;
      if (this.ctDrag > 0) {
        nx = Math.max(this.ctPts[(this.ctDrag - 1) * 2] + 0.02, nx);
      }
      if (this.ctDrag < this.ctPts.length / 2 - 1) {
        nx = Math.min(this.ctPts[(this.ctDrag + 1) * 2] - 0.02, nx);
      }
      this.ctPts[this.ctDrag * 2] = nx;
      this.ctPts[this.ctDrag * 2 + 1] = y;
      this.drawContour();
    } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.ctDrag >= 0) {
      this.ctDrag = -1;
      this.ctCommit();
      this.drawContour();
    }
  }

  private ctCommit(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const xs: number[] = [];
    const ys: number[] = [];
    for (let k = 0; k < this.ctPts.length / 2; ++k) {
      xs.push(this.ctPts[k * 2]);
      ys.push(this.ctPts[k * 2 + 1]);
    }
    const ok = this.engine.setStyleContour(a.id, this.ctTarget, xs, ys,
      this.ctCorners.slice(), this.ctRange / 100);
    this.log(`contour t=${this.ctTarget} pts=${xs.length}: ${ok}`);
  }

  private ctToggleCorner(): void {
    if (this.ctDrag < 0 || this.ctDrag >= this.ctCorners.length) {
      return;
    }
    this.ctCorners[this.ctDrag] = this.ctCorners[this.ctDrag] !== 0 ? 0 : 1;
    this.ctCommit();
    this.drawContour();
  }

  private ctDeletePoint(): void {
    if (this.ctDrag < 0 || this.ctPts.length / 2 <= 2) {
      return;
    }
    this.ctPts.splice(this.ctDrag * 2, 2);
    this.ctCorners.splice(this.ctDrag, 1);
    this.ctDrag = -1;
    this.ctCommit();
    this.drawContour();
  }

  private ctResetLinear(): void {
    this.ctPts = [0, 0, 1, 1];
    this.ctCorners = [0, 0];
    this.ctDrag = -1;
    this.ctCommit();
    this.drawContour();
  }

  private applyGradientOverlay(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const f = this.hexToRgb(this.styGdFrom);
    const t = this.hexToRgb(this.styGdTo);
    const ok = this.engine.setGradientOverlay(a.id, this.styGradientOn, this.styGdAngle,
      this.styGdOp / 100, f[0], f[1], f[2], t[0], t[1], t[2]);
    this.log(`gradient overlay on=${this.styGradientOn}: ${ok}`);
  }

  // ---------- M17b 光泽/斜面/全局光 ----------
  private applySatin(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const c = this.hexToRgb(this.stySatinColor);
    const ok = this.engine.setSatin(a.id, this.stySatinOn, this.stySatinAngle,
      this.stySatinDist, this.stySatinSize, this.stySatinOp / 100, c[0], c[1], c[2],
      this.stySatinInvert);
    this.log(`satin on=${this.stySatinOn}: ${ok}`);
  }

  private applyBevel(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const hi = this.hexToRgb(this.styBevelHiColor);
    const sh = this.hexToRgb(this.styBevelShColor);
    const ok = this.engine.setBevel(a.id, this.styBevelOn, this.styBevelOuter ? 1 : 0,
      this.styBevelDepth, this.styBevelUp, this.styBevelSize, this.styBevelAngle,
      this.styBevelAlt, this.styBevelHiOp / 100, hi[0], hi[1], hi[2],
      this.styBevelShOp / 100, sh[0], sh[1], sh[2]);
    this.log(`bevel on=${this.styBevelOn}: ${ok}`);
  }

  private applyGlobalLight(): void {
    const ok = this.engine.setGlobalLight(this.globalLightAngle);
    this.log(`全局光 ${Math.round(this.globalLightAngle)}°: ${ok}`);
  }

  private applyFillParams(): void {
    const a = this.activeLayer();
    if (a === undefined || !a.isFill) {
      return;
    }
    const c = this.hexToRgb(this.fillColor);
    const ok = this.engine.setFillLayerParams(a.id, this.fillKind, c[0], c[1], c[2],
      this.fillAngle);
    this.log(`fill params kind=${this.fillKind}: ${ok}`);
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
    if (this.editingTextId !== 0) {
      this.commitTextEditing();  // 文本编辑会话独占 history pending 槽：任何命令先提交
    }
    // M20d 消失点：进透视克隆工具
    if (id === 'vanishing.enter') {
      if (this.docOpen) {
        this.pickTool('vanishing');
        this.log('消失点：点 4 下定平面 → 取源 → 点按克隆（选项条重置）');
      }
      return;
    }
    // M20d 自适应广角：对话框（模型/焦距/裁切/缩放）
    if (id === 'wideangle.enter') {
      if (this.docOpen) {
        this.showWideAngle = true;
      }
      return;
    }
    // M20d 透视变形：进透视工具（拖四角→拉直→应用）
    if (id === 'perspective.enter') {
      if (this.docOpen) {
        this.pickTool('perspective');
        this.perspectiveEnter();
        this.log('透视变形：拖四角 → 「拉直」可选 → 「应用」');
      }
      return;
    }
    // M20c 操控变形：进操控工具（点击=钉，拖钉=变形，选项条应用）
    if (id === 'puppet.enter') {
      if (this.docOpen) {
        this.pickTool('puppet');
        this.log('操控变形：点击加钉 → 拖钉变形 → 选项条「应用」');
      }
      return;
    }
    // M20b 液化：进液化工具（选项条选工具/笔刷，画布拖拽一笔一事务）
    if (id === 'liquify.enter') {
      if (this.docOpen) {
        this.pickTool('liquify');
        this.log('液化：拖拽变形 · 每笔一步历史 · 选项条换工具');
      }
      return;
    }
    // M22 显示变换命令
    if (id === 'view.proofOff' || id === 'view.lookupOff') {
      this.engine.setViewProof('', 1, false).then((): void => { this.refreshViewXformMirror(); });
      if (id === 'view.lookupOff') {
        this.engine.setViewLookup(-1).then((): void => { this.refreshViewXformMirror(); });
      }
      this.log('显示变换已清除');
      return;
    }
    if (id === 'view.proofCoated') {
      this.engine.setViewProof('coated-cmyk', 1, true).then((ok: boolean): void => {
        this.toast(ok ? '软打样：合成涂布纸 CMYK' : '软打样构建失败');
        this.refreshViewXformMirror();
      });
      return;
    }
    if (id === 'view.proofP3') {
      this.engine.setViewProof('display-p3', 1, true).then((ok: boolean): void => {
        this.toast(ok ? '软打样：Display P3' : '软打样构建失败');
        this.refreshViewXformMirror();
      });
      return;
    }
    if (id === 'view.gamutWarning') {
      const on = !this.viewXformGamut;
      this.engine.setGamutWarning('coatedCmyk', 2.0, on).then((ok: boolean): void => {
        if (ok) {
          this.viewXformGamut = on;
          this.toast(on ? '色域警告开（CMYK 涂布）' : '色域警告关');
        } else {
          this.toast('色域警告构建失败');
        }
      });
      return;
    }
    if (id === 'view.lookup0' || id === 'view.lookupMono') {
      const param = id === 'view.lookup0' ? 0 : 2;
      this.engine.setViewProof('', 1, false).then((): void => {
        this.engine.setViewLookup(param).then((ok: boolean): void => {
          this.toast(ok ? '校样颜色已应用' : '查找表应用失败');
          this.refreshViewXformMirror();
        });
      });
      return;
    }
    // M23c：Camera Raw 滤镜（独立对话框，cameraJson 参数面超出目录 schema）
    if (id === 'filter.cameraRaw') {
      this.openCameraRawFilterDialog();
      return;
    }
    // M13 滤镜命令：'filter.<id>' → 查目录开对话框（proxy 预览）
    if (id.startsWith('filter.')) {
      const def = FILTER_CATALOG.find((f: FilterDef): boolean => f.id === id.substring(7));
      if (def !== undefined) {
        this.openFilterDialog(def);
      }
      return;
    }
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
      case 'file.saveSandbox': {
        // 隐藏调试通道（模拟器 picker 保存缺陷的往返验证替代，M12 定案）：cacheDir/rt_check.montage
        const path = this.cachePath('rt_check.montage');
        let f: fs.File;
        try {
          f = fs.openSync(path, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
        } catch (e) {
          const be = e as BusinessError;
          this.log(`沙箱保存打开失败: ${be.code} ${be.message}`);
          break;
        }
        this.engine.saveProject(f.fd).then((d: SaveProjectData): void => {
          this.log(`沙箱往返保存: ${d.layers} 层 ${d.bytes} 字节`);
          this.toast(`沙箱保存完成（${d.layers} 层）`);
        }).catch((e: BusinessError): void => {
          this.log(`沙箱保存失败: ${e.message}`);
        });
        break;
      }
      case 'file.openSandbox': {
        const p2 = this.cachePath('rt_check.montage');
        let f2: fs.File;
        try {
          f2 = fs.openSync(p2, fs.OpenMode.READ_ONLY);
        } catch (e) {
          const be = e as BusinessError;
          this.log(`沙箱打开失败（无往返包？先保存）: ${be.code} ${be.message}`);
          break;
        }
        this.engine.openProject(f2.fd).then((d: OpenProjectData): void => {
          this.docW = d.width;
          this.docH = d.height;
          this.afterDocChanged();
          this.log(`沙箱往返打开: ${d.width}x${d.height} ${d.layers} 层 notes=${d.notes.length}`);
          for (const n of d.notes) {
            this.log(`降级提示: ${n}`);
          }
          this.toast(`沙箱打开完成（${d.layers} 层）`);
        }).catch((e: BusinessError): void => {
          this.log(`沙箱打开失败: ${e.message}`);
        });
        break;
      }
      case 'file.openSandboxDng': {
        // M15d：DNG RAW 显影设备验证通道（内嵌合成 DNG base64 → cacheDir → 显影打开；
        // 模拟器 picker/沙箱写入受限，自包含可复跑）
        const B64_DNG = 'SUkqAAgAAAAOAAABBAABAAAACAAAAAEBBAABAAAACAAAAAIBAwABAAAAEAAAAAMBAwABAAAAAQAAAAYBAwABAAAAI4AAABEBBAABAAAAzgAAABUBAwABAAAAAQAAABcBBAABAAAAgAAAAI2CAwACAAAAAgACAI6CAQAEAAAAAAEBAhLGAQAEAAAAAQQAABrGAwABAAAAyAAAAB3GBAABAAAADDAAACjGBQADAAAAtgAAAAAAAAD0AQAA6AMAAOgDAADoAwAA6AMAAOgDAABwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwF3AXcBdwFw==';
        const pd = this.cachePath('raw_test.dng');
        const bin: Uint8Array = new util.Base64Helper().decodeSync(B64_DNG);
        let fw: fs.File;
        try {
          fw = fs.openSync(pd, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
          fs.writeSync(fw.fd, bin.buffer);
          fs.closeSync(fw);
        } catch (e) {
          const be = e as BusinessError;
          this.log(`沙箱 DNG 写入失败: ${be.code} ${be.message}`);
          break;
        }
        let fd2: fs.File;
        try {
          fd2 = fs.openSync(pd, fs.OpenMode.READ_ONLY);
        } catch (e) {
          const be = e as BusinessError;
          this.log(`沙箱 DNG 打开失败: ${be.code} ${be.message}`);
          break;
        }
        this.engine.openFileFromFd(fd2.fd).then((dd: OpenFileData): void => {
          this.docW = dd.width;
          this.docH = dd.height;
          this.docOpen = true;
          this.log(`沙箱 DNG 显影: ${dd.width}x${dd.height}`);
        }).catch((e: BusinessError): void => {
          this.log(`沙箱 DNG 打开失败: ${e.message}`);
        });
        break;
      }
      case 'file.openSandboxCr2': {
        // M23a：厂牌 RAW 设备验证通道（内嵌合成 CR2 base64——host testgen 生成并经
        // 内核自证 24×16 / WB 2.0,1.0,1.5；走 identify→CR2 解码→AHD 显影→发布全链）
        const B64_CR2 = 'SUkqAJIKAABDUgIAEAAAAAMAAwEDAAEAAAAGAAAAEQEEAAEAAABaCwAAFwEEAAEAAADhAQAAAAAAAAIAmoIFAAEAAABYAAAAfJIHAB4AAABgAAAAAAAAAAEAAABkAAAAAgDgAAMAEQAAAH4AAAABQAcA8gkAAKAAAAAAAAAAIgAYABAAAAAAAAgAAgAXAA8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAAEAAQABgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACwAAAQQAAQAAAAIAAAABAQQAAQAAAAIAAAADAQMAAQAAAAEAAAAGAQMAAQAAAAIAAAAPAQIABgAAABwLAAAQAQIAFAAAACILAAARAQQAAQAAADwNAAASAQMAAQAAAAEAAAAVAQMAAQAAAAMAAAAXAQQAAQAAAAwAAABphwQAAQAAADoAAAA2CwAAQ2Fub24AQ2Fub24gRU9TIFN5bnRoZXRpYwABAAABBAABAAAAAQAAAEgLAAABAAABBAABAAAAAQAAABAAAAD/2P/DAA4OABAADAIBEQACEQD/xAAkAAABAwICAgEBAQEBAQEAAAAACQoLCAwGDQcQDw4FBAMCAf/aAAoCAQACAAEAAMh/8h/wAAAAAAAAAAAAAAAAAJOG415EV4bIiPDJEV4bIivDJER4bIivDJEV4aAAFwCZ4+O1FOHaqnLtVTh2qpw7UU4dqqcO1VOQAAk4boFkRXTsiI6dkRXTsiK6dkRHTsiK6dkRXToAAXLRnNm5mU4cyqcOZVOHMqnDmZTlzKpw5lU4AACThuzKRFcWyIji6RFcWyIri6REcWyIri6RFcWgABdZubdK+ynC+ynC+qnK+ynC+ynC+ynC+qnAAAScN4vyIq30iIt9IirfSIq30iIt9IirfSIq3wAAXhlmg+sMpysOpwsMpwsMpwsMpwsOpysMpwAAEnDexkiKqdIiKnyIqp0iKqfIiKnSIqp8iKqcAAF7L5irKLqcKLKcKLqcKLqcqLqcKLKcKLqcAABJw31zIi1vyIlb8iLW/Ii1vyIlb8iLW/Ii1vgABffl9GVTqcVTqc1TqcVUKcVTqcVTqcVTqcgABJw3/NIi6b5ETThIi6b5EXThIiab5EXThIi6bwAAzBgu8PHCnGOFOMcKcY3U4xwpzjhTjHCnH//ZAICAgICAgICAgICAgA==';
        const pdCr2 = this.cachePath('raw_test.cr2');
        const binCr2: Uint8Array = new util.Base64Helper().decodeSync(B64_CR2);
        let fwC: fs.File;
        try {
          fwC = fs.openSync(pdCr2, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
          fs.writeSync(fwC.fd, binCr2.buffer);
          fs.closeSync(fwC);
        } catch (e) {
          const be = e as BusinessError;
          this.log(`沙箱 CR2 写入失败: ${be.code} ${be.message}`);
          break;
        }
        let fdC: fs.File;
        try {
          fdC = fs.openSync(pdCr2, fs.OpenMode.READ_ONLY);
        } catch (e) {
          const be = e as BusinessError;
          this.log(`沙箱 CR2 打开失败: ${be.code} ${be.message}`);
          break;
        }
        this.engine.openFileFromFd(fdC.fd).then((dd: OpenFileData): void => {
          this.docW = dd.width;
          this.docH = dd.height;
          this.docOpen = true;
          this.log(`沙箱 CR2 显影: ${dd.width}x${dd.height}`);
        }).catch((e: BusinessError): void => {
          this.log(`沙箱 CR2 打开失败: ${e.message}`);
        });
        break;
      }
      case 'file.debugJpeg':
        // M16a：JPEG 分带/swapRB 校准通道（模拟器自证 + 真机复跑同通道）
        this.debugJpegCalibration();
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
      case 'layer.newGroup': {
        const gid = this.engine.addGroup();
        if (gid !== 0) {
          this.log(`新建组 #${gid}`);
        } else {
          this.toast('新建组失败');
        }
        break;
      }
      case 'layer.group': {
        const ga = this.activeLayer();
        if (ga === undefined || ga.isGroup) {
          this.toast('选中一个图层再编组');
          break;
        }
        const ngid = this.engine.groupLayer(ga.id);
        if (ngid !== 0) {
          this.toast(`已归入新组`);
          this.log(`图层 #${ga.id} → 新组 #${ngid}`);
        } else {
          this.toast('编组失败');
        }
        break;
      }
      case 'layer.ungroup': {
        const ua = this.activeLayer();
        if (ua === undefined || !ua.isGroup) {
          this.toast('活动图层不是组');
          break;
        }
        if (this.engine.ungroupLayer(ua.id)) {
          this.log(`组 #${ua.id} 已解散`);
        } else {
          this.toast('解散组失败');
        }
        break;
      }
      case 'layer.duplicate': {
        const da = this.activeLayer();
        if (da === undefined) {
          break;
        }
        const nid = this.engine.duplicateLayer(da.id);
        if (nid !== 0) {
          this.toast(`已复制图层 → ${da.name} 副本`);
          this.log(`duplicate #${da.id} → #${nid}`);
        } else {
          this.toast('复制图层失败');
        }
        break;
      }
      case 'layer.mergeDown': {
        if (this.engine.mergeDown()) {
          this.toast('已向下合并');
          this.log('merge down');
        } else {
          this.toast('合并向下失败（下方需为相邻像素层）');
        }
        break;
      }
      case 'layer.mergeVisible': {
        if (this.engine.mergeVisible()) {
          this.toast('已合并可见图层');
          this.log('merge visible');
        } else {
          this.toast('合并可见失败（可见层 ≥2）');
        }
        break;
      }
      case 'layer.stamp': {
        const sid = this.engine.stampVisible();
        if (sid !== 0) {
          this.toast(`已盖印 → 图层 #${sid}`);
          this.log(`stamp visible → #${sid}`);
        } else {
          this.toast('盖印失败（无可见图层）');
        }
        break;
      }
      case 'layer.flatten': {
        if (this.engine.flattenImage()) {
          this.toast('已拼合图像');
          this.log('flatten image');
        } else {
          this.toast('拼合失败');
        }
        break;
      }
      case 'layer.style': {
        this.patternList = this.engine.getPatterns();  // M17d：图案选择器数据
        this.showStyle = true;
        break;
      }
      case 'layer.copyStyle': {
        const a = this.activeLayer();
        if (a !== undefined && this.engine.copyLayerStyle(a.id)) {
          this.toast('图层样式已拷贝');
          this.log(`拷贝样式: 层${a.id}`);
        }
        break;
      }
      case 'layer.pasteStyle': {
        const a = this.activeLayer();
        if (a !== undefined && this.engine.pasteLayerStyle(a.id)) {
          this.toast('图层样式已粘贴');
          this.log(`粘贴样式: 层${a.id}`);
        }
        break;
      }
      case 'layer.toggleEffects': {
        const a = this.activeLayer();
        if (a !== undefined) {
          const on = a.styleOn;
          if (this.engine.setLayerEffectsEnabled(a.id, !on)) {
            this.log(`全部效果: ${!on ? '显示' : '隐藏'}`);
          }
        }
        break;
      }
      case 'layer.fillSolid': {
        const f = this.hexToRgb(this.brushColor);
        const nid = this.engine.addFillLayer(0, f[0], f[1], f[2], 90);
        if (nid !== 0) {
          this.toast('已新建纯色填充图层');
          this.log(`fill layer solid #${nid}`);
        } else {
          this.toast('新建填充图层失败');
        }
        break;
      }
      case 'layer.fillGrad': {
        const f = this.hexToRgb(this.brushColor);
        const nid = this.engine.addFillLayer(1, f[0], f[1], f[2], 90);
        if (nid !== 0) {
          this.toast('已新建渐变填充图层');
          this.log(`fill layer grad #${nid}`);
        } else {
          this.toast('新建填充图层失败');
        }
        break;
      }
      case 'layer.alignLeft':
      case 'layer.alignHCenter':
      case 'layer.alignRight':
      case 'layer.alignTop':
      case 'layer.alignVCenter':
      case 'layer.alignBottom':
      case 'layer.distLeft':
      case 'layer.distHCenter':
      case 'layer.distRight':
      case 'layer.distTop':
      case 'layer.distVCenter':
      case 'layer.distBottom': {
        const alignModes: Record<string, number> = {
          'layer.alignLeft': 0, 'layer.alignHCenter': 1, 'layer.alignRight': 2,
          'layer.alignTop': 3, 'layer.alignVCenter': 4, 'layer.alignBottom': 5,
          'layer.distLeft': 0, 'layer.distHCenter': 1, 'layer.distRight': 2,
          'layer.distTop': 3, 'layer.distVCenter': 4, 'layer.distBottom': 5,
        };
        this.onAlignPick(alignModes[id] ?? 0, id.startsWith('layer.align'));
        break;
      }
      case 'layer.convertSmart': {
        const a = this.activeLayer();
        if (a === undefined || a.isGroup || a.isSmart) {
          this.toast('选中一个非智能对象图层');
          break;
        }
        this.busyText = '转换为智能对象…';
        this.engine.convertToSmartObject([a.id]).then((newId: number): void => {
          this.busyText = '';
          if (newId !== 0) {
            this.toast('已转换为智能对象');
            this.log(`转换智能对象 #${a.id} → #${newId}`);
          } else {
            this.toast('转换失败（空图层？）');
          }
        }).catch((e: BusinessError): void => {
          this.busyText = '';
          this.log(`转换失败: ${e.message}`);
        });
        break;
      }
      case 'layer.smartViaCopy': {
        const a = this.activeLayer();
        if (a === undefined || !a.isSmart) {
          this.toast('活动图层不是智能对象');
          break;
        }
        const nid = this.engine.newSmartObjectViaCopy(a.id);
        if (nid !== 0) {
          this.toast('已新建智能对象副本（独立内容）');
          this.log(`智能对象副本 #${nid}`);
        } else {
          this.toast('新建副本失败');
        }
        break;
      }
      case 'layer.smartRasterize': {
        const a = this.activeLayer();
        if (a === undefined || !a.isSmart) {
          this.toast('活动图层不是智能对象');
          break;
        }
        const ok = this.engine.rasterizeSmartObject(a.id);
        this.toast(ok ? '已栅格化（不可再编辑内容/滤镜）' : '栅格化失败');
        this.log(`栅格化智能对象 #${a.id}: ${ok}`);
        break;
      }
      case 'layer.smartReplace':
        this.pickSmartSource(true);
        break;
      case 'layer.smartExport':
        this.exportSmartContents();
        break;
      case 'layer.smartFiltersDialog':
        this.openSmartFiltersDialog();
        break;
      case 'layer.smartFiltersToggle': {
        const a = this.activeLayer();
        if (a !== undefined && a.isSmart) {
          this.onSmartFiltersEnabled(!a.smartFiltersOn);
        }
        break;
      }
      case 'layer.smartMaskClear': {
        const a = this.activeLayer();
        if (a !== undefined && a.isSmart) {
          this.onSmartFilterMask(0, false);
          this.toast('滤镜蒙版已清除');
        }
        break;
      }
      case 'file.placeEmbedded':
        this.pickSmartSource(false);
        break;
      case 'layer.rasterizeText': {
        const a = this.activeLayer();
        if (a !== undefined && a.isText) {
          const ok = this.engine.rasterizeTextLayer(a.id);
          this.toast(ok ? '文字已栅格化（不可再编辑）' : '栅格化失败');
          this.log(`栅格化文字层 #${a.id}: ${ok}`);
        } else {
          this.toast('活动图层不是文字层');
        }
        break;
      }
      case 'select.deselect':
        this.engine.clearSelection();
        this.hasSelectionFlag = false;
        this.log('选区已取消');
        break;
      case 'select.all': {
        const ok = this.engine.selectAll();
        this.hasSelectionFlag = ok;
        this.log(`全选: ${ok}`);
        break;
      }
      case 'select.inverse': {
        const ok = this.engine.invertSelection();
        this.hasSelectionFlag = ok;
        this.log(`反选: ${ok}`);
        break;
      }
      case 'select.reselect': {
        const ok = this.engine.reselectSelection();
        this.hasSelectionFlag = ok;
        this.log(`重新选择: ${ok ? '已恢复最近取消的选区' : '无可恢复选区'}`);
        break;
      }
      case 'select.quickMask': {
        // M10d 快速蒙版 toggle（PS Q）：进入 = 选区挂起转通道（红 overlay），退出 = 通道转选区
        if (!this.docOpen) {
          this.toast('无文档');
          break;
        }
        const st = this.engine.toggleQuickMask();
        if (st !== null) {
          this.quickMaskActive = st.active;
          this.hasSelectionFlag = st.hasSelection;
          if (st.active) {
            this.maskTarget = false;  // QM 模式笔画由引擎路由到通道，图层蒙版目标挂起
          }
          this.log(`快速蒙版: ${st.active ? '进入（蒙版笔编辑通道，Q 退出）' : '退出'}`);
        }
        break;
      }
      case 'select.feather':
        if (this.docOpen && this.hasSelectionFlag) {
          this.modifyKind = 'feather';
          this.modifyText = '10';
          this.showModify = true;
        } else {
          this.toast('需要先建立选区');
        }
        break;
      case 'select.border':
        if (this.docOpen && this.hasSelectionFlag) {
          this.modifyKind = 'border';
          this.modifyText = '5';
          this.showModify = true;
        } else {
          this.toast('需要先建立选区');
        }
        break;
      case 'select.expand':
        if (this.docOpen && this.hasSelectionFlag) {
          this.modifyKind = 'expand';
          this.modifyText = '5';
          this.showModify = true;
        } else {
          this.toast('需要先建立选区');
        }
        break;
      case 'select.contract':
        if (this.docOpen && this.hasSelectionFlag) {
          this.modifyKind = 'contract';
          this.modifyText = '5';
          this.showModify = true;
        } else {
          this.toast('需要先建立选区');
        }
        break;
      case 'select.smooth': {
        if (!this.docOpen || !this.hasSelectionFlag) {
          this.toast('需要先建立选区');
          break;
        }
        const ok = this.engine.smoothSelection();
        this.log(`平滑选区: ${ok}`);
        break;
      }
      case 'select.store':
        if (this.docOpen && this.hasSelectionFlag) {
          this.storeSelText = `选区 ${this.channelsCount + 1}`;  // 预填默认名（PS 记忆语义）
          this.showStoreSel = true;
        } else {
          this.toast('需要先建立选区');
        }
        break;
      case 'select.load':
        if (this.docOpen && this.channelsCount > 0) {
          this.loadSelText = `选区 ${this.channelsCount}`;  // 预填最近通道名
          this.showLoadSel = true;
        } else {
          this.toast('无已存储的通道');
        }
        break;
      case 'edit.copy':
        this.runClipboard(false, false);
        break;
      case 'edit.copyMerged':
        this.runClipboard(true, false);
        break;
      case 'edit.cut':
        this.runClipboard(false, true);
        break;
      case 'edit.paste': {
        if (!this.docOpen) {
          break;
        }
        const ok = this.engine.pasteClipboard();
        this.log(`粘贴: ${ok ? '新图层已创建' : '剪贴板为空'}`);
        break;
      }
      case 'edit.clear': {
        if (!this.docOpen || !this.hasSelectionFlag) {
          this.toast('需要先建立选区');
          break;
        }
        const ok = this.engine.deleteSelectionPixels();
        this.log(`清除选区像素: ${ok}`);
        break;
      }
      case 'edit.fill':
        if (this.docOpen) {
          this.fillSource = 0;
          this.showFill = true;
        }
        break;
      case 'edit.contentAwareFill':
        // M19b 内容感知填充：选区 = 孔，采样窗口内受限源补全（异步求解，一步撤销）
        if (this.docOpen && this.hasSelectionFlag) {
          this.toast('内容识别填充计算中…');
          this.engine.contentAwareFill((changed: boolean): void => {
            this.log(`内容识别填充: ${changed ? '已应用' : '失败（需要选区与活动层像素）'}`);
            if (!changed) {
              this.toast('内容识别填充失败');
            }
          });
        } else {
          this.toast('需要先建立选区');
        }
        break;
      case 'edit.stroke':
        if (this.docOpen && this.hasSelectionFlag) {
          this.modifyKind = 'stroke';
          this.modifyText = '2';
          this.showModify = true;
        } else {
          this.toast('需要先建立选区');
        }
        break;
      case 'edit.importLookup': {
        this.doImportLookupTable();
        break;
      }
      case 'edit.definePattern': {
        const p = this.engine.definePattern();
        if (p !== null) {
          this.patternList = this.engine.getPatterns();
          this.toast(`已定义 ${p.name}`);
          this.log(`定义图案: ${p.id} ${p.w}x${p.h}`);
        } else {
          this.toast('定义图案失败（需要选区与活动层像素）');
        }
        break;
      }
      case 'image.rotate90cw':
        this.docOp((): Promise<boolean> => this.engine.rotateDocument90(true), '旋转 90° 顺时针');
        break;
      case 'image.rotate90ccw':
        this.docOp((): Promise<boolean> => this.engine.rotateDocument90(false), '旋转 90° 逆时针');
        break;
      case 'image.rotate180':
        this.docOp(async (): Promise<boolean> => {
          const a = await this.engine.rotateDocument90(true);
          return a && await this.engine.rotateDocument90(true);
        }, '旋转 180°');
        break;
      case 'image.flipH':
        this.docOp((): Promise<boolean> => this.engine.flipDocument(true), '水平翻转');
        break;
      case 'image.flipV':
        this.docOp((): Promise<boolean> => this.engine.flipDocument(false), '垂直翻转');
        break;
      case 'image.trim':
        this.docOp((): Promise<boolean> => this.engine.trimCanvas(), '裁切');
        break;
      case 'image.develop':
        this.openDevelopPanel();
        break;
      case 'layer.globalLight':
        this.showGlobalLight = true;
        break;
      case 'edit.defineCustomShape':
        if (this.engine.defineCustomShape('')) {
          this.log('已定义自定形状');
        }
        break;
      case 'edit.placeCustomSub':
        break;
      case 'layer.vectorMaskFromPath': {
        const a = this.activeLayer();
        if (a !== undefined) {
          const flat = this.engine.getWorkPath();
          const ok = this.engine.setLayerVectorMask(a.id, flat);
          this.log(`矢量蒙版: ${ok ? '已添加（自工作路径）' : '失败'}`);
          if (ok) {
            this.refreshLayers();
          }
        }
        break;
      }
      case 'layer.vectorMaskToggle': {
        const a = this.activeLayer();
        if (a !== undefined && a.hasVectorMask) {
          this.engine.setVectorMaskEnabled(a.id, !a.vectorMaskOn);
          this.refreshLayers();
        }
        break;
      }
      case 'layer.vectorMaskRemove': {
        const a = this.activeLayer();
        if (a !== undefined && a.hasVectorMask) {
          this.engine.setLayerVectorMask(a.id, []);
          this.refreshLayers();
        }
        break;
      }
      case 'layer.rasterizeShape': {
        const a = this.activeLayer();
        if (a !== undefined && a.isShape) {
          const ok = this.engine.rasterizeShapeLayer(a.id);
          this.log(`栅格化形状: ${ok ? '完成' : '失败'}`);
          if (ok) {
            this.refreshLayers();
          }
        }
        break;
      }
      case 'image.revealAll':
        this.docOp((): Promise<boolean> => this.engine.revealAll(), '显示全部');
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
      // ---------- M15a 视图：标尺 / 参考线 / 网格 / 吸附 / 屏幕模式 ----------
      case 'view.rulers':
        this.rulersVisible = !this.rulersVisible;
        this.prefs.setBool('view_rulers', this.rulersVisible);
        this.log(`标尺: ${this.rulersVisible ? '开' : '关'}`);
        break;
      case 'view.rulerUnitPx':
      case 'view.rulerUnitPct':
        this.rulerUnit = id === 'view.rulerUnitPx' ? 0 : 1;
        this.prefs.setNumber('view_ruler_unit', this.rulerUnit);
        break;
      case 'view.guidesVisible': {
        const on = !this.guidesMirror.visible;
        this.engine.setGuidesVisible(on);
        this.refreshGuides();
        this.log(`参考线: ${on ? '显示' : '隐藏'}`);
        break;
      }
      case 'view.guidesLocked': {
        const lock = !this.guidesMirror.locked;
        this.engine.setGuidesLocked(lock);
        this.refreshGuides();
        this.toast(lock ? '参考线已锁定' : '参考线已解锁');
        break;
      }
      case 'view.guidesClear':
        if (this.engine.clearGuides()) {
          this.refreshGuides();
          this.log('参考线已清除');
        }
        break;
      case 'view.grid':
        this.gridVisible = !this.gridVisible;
        this.prefs.setBool('view_grid', this.gridVisible);
        this.log(`网格: ${this.gridVisible ? '开' : '关'}`);
        break;
      case 'view.gridSpacing50':
      case 'view.gridSpacing100':
      case 'view.gridSpacing250':
      case 'view.gridSpacing500': {
        const sp = Number(id.replace('view.gridSpacing', ''));
        this.gridSpacing = sp;
        this.prefs.setNumber('view_grid_spacing', sp);
        break;
      }
      case 'view.snapGuides':
        this.snapGuidesOn = !this.snapGuidesOn;
        this.prefs.setBool('view_snap_guides', this.snapGuidesOn);
        this.log(`对齐参考线: ${this.snapGuidesOn ? '开' : '关'}`);
        break;
      case 'view.snapGrid':
        this.snapGridOn = !this.snapGridOn;
        this.prefs.setBool('view_snap_grid', this.snapGridOn);
        this.log(`对齐网格: ${this.snapGridOn ? '开' : '关'}`);
        break;
      case 'view.screenStd':
      case 'view.screenFullMenu':
      case 'view.screenFull':
        this.screenMode = id === 'view.screenStd' ? 0 : id === 'view.screenFullMenu' ? 1 : 2;
        this.log(`屏幕模式: ${this.screenMode === 0 ? '标准' : this.screenMode === 1 ? '带菜单栏全屏' : '全屏'}`);
        break;
      case 'window.toggleLayers':
        this.layersVisible = !this.layersVisible;
        this.prefs.setBool('panel_layers', this.layersVisible);
        break;
      case 'window.toggleHistory':
        this.historyVisible = !this.historyVisible;
        this.prefs.setBool('panel_history', this.historyVisible);
        break;
      case 'window.toggleHistogram':
        this.histogramVisible = !this.histogramVisible;
        this.prefs.setBool('panel_histogram', this.histogramVisible);
        if (this.histogramVisible) {
          this.scheduleHistogram();  // 开启即拉一次
        }
        break;
      case 'window.toggleInfo':
        this.infoVisible = !this.infoVisible;
        this.prefs.setBool('panel_info', this.infoVisible);
        break;
      case 'window.toggleActions':
        this.actionsVisible = !this.actionsVisible;
        this.prefs.setBool('panel_actions', this.actionsVisible);
        break;
      case 'automate.batch':
        this.automateMode = 0;
        this.automateProgress = '';
        this.automateResult = '';
        this.refreshActionNames();
        this.showAutomate = true;
        break;
      case 'automate.processor':
        this.automateMode = 1;
        this.automateProgress = '';
        this.automateResult = '';
        this.refreshActionNames();
        this.showAutomate = true;
        break;
      case 'debug.control':
        this.toggleControlServer();
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
      case 'image.imageSize':
        if (this.docOpen) {
          this.imgWText = `${this.docW}`;
          this.imgHText = `${this.docH}`;
          this.imageSizeMode = 0;
          this.imgResampleMethod = 2;
          this.showImageSize = true;
        }
        break;
      case 'image.contentAwareScale':
        // M19c 内容感知比例：活动图层缝雕刻到新尺寸（选区灰度作保护图；异步）
        if (this.docOpen) {
          this.imgWText = `${this.docW}`;
          this.imgHText = `${this.docH}`;
          this.imageSizeMode = 1;
          this.showImageSize = true;
        }
        break;
      case 'image.canvasSize':
        if (this.docOpen) {
          this.imgWText = `${this.docW}`;
          this.imgHText = `${this.docH}`;
          this.showImageSize = true;  // v1 复用尺寸对话框
        }
        break;
      case 'image.rotateAny':
        if (this.docOpen) {
          this.angleText = '0';
          this.showRotateAny = true;
        }
        break;
      case 'layer.transform':
        this.enterTransformOverlay();
        break;
      case 'layer.transformNumeric':
        if (this.docOpen && this.activeLayer() !== undefined) {
          this.tfScale = 100;
          this.tfRotate = 0;
          this.tfSkew = 0;
          this.showTransform = true;
        }
        break;
      case 'theme.system':
      case 'theme.light':
      case 'theme.dark':
        this.applyTheme(id.slice(6) as ThemeMode);
        break;
      default: {
        // M18d：放置自定形状（edit.placeCustom:<id> 动态项）
        if (id.startsWith('edit.placeCustom:')) {
          const cid = id.slice('edit.placeCustom:'.length);
          const newId = this.engine.placeCustomShape(cid, this.docW / 2, this.docH / 2,
            this.docW / 2, this.docH / 2);
          this.log(`放置自定形状 ${cid}: ${newId > 0 ? `#${newId}` : '失败'}`);
          if (newId > 0) {
            this.refreshLayers();
          }
          break;
        }
        // 占位命令（M8/M10/M14+）：提示并登记
        this.toast('该功能在后续里程碑提供');
        this.log(`命令占位: ${id}`);
        break;
      }
    }
  }

  // M20a 重采样方法下拉项（PS Image Size 选项同序）
  private resampleOptions(): SelectOption[] {
    const opts: SelectOption[] = [];
    for (let i = 0; i < 8; ++i) {
      opts.push({ value: this.resampleLabel(i) });
    }
    return opts;
  }

  // M20b 液化手势：Down 起笔（定点工具单击=dab），Move 采样（≥4px），Up 提交一笔
  private liquifyTouch(e: TouchEvent, docX: number, docY: number): void {
    const a = this.activeLayer();
    if (a === undefined || a.isText) {
      return;
    }
    if (e.type === TouchType.Down) {
      this.liquifyPoints = [[docX, docY, 1]];
      this.liquifyDrawing = true;
      return;
    }
    if (!this.liquifyDrawing) {
      return;
    }
    if (e.type === TouchType.Move) {
      const last = this.liquifyPoints[this.liquifyPoints.length - 1];
      if (last === undefined || Math.hypot(docX - last[0], docY - last[1]) >= 4) {
        this.liquifyPoints.push([docX, docY, 1]);
      }
      return;
    }
    if (e.type === TouchType.Up || e.type === TouchType.Cancel) {
      this.liquifyDrawing = false;
      if (this.liquifyPoints.length === 0) {
        return;
      }
      const stroke: LiquifyStrokeDTO = {
        tool: this.liquifyTool,
        size: this.liquifySize,
        density: 50,
        pressure: this.liquifyPressure,
        rate: 80,
        points: this.liquifyPoints,
      };
      const layerId = a.id;
      const strokes: LiquifyStrokeDTO[] = [stroke];
      this.liquifyPoints = [];
      this.engine.applyLiquify(layerId, strokes).then((ok: boolean): void => {
        if (!ok) {
          this.toast('液化未生效（空层/并发编辑）');
        }
      }).catch((_e: BusinessError): void => {
        this.toast('液化失败');
      });
    }
  }

  // M20c 操控变形手势：点击=加钉，命中钉（16vp 内）=拖移其 dst
  private puppetTouch(e: TouchEvent, docX: number, docY: number): void {
    if (e.type === TouchType.Down) {
      const k = 16 * this.pxPerVp() / this.zoom;
      for (let i = this.puppetPins.length - 1; i >= 0; --i) {
        const p = this.puppetPins[i];
        if (p !== undefined && Math.hypot(docX - p.dx, docY - p.dy) <= k) {
          this.puppetDragIdx = i;
          return;
        }
      }
      const pin: PuppetPinDTO = { sx: docX, sy: docY, dx: docX, dy: docY };
      this.puppetPins.push(pin);
      this.puppetDragIdx = this.puppetPins.length - 1;
      return;
    }
    if (e.type === TouchType.Move && this.puppetDragIdx >= 0) {
      const p = this.puppetPins[this.puppetDragIdx];
      if (p !== undefined) {
        p.dx = docX;
        p.dy = docY;
      }
      return;
    }
    if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.puppetDragIdx >= 0) {
      this.puppetDragIdx = -1;
    }
  }

  // M20c 应用操控变形（钉集一步事务；成功后清钉）
  private puppetApply(): void {
    const a = this.activeLayer();
    if (a === undefined || a.isText) {
      this.toast('无栅格图层');
      return;
    }
    if (this.puppetPins.length < 2) {
      this.toast('至少两枚钉（一钉无变形）');
      return;
    }
    const layerId = a.id;
    const pins = this.puppetPins;
    this.engine.applyPuppet(layerId, pins, this.puppetMode, this.puppetDensity)
      .then((ok: boolean): void => {
        if (ok) {
          this.puppetPins = [];
          this.log(`操控变形已应用（${pins.length} 钉）`);
        } else {
          this.toast('操控未生效（跨度超限/无变化）');
        }
      })
      .catch((_e: BusinessError): void => {
        this.toast('操控变形失败');
      });
  }

  // M20d 消失点手势
  private vanishingTouch(e: TouchEvent, docX: number, docY: number): void {
    if (e.type !== TouchType.Down) {
      return;
    }
    if (this.vpQuad.length < 8) {
      this.vpQuad.push(docX, docY);
      if (this.vpQuad.length === 8) {
        this.log('消失点：平面已定形 → 取源 → 点按克隆');
      }
      return;
    }
    if (this.altHeld || this.vpPickingSource) {
      this.vpSource = [docX, docY];
      this.vpSourceSet = true;
      this.vpPickingSource = false;
      this.log(`消失点克隆源已设 (${docX.toFixed(0)},${docY.toFixed(0)})`);
      return;
    }
    if (!this.vpSourceSet) {
      this.toast('先设克隆源');
      return;
    }
    const a = this.activeLayer();
    if (a === undefined || a.isText) {
      return;
    }
    const layerId = a.id;
    const quad = this.vpQuad.slice();
    const sx = this.vpSource[0];
    const sy = this.vpSource[1];
    const pts: number[][] = [[docX, docY]];
    this.engine.applyVpClone(layerId, quad, sx, sy, pts, this.vpSize, 0.8, 1.0)
      .then((ok: boolean): void => {
        if (!ok) {
          this.toast('克隆未生效（平面/源出界）');
        }
      })
      .catch((_e: BusinessError): void => {
        this.toast('透视克隆失败');
      });
  }

  // M20d：平面采集中已落角点序号
  private vpPendingIdx(): number[] {
    const idx: number[] = [];
    for (let i = 0; i < this.vpQuad.length / 2; ++i) {
      idx.push(i);
    }
    return idx;
  }

  private vanishingReset(): void {
    this.vpQuad = [];
    this.vpSource = [];
    this.vpSourceSet = false;
    this.vpPickingSource = false;
    this.log('消失点：平面与源已重置');
  }

  // M20d 自适应广角应用（v1 无约束线；scale≠100 或模型弧形时非恒等）
  private wideAngleApply(): void {
    const a = this.activeLayer();
    if (a === undefined || a.isText) {
      return;
    }
    this.showWideAngle = false;
    const layerId = a.id;
    this.docOp((): Promise<boolean> =>
      this.engine.applyWideAngle(layerId, this.waModel, this.waFocal, this.waCrop, this.waScale, []),
      `自适应广角 (${this.waModel === 1 ? '鱼眼' : this.waModel === 3 ? '全球形' : '透视'} ${this.waFocal}mm ×${Math.round(this.waCrop * 10) / 10} ${this.waScale}%)`);
  }

  private waModelLabel(): string {
    if (this.waModel === 1) { return '鱼眼'; }
    if (this.waModel === 3) { return '全球形'; }
    return '透视';
  }

  // M20d 透视手势：命中角（20vp 内）=拖角；拖动中实时更新 dst
  private perspectiveTouch(e: TouchEvent, docX: number, docY: number): void {
    if (this.perspectiveQuad.length !== 8) {
      return;
    }
    if (e.type === TouchType.Down) {
      const k = 20 * this.pxPerVp() / this.zoom;
      for (let i = 0; i < 4; ++i) {
        const px = this.perspectiveQuad[8 + i * 2];
        const py = this.perspectiveQuad[8 + i * 2 + 1];
        if (Math.hypot(docX - px, docY - py) <= k) {
          this.perspectiveDragIdx = i;
          return;
        }
      }
      return;
    }
    if (e.type === TouchType.Move && this.perspectiveDragIdx >= 0) {
      this.perspectiveQuad[8 + this.perspectiveDragIdx * 2] = docX;
      this.perspectiveQuad[8 + this.perspectiveDragIdx * 2 + 1] = docY;
      return;
    }
    if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.perspectiveDragIdx >= 0) {
      this.perspectiveDragIdx = -1;
    }
  }

  // M20d 拉直：近水平 dst 边 y 取均值 / 近垂直 x 取均值（45° 判别；单平面无链接组）
  private perspectiveStraighten(): void {
    const q = this.perspectiveQuad;
    if (q.length !== 8) {
      return;
    }
    for (let k = 0; k < 4; ++k) {
      const a = 8 + k * 2;
      const b = 8 + ((k + 1) % 4) * 2;
      const dx = Math.abs(q[b] - q[a]);
      const dy = Math.abs(q[b + 1] - q[a + 1]);
      if (dy <= dx) {
        const y = (q[a + 1] + q[b + 1]) / 2;
        q[a + 1] = y;
        q[b + 1] = y;
      } else {
        const x = (q[a] + q[b]) / 2;
        q[a] = x;
        q[b] = x;
      }
    }
    this.log('透视：边已拉直（近水平取平/近垂直取直）');
  }

  private perspectiveReset(): void {
    if (this.perspectiveQuad.length === 8) {
      for (let i = 0; i < 8; ++i) {
        this.perspectiveQuad[8 + i] = this.perspectiveQuad[i];
      }
      this.log('透视：dst 已重置为 src');
    }
  }

  private perspectiveEnter(): void {
    const a = this.activeLayer();
    if (a === undefined || a.isText) {
      this.toast('无栅格图层');
      return;
    }
    const b = this.engine.getLayerBounds(a.id);
    let x0 = 0;
    let y0 = 0;
    let x1 = this.docW;
    let y1 = this.docH;
    if (b !== null && b.ok && b.w > 0 && b.h > 0) {
      x0 = b.x;
      y0 = b.y;
      x1 = b.x + b.w;
      y1 = b.y + b.h;
    }
    this.perspectiveQuad = [x0, y0, x1, y0, x1, y1, x0, y1, x0, y0, x1, y0, x1, y1, x0, y1];
    this.perspectiveDragIdx = -1;
    this.perspectiveSrcBackup.length = 0;
    for (let i = 0; i < 8; ++i) {
      this.perspectiveSrcBackup.push(this.perspectiveQuad[i]);
    }
  }

  private perspectiveApply(): void {
    const a = this.activeLayer();
    if (a === undefined || a.isText) {
      return;
    }
    if (this.perspectiveQuad.length !== 8) {
      return;
    }
    // 恒等跳过
    let moved = false;
    for (let i = 0; i < 8; ++i) {
      if (Math.abs(this.perspectiveQuad[8 + i] - this.perspectiveQuad[i]) > 0.5) {
        moved = true;
        break;
      }
    }
    if (!moved) {
      this.toast('未拖动角点');
      return;
    }
    const layerId = a.id;
    const quad = this.perspectiveQuad.slice();
    this.engine.applyPerspective(layerId, quad).then((ok: boolean): void => {
      if (ok) {
        this.perspectiveQuad = [];
        this.log('透视变形已应用');
      } else {
        this.toast('透视未生效（退化四边形/无变化）');
      }
    }).catch((_e: BusinessError): void => {
      this.toast('透视变形失败');
    });
  }

  // M20a 重采样方法名（对话框下拉与历史标签共用）
  private resampleLabel(m: number): string {
    if (m === 0) { return '最近邻'; }
    if (m === 1) { return '双线性'; }
    if (m === 3) { return '两次立方（较平滑）'; }
    if (m === 4) { return '两次立方（较锐利）'; }
    if (m === 5) { return '两次立方（自动）'; }
    if (m === 6) { return 'Lanczos'; }
    if (m === 7) { return '保留细节（扩大）'; }
    return '两次立方';
  }

  private commitImageSize(): void {
    const w = parseInt(this.imgWText);
    const h = parseInt(this.imgHText);
    if (isNaN(w) || isNaN(h) || w < 1 || h < 1 || w > 16384 || h > 16384) {
      this.toast('尺寸无效（1-16384）');
      return;
    }
    this.showImageSize = false;
    if (this.imageSizeMode === 1) {
      // M19c 内容感知比例（活动图层缝雕刻；DocGeom 异步）
      this.docOp((): Promise<boolean> => this.engine.contentAwareScale(w, h),
        `内容感知比例 ${w}×${h}`);
    } else {
      const methodName: string = this.resampleLabel(this.imgResampleMethod);
      this.docOp((): Promise<boolean> => this.engine.resampleDocument(w, h, this.imgResampleMethod),
        `图像大小 ${w}×${h} · ${methodName}`);
    }
  }

  // M10c 通道数镜像刷新（存储/删除后调用；驱动载入选区菜单使能）
  private refreshChannelCount(): void {
    this.engine.getChannelList().then((list: string[]): void => {
      this.channelsCount = list.length;
    }).catch((_e: BusinessError): void => {
      this.channelsCount = 0;
    });
  }

  // M10c 剪贴板：拷贝/剪切（merged=合并拷贝）
  private runClipboard(merged: boolean, cut: boolean): void {
    if (!this.docOpen || !this.hasSelectionFlag) {
      this.toast('需要先建立选区');
      return;
    }
    const ok = cut ? this.engine.cutSelection(merged) : this.engine.copySelection(merged);
    this.log(`${cut ? '剪切' : merged ? '合并拷贝' : '拷贝'}: ${ok}`);
  }

  private commitStoreSel(): void {
    const name = this.storeSelText.trim();
    if (name.length === 0) {
      this.toast('请输入通道名');
      return;
    }
    this.showStoreSel = false;
    const ok = this.engine.storeSelection(name);
    this.log(`存储选区 "${name}": ${ok}`);
    if (ok) {
      this.refreshChannelCount();
    }
  }

  private commitLoadSel(): void {
    const name = this.loadSelText.trim();
    if (name.length === 0) {
      this.toast('请输入通道名');
      return;
    }
    this.showLoadSel = false;
    const ok = this.engine.loadSelection(name, 0);
    this.hasSelectionFlag = ok;
    this.log(`载入选区 "${name}": ${ok}`);
  }

  // M10b 羽化/修改族 + M11a 描边提交（半径/宽度数值对话框共用）
  private commitModify(): void {
    const r = parseInt(this.modifyText);
    const kindLabel: Record<string, string> = {
      'feather': '羽化',
      'border': '边界',
      'expand': '扩展',
      'contract': '收缩',
      'stroke': '描边',
    };
    if (isNaN(r) || r < 1 || r > 100) {
      this.toast('数值无效（1-100）');
      return;
    }
    this.showModify = false;
    const kind = this.modifyKind;
    let ok = false;
    if (kind === 'feather') {
      ok = this.engine.featherSelection(r);
    } else if (kind === 'border') {
      ok = this.engine.borderSelection(r);
    } else if (kind === 'expand') {
      ok = this.engine.expandSelection(r);
    } else if (kind === 'contract') {
      ok = this.engine.contractSelection(r);
    } else if (kind === 'stroke') {
      ok = this.engine.strokeSelection(r);
    }
    this.log(`${kindLabel[kind] ?? '修改'} r=${r}: ${ok}`);
    if (!ok) {
      this.toast('操作失败');
    }
  }

  // M11a 填充提交（色源 = 引擎 fg/bg 现值或黑白）
  private commitFill(): void {
    this.showFill = false;
    const ok = this.engine.fillSelection(this.fillSource);
    const names: string[] = ['前景色', '背景色', '黑色', '白色'];
    this.log(`填充（${names[this.fillSource]}）: ${ok}`);
  }

  private commitRotateAny(): void {
    const deg = parseFloat(this.angleText);
    if (isNaN(deg)) {
      this.toast('角度无效');
      return;
    }
    this.showRotateAny = false;
    this.docOp((): Promise<boolean> => this.engine.rotateDocumentArbitrary(deg), `旋转 ${deg} 度`);
  }
  private commitTransform(): void {
    const a = this.activeLayer();
    if (a === undefined || !this.docOpen) {
      return;
    }
    this.showTransform = false;
    const scale = this.tfScale / 100;
    const id = a.id;
    this.docOp((): boolean => this.engine.bakeLayerTransform(id, scale, scale,
      this.tfRotate, this.tfSkew, this.docW / 2, this.docH / 2),
      `自由变换 ${Math.round(this.tfScale)}%/${this.tfRotate}°`);
  }
  private enterTransformOverlay(): void {
    if (!this.docOpen || this.activeLayer() === undefined) {
      return;
    }
    const box = this.engine.beginTransformOverlay();
    if (box === null) {
      this.toast('活动图层无内容');
      return;
    }
    this.overlayBox = box;
    this.overlayMat = [1, 0, 0, 0, 1, 0];
    this.maskTarget = false;
    this.log(`变换 overlay 进入 (${box.left},${box.top} ${box.width}x${box.height})`);
  }

  private async commitTransformOverlay(): Promise<void> {
    const a = this.activeLayer();
    const box = this.overlayBox;
    if (a === undefined || box === null) {
      return;
    }
    const id = a.id;
    const mat = this.overlayMat;
    this.overlayBox = null;  // 先撤 overlay（烘焙异步进行，渲染即回未变换态）
    const changed = await this.engine.bakeLayerMatrix(id, mat);
    if (changed) {
      this.hasSelectionFlag = false;
      this.refreshDocMeta();
      this.toast('变换已应用');
      this.log('自由变换烘焙完成');
    }
  }

  private cancelTransformOverlayUi(): void {
    this.engine.cancelTransformOverlay();
    this.overlayBox = null;
  }
  private composeOverlayMat(tx: number, ty: number, sx: number, sy: number): number[] {
    const box = this.overlayBox;
    if (box === null) {
      return [1, 0, 0, 0, 1, 0];
    }
    const cx = box.left + box.width / 2;
    const cy = box.top + box.height / 2;
    // M = T(tx,ty) - T(C) - S - T(-C)（围绕框中心缩放 + 平移）
    const m0 = sx;
    const m1 = 0;
    const m2 = tx + cx - sx * cx;
    const m3 = 0;
    const m4 = sy;
    const m5 = ty + cy - sy * cy;
    return [m0, m1, m2, m3, m4, m5];
  }
  private scheduleHistogram(): void {
    // M15b：直方图面板可见时任意文档都拉取；否则仅调整层（曲线编辑器背景）
    if (!this.docOpen || (this.activeLayer()?.isAdjustment !== true && !this.histogramVisible)) {
      return;
    }
    if (this.histTimer >= 0) {
      clearTimeout(this.histTimer);
    }
    this.histTimer = setTimeout((): void => {
      this.histTimer = -1;
      this.engine.getHistogram().then((h): void => {
        this.histLuma = h.luma;
      }).catch((_e: BusinessError): void => {
        // 直方图失败静默（下次 docVersion 再拉）
      });
    }, 300);
  }
  private applyTheme(mode: ThemeMode): void {
    this.themeMode = mode;
    this.prefs.setThemeMode(mode);
    const hc = this.hostCtx();
    if (hc !== undefined) {
      this.prefs.applyTheme(mode, hc);
    }
    this.showTheme = false;
  }

  /** M8a 文档级操作统一包装：执行 + 刷文档尺寸 + 清选区镜像 + 提示。 */
  private docBusy: boolean = false;

  // 文档级几何命令（旋转/翻转/裁切/显示全部/图像大小/任意角）已 async 化（审查修复 D2：
  // 大图重活走 worker 防主线程 appfreeze）。busy 防重入：上一项未发布前拒绝新操作。
  private async docOp(op: () => boolean | Promise<boolean>, label: string): Promise<void> {
    if (!this.docOpen) {
      return;
    }
    if (this.docBusy) {
      this.toast('上一项文档操作进行中…');
      return;
    }
    this.docBusy = true;
    this.busyText = `${label}…`;
    try {
      const changed = await op();
      if (changed) {
        this.hasSelectionFlag = false;
        this.cropRect = null;
        this.refreshDocMeta();
        this.toast(label + '完成');
        this.log(`${label} 完成`);
      }
    } finally {
      this.docBusy = false;
      this.busyText = '';
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
      leafItem('file.placeEmbedded', '置入嵌入的智能对象…', '', d),
      leafItem('file.close', '关闭文档', '', d),
    ];
    const edit: MenuItemVM[] = [
      leafItem('edit.undo', '撤销', 'Ctrl+Z', this.canUndo),
      leafItem('edit.redo', '重做', 'Ctrl+Shift+Z', this.canRedo),
      leafItem('edit.cut', '剪切', 'Ctrl+X', d && this.hasSelectionFlag),
      leafItem('edit.copy', '拷贝', 'Ctrl+C', d && this.hasSelectionFlag),
      leafItem('edit.copyMerged', '合并拷贝', 'Shift+Ctrl+C', d && this.hasSelectionFlag),
      leafItem('edit.paste', '粘贴', 'Ctrl+V', d),
      leafItem('edit.clear', '清除', '', d && this.hasSelectionFlag),
      leafItem('edit.fill', '填充…', 'Shift+F5', d),
      leafItem('edit.contentAwareFill', '内容识别填充', '', d && this.hasSelectionFlag),
      leafItem('edit.stroke', '描边…', '', d && this.hasSelectionFlag),
      leafItem('edit.definePattern', '定义图案', '', d && this.hasSelectionFlag),
      // M22 颜色查找表导入（.cube/.3dl/.look；模拟器 picker 不可注入留真机）
      leafItem('edit.importLookup', '载入查找表…', '', d),
      leafItem('edit.defineCustomShape', '定义自定形状', '',
        d && this.workPathNonEmpty === 1),
      subItem('edit.placeCustomSub', '放置自定形状', d && this.hasCustomShapes,
        this.engine.getCustomShapes().map((id: string): MenuItemVM =>
          leafItem(`edit.placeCustom:${id}`, id, '', d))),
    ];
    const imageMenu: MenuItemVM[] = [
      leafItem('image.canvasSize', '画布大小…', '', d),
      leafItem('image.imageSize', '图像大小…', '', d),
      leafItem('image.contentAwareScale', '内容感知比例…', '', d),
      leafItem('image.rotate90cw', '顺时针旋转 90°', '', d),
      leafItem('image.rotate90ccw', '逆时针旋转 90°', '', d),
      leafItem('image.rotate180', '旋转 180°', '', d),
      leafItem('image.rotateAny', '任意角度旋转…', '', d),
      leafItem('image.flipH', '水平翻转画布', '', d),
      leafItem('image.flipV', '垂直翻转画布', '', d),
      leafItem('image.trim', '裁切（基于像素）', '', d),
      leafItem('image.revealAll', '显示全部', '', d),
      // M15e：RAW 显影参数（会话内导入过 DNG 才可用，P1）
      leafItem('image.develop', 'RAW 显影参数…', '', d && this.hasRawDevelop),
      // M24b：自动（批处理 / 图像处理器）
      subItem('image.automateSub', '自动', true, [
        leafItem('automate.batch', '批处理…', '', true),
        leafItem('automate.processor', '图像处理器…', '', true),
      ]),
    ];
    const layer: MenuItemVM[] = [
      leafItem('layer.transform', '自由变换', 'Ctrl+T', d),
      leafItem('layer.transformNumeric', '数值变换…', '', d),
      leafItem('layer.new', '新建图层', '', d),
      leafItem('layer.newGroup', '新建组…', '', d),
      leafItem('layer.group', '图层编组', 'Ctrl+G', d && this.activeLayer()?.isGroup === false),
      leafItem('layer.ungroup', '取消编组', '', d && this.activeLayer()?.isGroup === true),
      leafItem('layer.duplicate', '复制图层', 'Ctrl+J', this.activeLayer() !== undefined),
      leafItem('layer.mergeDown', '合并向下', 'Ctrl+E', d && this.activeLayer()?.isGroup === false),
      leafItem('layer.mergeVisible', '合并可见', 'Shift+Ctrl+E', d),
      leafItem('layer.stamp', '盖印', 'Shift+Ctrl+Alt+E', d),
      leafItem('layer.flatten', '拼合图像', '', d),
      leafItem('layer.style', '图层样式…', '', d && this.activeLayer()?.isGroup === false),
      leafItem('layer.copyStyle', '拷贝图层样式', '',
        d && this.activeLayer()?.hasStyle === true),
      leafItem('layer.pasteStyle', '粘贴图层样式', '',
        d && this.activeLayer()?.isGroup === false && this.hasStyleClipboard),
      leafItem('layer.toggleEffects', '显示/隐藏全部效果', '',
        d && this.activeLayer()?.hasStyle === true,
        this.activeLayer()?.hasStyle === true && this.activeLayer()?.styleOn === false),
      leafItem('layer.globalLight', '全局光角度…', '', d),
      leafItem('layer.vectorMaskFromPath', '矢量蒙版（自工作路径）', '',
        d && this.workPathNonEmpty === 1),
      leafItem('layer.vectorMaskToggle', '启用/停用矢量蒙版', '',
        d && this.activeLayer()?.hasVectorMask === true,
        this.activeLayer()?.hasVectorMask === true && this.activeLayer()?.vectorMaskOn === false),
      leafItem('layer.vectorMaskRemove', '删除矢量蒙版', '',
        d && this.activeLayer()?.hasVectorMask === true),
      leafItem('layer.rasterizeShape', '栅格化形状', '',
        d && this.activeLayer()?.isShape === true),
      // M21 智能对象组
      leafItem('layer.convertSmart', '转换为智能对象', '',
        d && this.activeLayer() !== undefined && this.activeLayer()?.isGroup === false &&
          this.activeLayer()?.isSmart === false &&
          this.activeLayer()?.isAdjustment === false),
      leafItem('layer.smartFiltersDialog', '智能滤镜…', '',
        d && this.activeLayer()?.isSmart === true &&
          (this.activeLayer()?.smartFilters ?? 0) > 0),
      leafItem('layer.smartViaCopy', '通过拷贝新建智能对象', '',
        d && this.activeLayer()?.isSmart === true),
      leafItem('layer.smartReplace', '替换内容…', '', d && this.activeLayer()?.isSmart === true),
      leafItem('layer.smartExport', '导出内容…', '', d && this.activeLayer()?.isSmart === true),
      leafItem('layer.smartFiltersToggle', '禁用/启用智能滤镜', '',
        d && this.activeLayer()?.isSmart === true && (this.activeLayer()?.smartFilters ?? 0) > 0,
        this.activeLayer()?.isSmart === true && this.activeLayer()?.smartFiltersOn === false),
      leafItem('layer.smartMaskClear', '清除滤镜蒙版', '',
        d && this.activeLayer()?.isSmart === true &&
          this.activeLayer()?.smartHasMask === true),
      leafItem('layer.smartRasterize', '栅格化智能对象', '',
        d && this.activeLayer()?.isSmart === true),
      leafItem('layer.fillSolid', '新建纯色填充图层', '', d),
      leafItem('layer.fillGrad', '新建渐变填充图层', '', d),
      leafItem('layer.linkToggle', '链接图层 / 解除链接', '',
        d && (this.alignTargetIds().length >= 2 || this.layers.some((l: LayerDTO): boolean =>
          l.isActive && l.linkId !== 0))),
      subItem('layer.alignSub', '对齐', d && this.alignTargetIds().length >= 2, [
        leafItem('layer.alignLeft', '左边', '', d),
        leafItem('layer.alignHCenter', '水平居中', '', d),
        leafItem('layer.alignRight', '右边', '', d),
        leafItem('layer.alignTop', '顶边', '', d),
        leafItem('layer.alignVCenter', '垂直居中', '', d),
        leafItem('layer.alignBottom', '底边', '', d),
      ]),
      subItem('layer.distributeSub', '分布', d && this.alignTargetIds().length >= 3, [
        leafItem('layer.distLeft', '左', '', d),
        leafItem('layer.distHCenter', '水平居中', '', d),
        leafItem('layer.distRight', '右', '', d),
        leafItem('layer.distTop', '顶', '', d),
        leafItem('layer.distVCenter', '垂直居中', '', d),
        leafItem('layer.distBottom', '底', '', d),
      ]),
      leafItem('layer.rename', '重命名…', '', this.activeLayer() !== undefined),
      leafItem('layer.delete', '删除图层', '', this.activeLayer() !== undefined),
      leafItem('layer.moveUp', '上移一层', '', d),
      leafItem('layer.moveDown', '下移一层', '', d),
      leafItem('layer.clipToggle', '创建/释放剪贴蒙版', '', d),
      leafItem('layer.maskAdd', '添加图层蒙版（显示全部）', '', d),
      leafItem('layer.maskEnableToggle', '启用/停用蒙版', '', d && this.activeLayerHasMask()),
      leafItem('layer.adjustAdd', '新建调整图层（色阶）', '', d),
      leafItem('layer.rasterizeText', '栅格化文字', '',
        d && this.activeLayer()?.isText === true),
    ];
    const selReady: boolean = d && this.hasSelectionFlag;  // M10b：选区类命令使能态
    const select: MenuItemVM[] = [
      leafItem('select.all', '全部选择', 'Ctrl+A', d),
      leafItem('select.deselect', '取消选择', 'Ctrl+D', d),
      leafItem('select.inverse', '反选', 'Ctrl+Shift+I', d),
      leafItem('select.reselect', '重新选择', 'Shift+Ctrl+D', d),
      leafItem('select.quickMask', '快速蒙版', 'Q', d, this.quickMaskActive),
      leafItem('select.feather', '羽化…', 'Shift+F6', selReady),
      leafItem('select.store', '存储选区…', '', selReady),
      leafItem('select.load', '载入选区…', '', d && this.channelsCount > 0),
      leafItem('select.border', '修改：边界…', '', selReady),
      leafItem('select.smooth', '修改：平滑', '', selReady),
      leafItem('select.expand', '修改：扩展…', '', selReady),
      leafItem('select.contract', '修改：收缩…', '', selReady),
    ];
    // M13 滤镜菜单（分类子菜单；docs 未启用项不列）
    const filterMenus: MenuItemVM[] = [];
    const groups: Record<string, MenuItemVM[]> = {};
    for (const f of FILTER_CATALOG) {
      if (groups[f.menu] === undefined) {
        groups[f.menu] = [];
      }
      groups[f.menu].push(leafItem(`filter.${f.id}`, `${f.label}…`, '', d));
    }
    const menuOf = (key: string, label: string): MenuItemVM =>
      subItem(`filterGroup.${key}`, label, d, groups[key] ?? []);
    filterMenus.push(leafItem('filter.cameraRaw', 'Camera Raw…', '', d));
    filterMenus.push(leafItem('liquify.enter', '液化（笔刷变形）…', '', d));
    filterMenus.push(leafItem('puppet.enter', '操控变形…', '', d));
    filterMenus.push(leafItem('perspective.enter', '透视变形…', '', d));
    filterMenus.push(leafItem('wideangle.enter', '自适应广角…', '', d));
    filterMenus.push(leafItem('vanishing.enter', '消失点（透视克隆）…', '', d));
    filterMenus.push(menuOf('adjust', '调整'));
    filterMenus.push(menuOf('blur', '模糊'));
    filterMenus.push(menuOf('sharpen', '锐化'));
    filterMenus.push(menuOf('noise', '杂色'));
    filterMenus.push(menuOf('pixelate', '像素化'));
    filterMenus.push(menuOf('stylize', '风格化'));
    filterMenus.push(menuOf('render', '渲染'));
    filterMenus.push(menuOf('other', '其他'));

    const view: MenuItemVM[] = [
      leafItem('view.zoomIn', '放大', 'Ctrl+=', d),
      leafItem('view.zoomOut', '缩小', 'Ctrl+-', d),
      leafItem('view.zoom100', '100%', 'Ctrl+1', d),
      leafItem('view.fit', '适配画布', 'Ctrl+0', d),
      leafItem('view.rulers', '标尺', 'Ctrl+R', true, this.rulersVisible),
      subItem('view.rulerUnitSub', '标尺单位', true, [
        leafItem('view.rulerUnitPx', '像素', '', true, this.rulerUnit === 0),
        leafItem('view.rulerUnitPct', '百分比', '', true, this.rulerUnit === 1),
      ]),
      leafItem('view.guidesVisible', '显示参考线', 'Ctrl+;', true, this.guidesMirror.visible),
      leafItem('view.guidesLocked', '锁定参考线', 'Alt+Ctrl+;', true, this.guidesMirror.locked),
      leafItem('view.guidesClear', '清除参考线', '', true),
      leafItem('view.grid', '显示网格', "Ctrl+'", true, this.gridVisible),
      subItem('view.gridSpacingSub', '网格间距', true, [
        leafItem('view.gridSpacing50', '50 px', '', true, this.gridSpacing === 50),
        leafItem('view.gridSpacing100', '100 px', '', true, this.gridSpacing === 100),
        leafItem('view.gridSpacing250', '250 px', '', true, this.gridSpacing === 250),
        leafItem('view.gridSpacing500', '500 px', '', true, this.gridSpacing === 500),
      ]),
      leafItem('view.snapGuides', '对齐参考线', '', true, this.snapGuidesOn),
      // M22 显示变换（视图级；软打样/色域警告/查找表）
      subItem('view.proofSub', '校样设置', d, [
        leafItem('view.proofOff', '关闭校样', '', true, !this.viewXformLut),
        leafItem('view.proofCoated', '合成涂布纸 CMYK', '', true, this.viewXformLut && this.viewXformDesc === 'proof: coated-cmyk'),
        leafItem('view.proofP3', 'Display P3（广色域）', '', true, this.viewXformLut && this.viewXformDesc === 'proof: display-p3'),
      ]),
      leafItem('view.gamutWarning', '色域警告（CMYK）', 'Shift+Ctrl+Y', true, this.viewXformGamut),
      subItem('view.lookupSub', '校样颜色（外观）', d, [
        leafItem('view.lookupOff', '关闭', '', true, !this.viewXformLut || this.viewXformDesc.startsWith('proof')),
        leafItem('view.lookup0', 'Teal & Orange', '', true, this.viewXformDesc === 'lookup: tealOrange'),
        leafItem('view.lookupMono', 'Mono High Contrast', '', true, this.viewXformDesc === 'lookup: monoContrast'),
      ]),
      leafItem('view.snapGrid', '对齐网格', "Shift+Ctrl+'", true, this.snapGridOn),
      subItem('view.screenModeSub', '屏幕模式', true, [
        leafItem('view.screenStd', '标准屏幕模式', 'F', true, this.screenMode === 0),
        leafItem('view.screenFullMenu', '带菜单栏的全屏模式', '', true, this.screenMode === 1),
        leafItem('view.screenFull', '全屏模式', '', true, this.screenMode === 2),
      ]),
    ];
    const win: MenuItemVM[] = [
      leafItem('window.toggleLayers', '图层面板', '', true, this.layersVisible),
      leafItem('window.toggleHistory', '历史面板', '', true, this.historyVisible),
      leafItem('window.toggleHistogram', '直方图面板', '', true, this.histogramVisible),
      leafItem('window.toggleInfo', '信息面板', '', true, this.infoVisible),
      leafItem('window.toggleActions', '动作面板', '', true, this.actionsVisible),
    ];
    const help: MenuItemVM[] = [
      subItem('help.themeSub', '主题', true, [
        leafItem('theme.system', '跟随系统', '', true, this.themeMode === 'system'),
        leafItem('theme.light', '浅色', '', true, this.themeMode === 'light'),
        leafItem('theme.dark', '深色', '', true, this.themeMode === 'dark'),
      ]),
      leafItem('help.shortcuts', '快捷键表', '', true),
      leafItem('help.about', '关于 Montage', '', true),
      // M15a：沙箱往返包调试通道（模拟器 picker 保存缺陷的替代验证路径；三键组合注入受限）
      subItem('help.debugSub', '调试（沙箱往返）', true, [
        leafItem('file.saveSandbox', '保存往返包到沙箱', '', d),
        leafItem('file.openSandbox', '打开沙箱往返包', '', d),
        leafItem('file.openSandboxDng', '打开沙箱 DNG（RAW 显影）', '', d),
        leafItem('file.openSandboxCr2', '打开沙箱 CR2（M23 厂牌 RAW）', '', d),
        leafItem('file.debugJpeg', 'JPEG 分带校准（M16）', '', true),
        leafItem('debug.control', controlServer.isRunning() ? '停止控制通道' : '启动控制通道（M24）',
          '', true),
      ]),
    ];
    return [
      { id: 'file', label: '文件', items: file },
      { id: 'edit', label: '编辑', items: edit },
      { id: 'image', label: '图像', items: imageMenu },
      { id: 'layer', label: '图层', items: layer },
      { id: 'select', label: '选择', items: select },
      { id: 'filter', label: '滤镜', items: filterMenus },
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
      { id: 'polyLasso', label: '多边形套索', symbol: $r('sys.symbol.pencil_tip'), enabled: true },
      { id: 'wand', label: '魔棒', symbol: $r('sys.symbol.wand_and_stars'), enabled: true },
      { id: 'crop', label: '裁剪', symbol: $r('sys.symbol.crop'), enabled: true },
      { id: '-', label: '', symbol: $r('sys.symbol.minus'), enabled: false },
      { id: 'eyedropper', label: '吸管', symbol: $r('sys.symbol.eyedropper'), enabled: true },
      { id: 'text', label: '文字', symbol: $r('sys.symbol.textformat'), enabled: true },
      { id: 'brush', label: '画笔', symbol: $r('sys.symbol.paintbrush_pointed'), enabled: true },
      { id: 'pencil', label: '铅笔', symbol: $r('sys.symbol.pencil_line'), enabled: true },
      { id: 'eraser', label: '橡皮', symbol: $r('sys.symbol.eraser_line'), enabled: true },
      { id: 'gradient', label: '渐变', symbol: $r('sys.symbol.rectangle_righthalf_inset_filled'), enabled: true },
      { id: 'bucket', label: '油漆桶', symbol: $r('sys.symbol.paint_bucket'), enabled: true },
      { id: 'shape', label: '形状', symbol: $r('sys.symbol.circle_on_square'), enabled: true },
      { id: 'pen', label: '钢笔', symbol: $r('sys.symbol.pen_tip'), enabled: true },
      { id: 'directSelect', label: '直接选择', symbol: $r('sys.symbol.arrow_up_left'), enabled: true },
      { id: 'stamp', label: '仿制图章', symbol: $r('sys.symbol.seal'), enabled: true },
      { id: 'blur', label: '模糊', symbol: $r('sys.symbol.hand_draw'), enabled: true },
      { id: 'dodge', label: '减淡', symbol: $r('sys.symbol.contrast'), enabled: true },
      { id: 'historyBrush', label: '历史画笔', symbol: $r('sys.symbol.clock'), enabled: true },
      { id: 'heal', label: '修复画笔', symbol: $r('sys.symbol.cross'), enabled: true },
      { id: 'spotHeal', label: '污点修复', symbol: $r('sys.symbol.cross_case'), enabled: true },
      { id: 'patch', label: '修补', symbol: $r('sys.symbol.square_dashed_and_arrow_turn_up_right'), enabled: true },
      { id: '-', label: '', symbol: $r('sys.symbol.minus'), enabled: false },
      { id: 'hand', label: '抓手', symbol: $r('sys.symbol.hand'), enabled: true },
      { id: 'zoom', label: '缩放', symbol: $r('sys.symbol.magnifyingglass'), enabled: true },
    ];
  }

  private toolLabel(id: string): string {
    // M11e 效果组内具体工具名（buildTools 只列代表按钮）
    const effectLabels: Record<string, string> = {
      'blur': '模糊', 'sharpen': '锐化', 'smudge': '涂抹',
      'dodge': '减淡', 'burn': '加深', 'sponge': '海绵',
    };
    if (effectLabels[id] !== undefined) {
      return effectLabels[id];
    }
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
    // M15a 全屏模式（1/2）：Esc 退出回标准
    if (this.screenMode > 0 && e.keyCode === KC_ESC) {
      this.runCommand('view.screenStd');
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
    if (this.showExport || this.showRename || this.showShortcuts || this.showAbout || this.showTheme || this.showTransform || this.showModify || this.showStoreSel || this.showLoadSel || this.showFill || this.showStyle || this.showFilter || this.showDevelop || this.showGlobalLight || this.showContour) {
      if (e.keyCode === KC_ESC) {
        this.showTransform = false;
        this.showExport = false;
        this.showRename = false;
        this.showShortcuts = false;
        this.showAbout = false;
        this.showTheme = false;
        this.showModify = false;
        this.showStoreSel = false;
        this.showLoadSel = false;
        if (this.showDevelop) {
          this.cancelDevelop();  // M15e：Esc=取消（不落参数）
        }
        this.showFill = false;
        this.showStyle = false;
        this.showGlobalLight = false;
        this.showContour = false;
        this.cancelFilterDialog();
      }
      return;
    }
    // M12 文本编辑会话：Esc = 提交退出（PS Ctrl+Enter 语义的触屏等价）
    if (this.editingTextId !== 0) {
      if (e.keyCode === KC_ESC) {
        this.commitTextEditing();
      }
      return;  // 编辑会话期间其余快捷键不抢（输入法优先）
    }
    // 变换 overlay：Enter 确认 / Esc 取消
    if (this.overlayBox !== null) {
      if (e.keyCode === KC_ENTER) {
        this.commitTransformOverlay();
      } else if (e.keyCode === KC_ESC) {
        this.cancelTransformOverlayUi();
      }
      return;
    }
    // 多边形套索待闭合：Esc 取消（M10a）
    if (this.polyPendingPts.length > 0 && e.keyCode === KC_ESC) {
      this.cancelPolygonSelection();
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
          if (this.altHeld) {
            this.runCommand('file.openSandbox');  // 隐藏调试：打开沙箱往返包
          } else {
            this.runCommand('file.open');
          }
          break;
        case KC_S:
          if (this.altHeld) {
            this.runCommand('file.saveSandbox');  // 隐藏调试：沙箱往返包（模拟器 save→open 验证通道）
          } else {
            this.runCommand('file.save');
          }
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
          if (this.shiftHeld) {
            // Shift+Ctrl+D 重新选择（M10a）
            const ok = this.engine.reselectSelection();
            this.hasSelectionFlag = ok;
            this.log(`重新选择: ${ok ? '已恢复' : '无可恢复选区'}`);
          } else {
            this.runCommand('select.deselect');
          }
          break;
        case KC_A:
          this.runCommand('select.all');
          break;
        case KC_G:
          this.runCommand('layer.group');  // M14a 编组（PS Ctrl+G）
          break;
        case KC_J:
          this.runCommand('layer.duplicate');  // M14b 复制图层（PS Ctrl+J）
          break;
        case KC_E:
          if (this.shiftHeld && this.altHeld) {
            this.runCommand('layer.stamp');  // Shift+Ctrl+Alt+E 盖印
          } else if (this.shiftHeld) {
            this.runCommand('layer.mergeVisible');  // Shift+Ctrl+E 合并可见
          } else {
            this.runCommand('layer.mergeDown');  // Ctrl+E 合并向下
          }
          break;
        case KC_C:
          this.runCommand('edit.copy');
          break;
        case KC_X:
          this.runCommand('edit.cut');
          break;
        case KC_V:
          this.runCommand('edit.paste');
          break;
        case KC_I:
          if (this.shiftHeld) {
            this.runCommand('select.inverse');
          }
          break;
        case KC_T_KEY:
          this.runCommand('layer.transform');
        break;
        case KC_R:
          this.runCommand('view.rulers');  // Ctrl+R 标尺（PS 语义）
          break;
        case KC_SEMICOLON:
          // Ctrl+; 显示/隐藏参考线；Alt+Ctrl+; 锁定
          this.runCommand(this.altHeld ? 'view.guidesLocked' : 'view.guidesVisible');
          break;
        case KC_APOSTROPHE:
          // Ctrl+' 显示网格；Shift+Ctrl+' 对齐网格
          this.runCommand(this.shiftHeld ? 'view.snapGrid' : 'view.grid');
          break;
        default:
          break;
      }
      return;
    }
    // 单键
    if (this.shiftHeld && e.keyCode === KC_F6) {
      this.runCommand('select.feather');  // Shift+F6 羽化（PS 语义）
      return;
    }
    if (this.shiftHeld && e.keyCode === KC_F5) {
      this.runCommand('edit.fill');  // Shift+F5 填充（PS 语义）
      return;
    }
    if (this.shiftHeld && e.keyCode === KC_B) {
      // Shift+B 循环画笔组：画笔 ↔ 铅笔（PS 工具组语义）
      this.pickTool(this.tool === 'pencil' ? 'brush' : 'pencil');
      return;
    }
    if (this.shiftHeld && e.keyCode === KC_G) {
      // Shift+G 循环填充组：渐变 ↔ 油漆桶（PS 工具组语义）
      this.pickTool(this.tool === 'bucket' ? 'gradient' : 'bucket');
      return;
    }
    if (e.keyCode === KC_U && this.shiftHeld) {
      // Shift+U 循环形状种类（矩形→圆角→椭圆→直线）
      this.shapeKind = (this.shapeKind + 1) % 6;
      this.log(`形状种类: ${this.shapeKindLabel()}`);
      return;
    }
    switch (code) {
      case KC_V:
        this.pickTool('move');
        break;
      case KC_M:
        // M 循环选框组：矩形 ↔ 椭圆（PS 工具组语义）
        if (this.tool === 'rectSelect') {
          this.selShape = this.selShape === 'rect' ? 'ellipse' : 'rect';
          this.log(`选框形状: ${this.selShape === 'ellipse' ? '椭圆' : '矩形'}`);
        } else {
          this.pickTool('rectSelect');
        }
        break;
      case KC_L:
        // L 循环套索组：自由套索 ↔ 多边形套索
        if (this.tool === 'lasso') {
          this.pickTool('polyLasso');
        } else if (this.tool === 'polyLasso') {
          this.cancelPolygonSelection();
          this.pickTool('lasso');
        } else {
          this.pickTool('lasso');
        }
        break;
      case KC_S:
        // Ctrl+S 已在 Ctrl 分支处理；单键 S = 仿制图章（PS 语义）
        this.pickTool('stamp');
        break;
      case KC_P:
        // M18：P = 钢笔（PS 惯例）
        this.pickTool('pen');
        break;
      case KC_A:
        // M18.1：A = 直接选择（PS 惯例）
        this.pickTool('directSelect');
        break;
      case KC_R:
        // R = 模糊组（Shift+R 循环 模糊→锐化→涂抹）
        if (this.shiftHeld) {
          const next: string = this.tool === 'blur' ? 'sharpen'
            : this.tool === 'sharpen' ? 'smudge' : 'blur';
          this.pickTool(next);
        } else {
          this.pickTool('blur');
        }
        break;
      case KC_Y:
        this.pickTool('historyBrush');  // M11f 历史画笔（PS Y 语义）
        break;
      case KC_J:
        // M19a：J = 修复族（Shift+J 循环 修复画笔→污点→修补；PS J 族语义）
        if (this.shiftHeld) {
          const nextJ: string = this.tool === 'heal' ? 'spotHeal'
            : this.tool === 'spotHeal' ? 'patch' : 'heal';
          this.pickTool(nextJ);
        } else {
          this.pickTool('spotHeal');
        }
        break;
      case KC_O:
        // O = 减淡组（Shift+O 循环 减淡→加深→海绵）
        if (this.shiftHeld) {
          const next: string = this.tool === 'dodge' ? 'burn'
            : this.tool === 'burn' ? 'sponge' : 'dodge';
          this.pickTool(next);
        } else {
          this.pickTool('dodge');
        }
        break;
      case KC_W:
        this.pickTool('wand');
        break;
      case KC_T_KEY:
        this.pickTool('text');  // M12 文字工具（PS T 语义）
        break;
      case KC_Q:
        this.runCommand('select.quickMask');  // M10d 快速蒙版 toggle（PS Q 语义）
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
      case KC_G:
        this.pickTool('gradient');  // M11a 渐变（PS G 语义）
        break;
      case KC_U:
        this.pickTool('shape');  // M11c 形状（PS U 语义）→ Shift+U 循环种类
        break;
      case KC_H:
        this.pickTool('hand');
        break;
      case KC_F_KEY:
        // M15a 屏幕模式循环（PS F 语义）：标准 → 带菜单栏全屏 → 全屏
        this.runCommand(this.screenMode === 0 ? 'view.screenFullMenu'
          : this.screenMode === 1 ? 'view.screenFull' : 'view.screenStd');
        break;
      case KC_Z:
        this.pickTool('zoom');
        break;
      case KC_X_KEY:
        this.applyColors(this.bgColor, this.brushColor);  // 交换前后景
        break;
      case KC_D:
        this.applyColors('#000000', '#FFFFFF');  // 重置黑白
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

  // ---------- M14b 图层深化动作 ----------

  // 面板行点击：无修饰 = 单选（清多选）；Ctrl = 加减选；Shift = 活动层到该行的范围多选
  private onLayerRowSelect(id: number, e: ClickEvent): void {
    let ctrl = false;
    let shift = false;
    if (e.getModifierKeyState !== undefined) {
      try {
        ctrl = e.getModifierKeyState(['Ctrl']);
        shift = e.getModifierKeyState(['Shift']);
      } catch (_err) {
        hilog.warn(DOMAIN, TAG, 'getModifierKeyState failed');
      }
    }
    if (ctrl) {
      const cur: number[] = this.multiSelected.slice();
      const at = cur.indexOf(id);
      if (at >= 0) {
        cur.splice(at, 1);
      } else {
        cur.push(id);
      }
      this.multiSelected = cur;
      this.engine.selectLayer(id);
      return;
    }
    if (shift) {
      const a = this.activeLayer();
      if (a === undefined) {
        this.multiSelected = [id];
        this.engine.selectLayer(id);
        return;
      }
      // 面板可见序（自顶向下）内取范围
      const rows = this.panelRows();
      const i0 = rows.findIndex((l: LayerDTO): boolean => l.id === a.id);
      const i1 = rows.findIndex((l: LayerDTO): boolean => l.id === id);
      if (i0 < 0 || i1 < 0) {
        return;
      }
      const lo = Math.min(i0, i1);
      const hi = Math.max(i0, i1);
      const sel: number[] = [];
      for (let k = lo; k <= hi; k++) {
        sel.push(rows[k].id);
      }
      this.multiSelected = sel;
      this.engine.selectLayer(id);
      return;
    }
    this.multiSelected = [];
    this.engine.selectLayer(id);
  }

  // 面板可见行序（自顶向下；与 LayersPanel.panelTree 同规则——兄弟逆序 + 组先于子树 + 折叠过滤）
  private panelRows(): LayerDTO[] {
    const kids: Map<number, LayerDTO[]> = new Map();
    const rootKids: LayerDTO[] = [];
    for (const l of this.layers) {
      if (l.parentId === 0) {
        rootKids.push(l);
      } else {
        const arr = kids.get(l.parentId);
        if (arr !== undefined) {
          arr.push(l);
        } else {
          kids.set(l.parentId, [l]);
        }
      }
    }
    kids.set(0, rootKids);
    const out: LayerDTO[] = [];
    const walk = (parent: number): void => {
      const arr = kids.get(parent);
      if (arr === undefined) {
        return;
      }
      for (let i = arr.length - 1; i >= 0; i--) {
        const c = arr[i];
        out.push(c);
        if (c.isGroup && !c.collapsed) {
          walk(c.id);
        }
      }
    };
    walk(0);
    return out;
  }

  // 多选集（含活动层）→ 命令 ids
  private alignTargetIds(): number[] {
    const ids: number[] = [];
    const a = this.activeLayer();
    if (a !== undefined) {
      ids.push(a.id);
    }
    for (const mid of this.multiSelected) {
      if (a === undefined || mid !== a.id) {
        ids.push(mid);
      }
    }
    return ids;
  }

  private onAlignPick(mode: number, isAlign: boolean): void {
    const ids = this.alignTargetIds();
    const usable = ids.filter((id: number): boolean => {
      const l = this.layers.find((x: LayerDTO): boolean => x.id === id);
      return l !== undefined && !l.isGroup && !l.isAdjustment;
    });
    if (isAlign && usable.length < 2) {
      this.toast('对齐需多选 ≥2 个图层（Ctrl/Shift+点击）');
      return;
    }
    if (!isAlign && usable.length < 3) {
      this.toast('分布需多选 ≥3 个图层');
      return;
    }
    const ok = isAlign ? this.engine.alignLayers(usable, mode)
      : this.engine.distributeLayers(usable, mode);
    this.log(`${isAlign ? '对齐' : '分布'} mode=${mode}: ${ok}`);
  }

  private onLayerLinkToggle(): void {
    const a = this.activeLayer();
    if (a === undefined) {
      return;
    }
    const ids = this.alignTargetIds();
    // 任一选中层已链接 → 解除整组；否则给全部选中层分配同一链接组
    const anyLinked = this.layers.some((l: LayerDTO): boolean =>
      ids.indexOf(l.id) >= 0 && l.linkId !== 0);
    const ok = this.engine.setLayerLink(ids, !anyLinked);
    this.toast(ok ? (anyLinked ? '已解除链接' : '已链接图层') : '链接失败');
    this.log(`linkToggle ids=[${ids.join(',')}]: ${ok}`);
  }

  private onMoveToParent(id: number, newParent: number, flatIndex: number): void {
    if (!this.engine.setLayerParent(id, newParent, flatIndex)) {
      this.toast('移动会破坏组结构，已拒绝');
    }
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
        `${this.activeLayer() !== undefined ? 1 : 0}${this.activeLayerHasMask() ? 1 : 0}` +
        `${this.hasSelectionFlag ? 1 : 0}${this.quickMaskActive ? 1 : 0}` +
        `${this.rulersVisible ? 1 : 0}${this.rulerUnit}${this.gridVisible ? 1 : 0}` +
        `${this.gridSpacing}${this.snapGuidesOn ? 1 : 0}${this.snapGridOn ? 1 : 0}` +
        `${this.screenMode}${this.guidesMirror.visible ? 1 : 0}${this.guidesMirror.locked ? 1 : 0}` +
        `${this.historyVisible ? 1 : 0}${this.histogramVisible ? 1 : 0}${this.infoVisible ? 1 : 0}` +
        `${this.actionsVisible ? 1 : 0}${this.actionsRev}${this.docOpen ? 1 : 0}` +  // M24a 动作菜单使能位
        `${this.historyCurrent}${this.historyLabels.length}` +
        `${this.hasRawDevelop ? 1 : 0}`  // M15e：RAW 显影菜单项使能随会话 RAW 翻转
        + `${this.workPathNonEmpty}${this.activeLayer()?.isShape ? 1 : 0}` +
        `${this.activeLayer()?.hasVectorMask ? 1 : 0}${this.hasCustomShapes ? 1 : 0}`)  // M18：矢量菜单使能位
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
        if (this.screenMode < 2) {
          this.menuBar()
        }
        if (this.screenMode === 0) {
        OptionsBar({
          tool: this.tool,
          toolLabel: this.toolLabel(this.tool),
          docOpen: this.docOpen,
          brushSize: $brushSize,
          brushHardness: $brushHardness,
          brushOpacity: $brushOpacity,
          wandTolerance: $wandTolerance,
          wandContiguous: $wandContiguous,
          wandAllLayers: $wandAllLayers,
          wandSampleSize: $wandSampleSize,
          brushColor: this.brushColor,
          maskTarget: this.maskTarget,
          hasMaskOnActive: this.activeLayerHasMask(),
          quickMaskActive: this.quickMaskActive,
          gradientType: $gradientType,
          gradientStyle: $gradientStyle,
          gradientTarget: $gradientTarget,
          hasMaskOnActiveForGrad: this.activeLayerHasMask(),
          shapeKind: $shapeKind,
          shapeFill: $shapeFill,
          shapeStroke: $shapeStroke,
          shapeStrokeWidth: $shapeStrokeWidth,
          shapeLiveRadius: $shapeLiveRadius,
          onShapeRadiusCommit: (v: number): void => {
            this.applyShapeRadius(v);
          },
          penPending: this.penPoints.length,
          penClosed: this.penClosed,
          dsSelectedCount: this.dsSelected.length / 2,
          onDsDelete: (): void => {
            this.dsDeleteSelected();
          },
          onPenClose: (): void => {
            if (this.penPoints.length >= 6 && !this.penClosed) {
              this.penClosed = true;
              this.pushWorkPath();
              this.log('钢笔: 路径已闭合');
            }
          },
          onPenCommit: (): void => {
            this.penCommitShape();
          },
          onPenSave: (): void => {
            this.penSavePath();
          },
          onPenCancel: (): void => {
            this.penCancel();
          },
          cloneAligned: $cloneAligned,
          cloneSourceSet: this.cloneSourceSet,
          clonePicking: this.clonePicking,
          historyBrushSteps: $historyBrushSteps,
          spotHealMode: this.spotHealMode,
          liquifyTool: this.liquifyTool,
          onLiquifyTool: (t: number): void => {
            this.liquifyTool = t;
          },
          liquifySize: $liquifySize,
          liquifyPressure: $liquifyPressure,
          puppetMode: this.puppetMode,
          onPuppetMode: (m: number): void => {
            this.puppetMode = m;
          },
          puppetDensity: this.puppetDensity,
          onPuppetDensity: (d2: number): void => {
            this.puppetDensity = d2;
          },
          vpSourceSet: this.vpSourceSet,
          onVpPickSource: (): void => {
            this.vpPickingSource = !this.vpPickingSource;
            this.log(`取源模式: ${this.vpPickingSource ? '开' : '关'}`);
          },
          onVpReset: (): void => {
            this.vanishingReset();
          },
          vpSize: $vpSize,
          puppetPinCount: this.puppetPins.length,
          onPerspectiveStraighten: (): void => {
            this.perspectiveStraighten();
          },
          onPerspectiveReset: (): void => {
            this.perspectiveReset();
          },
          onPerspectiveApply: (): void => {
            this.perspectiveApply();
          },
          onPuppetApply: (): void => {
            this.puppetApply();
          },
          onPuppetCancel: (): void => {
            this.puppetPins = [];
            this.log('操控变形：钉已清空');
          },
          onSpotHealMode: (m: number): void => {
            this.spotHealMode = m;
            if (this.tool === 'spotHeal') {
              this.engine.setBrushHeal(2 + m);  // 活动工具即热切换引擎模式
            }
            this.log(`污点修复: ${m === 0 ? '内容识别' : m === 1 ? '创建纹理' : '接近匹配'}`);
          },
          onCloneAligned: (on: boolean): void => {
            this.engine.setCloneAligned(on);
            this.log(`克隆对齐: ${on ? '开' : '关'}`);
          },
          onClonePickToggle: (): void => {
            this.clonePicking = !this.clonePicking;
            this.log(`取源模式: ${this.clonePicking ? '开（点击画布取源）' : '关'}`);
          },
          cropPending: this.cropRect !== null,
          selOp: $selOp,
          selShape: this.selShape,
          onSelOpPick: (op: number): void => {
            this.selOp = op;
          },
          onSelShapeToggle: (): void => {
            this.selShape = this.selShape === 'rect' ? 'ellipse' : 'rect';
            this.log(`选框形状: ${this.selShape === 'ellipse' ? '椭圆' : '矩形'}`);
          },
          onToggleMaskTarget: (): void => this.onToggleMaskTarget(),
          onColorPick: (hex: string): void => {
            this.applyColors(hex, this.bgColor);
          },
          onZoom: (cmd: string): void => this.runCommand(`view.${cmd === 'fit' ? 'fit' : cmd === '100' ? 'zoom100' : cmd === 'in' ? 'zoomIn' : 'zoomOut'}`),
          onCropCommand: (cmd: string): void => this.runCommand(`crop.${cmd}`),
          multiSelectCount: this.alignTargetIds().length,
          onAlignPick: (mode: number, isAlign: boolean): void => this.onAlignPick(mode, isAlign),
          caMoveMode: this.caMoveMode,
          onCaMoveMode: (m: number): void => {
            this.caMoveMode = m;
            this.log(`移动模式: ${m === 0 ? '普通' : m === 1 ? '感知移动' : '感知扩展'}`);
          },
        })
        }
        Row() {
          if (this.screenMode === 0) {
          Toolbox({ tools: this.buildTools(),
            activeTool: this.tool === 'sharpen' || this.tool === 'smudge' ? 'blur'
              : this.tool === 'burn' || this.tool === 'sponge' ? 'dodge' : this.tool,
            onPick: (id: string): void => this.pickTool(id) })
          }

          // 画布区（M15a：标尺条 + 网格/参考线 overlay + 画布 Stack）
          Column() {
            if (this.rulersVisible && this.docOpen) {
              Row() {
                Column()
                  .width(RULER_VP)
                  .height('100%')
                  .backgroundColor(RULER_BG)
                RulerStrip({
                  horizontal: true,
                  zoom: this.zoom,
                  panX: this.panX,
                  panY: this.panY,
                  pxPerVp: this.pxPerVp(),
                  docW: this.docW,
                  docH: this.docH,
                  unit: this.rulerUnit,
                  previewPos: this.guidePreview.length === 2 && this.guidePreview[0] === 0
                    ? this.guidePreview[1] : -1,
                })
                  .layoutWeight(1)
                  .height('100%')
                  .onTouch((e: TouchEvent): void => this.onRulerTouch(e, true))
              }
              .width('100%')
              .height(RULER_VP)
            }
            Row() {
              if (this.rulersVisible && this.docOpen) {
                RulerStrip({
                  horizontal: false,
                  zoom: this.zoom,
                  panX: this.panX,
                  panY: this.panY,
                  pxPerVp: this.pxPerVp(),
                  docW: this.docW,
                  docH: this.docH,
                  unit: this.rulerUnit,
                  previewPos: this.guidePreview.length === 2 && this.guidePreview[0] === 1
                    ? this.guidePreview[1] : -1,
                })
                  .width(RULER_VP)
                  .height('100%')
                  .onTouch((e: TouchEvent): void => this.onRulerTouch(e, false))
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
              .onMouse((e: MouseEvent): void => {
                // M15b 信息面板：鼠标悬停读数（A-4；节流在 updatePointerInfo 内）
                if (e.action === MouseAction.Move && this.docOpen) {
                  const k = this.pxPerVp();
                  this.updatePointerInfo(this.panX + (e.x * k) / this.zoom,
                    this.panY + (e.y * k) / this.zoom, false);
                }
              })
              .onTouch((e: TouchEvent): void => {
                this.onTouchEvent(e);
              })
            // M15a 网格 + 参考线覆盖层（纯视觉；组件内已设 hitTestBehavior=None）
            GridGuidesOverlay({
              zoom: this.zoom,
              panX: this.panX,
              panY: this.panY,
              pxPerVp: this.pxPerVp(),
              docW: this.docW,
              docH: this.docH,
              gridOn: this.gridVisible,
              gridSpacing: this.gridSpacing,
              guidesOn: this.guidesMirror.visible,
              guidesH: this.guidesMirror.h,
              guidesV: this.guidesMirror.v,
              preview: this.guidePreview,
            })
            // M8.1 自由变换手柄 overlay
            if (this.overlayBox !== null) {
              TransformOverlay({
                ovLeft: this.overlayBox.left,
                ovTop: this.overlayBox.top,
                ovWidth: this.overlayBox.width,
                ovHeight: this.overlayBox.height,
                zoom: this.zoom,
                panX: this.panX,
                panY: this.panY,
                pxPerVp: this.pxPerVp(),
                onMatrix: (tx: number, ty: number, sx: number, sy: number): void => {
                  this.overlayMat = this.composeOverlayMat(tx, ty, sx, sy);
                  this.engine.setTransformPreview(true, this.overlayMat);  // GL 实时预览
                }
              })
            }
                        // 选框拖拽预览（矩形方框 / 椭圆圆角框；GL 蚂蚁线在提交后接管）
            if (this.rectDrag) {
              Column()
                .position({
                  x: (Math.min(this.rectX0, this.rectX1) - this.panX) * this.zoom / this.pxPerVp(),
                  y: (Math.min(this.rectY0, this.rectY1) - this.panY) * this.zoom / this.pxPerVp()
                })
                .width(Math.abs(this.rectX1 - this.rectX0) * this.zoom / this.pxPerVp())
                .height(Math.abs(this.rectY1 - this.rectY0) * this.zoom / this.pxPerVp())
                .border({ width: 1, color: '#FFFFFF' })
                .borderRadius(this.selShape === 'ellipse' ? '50%' : 0)
                .backgroundColor('#22000000')
            }
            // M10a 套索拖拽 / 多边形套索已落点折线预览（文档域 → 视口）。
            // hitTestBehavior=None：纯视觉 overlay，防全屏 Polyline 拦截画布触摸（模拟器实证）。
            if (this.lassoPreview.length >= 4) {
              Polyline()
                .width('100%')
                .height('100%')
                .points(this.lassoViewportPts())
                .stroke('#FFFFFF')
                .strokeWidth(1)
                .fillOpacity(0)
                .hitTestBehavior(HitTestMode.None)
            }
            if (this.polyPendingPts.length >= 2) {
              Circle()
                .width(8)
                .height(8)
                .fill('#00E5FF')
                .position({
                  x: (this.polyPendingPts[0] - this.panX) * this.zoom / this.pxPerVp() - 4,
                  y: (this.polyPendingPts[1] - this.panY) * this.zoom / this.pxPerVp() - 4
                })
                .hitTestBehavior(HitTestMode.None)
            }
            // M11a 渐变拖线预览：白线（视口两点）+ 两端手柄（纯视觉，None 防拦截触摸）
            if (this.gradientLine !== null) {
              Polyline()
                .width('100%')
                .height('100%')
                .points(this.gradientViewportPts())
                .stroke('#FFFFFF')
                .strokeWidth(1)
                .fillOpacity(0)
                .hitTestBehavior(HitTestMode.None)
              ForEach(this.gradientViewportHandles(), (p: number[]) => {
                Circle()
                  .width(10)
                  .height(10)
                  .fill('#00E5FF')
                  .position({ x: p[0] - 5, y: p[1] - 5 })
                  .hitTestBehavior(HitTestMode.None)
              }, (p: number[], idx: number): string => `${idx}|${p[0]}|${p[1]}`)
            }
            // M20c 操控变形钉预览：src→dst 细线 + 青dst圆点 + 黄src小点（纯视觉）
            if (this.tool === 'puppet' && this.puppetPins.length > 0) {
              ForEach(this.puppetPins, (p: PuppetPinDTO, idx: number): void => {
                Line()
                  .width('100%').height('100%')
                  .startPoint([this.pinVpX(p.sx), this.pinVpY(p.sy)])
                  .endPoint([this.pinVpX(p.dx), this.pinVpY(p.dy)])
                  .stroke('#88FFFFFF').strokeWidth(1)
                  .hitTestBehavior(HitTestMode.None)
                  .opacity(idx === this.puppetDragIdx ? 0 : 1)
                Circle()
                  .width(11).height(11).fill(idx === this.puppetDragIdx ? '#FFD54F' : '#00E5FF')
                  .position({ x: this.pinVpX(p.dx) - 5, y: this.pinVpY(p.dy) - 5 })
                  .hitTestBehavior(HitTestMode.None)
              }, (p: PuppetPinDTO, idx: number): string => `pp|${idx}|${p.dx}|${p.dy}`)
            }
            // M20d 透视四角预览：dst 闭合折线 + 白角点（拖拽中高亮）
            if (this.tool === 'perspective' && this.perspectiveQuad.length === 8) {
              Polyline()
                .width('100%').height('100%')
                .points(this.perspectiveViewportPts())
                .stroke('#FFFFFF').strokeWidth(1)
                .fillOpacity(0)
                .hitTestBehavior(HitTestMode.None)
              ForEach([0, 1, 2, 3], (i: number): void => {
                Circle()
                  .width(12).height(12)
                  .fill(i === this.perspectiveDragIdx ? '#FFD54F' : '#FFFFFF')
                  .position({
                    x: this.pinVpX(this.perspectiveQuad[8 + i * 2]) - 6,
                    y: this.pinVpY(this.perspectiveQuad[8 + i * 2 + 1]) - 6
                  })
                  .hitTestBehavior(HitTestMode.None)
              }, (i: number): string => `pq|${i}|${this.perspectiveQuad[8 + i * 2]}|${this.perspectiveQuad[8 + i * 2 + 1]}`)
            }
            // M20d 消失点预览：定形平面黄折线 / 采集中青点
            if (this.tool === 'vanishing') {
              if (this.vpQuad.length === 8) {
                Polyline()
                  .width('100%').height('100%')
                  .points(this.perspectiveViewportPtsOf(this.vpQuad))
                  .stroke('#FFD54F').strokeWidth(1)
                  .fillOpacity(0)
                  .hitTestBehavior(HitTestMode.None)
                if (this.vpSourceSet) {
                  Circle()
                    .width(10).height(10).fill('#00E5FF')
                    .position({ x: this.pinVpX(this.vpSource[0]) - 5, y: this.pinVpY(this.vpSource[1]) - 5 })
                    .hitTestBehavior(HitTestMode.None)
                }
              } else if (this.vpQuad.length >= 2) {
                ForEach(this.vpPendingIdx(), (i: number): void => {
                    Circle()
                      .width(8).height(8).fill('#00E5FF')
                      .position({ x: this.pinVpX(this.vpQuad[i * 2]) - 4, y: this.pinVpY(this.vpQuad[i * 2 + 1]) - 4 })
                      .hitTestBehavior(HitTestMode.None)
                  }, (i: number): string => `vq|${i}|${this.vpQuad[i * 2]}|${this.vpQuad[i * 2 + 1]}`)
              }
            }
            // M18c 钢笔路径预览：白折线 + 青锚点 + 首锚点大圈（闭合热点）
            if (this.penPoints.length >= 4) {
              Polyline()
                .width('100%')
                .height('100%')
                .points(this.penViewportPts())
                .stroke('#FFFFFF')
                .strokeWidth(1)
                .fillOpacity(0)
                .hitTestBehavior(HitTestMode.None)
            }
            ForEach(this.penViewportPts(), (p: number[], idx: number) => {
              Circle()
                .width(idx === 0 ? 12 : 7)
                .height(idx === 0 ? 12 : 7)
                .fill(idx === 0 ? '#00E5FF' : '#FFFFFF')
                .position({ x: p[0] - (idx === 0 ? 6 : 3.5), y: p[1] - (idx === 0 ? 6 : 3.5) })
                .hitTestBehavior(HitTestMode.None)
            }, (p: number[], idx: number): string => `pen${idx}|${p[0]}|${p[1]}`)
            // M18.1 直接选择预览：白折线 + 锚点方点（选中=青）+ 单选手柄线
            if (this.tool === 'directSelect' && this.dsPreviewPts.length >= 2) {
              Polyline()
                .width('100%')
                .height('100%')
                .points(this.dsViewportPts())
                .stroke('#FFFFFF')
                .strokeWidth(1)
                .fillOpacity(0)
                .hitTestBehavior(HitTestMode.None)
              ForEach(this.dsAnchorPts, (p: number[], idx: number) => {
                Rect()
                  .width(p[2] > 0.5 ? 10 : 7)
                  .height(p[2] > 0.5 ? 10 : 7)
                  .fill(p[2] > 0.5 ? '#00E5FF' : '#FFFFFF')
                  .position({ x: p[0] - (p[2] > 0.5 ? 5 : 3.5), y: p[1] - (p[2] > 0.5 ? 5 : 3.5) })
                  .hitTestBehavior(HitTestMode.None)
              }, (p: number[], idx: number): string => `dsa${idx}|${p[0]}|${p[1]}|${p[2]}`)
              ForEach(this.dsHandlePts, (p: number[], idx: number) => {
                Line()
                  .startPoint([p[2], p[3]])
                  .endPoint([p[0], p[1]])
                  .stroke('#888888')
                  .strokeWidth(1)
                  .hitTestBehavior(HitTestMode.None)
                Circle()
                  .width(7)
                  .height(7)
                  .fill('#00E5FF')
                  .position({ x: p[0] - 3.5, y: p[1] - 3.5 })
                  .hitTestBehavior(HitTestMode.None)
              }, (p: number[], idx: number): string => `dsh${idx}|${p[0]}|${p[1]}`)
            }
            if (this.dsMarqueeRect !== null) {
              Column()
                .position({ x: this.dsMarqueeRect[0], y: this.dsMarqueeRect[1] })
                .width(this.dsMarqueeRect[2])
                .height(this.dsMarqueeRect[3])
                .border({ width: 1, color: '#00E5FF' })
                .backgroundColor('#10000000')
                .hitTestBehavior(HitTestMode.None)
            }
            // M11c 形状拖拽预览：青色虚线框（纯视觉，None 防拦截触摸）
            if (this.shapePreview !== null) {
              Column()
                .position({
                  x: this.shapePreview[0],
                  y: this.shapePreview[1]
                })
                .width(this.shapePreview[2])
                .height(this.shapePreview[3])
                .border({ width: 1, color: '#00E5FF' })
                .backgroundColor('#14000000')
                .hitTestBehavior(HitTestMode.None)
            }
            // M12 文字拖框预览：白色虚线框（拖宽 = 段落换行宽）
            if (this.textDrag !== null) {
              Column()
                .position({ x: this.textDrag[0], y: this.textDrag[1] })
                .width(this.textDrag[2])
                .height(this.textDrag[3])
                .border({ width: 1, color: '#FFFFFF' })
                .backgroundColor('#14000000')
                .hitTestBehavior(HitTestMode.None)
            }
            // M12 文本编辑 overlay：RichEditor 浮于文本框位置（编辑层 GL 隐藏防双影）。
            // 字号按视口换算 px；内容变更 debounce 后走无事务 relayout（endTextSession 一步撤销）。
            if (this.editingBoxVp().length === 4) {
              RichEditor({ controller: this.richController })
                .position({ x: this.editingBoxVp()[0], y: this.editingBoxVp()[1] })
                .width(this.editingBoxVp()[2])
                .height(this.editingBoxVp()[3])
                .padding(0)
                .backgroundColor('#0D000000')
                .border({ width: 1, color: '#00E5FF' })
                .onReady((): void => {
                  const a = this.layers.find(
                    (l: LayerDTO): boolean => l.id === this.editingTextId);
                  if (a !== undefined && a.textContent.length > 0) {
                    this.richController.addTextSpan(a.textContent, {
                      style: {
                        fontSize: `${Math.max(6, a.textFontSize * this.zoom / this.pxPerVp())}px`,
                        fontColor: a.textColor
                      }
                    });
                  }
                })
                .onDidChange((): void => {
                  this.onOverlayChanged();
                })
                .onKeyEvent((ev: KeyEvent): void => {
                  // 焦点在 overlay 时画布 onKey 收不到键（焦点链实证）：Esc/Ctrl+Enter 提交会话
                  if (ev.type !== KeyType.Down) {
                    return;
                  }
                  if (ev.keyCode === 2070 || (ev.keyCode === 2054 && this.ctrlHeld)) {
                    this.commitTextEditing();
                  }
                })
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
            }
            .layoutWeight(1)
            .height('100%')
          }
          .layoutWeight(1)
          .height('100%')

          // 右侧面板坞
          if (this.screenMode === 0 && this.layersVisible) {
            Column() {
              if (this.activeLayer()?.isAdjustment === true || this.showAdjCreate) {
                AdjustPanel({
                  adjKind: this.activeLayer()?.adjKind ?? 0,
                  adjBlack: this.adjBlack,
                  adjWhite: this.adjWhite,
                  adjGammaX100: Math.round(this.adjGamma * 100),
                  adjP0: this.adjP0,
                  adjP1: this.adjP1,
                  adjP2: this.adjP2,
                  curveXs: this.curveXs,
                  curveYs: this.curveYs,
                  histLuma: this.histLuma,
                  isAdjustment: this.activeLayer()?.isAdjustment === true,
                  onSetKind: (kind: number): void => {
                    const a = this.activeLayer();
                    if (a !== undefined && a.isAdjustment) {
                      this.engine.setAdjustmentKind(a.id, kind, 0, 0, 0, 0);
                    } else {
                      this.engine.addAdjustmentLayer(kind);
                    }
                  },
                  onLevels: (b: number, w: number, g: number): void => {
                    const a = this.activeLayer();
                    if (a !== undefined) {
                      this.engine.setAdjustmentParams(a.id, b / 255, w / 255, g);
                    }
                  },
                  onParams: (p0: number, p1: number, p2: number): void => {
                    const a = this.activeLayer();
                    if (a !== undefined) {
                      this.engine.setAdjustmentKind(a.id, a.adjKind, p0, p1, p2, 0);
                    }
                  },
                  onCurve: (xs: number[], ys: number[]): void => {
                    const a = this.activeLayer();
                    if (a !== undefined) {
                      this.engine.setCurvePoints(a.id, xs, ys);
                    }
                  }
                })
              }
              // M10c 通道面板 v1（只读列表 + 删除；存储选区入口在选择菜单）
              if (this.channelsCount > 0) {
                ChannelsPanel({
                  engine: this.engine,
                  onChanged: (): void => this.refreshChannelCount(),
                })
              }
              // M18c 路径面板（钢笔工具激活或工作路径非空时显示）
              if (this.tool === 'pen' || this.engine.getWorkPath().length > 1) {
                PathsPanel({
                  engine: this.engine,
                  fgHex: this.brushColor,
                  onChanged: (): void => {
                    this.refreshLayers();
                    this.refreshStats();
                  }
                })
              }
              // M12 文字面板（T 工具激活或活动层为文本层时显示）
              if (this.tool === 'text' || this.activeLayer()?.isText === true) {
                TextPanel({
                  fontList: this.fontOptions(),
                  fontSel: this.panelFontSel,
                  fontSize: this.textFontSize,
                  lineHeightPct: this.textLineHeightPct,
                  tracking: this.textTracking,
                  bold: this.textBold,
                  italic: this.textItalic,
                  colorHex: this.textColor,
                  alignIdx: this.textAlignIdx,
                  wrapW: this.activeLayer()?.textWrapWidth ?? 0,
                  content: this.textPanelContent,
                  editing: this.editingTextId !== 0,
                  isText: this.activeLayer()?.isText === true,
                  onPickFont: (idx: number): void => {
                    this.panelFontSel = idx;
                    this.pushTextStyle();
                  },
                  onImportFont: (): void => this.doImportFont(),
                  onFontSize: (v: number): void => {
                    this.textFontSize = v;
                    this.pushTextStyle();
                  },
                  onLineHeight: (v: number): void => {
                    this.textLineHeightPct = v;
                    this.pushTextStyle();
                  },
                  onTracking: (v: number): void => {
                    this.textTracking = v;
                    this.pushTextStyle();
                  },
                  onToggleBold: (): void => {
                    this.textBold = !this.textBold;
                    this.pushTextStyle();
                  },
                  onToggleItalic: (): void => {
                    this.textItalic = !this.textItalic;
                    this.pushTextStyle();
                  },
                  onPickColor: (c: string): void => {
                    this.textColor = c;
                    this.pushTextStyle();
                  },
                  onAlign: (al: number): void => {
                    this.textAlignIdx = al;
                    this.pushTextStyle();
                  },
                  onContent: (t: string): void => {
                    this.pushTextContent(t);
                  },
                  onRasterize: (): void => this.runCommand('layer.rasterizeText'),
                })
              }
              LayersPanel({
                layers: $layers,
                layerOpacity: $layerOpacity,
                blendSel: $blendSel,
                adjBlack: $adjBlack,
                adjWhite: $adjWhite,
                adjGamma: $adjGamma,
                thumbVersion: this.thumbVersion,
                panelVersion: this.panelVersion,
                docOpen: this.docOpen,
                selectedIds: this.multiSelected,
                getThumb: (id: number): Object | null => this.getThumb(id),
                onSelect: (id: number, e: ClickEvent): void => this.onLayerRowSelect(id, e),
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
                onToggleCollapse: (l: LayerDTO): void => {
                  this.engine.setGroupCollapsed(l.id, !l.collapsed);
                },
                onGroup: (): void => this.runCommand('layer.group'),
                onUngroup: (): void => this.runCommand('layer.ungroup'),
                onDuplicate: (): void => this.runCommand('layer.duplicate'),
                onLinkToggle: (): void => this.onLayerLinkToggle(),
                onStyle: (): void => this.runCommand('layer.style'),
                onSmartFilters: (l: LayerDTO): void => {
                  this.runCommand('layer.smartFiltersDialog');
                },
                onMoveToParent: (id: number, newParent: number, flatIndex: number): void =>
                  this.onMoveToParent(id, newParent, flatIndex),
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

          // M15b 面板坞第二列（历史/直方图/信息/动作；PS 多列面板语义）
          if (this.screenMode === 0 &&
            (this.historyVisible || this.histogramVisible || this.infoVisible ||
              this.actionsVisible)) {
            Column({ space: 6 }) {
              if (this.historyVisible) {
                HistoryPanel({
                  labels: this.historyLabels,
                  current: this.historyCurrent,
                  onPick: (idx: number): void => this.pickHistoryState(idx),
                })
                  .layoutWeight(1)
              }
              if (this.histogramVisible) {
                HistogramPanel({ luma: this.histLuma })
              }
              if (this.infoVisible) {
                InfoPanel({
                  docW: this.docW,
                  docH: this.docH,
                  pointerX: this.infoPointerX,
                  pointerY: this.infoPointerY,
                  colorHex: this.infoColor,
                  hasSelection: this.infoSelOk,
                  selW: this.infoSelW,
                  selH: this.infoSelH,
                })
              }
              if (this.actionsVisible) {
                ActionsPanel({
                  rev: this.actionsRev,
                  selected: this.actionsSel,
                  docOpen: this.docOpen,
                  onSelect: (idx: number): void => {
                    this.actionsSel = idx;
                  },
                  onMessage: (msg: string): void => {
                    if (msg === '__rename_action__') {
                      const g = automation.getAction({ 'action': this.actionsSel });
                      let name = '';
                      if (g.ok && g.value !== undefined) {
                        const v = g.value as Record<string, Object>;
                        const nm = v['name'];
                        if (typeof nm === 'string') {
                          name = nm;
                        }
                      }
                      this.actionRenameText = name;
                      this.showActionRename = true;
                    } else {
                      this.log(msg);
                      this.toast(msg);
                    }
                  },
                  onMutated: (): void => {
                    this.actionsRev++;
                  },
                })
                  .layoutWeight(1)
              }
            }
            .width(240)
            .height('100%')
            .padding(4)
            .border({ width: { left: 1 }, color: { left: $r('app.color.divider') } })
          }
        }
        .layoutWeight(1)
        .alignItems(VerticalAlign.Top)

        if (this.screenMode < 2) {
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
      }
      // M24a：动作重命名对话框
      if (this.showActionRename) {
        Stack() {
          Column()
            .width('100%')
            .height('100%')
            .backgroundColor('#66000000')
            .onClick((): void => {
              this.showActionRename = false;
            })
          Column({ space: 14 }) {
            Text('重命名动作').fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            TextInput({ text: this.actionRenameText })
              .height(40)
              .width('100%')
              .fontSize(14)
              .onChange((v: string): void => {
                this.actionRenameText = v;
              })
              .defaultFocus(true)
            Row({ space: 12 }) {
              Button('取消').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => {
                  this.showActionRename = false;
                })
              Button('确定').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => {
                  automation.execute('actions.rename', {
                    'action': this.actionsSel, 'name': this.actionRenameText,
                  }).then((r): void => {
                    this.showActionRename = false;
                    if (!r.ok) {
                      this.toast(r.error);
                    } else {
                      this.actionsRev++;
                      this.toast('动作已重命名');
                    }
                  });
                })
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
      // M24b：批处理 / 图像处理器对话框
      if (this.showAutomate) {
        AutomateDialog({
          mode: this.automateMode,
          busy: this.automateBusy,
          progressText: this.automateProgress,
          resultText: this.automateResult,
          actionNames: this.actionNamesMirror,
          onRun: (params: Record<string, Object>): void => this.runAutomate(params),
          onCancelRun: (): void => {
            this.automateCancel = true;
          },
          onClose: (): void => {
            this.showAutomate = false;
          },
        })
      }
      if (this.showSmartFilters && this.smartInfo !== null) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.showSmartFilters = false; })
          Column({ space: 12 }) {
            Text('智能滤镜')
              .fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            Text('栈底 → 栈顶 · 混合/蒙版作用于下方全部滤镜结果')
              .fontSize(11).fontColor($r('app.color.text_secondary'))
            Row({ space: 10 }) {
              Toggle({ type: ToggleType.Checkbox, isOn: this.smartInfo.filtersEnabled })
                .onChange((v: boolean): void => { this.onSmartFiltersEnabled(v); })
              Text('启用智能滤镜（整栈）').fontSize(13).fontColor($r('app.color.text_primary'))
              Blank()
              Toggle({ type: ToggleType.Checkbox, isOn: this.smartInfo.filterMaskEnabled })
                .onChange((v: boolean): void => { this.onSmartFilterMask(2, v); })
                .enabled(this.smartInfo.hasFilterMask)
              Text('滤镜蒙版').fontSize(13).fontColor($r('app.color.text_primary'))
            }.width('100%')
            List({ space: 4 }) {
              ForEach(this.smartInfo.filters, (f: SmartFilterDTO, idx: number): void => {
                ListItem() {
                Row({ space: 8 }) {
                  Toggle({ type: ToggleType.Checkbox, isOn: f.visible })
                    .onChange((v: boolean): void => { this.onSmartFilterVisible(idx, v); })
                  Column({ space: 2 }).layoutWeight(1).alignItems(HorizontalAlign.Start) {
                    Text(f.name + (f.unsupported ? '（PSD 未实现，直通）' : ''))
                      .fontSize(13).fontColor($r('app.color.text_primary'))
                      .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
                    Text(`不透明度 ${Math.round(f.opacity * 100)}% · 混合 #${f.blend}`)
                      .fontSize(10).fontColor($r('app.color.text_secondary'))
                  }
                  Button('上').fontSize(11).height(26).width(34)
                    .backgroundColor($r('app.color.surface'))
                    .fontColor($r('app.color.text_secondary'))
                    .onClick((): void => this.onSmartFilterMove(idx, true))
                  Button('下').fontSize(11).height(26).width(34)
                    .backgroundColor($r('app.color.surface'))
                    .fontColor($r('app.color.text_secondary'))
                    .onClick((): void => this.onSmartFilterMove(idx, false))
                  if (!f.unsupported) {
                    Button('编辑').fontSize(11).height(26).width(48)
                      .backgroundColor($r('app.color.surface'))
                      .fontColor($r('app.color.accent'))
                      .onClick((): void => this.onSmartFilterEdit(idx))
                  }
                  Button('删').fontSize(11).height(26).width(34)
                    .backgroundColor($r('app.color.surface'))
                    .fontColor($r('app.color.danger'))
                    .onClick((): void => this.onSmartFilterRemove(idx))
                }.width('100%').padding(6).borderRadius(8)
                .backgroundColor($r('app.color.panel_background'))
                }
              }, (f: SmartFilterDTO, idx: number): string =>
                `${idx}|${f.name}|${f.visible}|${this.smartInfo?.filters.length ?? 0}`)
            }.layoutWeight(1).width('100%')
            Row({ space: 12 }) {
              if (this.smartInfo.hasFilterMask) {
                Button('清除滤镜蒙版').fontSize(13).height(38)
                  .backgroundColor($r('app.color.surface'))
                  .fontColor($r('app.color.text_primary'))
                  .onClick((): void => { this.onSmartFilterMask(0, false); })
              }
              Blank()
              Button('关闭').fontSize(14).height(38).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => { this.showSmartFilters = false; })
            }.width('100%')
          }.padding(20).borderRadius(14).backgroundColor($r('app.color.surface'))
            .border({ width: 1, color: $r('app.color.border') }).width(420).height(480)
        }.width('100%').height('100%')
      }
      if (this.showImageSize) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.showImageSize = false; })
          Column({ space: 14 }) {
            Text(this.imageSizeMode === 1 ? '内容感知比例' : '图像大小')
              .fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            if (this.imageSizeMode === 1) {
              Text('活动图层缝雕刻到新尺寸 · 选区=保护区')
                .fontSize(11)
                .fontColor($r('app.color.text_secondary'))
            }
            Row({ space: 10 }) {
              Text('宽').fontSize(13).fontColor($r('app.color.text_secondary'))
              TextInput({ text: this.imgWText }).width(120).height(40)
                .onChange((v: string): void => { this.imgWText = v; })
              Text('高').fontSize(13).fontColor($r('app.color.text_secondary'))
              TextInput({ text: this.imgHText }).width(120).height(40)
                .onChange((v: string): void => { this.imgHText = v; })
            }
            if (this.imageSizeMode === 0) {
              // M20a 重采样方法下拉（PS Image Size 选项同序）
              Row({ space: 10 }) {
                Text('重新采样').fontSize(13).fontColor($r('app.color.text_secondary'))
                Select(this.resampleOptions())
                  .selected(this.imgResampleMethod)
                  .value(this.resampleLabel(this.imgResampleMethod))
                  .fontColor($r('app.color.text_primary'))
                  .font({ size: 13 })
                  .optionFont({ size: 13 })
                  .width(200)
                  .onSelect((idx: number): void => { this.imgResampleMethod = idx; })
              }.width('100%')
            }
            Row({ space: 12 }) {
              Button('取消').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => { this.showImageSize = false; })
              Button('确定').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => this.commitImageSize())
            }.justifyContent(FlexAlign.End).width('100%')
          }.padding(20).borderRadius(14).backgroundColor($r('app.color.surface'))
            .border({ width: 1, color: $r('app.color.border') }).width(400)
        }.width('100%').height('100%')
      }
      if (this.showWideAngle) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.showWideAngle = false; })
          Column({ space: 14 }) {
            Text('自适应广角').fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            Text('拉直广角/鱼眼镜头的弯线 · 约束线 overlay 登记下批')
              .fontSize(11)
              .fontColor($r('app.color.text_secondary'))
            Row({ space: 10 }) {
              Button(this.waModelLabel()).fontSize(12).height(28).width(76)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => { this.waModel = (this.waModel + 1) % 4; })
              Text('焦距').fontSize(13).fontColor($r('app.color.text_secondary'))
              Slider({ value: this.waFocal, min: 5, max: 120, step: 1 })
                .layoutWeight(1)
                .onChange((v: number): void => { this.waFocal = Math.round(v); })
              Text(`${Math.round(this.waFocal)}mm`).fontSize(12).fontColor($r('app.color.text_primary')).width(52)
            }.width('100%')
            Row({ space: 10 }) {
              Text('裁切系数').fontSize(13).fontColor($r('app.color.text_secondary'))
              Slider({ value: this.waCrop, min: 1, max: 3, step: 0.1 })
                .layoutWeight(1)
                .onChange((v: number): void => { this.waCrop = Math.round(v * 10) / 10; })
              Text(`${Math.round(this.waCrop * 10) / 10}`).fontSize(12).fontColor($r('app.color.text_primary')).width(52)
            }.width('100%')
            Row({ space: 10 }) {
              Text('缩放').fontSize(13).fontColor($r('app.color.text_secondary'))
              Slider({ value: this.waScale, min: 50, max: 150, step: 1 })
                .layoutWeight(1)
                .onChange((v: number): void => { this.waScale = Math.round(v); })
              Text(`${Math.round(this.waScale)}%`).fontSize(12).fontColor($r('app.color.text_primary')).width(52)
            }.width('100%')
            Row({ space: 12 }) {
              Button('取消').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => { this.showWideAngle = false; })
              Button('确定').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => this.wideAngleApply())
            }.justifyContent(FlexAlign.End).width('100%')
          }.padding(20).borderRadius(14).backgroundColor($r('app.color.surface'))
            .border({ width: 1, color: $r('app.color.border') }).width(420)
        }.width('100%').height('100%')
      }
      if (this.showModify) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.showModify = false; })
          Column({ space: 14 }) {
            Text(this.modifyKind === 'feather' ? '羽化选区' :
              this.modifyKind === 'border' ? '边界' :
              this.modifyKind === 'expand' ? '扩展选区' :
              this.modifyKind === 'stroke' ? '描边' : '收缩选区')
              .fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            Row({ space: 10 }) {
              Text(this.modifyKind === 'stroke' ? '宽度' : '半径').fontSize(13)
                .fontColor($r('app.color.text_secondary'))
              TextInput({ text: this.modifyText }).width(120).height(40)
                .onChange((v: string): void => { this.modifyText = v; })
              Text('像素').fontSize(13).fontColor($r('app.color.text_secondary'))
            }
            Row({ space: 12 }) {
              Button('取消').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => { this.showModify = false; })
              Button('确定').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => this.commitModify())
            }.justifyContent(FlexAlign.End).width('100%')
          }.padding(20).borderRadius(14).backgroundColor($r('app.color.surface'))
            .border({ width: 1, color: $r('app.color.border') }).width(360)
        }.width('100%').height('100%')
      }
      if (this.showFilter && this.filterDef !== null) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.cancelFilterDialog(); })
          Column({ space: 14 }) {
            Text(this.filterDef.label)
              .fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            Text('1/4 代理实时预览；选区约束自动生效')
              .fontSize(11).fontColor($r('app.color.text_secondary'))
            if (this.filterDef.kind === 30) {
              // M22 颜色查找：预设 chips（内置外观 + 已导入用户表槽）
              Flex({ wrap: FlexWrap.Wrap }) {
                ForEach(this.lookupPresets, (pr: LookupPresetDTO) => {
                  Text(pr.label)
                    .fontSize(11)
                    .fontColor((this.filterP[0] ?? 0) === pr.param
                      ? $r('app.color.on_accent') : $r('app.color.text_primary'))
                    .backgroundColor((this.filterP[0] ?? 0) === pr.param
                      ? $r('app.color.accent') : $r('app.color.surface'))
                    .borderRadius(6)
                    .padding({ left: 8, right: 8, top: 4, bottom: 4 })
                    .margin(2)
                    .onClick((): void => { this.onFilterParamChange(0, pr.param); })
                }, (pr: LookupPresetDTO): string => `${pr.id}|${pr.param}`)
              }.width('100%')
              Text('更多外观：编辑菜单「载入查找表…」导入 .cube/.3dl/.look')
                .fontSize(10).fontColor($r('app.color.text_secondary'))
            } else {
            ForEach(this.filterDef.params, (d: FilterParamDef, idx: number) => {
              Row({ space: 10 }) {
                Text(d.label).fontSize(13).fontColor($r('app.color.text_secondary')).width(84)
                Slider({ value: this.filterP[idx] ?? d.def, min: d.min, max: d.max, step: d.step })
                  .layoutWeight(1)
                  .onChange((v: number): void => { this.onFilterParamChange(idx, v); })
                Text(`${Math.round((this.filterP[idx] ?? d.def) * 10) / 10}${d.unit}`)
                  .fontSize(12).fontColor($r('app.color.text_primary')).width(64)
                  .textAlign(TextAlign.End)
              }.width('100%')
            }, (d: FilterParamDef, idx: number): string => `${idx}|${d.label}`)
            }
            if (this.filterDef.hasSeed) {
              Row({ space: 10 }) {
                Text('随机种子').fontSize(13).fontColor($r('app.color.text_secondary')).width(84)
                Slider({ value: this.filterSeed, min: 1, max: 9999, step: 1 })
                  .layoutWeight(1)
                  .onChange((v: number): void => {
                    this.filterSeed = Math.round(v);
                    this.onFilterParamChange(-1, 0);
                  })
                Text(`${this.filterSeed}`).fontSize(12).fontColor($r('app.color.text_primary')).width(64)
                  .textAlign(TextAlign.End)
              }.width('100%')
            }
            Row({ space: 12 }) {
              Button('取消').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => { this.cancelFilterDialog(); })
              Button('确定').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => this.commitFilterDialog())
            }.justifyContent(FlexAlign.End).width('100%')
          }.padding(20).borderRadius(14).backgroundColor($r('app.color.surface'))
            .border({ width: 1, color: $r('app.color.border') }).width(400)
        }.width('100%').height('100%')
      }
      // M15e RAW 显影参数面板（会话内有 RAW 缓存才可开；确定=异步重显影+一步事务）
      if (this.showDevelop) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.cancelDevelop(); })
          Column({ space: 14 }) {
            Text(this.devLayerMode ? 'Camera Raw 滤镜' : 'RAW 显影参数').fontSize(16)
              .fontWeight(FontWeight.Bold).fontColor($r('app.color.text_primary'))
            Text(this.devLayerMode ? '作用于活动图层 · 实时预览 · 确定 = 一步撤销事务'
              : '恒从原始 RAW 重显影（会话内非破坏）· 实时预览 · 确定 = 一步撤销')
              .fontSize(11).fontColor($r('app.color.text_secondary'))
            Scroll() {
              Column({ space: 10 }) {
                if (!this.devLayerMode) {
                  Row({ space: 8 }) {
                    Toggle({ type: ToggleType.Switch, isOn: this.devCameraOn })
                      .onChange((on: boolean): void => { this.devCameraOn = on; })
                    Text('Camera Raw 显影链').fontSize(13)
                      .fontColor($r('app.color.text_primary'))
                  }.width('100%')
                }
                if (!this.devCameraOn) {
                  Row({ space: 10 }) {
                    Text('色温').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devTemp, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devTemp = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devTemp)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('色调').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devTint, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devTint = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devTint)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('曝光').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devEv, min: -2, max: 2, step: 0.05 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devEv = v; this.onDevSlider(mode); })
                    Text(`${this.devEv.toFixed(2)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                }
                if (!this.devLayerMode) {
                  Row({ space: 10 }) {
                    Text('伽马').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devGamma, min: 0.5, max: 2, step: 0.05 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devGamma = v; this.onDevSlider(mode); })
                    Text(`${this.devGamma.toFixed(2)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                }
                if (this.devCameraOn) {
                  Text('Basic').fontSize(12).fontWeight(FontWeight.Bold)
                    .fontColor($r('app.color.text_secondary')).width('100%')
                  Row({ space: 10 }) {
                    Text('色温').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devTemp, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devTemp = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devTemp)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('色调').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devTint, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devTint = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devTint)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('曝光').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devExposure, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devExposure = v; this.onDevSlider(mode); })
                    Text(`${(this.devExposure / 50).toFixed(1)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('对比度').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devContrast, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devContrast = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devContrast)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('高光').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devHighlights, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devHighlights = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devHighlights)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('阴影').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devShadows, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devShadows = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devShadows)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('白色').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devWhites, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devWhites = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devWhites)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('黑色').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devBlacks, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devBlacks = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devBlacks)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('纹理').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devTexture, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devTexture = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devTexture)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('清晰度').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devClarity, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devClarity = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devClarity)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('去雾').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devDehaze, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devDehaze = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devDehaze)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('自然饱和').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devVibrance, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devVibrance = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devVibrance)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('饱和度').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devSaturation, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devSaturation = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devSaturation)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  // M23c：色调曲线（参数四区 + 点曲线编辑器）
                  Text('色调曲线').fontSize(12).fontWeight(FontWeight.Bold)
                    .fontColor($r('app.color.text_secondary')).width('100%')
                  Row({ space: 10 }) {
                    Text('高光').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devCrvHl, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devCrvHl = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devCrvHl)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('亮调').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devCrvLg, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devCrvLg = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devCrvLg)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('暗调').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devCrvDk, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devCrvDk = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devCrvDk)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('阴影').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devCrvSh, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devCrvSh = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devCrvSh)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 8 }) {
                    Canvas(this.devCurveCtx)
                      .width(200).height(200)
                      .onReady((): void => {
                        this.devCurveReady = true;
                        this.devDrawCurve();
                      })
                      .onTouch((e: TouchEvent): void => this.devCurveTouch(e))
                    Column({ space: 6 }) {
                      Text('点曲线').fontSize(11).fontColor($r('app.color.text_secondary'))
                      Button('重置曲线').fontSize(11).height(30)
                        .backgroundColor($r('app.color.surface'))
                        .fontColor($r('app.color.text_primary'))
                        .border({ width: 1, color: $r('app.color.border') })
                        .onClick((): void => {
                          this.devCurvePts = [];
                          this.devCurveDrag = -1;
                          if (this.devCurveReady) {
                            this.devDrawCurve();
                          }
                          this.scheduleDevPreview();
                        })
                      Text('拖点调整；空白单击加点').fontSize(10)
                        .fontColor($r('app.color.text_secondary'))
                    }.alignItems(HorizontalAlign.Center)
                  }.width('100%').justifyContent(FlexAlign.Center)
                  // M23c：HSL 混色器（八色相带；选中带三滑条）
                  Text('HSL 混色器').fontSize(12).fontWeight(FontWeight.Bold)
                    .fontColor($r('app.color.text_secondary')).width('100%')
                  Flex({ wrap: FlexWrap.Wrap }) {
                    ForEach(['红', '橙', '黄', '绿', '青', '蓝', '紫', '洋红'],
                      (lb: string, i: number) => {
                        Text(lb)
                          .fontSize(10)
                          .fontColor(this.devHslBand === i
                            ? $r('app.color.on_accent') : $r('app.color.text_primary'))
                          .backgroundColor(this.devHslBand === i
                            ? $r('app.color.accent') : $r('app.color.surface'))
                          .borderRadius(6)
                          .padding({ left: 8, right: 8, top: 4, bottom: 4 })
                          .margin(2)
                          .onClick((): void => { this.devHslBand = i; })
                      }, (lb: string): string => lb)
                  }.width('100%')
                  Row({ space: 10 }) {
                    Text('色相').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devHslH[this.devHslBand], min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => {
                        this.devHslH = this.devHslH.map((o: number, i: number): number =>
                          i === this.devHslBand ? v : o);
                        this.onDevSlider(mode);
                      })
                    Text(`${Math.round(this.devHslH[this.devHslBand])}`).fontSize(12).width(44)
                      .textAlign(TextAlign.End).fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('饱和度').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devHslS[this.devHslBand], min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => {
                        this.devHslS = this.devHslS.map((o: number, i: number): number =>
                          i === this.devHslBand ? v : o);
                        this.onDevSlider(mode);
                      })
                    Text(`${Math.round(this.devHslS[this.devHslBand])}`).fontSize(12).width(44)
                      .textAlign(TextAlign.End).fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('明度').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devHslL[this.devHslBand], min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => {
                        this.devHslL = this.devHslL.map((o: number, i: number): number =>
                          i === this.devHslBand ? v : o);
                        this.onDevSlider(mode);
                      })
                    Text(`${Math.round(this.devHslL[this.devHslBand])}`).fontSize(12).width(44)
                      .textAlign(TextAlign.End).fontColor($r('app.color.text_primary'))
                  }
                  // M23c：颜色分级（四轮；选中轮三滑条 + 混合/平衡）
                  Text('颜色分级').fontSize(12).fontWeight(FontWeight.Bold)
                    .fontColor($r('app.color.text_secondary')).width('100%')
                  Flex({ wrap: FlexWrap.Wrap }) {
                    ForEach(['阴影', '中间调', '高光', '全局'],
                      (lb: string, i: number) => {
                        Text(lb)
                          .fontSize(10)
                          .fontColor(this.devGradeWheel === i
                            ? $r('app.color.on_accent') : $r('app.color.text_primary'))
                          .backgroundColor(this.devGradeWheel === i
                            ? $r('app.color.accent') : $r('app.color.surface'))
                          .borderRadius(6)
                          .padding({ left: 8, right: 8, top: 4, bottom: 4 })
                          .margin(2)
                          .onClick((): void => { this.devGradeWheel = i; })
                      }, (lb: string): string => lb)
                  }.width('100%')
                  Row({ space: 10 }) {
                    Text('色相').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devGradeH[this.devGradeWheel], min: 0, max: 360, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => {
                        this.devGradeH = this.devGradeH.map((o: number, i: number): number =>
                          i === this.devGradeWheel ? v : o);
                        this.onDevSlider(mode);
                      })
                    Text(`${Math.round(this.devGradeH[this.devGradeWheel])}°`).fontSize(12).width(44)
                      .textAlign(TextAlign.End).fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('饱和').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devGradeS[this.devGradeWheel], min: 0, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => {
                        this.devGradeS = this.devGradeS.map((o: number, i: number): number =>
                          i === this.devGradeWheel ? v : o);
                        this.onDevSlider(mode);
                      })
                    Text(`${Math.round(this.devGradeS[this.devGradeWheel])}`).fontSize(12).width(44)
                      .textAlign(TextAlign.End).fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('亮度').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devGradeL[this.devGradeWheel], min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => {
                        this.devGradeL = this.devGradeL.map((o: number, i: number): number =>
                          i === this.devGradeWheel ? v : o);
                        this.onDevSlider(mode);
                      })
                    Text(`${Math.round(this.devGradeL[this.devGradeWheel])}`).fontSize(12).width(44)
                      .textAlign(TextAlign.End).fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('混合').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devGradeBlend, min: 0, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devGradeBlend = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devGradeBlend)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('平衡').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devGradeBal, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devGradeBal = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devGradeBal)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Text('细节 / 效果').fontSize(12).fontWeight(FontWeight.Bold)
                    .fontColor($r('app.color.text_secondary')).width('100%')
                  Row({ space: 10 }) {
                    Text('锐化').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devSharpen, min: 0, max: 300, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devSharpen = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devSharpen)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('亮度降噪').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devNoiseLum, min: 0, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devNoiseLum = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devNoiseLum)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('彩色降噪').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devNoiseColor, min: 0, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devNoiseColor = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devNoiseColor)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('暗角').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devVignette, min: -100, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devVignette = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devVignette)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                  Row({ space: 10 }) {
                    Text('颗粒').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                    Slider({ value: this.devGrain, min: 0, max: 100, step: 1 })
                      .layoutWeight(1)
                      .onChange((v: number, mode: SliderChangeMode): void => { this.devGrain = v; this.onDevSlider(mode); })
                    Text(`${Math.round(this.devGrain)}`).fontSize(12).width(44).textAlign(TextAlign.End)
                      .fontColor($r('app.color.text_primary'))
                  }
                }
              }.width('100%')
            }.scrollable(ScrollDirection.Vertical).width('100%').constraintSize({ maxHeight: 420 })
            Row({ space: 12 }) {
              Button('取消').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => this.cancelDevelop())
              Button('确定').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => this.commitDevelop())
            }.justifyContent(FlexAlign.End).width('100%')
          }.padding(20).borderRadius(14).backgroundColor($r('app.color.surface'))
            .border({ width: 1, color: $r('app.color.border') }).width(440)
        }.width('100%').height('100%')
      }
      // M17b 全局光角度
      if (this.showGlobalLight) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.showGlobalLight = false; })
          Column({ space: 14 }) {
            Text('全局光角度').fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            Text('投影 / 内阴影 / 斜面浮雕勾选「全局光」时引用此角度')
              .fontSize(11).fontColor($r('app.color.text_secondary'))
            Row({ space: 10 }) {
              Text('角度').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
              Slider({ value: this.globalLightAngle, min: 0, max: 360, step: 1 })
                .layoutWeight(1)
                .onChange((v: number, mode: SliderChangeMode): void => {
                  if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                    this.globalLightAngle = v;
                    this.applyGlobalLight();
                  }
                })
              Text(`${Math.round(this.globalLightAngle)}°`).fontSize(12).width(44)
                .textAlign(TextAlign.End).fontColor($r('app.color.text_primary'))
            }
            Row({ space: 12 }) {
              Button('关闭').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => { this.showGlobalLight = false; })
            }.justifyContent(FlexAlign.End).width('100%')
          }.padding(20).borderRadius(14).backgroundColor($r('app.color.surface'))
            .border({ width: 1, color: $r('app.color.border') }).width(400)
        }.width('100%').height('100%')
      }
      if (this.showContour) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.showContour = false; })
          Column({ space: 12 }) {
            Text(`等高线 · ${this.ctLabel()}`).fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            Canvas(this.contourCtx)
              .width(220).height(150)
              .onReady((): void => this.drawContour())
              .onTouch((e: TouchEvent): void => this.ctTouch(e))
            Text('拖点调整 · 空白单击加点 · 选中后「角点」切换折角')
              .fontSize(10).fontColor($r('app.color.text_secondary'))
            Row({ space: 8 }) {
              Button('线性').fontSize(12).height(32)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => { this.ctResetLinear(); })
              Button('角点').fontSize(12).height(32)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => { this.ctToggleCorner(); })
              Button('删除点').fontSize(12).height(32)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.danger'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => { this.ctDeletePoint(); })
            }
            if (this.ctTarget === 2 || this.ctTarget === 3) {
              Row({ space: 10 }) {
                Text('范围').fontSize(13).fontColor($r('app.color.text_secondary')).width(52)
                Slider({ value: this.ctRange, min: 1, max: 100, step: 1 })
                  .layoutWeight(1)
                  .onChange((v: number, mode: SliderChangeMode): void => {
                    if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                      this.ctRange = v;
                      if (this.ctTarget === 2) {
                        this.styOgRange = v;
                      } else if (this.ctTarget === 3) {
                        this.styIgRange = v;
                      }
                      this.ctCommit();
                      this.drawContour();
                    }
                  })
                Text(`${Math.round(this.ctRange)}%`).fontSize(12).width(44)
                  .textAlign(TextAlign.End).fontColor($r('app.color.text_primary'))
              }
            }
            Row({ space: 12 }) {
              Button('关闭').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => { this.showContour = false; })
            }.justifyContent(FlexAlign.End).width('100%')
          }.padding(20).borderRadius(14).backgroundColor($r('app.color.surface'))
            .border({ width: 1, color: $r('app.color.border') }).width(400)
        }.width('100%').height('100%')
      }
      if (this.showFill) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.showFill = false; })
          Column({ space: 14 }) {
            Text('填充').fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            Text('选区约束生效（无选区 = 整层）').fontSize(12)
              .fontColor($r('app.color.text_secondary'))
            Row({ space: 8 }) {
              ForEach(['前景色', '背景色', '黑色', '白色'], (label: string, idx: number) => {
                Button(label)
                  .fontSize(12).height(32)
                  .backgroundColor(this.fillSource === idx
                    ? $r('app.color.accent_container') : $r('app.color.surface'))
                  .fontColor(this.fillSource === idx
                    ? $r('app.color.accent') : $r('app.color.text_primary'))
                  .border({
                    width: this.fillSource === idx ? 1.5 : 1,
                    color: this.fillSource === idx ? $r('app.color.accent') : $r('app.color.border')
                  })
                  .onClick((): void => {
                    this.fillSource = idx;
                  })
              }, (label: string): string => label)
            }
            Row({ space: 12 }) {
              Button('取消').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => { this.showFill = false; })
              Button('填充').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => this.commitFill())
            }.justifyContent(FlexAlign.End).width('100%')
          }.padding(20).borderRadius(14).backgroundColor($r('app.color.surface'))
            .border({ width: 1, color: $r('app.color.border') }).width(420)
        }.width('100%').height('100%')
      }
      if (this.showStyle) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.showStyle = false; })
          Scroll() {
            Column({ space: 12 }) {
              Text('图层样式').fontSize(16).fontWeight(FontWeight.Bold)
                .fontColor($r('app.color.text_primary'))
              // 填充层参数段（活动层为填充层时）
              if (this.activeLayer()?.isFill === true) {
                Column({ space: 8 }) {
                  Text('填充层').fontSize(13).fontWeight(FontWeight.Medium)
                    .fontColor($r('app.color.text_primary'))
                  Row({ space: 8 }) {
                    Button('纯色')
                      .fontSize(12).height(30)
                      .backgroundColor(this.fillKind === 0
                        ? $r('app.color.accent_container') : $r('app.color.surface'))
                      .fontColor(this.fillKind === 0
                        ? $r('app.color.accent') : $r('app.color.text_primary'))
                      .onClick((): void => { this.fillKind = 0; this.applyFillParams(); })
                    Button('线性渐变')
                      .fontSize(12).height(30)
                      .backgroundColor(this.fillKind === 1
                        ? $r('app.color.accent_container') : $r('app.color.surface'))
                      .fontColor(this.fillKind === 1
                        ? $r('app.color.accent') : $r('app.color.text_primary'))
                      .onClick((): void => { this.fillKind = 1; this.applyFillParams(); })
                  }
                  Row({ space: 6 }) {
                    Text('颜色').fontSize(12).fontColor($r('app.color.text_secondary'))
                    ForEach(SWATCHES, (sw: string) => {
                      Column()
                        .width(22).height(22).borderRadius(4)
                        .backgroundColor(sw)
                        .border({
                          width: this.fillColor === sw ? 2 : 1,
                          color: this.fillColor === sw
                            ? $r('app.color.accent') : $r('app.color.border')
                        })
                        .onClick((): void => { this.fillColor = sw; this.applyFillParams(); })
                    }, (sw: string): string => sw)
                  }
                  if (this.fillKind === 1) {
                    Row({ space: 6 }) {
                      Text('角度').fontSize(12).fontColor($r('app.color.text_secondary'))
                      Slider({ value: this.fillAngle, min: 0, max: 360, step: 5 })
                        .layoutWeight(1).height(24)
                        .onChange((v: number, mode: SliderChangeMode): void => {
                          if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                            this.fillAngle = v;
                            this.applyFillParams();
                          }
                        })
                      Text(`${Math.round(this.fillAngle)}°`).fontSize(11).width(36)
                        .fontColor($r('app.color.text_primary'))
                    }.width('100%')
                  }
                }
                .width('100%').padding(10).borderRadius(8)
                .backgroundColor($r('app.color.accent_container'))
              }
              // 投影
              Column({ space: 6 }) {
                Row({ space: 8 }) {
                  Toggle({ type: ToggleType.Checkbox, isOn: this.styShadowOn })
                    .onChange((on: boolean): void => {
                      this.styShadowOn = on;
                      this.applyDropShadow();
                    })
                  Text('投影').fontSize(13).fontColor($r('app.color.text_primary'))
                }
                Row({ space: 6 }) {
                  Text('扩展').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styShadowSpread, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styShadowSpread = v;
                        this.applyDropShadow();
                      }
                    })
                  Text(`${Math.round(this.styShadowSpread)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('全局光').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Toggle({ type: ToggleType.Checkbox, isOn: this.styShadowGL })
                    .onChange((on: boolean): void => {
                      this.styShadowGL = on;
                      this.applyDropShadow();
                    })
                }.width('100%')
                Row({ space: 6 }) {
                  Text('角度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styAngle, min: 0, max: 360, step: 5 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styAngle = v;
                        this.applyDropShadow();
                      }
                    })
                  Text(`${Math.round(this.styAngle)}°`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('距离').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styDist, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styDist = v;
                        this.applyDropShadow();
                      }
                    })
                  Text(`${Math.round(this.styDist)}`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('大小').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.stySize, min: 0, max: 50, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.stySize = v;
                        this.applyDropShadow();
                      }
                    })
                  Text(`${Math.round(this.stySize)}`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('不透明度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styShadowOp, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styShadowOp = v;
                        this.applyDropShadow();
                      }
                    })
                  Text(`${Math.round(this.styShadowOp)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('等高线').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Blank()
                  Button('编辑…').fontSize(11).height(24)
                    .backgroundColor($r('app.color.surface'))
                    .fontColor($r('app.color.accent'))
                    .border({ width: 1, color: $r('app.color.border') })
                    .onClick((): void => { this.openContour(0); })
                }.width('100%')
                Row({ space: 6 }) {
                  Text('颜色').fontSize(12).fontColor($r('app.color.text_secondary'))
                  ForEach(SWATCHES, (sw: string) => {
                    Column()
                      .width(22).height(22).borderRadius(4)
                      .backgroundColor(sw)
                      .border({
                        width: this.styShadowColor === sw ? 2 : 1,
                        color: this.styShadowColor === sw
                          ? $r('app.color.accent') : $r('app.color.border')
                      })
                      .onClick((): void => { this.styShadowColor = sw; this.applyDropShadow(); })
                  }, (sw: string): string => sw)
                }
              }
              .width('100%').padding(10).borderRadius(8)
              .backgroundColor($r('app.color.surface'))
              .border({ width: 1, color: $r('app.color.border') })
              // 描边
              Column({ space: 6 }) {
                Row({ space: 8 }) {
                  Toggle({ type: ToggleType.Checkbox, isOn: this.styStrokeOn })
                    .onChange((on: boolean): void => {
                      this.styStrokeOn = on;
                      this.applyStroke();
                    })
                  Text('描边（外部）').fontSize(13).fontColor($r('app.color.text_primary'))
                }
                Row({ space: 6 }) {
                  Text('宽度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styStrokeW, min: 1, max: 30, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styStrokeW = v;
                        this.applyStroke();
                      }
                    })
                  Text(`${Math.round(this.styStrokeW)}px`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('颜色').fontSize(12).fontColor($r('app.color.text_secondary'))
                  ForEach(SWATCHES, (sw: string) => {
                    Column()
                      .width(22).height(22).borderRadius(4)
                      .backgroundColor(sw)
                      .border({
                        width: this.styStrokeColor === sw ? 2 : 1,
                        color: this.styStrokeColor === sw
                          ? $r('app.color.accent') : $r('app.color.border')
                      })
                      .onClick((): void => { this.styStrokeColor = sw; this.applyStroke(); })
                  }, (sw: string): string => sw)
                }
              }
              .width('100%').padding(10).borderRadius(8)
              .backgroundColor($r('app.color.surface'))
              .border({ width: 1, color: $r('app.color.border') })
              // 颜色叠加
              Column({ space: 6 }) {
                Row({ space: 8 }) {
                  Toggle({ type: ToggleType.Checkbox, isOn: this.styOverlayOn })
                    .onChange((on: boolean): void => {
                      this.styOverlayOn = on;
                      this.applyOverlay();
                    })
                  Text('颜色叠加').fontSize(13).fontColor($r('app.color.text_primary'))
                }
                Row({ space: 6 }) {
                  Text('不透明度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styOverlayOp, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styOverlayOp = v;
                        this.applyOverlay();
                      }
                    })
                  Text(`${Math.round(this.styOverlayOp)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('颜色').fontSize(12).fontColor($r('app.color.text_secondary'))
                  ForEach(SWATCHES, (sw: string) => {
                    Column()
                      .width(22).height(22).borderRadius(4)
                      .backgroundColor(sw)
                      .border({
                        width: this.styOverlayColor === sw ? 2 : 1,
                        color: this.styOverlayColor === sw
                          ? $r('app.color.accent') : $r('app.color.border')
                      })
                      .onClick((): void => { this.styOverlayColor = sw; this.applyOverlay(); })
                  }, (sw: string): string => sw)
                }
              }
              .width('100%').padding(10).borderRadius(8)
              .backgroundColor($r('app.color.surface'))
              .border({ width: 1, color: $r('app.color.border') })
              // M17a 内阴影
              Column({ space: 6 }) {
                Row({ space: 8 }) {
                  Toggle({ type: ToggleType.Checkbox, isOn: this.styInnerShadowOn })
                    .onChange((on: boolean): void => {
                      this.styInnerShadowOn = on;
                      this.applyInnerShadow();
                    })
                  Text('内阴影').fontSize(13).fontColor($r('app.color.text_primary'))
                }
                Row({ space: 6 }) {
                  Text('扩展').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styIsSpread, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styIsSpread = v;
                        this.applyInnerShadow();
                      }
                    })
                  Text(`${Math.round(this.styIsSpread)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('全局光').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Toggle({ type: ToggleType.Checkbox, isOn: this.styIsGL })
                    .onChange((on: boolean): void => {
                      this.styIsGL = on;
                      this.applyInnerShadow();
                    })
                }.width('100%')
                Row({ space: 6 }) {
                  Text('角度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styIsAngle, min: 0, max: 360, step: 5 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styIsAngle = v;
                        this.applyInnerShadow();
                      }
                    })
                  Text(`${Math.round(this.styIsAngle)}°`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('距离').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styIsDist, min: 0, max: 60, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styIsDist = v;
                        this.applyInnerShadow();
                      }
                    })
                  Text(`${Math.round(this.styIsDist)}`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('大小').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styIsSize, min: 0, max: 50, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styIsSize = v;
                        this.applyInnerShadow();
                      }
                    })
                  Text(`${Math.round(this.styIsSize)}`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('不透明度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styIsOp, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styIsOp = v;
                        this.applyInnerShadow();
                      }
                    })
                  Text(`${Math.round(this.styIsOp)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('等高线').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Blank()
                  Button('编辑…').fontSize(11).height(24)
                    .backgroundColor($r('app.color.surface'))
                    .fontColor($r('app.color.accent'))
                    .border({ width: 1, color: $r('app.color.border') })
                    .onClick((): void => { this.openContour(1); })
                }.width('100%')
                Row({ space: 6 }) {
                  Text('颜色').fontSize(12).fontColor($r('app.color.text_secondary'))
                  ForEach(SWATCHES, (sw: string) => {
                    Column()
                      .width(22).height(22).borderRadius(4)
                      .backgroundColor(sw)
                      .border({
                        width: this.styIsColor === sw ? 2 : 1,
                        color: this.styIsColor === sw
                          ? $r('app.color.accent') : $r('app.color.border')
                      })
                      .onClick((): void => { this.styIsColor = sw; this.applyInnerShadow(); })
                  }, (sw: string): string => sw)
                }
              }
              .width('100%').padding(10).borderRadius(8)
              .backgroundColor($r('app.color.surface'))
              .border({ width: 1, color: $r('app.color.border') })
              // M17a 外发光 / 内发光（同构段）
              Column({ space: 6 }) {
                Row({ space: 8 }) {
                  Toggle({ type: ToggleType.Checkbox, isOn: this.styOuterGlowOn })
                    .onChange((on: boolean): void => {
                      this.styOuterGlowOn = on;
                      this.applyOuterGlow();
                    })
                  Text('外发光').fontSize(13).fontColor($r('app.color.text_primary'))
                }
                Row({ space: 6 }) {
                  Text('扩展').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styOgSpread, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styOgSpread = v;
                        this.applyOuterGlow();
                      }
                    })
                  Text(`${Math.round(this.styOgSpread)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('大小').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styOgSize, min: 0, max: 50, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styOgSize = v;
                        this.applyOuterGlow();
                      }
                    })
                  Text(`${Math.round(this.styOgSize)}`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('不透明度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styOgOp, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styOgOp = v;
                        this.applyOuterGlow();
                      }
                    })
                  Text(`${Math.round(this.styOgOp)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('范围').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styOgRange, min: 1, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styOgRange = v;
                        this.applyGlowRange(2, v);
                      }
                    })
                  Text(`${Math.round(this.styOgRange)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('等高线').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Blank()
                  Button('编辑…').fontSize(11).height(24)
                    .backgroundColor($r('app.color.surface'))
                    .fontColor($r('app.color.accent'))
                    .border({ width: 1, color: $r('app.color.border') })
                    .onClick((): void => { this.openContour(2); })
                }.width('100%')
                Row({ space: 6 }) {
                  Text('颜色').fontSize(12).fontColor($r('app.color.text_secondary'))
                  ForEach(SWATCHES, (sw: string) => {
                    Column()
                      .width(22).height(22).borderRadius(4)
                      .backgroundColor(sw)
                      .border({
                        width: this.styOgColor === sw ? 2 : 1,
                        color: this.styOgColor === sw
                          ? $r('app.color.accent') : $r('app.color.border')
                      })
                      .onClick((): void => { this.styOgColor = sw; this.applyOuterGlow(); })
                  }, (sw: string): string => sw)
                }
              }
              .width('100%').padding(10).borderRadius(8)
              .backgroundColor($r('app.color.surface'))
              .border({ width: 1, color: $r('app.color.border') })
              Column({ space: 6 }) {
                Row({ space: 8 }) {
                  Toggle({ type: ToggleType.Checkbox, isOn: this.styInnerGlowOn })
                    .onChange((on: boolean): void => {
                      this.styInnerGlowOn = on;
                      this.applyInnerGlow();
                    })
                  Text('内发光').fontSize(13).fontColor($r('app.color.text_primary'))
                }
                Row({ space: 6 }) {
                  Text('扩展').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styIgSpread, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styIgSpread = v;
                        this.applyInnerGlow();
                      }
                    })
                  Text(`${Math.round(this.styIgSpread)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('大小').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styIgSize, min: 0, max: 50, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styIgSize = v;
                        this.applyInnerGlow();
                      }
                    })
                  Text(`${Math.round(this.styIgSize)}`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('不透明度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styIgOp, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styIgOp = v;
                        this.applyInnerGlow();
                      }
                    })
                  Text(`${Math.round(this.styIgOp)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('范围').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styIgRange, min: 1, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styIgRange = v;
                        this.applyGlowRange(3, v);
                      }
                    })
                  Text(`${Math.round(this.styIgRange)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('等高线').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Blank()
                  Button('编辑…').fontSize(11).height(24)
                    .backgroundColor($r('app.color.surface'))
                    .fontColor($r('app.color.accent'))
                    .border({ width: 1, color: $r('app.color.border') })
                    .onClick((): void => { this.openContour(3); })
                }.width('100%')
                Row({ space: 6 }) {
                  Text('颜色').fontSize(12).fontColor($r('app.color.text_secondary'))
                  ForEach(SWATCHES, (sw: string) => {
                    Column()
                      .width(22).height(22).borderRadius(4)
                      .backgroundColor(sw)
                      .border({
                        width: this.styIgColor === sw ? 2 : 1,
                        color: this.styIgColor === sw
                          ? $r('app.color.accent') : $r('app.color.border')
                      })
                      .onClick((): void => { this.styIgColor = sw; this.applyInnerGlow(); })
                  }, (sw: string): string => sw)
                }
              }
              .width('100%').padding(10).borderRadius(8)
              .backgroundColor($r('app.color.surface'))
              .border({ width: 1, color: $r('app.color.border') })
              // M17a 渐变叠加（v1 双色线性）
              Column({ space: 6 }) {
                Row({ space: 8 }) {
                  Toggle({ type: ToggleType.Checkbox, isOn: this.styGradientOn })
                    .onChange((on: boolean): void => {
                      this.styGradientOn = on;
                      this.applyGradientOverlay();
                    })
                  Text('渐变叠加').fontSize(13).fontColor($r('app.color.text_primary'))
                }
                Row({ space: 6 }) {
                  Text('角度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styGdAngle, min: 0, max: 360, step: 5 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styGdAngle = v;
                        this.applyGradientOverlay();
                      }
                    })
                  Text(`${Math.round(this.styGdAngle)}°`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('不透明度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styGdOp, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styGdOp = v;
                        this.applyGradientOverlay();
                      }
                    })
                  Text(`${Math.round(this.styGdOp)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('起色').fontSize(12).fontColor($r('app.color.text_secondary'))
                  ForEach(SWATCHES, (sw: string) => {
                    Column()
                      .width(22).height(22).borderRadius(4)
                      .backgroundColor(sw)
                      .border({
                        width: this.styGdFrom === sw ? 2 : 1,
                        color: this.styGdFrom === sw
                          ? $r('app.color.accent') : $r('app.color.border')
                      })
                      .onClick((): void => { this.styGdFrom = sw; this.applyGradientOverlay(); })
                  }, (sw: string): string => sw)
                }.width('100%')
                Row({ space: 6 }) {
                  Text('止色').fontSize(12).fontColor($r('app.color.text_secondary'))
                  ForEach(SWATCHES, (sw: string) => {
                    Column()
                      .width(22).height(22).borderRadius(4)
                      .backgroundColor(sw)
                      .border({
                        width: this.styGdTo === sw ? 2 : 1,
                        color: this.styGdTo === sw
                          ? $r('app.color.accent') : $r('app.color.border')
                      })
                      .onClick((): void => { this.styGdTo = sw; this.applyGradientOverlay(); })
                  }, (sw: string): string => sw)
                }
              }
              .width('100%').padding(10).borderRadius(8)
              .backgroundColor($r('app.color.surface'))
              .border({ width: 1, color: $r('app.color.border') })
              // M17b 光泽
              Column({ space: 6 }) {
                Row({ space: 8 }) {
                  Toggle({ type: ToggleType.Checkbox, isOn: this.stySatinOn })
                    .onChange((on: boolean): void => {
                      this.stySatinOn = on;
                      this.applySatin();
                    })
                  Text('光泽').fontSize(13).fontColor($r('app.color.text_primary'))
                  Toggle({ type: ToggleType.Checkbox, isOn: this.stySatinInvert })
                    .onChange((on: boolean): void => {
                      this.stySatinInvert = on;
                      this.applySatin();
                    })
                  Text('反相').fontSize(11).fontColor($r('app.color.text_secondary'))
                }
                Row({ space: 6 }) {
                  Text('角度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.stySatinAngle, min: 0, max: 360, step: 5 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.stySatinAngle = v;
                        this.applySatin();
                      }
                    })
                  Text(`${Math.round(this.stySatinAngle)}°`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('距离').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.stySatinDist, min: 0, max: 60, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.stySatinDist = v;
                        this.applySatin();
                      }
                    })
                  Text(`${Math.round(this.stySatinDist)}`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('大小').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.stySatinSize, min: 0, max: 50, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.stySatinSize = v;
                        this.applySatin();
                      }
                    })
                  Text(`${Math.round(this.stySatinSize)}`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('不透明度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.stySatinOp, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.stySatinOp = v;
                        this.applySatin();
                      }
                    })
                  Text(`${Math.round(this.stySatinOp)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('等高线').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Blank()
                  Button('编辑…').fontSize(11).height(24)
                    .backgroundColor($r('app.color.surface'))
                    .fontColor($r('app.color.accent'))
                    .border({ width: 1, color: $r('app.color.border') })
                    .onClick((): void => { this.openContour(4); })
                }.width('100%')
                Row({ space: 6 }) {
                  Text('颜色').fontSize(12).fontColor($r('app.color.text_secondary'))
                  ForEach(SWATCHES, (sw: string) => {
                    Column()
                      .width(22).height(22).borderRadius(4)
                      .backgroundColor(sw)
                      .border({
                        width: this.stySatinColor === sw ? 2 : 1,
                        color: this.stySatinColor === sw
                          ? $r('app.color.accent') : $r('app.color.border')
                      })
                      .onClick((): void => { this.stySatinColor = sw; this.applySatin(); })
                  }, (sw: string): string => sw)
                }
              }
              .width('100%').padding(10).borderRadius(8)
              .backgroundColor($r('app.color.surface'))
              .border({ width: 1, color: $r('app.color.border') })
              // M17b 斜面浮雕
              Column({ space: 6 }) {
                Row({ space: 8 }) {
                  Toggle({ type: ToggleType.Checkbox, isOn: this.styBevelOn })
                    .onChange((on: boolean): void => {
                      this.styBevelOn = on;
                      this.applyBevel();
                    })
                  Text('斜面浮雕').fontSize(13).fontColor($r('app.color.text_primary'))
                  Button(this.styBevelOuter ? '外斜面' : '内斜面')
                    .fontSize(11).height(26)
                    .backgroundColor($r('app.color.accent_container'))
                    .fontColor($r('app.color.accent'))
                    .onClick((): void => {
                      this.styBevelOuter = !this.styBevelOuter;
                      this.applyBevel();
                    })
                  Button(this.styBevelUp ? '向上' : '向下')
                    .fontSize(11).height(26)
                    .backgroundColor($r('app.color.accent_container'))
                    .fontColor($r('app.color.accent'))
                    .onClick((): void => {
                      this.styBevelUp = !this.styBevelUp;
                      this.applyBevel();
                    })
                }
                Row({ space: 6 }) {
                  Text('深度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styBevelDepth * 10, min: 10, max: 100, step: 5 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styBevelDepth = v / 10;
                        this.applyBevel();
                      }
                    })
                  Text(`${Math.round(this.styBevelDepth * 100)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('大小').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styBevelSize, min: 0, max: 50, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styBevelSize = v;
                        this.applyBevel();
                      }
                    })
                  Text(`${Math.round(this.styBevelSize)}`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('角度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styBevelAngle, min: 0, max: 360, step: 5 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styBevelAngle = v;
                        this.applyBevel();
                      }
                    })
                  Text(`${Math.round(this.styBevelAngle)}°`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('高度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styBevelAlt, min: 1, max: 89, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styBevelAlt = v;
                        this.applyBevel();
                      }
                    })
                  Text(`${Math.round(this.styBevelAlt)}°`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('高光').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styBevelHiOp, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styBevelHiOp = v;
                        this.applyBevel();
                      }
                    })
                  Text(`${Math.round(this.styBevelHiOp)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                  ForEach(SWATCHES, (sw: string) => {
                    Column()
                      .width(22).height(22).borderRadius(4)
                      .backgroundColor(sw)
                      .border({
                        width: this.styBevelHiColor === sw ? 2 : 1,
                        color: this.styBevelHiColor === sw
                          ? $r('app.color.accent') : $r('app.color.border')
                      })
                      .onClick((): void => { this.styBevelHiColor = sw; this.applyBevel(); })
                  }, (sw: string): string => sw)
                }.width('100%')
                Row({ space: 6 }) {
                  Text('暗影').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styBevelShOp, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styBevelShOp = v;
                        this.applyBevel();
                      }
                    })
                  Text(`${Math.round(this.styBevelShOp)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                  ForEach(SWATCHES, (sw: string) => {
                    Column()
                      .width(22).height(22).borderRadius(4)
                      .backgroundColor(sw)
                      .border({
                        width: this.styBevelShColor === sw ? 2 : 1,
                        color: this.styBevelShColor === sw
                          ? $r('app.color.accent') : $r('app.color.border')
                      })
                      .onClick((): void => { this.styBevelShColor = sw; this.applyBevel(); })
                  }, (sw: string): string => sw)
                }.width('100%')
                Row({ space: 6 }) {
                  Text('等高线').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Blank()
                  Button('编辑…').fontSize(11).height(24)
                    .backgroundColor($r('app.color.surface'))
                    .fontColor($r('app.color.accent'))
                    .border({ width: 1, color: $r('app.color.border') })
                    .onClick((): void => { this.openContour(5); })
                }.width('100%')
                Row({ space: 6 }) {
                  Text('等高线').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Blank()
                  Button('编辑…').fontSize(11).height(24)
                    .backgroundColor($r('app.color.surface'))
                    .fontColor($r('app.color.accent'))
                    .border({ width: 1, color: $r('app.color.border') })
                    .onClick((): void => { this.openContour(6); })
                }.width('100%')
              }
              .width('100%').padding(10).borderRadius(8)
              .backgroundColor($r('app.color.surface'))
              .border({ width: 1, color: $r('app.color.border') })
              // M17d 图案叠加（图案来源 = 编辑菜单「定义图案」；渲染序 = 渐变后颜色叠加前）
              Column({ space: 6 }) {
                Row({ space: 8 }) {
                  Toggle({ type: ToggleType.Checkbox, isOn: this.styPatternOn })
                    .onChange((on: boolean): void => {
                      this.styPatternOn = on;
                      this.applyPatternOverlay();
                    })
                  Text('图案叠加').fontSize(13).fontColor($r('app.color.text_primary'))
                }
                Row({ space: 6 }) {
                  Text('图案').fontSize(12).fontColor($r('app.color.text_secondary'))
                  if (this.patternList.length === 0) {
                    Text('无图案：编辑菜单 → 定义图案').fontSize(11)
                      .fontColor($r('app.color.text_secondary'))
                  } else {
                    Flex({ wrap: FlexWrap.Wrap }) {
                      ForEach(this.patternList, (pt: PatternInfo) => {
                        Text(pt.name)
                          .fontSize(10)
                          .fontColor(this.styPatternId === pt.id
                            ? $r('app.color.on_accent') : $r('app.color.text_primary'))
                          .backgroundColor(this.styPatternId === pt.id
                            ? $r('app.color.accent') : $r('app.color.surface'))
                          .borderRadius(6)
                          .padding({ left: 8, right: 8, top: 4, bottom: 4 })
                          .margin(2)
                          .onClick((): void => {
                            this.styPatternId = pt.id;
                            this.styPatternOn = true;
                            this.applyPatternOverlay();
                          })
                      }, (pt: PatternInfo): string => pt.id)
                    }.layoutWeight(1)
                  }
                }.width('100%')
                Row({ space: 6 }) {
                  Text('缩放').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styPatternScale, min: 25, max: 500, step: 5 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styPatternScale = v;
                        this.applyPatternOverlay();
                      }
                    })
                  Text(`${Math.round(this.styPatternScale)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
                Row({ space: 6 }) {
                  Text('不透明度').fontSize(12).fontColor($r('app.color.text_secondary'))
                  Slider({ value: this.styPatternOp, min: 0, max: 100, step: 1 })
                    .layoutWeight(1).height(24)
                    .onChange((v: number, mode: SliderChangeMode): void => {
                      if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                        this.styPatternOp = v;
                        this.applyPatternOverlay();
                      }
                    })
                  Text(`${Math.round(this.styPatternOp)}%`).fontSize(11).width(36)
                    .fontColor($r('app.color.text_primary'))
                }.width('100%')
              }
              .width('100%').padding(10).borderRadius(8)
              .backgroundColor($r('app.color.surface'))
              .border({ width: 1, color: $r('app.color.border') })
              Row({ space: 12 }) {
                Button('清除样式').fontSize(14).height(40).width(110)
                  .backgroundColor($r('app.color.surface'))
                  .fontColor($r('app.color.danger'))
                  .border({ width: 1, color: $r('app.color.border') })
                  .onClick((): void => {
                    const a = this.activeLayer();
                    if (a !== undefined && this.engine.clearLayerStyle(a.id)) {
                      this.toast('样式已清除');
                      this.showStyle = false;
                    }
                  })
                Blank()
                Button('关闭').fontSize(14).height(40).width(96)
                  .backgroundColor($r('app.color.accent'))
                  .fontColor($r('app.color.on_accent'))
                  .onClick((): void => { this.showStyle = false; })
              }.justifyContent(FlexAlign.End).width('100%')
              Text('滑条实时生效（一步撤销）· Esc 关闭').fontSize(11)
                .fontColor($r('app.color.text_secondary'))
            }.padding(20)
          }.width(440).height(520)
        }.width('100%').height('100%')
      }
      if (this.showStoreSel) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.showStoreSel = false; })
          Column({ space: 14 }) {
            Text('存储选区为 Alpha 通道').fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            Row({ space: 10 }) {
              Text('名称').fontSize(13).fontColor($r('app.color.text_secondary'))
              TextInput({ text: this.storeSelText }).width(180).height(40)
                .onChange((v: string): void => { this.storeSelText = v; })
            }
            Row({ space: 12 }) {
              Button('取消').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => { this.showStoreSel = false; })
              Button('存储').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => this.commitStoreSel())
            }.justifyContent(FlexAlign.End).width('100%')
          }.padding(20).borderRadius(14).backgroundColor($r('app.color.surface'))
            .border({ width: 1, color: $r('app.color.border') }).width(360)
        }.width('100%').height('100%')
      }
      if (this.showLoadSel) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.showLoadSel = false; })
          Column({ space: 14 }) {
            Text('载入选区（输入通道名）').fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            Row({ space: 10 }) {
              Text('名称').fontSize(13).fontColor($r('app.color.text_secondary'))
              TextInput({ text: this.loadSelText }).width(180).height(40)
                .onChange((v: string): void => { this.loadSelText = v; })
            }
            Row({ space: 12 }) {
              Button('取消').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => { this.showLoadSel = false; })
              Button('载入').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => this.commitLoadSel())
            }.justifyContent(FlexAlign.End).width('100%')
          }.padding(20).borderRadius(14).backgroundColor($r('app.color.surface'))
            .border({ width: 1, color: $r('app.color.border') }).width(360)
        }.width('100%').height('100%')
      }
      if (this.showRotateAny) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.showRotateAny = false; })
          Column({ space: 14 }) {
            Text('任意角度旋转（顺时针）').fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            Row({ space: 10 }) {
              Text('角度').fontSize(13).fontColor($r('app.color.text_secondary'))
              TextInput({ text: this.angleText }).width(120).height(40)
                .onChange((v: string): void => { this.angleText = v; })
              Text('度').fontSize(13).fontColor($r('app.color.text_secondary'))
            }
            Row({ space: 12 }) {
              Button('取消').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => { this.showRotateAny = false; })
              Button('确定').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => this.commitRotateAny())
            }.justifyContent(FlexAlign.End).width('100%')
          }.padding(20).borderRadius(14).backgroundColor($r('app.color.surface'))
            .border({ width: 1, color: $r('app.color.border') }).width(400)
        }.width('100%').height('100%')
      }
            if (this.showTransform) {
        Stack() {
          Column().width('100%').height('100%').backgroundColor('#66000000')
            .onClick((): void => { this.showTransform = false; })
          Column({ space: 14 }) {
            Text('自由变换（围绕画布中心）').fontSize(16).fontWeight(FontWeight.Bold)
              .fontColor($r('app.color.text_primary'))
            Row({ space: 10 }) {
              Text('缩放%').fontSize(13).fontColor($r('app.color.text_secondary'))
              TextInput({ text: this.tfScale.toString() }).width(90).height(40)
                .onChange((v: string): void => { this.tfScale = parseFloat(v) || 0; })
              Text('旋转°').fontSize(13).fontColor($r('app.color.text_secondary'))
              TextInput({ text: this.tfRotate.toString() }).width(90).height(40)
                .onChange((v: string): void => { this.tfRotate = parseFloat(v) || 0; })
              Text('斜切°').fontSize(13).fontColor($r('app.color.text_secondary'))
              TextInput({ text: this.tfSkew.toString() }).width(90).height(40)
                .onChange((v: string): void => { this.tfSkew = parseFloat(v) || 0; })
            }
            Text('确认后烘焙进图层（一步撤销）。Esc 取消。')
              .fontSize(11).fontColor($r('app.color.text_secondary'))
            Row({ space: 12 }) {
              Button('取消').fontSize(14).height(40).width(96)
                .backgroundColor($r('app.color.surface'))
                .fontColor($r('app.color.text_primary'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => { this.showTransform = false; })
              Button('应用变换').fontSize(14).height(40).width(120)
                .backgroundColor($r('app.color.accent'))
                .fontColor($r('app.color.on_accent'))
                .onClick((): void => this.commitTransform())
            }.justifyContent(FlexAlign.End).width('100%')
          }.padding(20).borderRadius(14).backgroundColor($r('app.color.surface'))
            .border({ width: 1, color: $r('app.color.border') }).width(480)
        }.width('100%').height('100%')
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
