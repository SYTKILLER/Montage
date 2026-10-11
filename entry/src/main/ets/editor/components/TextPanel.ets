/**
 * M12 文字面板（字符/段落合一 v1，06 规划）：
 * 字体列表 / 字号 / 行距 / 字距 / 颜色 / B·I / 对齐 / 内容编辑 / 栅格化。
 * 受控组件：活动文本层 DTO 由父组件传值；操作回调发引擎命令（txn 由父按编辑会话态决定）。
 * 段落缩进 v1 不做（TypographySetIndents 登记后续）；直排 P1 不做。
 */

const TEXT_SWATCHES: string[] = ['#111111', '#FFFFFF', '#D93025', '#F9AB00', '#188038', '#1A6BD9', '#7B1FA2', '#8D6E63'];
const ALIGN_LABELS: string[] = ['左', '中', '右'];

@Component
export struct TextPanel {
  @Prop fontList: string[] = [];
  @Prop fontSel: number = -1;
  @Prop fontSize: number = 40;
  @Prop lineHeightPct: number = 120;   // %（引擎值 ×100）
  @Prop tracking: number = 0;          // 千分比（PS 语义）
  @Prop bold: boolean = false;
  @Prop italic: boolean = false;
  @Prop colorHex: string = '#000000';
  @Prop alignIdx: number = 0;
  @Prop wrapW: number = 0;             // 0 = 点文字
  @Prop content: string = '';
  @Prop editing: boolean = false;      // overlay 编辑会话中（面板改动 txn=0）
  @Prop isText: boolean = false;
  onPickFont: (idx: number) => void = (_i: number) => {};
  onFontSize: (v: number) => void = (_v: number) => {};
  onLineHeight: (v: number) => void = (_v: number) => {};
  onTracking: (v: number) => void = (_v: number) => {};
  onToggleBold: () => void = () => {};
  onToggleItalic: () => void = () => {};
  onPickColor: (c: string) => void = (_c: string) => {};
  onAlign: (a: number) => void = (_a: number) => {};
  onContent: (text: string) => void = (_t: string) => {};
  onRasterize: () => void = () => {};
  onImportFont: () => void = () => {};

  @State private contentBox: string = '';

  build() {
    Column({ space: 8 }) {
      Text('文字')
        .fontSize(13)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .width('100%')

      // 字体
      Row({ space: 6 }) {
        Text('字体').fontSize(10).fontColor($r('app.color.text_secondary')).width(30)
        Select(this.fontList.map((f: string): SelectOption => ({ value: f })))
          .selected(this.fontSel)
          .value(this.fontSel >= 0 && this.fontSel < this.fontList.length
            ? this.fontList[this.fontSel] : '默认字体')
          .font({ size: 11 })
          .onSelect((idx: number): void => this.onPickFont(idx))
          .layoutWeight(1)
        Button('+')
          .fontSize(11).height(24).width(28).padding(0)
          .backgroundColor($r('app.color.surface'))
          .fontColor($r('app.color.text_primary'))
          .onClick((): void => this.onImportFont())
      }.width('100%')

      // 字号
      Row({ space: 6 }) {
        Text('字号').fontSize(10).fontColor($r('app.color.text_secondary')).width(30)
        Slider({ value: this.fontSize, min: 8, max: 300, step: 1 })
          .layoutWeight(1)
          .onChange((v: number, mode: SliderChangeMode): void => {
            if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
              this.onFontSize(Math.round(v));
            }
          })
        Text(`${Math.round(this.fontSize)}`)
          .fontSize(10).fontColor($r('app.color.text_primary')).width(34).textAlign(TextAlign.End)
      }.width('100%')

      // 行距（%）
      Row({ space: 6 }) {
        Text('行距').fontSize(10).fontColor($r('app.color.text_secondary')).width(30)
        Slider({ value: this.lineHeightPct, min: 50, max: 400, step: 5 })
          .layoutWeight(1)
          .onChange((v: number, mode: SliderChangeMode): void => {
            if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
              this.onLineHeight(Math.round(v));
            }
          })
        Text(`${Math.round(this.lineHeightPct)}%`)
          .fontSize(10).fontColor($r('app.color.text_primary')).width(34).textAlign(TextAlign.End)
      }.width('100%')

      // 字距（千分比）
      Row({ space: 6 }) {
        Text('字距').fontSize(10).fontColor($r('app.color.text_secondary')).width(30)
        Slider({ value: this.tracking, min: -200, max: 800, step: 10 })
          .layoutWeight(1)
          .onChange((v: number, mode: SliderChangeMode): void => {
            if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
              this.onTracking(Math.round(v));
            }
          })
        Text(`${Math.round(this.tracking)}`)
          .fontSize(10).fontColor($r('app.color.text_primary')).width(34).textAlign(TextAlign.End)
      }.width('100%')

      // B / I / 对齐
      Row({ space: 6 }) {
        Text('B')
          .fontSize(13)
          .fontWeight(this.bold ? FontWeight.Bold : FontWeight.Normal)
          .fontColor(this.bold ? $r('app.color.on_accent') : $r('app.color.text_primary'))
          .backgroundColor(this.bold ? $r('app.color.accent') : $r('app.color.surface'))
          .borderRadius(6)
          .width(28).height(24).textAlign(TextAlign.Center)
          .onClick((): void => this.onToggleBold())
        Text('I')
          .fontSize(13)
          .fontStyle(this.italic ? FontStyle.Italic : FontStyle.Normal)
          .fontColor(this.italic ? $r('app.color.on_accent') : $r('app.color.text_primary'))
          .backgroundColor(this.italic ? $r('app.color.accent') : $r('app.color.surface'))
          .borderRadius(6)
          .width(28).height(24).textAlign(TextAlign.Center)
          .onClick((): void => this.onToggleItalic())
        Divider().strokeWidth(1).vertical(true).height(20).color($r('app.color.divider'))
        ForEach(ALIGN_LABELS, (lb: string, idx: number) => {
          Text(lb)
            .fontSize(11)
            .fontColor(this.alignIdx === idx
              ? $r('app.color.on_accent') : $r('app.color.text_primary'))
            .backgroundColor(this.alignIdx === idx ? $r('app.color.accent') : $r('app.color.surface'))
            .borderRadius(6)
            .width(28).height(24).textAlign(TextAlign.Center)
            .onClick((): void => this.onAlign(idx))
        }, (lb: string): string => lb)
        Text(this.wrapW > 0 ? `段落 ${Math.round(this.wrapW)}px` : '点文字')
          .fontSize(9)
          .fontColor($r('app.color.text_secondary'))
          .margin({ left: 'auto' })
      }.width('100%')

      // 颜色
      Row({ space: 6 }) {
        Text('颜色').fontSize(10).fontColor($r('app.color.text_secondary')).width(30)
        ForEach(TEXT_SWATCHES, (c: string) => {
          Column()
            .width(18)
            .height(18)
            .borderRadius(9)
            .backgroundColor(c)
            .border({
              width: this.colorHex.toUpperCase() === c.toUpperCase() ? 3 : 1,
              color: this.colorHex.toUpperCase() === c.toUpperCase()
                ? $r('app.color.text_primary') : $r('app.color.border')
            })
            .onClick((): void => this.onPickColor(c))
        }, (c: string): string => c)
      }.width('100%')

      // 内容（面板直改入口：编辑 overlay 之外亦可用；blur 提交）
      TextArea({ text: this.contentBox, placeholder: '输入文字内容…' })
        .fontSize(11)
        .height(56)
        .width('100%')
        .onChange((v: string): void => {
          this.contentBox = v;
        })
        .onBlur((): void => {
          this.onContent(this.contentBox);
        })
      Row({ space: 6 }) {
        Button('应用内容')
          .fontSize(10).height(24).padding({ left: 10, right: 10 })
          .backgroundColor($r('app.color.accent_container'))
          .fontColor($r('app.color.text_primary'))
          .onClick((): void => this.onContent(this.contentBox))
        Button('栅格化')
          .fontSize(10).height(24).padding({ left: 10, right: 10 })
          .backgroundColor($r('app.color.surface'))
          .fontColor($r('app.color.text_primary'))
          .enabled(this.isText)
          .onClick((): void => this.onRasterize())
        if (this.editing) {
          Text('编辑中（画布 overlay）')
            .fontSize(9).fontColor($r('app.color.accent'))
        }
      }.width('100%')
    }
    .width('100%')
    .padding(10)
    .borderRadius(10)
    .backgroundColor($r('app.color.bar_background'))
  }
}
