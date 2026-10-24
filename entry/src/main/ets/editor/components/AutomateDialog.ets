/**
 * M24b 批处理 / 图像处理器对话框（文件›自动 语义，photocraft file.automate.batch /
 * file.scripts.imageProcessor 参数面）。
 * 路径输入为沙箱绝对路径（模拟器 picker 不可注入；真机可挂 picker 留登记）。
 * 运行进度/结果经 props 由 Index 回填（对话框只收集参数，引擎面在 Index）。
 */

@Component
export struct AutomateDialog {
  @Prop @Watch('noop') mode: number = 0;            // 0 = 批处理 1 = 图像处理器
  @Prop @Watch('noop') busy: boolean = false;
  @Prop @Watch('noop') progressText: string = '';
  @Prop @Watch('noop') resultText: string = '';
  actionNames: string[] = [];
  onRun: (params: Record<string, Object>) => void = (_p: Record<string, Object>): void => {};
  onCancelRun: () => void = (): void => {};
  onClose: () => void = (): void => {};

  noop(): void {
  }

  @State inputDir: string = '';
  @State outputDir: string = '';
  @State actionName: string = '';
  @State format: string = 'same';
  @State quality: number = 85;
  @State fitW: number = 0;
  @State fitH: number = 0;

  private formatsBatch: string[] = ['same', 'png', 'jpeg', 'psd', 'tiff', 'bmp', 'qoi', 'tga', 'webp'];
  private formatsProc: string[] = ['jpeg', 'png', 'tiff', 'webp', 'bmp'];

  build() {
    Stack() {
      Column()
        .width('100%')
        .height('100%')
        .backgroundColor('#66000000')
      Column({ space: 10 }) {
        Text(this.mode === 0 ? '批处理' : '图像处理器')
          .fontSize(16).fontWeight(FontWeight.Bold).fontColor($r('app.color.text_primary'))
        Column({ space: 6 }) {
          Text('输入目录（沙箱绝对路径）').fontSize(11).fontColor($r('app.color.text_secondary'))
          TextInput({ text: this.inputDir, placeholder: '如 /data/app/.../cache/batch_in' })
            .height(36).fontSize(12)
            .onChange((v: string): void => {
              this.inputDir = v;
            })
          Text('输出目录（须已存在）').fontSize(11).fontColor($r('app.color.text_secondary'))
          TextInput({ text: this.outputDir, placeholder: '如 /data/app/.../cache/batch_out' })
            .height(36).fontSize(12)
            .onChange((v: string): void => {
              this.outputDir = v;
            })
        }
        .width('100%')

        Row({ space: 8 }) {
          Column({ space: 6 }) {
            Text(this.mode === 0 ? '运行动作' : '输出格式').fontSize(11)
              .fontColor($r('app.color.text_secondary'))
            if (this.mode === 0) {
              Select(this.actionNames.map((n: string): SelectOption => {
                return { value: n };
              }))
                .selected(this.actionNames.indexOf(this.actionName))
                .value(this.actionName.length > 0 ? this.actionName : '选择动作')
                .onSelect((_: number, v: string): void => {
                  this.actionName = v;
                })
            } else {
              Select(this.formatsProc.map((n: string): SelectOption => {
                return { value: n };
              }))
                .selected(this.formatsProc.indexOf(this.format) >= 0
                  ? this.formatsProc.indexOf(this.format) : 0)
                .value(this.format)
                .onSelect((_: number, v: string): void => {
                  this.format = v;
                })
            }
          }
          .layoutWeight(1)
          Column({ space: 6 }) {
            Text(this.mode === 0 ? '输出格式' : '质量').fontSize(11)
              .fontColor($r('app.color.text_secondary'))
            if (this.mode === 0) {
              Select(this.formatsBatch.map((n: string): SelectOption => {
                return { value: n };
              }))
                .selected(this.formatsBatch.indexOf(this.format))
                .value(this.format)
                .onSelect((_: number, v: string): void => {
                  this.format = v;
                })
            } else {
              Text(`${this.quality}`).fontSize(14).fontColor($r('app.color.text_primary'))
            }
          }
          .layoutWeight(1)
        }
        .width('100%')

        if (this.mode === 0) {
          Text('动作中的保存/视图步骤将被输出目录接管（跳过）').fontSize(10)
            .fontColor($r('app.color.text_secondary'))
        }
        if (this.mode === 1) {
          Row({ space: 8 }) {
            Column({ space: 6 }) {
              Text('适配宽（0=不限）').fontSize(11).fontColor($r('app.color.text_secondary'))
              TextInput({ text: `${this.fitW > 0 ? this.fitW : 0}` })
                .height(36).fontSize(12).type(InputType.Number)
                .onChange((v: string): void => {
                  this.fitW = parseInt(v.length > 0 ? v : '0');
                })
            }
            .layoutWeight(1)
            Column({ space: 6 }) {
              Text('适配高（0=不限）').fontSize(11).fontColor($r('app.color.text_secondary'))
              TextInput({ text: `${this.fitH > 0 ? this.fitH : 0}` })
                .height(36).fontSize(12).type(InputType.Number)
                .onChange((v: string): void => {
                  this.fitH = parseInt(v.length > 0 ? v : '0');
                })
            }
            .layoutWeight(1)
            Column({ space: 6 }) {
              Text('质量').fontSize(11).fontColor($r('app.color.text_secondary'))
              Slider({ value: this.quality, min: 10, max: 100, step: 1 })
                .onChange((v: number): void => {
                  this.quality = Math.round(v);
                })
            }
            .layoutWeight(2)
          }
          .width('100%')
          Text('仅缩小不放大（dontEnlarge 固定开）；转 sRGB 恒生效（文档像素即 sRGB）')
            .fontSize(10).fontColor($r('app.color.text_secondary'))
        }

        if (this.progressText.length > 0) {
          Text(this.progressText).fontSize(12).fontColor('#0066CC')
        }
        if (this.resultText.length > 0) {
          Text(this.resultText).fontSize(11).fontColor($r('app.color.text_primary'))
            .maxLines(6)
        }

        Row({ space: 12 }) {
          if (this.busy) {
            Button('取消运行').fontSize(14).height(40).width(110)
              .backgroundColor('#88665555')
              .fontColor($r('app.color.text_primary'))
              .onClick((): void => this.onCancelRun())
          } else {
            Button('关闭').fontSize(14).height(40).width(96)
              .backgroundColor($r('app.color.surface'))
              .fontColor($r('app.color.text_primary'))
              .border({ width: 1, color: $r('app.color.border') })
              .onClick((): void => this.onClose())
            Button('运行').fontSize(14).height(40).width(96)
              .backgroundColor($r('app.color.accent'))
              .fontColor($r('app.color.on_accent'))
              .enabled(this.inputDir.length > 0 && this.outputDir.length > 0 &&
                (this.mode === 1 || this.actionName.length > 0))
              .onClick((): void => this.onRun({
                'inputDir': this.inputDir,
                'output': this.outputDir,
                'actionName': this.mode === 0 ? this.actionName : '',
                'format': this.mode === 0 ? this.format : this.format,
                'width': this.fitW,
                'height': this.fitH,
                'quality': this.quality,
              }))
          }
        }
        .justifyContent(FlexAlign.End)
        .width('100%')
      }
      .padding(20)
      .borderRadius(14)
      .backgroundColor($r('app.color.surface'))
      .border({ width: 1, color: $r('app.color.border') })
      .width(460)
    }
  }
}
