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
  @Link wandAllLayers: boolean;
  @Link wandSampleSize: number;
  @Prop brushColor: string = '#000000';
  @Prop maskTarget: boolean = false;
  @Prop hasMaskOnActive: boolean = false;
  @Prop quickMaskActive: boolean = false;  // M10d：快速蒙版模式中（笔画自动落通道）
  // M11a 渐变工具
  @Link gradientType: number;    // 0=线性 1=径向
  @Link gradientStyle: number;   // 0=前景→背景 1=前景→透明
  @Link gradientTarget: number;  // 0=像素 1=图层蒙版
  @Prop hasMaskOnActiveForGrad: boolean = false;
  // M11c 形状工具
  @Link shapeKind: number;       // 0=矩形 1=圆角矩形 2=椭圆 3=直线
  @Link shapeFill: boolean;
  @Link shapeStroke: boolean;
  @Link shapeStrokeWidth: number;
  // M11d 仿制图章
  @Link cloneAligned: boolean;
  @Prop cloneSourceSet: boolean = false;
  @Prop clonePicking: boolean = false;
  onCloneAligned: (on: boolean) => void = (_on: boolean) => {};
  onClonePickToggle: () => void = () => {};
  @Prop cropPending: boolean = false;
  // M10a 选区增强：布尔四态 + 选框形状
  @Link selOp: number;
  @Prop selShape: string = 'rect';
  onSelOpPick: (op: number) => void = (_op: number) => {};
  onSelShapeToggle: () => void = () => {};
  onToggleMaskTarget: () => void = () => {};
  onColorPick: (hex: string) => void = (_hex: string) => {};
  onZoom: (cmd: string) => void = (_cmd: string) => {};
  onCropCommand: (cmd: string) => void = (_cmd: string) => {};

  // M10a 布尔四态按钮（新/加/减/交；PS 选项条语义，Shift/Alt 临时修饰在引擎入口生效）
  @Builder
  selOpButtons() {
    Row({ space: 2 }) {
      this.selOpBtn('square_dashed', 0, '新选区')
      this.selOpBtn('plus_square_on_square', 1, '添加到选区')
      this.selOpBtn('minus_square_fill', 2, '从选区减去')
      this.selOpBtn('plus_square_dashed_on_square', 3, '与选区交叉')
    }
    .height(28)
  }

  @Builder
  selOpBtn(symbol: string, op: number, _tip: string) {
    Column() {
      SymbolGlyph($r(`sys.symbol.${symbol}`))
        .fontSize(18)
        .fontColor([this.selOp === op ? $r('app.color.accent') : $r('app.color.text_secondary')])
    }
    .justifyContent(FlexAlign.Center)
    .width(32)
    .height(28)
    .borderRadius(6)
    .backgroundColor(this.selOp === op ? $r('app.color.accent_container') : Color.Transparent)
    .onClick((): void => this.onSelOpPick(op))
  }

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
        if (this.quickMaskActive) {
          // M10d：QM 模式中笔画由引擎路由到快速蒙版通道（黑笔加红减选 / 白笔减红加选）
          Text('快速蒙版：黑笔加红（减选区）· 白笔减红（加选区）· Q 退出')
            .fontSize(11)
            .fontColor($r('app.color.danger'))
            .maxLines(1)
        } else {
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
        Text('所有图层')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
        Toggle({ type: ToggleType.Checkbox, isOn: this.wandAllLayers })
          .selectedColor($r('app.color.accent'))
          .onChange((on: boolean): void => {
            this.wandAllLayers = on;
          })
        Text('取样')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
        OptionSlider({
          label: '',
          min: 1,
          max: 101,
          step: 2,
          value: $wandSampleSize,
          suffix: 'px'
        })
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

  // M11a 渐变选项：类型/样式/目标
  @Builder
  gradientOptions() {
    Row({ space: 10 }) {
      Button(this.gradientType === 0 ? '线性' : '径向')
        .fontSize(12).height(28).width(56)
        .backgroundColor($r('app.color.surface'))
        .fontColor($r('app.color.text_primary'))
        .border({ width: 1, color: $r('app.color.border') })
        .onClick((): void => {
          this.gradientType = this.gradientType === 0 ? 1 : 0;
        })
      Button(this.gradientStyle === 0 ? '前景→背景' : '前景→透明')
        .fontSize(12).height(28).width(96)
        .backgroundColor($r('app.color.surface'))
        .fontColor($r('app.color.text_primary'))
        .border({ width: 1, color: $r('app.color.border') })
        .onClick((): void => {
          this.gradientStyle = this.gradientStyle === 0 ? 1 : 0;
        })
      Button(this.gradientTarget === 0 ? '目标:像素' : '目标:蒙版')
        .fontSize(12).height(28).width(88)
        .backgroundColor($r('app.color.surface'))
        .fontColor(this.gradientTarget === 1 ? $r('app.color.danger') : $r('app.color.text_primary'))
        .border({ width: 1, color: $r('app.color.border') })
        .opacity(this.gradientTarget === 1 && !this.hasMaskOnActiveForGrad ? 0.35 : 1)
        .onClick((): void => {
          this.gradientTarget = this.gradientTarget === 0 ? 1 : 0;
        })
      Text('拖线定方向与长度 · 选区约束生效')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
    }
    .height(36)
  }

  // M11b 油漆桶选项（与魔棒同参：容差/连续/所有图层/取样）
  @Builder
  bucketOptions() {
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
        Text('所有图层')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
        Toggle({ type: ToggleType.Checkbox, isOn: this.wandAllLayers })
          .selectedColor($r('app.color.accent'))
          .onChange((on: boolean): void => {
            this.wandAllLayers = on;
          })
      }
      .height(28)
      Text('点击填充前景色 · 受选区约束')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
    }
    .height(36)
  }

  // M11c 形状选项：种类循环 / 填充描边 / 描边宽
  private shapeKindLabel(): string {
    const labels: string[] = ['矩形', '圆角矩形', '椭圆', '直线'];
    return labels[this.shapeKind] ?? '形状';
  }

  // M11d 仿制图章选项：取源按钮/对齐开关 + 提示
  @Builder
  stampOptions() {
    Row({ space: 10 }) {
      Button(this.clonePicking ? '点击画布取源…' : '取源')
        .fontSize(12).height(28).width(this.clonePicking ? 130 : 56)
        .backgroundColor(this.clonePicking ? $r('app.color.accent') : $r('app.color.surface'))
        .fontColor(this.clonePicking ? $r('app.color.on_accent') : $r('app.color.text_primary'))
        .border({ width: 1, color: $r('app.color.border') })
        .onClick((): void => {
          this.onClonePickToggle();
        })
      Row({ space: 4 }) {
        Text('对齐')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
        Toggle({ type: ToggleType.Checkbox, isOn: this.cloneAligned })
          .selectedColor($r('app.color.accent'))
          .onChange((on: boolean): void => {
            this.cloneAligned = on;
            this.onCloneAligned(on);
          })
      }
      .height(28)
      Text(this.cloneSourceSet ? '已取源 · 涂抹克隆' :
        'Alt+点击或取源模式选点 → 涂抹克隆')
        .fontSize(11)
        .fontColor(this.cloneSourceSet ? $r('app.color.text_secondary') : $r('app.color.danger'))
    }
    .height(36)
  }

  @Builder
  shapeOptions() {
    Row({ space: 10 }) {
      Button(this.shapeKindLabel())
        .fontSize(12).height(28).width(84)
        .backgroundColor($r('app.color.surface'))
        .fontColor($r('app.color.text_primary'))
        .border({ width: 1, color: $r('app.color.border') })
        .onClick((): void => {
          this.shapeKind = (this.shapeKind + 1) % 4;
        })
      Row({ space: 4 }) {
        Text('填充')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
        Toggle({ type: ToggleType.Checkbox, isOn: this.shapeFill })
          .selectedColor($r('app.color.accent'))
          .onChange((on: boolean): void => {
            this.shapeFill = on;
          })
        Text('描边')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
        Toggle({ type: ToggleType.Checkbox, isOn: this.shapeStroke })
          .selectedColor($r('app.color.danger'))
          .onChange((on: boolean): void => {
            this.shapeStroke = on;
          })
      }
      .height(28)
      OptionSlider({
        label: '描边宽',
        min: 1,
        max: 40,
        step: 1,
        value: $shapeStrokeWidth,
        suffix: 'px'
      })
      Text('填充=前景 · 描边=背景 · 拖拽绘制')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
    }
    .height(36)
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
      } else if (this.tool === 'pencil') {
        // 铅笔 = 硬度 100 画笔别名：无硬度滑条（强制硬边）
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
            label: '不透明度',
            min: 5,
            max: 100,
            step: 1,
            value: $brushOpacity,
            suffix: '%',
            onCommitted: (v: number): void => {
              this.brushOpacity = v;
            }
          })
          Text('铅笔：硬边无衰减')
            .fontSize(11)
            .fontColor($r('app.color.text_secondary'))
        }
        .height(36)
      } else if (this.tool === 'gradient') {
        this.gradientOptions()
      } else if (this.tool === 'bucket') {
        this.bucketOptions()
      } else if (this.tool === 'shape') {
        this.shapeOptions()
      } else if (this.tool === 'stamp') {
        this.stampOptions()
      } else if (this.tool === 'wand') {
        Row({ space: 10 }) {
          this.selOpButtons()
          this.wandOptions()
        }
        .height(36)
      } else if (this.tool === 'rectSelect') {
        Row({ space: 10 }) {
          Button(this.selShape === 'ellipse' ? '椭圆' : '矩形')
            .fontSize(12).height(28).width(52)
            .backgroundColor($r('app.color.surface'))
            .fontColor($r('app.color.text_primary'))
            .border({ width: 1, color: $r('app.color.border') })
            .onClick((): void => this.onSelShapeToggle())
          this.selOpButtons()
          Text('拖拽出选区 · M 切形状 · Shift=加 Alt=减')
            .fontSize(11)
            .fontColor($r('app.color.text_secondary'))
        }
        .height(36)
      } else if (this.tool === 'eyedropper') {
        Text('点击画布取色（合成结果）')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
      } else if (this.tool === 'move') {
        Text('拖拽移动图层 · 方向键微调（Shift=10px）')
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
      } else if (this.tool === 'lasso') {
        Row({ space: 10 }) {
          this.selOpButtons()
          Text('拖拽画折线，抬起自动闭合 · Shift=加 Alt=减')
            .fontSize(11)
            .fontColor($r('app.color.text_secondary'))
        }
        .height(36)
      } else if (this.tool === 'polyLasso') {
        Row({ space: 10 }) {
          this.selOpButtons()
          Text('点击加点 → 点起点/双击闭合 · Esc 取消')
            .fontSize(11)
            .fontColor($r('app.color.text_secondary'))
        }
        .height(36)
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
