/**
 * 导出对话框（05 §5 模态范式：Stack 遮罩 + 居中卡片）。
 * PNG（引擎 PngRowEncoder 直写）/ JPEG·WebP（ImagePacker）/ PSD（保留图层）/
 * TIFF·GIF·BMP·TGA·QOI（M25 引擎编码器直写）。
 */
@Component
export struct ExportDialog {
  @Prop docW: number = 0;
  @Prop docH: number = 0;
  // 格式槽位：0=PNG 1=JPEG 2=PSD 3=TIFF 4=GIF 5=BMP 6=TGA 7=QOI 8=WebP
  @State formatIdx: number = 0;
  @State quality: number = 90;
  onConfirm: (format: string, quality: number) => void = (_f: string, _q: number) => {};
  onCancel: () => void = () => {};

  private fmtName(i: number): string {
    if (i === 0) return 'PNG（无损）';
    if (i === 1) return 'JPEG';
    if (i === 2) return 'PSD（保留图层）';
    if (i === 3) return 'TIFF';
    if (i === 4) return 'GIF（256 色抖动）';
    if (i === 5) return 'BMP';
    if (i === 6) return 'TGA';
    if (i === 7) return 'QOI';
    return 'WebP';
  }

  private fmtKey(i: number): string {
    if (i === 0) return 'png';
    if (i === 1) return 'jpeg';
    if (i === 2) return 'psd';
    if (i === 3) return 'tiff';
    if (i === 4) return 'gif';
    if (i === 5) return 'bmp';
    if (i === 6) return 'tga';
    if (i === 7) return 'qoi';
    return 'webp';
  }

  @Builder
  fmtRadio(i: number) {
    Row({ space: 6 }) {
      Radio({ value: this.fmtName(i), group: 'exportFmt' })
        .checked(this.formatIdx === i)
        .onChange((on: boolean): void => {
          if (on) {
            this.formatIdx = i;
          }
        })
      Text(this.fmtName(i)).fontSize(13).fontColor($r('app.color.text_primary'))
    }
  }

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

        Grid() {
          GridItem() {
            this.fmtRadio(0)
          }
          GridItem() {
            this.fmtRadio(1)
          }
          GridItem() {
            this.fmtRadio(2)
          }
          GridItem() {
            this.fmtRadio(3)
          }
          GridItem() {
            this.fmtRadio(4)
          }
          GridItem() {
            this.fmtRadio(5)
          }
          GridItem() {
            this.fmtRadio(6)
          }
          GridItem() {
            this.fmtRadio(7)
          }
          GridItem() {
            this.fmtRadio(8)
          }
        }
        .columnsTemplate('1fr 1fr 1fr')
        .rowsGap(10)
        .width('100%')

        if (this.formatIdx === 1 || this.formatIdx === 8) {
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
            .onClick((): void => this.onConfirm(this.fmtKey(this.formatIdx), this.quality))
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
