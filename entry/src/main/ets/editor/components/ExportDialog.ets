/**
 * 导出对话框（05 §5 模态范式：Stack 遮罩 + 居中卡片）。
 * PNG（引擎 PngRowEncoder 直写）/ JPEG（ImagePacker，质量可调）。
 */
@Component
export struct ExportDialog {
  @Prop docW: number = 0;
  @Prop docH: number = 0;
  @State formatIdx: number = 0;   // 0=PNG 1=JPEG
  @State quality: number = 90;
  onConfirm: (format: string, quality: number) => void = (_f: string, _q: number) => {};
  onCancel: () => void = () => {};

  build() {
    Stack() {
      // 遮罩层（点击取消；卡片为兄弟节点，点击卡片不会命中遮罩）
      Column()
        .width('100%')
        .height('100%')
        .backgroundColor('#66000000')
        .onClick((): void => this.onCancel())
      Column({ space: 14 }) {
        Text('导出图像')
          .fontSize(16)
          .fontWeight(FontWeight.Bold)
          .fontColor($r('app.color.text_primary'))
        Text(`${this.docW} × ${this.docH} · 可见图层扁平合成`)
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))

        Row({ space: 16 }) {
          Row({ space: 6 }) {
            Radio({ value: 'PNG', group: 'exportFmt' })
              .checked(this.formatIdx === 0)
              .onChange((on: boolean): void => {
                if (on) {
                  this.formatIdx = 0;
                }
              })
            Text('PNG（无损）').fontSize(13).fontColor($r('app.color.text_primary'))
          }
          Row({ space: 6 }) {
            Radio({ value: 'JPEG', group: 'exportFmt' })
              .checked(this.formatIdx === 1)
              .onChange((on: boolean): void => {
                if (on) {
                  this.formatIdx = 1;
                }
              })
            Text('JPEG').fontSize(13).fontColor($r('app.color.text_primary'))
          }
        }

        if (this.formatIdx === 1) {
          Row({ space: 8 }) {
            Text('质量').fontSize(12).fontColor($r('app.color.text_secondary'))
            Slider({ value: this.quality, min: 10, max: 100, step: 1 })
              .layoutWeight(1)
              .onChange((v: number, mode: SliderChangeMode): void => {
                if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                  this.quality = Math.round(v);
                }
              })
            Text(`${this.quality}`).fontSize(11).fontColor($r('app.color.text_primary')).width(28)
          }
          .width('100%')
        }

        Row({ space: 12 }) {
          Button('取消')
            .fontSize(14)
            .height(40)
            .width(96)
            .backgroundColor($r('app.color.surface'))
            .fontColor($r('app.color.text_primary'))
            .border({ width: 1, color: $r('app.color.border') })
            .onClick((): void => this.onCancel())
          Button('导出')
            .fontSize(14)
            .height(40)
            .width(96)
            .backgroundColor($r('app.color.accent'))
            .fontColor($r('app.color.on_accent'))
            .onClick((): void => this.onConfirm(this.formatIdx === 0 ? 'png' : 'jpeg', this.quality))
        }
        .justifyContent(FlexAlign.End)
        .width('100%')
      }
      .padding(20)
      .borderRadius(14)
      .backgroundColor($r('app.color.surface'))
      .border({ width: 1, color: $r('app.color.border') })
      .width(420)
    }
    .width('100%')
    .height('100%')
  }
}
