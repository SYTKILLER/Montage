/**
 * M24 自动化内核：H1-lite 命令注册表 + 动作（Actions）录制/回放/管理。
 * 语义对拍 photocraft crates/engine/src/actions_cmds.rs（2026-10-24 快照）：
 * - 录制 = 引擎包装层成功路径 note() 收集可重放条目（journal 复制）；
 * - 回放 = 逐 enabled 步注册表 apply，首错即停，每步一历史事务；
 * - 嵌套 play ≤16 层、按名循环检测、播放中挂起录制并回填 actions.play 命名调用；
 * - 管理命令 list/get/record/stop/delete/move/toggle/rename（含 moved_index 联动）。
 * 排除（不可重放）：文件开存导出（fd 瞬态）、undo/redo、视图/工具/会话原语。
 * P4 增强：交互笔画录制（paint.brushStroke 点列步骤化+回放重放）；动作 F 键绑定
 *（F2..F12 全局唯一，持久化 v2）；replayStroke 等待笔画线程落账（strokeBusy 轮询）。
 * 持久化：actions JSON 存 Preferences（Index 接线）；C++ 零改动。
 */

import { hilog } from '@kit.PerformanceAnalysisKit';
import { CompositorEngine } from './CompositorEngine';
import { LiquifyStrokeDTO, PuppetPinDTO } from '../model/EngineTypes';

const TAG: string = 'Montage.Automation';
const DOMAIN: number = 0x4D30;

/** 一条可重放命令步（photocraft Action.steps 同构 [[id, params]…]）。 */
export interface ActionStep {
  id: string;
  params: Record<string, Object>;
}

/** 一个动作：步列表 + 回放跳过索引（面板勾选关）+ F 键绑定（0=无，2..12）。 */
export interface Action {
  name: string;
  steps: ActionStep[];
  disabled: number[];
  fkey: number;
}

/** 回放失败步描述。 */
export interface PlayFailure {
  step: number;
  id: string;
  error: string;
}

/** 回放结果（photocraft play 返回同构）。 */
export interface PlayResult {
  action: string;
  ran: number;
  failed?: PlayFailure;
}

/** 管理命令/execute 的结构化返回（控制通道与面板共用）。 */
export interface AutoResult {
  ok: boolean;
  error: string;
  value?: Object;
}

/** 注册表条目：apply 返回 false = 该步失败（回放停）。 */
interface CmdEntry {
  id: string;
  label: string;
  coalesce: boolean;
  apply: (e: CompositorEngine, p: Record<string, Object>) => boolean | Promise<boolean>;
}

// ---------- 参数读取小帮手（Record bracket 索引纪律） ----------

function pn(p: Record<string, Object>, key: string, def: number): number {
  const v: Object = p[key];
  return typeof v === 'number' ? v : def;
}

function pb(p: Record<string, Object>, key: string, def: boolean): boolean {
  const v: Object = p[key];
  return typeof v === 'boolean' ? v : def;
}

function ps(p: Record<string, Object>, key: string, def: string): string {
  const v: Object = p[key];
  return typeof v === 'string' ? v : def;
}

function pobjArr(p: Record<string, Object>, key: string): Object[] {
  const v: Object = p[key];
  if (v === undefined || v === null) {
    return [];
  }
  return v as Object as Object[];
}

function pna(p: Record<string, Object>, key: string): number[] {
  const v: Object = p[key];
  if (v === undefined || v === null) {
    return [];
  }
  const arr = v as Object as Object[];
  const out: number[] = [];
  for (const x of arr) {
    if (typeof x === 'number') {
      out.push(x);
    }
  }
  return out;
}

// ---------- 注册表（H1-lite 单一事实来源） ----------

function buildRegistry(): Map<string, CmdEntry> {
  const t: CmdEntry[] = [
    // 文档
    { id: 'file.new', label: '新建文档', coalesce: false,
      apply: (e, p) => e.newDocument(pn(p, 'width', 0), pn(p, 'height', 0)) },
    // 图层族
    { id: 'layer.add', label: '新建图层', coalesce: false,
      apply: (e, p) => e.addLayer(ps(p, 'name', '')) >= 0 },
    { id: 'layer.remove', label: '删除图层', coalesce: false,
      apply: (e, p) => e.removeLayer(pn(p, 'id', 0)) },
    { id: 'layer.select', label: '选择图层', coalesce: false,
      apply: (e, p) => e.selectLayer(pn(p, 'id', 0)) },
    { id: 'layer.visible', label: '图层显隐', coalesce: false,
      apply: (e, p) => e.setLayerVisible(pn(p, 'id', 0), pb(p, 'v', true)) },
    { id: 'layer.opacity', label: '图层不透明度', coalesce: true,
      apply: (e, p) => e.setLayerOpacity(pn(p, 'id', 0), pn(p, 'v', 1)) },
    { id: 'layer.blend', label: '图层混合模式', coalesce: false,
      apply: (e, p) => e.setLayerBlendMode(pn(p, 'id', 0), pn(p, 'mode', 0)) },
    { id: 'layer.maskEnable', label: '蒙版启停', coalesce: false,
      apply: (e, p) => e.setLayerMaskEnabled(pn(p, 'id', 0), pb(p, 'v', true)) },
    { id: 'layer.clip', label: '剪贴蒙版', coalesce: false,
      apply: (e, p) => e.setLayerClipping(pn(p, 'id', 0), pb(p, 'v', true)) },
    { id: 'layer.groupAdd', label: '新建组', coalesce: false, apply: (e) => e.addGroup() >= 0 },
    { id: 'layer.group', label: '图层编组', coalesce: false,
      apply: (e, p) => e.groupLayer(pn(p, 'id', 0)) >= 0 },
    { id: 'layer.ungroup', label: '图层解组', coalesce: false,
      apply: (e, p) => e.ungroupLayer(pn(p, 'id', 0)) },
    { id: 'layer.setParent', label: '移动入组', coalesce: false,
      apply: (e, p) => e.setLayerParent(pn(p, 'id', 0), pn(p, 'parent', 0), pn(p, 'index', -1)) },
    { id: 'layer.collapsed', label: '组折叠', coalesce: false,
      apply: (e, p) => e.setGroupCollapsed(pn(p, 'id', 0), pb(p, 'collapsed', true)) },
    { id: 'layer.mergeDown', label: '向下合并', coalesce: false, apply: (e) => e.mergeDown() },
    { id: 'layer.mergeVisible', label: '合并可见图层', coalesce: false, apply: (e) => e.mergeVisible() },
    { id: 'layer.flatten', label: '拼合图像', coalesce: false, apply: (e) => e.flattenImage() },
    { id: 'layer.stampVisible', label: '盖印可见图层', coalesce: false,
      apply: (e) => e.stampVisible() >= 0 },
    { id: 'layer.duplicate', label: '复制图层', coalesce: false,
      apply: (e, p) => e.duplicateLayer(pn(p, 'id', 0)) >= 0 },
    { id: 'layer.link', label: '链接图层', coalesce: false,
      apply: (e, p) => e.setLayerLink(pna(p, 'ids'), pb(p, 'link', true)) },
    { id: 'layer.align', label: '对齐图层', coalesce: false,
      apply: (e, p) => e.alignLayers(pna(p, 'ids'), pn(p, 'mode', 0)) },
    { id: 'layer.distribute', label: '分布图层', coalesce: false,
      apply: (e, p) => e.distributeLayers(pna(p, 'ids'), pn(p, 'mode', 0)) },
    { id: 'layer.reorder', label: '调整叠放次序', coalesce: false,
      apply: (e, p) => e.reorderLayer(pn(p, 'id', 0), pn(p, 'toIndex', 0)) },
    { id: 'layer.rename', label: '重命名图层', coalesce: false,
      apply: (e, p) => e.renameLayer(pn(p, 'id', 0), ps(p, 'name', '')) },
    { id: 'layer.translate', label: '移动图层', coalesce: false,
      apply: (e, p) => e.translateLayer(pn(p, 'id', 0), pn(p, 'dx', 0), pn(p, 'dy', 0),
        pn(p, 'phase', 0)) },
    { id: 'layer.maskAdd', label: '添加图层蒙版', coalesce: false,
      apply: (e, p) => e.addLayerMask(pn(p, 'id', 0)) },
    // 样式族
    { id: 'style.dropShadow', label: '投影', coalesce: false,
      apply: (e, p) => e.setDropShadow(pn(p, 'id', 0), pb(p, 'enabled', true), pn(p, 'angle', 120),
        pn(p, 'dist', 10), pn(p, 'size', 5), pn(p, 'opacity', 1), pn(p, 'r', 0), pn(p, 'g', 0),
        pn(p, 'b', 0), pn(p, 'spread', 0), pb(p, 'useGL', true)) },
    { id: 'style.stroke', label: '描边(样式)', coalesce: false,
      apply: (e, p) => e.setLayerStroke(pn(p, 'id', 0), pb(p, 'enabled', true), pn(p, 'width', 3),
        pn(p, 'r', 0), pn(p, 'g', 0), pn(p, 'b', 0)) },
    { id: 'style.colorOverlay', label: '颜色叠加', coalesce: false,
      apply: (e, p) => e.setColorOverlay(pn(p, 'id', 0), pb(p, 'enabled', true), pn(p, 'r', 0),
        pn(p, 'g', 0), pn(p, 'b', 0), pn(p, 'opacity', 1)) },
    { id: 'style.clear', label: '清除图层样式', coalesce: false,
      apply: (e, p) => e.clearLayerStyle(pn(p, 'id', 0)) },
    { id: 'style.innerShadow', label: '内阴影', coalesce: false,
      apply: (e, p) => e.setInnerShadow(pn(p, 'id', 0), pb(p, 'enabled', true), pn(p, 'angle', 120),
        pn(p, 'dist', 10), pn(p, 'size', 5), pn(p, 'opacity', 1), pn(p, 'r', 0), pn(p, 'g', 0),
        pn(p, 'b', 0), pn(p, 'spread', 0), pb(p, 'useGL', true)) },
    { id: 'style.outerGlow', label: '外发光', coalesce: false,
      apply: (e, p) => e.setOuterGlow(pn(p, 'id', 0), pb(p, 'enabled', true), pn(p, 'size', 5),
        pn(p, 'opacity', 1), pn(p, 'r', 255), pn(p, 'g', 255), pn(p, 'b', 0), pn(p, 'spread', 0)) },
    { id: 'style.innerGlow', label: '内发光', coalesce: false,
      apply: (e, p) => e.setInnerGlow(pn(p, 'id', 0), pb(p, 'enabled', true), pn(p, 'size', 5),
        pn(p, 'opacity', 1), pn(p, 'r', 255), pn(p, 'g', 255), pn(p, 'b', 0), pn(p, 'spread', 0)) },
    { id: 'style.gradientOverlay', label: '渐变叠加', coalesce: false,
      apply: (e, p) => e.setGradientOverlay(pn(p, 'id', 0), pb(p, 'enabled', true),
        pn(p, 'angle', 90), pn(p, 'opacity', 1), pn(p, 'fr', 0), pn(p, 'fg', 0), pn(p, 'fb', 0),
        pn(p, 'tr', 255), pn(p, 'tg', 255), pn(p, 'tb', 255)) },
    { id: 'style.satin', label: '光泽', coalesce: false,
      apply: (e, p) => e.setSatin(pn(p, 'id', 0), pb(p, 'enabled', true), pn(p, 'angle', 19),
        pn(p, 'dist', 8), pn(p, 'size', 8), pn(p, 'opacity', 0.5), pn(p, 'r', 0), pn(p, 'g', 0),
        pn(p, 'b', 0), pb(p, 'invert', false)) },
    { id: 'style.bevel', label: '斜面浮雕', coalesce: false,
      apply: (e, p) => e.setBevel(pn(p, 'id', 0), pb(p, 'enabled', true), pn(p, 'style', 0),
        pn(p, 'depth', 100), pb(p, 'up', true), pn(p, 'size', 6), pn(p, 'angle', 120),
        pn(p, 'altitude', 30), pn(p, 'hiOp', 1), pn(p, 'hr', 255), pn(p, 'hg', 255),
        pn(p, 'hb', 255), pn(p, 'shOp', 1), pn(p, 'sr', 0), pn(p, 'sg', 0), pn(p, 'sb', 0)) },
    { id: 'style.contour', label: '等高线', coalesce: false,
      apply: (e, p) => e.setStyleContour(pn(p, 'id', 0), pn(p, 'effect', 0), pna(p, 'xs'),
        pna(p, 'ys'), pna(p, 'corners'), pn(p, 'range', 0.5)) },
    { id: 'style.patternOverlay', label: '图案叠加', coalesce: false,
      apply: (e, p) => e.setPatternOverlay(pn(p, 'id', 0), pb(p, 'enabled', true),
        ps(p, 'patternId', ''), pn(p, 'scale', 1), pn(p, 'opacity', 1)) },
    { id: 'style.definePattern', label: '定义图案', coalesce: false,
      apply: (e) => e.definePattern() !== null },
    { id: 'style.globalLight', label: '全局光角度', coalesce: false,
      apply: (e, p) => e.setGlobalLight(pn(p, 'angle', 120)) },
    { id: 'style.copy', label: '拷贝图层样式', coalesce: false,
      apply: (e, p) => e.copyLayerStyle(pn(p, 'id', 0)) },
    { id: 'style.paste', label: '粘贴图层样式', coalesce: false,
      apply: (e, p) => e.pasteLayerStyle(pn(p, 'id', 0)) },
    { id: 'style.effectsEnabled', label: '图层效果总开关', coalesce: false,
      apply: (e, p) => e.setLayerEffectsEnabled(pn(p, 'id', 0), pb(p, 'enabled', true)) },
    { id: 'layer.fillAdd', label: '新建填充图层', coalesce: false,
      apply: (e, p) => e.addFillLayer(pn(p, 'kind', 0), pn(p, 'r', 255), pn(p, 'g', 255),
        pn(p, 'b', 255), pn(p, 'angle', 90)) >= 0 },
    { id: 'layer.fillParams', label: '填充图层参数', coalesce: true,
      apply: (e, p) => e.setFillLayerParams(pn(p, 'id', 0), pn(p, 'kind', 0), pn(p, 'r', 255),
        pn(p, 'g', 255), pn(p, 'b', 255), pn(p, 'angle', 90)) },
    // 调整族
    { id: 'layer.adjustAdd', label: '新建调整图层', coalesce: false,
      apply: (e, p) => e.addAdjustmentLayer(pn(p, 'kind', 0)) >= 0 },
    { id: 'adjust.params', label: '调整参数', coalesce: true,
      apply: (e, p) => e.setAdjustmentParams(pn(p, 'id', 0), pn(p, 'inBlack', 0),
        pn(p, 'inWhite', 255), pn(p, 'gamma', 1)) },
    { id: 'adjust.kind', label: '调整类型与参数', coalesce: true,
      apply: (e, p) => e.setAdjustmentKind(pn(p, 'id', 0), pn(p, 'kind', 0), pn(p, 'p0', 0),
        pn(p, 'p1', 0), pn(p, 'p2', 0), pn(p, 'p3', 0)) },
    { id: 'adjust.curve', label: '曲线', coalesce: true,
      apply: (e, p) => e.setCurvePoints(pn(p, 'id', 0), pna(p, 'xs'), pna(p, 'ys')) },
    // 选区族
    { id: 'select.rect', label: '矩形选框', coalesce: false,
      apply: (e, p) => e.setRectSelection(pn(p, 'x', 0), pn(p, 'y', 0), pn(p, 'w', 0),
        pn(p, 'h', 0), pn(p, 'op', 0)) },
    { id: 'select.ellipse', label: '椭圆选框', coalesce: false,
      apply: (e, p) => e.setEllipseSelection(pn(p, 'x', 0), pn(p, 'y', 0), pn(p, 'w', 0),
        pn(p, 'h', 0), pn(p, 'op', 0)) },
    { id: 'select.polygon', label: '多边形套索', coalesce: false,
      apply: (e, p) => e.setPolygonSelection(pna(p, 'points'), pn(p, 'op', 0)) },
    { id: 'select.wand', label: '魔棒', coalesce: false,
      apply: (e, p) => e.setWandSelection(pn(p, 'x', 0), pn(p, 'y', 0), pn(p, 'tolerance', 32),
        pb(p, 'contiguous', true), pn(p, 'op', 0), pn(p, 'allLayers', 0),
        pn(p, 'sampleSize', 1)) },
    { id: 'select.all', label: '全部选择', coalesce: false, apply: (e) => e.selectAll() },
    { id: 'select.inverse', label: '反选', coalesce: false, apply: (e) => e.invertSelection() },
    { id: 'select.reselect', label: '重新选择', coalesce: false, apply: (e) => e.reselectSelection() },
    { id: 'select.feather', label: '羽化', coalesce: false,
      apply: (e, p) => e.featherSelection(pn(p, 'radius', 0)) },
    { id: 'select.expand', label: '扩展选区', coalesce: false,
      apply: (e, p) => e.expandSelection(pn(p, 'radius', 0)) },
    { id: 'select.contract', label: '收缩选区', coalesce: false,
      apply: (e, p) => e.contractSelection(pn(p, 'radius', 0)) },
    { id: 'select.border', label: '边界选区', coalesce: false,
      apply: (e, p) => e.borderSelection(pn(p, 'radius', 0)) },
    { id: 'select.smooth', label: '平滑选区', coalesce: false, apply: (e) => e.smoothSelection() },
    { id: 'select.quickMask', label: '快速蒙版', coalesce: false,
      apply: (e) => e.toggleQuickMask() !== null },
    { id: 'select.store', label: '存储选区', coalesce: false,
      apply: (e, p) => e.storeSelection(ps(p, 'name', '')) },
    { id: 'select.load', label: '载入选区', coalesce: false,
      apply: (e, p) => e.loadSelection(ps(p, 'name', ''), pn(p, 'op', 0)) },
    { id: 'select.channelDelete', label: '删除通道', coalesce: false,
      apply: (e, p) => e.deleteChannel(ps(p, 'name', '')) },
    { id: 'select.clear', label: '取消选择', coalesce: false, apply: (e) => e.clearSelection() },
    { id: 'select.translate', label: '选区平移', coalesce: false,
      apply: (e, p) => e.translateSelection(pn(p, 'dx', 0), pn(p, 'dy', 0)) },
    // 绘制/编辑族
    { id: 'paint.gradient', label: '渐变', coalesce: false,
      apply: (e, p) => e.applyGradient(pn(p, 'layerId', 0), pn(p, 'type', 0), pn(p, 'style', 0),
        pn(p, 'x0', 0), pn(p, 'y0', 0), pn(p, 'x1', 0), pn(p, 'y1', 0), pn(p, 'target', 0)) },
    { id: 'paint.fill', label: '填充', coalesce: false,
      apply: (e, p) => e.fillSelection(pn(p, 'source', 0)) },
    { id: 'paint.stroke', label: '描边(选区)', coalesce: false,
      apply: (e, p) => e.strokeSelection(pn(p, 'width', 1)) },
    { id: 'paint.bucket', label: '油漆桶', coalesce: false,
      apply: (e, p) => e.bucketFill(pn(p, 'x', 0), pn(p, 'y', 0), pn(p, 'tolerance', 32),
        pb(p, 'contiguous', true), pn(p, 'allLayers', 0), pn(p, 'sampleSize', 1)) },
    { id: 'paint.patch', label: '修补', coalesce: false,
      apply: (e, p) => e.patchSelection(pn(p, 'dx', 0), pn(p, 'dy', 0)) },
    // P4 交互笔画录制：点列步骤化（flat [x,y,pressure,...]；回放=设笔刷→选层→逐点重放，
    // 轮询 strokeBusy 等笔画线程落账再放行下一步——回放确定性保证）
    { id: 'paint.brushStroke', label: '画笔笔画', coalesce: false,
      apply: (e, p) => new Promise<boolean>((resolve: (v: boolean) => void) => {
        const layerId = pn(p, 'layerId', 0);
        if (layerId > 0) {
          e.selectLayer(layerId);  // 目标层存在才选择（缺失返回 false 但不阻断笔画，PS 容错）
        }
        e.setBrushTarget(pb(p, 'mask', false));
        e.setBrushSettings(pn(p, 'size', 16), pn(p, 'hardness', 0.8), pn(p, 'opacity', 1),
          pn(p, 'r', 0), pn(p, 'g', 0), pn(p, 'b', 0), pb(p, 'erasing', false));
        const pts = pna(p, 'points');
        if (pts.length < 3) {
          resolve(false);
          return;
        }
        e.beginStroke(pts[0], pts[1], pts.length >= 3 ? pts[2] : 1);
        for (let i = 3; i + 2 < pts.length; i += 3) {
          e.continueStroke(pts[i], pts[i + 1], pts[i + 2]);
        }
        e.endStroke();
        // 等笔画线程提交完成（busy 轮询 200ms 上限；超时按成功放行——后续步的
        // beginEdit 会并入本条 pending，历史仍单条）
        const started = Date.now();
        const poll = (): void => {
          if (!e.getStats().strokeBusy || Date.now() - started > 200) {
            resolve(true);
          } else {
            setTimeout(poll, 8);
          }
        };
        setTimeout(poll, 8);
      }) },
    { id: 'paint.caFill', label: '内容识别填充', coalesce: false,
      apply: (e) => new Promise<boolean>((resolve: (v: boolean) => void) => {
        const started = e.contentAwareFill((changed: boolean): void => resolve(changed));
        if (!started) {
          resolve(false);
        }
      }) },
    { id: 'paint.caMove', label: '内容识别移动', coalesce: false,
      apply: (e, p) => new Promise<boolean>((resolve: (v: boolean) => void) => {
        const started = e.contentAwareMove(pn(p, 'dx', 0), pn(p, 'dy', 0), pb(p, 'extend', false),
          (changed: boolean): void => resolve(changed));
        if (!started) {
          resolve(false);
        }
      }) },
    { id: 'edit.copy', label: '拷贝', coalesce: false,
      apply: (e, p) => e.copySelection(pb(p, 'merged', false)) },
    { id: 'edit.cut', label: '剪切', coalesce: false,
      apply: (e, p) => e.cutSelection(pb(p, 'merged', false)) },
    { id: 'edit.paste', label: '粘贴', coalesce: false, apply: (e) => e.pasteClipboard() },
    { id: 'edit.clear', label: '清除像素', coalesce: false, apply: (e) => e.deleteSelectionPixels() },
    { id: 'paint.rasterShape', label: '栅格形状', coalesce: false,
      apply: (e, p) => e.rasterizeShape(pn(p, 'kind', 0), pn(p, 'x0', 0), pn(p, 'y0', 0),
        pn(p, 'x1', 0), pn(p, 'y1', 0), pn(p, 'cornerRadius', 0), pb(p, 'fill', true),
        pb(p, 'stroke', false), pn(p, 'strokeWidth', 0)) },
    // 矢量族
    { id: 'vector.shapeAdd', label: '新建形状图层', coalesce: false,
      apply: (e, p) => e.createShapeLayer(pn(p, 'kind', 0), pn(p, 'x0', 0), pn(p, 'y0', 0),
        pn(p, 'x1', 0), pn(p, 'y1', 0), pn(p, 'radius', 0), pn(p, 'sides', 3),
        pn(p, 'starRatio', 0.4), pn(p, 'weight', 0), pn(p, 'fillKind', 0), pn(p, 'fr', 255),
        pn(p, 'fg', 255), pn(p, 'fb', 255), pn(p, 'fa', 255), pn(p, 'hasStroke', 0), pn(p, 'sr', 0),
        pn(p, 'sg', 0), pn(p, 'sb', 0), pn(p, 'sa', 255), pn(p, 'strokeWidth', 0)) >= 0 },
    { id: 'vector.live', label: '形状参数', coalesce: false,
      apply: (e, p) => e.setShapeLive(pn(p, 'id', 0), pn(p, 'kind', 0), pn(p, 'x0', 0),
        pn(p, 'y0', 0), pn(p, 'x1', 0), pn(p, 'y1', 0), pn(p, 'radius', 0), pn(p, 'sides', 3),
        pn(p, 'starRatio', 0.4), pn(p, 'weight', 0)) },
    { id: 'vector.fill', label: '形状填充', coalesce: false,
      apply: (e, p) => e.setShapeFill(pn(p, 'id', 0), pn(p, 'kind', 0), pn(p, 'r', 255),
        pn(p, 'g', 255), pn(p, 'b', 255), pn(p, 'a', 255)) },
    { id: 'vector.stroke', label: '形状描边', coalesce: false,
      apply: (e, p) => e.setShapeStroke(pn(p, 'id', 0), pb(p, 'on', true), pn(p, 'r', 0),
        pn(p, 'g', 0), pn(p, 'b', 0), pn(p, 'a', 255), pn(p, 'width', 4)) },
    { id: 'vector.rasterize', label: '栅格化形状', coalesce: false,
      apply: (e, p) => e.rasterizeShapeLayer(pn(p, 'id', 0)) },
    { id: 'vector.path', label: '设置形状路径', coalesce: false,
      apply: (e, p) => e.setShapePath(pn(p, 'id', 0), pna(p, 'flat')) },
    { id: 'vector.workPath', label: '设置工作路径', coalesce: false,
      apply: (e, p) => e.setWorkPath(pna(p, 'flat')) },
    { id: 'vector.workCommit', label: '工作路径成形状', coalesce: false,
      apply: (e, p) => e.commitWorkPathAsShape(pn(p, 'fillKind', 0), pn(p, 'fr', 255),
        pn(p, 'fg', 255), pn(p, 'fb', 255), pn(p, 'fa', 255), pn(p, 'hasStroke', 0), pn(p, 'sr', 0),
        pn(p, 'sg', 0), pn(p, 'sb', 0), pn(p, 'sa', 255), pn(p, 'strokeWidth', 0)) >= 0 },
    { id: 'vector.pathToSelection', label: '路径转选区', coalesce: false,
      apply: (e, p) => e.pathToSelection(pn(p, 'op', 0)) },
    { id: 'vector.fillPath', label: '填充路径', coalesce: false,
      apply: (e, p) => e.fillPath(pn(p, 'r', 0), pn(p, 'g', 0), pn(p, 'b', 0), pn(p, 'a', 255)) },
    { id: 'vector.strokePath', label: '描边路径', coalesce: false,
      apply: (e, p) => e.strokePath(pn(p, 'r', 0), pn(p, 'g', 0), pn(p, 'b', 0), pn(p, 'a', 255),
        pn(p, 'width', 4)) },
    { id: 'vector.savePath', label: '存储路径', coalesce: false,
      apply: (e, p) => e.saveWorkPath(ps(p, 'name', '')) },
    { id: 'vector.deletePath', label: '删除路径', coalesce: false,
      apply: (e, p) => e.deleteSavedPath(pn(p, 'index', 0)) },
    { id: 'vector.loadPath', label: '载入路径', coalesce: false,
      apply: (e, p) => e.loadSavedPath(pn(p, 'index', 0)) },
    { id: 'vector.selectionToPath', label: '选区转路径', coalesce: false,
      apply: (e) => e.selectionToWorkPath() },
    { id: 'vector.customDefine', label: '定义自定形状', coalesce: false,
      apply: (e, p) => e.defineCustomShape(ps(p, 'name', '')) },
    { id: 'vector.mask', label: '矢量蒙版', coalesce: false,
      apply: (e, p) => e.setLayerVectorMask(pn(p, 'id', 0), pna(p, 'flat')) },
    { id: 'vector.maskEnable', label: '矢量蒙版启停', coalesce: false,
      apply: (e, p) => e.setVectorMaskEnabled(pn(p, 'id', 0), pb(p, 'v', true)) },
    { id: 'vector.placeCustom', label: '放置自定形状', coalesce: false,
      apply: (e, p) => e.placeCustomShape(ps(p, 'shapeId', ''), pn(p, 'cx', 0), pn(p, 'cy', 0),
        pn(p, 'w', 0), pn(p, 'h', 0)) >= 0 },
    // 文字族
    { id: 'text.create', label: '新建文字图层', coalesce: false,
      apply: (e, p) => e.createTextLayer(pn(p, 'x', 0), pn(p, 'y', 0), pn(p, 'wrapW', 0)) >= 0 },
    { id: 'text.content', label: '文字内容', coalesce: true,
      apply: (e, p) => e.setTextLayerContent(pn(p, 'id', 0), ps(p, 'content', ''),
        pb(p, 'txn', false)) },
    { id: 'text.style', label: '文字样式', coalesce: true,
      apply: (e, p) => e.setTextLayerStyle(pn(p, 'id', 0), ps(p, 'family', ''), pn(p, 'fontSize', 24),
        pn(p, 'lineHeight', 1.2), pn(p, 'tracking', 0), pb(p, 'bold', false),
        pb(p, 'italic', false), pn(p, 'colorR', 0), pn(p, 'colorG', 0), pn(p, 'colorB', 0),
        pn(p, 'align', 0), pn(p, 'wrapWidth', 0), pb(p, 'txn', false)) },
    { id: 'text.rasterize', label: '栅格化文字', coalesce: false,
      apply: (e, p) => e.rasterizeTextLayer(pn(p, 'id', 0)) },
    // 颜色
    { id: 'tools.setColors', label: '设置前景/背景色', coalesce: false,
      // 颜色单位 0..1（native SetColors 校验 v>1 拒绝；UI 同单位）
      apply: (e, p) => e.setColors(pn(p, 'fgR', 0), pn(p, 'fgG', 0), pn(p, 'fgB', 0),
        pn(p, 'bgR', 1), pn(p, 'bgG', 1), pn(p, 'bgB', 1)) },
    // 几何/变换族
    { id: 'image.resizeCanvas', label: '画布大小', coalesce: false,
      apply: (e, p) => e.resizeCanvas(pn(p, 'x', 0), pn(p, 'y', 0), pn(p, 'w', 0), pn(p, 'h', 0)) },
    { id: 'image.trim', label: '裁切透明像素', coalesce: false, apply: (e) => e.trimCanvas() },
    { id: 'image.revealAll', label: '显示全部', coalesce: false, apply: (e) => e.revealAll() },
    { id: 'image.rotate90', label: '90 度旋转', coalesce: false,
      apply: (e, p) => e.rotateDocument90(pb(p, 'clockwise', true)) },
    { id: 'image.flip', label: '翻转画布', coalesce: false,
      apply: (e, p) => e.flipDocument(pb(p, 'horizontal', true)) },
    { id: 'image.resample', label: '图像大小', coalesce: false,
      apply: (e, p) => e.resampleDocument(pn(p, 'w', 0), pn(p, 'h', 0), pn(p, 'method', 2)) },
    { id: 'image.rotateArbitrary', label: '任意角旋转', coalesce: false,
      apply: (e, p) => e.rotateDocumentArbitrary(pn(p, 'deg', 0)) },
    { id: 'transform.bake', label: '应用变换矩阵', coalesce: false,
      apply: (e, p) => e.bakeLayerMatrix(pn(p, 'id', 0), pna(p, 'm')) },
    { id: 'transform.bakeT', label: '自由变换应用', coalesce: false,
      apply: (e, p) => e.bakeLayerTransform(pn(p, 'id', 0), pn(p, 'scaleX', 1), pn(p, 'scaleY', 1),
        pn(p, 'rotDeg', 0), pn(p, 'skewDeg', 0), pn(p, 'pivotX', 0), pn(p, 'pivotY', 0)) },
    { id: 'image.caScale', label: '内容识别比例', coalesce: false,
      apply: (e, p) => e.contentAwareScale(pn(p, 'w', 0), pn(p, 'h', 0)) },
    // 变形族（一次性命令，笔画/钉数据随步走）
    { id: 'warp.liquify', label: '液化', coalesce: false,
      apply: (e, p) => e.applyLiquify(pn(p, 'layerId', 0),
        pobjArr(p, 'strokes') as Object as LiquifyStrokeDTO[]) },
    { id: 'warp.puppet', label: '操控变形', coalesce: false,
      apply: (e, p) => e.applyPuppet(pn(p, 'layerId', 0),
        pobjArr(p, 'pins') as Object as PuppetPinDTO[],
        pn(p, 'mode', 0), pn(p, 'density', 2)) },
    { id: 'warp.perspective', label: '透视变形', coalesce: false,
      apply: (e, p) => e.applyPerspective(pn(p, 'layerId', 0), pna(p, 'quad8')) },
    { id: 'warp.wideangle', label: '自适应广角', coalesce: false,
      apply: (e, p) => e.applyWideAngle(pn(p, 'layerId', 0), pn(p, 'model', 0), pn(p, 'focal', 0),
        pn(p, 'crop', 0), pn(p, 'scale', 1), pna(p, 'constraints') as Object as Object[]) },
    { id: 'warp.vpClone', label: '消失点克隆', coalesce: false,
      apply: (e, p) => e.applyVpClone(pn(p, 'layerId', 0), pna(p, 'quad8'), pn(p, 'sx', 1),
        pn(p, 'sy', 1), pobjArr(p, 'points') as Object as number[][], pn(p, 'size', 32),
        pn(p, 'hardness', 0.5), pn(p, 'opacity', 1)) },
    // 滤镜族（filter.apply 由 UI 提交点录制：begin+end 组合语义；apply 同构组合）
    { id: 'filter.apply', label: '应用滤镜', coalesce: false,
      apply: (e, p) => {
        if (!e.beginFilterPreview(pn(p, 'layerId', 0))) {
          return false;
        }
        e.endFilterPreview(true, pn(p, 'kind', 0), pn(p, 'p0', 0), pn(p, 'p1', 0), pn(p, 'p2', 0),
          pn(p, 'p3', 0), pn(p, 'p4', 0), pn(p, 'p5', 0), pn(p, 'seed', 0),
          ps(p, 'label', 'filter'));
        return true;
      } },
    { id: 'filter.direct', label: '滤镜(直应用)', coalesce: false,
      apply: (e, p) => e.applyFilter(pn(p, 'layerId', 0), pn(p, 'kind', 0), pn(p, 'p0', 0),
        pn(p, 'p1', 0), pn(p, 'p2', 0), pn(p, 'p3', 0), pn(p, 'p4', 0), pn(p, 'p5', 0),
        pn(p, 'seed', 0)) },
    { id: 'develop.raw', label: 'RAW 显影', coalesce: false,
      apply: (e, p) => e.developRaw(pn(p, 'temp', 0), pn(p, 'tint', 0), pn(p, 'ev', 0),
        pn(p, 'gamma', 1), ps(p, 'cameraJson', ''), 0).then((v: Object): boolean => v !== null) },
    { id: 'cameraRaw.apply', label: 'Camera Raw 滤镜', coalesce: false,
      apply: (e, p) => e.cameraRawFilter(pn(p, 'layerId', 0), ps(p, 'cameraJson', ''), 0)
        .then((v: Object): boolean => v !== null) },
    // 智能对象族
    { id: 'smart.convert', label: '转换为智能对象', coalesce: false,
      apply: (e, p) => e.convertToSmartObject(pna(p, 'layerIds')).then((v: number): boolean => v > 0) },
    { id: 'smart.viaCopy', label: '通过拷贝新建智能对象', coalesce: false,
      apply: (e, p) => e.newSmartObjectViaCopy(pn(p, 'id', 0)) > 0 },
    { id: 'smart.transform', label: '智能对象变换', coalesce: false,
      apply: (e, p) => e.setSmartTransform(pn(p, 'layerId', 0), pna(p, 'm')) },
    { id: 'smart.filterAdd', label: '添加智能滤镜', coalesce: false,
      apply: (e, p) => e.addSmartFilter(pn(p, 'layerId', 0), pn(p, 'kind', 0), pna(p, 'p'),
        pn(p, 'seed', 0), pn(p, 'blend', 0), pn(p, 'opacity', 1), ps(p, 'name', '')) },
    { id: 'smart.filterSet', label: '编辑智能滤镜', coalesce: false,
      apply: (e, p) => e.setSmartFilter(pn(p, 'layerId', 0), pn(p, 'index', 0), pna(p, 'p'),
        pn(p, 'seed', 0), pn(p, 'blend', 0), pn(p, 'opacity', 1), pb(p, 'visible', true),
        ps(p, 'name', '')) },
    { id: 'smart.filterRemove', label: '删除智能滤镜', coalesce: false,
      apply: (e, p) => e.removeSmartFilter(pn(p, 'layerId', 0), pn(p, 'index', 0)) },
    { id: 'smart.filterMove', label: '重排智能滤镜', coalesce: false,
      apply: (e, p) => e.moveSmartFilter(pn(p, 'layerId', 0), pn(p, 'from', 0), pn(p, 'to', 0)) },
    { id: 'smart.filtersEnabled', label: '智能滤镜总开关', coalesce: false,
      apply: (e, p) => e.setSmartFiltersEnabled(pn(p, 'layerId', 0), pb(p, 'enabled', true)) },
    { id: 'smart.filterMask', label: '智能滤镜蒙版', coalesce: false,
      apply: (e, p) => e.setSmartFilterMask(pn(p, 'layerId', 0), pn(p, 'mode', 0),
        pb(p, 'flag', true)) },
    { id: 'smart.rasterize', label: '栅格化智能对象', coalesce: false,
      apply: (e, p) => e.rasterizeSmartObject(pn(p, 'layerId', 0)) },
  ];
  const map: Map<string, CmdEntry> = new Map();
  for (const c of t) {
    map.set(c.id, c);
  }
  return map;
}

// ---------- 播放栈上限 / 管理常量 ----------

const MAX_PLAY_NESTING: number = 16;
/** P4 F 键绑定域：F2..F12（F1 保留系统帮助；裸键触发，与注册表 Ctrl 族无冲突）。 */
const FKEY_MIN: number = 2;
const FKEY_MAX: number = 12;

/** 引用参数解析：name|index（photocraft resolve 同义）。 */
function resolveRef(list: Action[], p: Record<string, Object>): number {
  const v: Object = p['action'];
  if (v === undefined || v === null) {
    return -1;
  }
  if (typeof v === 'number') {
    if (v < 0 || v >= list.length) {
      return -1;
    }
    return Math.floor(v);
  }
  if (typeof v === 'string' && v.length > 0) {
    for (let i = 0; i < list.length; i++) {
      if (list[i].name === v) {
        return i;
      }
    }
  }
  return -1;
}

/** 移动后索引跟随（photocraft moved_index 同义）。 */
function movedIndex(index: number, from: number, to: number): number {
  if (index === from) {
    return to;
  }
  if (from < index && index <= to) {
    return index - 1;
  }
  if (to <= index && index < from) {
    return index + 1;
  }
  return index;
}

function uniqueName(list: Action[]): string {
  let n = list.length + 1;
  while (true) {
    const name = `动作 ${n}`;
    let clash = false;
    for (const a of list) {
      if (a.name === name) {
        clash = true;
        break;
      }
    }
    if (!clash) {
      return name;
    }
    n++;
  }
}

/** 复制一串步（回放嵌套/持久化边界防共享可变状态）。 */
function copySteps(steps: ActionStep[]): ActionStep[] {
  const out: ActionStep[] = [];
  for (const s of steps) {
    out.push({ id: s.id, params: JSON.parse(JSON.stringify(s.params)) as Object as Record<string, Object> });
  }
  return out;
}

/**
 * 自动化单例：动作状态机 + 命令注册表 + execute 分发。
 * Index 挂 engine（setEngine）并接持久化回调（onDirty）。
 */
export class Automation {
  private registry: Map<string, CmdEntry> = buildRegistry();
  private engineRef: CompositorEngine | null = null;
  private list: Action[] = [];
  private recording: number = -1;            // 正在录制的动作索引（-1 = 未录制）
  private buffer: ActionStep[] = [];         // 录制窗内条目（journal 复制）
  private playing: number = 0;               // 嵌套深度
  private playbackStack: string[] = [];      // 循环检测 + 调试
  private batchSuspend: number = 0;          // 批处理挂起录制
  private dirty: (() => void) | null = null;

  setEngine(e: CompositorEngine): void {
    this.engineRef = e;
  }

  setDirtyCb(cb: (() => void) | null): void {
    this.dirty = cb;
  }

  private touch(): void {
    if (this.dirty !== null) {
      this.dirty();
    }
  }

  /** 注册表快照（面板/控制通道枚举）。 */
  catalog(): string[] {
    const out: string[] = [];
    this.registry.forEach((c: CmdEntry) => {
      out.push(c.id);
    });
    return out;
  }

  /** P3-H1 一致性导出：注册表条目元数据（id/label/coalesce；commands.list 控制通道用）。 */
  specDump(): AutoResult {
    const rows: Record<string, Object>[] = [];
    this.registry.forEach((c: CmdEntry): void => {
      const r: Record<string, Object> = {};
      r['id'] = c.id;
      r['label'] = c.label;
      r['coalesce'] = c.coalesce;
      rows.push(r);
    });
    const out: AutoResult = { ok: true, error: '', value: rows as Object };
    return out;
  }

  labelOf(id: string): string {
    const c = this.registry.get(id);
    return c !== undefined ? c.label : id;
  }

  /** 批处理步骤预检用：命令是否在注册表（可执行/可重放）。 */
  hasCommand(id: string): boolean {
    return this.registry.has(id) || id.startsWith('actions.');
  }

  /** 播放/批处理期间命令静默（photocraft 录制挂起语义）。 */
  isSuspended(): boolean {
    return this.playing > 0 || this.batchSuspend > 0;
  }

  recordingName(): string {
    if (this.recording < 0 || this.recording >= this.list.length) {
      return '';
    }
    return this.list[this.recording].name;
  }

  /**
   * 命令成功路径上报（CompositorEngine 包装层 + Index UI 提交点）。
   * 挂起（回放/批处理）或未录制时忽略；coalesce 同 id 替换末步。
   */
  note(id: string, params: Record<string, Object>): void {
    if (this.recording < 0 || this.isSuspended()) {
      return;
    }
    const entry = this.registry.get(id);
    if (entry === undefined) {
      return; // 注册表外命令不可重放（视图/工具/会话原语等）
    }
    if (entry.coalesce && this.buffer.length > 0) {
      const last = this.buffer[this.buffer.length - 1];
      if (last.id === id) {
        last.params = params;
        return;
      }
    }
    this.buffer.push({ id: id, params: params });
  }

  /** P4：录制窗内（Index 笔画收笔点调用）。 */
  isRecordingActive(): boolean {
    return this.recording >= 0 && !this.isSuspended();
  }

  /**
   * P4 交互笔画录制：收笔后把整笔点列步骤化入 buffer（photocraft Action::Stroke 同义）。
   * 超过 2100 数（700 点）等距抽稀（保首尾）——回放确定性优先，轨迹近似可接受。
   */
  noteStroke(params: Record<string, Object>): void {
    if (!this.isRecordingActive()) {
      return;
    }
    const raw = pna(params, 'points');
    if (raw.length > 2100) {
      const step = Math.ceil(raw.length / 2100);
      const pts: number[] = [];
      for (let i = 0; i < raw.length; i += 3 * step) {
        pts.push(raw[i], raw[i + 1], raw[i + 2]);
      }
      // 保尾点（起止完整 = 轨迹端点不变）
      if ((raw.length - 3) % (3 * step) !== 0) {
        pts.push(raw[raw.length - 3], raw[raw.length - 2], raw[raw.length - 1]);
      }
      params['points'] = pts as Object as Object[];
    }
    this.note('paint.brushStroke', params);
  }

  /** P4：面板 F 键循环点击（0→F2→…→F12→0，跳过被占用键；全占用退化为解绑）。 */
  cycleFkey(idx: number): AutoResult {
    const cands: number[] = [0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];
    if (idx < 0 || idx >= this.list.length) {
      return { ok: false, error: '没有这个动作' };
    }
    const cur = this.list[idx].fkey;
    let pos = cands.indexOf(cur);
    if (pos < 0) {
      pos = 0;
    }
    for (let i = 1; i <= cands.length; i++) {
      const cand = cands[(pos + i) % cands.length];
      const r = this.setFkey({ 'action': idx, 'key': cand });
      if (r.ok) {
        return r;
      }
    }
    return { ok: false, error: '没有可用的 F 键' };
  }

  /** P4：F 键 → 绑定动作名（无 = null）。Index onKey 裸 F2..F12 分发用。 */
  actionByFkey(key: number): string | null {
    if (key < FKEY_MIN || key > FKEY_MAX) {
      return null;
    }
    for (const a of this.list) {
      if (a.fkey === key) {
        return a.name;
      }
    }
    return null;
  }

  /** P4 actions.fkey {action, key}：绑定（key=0 解绑；全局唯一——旧主自动让位）。 */
  setFkey(p: Record<string, Object>): AutoResult {
    if (this.playing > 0) {
      return { ok: false, error: '回放中不能绑定 F 键' };
    }
    const idx = resolveRef(this.list, p);
    if (idx < 0) {
      return { ok: false, error: '没有这个动作' };
    }
    const keyV: Object = p['key'];
    let key = 0;
    if (keyV !== undefined && keyV !== null) {
      if (typeof keyV !== 'number') {
        return { ok: false, error: 'key 必须是 0（解绑）或 2..12' };
      }
      key = Math.floor(keyV);
      if (key !== 0 && (key < FKEY_MIN || key > FKEY_MAX)) {
        return { ok: false, error: 'key 必须是 0（解绑）或 2..12' };
      }
    }
    // 同键唯一：占用即拒绝（显式报错优于静默抢键；key=0 解绑无冲突）
    if (key !== 0) {
      for (const a of this.list) {
        if (a.fkey === key) {
          return { ok: false, error: `F${key} 已绑定动作「${a.name}」` };
        }
      }
    }
    this.list[idx].fkey = key;
    this.touch();
    const value: Record<string, Object> = {
      'action': this.list[idx].name, 'key': key,
    };
    return { ok: true, error: '', value: value };
  }

  // ---------- 管理命令（photocraft actions.* 对拍） ----------

  listActions(): AutoResult {
    const arr: Record<string, Object>[] = [];
    for (const a of this.list) {
      const item: Record<string, Object> = {
        'name': a.name, 'steps': a.steps.length, 'fkey': a.fkey,
        'recording': this.recording >= 0 && this.list[this.recording] === a,
      };
      arr.push(item);
    }
    const value: Record<string, Object> = {
      'actions': arr as Object as Object[],
      'recording': this.recordingName(),
    };
    return { ok: true, error: '', value: value };
  }

  getAction(p: Record<string, Object>): AutoResult {
    const idx = resolveRef(this.list, p);
    if (idx < 0) {
      return { ok: false, error: '没有这个动作' };
    }
    const a = this.list[idx];
    const steps: ActionStep[] = a.steps;
    const value: Record<string, Object> = {
      'name': a.name,
      'steps': steps as Object as Object[],
      'disabled': a.disabled as Object as Object[],
      'fkey': a.fkey,
    };
    return { ok: true, error: '', value: value };
  }

  recordAction(p: Record<string, Object>): AutoResult {
    if (this.playing > 0) {
      return { ok: false, error: '回放中不能录制' };
    }
    if (this.recording >= 0) {
      return { ok: false, error: '已在录制中，请先停止' };
    }
    let idx = -1;
    const v: Object = p['action'];
    if (v !== undefined && v !== null) {
      idx = resolveRef(this.list, p);
      if (idx < 0) {
        return { ok: false, error: '没有这个动作' };
      }
    } else {
      let name = ps(p, 'name', '');
      if (name.length === 0) {
        name = uniqueName(this.list);
      }
      for (const a of this.list) {
        if (a.name === name) {
          return { ok: false, error: '同名动作已存在' };
        }
      }
      this.list.push({ name: name, steps: [], disabled: [], fkey: 0 });
      idx = this.list.length - 1;
      this.touch();
    }
    this.recording = idx;
    this.buffer = [];
    const value: Record<string, Object> = { 'action': this.list[idx].name, 'index': idx };
    return { ok: true, error: '', value: value };
  }

  stopRecord(): AutoResult {
    if (this.playing > 0) {
      return { ok: false, error: '回放中不能停止录制' };
    }
    if (this.recording < 0) {
      return { ok: false, error: '当前没有在录制' };
    }
    const idx = this.recording;
    const steps = this.buffer;
    this.buffer = [];
    this.recording = -1;
    if (steps.length > 0) {
      this.list[idx].steps.push(...steps);
      this.touch();
    }
    const value: Record<string, Object> = {
      'action': this.list[idx].name, 'steps': this.list[idx].steps.length,
    };
    return { ok: true, error: '', value: value };
  }

  /** 回放步数（含录制未落盘的 pending 步，photocraft pending 语义）。 */
  private stepCount(idx: number): number {
    const a = this.list[idx];
    return a.steps.length + (this.recording === idx ? this.buffer.length : 0);
  }

  private flushRecording(): void {
    if (this.recording < 0) {
      return;
    }
    const steps = this.buffer;
    this.buffer = [];
    if (steps.length > 0) {
      this.list[this.recording].steps.push(...steps);
      this.touch();
    }
  }

  deleteAction(p: Record<string, Object>): AutoResult {
    if (this.playing > 0) {
      return { ok: false, error: '回放中不能删除' };
    }
    const idx = resolveRef(this.list, p);
    if (idx < 0) {
      return { ok: false, error: '没有这个动作' };
    }
    const v: Object = p['step'];
    if (v !== undefined && v !== null && typeof v === 'number') {
      const step = Math.floor(v);
      if (step < 0 || step >= this.stepCount(idx)) {
        return { ok: false, error: '步索引越界' };
      }
      if (this.recording === idx) {
        this.flushRecording();
      }
      const a = this.list[idx];
      a.steps.splice(step, 1);
      const nd: number[] = [];
      for (const d of a.disabled) {
        if (d !== step) {
          nd.push(d > step ? d - 1 : d);
        }
      }
      a.disabled = nd;
      this.touch();
      const value: Record<string, Object> = {
        'action': a.name, 'deletedStep': step, 'steps': a.steps.length,
      };
      return { ok: true, error: '', value: value };
    }
    if (this.recording >= 0) {
      return { ok: false, error: '请先停止录制再删除动作' };
    }
    const name = this.list[idx].name;
    this.list.splice(idx, 1);
    this.touch();
    const value: Record<string, Object> = { 'deleted': name };
    return { ok: true, error: '', value: value };
  }

  moveItem(p: Record<string, Object>): AutoResult {
    if (this.playing > 0) {
      return { ok: false, error: '回放中不能移动' };
    }
    const idx = resolveRef(this.list, p);
    if (idx < 0) {
      return { ok: false, error: '没有这个动作' };
    }
    const toV: Object = p['to'];
    if (typeof toV !== 'number') {
      return { ok: false, error: 'to 必须是非负整数' };
    }
    const to = Math.floor(toV);
    const v: Object = p['step'];
    if (v !== undefined && v !== null && typeof v === 'number') {
      const step = Math.floor(v);
      const len = this.stepCount(idx);
      if (step < 0 || step >= len || to < 0 || to >= len) {
        return { ok: false, error: '步或目标索引越界' };
      }
      if (this.recording === idx) {
        this.flushRecording();
      }
      const a = this.list[idx];
      const moved = a.steps.splice(step, 1)[0];
      a.steps.splice(to, 0, moved);
      const nd: number[] = [];
      for (const d of a.disabled) {
        nd.push(movedIndex(d, step, to));
      }
      a.disabled = nd;
      this.touch();
      const value: Record<string, Object> = { 'action': a.name, 'step': to };
      return { ok: true, error: '', value: value };
    }
    if (to < 0 || to >= this.list.length) {
      return { ok: false, error: '目标索引越界' };
    }
    const moved = this.list.splice(idx, 1)[0];
    this.list.splice(to, 0, moved);
    if (this.recording >= 0) {
      this.recording = movedIndex(this.recording, idx, to);
    }
    this.touch();
    const value: Record<string, Object> = { 'action': moved.name, 'index': to };
    return { ok: true, error: '', value: value };
  }

  toggleStep(p: Record<string, Object>): AutoResult {
    if (this.playing > 0) {
      return { ok: false, error: '回放中不能切换' };
    }
    const idx = resolveRef(this.list, p);
    if (idx < 0) {
      return { ok: false, error: '没有这个动作' };
    }
    const enabledV: Object = p['enabled'];
    let enabled = -1; // -1 = 缺省翻转
    if (typeof enabledV === 'boolean') {
      enabled = enabledV ? 1 : 0;
    }
    const stepV: Object = p['step'];
    if (stepV !== undefined && stepV !== null && typeof stepV === 'number') {
      const step = Math.floor(stepV);
      if (step < 0 || step >= this.stepCount(idx)) {
        return { ok: false, error: '步索引越界' };
      }
      if (this.recording === idx) {
        this.flushRecording();
      }
      const a = this.list[idx];
      const on = enabled === -1 ? a.disabled.includes(step) : enabled === 1;
      if (on) {
        const nd: number[] = [];
        for (const d of a.disabled) {
          if (d !== step) {
            nd.push(d);
          }
        }
        a.disabled = nd;
      } else if (!a.disabled.includes(step)) {
        a.disabled.push(step);
      }
      this.touch();
      const value: Record<string, Object> = { 'action': a.name, 'disabled': a.disabled as Object as Object[] };
      return { ok: true, error: '', value: value };
    }
    // 无 step：全体开或全体关（photocraft all-on-unless-all-off）
    if (this.recording === idx) {
      this.flushRecording();
    }
    const a = this.list[idx];
    const allOn = enabled === -1 ? a.disabled.length > 0 : enabled === 1;
    if (allOn) {
      a.disabled = [];
    } else {
      const nd: number[] = [];
      for (let i = 0; i < a.steps.length; i++) {
        nd.push(i);
      }
      a.disabled = nd;
    }
    this.touch();
    const value: Record<string, Object> = { 'action': a.name, 'disabled': a.disabled as Object as Object[] };
    return { ok: true, error: '', value: value };
  }

  renameAction(p: Record<string, Object>): AutoResult {
    if (this.playing > 0) {
      return { ok: false, error: '回放中不能重命名' };
    }
    const idx = resolveRef(this.list, p);
    if (idx < 0) {
      return { ok: false, error: '没有这个动作' };
    }
    const name = ps(p, 'name', '').trim();
    if (name.length === 0) {
      return { ok: false, error: '名称不能为空' };
    }
    const old = this.list[idx].name;
    if (old === name) {
      const value: Record<string, Object> = { 'action': name, 'index': idx };
      return { ok: true, error: '', value: value };
    }
    for (const a of this.list) {
      if (a.name === name) {
        return { ok: false, error: '同名动作已存在' };
      }
    }
    // 嵌套按名调用跟随改名（photocraft rename 同义）
    const pending = this.recording >= 0 ? this.buffer : [];
    const allSteps: ActionStep[] = pending.slice();
    for (const a of this.list) {
      for (const s of a.steps) {
        allSteps.push(s);
      }
    }
    for (const s of allSteps) {
      if (s.id === 'actions.play' && s.params['action'] === old) {
        s.params['action'] = name;
      }
    }
    this.list[idx].name = name;
    this.touch();
    const value: Record<string, Object> = { 'action': name, 'index': idx };
    return { ok: true, error: '', value: value };
  }

  // ---------- 回放 ----------

  /** 回放（photocraft play 语义：首错即停；嵌套 ≤16；循环拒绝）。 */
  async playAction(p: Record<string, Object>): Promise<AutoResult> {
    if (this.playing === 0 && this.playbackStack.length > 0) {
      return { ok: false, error: '回放栈状态异常' };
    }
    const idx = resolveRef(this.list, p);
    if (idx < 0) {
      return { ok: false, error: '没有这个动作' };
    }
    const a = this.list[idx];
    if (this.playbackStack.includes(a.name)) {
      return { ok: false, error: `循环调用动作「${a.name}」` };
    }
    if (this.recording === idx) {
      return { ok: false, error: '不能回放正在录制的动作' };
    }
    if (this.playbackStack.length >= MAX_PLAY_NESTING) {
      return { ok: false, error: '动作嵌套超过 16 层' };
    }
    const fromV: Object = p['from'];
    let from = 0;
    if (fromV !== undefined && fromV !== null) {
      if (typeof fromV !== 'number' || fromV < 0) {
        return { ok: false, error: 'from 必须是非负整数' };
      }
      from = Math.floor(fromV);
      if (from > a.steps.length) {
        return { ok: false, error: 'from 超出步数' };
      }
    }
    // 挂起录制（photocraft begin_playback），嵌套步进走完整状态机
    this.flushRecording();
    const savedRecording = this.recording;
    this.recording = -1;
    this.playing++;
    this.playbackStack.push(a.name);
    let ran = 0;
    let failed: PlayFailure | undefined = undefined;
    if (this.engineRef !== null) {
      const eng = this.engineRef;
      for (let i = from; i < a.steps.length; i++) {
        if (a.disabled.includes(i)) {
          continue;
        }
        const s = a.steps[i];
        let ok = false;
        try {
          ok = await this.runStep(eng, s.id, s.params);
        } catch (err) {
          ok = false;
        }
        if (!ok) {
          failed = { step: i, id: s.id, error: '命令执行失败' };
          break;
        }
        if (s.id === 'actions.play') {
          // 嵌套 play 的结构化失败冒泡（photocraft nested_failure）
          const nested = this.lastNestedFailure;
          if (nested !== null) {
            failed = { step: i, id: s.id, error: nested };
            this.lastNestedFailure = null;
            break;
          }
        }
        ran++;
      }
    } else {
      failed = { step: from, id: '', error: '引擎未挂载' };
    }
    this.playbackStack.pop();
    this.playing--;
    if (savedRecording >= 0) {
      // 恢复录制并回填命名调用（photocraft finish_playback）
      this.recording = savedRecording;
      const callParams: Record<string, Object> = { 'action': a.name };
      if (from > 0) {
        callParams['from'] = from;
      }
      this.buffer.push({ id: 'actions.play', params: callParams });
    }
    const value: Record<string, Object> = { 'action': a.name, 'ran': ran };
    if (failed !== undefined) {
      value['failed'] = failed as Object;
    }
    return { ok: true, error: '', value: value };
  }

  private lastNestedFailure: string | null = null;

  private async runStep(eng: CompositorEngine, id: string, params: Record<string, Object>): Promise<boolean> {
    if (id === 'actions.play') {
      const r = await this.playAction(params);
      if (!r.ok) {
        return false;
      }
      const value = r.value as Record<string, Object>;
      const failed = value['failed'];
      if (failed !== null && failed !== undefined) {
        const f = failed as Record<string, Object>;
        const stepN = typeof f['step'] === 'number' ? f['step'] : 0;
        const errMsg = typeof f['error'] === 'string' ? f['error'] : '未知错误';
        this.lastNestedFailure = `动作步骤 ${stepN + 1} 失败: ${errMsg}`;
        return true; // 命令本身成功，失败由外层检测 lastNestedFailure
      }
      return true;
    }
    const entry = this.registry.get(id);
    if (entry === undefined) {
      return false;
    }
    return await entry.apply(eng, params);
  }

  // ---------- execute 分发（控制通道 / 面板共用命令面） ----------

  async execute(id: string, params: Record<string, Object>): Promise<AutoResult> {
    if (id === 'undo' || id === 'redo') {
      // 历史操控：可执行但永不录制（photocraft journal:false 同义）
      if (this.engineRef === null) {
        return { ok: false, error: '引擎未挂载' };
      }
      const ok = id === 'undo' ? this.engineRef.undo() : this.engineRef.redo();
      return { ok: ok, error: ok ? '' : (id === 'undo' ? '无可撤销' : '无可重做') };
    }
    if (id.startsWith('actions.')) {
      const sub = id.substring(8);
      if (sub === 'list') {
        return this.listActions();
      }
      if (sub === 'get') {
        return this.getAction(params);
      }
      if (sub === 'record') {
        return this.recordAction(params);
      }
      if (sub === 'stop') {
        return this.stopRecord();
      }
      if (sub === 'play') {
        return await this.playAction(params);
      }
      if (sub === 'delete') {
        return this.deleteAction(params);
      }
      if (sub === 'move') {
        return this.moveItem(params);
      }
      if (sub === 'toggle') {
        return this.toggleStep(params);
      }
      if (sub === 'rename') {
        return this.renameAction(params);
      }
      if (sub === 'fkey') {
        return this.setFkey(params);
      }
      return { ok: false, error: `未知动作命令 actions.${sub}` };
    }
    const entry = this.registry.get(id);
    if (entry === undefined) {
      return { ok: false, error: `未知命令 ${id}` };
    }
    if (this.engineRef === null) {
      return { ok: false, error: '引擎未挂载' };
    }
    let ok = false;
    try {
      ok = await entry.apply(this.engineRef, params);
    } catch (err) {
      return { ok: false, error: '命令执行异常' };
    }
    // 录制由包装层 note() 负责（apply 调引擎方法即触发），此处不再补记避免双条
    return { ok: ok, error: ok ? '' : '命令返回失败' };
  }

  // ---------- 批处理挂起 ----------

  suspendRecording(): void {
    this.batchSuspend++;
  }

  resumeRecording(): void {
    this.batchSuspend = Math.max(0, this.batchSuspend - 1);
  }

  // ---------- 持久化（Photocraft ActionsFile 同构 v1） ----------

  serialize(): string {
    const actions: Record<string, Object>[] = [];
    for (const a of this.list) {
      const item: Record<string, Object> = {
        'name': a.name,
        'steps': a.steps as Object as Object[],
        'disabled': a.disabled as Object as Object[],
        'fkey': a.fkey,  // P4 v2（旧档缺省读 0）
      };
      actions.push(item);
    }
    const root: Record<string, Object> = { 'version': 2, 'actions': actions as Object as Object[] };
    return JSON.stringify(root);
  }

  loadFromJson(text: string): void {
    this.list = [];
    this.recording = -1;
    this.buffer = [];
    this.playing = 0;
    this.playbackStack = [];
    if (text.length === 0) {
      return;
    }
    try {
      const root = JSON.parse(text) as Object as Record<string, Object>;
      const arr = root['actions'] as Object as Object[];
      if (arr === undefined || arr === null) {
        return;
      }
      for (const raw of arr) {
        const a = raw as Record<string, Object>;
        const name = typeof a['name'] === 'string' ? a['name'] : '';
        if (name.length === 0) {
          continue;
        }
        const steps: ActionStep[] = [];
        const rawSteps = a['steps'] as Object as Object[];
        if (rawSteps !== undefined && rawSteps !== null) {
          for (const rs of rawSteps) {
            const pair = rs as Object as Object[];
            if (pair === undefined || pair.length < 1 || typeof pair[0] !== 'string') {
              continue;
            }
            const params = pair.length > 1
              ? JSON.parse(JSON.stringify(pair[1])) as Object as Record<string, Object>
              : {} as Record<string, Object>;
            steps.push({ id: pair[0], params: params });
          }
        }
        const disabled: number[] = [];
        const rawDis = a['disabled'] as Object as Object[];
        if (rawDis !== undefined && rawDis !== null) {
          for (const d of rawDis) {
            if (typeof d === 'number') {
              disabled.push(d);
            }
          }
        }
        const fk: Object = a['fkey'];
        const fkey = typeof fk === 'number' ? Math.floor(fk) : 0;  // P4 v2（v1 档缺省 0）
        this.list.push({ name: name, steps: steps, disabled: disabled, fkey: fkey });
      }
    } catch (err) {
      hilog.error(DOMAIN, TAG, 'loadFromJson failed: %{public}s', (err as Error).message);
    }
  }

  /** 步显示文案（面板/控制通道）。 */
  stepLabel(s: ActionStep): string {
    const entry = this.registry.get(s.id);
    const label = entry !== undefined ? entry.label : s.id;
    let detail = '';
    const keys: string[] = Object.keys(s.params);
    if (keys.length > 0 && keys.length <= 4) {
      const parts: string[] = [];
      for (const k of keys) {
        parts.push(`${k}=${String(s.params[k])}`);
      }
      detail = parts.join(' ');
    } else if (keys.length > 4) {
      detail = `${keys.length} 参数`;
    }
    return detail.length > 0 ? `${label}（${detail}）` : label;
  }
}

/** 全局单例（CompositorEngine note / Index 挂载 / Automate / ControlServer 共用）。 */
export const automation: Automation = new Automation();
