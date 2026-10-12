import { hilog } from '@kit.PerformanceAnalysisKit';
import { image } from '@kit.ImageKit';
import { LayerDTO } from 'core';
import { BLEND_LABELS } from '../MenuModel';

/** 缩略图缓存条目（token 对齐引擎侧内容版本；pm=null = 引擎确认空内容，也要入缓存防旧图残留）。 */
export interface ThumbEntry {
  token: number;
  pm: Object | null;
}

/**
 * 图层面板（05 §6 正式化）：外观条（不透明度+混合）+ 调整层参数区（活动层为调整层时）
 * + 缩略图列表（token 编入 key，异步到位后经 thumbVersion 触发重建）+ 操作条 + 右键菜单。
 */
@Component
export struct LayersPanel {
  @Link layers: LayerDTO[];
  @Link layerOpacity: number;   // 0-100（活动层外观条）
  @Link blendSel: number;       // 活动层混合模式索引
  @Link adjBlack: number;       // 0-255（调整层参数区）
  @Link adjWhite: number;
  @Link adjGamma: number;
  @Prop thumbVersion: number = 0;
  @Prop panelVersion: number = 0;  // M14a：面板序变化强制整表重建（ForEach 同 key 复用不移位）
  @Prop docOpen: boolean = false;
  getThumb: (id: number) => Object | null = (_id: number): Object | null => null;
  onSelect: (id: number) => void = (_id: number) => {};
  onToggleVisible: (l: LayerDTO) => void = (_l: LayerDTO) => {};
  onToggleMaskEnabled: (l: LayerDTO) => void = (_l: LayerDTO) => {};
  onToggleClipping: (l: LayerDTO) => void = (_l: LayerDTO) => {};
  onMove: (l: LayerDTO, up: boolean) => void = (_l: LayerDTO, _up: boolean) => {};
  onDelete: () => void = () => {};
  onAdd: () => void = () => {};
  onAddMask: () => void = () => {};
  onAddAdjustment: () => void = () => {};
  onRequestRename: (l: LayerDTO) => void = (_l: LayerDTO) => {};
  // M14a 图层编组
  onToggleCollapse: (l: LayerDTO) => void = (_l: LayerDTO) => {};
  onGroup: () => void = () => {};
  onUngroup: () => void = () => {};
  onOpacityChange: (v: number) => void = (_v: number) => {};
  onBlendChange: (idx: number) => void = (_idx: number) => {};
  onAdjustChange: (b: number, w: number, g: number) => void =
    (_b: number, _w: number, _g: number) => {};

  private active(): LayerDTO | undefined {
    return this.layers.find((l: LayerDTO): boolean => l.isActive);
  }

  @Builder
  layerThumb(l: LayerDTO) {
    Stack() {
      // 透明底棋盘（简化：双色细网格用两块三角？v1 用中性底+内容图）
      Column()
        .width(40)
        .height(40)
        .borderRadius(4)
        .backgroundColor($r('app.color.surface'))
        .border({ width: 1, color: $r('app.color.border') })
      if (this.getThumb(l.id) !== null) {
        Image(this.getThumb(l.id) as image.PixelMap)
          .width(38)
          .height(38)
          .borderRadius(3)
          .objectFit(ImageFit.Contain)
      }
    }
    .width(42)
    .height(42)
  }

  @Builder
  rowMenu(l: LayerDTO) {
    Menu() {
      MenuItem({ content: '重命名' })
        .onClick((): void => this.onRequestRename(l))
      MenuItem({ content: l.visible ? '隐藏' : '显示' })
        .onClick((): void => this.onToggleVisible(l))
      MenuItem({ content: '上移一层' })
        .onClick((): void => this.onMove(l, true))
      MenuItem({ content: '下移一层' })
        .onClick((): void => this.onMove(l, false))
      if (l.hasMask) {
        MenuItem({ content: l.maskEnabled ? '停用蒙版' : '启用蒙版' })
          .onClick((): void => this.onToggleMaskEnabled(l))
      }
      MenuItem({ content: l.clipping ? '释放剪贴蒙版' : '创建剪贴蒙版' })
        .onClick((): void => this.onToggleClipping(l))
      MenuItem({ content: '删除图层' })
        .onClick((): void => this.onDelete())
    }
  }

  build() {
    Column({ space: 8 }) {
      Text('图层')
        .fontSize(13)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .width('100%')

      // 外观条：不透明度 + 混合模式（作用于活动层）
      Row({ space: 6 }) {
        Text('不透明度')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
        Slider({ value: this.layerOpacity, min: 0, max: 100, step: 1 })
          .layoutWeight(1)
          .height(24)
          .onChange((v: number, mode: SliderChangeMode): void => {
            if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
              this.onOpacityChange(v / 100);
            }
          })
        Text(`${Math.round(this.layerOpacity)}%`)
          .fontSize(10)
          .fontColor($r('app.color.text_primary'))
          .width(34)
      }
      .width('100%')
      Row({ space: 6 }) {
        Text('混合')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
        Select(this.blendOptions())
          .selected(this.blendSel)
          .value(BLEND_LABELS[this.blendSel] ?? '正常')
          .onSelect((index: number): void => {
            this.onBlendChange(index);
          })
      }
      .width('100%')

      // 调整层参数区（M6a：活动层为 Levels 时）
      if (this.active()?.isAdjustment === true) {
        Column({ space: 4 }) {
          Text('调整（色阶）')
            .fontSize(11)
            .fontColor($r('app.color.text_secondary'))
          Row({ space: 6 }) {
            Text('黑').fontSize(11).fontColor($r('app.color.text_secondary'))
            Slider({ value: this.adjBlack, min: 0, max: 250, step: 1 })
              .layoutWeight(1).height(22)
              .onChange((v: number, mode: SliderChangeMode): void => {
                if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                  this.onAdjustChange(v / 255, this.adjWhite / 255, this.adjGamma);
                }
              })
            Text(`${Math.round(this.adjBlack)}`).fontSize(10).width(30)
              .fontColor($r('app.color.text_primary'))
          }
          .width('100%')
          Row({ space: 6 }) {
            Text('白').fontSize(11).fontColor($r('app.color.text_secondary'))
            Slider({ value: this.adjWhite, min: 5, max: 255, step: 1 })
              .layoutWeight(1).height(22)
              .onChange((v: number, mode: SliderChangeMode): void => {
                if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                  this.onAdjustChange(this.adjBlack / 255, v / 255, this.adjGamma);
                }
              })
            Text(`${Math.round(this.adjWhite)}`).fontSize(10).width(30)
              .fontColor($r('app.color.text_primary'))
          }
          .width('100%')
          Row({ space: 6 }) {
            Text('γ  ').fontSize(11).fontColor($r('app.color.text_secondary'))
            Slider({ value: this.adjGamma, min: 0.1, max: 10, step: 0.05 })
              .layoutWeight(1).height(22)
              .onChange((v: number, mode: SliderChangeMode): void => {
                if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                  this.onAdjustChange(this.adjBlack / 255, this.adjWhite / 255, v);
                }
              })
            Text(this.adjGamma.toFixed(2)).fontSize(10).width(30)
              .fontColor($r('app.color.text_primary'))
          }
          .width('100%')
        }
        .width('100%')
        .padding(8)
        .borderRadius(8)
        .backgroundColor($r('app.color.accent_container'))
      }

      // 图层列表（自顶向下 = PS 惯例）
      List({ space: 4 }) {
        ForEach(this.reversed(), (l: LayerDTO) => {
          ListItem() {
            Row({ space: 8 }) {
              // M14a：组折叠箭头（组节点）/ 缩进（子层）
              if (l.isGroup) {
                SymbolGlyph(l.collapsed ? $r('sys.symbol.chevron_right') : $r('sys.symbol.chevron_down'))
                  .fontSize(14)
                  .fontColor([$r('app.color.text_primary')])
                  .width(18)
                  .onClick((): void => this.onToggleCollapse(l))
              } else {
                Row().width(18).height(1).margin({ left: this.depthOf(l) * 14 })
              }
              SymbolGlyph(l.visible ? $r('sys.symbol.eye') : $r('sys.symbol.eye_slash'))
                .fontSize(16)
                .fontColor([l.visible ? $r('app.color.text_primary') : $r('app.color.icon_muted')])
                .onClick((): void => this.onToggleVisible(l))
              if (l.isGroup) {
                Column() {
                  SymbolGlyph($r('sys.symbol.folder'))
                    .fontSize(20)
                    .fontColor([$r('app.color.accent')])
                }
                .width(42)
                .height(42)
                .justifyContent(FlexAlign.Center)
              } else {
                this.layerThumb(l)
              }
              Column({ space: 2 }) {
                Row({ space: 4 }) {
                  if (l.isAdjustment) {
                    SymbolGlyph($r('sys.symbol.adjustment'))
                      .fontSize(11)
                      .fontColor([$r('app.color.accent')])
                  }
                  if (l.clipping) {
                    SymbolGlyph($r('sys.symbol.arrow_down'))
                      .fontSize(11)
                      .fontColor([$r('app.color.accent')])
                  }
                  Text(l.name)
                    .fontSize(12)
                    .fontColor($r('app.color.text_primary'))
                    .maxLines(1)
                    .textOverflow({ overflow: TextOverflow.Ellipsis })
                }
                Row({ space: 6 }) {
                  Text(`${BLEND_LABELS[l.blendMode] ?? '正常'} · ${Math.round(l.opacity * 100)}%`)
                    .fontSize(10)
                    .fontColor($r('app.color.text_secondary'))
                  if (l.hasMask) {
                    Text('蒙')
                      .fontSize(9)
                      .width(16).height(16).borderRadius(3)
                      .textAlign(TextAlign.Center)
                      .fontColor($r('app.color.on_accent'))
                      .backgroundColor(l.maskEnabled ? $r('app.color.accent') : $r('app.color.icon_muted'))
                      .onClick((): void => this.onToggleMaskEnabled(l))
                  }
                }
              }
              .alignItems(HorizontalAlign.Start)
              .layoutWeight(1)
              .onClick((): void => this.onSelect(l.id))
            }
            .width('100%')
            .padding(6)
            .borderRadius(8)
            .backgroundColor(l.isActive ? $r('app.color.accent_container') : Color.Transparent)
            .border({ width: 1, color: l.isActive ? $r('app.color.accent') : Color.Transparent })
            .bindContextMenu(this.rowMenu(l), ResponseType.RightClick)
            .onClick((): void => this.onSelect(l.id))
          }
        }, (l: LayerDTO): string =>
          `${l.id}|${l.visible}|${l.isActive}|${l.blendMode}|${Math.round(l.opacity * 100)}|` +
          `${l.thumbToken}|${l.hasMask}|${l.maskEnabled}|${l.clipping}|${l.name}|` +
          `${this.getThumb(l.id) !== null ? 'p' : 'n'}|${this.thumbVersion}|` +
          `${l.isGroup ? 'g' : 'n'}|${l.collapsed ? 'c' : 'o'}|${l.parentId}|${this.panelVersion}`)
      }
      .layoutWeight(1)
      .scrollBar(BarState.Auto)

      // 操作条
      Row({ space: 6 }) {
        Button() {
          SymbolGlyph($r('sys.symbol.plus')).fontSize(16)
            .fontColor([$r('app.color.text_primary')])
        }
        .type(ButtonType.Circle)
        .width(30).height(30)
        .backgroundColor(Color.Transparent)
        .onClick((): void => this.onAdd())
        Button() {
          SymbolGlyph($r('sys.symbol.adjustment')).fontSize(16)
            .fontColor([$r('app.color.text_primary')])
        }
        .type(ButtonType.Circle)
        .width(30).height(30)
        .backgroundColor(Color.Transparent)
        .onClick((): void => this.onAddAdjustment())
        Button() {
          SymbolGlyph($r('sys.symbol.square_dashed')).fontSize(16)
            .fontColor([$r('app.color.text_primary')])
        }
        .type(ButtonType.Circle)
        .width(30).height(30)
        .backgroundColor(Color.Transparent)
        .enabled(this.active() !== undefined)
        .onClick((): void => this.onAddMask())
        Button() {
          SymbolGlyph($r('sys.symbol.folder_badge_plus')).fontSize(16)
            .fontColor([$r('app.color.text_primary')])
        }
        .type(ButtonType.Circle)
        .width(30).height(30)
        .backgroundColor(Color.Transparent)
        .enabled(this.active() !== undefined)
        .onClick((): void => this.onGroup())
        Button() {
          SymbolGlyph($r('sys.symbol.folder_badge_arrow_left')).fontSize(16)
            .fontColor([$r('app.color.text_primary')])
        }
        .type(ButtonType.Circle)
        .width(30).height(30)
        .backgroundColor(Color.Transparent)
        .enabled(this.active()?.isGroup === true)
        .onClick((): void => this.onUngroup())
        Blank()
        Button() {
          SymbolGlyph($r('sys.symbol.trash')).fontSize(16)
            .fontColor([$r('app.color.danger')])
        }
        .type(ButtonType.Circle)
        .width(30).height(30)
        .backgroundColor(Color.Transparent)
        .enabled(this.active() !== undefined)
        .onClick((): void => this.onDelete())
      }
      .width('100%')
    }
    .width('100%')
    .height('100%')
    .padding(10)
    .backgroundColor($r('app.color.panel_background'))
  }

  private byId(): Map<number, LayerDTO> {
    const m = new Map<number, LayerDTO>();
    for (const l of this.layers) {
      m.set(l.id, l);
    }
    return m;
  }

  // M14a：父链深度（缩进）
  private depthOf(l: LayerDTO): number {
    const m = this.byId();
    let d = 0;
    let cur = l.parentId;
    while (cur !== 0 && d < 8) {
      const p = m.get(cur);
      if (p === undefined) {
        break;
      }
      d++;
      cur = p.parentId;
    }
    return d;
  }

  // M14a：被折叠祖先隐藏
  private hiddenByCollapse(l: LayerDTO): boolean {
    const m = this.byId();
    let cur = l.parentId;
    while (cur !== 0) {
      const p = m.get(cur);
      if (p === undefined) {
        break;
      }
      if (p.collapsed) {
        return true;
      }
      cur = p.parentId;
    }
    return false;
  }

  // M14a：面板树序（PS 惯例）——兄弟按"上层先"（平铺逆序），组节点先于其子树；
  // 折叠组的子层整段隐藏。
  private panelTree(): LayerDTO[] {
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
    const out: LayerDTO[] = [];
    kids.set(0, rootKids);  // 根层孩子统一走 kids（否则 walk(0) 立即 return，触发线性兜底）
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
    // 兜底：仅补父 id 悬空（父层不存在）的层——被折叠隐藏的层不入列
    if (out.length !== this.layers.length) {
      const seen = new Set<number>();
      for (const l of out) {
        seen.add(l.id);
      }
      const ids = new Set<number>();
      for (const l of this.layers) {
        ids.add(l.id);
      }
      for (let i = this.layers.length - 1; i >= 0; i--) {
        const l = this.layers[i];
        if (!seen.has(l.id) && (l.parentId === 0 || !ids.has(l.parentId))) {
          out.push(l);
        }
      }
    }
    return out;
  }

  private reversed(): LayerDTO[] {
    return this.panelTree();
  }

  private blendOptions(): SelectOption[] {
    const opts: SelectOption[] = [];
    for (const lbl of BLEND_LABELS) {
      opts.push({ value: lbl });
    }
    return opts;
  }
}
