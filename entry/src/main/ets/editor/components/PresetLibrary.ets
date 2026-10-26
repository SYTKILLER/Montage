// P1 预设库面板（窗口 › 预设库；M25 资源半）：色板/渐变/图案/笔刷四库。
// - 库数据由宿主（Index.ets）持有（@Link）+ AppPrefs 持久化（色板/渐变/笔刷参数全量，
//   图案/笔刷 tip 像素在引擎进程库——重启丢失，清单 stale 由宿主清理）。
// - 缩略图：引擎 getPresetThumb 同步出 RGBA → image.createPixelMapSync（d.ts:3899 已核验），
//   进程内缓存 Map（rev 键失效）；渐变缩略 = Canvas createLinearGradient + addColorStop
//   （引擎 evalStops 同式分段，addColorStop offset 0..1）。
// - 交互：色板点击=设前景、渐变点击=应用为当前渐变、图案点击=设为图案图章源、
//   笔刷点击=应用参数；各库项长按=删除（宿主弹确认→dropPresetPixels 释放像素）。

import { image } from '@kit.ImageKit';
import { CompositorEngine, PresetSwatchDTO, PresetGroupDTO, PresetGradientDTO, BrushPresetDTO, LibPatternDTO } from 'core';

function hexOf(r: number, g: number, b: number): string {
  const q = (v: number): string => {
    const c = Math.max(0, Math.min(255, Math.round(v))).toString(16);
    return c.length < 2 ? '0' + c : c;
  };
  return '#' + q(r) + q(g) + q(b);
}

@Component
export struct PresetLibrary {
  @Link swatchLib: PresetGroupDTO[];
  @Link gradientLib: PresetGradientDTO[];
  @Link brushLib: BrushPresetDTO[];
  @Link patternLib: LibPatternDTO[];
  onPickSwatch: (r: number, g: number, b: number) => void = () => {};
  onUseGradient: (idx: number) => void = () => {};
  onEditGradient: (idx: number) => void = () => {};
  onNewGradient: () => void = () => {};
  onUsePattern: (id: string) => void = () => {};
  onAddToDoc: (id: string) => void = () => {};
  onUseBrush: (b: BrushPresetDTO) => void = () => {};
  onImport: (kind: number) => void = () => {};
  onDelete: (kind: number, idx: number, sub: number) => void = () => {};
  @State private tab: number = 0;
  @State private search: string = '';
  @State private thumbRev: number = 0;
  private thumbs: Map<string, image.PixelMap> = new Map();

  private toast(msg: string): void {
    this.getUIContext().getPromptAction().showToast({ message: msg, duration: 1600 });
  }

  aboutToAppear(): void {
    this.thumbRev++;
  }

  private thumbFor(id: string, isTip: boolean, engine: CompositorEngine | undefined): image.PixelMap | undefined {
    if (engine === undefined) {
      return undefined;
    }
    const hit = this.thumbs.get(id);
    if (hit !== undefined) {
      return hit;
    }
    const dto = engine.getPresetThumb(isTip ? 1 : 0, id);
    if (dto === null) {
      return undefined;
    }
    if (dto.w <= 0 || dto.h <= 0) {
      return undefined;
    }
    try {
      const pm = image.createPixelMapSync(dto.rgba, {
        size: { width: dto.w, height: dto.h },
        srcPixelFormat: image.PixelMapFormat.RGBA_8888
      });
      this.thumbs.set(id, pm);
      return pm;
    } catch (e) {
      return undefined;
    }
  }

  private matches(name: string): boolean {
    return this.search.length === 0 || name.toLowerCase().includes(this.search.toLowerCase());
  }

  @Builder
  private tabBtn(label: string, idx: number) {
    Text(label)
      .fontSize(12)
      .fontColor(this.tab === idx ? '#FFFFFF' : '#666666')
      .backgroundColor(this.tab === idx ? '#0A66C2' : '#00000000')
      .padding({ left: 10, right: 10, top: 4, bottom: 4 })
      .borderRadius(10)
      .onClick(() => {
        this.tab = idx;
      })
  }

  @Builder
  private swatchCells() {
    ForEach(this.swatchLib, (g: PresetGroupDTO, gi: number) => {
      if (this.matches(g.name) || g.name.length === 0) {
        Column() {
          Text(g.name.length > 0 ? g.name : '色板')
            .fontSize(11)
            .fontColor('#888888')
            .margin({ top: 6, bottom: 2 })
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
          Flex({ wrap: FlexWrap.Wrap }) {
            ForEach(g.swatches, (s: PresetSwatchDTO, si: number) => {
              if (this.matches(s.name)) {
                Column() {
                  Circle({ width: 26, height: 26 })
                    .fill(hexOf(s.r, s.g, s.b))
                    .stroke('#DDDDDD')
                    .strokeWidth(1)
                  Text(s.name.length > 0 ? s.name : '色')
                    .fontSize(9)
                    .fontColor('#666666')
                    .maxLines(1)
                    .constraintSize({ maxWidth: 44 })
                    .textOverflow({ overflow: TextOverflow.Ellipsis })
                }
                .margin(3)
                .onClick(() => {
                  this.onPickSwatch(s.r, s.g, s.b);
                  this.toast('前景色 ' + hexOf(s.r, s.g, s.b));
                })
                .gesture(LongPressGesture().onAction(() => {
                  this.onDelete(0, gi, si);
                  this.toast('已删除「' + (s.name.length > 0 ? s.name : '色') + '」');
                }))
              }
            }, (s: PresetSwatchDTO, si: number) => `${gi}_${si}_${s.r}_${s.g}_${s.b}`)
          }
          .width('100%')
        }
        .width('100%')
      }
    }, (g: PresetGroupDTO, gi: number) => `g${gi}_${g.name}_${g.swatches.length}`)
  }

  @Builder
  private gradientCells() {
    Row() {
      Text('+ 新建渐变')
        .fontSize(12)
        .fontColor('#0A66C2')
        .padding(6)
        .onClick(() => {
          this.onNewGradient();
        })
      Blank()
      Text('导入 .grd…')
        .fontSize(12)
        .fontColor('#0A66C2')
        .padding(6)
        .onClick(() => {
          this.onImport(1);
        })
    }
    .width('100%')

    ForEach(this.gradientLib, (g: PresetGradientDTO, i: number) => {
      if (this.matches(g.name)) {
        Column() {
          Canvas(this.gCtx(g.name, i))
            .width('100%')
            .height(18)
            .borderRadius(3)
            .onReady(() => {
              this.drawGradient(g.name, i);
            })
          Row() {
            Text(g.name)
              .fontSize(10)
              .fontColor('#555555')
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
            Blank()
            Text('编辑')
              .fontSize(10)
              .fontColor('#0A66C2')
              .padding({ left: 6, right: 6 })
              .onClick(() => {
                this.onEditGradient(i);
              })
          }
          .width('100%')
        }
        .width('100%')
        .margin({ top: 4 })
        .onClick(() => {
          this.onUseGradient(i);
          this.toast('渐变「' + g.name + '」已应用');
        })
        .gesture(LongPressGesture().onAction(() => {
          this.onDelete(1, i, 0);
          this.toast('已删除「' + g.name + '」');
        }))
      }
    }, (g: PresetGradientDTO, i: number) => `grad${i}_${g.name}_${g.stops.length}_${g.transparency.length}_${this.thumbRev}`)
  }

  private gCache: Map<string, CanvasRenderingContext2D> = new Map();

  private gCtx(name: string, i: number): CanvasRenderingContext2D {
    const key = name + '_' + i;
    let c = this.gCache.get(key);
    if (c === undefined) {
      c = new CanvasRenderingContext2D(new RenderingContextSettings(true));
      this.gCache.set(key, c);
    }
    return c;
  }

  private drawGradient(name: string, i: number): void {
    const g = this.gradientLib[i];
    if (g === undefined || g.stops.length < 2) {
      return;
    }
    const c = this.gCtx(name, i);
    const grad = c.createLinearGradient(0, 0, c.width, 0);
    for (const s of g.stops) {
      const r = Math.round((s[2] ?? 0) * 255);
      const gg = Math.round((s[3] ?? 0) * 255);
      const b = Math.round((s[4] ?? 0) * 255);
      const a = this.stopAlpha(g, s[0]);
      grad.addColorStop(Math.max(0, Math.min(1, s[0])), `rgba(${r},${gg},${b},${a})`);
    }
    c.fillStyle = grad;
    c.fillRect(0, 0, c.width, c.height);
  }

  private stopAlpha(g: PresetGradientDTO, t: number): number {
    if (g.transparency.length === 0) {
      return 1;
    }
    let best = 1;
    // 分段线性（引擎 apply_opacity 简化：取最近区间插值）
    const tr = g.transparency;
    if (t <= tr[0][0]) {
      return tr[0][1];
    }
    if (t >= tr[tr.length - 1][0]) {
      return tr[tr.length - 1][1];
    }
    for (let k = 1; k < tr.length; k++) {
      if (t <= tr[k][0]) {
        const t0 = tr[k - 1][0];
        const t1 = tr[k][0];
        const u = t1 > t0 ? (t - t0) / (t1 - t0) : 1;
        return tr[k - 1][1] + (tr[k][1] - tr[k - 1][1]) * u;
      }
    }
    return best;
  }

  @Builder
  private patternCells() {
    Row() {
      Text('导入 .pat…')
        .fontSize(12)
        .fontColor('#0A66C2')
        .padding(6)
        .onClick(() => {
          this.onImport(2);
        })
    }
    .width('100%')

    Flex({ wrap: FlexWrap.Wrap }) {
      ForEach(this.patternLib, (p: LibPatternDTO, i: number) => {
        if (this.matches(p.name)) {
          Column() {
            Image(this.thumbFor(p.id, false, this.engineRef))
              .width(44)
              .height(44)
              .objectFit(ImageFit.Contain)
              .borderRadius(3)
              .backgroundColor('#F2F2F2')
            Text(p.name)
              .fontSize(9)
              .fontColor('#666666')
              .maxLines(1)
              .constraintSize({ maxWidth: 48 })
              .textOverflow({ overflow: TextOverflow.Ellipsis })
            Text('+文档')
              .fontSize(9)
              .fontColor('#0A66C2')
              .onClick(() => {
                this.onAddToDoc(p.id);
              })
          }
          .margin(3)
          .onClick(() => {
            this.onUsePattern(p.id);
            this.toast('图案图章源「' + p.name + '」');
          })
          .gesture(LongPressGesture().onAction(() => {
            this.onDelete(2, i, 0);
            this.toast('已删除「' + p.name + '」');
          }))
        }
      }, (p: LibPatternDTO, i: number) => `pat${i}_${p.id}_${this.thumbRev}`)
    }
    .width('100%')
  }

  @Builder
  private brushCells() {
    Row() {
      Text('导入 .abr…')
        .fontSize(12)
        .fontColor('#0A66C2')
        .padding(6)
        .onClick(() => {
          this.onImport(3);
        })
    }
    .width('100%')

    Flex({ wrap: FlexWrap.Wrap }) {
      ForEach(this.brushLib, (b: BrushPresetDTO, i: number) => {
        if (this.matches(b.name)) {
          Column() {
            if (b.tipId.length > 0) {
              Image(this.thumbFor(b.tipId, true, this.engineRef))
                .width(44)
                .height(44)
                .objectFit(ImageFit.Contain)
                .borderRadius(3)
                .backgroundColor('#F2F2F2')
            } else {
              SymbolGlyph($r('sys.symbol.paintbrush'))
                .fontSize(30)
                .fontColor(['#888888'])
                .margin(6)
            }
            Text(b.name)
              .fontSize(9)
              .fontColor('#666666')
              .maxLines(1)
              .constraintSize({ maxWidth: 52 })
              .textOverflow({ overflow: TextOverflow.Ellipsis })
            Text(Math.round(b.size) + 'px')
              .fontSize(8)
              .fontColor('#999999')
          }
          .margin(3)
          .onClick(() => {
            this.onUseBrush(b);
            this.toast('笔刷「' + b.name + '」');
          })
          .gesture(LongPressGesture().onAction(() => {
            this.onDelete(3, i, 0);
            this.toast('已删除「' + b.name + '」');
          }))
        }
      }, (b: BrushPresetDTO, i: number) => `br${i}_${b.name}_${b.size}_${this.thumbRev}`)
    }
    .width('100%')
  }

  engineRef: CompositorEngine | undefined = undefined;

  build() {
    Column() {
      Row() {
        this.tabBtn('色板', 0)
        this.tabBtn('渐变', 1)
        this.tabBtn('图案', 2)
        this.tabBtn('笔刷', 3)
        Blank()
      }
      .width('100%')

      Row() {
        SymbolGlyph($r('sys.symbol.plus_magnifyingglass'))
          .fontSize(14)
          .fontColor(['#999999'])
        TextInput({ text: this.search, placeholder: '搜索预设' })
          .fontSize(12)
          .height(28)
          .layoutWeight(1)
          .backgroundColor('#F2F2F2')
          .onChange((v: string) => {
            this.search = v;
          })
      }
      .width('100%')
      .margin({ top: 4, bottom: 4 })

      Scroll() {
        Column() {
          if (this.tab === 0) {
            Row() {
              Text('导入 .aco/.ase…')
                .fontSize(12)
                .fontColor('#0A66C2')
                .padding(6)
                .onClick(() => {
                  this.onImport(0);
                })
            }
            .width('100%')
            this.swatchCells()
          } else if (this.tab === 1) {
            this.gradientCells()
          } else if (this.tab === 2) {
            this.patternCells()
          } else {
            this.brushCells()
          }
          if (this.tab === 0 && this.swatchLib.length === 0) {
            Text('空库：导入 .aco/.ase 或右键画布取色后添加')
              .fontSize(11)
              .fontColor('#999999')
              .margin(8)
          }
        }
        .width('100%')
      }
      .layoutWeight(1)
      .align(Alignment.Top)
      .scrollBar(BarState.Auto)
    }
    .width('100%')
    .height('100%')
    .padding(6)
  }
}
