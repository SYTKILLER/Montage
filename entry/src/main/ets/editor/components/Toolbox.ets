import { ToolVM } from '../MenuModel';

/**
 * 左侧工具箱（05 §1：56vp 单列）。纯展示：tools 由父组件给表，
 * activeTool 高亮，onPick 回调切工具态（手势动态换绑由父组件做）。
 */
@Component
export struct Toolbox {
  @Prop tools: ToolVM[] = [];
  @Prop activeTool: string = 'brush';
  onPick: (id: string) => void = (_id: string) => {};

  @Builder
  toolButton(t: ToolVM) {
    Column() {
      SymbolGlyph(t.symbol)
        .fontSize(20)
        .fontColor([this.activeTool === t.id ? $r('app.color.accent') : $r('app.color.text_primary')])
        .opacity(t.enabled ? 1 : 0.35)
    }
    .width(40)
    .height(40)
    .borderRadius(8)
    .backgroundColor(this.activeTool === t.id ? $r('app.color.accent_container') : Color.Transparent)
    .onClick((): void => {
      this.onPick(t.id);
    })
  }

  build() {
    Column({ space: 2 }) {
      ForEach(this.tools, (t: ToolVM, idx: number) => {
        if (t.id === '-') {
          // 分组分隔线
          Divider()
            .width(28)
            .strokeWidth(1)
            .color($r('app.color.divider'))
            .margin({ top: 3, bottom: 3 })
        } else {
          this.toolButton(t)
        }
      }, (t: ToolVM): string => `${t.id}`)
    }
    .width(52)
    .padding({ top: 6, bottom: 6 })
    .backgroundColor($r('app.color.bar_background'))
    .border({ width: { right: 1 }, color: { right: $r('app.color.divider') } })
  }
}
