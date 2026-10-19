// M18c 路径面板：工作路径（钢笔/选区转路径产物）+ 保存路径列表 + 路径运算入口。
// 操作走引擎命令（pathToSelection/fillPath/strokePath/saveWorkPath/loadSavedPath/
// deleteSavedPath/selectionToWorkPath/defineCustomShape）；工作路径为会话态、保存路径
// 随 .montage v12 持久化。
import { CompositorEngine } from 'core';

@Component
export struct PathsPanel {
  engine: CompositorEngine = new CompositorEngine();
  onChanged: () => void = () => {};
  fgHex: string = '#000000';
  @State savedNames: string[] = [];
  @State hasWork: boolean = false;
  @State customCount: number = 0;

  aboutToAppear(): void {
    this.refresh();
  }

  private refresh(): void {
    const flat = this.engine.getWorkPath();
    this.hasWork = flat.length > 1;
    this.savedNames = this.engine.getSavedPaths();
    this.customCount = this.engine.getCustomShapes().length;
  }

  private fgRgb(): number[] {
    return this.hexToRgb(this.fgHex);
  }

  private hexToRgb(hex: string): number[] {
    const h = hex.replace('#', '');
    const r = parseInt(h.substring(0, 2), 16) / 255;
    const g = parseInt(h.substring(2, 4), 16) / 255;
    const b = parseInt(h.substring(4, 6), 16) / 255;
    if (Number.isNaN(r) || Number.isNaN(g) || Number.isNaN(b)) {
      return [0, 0, 0];
    }
    return [r, g, b];
  }

  build() {
    Column({ space: 6 }) {
      Text('路径')
        .fontSize(13)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
      Row({ space: 6 }) {
        Text(this.hasWork ? '工作路径 ●' : '工作路径（钢笔/选区转路径）')
          .fontSize(12)
          .fontColor(this.hasWork ? $r('app.color.accent') : $r('app.color.text_secondary'))
          .layoutWeight(1)
          .maxLines(1)
      }
      .width('100%')
      Row({ space: 6 }) {
        Button('转选区')
          .fontSize(11).height(26)
          .onClick((): void => {
            if (this.engine.pathToSelection(0)) {
              this.onChanged();
            }
          })
        Button('填充')
          .fontSize(11).height(26)
          .onClick((): void => {
            const c = this.fgRgb();
            if (this.engine.fillPath(c[0], c[1], c[2], 1.0)) {
              this.onChanged();
            }
          })
        Button('描边')
          .fontSize(11).height(26)
          .onClick((): void => {
            const c = this.fgRgb();
            if (this.engine.strokePath(c[0], c[1], c[2], 1.0, 3)) {
              this.onChanged();
            }
          })
        Button('存为路径')
          .fontSize(11).height(26)
          .onClick((): void => {
            if (this.engine.saveWorkPath('')) {
              this.refresh();
              this.onChanged();
            }
          })
      }
      Row({ space: 6 }) {
        Button('选区转路径')
          .fontSize(11).height(26)
          .onClick((): void => {
            if (this.engine.selectionToWorkPath()) {
              this.refresh();
            }
          })
        Button('定义自定形状')
          .fontSize(11).height(26)
          .onClick((): void => {
            if (this.engine.defineCustomShape('')) {
              this.refresh();
              this.onChanged();
            }
          })
      }
      if (this.savedNames.length === 0) {
        Text('无保存路径')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
      } else {
        ForEach(this.savedNames, (n: string) => {
          Row({ space: 6 }) {
            Text(n)
              .fontSize(12)
              .fontColor($r('app.color.text_primary'))
              .layoutWeight(1)
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
            Text('载入')
              .fontSize(11)
              .fontColor($r('app.color.accent'))
              .onClick((): void => {
                if (this.engine.loadSavedPath(this.savedNames.indexOf(n))) {
                  this.refresh();
                }
              })
            SymbolGlyph($r('sys.symbol.trash'))
              .fontSize(14)
              .fontColor([$r('app.color.text_secondary')])
              .onClick((): void => {
                this.engine.deleteSavedPath(this.savedNames.indexOf(n));
                this.refresh();
              })
          }
          .width('100%')
        }, (n: string): string => n)
      }
      if (this.customCount > 0) {
        Text(`自定形状库：${this.customCount} 个（形状工具可用）`)
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding(8)
    .borderRadius(8)
    .backgroundColor($r('app.color.surface'))
  }
}
