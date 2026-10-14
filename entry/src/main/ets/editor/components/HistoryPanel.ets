/**
 * M15b 历史面板（06 规划 M15）：线性状态列表 + 当前指针高亮 + 点选跳转。
 * labels[0] = 初始态（空串 → 渲染「打开 / 新建」）；current 之后的状态灰显（可重做）。
 * 点选 = 连续 undo/redo 至目标（引擎 setHistoryState，整步快照精确）。
 */
import { HistoryListData } from 'core';

/** 引擎历史标签 → 中文（PS 术语；未命中回退原文）。 */
const LABEL_ZH: Record<string, string> = {
  'brush': '画笔',
  'eraser': '橡皮擦',
  'clone stamp': '仿制图章',
  'history brush': '历史画笔',
  'mask brush': '蒙版画笔',
  'quick mask': '快速蒙版',
  'add layer': '新建图层',
  'add adjustment': '新建调整图层',
  'add mask': '添加蒙版',
  'add text': '新建文字层',
  'adjust params': '调整参数',
  'adjustment kind': '调整类型',
  'align layers': '对齐图层',
  'bucket fill': '油漆桶',
  'canvas size': '画布大小',
  'clear': '清除',
  'clipping': '剪贴蒙版',
  'curves points': '曲线控制点',
  'cut': '剪切',
  'delete channel': '删除通道',
  'deselect': '取消选择',
  'distribute layers': '分布图层',
  'duplicate layer': '复制图层',
  'ellipse select': '椭圆选框',
  'fill': '填充',
  'fill layer params': '填充图层参数',
  'filter': '滤镜',
  'flatten image': '拼合图像',
  'free transform': '自由变换',
  'gradient': '渐变',
  'group layer': '图层编组',
  'image size': '图像大小',
  'invert selection': '反选',
  'load selection': '载入选区',
  'magic wand': '魔棒',
  'mask enable': '启用蒙版',
  'merge down': '合并向下',
  'merge visible': '合并可见',
  'move selection': '移动选区',
  'new fill layer': '新建填充图层',
  'new group': '新建组',
  'paste': '粘贴',
  'polygon select': '多边形套索',
  'quick mask toggle': '快速蒙版切换',
  'rasterize text': '栅格化文字',
  'rasterize text (paint)': '栅格化文字（上画笔）',
  'rect select': '矩形选框',
  'reorder layer': '调整图层顺序',
  'reselect': '重新选择',
  'reveal all': '显示全部',
  'rotate arbitrary': '任意角度旋转',
  'select all': '全部选择',
  'shape': '形状',
  'smooth selection': '平滑选区',
  'stamp visible': '盖印',
  'store selection': '存储选区',
  'stroke': '描边',
  'text edit': '编辑文字',
  'text style': '文字样式',
  'trim': '裁切',
  'ungroup': '取消编组',
};

@Component
export struct HistoryPanel {
  @Prop @Watch('noop') labels: string[] = [''];
  @Prop @Watch('noop') current: number = 0;
  onPick: (index: number) => void = (_index: number): void => {};
  // 引用型回调字段被 build 读取需触发刷新的场景：current/labels 变化经 @Prop 同步重建 ForEach

  noop(): void {
  }

  private labelOf(idx: number): string {
    const l = this.labels[idx];
    if (idx === 0 || l === undefined || l.length === 0) {
      return '打开 / 新建';
    }
    const zh = LABEL_ZH[l];
    return zh !== undefined ? zh : l;
  }

  build() {
    Column({ space: 4 }) {
      Text('历史记录')
        .fontSize(12)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .width('100%')
        .padding({ left: 8, top: 4 })
      List({ space: 2 }) {
        ForEach(this.labels, (label: string, idx: number) => {
          ListItem() {
            Row({ space: 6 }) {
              SymbolGlyph(idx < this.current ? $r('sys.symbol.clock') : idx === this.current
                ? $r('sys.symbol.checkmark_circle_fill') : $r('sys.symbol.arrow_counterclockwise_clock'))
                .fontSize(14)
                .fontColor([idx === this.current ? '#0066CC' : '#8A8A8A'])
              Text(this.labelOf(idx))
                .fontSize(12)
                .maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
                .fontColor(idx > this.current ? '#9A9A9A' : $r('app.color.text_primary'))
                .layoutWeight(1)
            }
            .width('100%')
            .padding({ left: 8, right: 6, top: 5, bottom: 5 })
            .borderRadius(6)
            .backgroundColor(idx === this.current ? '#14006BD9' : '#00000000')
            .onClick((): void => this.onPick(idx))
          }
        }, (label: string, idx: number): string => `${idx}|${label}|${this.current}`)
      }
      .width('100%')
      .layoutWeight(1)
      .scrollBar(BarState.Auto)
    }
    .width('100%')
    .padding(4)
    .borderRadius(10)
    .backgroundColor($r('app.color.bar_background'))
  }
}
