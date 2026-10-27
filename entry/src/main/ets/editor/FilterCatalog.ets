/**
 * M13 滤镜目录（06 §4 M13 第一批核 + 07 §3.4 注入表；kind 序号 = 引擎 filters.h FilterKind）。
 * schema 驱动滤镜对话框：每核参数滑条定义（min/max/step/默认值）。
 * 菜单分类 = menu 字段（blur/sharpen/noise/pixelate/stylize/render/other）。
 */

export interface FilterParamDef {
  label: string;
  min: number;
  max: number;
  step: number;
  def: number;
  unit: string; // '' | '%' | 'px' | '°' | '级'
}

export interface FilterDef {
  id: string;        // 菜单命令 id 后缀（'filter.gauss' → id 'gauss'）
  en: string;        // 英文标签（H1 注册表 en 语言包生成源）
  label: string;
  kind: number;      // 引擎 FilterKind
  menu: string;      // 分类
  params: FilterParamDef[];
  hasSeed: boolean;  // 噪声/云彩类确定性种子
}

export const FILTER_CATALOG: FilterDef[] = [
  {
    id: 'gauss', en: 'Gaussian Blur', label: '高斯模糊', kind: 0, menu: 'blur', hasSeed: false,
    params: [{ label: '半径', min: 0.1, max: 250, step: 0.1, def: 5, unit: 'px' }],
  },
  {
    id: 'box', en: 'Box Blur', label: '方框模糊', kind: 1, menu: 'blur', hasSeed: false,
    params: [{ label: '半径', min: 1, max: 200, step: 1, def: 8, unit: 'px' }],
  },
  {
    id: 'motion', en: 'Motion Blur', label: '动感模糊', kind: 2, menu: 'blur', hasSeed: false,
    params: [
      { label: '角度', min: -360, max: 360, step: 1, def: 0, unit: '°' },
      { label: '距离', min: 1, max: 500, step: 1, def: 30, unit: 'px' },
    ],
  },
  {
    id: 'usm', en: 'Unsharp Mask', label: 'USM 锐化', kind: 3, menu: 'sharpen', hasSeed: false,
    params: [
      { label: '数量', min: 1, max: 500, step: 1, def: 100, unit: '%' },
      { label: '半径', min: 0.1, max: 100, step: 0.1, def: 2, unit: 'px' },
      { label: '阈值', min: 0, max: 255, step: 1, def: 3, unit: '级' },
    ],
  },
  {
    id: 'smartSharpen', en: 'Smart Sharpen', label: '智能锐化', kind: 4, menu: 'sharpen', hasSeed: false,
    params: [
      { label: '数量', min: 1, max: 500, step: 1, def: 100, unit: '%' },
      { label: '半径', min: 0.1, max: 100, step: 0.1, def: 2, unit: 'px' },
      { label: '降噪', min: 0, max: 100, step: 1, def: 20, unit: '%' },
    ],
  },
  {
    id: 'highpass', en: 'High Pass', label: '高反差保留', kind: 5, menu: 'sharpen', hasSeed: false,
    params: [{ label: '半径', min: 0.1, max: 100, step: 0.1, def: 3, unit: 'px' }],
  },
  {
    id: 'addNoise', en: 'Add Noise', label: '添加杂色', kind: 6, menu: 'noise', hasSeed: true,
    params: [
      { label: '数量', min: 0, max: 400, step: 1, def: 20, unit: '%' },
      { label: '高斯分布', min: 0, max: 1, step: 1, def: 0, unit: '' },
      { label: '单色', min: 0, max: 1, step: 1, def: 1, unit: '' },
    ],
  },
  {
    id: 'median', en: 'Median', label: '中间值', kind: 7, menu: 'noise', hasSeed: false,
    params: [{ label: '半径', min: 1, max: 10, step: 1, def: 1, unit: 'px' }],
  },
  {
    id: 'dust', en: 'Dust & Scratches', label: '蒙尘与划痕', kind: 8, menu: 'noise', hasSeed: false,
    params: [
      { label: '半径', min: 1, max: 10, step: 1, def: 2, unit: 'px' },
      { label: '阈值', min: 0, max: 255, step: 1, def: 20, unit: '级' },
    ],
  },
  {
    id: 'reduceNoise', en: 'Reduce Noise', label: '减少杂色', kind: 9, menu: 'noise', hasSeed: false,
    params: [{ label: '强度', min: 1, max: 10, step: 1, def: 3, unit: '' }],
  },
  {
    id: 'mosaic', en: 'Mosaic', label: '彩块化', kind: 10, menu: 'pixelate', hasSeed: false,
    params: [{ label: '单元格大小', min: 2, max: 200, step: 1, def: 10, unit: 'px' }],
  },
  {
    id: 'crystallize', en: 'Crystallize', label: '晶格化', kind: 11, menu: 'pixelate', hasSeed: true,
    params: [
      { label: '单元格大小', min: 3, max: 200, step: 1, def: 15, unit: 'px' },
    ],
  },
  {
    id: 'mezzotint', en: 'Mezzotint', label: '铜版画', kind: 12, menu: 'pixelate', hasSeed: true,
    params: [{ label: '类型(细/中/粗)', min: 0, max: 2, step: 1, def: 1, unit: '' }],
  },
  {
    id: 'fragment', en: 'Fragment', label: '碎片', kind: 13, menu: 'pixelate', hasSeed: false, params: [],
  },
  {
    id: 'pointillize', en: 'Pointillize', label: '点状化', kind: 14, menu: 'pixelate', hasSeed: true,
    params: [{ label: '单元格大小', min: 3, max: 200, step: 1, def: 15, unit: 'px' }],
  },
  {
    id: 'halftone', en: 'Color Halftone', label: '彩色半调', kind: 15, menu: 'pixelate', hasSeed: false,
    params: [{ label: '最大半径', min: 4, max: 127, step: 1, def: 8, unit: 'px' }],
  },
  {
    id: 'findEdges', en: 'Find Edges', label: '查找边缘', kind: 16, menu: 'stylize', hasSeed: false, params: [],
  },
  {
    id: 'emboss', en: 'Emboss', label: '浮雕效果', kind: 17, menu: 'stylize', hasSeed: false,
    params: [
      { label: '角度', min: -360, max: 360, step: 1, def: 135, unit: '°' },
      { label: '高度', min: 1, max: 100, step: 1, def: 2, unit: 'px' },
      { label: '数量', min: 1, max: 500, step: 1, def: 100, unit: '%' },
    ],
  },
  {
    id: 'wind', en: 'Wind', label: '风', kind: 18, menu: 'stylize', hasSeed: false,
    params: [
      { label: '方向(左/右)', min: 0, max: 1, step: 1, def: 1, unit: '' },
      { label: '强度', min: 1, max: 100, step: 1, def: 20, unit: '' },
    ],
  },
  {
    id: 'tiles', en: 'Tiles', label: '拼贴', kind: 19, menu: 'stylize', hasSeed: true,
    params: [
      { label: '拼贴宽', min: 8, max: 500, step: 1, def: 64, unit: 'px' },
      { label: '拼贴高', min: 8, max: 500, step: 1, def: 64, unit: 'px' },
      { label: '偏移', min: 0, max: 99, step: 1, def: 20, unit: '%' },
    ],
  },
  {
    id: 'exposure', en: 'Solarize', label: '曝光过度', kind: 20, menu: 'stylize', hasSeed: false, params: [],
  },
  {
    id: 'clouds', en: 'Clouds', label: '云彩', kind: 21, menu: 'render', hasSeed: true, params: [],
  },
  {
    id: 'diffClouds', en: 'Difference Clouds', label: '分层云彩', kind: 22, menu: 'render', hasSeed: true, params: [],
  },
  {
    id: 'lensFlare', en: 'Lens Flare', label: '镜头光晕', kind: 23, menu: 'render', hasSeed: false,
    params: [
      { label: '中心X', min: -2000, max: 8000, step: 1, def: 1000, unit: '' },
      { label: '中心Y', min: -2000, max: 8000, step: 1, def: 1000, unit: '' },
      { label: '亮度', min: 10, max: 300, step: 1, def: 100, unit: '%' },
    ],
  },
  {
    id: 'minimum', en: 'Minimum', label: '最小值', kind: 24, menu: 'other', hasSeed: false,
    params: [
      { label: '半径', min: 1, max: 100, step: 1, def: 2, unit: 'px' },
      { label: '圆盘', min: 0, max: 1, step: 1, def: 1, unit: '' },
    ],
  },
  {
    id: 'maximum', en: 'Maximum', label: '最大值', kind: 25, menu: 'other', hasSeed: false,
    params: [
      { label: '半径', min: 1, max: 100, step: 1, def: 2, unit: 'px' },
      { label: '圆盘', min: 0, max: 1, step: 1, def: 1, unit: '' },
    ],
  },
  {
    id: 'offset', en: 'Offset', label: '位移', kind: 26, menu: 'other', hasSeed: false,
    params: [
      { label: '水平', min: -2000, max: 2000, step: 1, def: 50, unit: 'px' },
      { label: '垂直', min: -2000, max: 2000, step: 1, def: 0, unit: 'px' },
    ],
  },
  {
    // M20d 镜头校正（lens_ops；色差/暗角中点 v1 固定默认登记）
    id: 'lensCorrection', en: 'Lens Correction', label: '镜头校正', kind: 28, menu: 'other', hasSeed: false,
    params: [
      { label: ' Remove Distortion', min: -100, max: 100, step: 1, def: 0, unit: '' },
      { label: '垂直透视', min: -100, max: 100, step: 1, def: 0, unit: '' },
      { label: '水平透视', min: -100, max: 100, step: 1, def: 0, unit: '' },
      { label: '角度', min: -180, max: 180, step: 0.5, def: 0, unit: '°' },
      { label: '缩放', min: 50, max: 150, step: 1, def: 100, unit: '%' },
      { label: '暗角数量', min: -100, max: 100, step: 1, def: 0, unit: '' },
    ],
  },
  {
    // M22 颜色查找：参数 = 表选择（0..内置数-1 / 100+用户槽）；
    // 对话框按 kind===30 渲染预设 chips（内置外观 + 已导入用户表）。
    id: 'colorLookup', en: 'Color Lookup', label: '颜色查找', kind: 30, menu: 'adjust', hasSeed: false,
    params: [{ label: '预设', min: 0, max: 115, step: 1, def: 0, unit: '' }],
  },
];

export const FILTER_MENU_GROUPS: string[] = ['blur', 'sharpen', 'noise', 'pixelate', 'stylize', 'render', 'other', 'adjust'];
export const FILTER_MENU_LABELS: Record<string, string> = {
  'blur': '模糊',
  'sharpen': '锐化',
  'noise': '杂色',
  'pixelate': '像素化',
  'stylize': '风格化',
  'render': '渲染',
  'other': '其他',
  'adjust': '调整',
};
