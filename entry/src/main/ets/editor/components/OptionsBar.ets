const SWATCHES: string[] = ['#D93025', '#1A6BD9', '#188038', '#F9AB00', '#111111', '#FFFFFF'];

/** 数值滑条行（@Link 双向，规避 @Builder 参数冻结坑）。onCommitted 在拖动结束/点击时回调。 */
@Component
struct OptionSlider {
  label: string = '';
  min: number = 0;
  max: number = 100;
  step: number = 1;
  @Link value: number;
  @Prop suffix: string = '';
  onCommitted: (v: number) => void = (_v: number) => {};

  build() {
    Row({ space: 4 }) {
      Text(this.label)
        .fontSize(12)
        .fontColor($r('app.color.text_secondary'))
      Slider({ value: this.value, min: this.min, max: this.max, step: this.step, style: SliderStyle.OutSet })
        .width(110)
        .height(26)
        .onChange((v: number, mode: SliderChangeMode): void => {
          if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
            this.onCommitted(v);
          }
        })
      Text(`${Math.round(this.value)}${this.suffix}`)
        .fontSize(11)
        .fontColor($r('app.color.text_primary'))
        .width(40)
    }
    .height(28)
  }
}

/**
 * 工具选项条（05 §1：44vp，随 tool 切换）。
 * 画笔/橡皮：直径/硬度/不透明度/蒙版笔/色板；魔棒：容差/连续；
 * 选框/吸管：操作提示；导航/缩放：视口按钮。
 */
@Component
export struct OptionsBar {
  @Prop tool: string = 'brush';
  @Prop docOpen: boolean = false;
  @Prop toolLabel: string = '';
  @Link brushSize: number;
  @Link brushHardness: number;
  @Link brushOpacity: number;
  @Link wandTolerance: number;
  @Link wandContiguous: boolean;
  @Prop brushColor: string = '#000000';
  @Prop maskTarget: boolean = false;
  @Prop hasMaskOnActive: boolean = false;
  onToggleMaskTarget: () => void = () => {};
  onColorPick: (hex: string) => void = (_hex: string) => {};
  onZoom: (cmd: string) => void = (_cmd: string) => {};
  onCropCommand: (cmd: string) => void = (_cmd: string) => {};
  @Prop cropPending: boolean = false;

  @Builder
  brushOptions() {
    Row({ space: 10 }) {
      OptionSlider({
        label: '直径',
        min: 2,
        max: 400,
        step: 1,
        value: $brushSize,
        onCommitted: (v: number): void => {
          this.brushSize = v;
        }
      })
      OptionSlider({
        label: '硬度',
        min: 0,
        max: 100,
        step: 1,
        value: $brushHardness,
        onCommitted: (v: number): void => {
          this.brushHardness = v;
        }
      })
      OptionSlider({
        label: '不透明度',
        min: 5,
        max: 100,
        step: 1,
        value: $brushOpacity,
        suffix: '%',
        onCommitted: (v: number): void => {
          this.brushOpacity = v;  // 百分比直存（引擎换算在 pushBrushSettings）
        }
      })
      Row({ space: 4 }) {
        Text('蒙版笔')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
        Toggle({ type: ToggleType.Checkbox, isOn: this.maskTarget })
          .selectedColor($r('app.color.danger'))
          .opacity(this.hasMaskOnActive ? 1 : 0.35)
          .onChange((on: boolean): void => {
            if (on !== this.maskTarget) {
              this.onToggleMaskTarget();
            }
          })
      }
      .height(28)
      Row({ space: 6 }) {
        ForEach(SWATCHES, (c: string) => {
          Column()
            .width(20)
            .height(20)
            .borderRadius(10)
            .backgroundColor(c)
            .border({
              width: this.brushColor === c ? 3 : 1,
              color: this.brushColor === c ? $r('app.color.text_primary') : $r('app.color.border')
            })
            .onClick((): void => {
              this.onColorPick(c);
            })
        }, (c: string): string => c)
      }
    }
    .height(36)
  }

  @Builder
  wandOptions() {
    Row({ space: 10 }) {
      OptionSlider({
        label: '容差',
        min: 1,
        max: 255,
        step: 1,
        value: $wandTolerance,
        onCommitted: (v: number): void => {
          this.wandTolerance = v;
        }
      })
      Row({ space: 4 }) {
        Text('连续')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
        Toggle({ type: ToggleType.Checkbox, isOn: this.wandContiguous })
          .selectedColor($r('app.color.accent'))
          .onChange((on: boolean): void => {
            this.wandContiguous = on;
          })
        Text('点击画布取样生成选区')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
      }
      .height(28)
    }
    .height(36)
  }

  @Builder
  zoomButtons() {
    Row({ space: 6 }) {
      Button('适配').fontSize(12).height(28).onClick((): void => this.onZoom('fit'))
      Button('100%').fontSize(12).height(28).onClick((): void => this.onZoom('100'))
      Button('+').fontSize(12).height(28).width(36).onClick((): void => this.onZoom('in'))
      Button('−').fontSize(12).height(28).width(36).onClick((): void => this.onZoom('out'))
    }
  }

  build() {
    Row({ space: 8 }) {
      Text(this.toolLabel)
        .fontSize(13)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .width(60)
      if (this.tool === 'brush' || this.tool === 'eraser') {
        this.brushOptions()
      } else if (this.tool === 'wand') {
        this.wandOptions()
      } else if (this.tool === 'rectSelect') {
        Text('在画布拖拽出矩形选区')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
      } else if (this.tool === 'eyedropper') {
        Text('点击画布取色（合成结果）')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
      } else if (this.tool === 'move') {
        Text('拖拽移动图层 · 方向键微调（Shift=10px）')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
      } else if (this.tool === 'lasso') {
        Text('套索工具 M10 提供')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
      } else if (this.tool === 'crop') {
        if (this.cropPending) {
          Text('裁剪框就绪：')
            .fontSize(12)
            .fontColor($r('app.color.text_secondary'))
          Button('提交裁剪').fontSize(12).height(28)
            .backgroundColor($r('app.color.accent'))
            .fontColor($r('app.color.on_accent'))
            .onClick((): void => this.onCropCommand('commit'))
          Button('取消').fontSize(12).height(28)
            .backgroundColor($r('app.color.surface'))
            .fontColor($r('app.color.text_primary'))
            .border({ width: 1, color: $r('app.color.border') })
            .onClick((): void => this.onCropCommand('cancel'))
        } else {
          Text('拖拽出裁剪区域（回车提交 / Esc 取消）')
            .fontSize(12)
            .fontColor($r('app.color.text_secondary'))
        }
      } else if (this.tool === 'hand' || this.tool === 'zoom') {
        this.zoomButtons()
      } else {
        Text('空格 = 临时抓手')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
      }
      Blank()
    }
    .width('100%')
    .height(44)
    .padding({ left: 10, right: 10 })
    .backgroundColor($r('app.color.bar_background'))
    .border({ width: { bottom: 1 }, color: { bottom: $r('app.color.divider') } })
  }
}
