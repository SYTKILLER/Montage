/**
 * 菜单/工具/选项条的展示模型（05 §2 命令表的 VM 层）。
 * Index 按当前状态求值 → 子组件纯展示（enabled/checked 已算好）。
 */

/** 菜单项 VM（children 非空 = 二级子菜单，V-05：MenuItemOptions.builder）。 */
export interface MenuItemVM {
  id: string;
  label: string;
  shortcut: string;
  enabled: boolean;
  checked: boolean;
  children: MenuItemVM[];
}

export interface MenuGroupVM {
  id: string;
  label: string;
  items: MenuItemVM[];
}

export function leafItem(id: string, label: string, shortcut: string,
  enabled: boolean, checked: boolean = false): MenuItemVM {
  return { id: id, label: label, shortcut: shortcut, enabled: enabled, checked: checked, children: [] };
}

export function subItem(id: string, label: string, enabled: boolean,
  children: MenuItemVM[]): MenuItemVM {
  return { id: id, label: label, shortcut: '', enabled: enabled, checked: false, children: children };
}

/** 工具箱按钮 VM（M7.5 已实现 8 个 + 占位 2 个）。 */
export interface ToolVM {
  id: string;
  label: string;
  symbol: Resource;
  enabled: boolean;
}

/** 24 混合模式中文名（索引 = 引擎 BlendMode 值，M2.1 对齐 PSD 键）。 */
export const BLEND_LABELS: string[] = [
  '正常',
  '变暗', '正片叠底', '颜色加深', '线性加深',
  '变亮', '滤色', '颜色减淡', '线性减淡(添加)',
  '叠加', '柔光', '强光', '亮光', '线性光', '点光', '实色混合',
  '差值', '排除', '减去', '划分',
  '色相', '饱和度', '颜色', '明度',
];
