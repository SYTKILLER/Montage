import { RecentProject } from '../AppPrefs';

/**
 * 欢迎态（05 §8）：无文档时画布区内嵌视图（非路由页）——新建/打开/最近工程。
 */
@Component
export struct WelcomeView {
  @Prop recents: RecentProject[] = [];
  onNew: () => void = () => {};
  onOpen: () => void = () => {};
  onOpenRecent: (uri: string) => void = (_uri: string) => {};

  build() {
    Column({ space: 24 }) {
      Text('Montage')
        .fontSize(34)
        .fontWeight(FontWeight.Bold)
        .fontColor($r('app.color.text_primary'))
      Text('鸿蒙开源图像编辑器 · PSD 中心')
        .fontSize(13)
        .fontColor($r('app.color.text_secondary'))

      Row({ space: 16 }) {
        Column({ space: 10 }) {
          SymbolGlyph($r('sys.symbol.doc_plaintext'))
            .fontSize(40)
            .fontColor([$r('app.color.accent')])
          Text('新建').fontSize(15).fontColor($r('app.color.text_primary'))
          Text('4096 × 4096 画布').fontSize(11).fontColor($r('app.color.text_secondary'))
        }
        .width(180)
        .height(140)
        .justifyContent(FlexAlign.Center)
        .borderRadius(14)
        .backgroundColor($r('app.color.surface'))
        .border({ width: 1, color: $r('app.color.border') })
        .onClick((): void => this.onNew())

        Column({ space: 10 }) {
          SymbolGlyph($r('sys.symbol.folder'))
            .fontSize(40)
            .fontColor([$r('app.color.accent')])
          Text('打开').fontSize(15).fontColor($r('app.color.text_primary'))
          Text('图片 / PSD / .montage').fontSize(11).fontColor($r('app.color.text_secondary'))
        }
        .width(180)
        .height(140)
        .justifyContent(FlexAlign.Center)
        .borderRadius(14)
        .backgroundColor($r('app.color.surface'))
        .border({ width: 1, color: $r('app.color.border') })
        .onClick((): void => this.onOpen())
      }

      if (this.recents.length > 0) {
        Column({ space: 8 }) {
          Text('最近工程')
            .fontSize(13)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
          List({ space: 4 }) {
            ForEach(this.recents, (r: RecentProject) => {
              ListItem() {
                Row({ space: 10 }) {
                  SymbolGlyph($r('sys.symbol.layer'))
                    .fontSize(16)
                    .fontColor([$r('app.color.icon_muted')])
                  Column({ space: 2 }) {
                    Text(r.name)
                      .fontSize(13)
                      .fontColor($r('app.color.text_primary'))
                      .maxLines(1)
                      .textOverflow({ overflow: TextOverflow.Ellipsis })
                    Text(r.uri)
                      .fontSize(10)
                      .fontColor($r('app.color.text_secondary'))
                      .maxLines(1)
                      .textOverflow({ overflow: TextOverflow.Ellipsis })
                  }
                  .alignItems(HorizontalAlign.Start)
                  .layoutWeight(1)
                }
                .width('100%')
                .padding({ left: 12, right: 12, top: 8, bottom: 8 })
                .borderRadius(8)
                .backgroundColor($r('app.color.surface'))
                .border({ width: 1, color: $r('app.color.border') })
                .onClick((): void => this.onOpenRecent(r.uri))
              }
            }, (r: RecentProject): string => r.uri)
          }
          .width(420)
          .height(Math.min(this.recents.length * 52, 200))
          .scrollBar(BarState.Off)
        }
        .alignItems(HorizontalAlign.Start)
      }
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
    .backgroundColor($r('app.color.canvas_backdrop'))
  }
}
