/**
 * M15b 信息面板（06 规划 M15，A-4）：文档尺寸 / 指针坐标（文档域精确）/
 * 指针处颜色（samplePixel 同源）/ 选区尺寸。数据由 Index 触摸/鼠标移动节流推送。
 * 行内容全部内联在 build（@Builder 按值参数不随状态重渲染——实证坑）。
 */
@Component
export struct InfoPanel {
  @Prop docW: number = 0;
  @Prop docH: number = 0;
  @Prop pointerX: number = -1;   // 文档域；<0 = 指针不在画布
  @Prop pointerY: number = -1;
  @Prop colorHex: string = '';   // #RRGGBB；空 = 未采样
  @Prop hasSelection: boolean = false;
  @Prop selW: number = 0;
  @Prop selH: number = 0;

  build() {
    Column({ space: 2 }) {
      Text('信息')
        .fontSize(12)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .width('100%')
        .padding({ left: 8, top: 4 })
      Row() {
        Text('文档')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
          .width(64)
        Text(`${Math.round(this.docW)} × ${Math.round(this.docH)} px`)
          .fontSize(11)
          .fontColor($r('app.color.text_primary'))
          .layoutWeight(1)
          .textAlign(TextAlign.End)
      }
      .width('100%')
      .padding({ left: 8, right: 8, top: 2, bottom: 2 })
      Row() {
        Text('指针')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
          .width(64)
        Text(this.pointerX >= 0
          ? `X ${Math.round(this.pointerX)}  Y ${Math.round(this.pointerY)}` : '—')
          .fontSize(11)
          .fontColor($r('app.color.text_primary'))
          .layoutWeight(1)
          .textAlign(TextAlign.End)
      }
      .width('100%')
      .padding({ left: 8, right: 8, top: 2, bottom: 2 })
      Row() {
        Text('颜色')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
          .width(64)
        Row({ space: 6 }) {
          Column()
            .width(14)
            .height(14)
            .borderRadius(3)
            .backgroundColor(this.colorHex.length === 7 ? this.colorHex : '#00000000')
            .border({ width: 1, color: '#666666' })
          Text(this.colorHex.length === 7 ? this.colorHex : '—')
            .fontSize(11)
            .fontColor($r('app.color.text_primary'))
        }
        .justifyContent(FlexAlign.End)
        .layoutWeight(1)
      }
      .width('100%')
      .padding({ left: 8, right: 8, top: 2, bottom: 2 })
      Row() {
        Text('选区')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
          .width(64)
        Text(this.hasSelection
          ? `${Math.round(this.selW)} × ${Math.round(this.selH)} px` : '无')
          .fontSize(11)
          .fontColor($r('app.color.text_primary'))
          .layoutWeight(1)
          .textAlign(TextAlign.End)
      }
      .width('100%')
      .padding({ left: 8, right: 8, top: 2, bottom: 2 })
    }
    .width('100%')
    .padding(4)
    .borderRadius(10)
    .backgroundColor($r('app.color.bar_background'))
  }
}
