/**
 * H1 命令注册表全量（photocraft CommandSpec 模式，2026-10-27 快照对拍）。
 * 命令面元数据单一事实来源：菜单(buildMenuVM)/快捷键(shortcutMap)/动作录制(Automation
 * 注册表沿用)/控制通道(commands.list) 四端同源。typed NAPI 桥接保持不变（07 §H1 定案：
 * Montage 保持 typed 入口、注册表承载元数据）；runCommand 分发器为唯一执行体。
 * 字段：id（=runCommand/动作 id 命名空间）、zh（基线标签，i18n 键 = cmd.<id>）、
 * menu/parent（菜单落位）、shortcut、params（手写 JSON schema 串）、when/chk（使能/勾选
 * 谓词键）、coalesce（滑杆族：录制替换末步 + C++ history coalesce 同族）。
 * M24 登记的「H1 全量」收口：菜单/快捷键自本表生成；分发器 default 分支 UNHANDLED 日志。
 */
import { FilterDef, FILTER_CATALOG, FILTER_MENU_GROUPS, FILTER_MENU_LABELS } from './FilterCatalog';
import { registerPack, t } from './I18n';
import { leafItem, MenuGroupVM, MenuItemVM, subItem } from './MenuModel';

/** 菜单条目（命令或子菜单容器）。 */
export interface CmdSpec {
  id: string;
  zh: string;
  en: string;          // 英文标签（en 语言包生成源；强制覆盖测试断言非空）
  menu: string;        // 顶级菜单组 id（'' = 不入菜单）
  parent: string;      // 二级子菜单容器 id（'' = 顶级）
  shortcut: string;    // 'Ctrl+N' / 'Shift+F5' / 'Q'（'' = 无）
  params: string;      // 参数 schema（手写 JSON 串，agent/控制通道可读）
  isSub: boolean;      // true = 子菜单容器（children = parent==id 的条目）
  when: string;        // 使能谓词键（'' = 恒真）
  chk: string;         // 勾选谓词键（'' = 无勾选态）
  coalesce: boolean;   // 滑杆族
  dyn: string;         // 动态标签提供者键（'' = 静态 zh/en；'control' = 控制通道启停）
}

/** c() 的可选字段（不用 Partial，规避 ArkTS 工具类型边界）。 */
interface CmdOpt {
  parent?: string;
  shortcut?: string;
  params?: string;
  isSub?: boolean;
  when?: string;
  chk?: string;
  coalesce?: boolean;
  dyn?: string;
}

function c(id: string, zh: string, en: string, menu: string, o?: CmdOpt): CmdSpec {
  const s: CmdSpec = {
    id: id, zh: zh, en: en, menu: menu, parent: '', shortcut: '', params: '{}',
    isSub: false, when: '', chk: '', coalesce: false, dyn: '',
  };
  if (o !== undefined) {
    if (o.parent !== undefined) {
      s.parent = o.parent;
    }
    if (o.shortcut !== undefined) {
      s.shortcut = o.shortcut;
    }
    if (o.params !== undefined) {
      s.params = o.params;
    }
    if (o.isSub !== undefined) {
      s.isSub = o.isSub;
    }
    if (o.when !== undefined) {
      s.when = o.when;
    }
    if (o.chk !== undefined) {
      s.chk = o.chk;
    }
    if (o.coalesce !== undefined) {
      s.coalesce = o.coalesce;
    }
    if (o.dyn !== undefined) {
      s.dyn = o.dyn;
    }
  }
  return s;
}

/** 顶级菜单组（显示序）。 */
export interface MenuGroupSpec {
  id: string;
  zh: string;
  en: string;
}

export const MENU_GROUPS: MenuGroupSpec[] = [
  { id: 'file', zh: '文件', en: 'File' },
  { id: 'edit', zh: '编辑', en: 'Edit' },
  { id: 'image', zh: '图像', en: 'Image' },
  { id: 'layer', zh: '图层', en: 'Layer' },
  { id: 'select', zh: '选择', en: 'Select' },
  { id: 'filter', zh: '滤镜', en: 'Filter' },
  { id: 'view', zh: '视图', en: 'View' },
  { id: 'window', zh: '窗口', en: 'Window' },
  { id: 'help', zh: '帮助', en: 'Help' },
];

// ---------- 命令表（组内顺序 = 渲染顺序） ----------

function buildTable(): CmdSpec[] {
  const v: CmdSpec[] = [];
  // ----- 文件 -----
  v.push(c('file.new', '新建', 'New…', 'file', { shortcut: 'Ctrl+N',
    params: '{"width":px=1920,"height":px=1080,"name":str?}' }));
  v.push(c('file.open', '打开…', 'Open…', 'file', { shortcut: 'Ctrl+O' }));
  v.push(c('file.save', '保存', 'Save', 'file', { shortcut: 'Ctrl+S', when: 'doc' }));
  v.push(c('file.saveAs', '另存为…', 'Save As…', 'file', { when: 'doc' }));
  v.push(c('file.exportSub', '导出', 'Export', 'file', { isSub: true, when: 'doc' }));
  v.push(c('file.export', '导出图像（PNG / JPEG）…', 'Export Image (PNG / JPEG)…', 'file',
    { parent: 'file.exportSub', when: 'doc' }));
  v.push(c('file.placeEmbedded', '置入嵌入的智能对象…', 'Place Embedded Smart Object…', 'file',
    { when: 'doc' }));
  v.push(c('file.close', '关闭文档', 'Close Document', 'file', { when: 'doc' }));
  // ----- 编辑 -----
  v.push(c('edit.undo', '撤销', 'Undo', 'edit', { shortcut: 'Ctrl+Z', when: 'canUndo' }));
  v.push(c('edit.redo', '重做', 'Redo', 'edit', { shortcut: 'Ctrl+Shift+Z', when: 'canRedo' }));
  v.push(c('edit.cut', '剪切', 'Cut', 'edit', { shortcut: 'Ctrl+X', when: 'docSel' }));
  v.push(c('edit.copy', '拷贝', 'Copy', 'edit', { shortcut: 'Ctrl+C', when: 'docSel' }));
  v.push(c('edit.copyMerged', '合并拷贝', 'Copy Merged', 'edit',
    { shortcut: 'Ctrl+Shift+C', when: 'docSel' }));
  v.push(c('edit.paste', '粘贴', 'Paste', 'edit', { shortcut: 'Ctrl+V', when: 'doc' }));
  v.push(c('edit.clear', '清除', 'Clear', 'edit', { when: 'docSel' }));
  v.push(c('edit.fill', '填充…', 'Fill…', 'edit', { shortcut: 'Shift+F5', when: 'doc' }));
  v.push(c('edit.contentAwareFill', '内容识别填充', 'Content-Aware Fill…', 'edit',
    { when: 'docSel' }));
  v.push(c('edit.stroke', '描边…', 'Stroke…', 'edit', { when: 'docSel' }));
  v.push(c('edit.definePattern', '定义图案', 'Define Pattern', 'edit', { when: 'docSel' }));
  v.push(c('edit.importLookup', '载入查找表…', 'Import Lookup Table…', 'edit', { when: 'doc' }));
  v.push(c('edit.defineCustomShape', '定义自定形状', 'Define Custom Shape', 'edit',
    { when: 'workPath' }));
  v.push(c('edit.placeCustomSub', '放置自定形状', 'Place Custom Shape', 'edit',
    { isSub: true, when: 'customShapes' }));
  // ----- 图像 -----
  v.push(c('image.canvasSize', '画布大小…', 'Canvas Size…', 'image', { when: 'doc' }));
  v.push(c('image.imageSize', '图像大小…', 'Image Size…', 'image', { when: 'doc' }));
  v.push(c('image.contentAwareScale', '内容感知比例…', 'Content-Aware Scale…', 'image',
    { when: 'doc' }));
  v.push(c('image.rotate90cw', '顺时针旋转 90°', '90° Clockwise', 'image', { when: 'doc' }));
  v.push(c('image.rotate90ccw', '逆时针旋转 90°', '90° Counter Clockwise', 'image',
    { when: 'doc' }));
  v.push(c('image.rotate180', '旋转 180°', '180°', 'image', { when: 'doc' }));
  v.push(c('image.rotateAny', '任意角度旋转…', 'Arbitrary Rotation…', 'image', { when: 'doc' }));
  v.push(c('image.flipH', '水平翻转画布', 'Flip Canvas Horizontal', 'image', { when: 'doc' }));
  v.push(c('image.flipV', '垂直翻转画布', 'Flip Canvas Vertical', 'image', { when: 'doc' }));
  v.push(c('image.trim', '裁切（基于像素）', 'Trim (Based on Pixels)', 'image', { when: 'doc' }));
  v.push(c('image.revealAll', '显示全部', 'Reveal All', 'image', { when: 'doc' }));
  v.push(c('image.develop', 'RAW 显影参数…', 'RAW Develop Parameters…', 'image',
    { when: 'develop' }));
  v.push(c('image.automateSub', '自动', 'Automate', 'image', { isSub: true }));
  v.push(c('automate.batch', '批处理…', 'Batch…', 'image', { parent: 'image.automateSub' }));
  v.push(c('automate.processor', '图像处理器…', 'Image Processor…', 'image',
    { parent: 'image.automateSub' }));
  // ----- 图层 -----
  v.push(c('layer.transform', '自由变换', 'Free Transform', 'layer', { shortcut: 'Ctrl+T',
    when: 'doc' }));
  v.push(c('layer.transformNumeric', '数值变换…', 'Numeric Transform…', 'layer', { when: 'doc' }));
  v.push(c('layer.new', '新建图层', 'New Layer', 'layer', { when: 'doc' }));
  v.push(c('layer.newGroup', '新建组…', 'New Group…', 'layer', { when: 'doc' }));
  v.push(c('layer.group', '图层编组', 'Group Layers', 'layer', { shortcut: 'Ctrl+G',
    when: 'notGroup' }));
  v.push(c('layer.ungroup', '取消编组', 'Ungroup', 'layer', { when: 'isGroup' }));
  v.push(c('layer.duplicate', '复制图层', 'Duplicate Layer', 'layer', { shortcut: 'Ctrl+J',
    when: 'active' }));
  v.push(c('layer.mergeDown', '合并向下', 'Merge Down', 'layer', { shortcut: 'Ctrl+E',
    when: 'notGroup' }));
  v.push(c('layer.mergeVisible', '合并可见', 'Merge Visible', 'layer',
    { shortcut: 'Ctrl+Shift+E', when: 'doc' }));
  v.push(c('layer.stamp', '盖印', 'Stamp Visible', 'layer',
    { shortcut: 'Ctrl+Alt+Shift+E', when: 'doc' }));
  v.push(c('layer.flatten', '拼合图像', 'Flatten Image', 'layer', { when: 'doc' }));
  v.push(c('layer.style', '图层样式…', 'Layer Style…', 'layer', { when: 'notGroup' }));
  v.push(c('layer.copyStyle', '拷贝图层样式', 'Copy Layer Style', 'layer', { when: 'hasStyle' }));
  v.push(c('layer.pasteStyle', '粘贴图层样式', 'Paste Layer Style', 'layer',
    { when: 'pasteStyle' }));
  v.push(c('layer.toggleEffects', '显示/隐藏全部效果', 'Show/Hide All Effects', 'layer',
    { when: 'hasStyle', chk: 'fxOff' }));
  v.push(c('layer.globalLight', '全局光角度…', 'Global Light Angle…', 'layer', { when: 'doc' }));
  v.push(c('layer.vectorMaskFromPath', '矢量蒙版（自工作路径）', 'Vector Mask (from Work Path)',
    'layer', { when: 'workPath' }));
  v.push(c('layer.vectorMaskToggle', '启用/停用矢量蒙版', 'Enable/Disable Vector Mask', 'layer',
    { when: 'vecMask', chk: 'vmOff' }));
  v.push(c('layer.vectorMaskRemove', '删除矢量蒙版', 'Delete Vector Mask', 'layer',
    { when: 'vecMask' }));
  v.push(c('layer.rasterizeShape', '栅格化形状', 'Rasterize Shape', 'layer', { when: 'isShape' }));
  v.push(c('layer.convertSmart', '转换为智能对象', 'Convert to Smart Object', 'layer',
    { when: 'convertSmart' }));
  v.push(c('layer.smartFiltersDialog', '智能滤镜…', 'Smart Filters…', 'layer',
    { when: 'smartFilters' }));
  v.push(c('layer.smartViaCopy', '通过拷贝新建智能对象', 'New Smart Object via Copy', 'layer',
    { when: 'isSmart' }));
  v.push(c('layer.smartEdit', '编辑内容…', 'Edit Contents…', 'layer',
    { when: 'smartEditSrc' }));
  v.push(c('layer.smartFinish', '完成编辑（回写）', 'Finish Editing (Write Back)', 'layer',
    { when: 'smartEditing' }));
  v.push(c('layer.smartReload', '重新加载链接', 'Reload Link', 'layer',
    { when: 'linkedSmart' }));
  v.push(c('layer.smartReplace', '替换内容…', 'Replace Contents…', 'layer', { when: 'isSmart' }));
  v.push(c('layer.smartExport', '导出内容…', 'Export Contents…', 'layer', { when: 'isSmart' }));
  v.push(c('layer.smartFiltersToggle', '禁用/启用智能滤镜', 'Disable/Enable Smart Filters',
    'layer', { when: 'smartFilters', chk: 'sfOff' }));
  v.push(c('layer.stackModeSub', '堆栈模式', 'Stack Mode', 'layer', { isSub: true,
    when: 'isSmart' }));
  v.push(c('layer.stackOff', '关闭', 'Off', 'layer', { parent: 'layer.stackModeSub',
    chk: 'stack0' }));
  v.push(c('layer.stackEntropy', '熵', 'Entropy', 'layer', { parent: 'layer.stackModeSub',
    chk: 'stack1' }));
  v.push(c('layer.stackMax', '最大值', 'Maximum', 'layer', { parent: 'layer.stackModeSub',
    chk: 'stack2' }));
  v.push(c('layer.stackMean', '平均值', 'Mean', 'layer', { parent: 'layer.stackModeSub',
    chk: 'stack3' }));
  v.push(c('layer.stackMedian', '中间值', 'Median', 'layer', { parent: 'layer.stackModeSub',
    chk: 'stack4' }));
  v.push(c('layer.stackMin', '最小值', 'Minimum', 'layer', { parent: 'layer.stackModeSub',
    chk: 'stack5' }));
  v.push(c('layer.stackRange', '极差', 'Range', 'layer', { parent: 'layer.stackModeSub',
    chk: 'stack6' }));
  v.push(c('layer.stackSd', '标准偏差', 'Standard Deviation', 'layer',
    { parent: 'layer.stackModeSub', chk: 'stack8' }));
  v.push(c('layer.stackVar', '方差', 'Variance', 'layer', { parent: 'layer.stackModeSub',
    chk: 'stack10' }));
  v.push(c('layer.stackSum', '总和', 'Sum', 'layer', { parent: 'layer.stackModeSub',
    chk: 'stack9' }));
  v.push(c('layer.smartMaskClear', '清除滤镜蒙版', 'Clear Filter Mask', 'layer',
    { when: 'smartMask' }));
  v.push(c('layer.smartRasterize', '栅格化智能对象', 'Rasterize Smart Object', 'layer',
    { when: 'isSmart' }));
  v.push(c('layer.fillSolid', '新建纯色填充图层', 'New Solid Color Fill Layer', 'layer',
    { when: 'doc' }));
  v.push(c('layer.fillGrad', '新建渐变填充图层', 'New Gradient Fill Layer', 'layer',
    { when: 'doc' }));
  v.push(c('layer.linkToggle', '链接图层 / 解除链接', 'Link Layers / Unlink', 'layer',
    { when: 'linkable' }));
  v.push(c('layer.alignSub', '对齐', 'Align', 'layer', { isSub: true, when: 'align2' }));
  v.push(c('layer.alignLeft', '左边', 'Left Edges', 'layer', { parent: 'layer.alignSub' }));
  v.push(c('layer.alignHCenter', '水平居中', 'Horizontal Centers', 'layer',
    { parent: 'layer.alignSub' }));
  v.push(c('layer.alignRight', '右边', 'Right Edges', 'layer', { parent: 'layer.alignSub' }));
  v.push(c('layer.alignTop', '顶边', 'Top Edges', 'layer', { parent: 'layer.alignSub' }));
  v.push(c('layer.alignVCenter', '垂直居中', 'Vertical Centers', 'layer',
    { parent: 'layer.alignSub' }));
  v.push(c('layer.alignBottom', '底边', 'Bottom Edges', 'layer', { parent: 'layer.alignSub' }));
  v.push(c('layer.distributeSub', '分布', 'Distribute', 'layer', { isSub: true, when: 'align3' }));
  v.push(c('layer.distLeft', '左', 'Left Edges', 'layer', { parent: 'layer.distributeSub' }));
  v.push(c('layer.distHCenter', '水平居中', 'Horizontal Centers', 'layer',
    { parent: 'layer.distributeSub' }));
  v.push(c('layer.distRight', '右', 'Right Edges', 'layer', { parent: 'layer.distributeSub' }));
  v.push(c('layer.distTop', '顶', 'Top Edges', 'layer', { parent: 'layer.distributeSub' }));
  v.push(c('layer.distVCenter', '垂直居中', 'Vertical Centers', 'layer',
    { parent: 'layer.distributeSub' }));
  v.push(c('layer.distBottom', '底', 'Bottom Edges', 'layer', { parent: 'layer.distributeSub' }));
  v.push(c('layer.rename', '重命名…', 'Rename…', 'layer', { when: 'active' }));
  v.push(c('layer.delete', '删除图层', 'Delete Layer', 'layer', { when: 'active' }));
  v.push(c('layer.moveUp', '上移一层', 'Bring Forward', 'layer', { when: 'doc' }));
  v.push(c('layer.moveDown', '下移一层', 'Send Backward', 'layer', { when: 'doc' }));
  v.push(c('layer.clipToggle', '创建/释放剪贴蒙版', 'Create/Release Clipping Mask', 'layer',
    { when: 'doc' }));
  v.push(c('layer.maskAdd', '添加图层蒙版（显示全部）', 'Add Layer Mask (Reveal All)', 'layer',
    { when: 'doc' }));
  v.push(c('layer.maskEnableToggle', '启用/停用蒙版', 'Enable/Disable Mask', 'layer',
    { when: 'hasMask' }));
  v.push(c('layer.adjustAdd', '新建调整图层（色阶）', 'New Adjustment Layer (Levels)', 'layer',
    { when: 'doc' }));
  v.push(c('layer.rasterizeText', '栅格化文字', 'Rasterize Type', 'layer', { when: 'isText' }));
  // ----- 选择 -----
  v.push(c('select.all', '全部选择', 'All', 'select', { shortcut: 'Ctrl+A', when: 'doc',
    params: '{}' }));
  v.push(c('select.deselect', '取消选择', 'Deselect', 'select', { shortcut: 'Ctrl+D',
    when: 'doc' }));
  v.push(c('select.inverse', '反选', 'Inverse', 'select', { shortcut: 'Ctrl+Shift+I',
    when: 'doc' }));
  v.push(c('select.reselect', '重新选择', 'Reselect', 'select', { shortcut: 'Ctrl+Shift+D',
    when: 'doc' }));
  v.push(c('select.quickMask', '快速蒙版', 'Quick Mask', 'select', { shortcut: 'Q',
    when: 'doc', chk: 'quickMask' }));
  v.push(c('select.feather', '羽化…', 'Feather…', 'select', { shortcut: 'Shift+F6',
    when: 'docSel' }));
  v.push(c('select.store', '存储选区…', 'Store Selection…', 'select', { when: 'docSel' }));
  v.push(c('select.load', '载入选区…', 'Load Selection…', 'select', { when: 'hasChannels' }));
  v.push(c('select.border', '修改：边界…', 'Modify: Border…', 'select', { when: 'docSel' }));
  v.push(c('select.smooth', '修改：平滑', 'Modify: Smooth', 'select', { when: 'docSel' }));
  v.push(c('select.expand', '修改：扩展…', 'Modify: Expand…', 'select', { when: 'docSel' }));
  v.push(c('select.contract', '修改：收缩…', 'Modify: Contract…', 'select', { when: 'docSel' }));
  // ----- 滤镜（变形族 + Camera Raw 静态；27 目录滤镜 + 8 分类容器自 FILTER_CATALOG 生成） -----
  v.push(c('filter.cameraRaw', 'Camera Raw…', 'Camera Raw…', 'filter', { when: 'doc' }));
  v.push(c('liquify.enter', '液化（笔刷变形）…', 'Liquify (Brush Warp)…', 'filter',
    { when: 'doc' }));
  v.push(c('puppet.enter', '操控变形…', 'Puppet Warp…', 'filter', { when: 'doc' }));
  v.push(c('perspective.enter', '透视变形…', 'Perspective Warp…', 'filter', { when: 'doc' }));
  v.push(c('wideangle.enter', '自适应广角…', 'Adaptive Wide Angle…', 'filter', { when: 'doc' }));
  v.push(c('vanishing.enter', '消失点（透视克隆）…', 'Vanishing Point (Perspective Clone)…',
    'filter', { when: 'doc' }));
  for (const gkey of FILTER_MENU_GROUPS) {
    const gl: Object = FILTER_MENU_LABELS[gkey];
    v.push(c(`filterGroup.${gkey}`, typeof gl === 'string' ? gl as string : gkey, filterGroupEn(gkey),
      'filter', { isSub: true, when: 'doc' }));
    for (const f of FILTER_CATALOG) {
      if (f.menu === gkey) {
        v.push(c(`filter.${f.id}`, `${f.label}…`, `${f.en}…`, 'filter',
          { parent: `filterGroup.${gkey}`, when: 'doc', params: filterSchema(f) }));
      }
    }
  }
  // ----- 视图 -----
  v.push(c('view.zoomIn', '放大', 'Zoom In', 'view', { shortcut: 'Ctrl+=', when: 'doc' }));
  v.push(c('view.zoomOut', '缩小', 'Zoom Out', 'view', { shortcut: 'Ctrl+-', when: 'doc' }));
  v.push(c('view.zoom100', '100%', '100%', 'view', { shortcut: 'Ctrl+1', when: 'doc' }));
  v.push(c('view.fit', '适配画布', 'Fit on Screen', 'view', { shortcut: 'Ctrl+0', when: 'doc' }));
  v.push(c('view.rulers', '标尺', 'Rulers', 'view', { shortcut: 'Ctrl+R', chk: 'rulers' }));
  v.push(c('view.rulerUnitSub', '标尺单位', 'Ruler Units', 'view', { isSub: true }));
  v.push(c('view.rulerUnitPx', '像素', 'Pixels', 'view', { parent: 'view.rulerUnitSub',
    chk: 'unit0' }));
  v.push(c('view.rulerUnitPct', '百分比', 'Percent', 'view', { parent: 'view.rulerUnitSub',
    chk: 'unit1' }));
  v.push(c('view.guidesVisible', '显示参考线', 'Guides', 'view', { shortcut: 'Ctrl+;',
    chk: 'guidesVis' }));
  v.push(c('view.guidesLocked', '锁定参考线', 'Lock Guides', 'view',
    { shortcut: 'Ctrl+Alt+;', chk: 'guidesLock' }));
  v.push(c('view.guidesClear', '清除参考线', 'Clear Guides', 'view'));
  v.push(c('view.grid', '显示网格', 'Grid', 'view', { shortcut: "Ctrl+'", chk: 'gridVis' }));
  v.push(c('view.gridSpacingSub', '网格间距', 'Grid Spacing', 'view', { isSub: true }));
  v.push(c('view.gridSpacing50', '50 px', '50 px', 'view', { parent: 'view.gridSpacingSub',
    chk: 'grid50' }));
  v.push(c('view.gridSpacing100', '100 px', '100 px', 'view', { parent: 'view.gridSpacingSub',
    chk: 'grid100' }));
  v.push(c('view.gridSpacing250', '250 px', '250 px', 'view', { parent: 'view.gridSpacingSub',
    chk: 'grid250' }));
  v.push(c('view.gridSpacing500', '500 px', '500 px', 'view', { parent: 'view.gridSpacingSub',
    chk: 'grid500' }));
  v.push(c('view.snapGuides', '对齐参考线', 'Snap to Guides', 'view', { chk: 'snapGuides' }));
  v.push(c('view.proofSub', '校样设置', 'Proof Setup', 'view', { isSub: true, when: 'doc' }));
  v.push(c('view.proofOff', '关闭校样', 'Proof Off', 'view', { parent: 'view.proofSub',
    chk: 'proofOff' }));
  v.push(c('view.proofCoated', '合成涂布纸 CMYK', 'Coated CMYK Proof', 'view',
    { parent: 'view.proofSub', chk: 'proofCoated' }));
  v.push(c('view.proofP3', 'Display P3（广色域）', 'Display P3 (Wide Gamut)', 'view',
    { parent: 'view.proofSub', chk: 'proofP3' }));
  v.push(c('view.gamutWarning', '色域警告（CMYK）', 'Gamut Warning (CMYK)', 'view',
    { shortcut: 'Ctrl+Shift+Y', chk: 'gamut' }));
  v.push(c('view.lookupSub', '校样颜色（外观）', 'Proof Colors (Look)', 'view', { isSub: true,
    when: 'doc' }));
  v.push(c('view.lookupOff', '关闭', 'Off', 'view', { parent: 'view.lookupSub',
    chk: 'lookupOff' }));
  v.push(c('view.lookup0', 'Teal & Orange', 'Teal & Orange', 'view', { parent: 'view.lookupSub',
    chk: 'lookup0' }));
  v.push(c('view.lookupMono', 'Mono High Contrast', 'Mono High Contrast', 'view',
    { parent: 'view.lookupSub', chk: 'lookupMono' }));
  v.push(c('view.snapGrid', '对齐网格', 'Snap to Grid', 'view', { shortcut: "Ctrl+Shift+'",
    chk: 'snapGrid' }));
  v.push(c('view.screenModeSub', '屏幕模式', 'Screen Mode', 'view', { isSub: true }));
  v.push(c('view.screenStd', '标准屏幕模式', 'Standard Screen Mode', 'view',
    { parent: 'view.screenModeSub', chk: 'screen0' }));
  v.push(c('view.screenFullMenu', '带菜单栏的全屏模式', 'Full Screen Mode With Menu Bar', 'view',
    { parent: 'view.screenModeSub', chk: 'screen1' }));
  v.push(c('view.screenFull', '全屏模式', 'Full Screen Mode', 'view',
    { parent: 'view.screenModeSub', chk: 'screen2' }));
  // ----- 窗口 -----
  v.push(c('window.toggleLayers', '图层面板', 'Layers Panel', 'window', { chk: 'panelLayers' }));
  v.push(c('window.toggleHistory', '历史面板', 'History Panel', 'window', { chk: 'panelHistory' }));
  v.push(c('window.toggleHistogram', '直方图面板', 'Histogram Panel', 'window',
    { chk: 'panelHistogram' }));
  v.push(c('window.toggleInfo', '信息面板', 'Info Panel', 'window', { chk: 'panelInfo' }));
  v.push(c('window.toggleActions', '动作面板', 'Actions Panel', 'window', { chk: 'panelActions' }));
  v.push(c('window.togglePresets', '预设库', 'Preset Library', 'window', { chk: 'panelPresets' }));
  // D2 多文档（P4）：文档动态子树 + 循环切换 + 关闭全部
  v.push(c('window.docsSub', '文档', 'Documents', 'window', { isSub: true, when: 'docs1' }));
  v.push(c('window.nextDoc', '下一文档', 'Next Document', 'window',
    { shortcut: 'Ctrl+Tab', when: 'docs2' }));
  v.push(c('window.prevDoc', '上一文档', 'Previous Document', 'window',
    { shortcut: 'Ctrl+Shift+Tab', when: 'docs2' }));
  v.push(c('window.closeAll', '关闭全部文档', 'Close All Documents', 'window', { when: 'docs1' }));
  // ----- 帮助 -----
  v.push(c('help.themeSub', '主题', 'Theme', 'help', { isSub: true }));
  v.push(c('theme.system', '跟随系统', 'Follow System', 'help', { parent: 'help.themeSub',
    chk: 'themeSystem' }));
  v.push(c('theme.light', '浅色', 'Light', 'help', { parent: 'help.themeSub', chk: 'themeLight' }));
  v.push(c('theme.dark', '深色', 'Dark', 'help', { parent: 'help.themeSub', chk: 'themeDark' }));
  v.push(c('help.langSub', '语言', 'Language', 'help', { isSub: true }));
  v.push(c('lang.zh', '中文', '中文', 'help', { parent: 'help.langSub', chk: 'langZh' }));
  v.push(c('lang.en', 'English', 'English', 'help', { parent: 'help.langSub', chk: 'langEn' }));
  v.push(c('help.shortcuts', '快捷键表', 'Keyboard Shortcuts', 'help'));
  v.push(c('help.about', '关于 Montage', 'About Montage', 'help'));
  v.push(c('help.debugSub', '调试（沙箱往返）', 'Debug (Sandbox Round-trip)', 'help',
    { isSub: true }));
  v.push(c('file.saveSandbox', '保存往返包到沙箱', 'Save Round-trip Package to Sandbox', 'help',
    { parent: 'help.debugSub', shortcut: 'Ctrl+Alt+S', when: 'doc' }));
  v.push(c('file.openSandbox', '打开沙箱往返包', 'Open Sandbox Round-trip Package', 'help',
    { parent: 'help.debugSub', shortcut: 'Ctrl+Alt+O', when: 'doc' }));
  v.push(c('file.openSandboxDng', '打开沙箱 DNG（RAW 显影）', 'Open Sandbox DNG (RAW Develop)',
    'help', { parent: 'help.debugSub', when: 'doc' }));
  v.push(c('file.openSandboxCr2', '打开沙箱 CR2（M23 厂牌 RAW）', 'Open Sandbox CR2 (Vendor RAW)',
    'help', { parent: 'help.debugSub', when: 'doc' }));
  v.push(c('file.debugJpeg', 'JPEG 分带校准（M16）', 'JPEG Band Calibration (M16)', 'help',
    { parent: 'help.debugSub' }));
  v.push(c('debug.control', '启动控制通道（M24）', 'Start Control Channel (M24)', 'help',
    { parent: 'help.debugSub', dyn: 'control' }));
  return v;
}

function filterGroupEn(key: string): string {
  const map: Record<string, string> = {
    'adjust': 'Adjust', 'blur': 'Blur', 'sharpen': 'Sharpen', 'noise': 'Noise',
    'pixelate': 'Pixelate', 'stylize': 'Stylize', 'render': 'Render', 'other': 'Other',
  };
  const v: Object = map[key];
  return typeof v === 'string' ? v as string : key;
}

/** 滤镜参数 schema 串（FilterParamDef[] → JSON；控制通道/agent 可读）。 */
function filterSchema(f: FilterDef): string {
  const ps: Record<string, Object> = {};
  for (const p of f.params) {
    const d: Record<string, Object> = {};
    d['min'] = p.min;
    d['max'] = p.max;
    d['step'] = p.step;
    d['def'] = p.def;
    d['unit'] = p.unit;
    ps[p.label] = d;
  }
  if (f.hasSeed) {
    ps['seed'] = 'u64';
  }
  return JSON.stringify(ps);
}

/** 全量命令表（模块级一次构建；filter 部分由 FILTER_CATALOG 派生）。 */
export const COMMAND_TABLE: CmdSpec[] = buildTable();

export function findCmd(id: string): CmdSpec | undefined {
  return COMMAND_TABLE.find((s: CmdSpec): boolean => s.id === id);
}

// ---------- 菜单状态快照（Index 求值 → 纯函数生成 VM，MenuModel 同架构） ----------

/** 活动层摘要（LayerDTO 中菜单谓词所需字段）。 */
export interface ActiveSnap {
  isGroup: boolean;
  hasStyle: boolean;
  styleOn: boolean;
  hasVectorMask: boolean;
  vectorMaskOn: boolean;
  isShape: boolean;
  isSmart: boolean;
  isAdjustment: boolean;
  isText: boolean;
  smartFilters: number;
  smartFiltersOn: boolean;
  smartHasMask: boolean;
  smartEmbedded: boolean;    // M-EC：false = 链接 SO
  smartSourceAvail: boolean; // M-EC：源可用（嵌入有字节/链接路径可达）
  stackMode: number;
}

/** 菜单状态快照：Index 每次刷新填一份，谓词纯函数求值。 */
export interface MenuState {
  docOpen: boolean;
  canUndo: boolean;
  canRedo: boolean;
  hasSelection: boolean;
  channelsCount: number;
  quickMask: boolean;
  hasRawDevelop: boolean;
  hasCustomShapes: boolean;
  customShapeIds: string[];
  workPathNonEmpty: boolean;
  hasStyleClipboard: boolean;
  alignCount: number;
  linkActive: boolean;
  activeLayerHasMask: boolean;
  active: ActiveSnap | null;
  smartEditing: boolean;  // M-EC：活动标签是智能对象编辑会话
  rulersVisible: boolean;
  rulerUnit: number;
  guidesVisible: boolean;
  guidesLocked: boolean;
  gridVisible: boolean;
  gridSpacing: number;
  snapGuides: boolean;
  snapGrid: boolean;
  screenMode: number;
  viewXformOn: boolean;
  viewXformDesc: string;
  viewGamut: boolean;
  panelLayers: boolean;
  panelHistory: boolean;
  panelHistogram: boolean;
  panelInfo: boolean;
  panelActions: boolean;
  panelPresets: boolean;
  themeMode: string;
  lang: string;
  controlRunning: boolean;
  docs: MenuDoc[];  // D2 多文档：标签条/窗口菜单动态子树数据源
}

/** D2 多文档：菜单态文档条目。 */
export interface MenuDoc {
  id: number;
  name: string;
  modified: boolean;
  active: boolean;
}

type Pred = (s: MenuState) => boolean;

const PRED: Record<string, Pred> = {
  'doc': (s: MenuState): boolean => s.docOpen,
  'docSel': (s: MenuState): boolean => s.docOpen && s.hasSelection,
  'canUndo': (s: MenuState): boolean => s.canUndo,
  'canRedo': (s: MenuState): boolean => s.canRedo,
  'active': (s: MenuState): boolean => s.active !== null,
  'notGroup': (s: MenuState): boolean => s.docOpen && s.active !== null && !s.active.isGroup,
  'isGroup': (s: MenuState): boolean => s.docOpen && s.active !== null && s.active.isGroup,
  'hasStyle': (s: MenuState): boolean => s.docOpen && s.active !== null && s.active.hasStyle,
  'pasteStyle': (s: MenuState): boolean =>
    s.docOpen && s.active !== null && !s.active.isGroup && s.hasStyleClipboard,
  'workPath': (s: MenuState): boolean => s.docOpen && s.workPathNonEmpty,
  'customShapes': (s: MenuState): boolean => s.docOpen && s.hasCustomShapes,
  'vecMask': (s: MenuState): boolean => s.docOpen && s.active !== null && s.active.hasVectorMask,
  'isShape': (s: MenuState): boolean => s.docOpen && s.active !== null && s.active.isShape,
  'convertSmart': (s: MenuState): boolean => s.docOpen && s.active !== null &&
    !s.active.isGroup && !s.active.isSmart && !s.active.isAdjustment,
  'smartFilters': (s: MenuState): boolean => s.docOpen && s.active !== null &&
    s.active.isSmart && s.active.smartFilters > 0,
  'isSmart': (s: MenuState): boolean => s.docOpen && s.active !== null && s.active.isSmart,
  'smartEditSrc': (s: MenuState): boolean => s.docOpen && s.active !== null && s.active.isSmart &&
    s.active.smartSourceAvail,
  'smartEditing': (s: MenuState): boolean => s.docOpen && s.smartEditing,
  'linkedSmart': (s: MenuState): boolean => s.docOpen && s.active !== null && s.active.isSmart &&
    !s.active.smartEmbedded,
  'smartMask': (s: MenuState): boolean => s.docOpen && s.active !== null && s.active.isSmart &&
    s.active.smartHasMask,
  'isText': (s: MenuState): boolean => s.docOpen && s.active !== null && s.active.isText,
  'align2': (s: MenuState): boolean => s.docOpen && s.alignCount >= 2,
  'align3': (s: MenuState): boolean => s.docOpen && s.alignCount >= 3,
  'linkable': (s: MenuState): boolean => s.docOpen && (s.alignCount >= 2 || s.linkActive),
  'hasMask': (s: MenuState): boolean => s.docOpen && s.activeLayerHasMask,
  'hasChannels': (s: MenuState): boolean => s.docOpen && s.channelsCount > 0,
  'develop': (s: MenuState): boolean => s.docOpen && s.hasRawDevelop,
  // 勾选谓词
  'fxOff': (s: MenuState): boolean => s.active !== null && !s.active.styleOn,
  'vmOff': (s: MenuState): boolean => s.active !== null && !s.active.vectorMaskOn,
  'sfOff': (s: MenuState): boolean => s.active !== null && !s.active.smartFiltersOn,
  'stack0': (s: MenuState): boolean => s.active !== null && s.active.stackMode === 0,
  'stack1': (s: MenuState): boolean => s.active !== null && s.active.stackMode === 1,
  'stack2': (s: MenuState): boolean => s.active !== null && s.active.stackMode === 2,
  'stack3': (s: MenuState): boolean => s.active !== null && s.active.stackMode === 3,
  'stack4': (s: MenuState): boolean => s.active !== null && s.active.stackMode === 4,
  'stack5': (s: MenuState): boolean => s.active !== null && s.active.stackMode === 5,
  'stack6': (s: MenuState): boolean => s.active !== null && s.active.stackMode === 6,
  'stack8': (s: MenuState): boolean => s.active !== null && s.active.stackMode === 8,
  'stack9': (s: MenuState): boolean => s.active !== null && s.active.stackMode === 9,
  'stack10': (s: MenuState): boolean => s.active !== null && s.active.stackMode === 10,
  'quickMask': (s: MenuState): boolean => s.quickMask,
  'rulers': (s: MenuState): boolean => s.rulersVisible,
  'unit0': (s: MenuState): boolean => s.rulerUnit === 0,
  'unit1': (s: MenuState): boolean => s.rulerUnit === 1,
  'guidesVis': (s: MenuState): boolean => s.guidesVisible,
  'guidesLock': (s: MenuState): boolean => s.guidesLocked,
  'gridVis': (s: MenuState): boolean => s.gridVisible,
  'grid50': (s: MenuState): boolean => s.gridSpacing === 50,
  'grid100': (s: MenuState): boolean => s.gridSpacing === 100,
  'grid250': (s: MenuState): boolean => s.gridSpacing === 250,
  'grid500': (s: MenuState): boolean => s.gridSpacing === 500,
  'snapGuides': (s: MenuState): boolean => s.snapGuides,
  'snapGrid': (s: MenuState): boolean => s.snapGrid,
  'proofOff': (s: MenuState): boolean => !s.viewXformOn,
  'proofCoated': (s: MenuState): boolean => s.viewXformOn && s.viewXformDesc === 'proof: coated-cmyk',
  'proofP3': (s: MenuState): boolean => s.viewXformOn && s.viewXformDesc === 'proof: display-p3',
  'gamut': (s: MenuState): boolean => s.viewGamut,
  'lookupOff': (s: MenuState): boolean => !s.viewXformOn || s.viewXformDesc.startsWith('proof'),
  'lookup0': (s: MenuState): boolean => s.viewXformDesc === 'lookup: tealOrange',
  'lookupMono': (s: MenuState): boolean => s.viewXformDesc === 'lookup: monoContrast',
  'screen0': (s: MenuState): boolean => s.screenMode === 0,
  'screen1': (s: MenuState): boolean => s.screenMode === 1,
  'screen2': (s: MenuState): boolean => s.screenMode === 2,
  'panelLayers': (s: MenuState): boolean => s.panelLayers,
  'panelHistory': (s: MenuState): boolean => s.panelHistory,
  'panelHistogram': (s: MenuState): boolean => s.panelHistogram,
  'panelInfo': (s: MenuState): boolean => s.panelInfo,
  'panelActions': (s: MenuState): boolean => s.panelActions,
  'panelPresets': (s: MenuState): boolean => s.panelPresets,
  'themeSystem': (s: MenuState): boolean => s.themeMode === 'system',
  'themeLight': (s: MenuState): boolean => s.themeMode === 'light',
  'themeDark': (s: MenuState): boolean => s.themeMode === 'dark',
  'langZh': (s: MenuState): boolean => s.lang === 'zh',
  'langEn': (s: MenuState): boolean => s.lang === 'en',
  'docs1': (s: MenuState): boolean => s.docs.length > 0,
  'docs2': (s: MenuState): boolean => s.docs.length > 1,
};

function evalPred(key: string, s: MenuState): boolean {
  if (key === '') {
    return true;
  }
  const p: Pred | undefined = PRED[key];
  if (p === undefined) {
    return true;
  }
  return p(s);
}

/** P3-H1 控制通道求值助手：命令当前使能/勾选态（菜单 VM 同源谓词）。 */
export function cmdEnabled(s: CmdSpec, st: MenuState): boolean {
  return evalPred(s.when, st);
}

export function cmdChecked(s: CmdSpec, st: MenuState): boolean {
  return s.chk !== '' && evalPred(s.chk, st);
}

/** 条目标签：i18n 键 cmd.<id>，回退注册表 zh；dyn 条目按状态出动态标签。 */
function labelOf(spec: CmdSpec, s: MenuState): string {
  if (spec.dyn === 'control') {
    return s.controlRunning
      ? t('cmd.debug.control.stop', '停止控制通道')
      : t('cmd.debug.control', spec.zh);
  }
  return t(`cmd.${spec.id}`, spec.zh);
}

function leafOf(spec: CmdSpec, s: MenuState): MenuItemVM {
  return leafItem(spec.id, labelOf(spec, s), spec.shortcut, evalPred(spec.when, s),
    spec.chk === '' ? false : evalPred(spec.chk, s));
}

/**
 * 从注册表生成菜单 VM（Index.buildMenuGroups 唯一来源）。
 * dynamic 子树：放置自定形状（customShapeIds 运行时数据）。
 */
export function buildMenuVM(s: MenuState): MenuGroupVM[] {
  const groups: MenuGroupVM[] = [];
  for (const g of MENU_GROUPS) {
    const items: MenuItemVM[] = [];
    for (const spec of COMMAND_TABLE) {
      if (spec.menu !== g.id || spec.parent !== '') {
        continue;
      }
      if (spec.isSub) {
        const kids: MenuItemVM[] = [];
        if (spec.id === 'edit.placeCustomSub') {
          for (const shapeId of s.customShapeIds) {
            kids.push(leafItem(`edit.placeCustom:${shapeId}`, shapeId, '', s.docOpen));
          }
        } else if (spec.id === 'window.docsSub') {
          for (const d of s.docs) {
            kids.push(leafItem(`doc.switch:${d.id}`, d.name + (d.modified ? ' *' : ''), '',
              true, d.active));
          }
        } else {
          for (const sub of COMMAND_TABLE) {
            if (sub.parent === spec.id) {
              kids.push(leafOf(sub, s));
            }
          }
        }
        items.push(subItem(spec.id, t(`sub.${spec.id}`, spec.zh), evalPred(spec.when, s), kids));
      } else {
        items.push(leafOf(spec, s));
      }
    }
    groups.push({ id: g.id, label: t(`group.${g.id}`, g.zh), items: items });
  }
  return groups;
}

// ---------- 快捷键表（onKey 同源） ----------

/** 解析注册表快捷键串 → 修饰键 + 键名（'Ctrl+Alt+;' → {c,a,s,k:';'}）。 */
export interface Chord {
  c: boolean;
  a: boolean;
  s: boolean;
  k: string;
}

export function parseShortcut(str: string): Chord | null {
  if (str === '') {
    return null;
  }
  const parts = str.split('+');
  let cc = false;
  let aa = false;
  let ss = false;
  let key = '';
  for (let i = 0; i < parts.length; i++) {
    const p = parts[i];
    if (p === 'Ctrl') {
      cc = true;
    } else if (p === 'Alt') {
      aa = true;
    } else if (p === 'Shift') {
      ss = true;
    } else {
      key = p;
    }
  }
  if (key === '') {
    return null;
  }
  const ch: Chord = { c: cc, a: aa, s: ss, k: key };
  return ch;
}

/** 快捷键命中条目（预解析修饰键）。 */
export interface ShortcutEntry {
  c: boolean;
  a: boolean;
  s: boolean;
  id: string;
}

/** 快捷键 → 命中条目（键名索引；Index 构建一次，onKey 查表分发）。 */
export function buildShortcutMap(): Map<string, ShortcutEntry[]> {
  const map = new Map<string, ShortcutEntry[]>();
  for (const spec of COMMAND_TABLE) {
    if (spec.shortcut === '') {
      continue;
    }
    const ch = parseShortcut(spec.shortcut);
    if (ch === null) {
      continue;
    }
    let arr = map.get(ch.k);
    if (arr === undefined) {
      arr = [];
      map.set(ch.k, arr);
    }
    const e: ShortcutEntry = { c: ch.c, a: ch.a, s: ch.s, id: spec.id };
    arr.push(e);
  }
  return map;
}

// ---------- H8 i18n：en 语言包（注册表 en 字段生成；强制覆盖测试对全表键核验） ----------

export function i18nKeys(): string[] {
  const keys: string[] = [];
  for (const g of MENU_GROUPS) {
    keys.push(`group.${g.id}`);
  }
  for (const spec of COMMAND_TABLE) {
    keys.push(spec.isSub ? `sub.${spec.id}` : `cmd.${spec.id}`);
    if (spec.dyn === 'control') {
      keys.push('cmd.debug.control.stop');
    }
  }
  return keys;
}

export function registerLangPacks(): void {
  const en: Record<string, string> = {};
  for (const g of MENU_GROUPS) {
    en[`group.${g.id}`] = g.en;
  }
  for (const spec of COMMAND_TABLE) {
    const key = spec.isSub ? `sub.${spec.id}` : `cmd.${spec.id}`;
    en[key] = spec.en;
  }
  en['cmd.debug.control.stop'] = 'Stop Control Channel';
  registerPack('en', en);
}
