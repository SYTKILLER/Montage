/**
 * 状态栏（05 §1：28vp）：缩放% / 文档尺寸 / 活动层 / 忙碌与进度。
 */
@Component
export struct StatusBar {
  @Prop docOpen: boolean = false;
  @Prop docW: number = 0;
  @Prop docH: number = 0;
  @Prop zoom: number = 1;
  @Prop activeLayerName: string = '';
  @Prop busyText: string = '';
  @Prop importing: boolean = false;
  @Prop importProgress: number = 0;
  @Prop fps: number = 0;

  build() {
    Row({ space: 14 }) {
      Text(this.docOpen ? `${(this.zoom * 100).toFixed(1)}%` : '—')
        .fontSize(11)
        .fontColor($r('app.color.text_primary'))
        .width(64)
      Text(this.docOpen ? `${this.docW} × ${this.docH}` : '无文档')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
      Text(this.activeLayerName !== '' ? `层: ${this.activeLayerName}` : '')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .constraintSize({ maxWidth: 180 })
      if (this.importing) {
        Progress({ value: this.importProgress * 100, total: 100 })
          .width(120)
          .height(14)
        Text(`导入 ${(this.importProgress * 100).toFixed(0)}%`)
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
      } else if (this.busyText !== '') {
        Text(this.busyText)
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
      }
      Blank()
      Text(`${this.fps.toFixed(0)} fps`)
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
    }
    .width('100%')
    .height(28)
    .padding({ left: 12, right: 12 })
    .backgroundColor($r('app.color.bar_background'))
    .border({ width: { top: 1 }, color: { top: $r('app.color.divider') } })
  }
}
