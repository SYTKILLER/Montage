// M10c 通道面板 v1（06 规划 §4 M10：只读列表——Alpha 通道=存储的选区；载入走选择菜单）。
// 数据源 getChannelList（同步 NAPI 返回名字数组）；删除走 deleteChannel。
import { CompositorEngine } from 'core';

@Component
export struct ChannelsPanel {
  engine: CompositorEngine = new CompositorEngine();
  onChanged: () => void = () => {};
  @State names: string[] = [];

  aboutToAppear(): void {
    this.refresh();
  }

  private refresh(): void {
    this.engine.getChannelList().then((list: string[]): void => {
      this.names = list;
    }).catch((_e: BusinessError<object>): void => {
      this.names = [];
    });
  }

  build() {
    Column({ space: 6 }) {
      Text('通道')
        .fontSize(13)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
      if (this.names.length === 0) {
        Text('无 Alpha 通道（选择 → 存储选区）')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
      } else {
        ForEach(this.names, (n: string) => {
          Row({ space: 6 }) {
            Column()
              .width(18)
              .height(18)
              .borderRadius(4)
              .linearGradient({
                angle: 135,
                colors: [['#FFFFFF', 0], ['#333333', 1]]
              })
              .border({ width: 1, color: $r('app.color.border') })
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
                this.engine.loadSelection(n, 0);
                this.onChanged();
              })
            SymbolGlyph($r('sys.symbol.trash'))
              .fontSize(14)
              .fontColor([$r('app.color.text_secondary')])
              .onClick((): void => {
                this.engine.deleteChannel(n);
                this.refresh();
                this.onChanged();
              })
          }
          .width('100%')
          .height(26)
        }, (n: string): string => n)
      }
    }
    .alignItems(HorizontalAlign.Start)
    .padding(10)
    .borderRadius(10)
    .backgroundColor($r('app.color.bar_background'))
    .border({ width: 1, color: $r('app.color.divider') })
  }
}
