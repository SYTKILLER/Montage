export const SWATCHES: string[] = ['#D93025', '#1A6BD9', '#188038', '#F9AB00', '#111111', '#FFFFFF'];

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
  @Link shapeKind: number;       // 0=矩形 1=圆角矩形 2=椭圆 3=多边形 4=星形 5=直线
  @Link shapeFill: boolean;
  @Link shapeStroke: boolean;
  @Link shapeStrokeWidth: number;
  // M18 live 形状再编辑（>0 = 活动 live 矩形存在，圆角滑条可用）
  @Link shapeLiveRadius: number;
  onShapeRadiusCommit: (v: number) => void = (_v: number) => {};
  // M18.1 直接选择
  @Prop dsSelectedCount: number = 0;
  onDsDelete: () => void = () => {};
  // M18c 钢笔（pending = 进行中锚点数）
  @Prop penPending: number = 0;
  @Prop penClosed: boolean = false;
  onPenClose: () => void = () => {};
  onPenCommit: () => void = () => {};
  onPenSave: () => void = () => {};
  onPenCancel: () => void = () => {};
  // M11d 仿制图章
  @Link cloneAligned: boolean;
  @Prop cloneSourceSet: boolean = false;
  @Prop clonePicking: boolean = false;
  onCloneAligned: (on: boolean) => void = (_on: boolean) => {};
  onClonePickToggle: () => void = () => {};
  // M11f 历史画笔
  @Link historyBrushSteps: number;
  // M19a 修复族
  @Prop spotHealMode: number = 0;  // 0 内容识别/1 创建纹理/2 接近匹配
  onSpotHealMode: (m: number) => void = (_m: number) => {};
  // M20b 液化
  @Prop liquifyTool: number = 0;
  onLiquifyTool: (t: number) => void = (_t: number) => {};
  @Link liquifySize: number;
  @Link liquifyPressure: number;
  // M20c 操控变形
  @Prop puppetMode: number = 1;
  onPuppetMode: (m: number) => void = (_m: number) => {};
  @Prop puppetDensity: number = 1;
  onPuppetDensity: (d: number) => void = (_d: number) => {};
  @Prop puppetPinCount: number = 0;
  onPuppetApply: () => void = () => {};
  onPuppetCancel: () => void = () => {};
  // M20d 透视变形
  onPerspectiveStraighten: () => void = () => {};
  onPerspectiveReset: () => void = () => {};
  onPerspectiveApply: () => void = () => {};
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
  // M14b 对齐/分布（move 工具选项条；PS 同布局）
  @Prop multiSelectCount: number = 1;
  onAlignPick: (mode: number, isAlign: boolean) => void =
    (_mode: number, _isAlign: boolean) => {};
  // M19b 内容感知移动（move 工具选项条三态：0 普通/1 感知移动/2 感知扩展）
  @Prop caMoveMode: number = 0;
  onCaMoveMode: (m: number) => void = (_m: number) => {};

  @Builder
  alignButton(symbol: string, mode: number, isAlign: boolean) {
    Column() {
      SymbolGlyph($r(`sys.symbol.${symbol}`))
        .fontSize(17)
        .fontColor([$r('app.color.text_primary')])
    }
    .justifyContent(FlexAlign.Center)
    .width(30)
    .height(28)
    .borderRadius(6)
    .opacity(isAlign ? (this.multiSelectCount >= 2 ? 1.0 : 0.4) :
      (this.multiSelectCount >= 3 ? 1.0 : 0.4))
    .onClick((): void => this.onAlignPick(mode, isAlign))
  }

  // M14b 对齐/分布按钮组（mode 0..5 = 左/水平中/右/顶/垂直中/底）
  @Builder
  moveAlignOptions() {
    Row({ space: 6 }) {
      Text('对齐')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
      Row({ space: 2 }) {
        this.alignButton('align_left', 0, true)
        this.alignButton('align_center', 1, true)
        this.alignButton('align_right', 2, true)
        this.alignButton('align_top', 3, true)
        this.alignButton('align_middle', 4, true)
        this.alignButton('align_bottom', 5, true)
      }
      .height(28)
      Text('分布')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
      Row({ space: 2 }) {
        this.alignButton('align_left', 0, false)
        this.alignButton('align_center', 1, false)
        this.alignButton('align_right', 2, false)
        this.alignButton('align_top', 3, false)
        this.alignButton('align_middle', 4, false)
        this.alignButton('align_bottom', 5, false)
      }
      .height(28)
      Text('图层面板 Ctrl/Shift+点击多选')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
    }
    .height(36)
  }

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

  // M11c/M18 形状选项：种类循环 / 填充描边 / 描边宽 / live 圆角再编辑
  private shapeKindLabel(): string {
    const labels: string[] = ['矩形', '圆角矩形', '椭圆', '多边形', '星形', '直线'];
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

  // M19a 修复画笔选项：取源（复用克隆源机制）+ 提示
  @Builder
  healOptions() {
    Row({ space: 10 }) {
      Button(this.clonePicking ? '点击画布取源…' : '取源')
        .fontSize(12).height(28).width(this.clonePicking ? 130 : 56)
        .backgroundColor(this.clonePicking ? $r('app.color.accent') : $r('app.color.surface'))
        .fontColor(this.clonePicking ? $r('app.color.on_accent') : $r('app.color.text_primary'))
        .border({ width: 1, color: $r('app.color.border') })
        .onClick((): void => {
          this.onClonePickToggle();
        })
      Text(this.cloneSourceSet ? '已取源 · 涂抹修复（纹理保留 · 光照自适应）' :
        'Alt+点击或取源模式选点 → 涂抹修复')
        .fontSize(11)
        .fontColor(this.cloneSourceSet ? $r('app.color.text_secondary') : $r('app.color.danger'))
    }
    .height(36)
  }

  // M20d 透视选项：拉直/重置/应用
  @Builder
  perspectiveOptions() {
    Row({ space: 10 }) {
      Button('拉直').fontSize(12).height(28).width(56)
        .backgroundColor($r('app.color.surface'))
        .fontColor($r('app.color.text_primary'))
        .border({ width: 1, color: $r('app.color.border') })
        .onClick((): void => this.onPerspectiveStraighten())
      Button('重置').fontSize(12).height(28).width(56)
        .backgroundColor($r('app.color.surface'))
        .fontColor($r('app.color.text_primary'))
        .border({ width: 1, color: $r('app.color.border') })
        .onClick((): void => this.onPerspectiveReset())
      Button('应用').fontSize(12).height(28).width(56)
        .backgroundColor($r('app.color.accent'))
        .fontColor($r('app.color.on_accent'))
        .onClick((): void => this.onPerspectiveApply())
      Text('拖四角变形 · 近水平/垂直边可拉直')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
    }
    .height(36)
  }

  // M20c 操控变形选项：模式/密度切换 + 应用/取消 + 钉数
  @Builder
  puppetOptions() {
    Row({ space: 10 }) {
      Button(this.puppetModeLabel())
        .fontSize(12).height(28).width(76)
        .backgroundColor($r('app.color.surface'))
        .fontColor($r('app.color.text_primary'))
        .border({ width: 1, color: $r('app.color.border') })
        .onClick((): void => this.onPuppetMode((this.puppetMode + 1) % 3))
      Button(this.puppetDensityLabel())
        .fontSize(12).height(28).width(76)
        .backgroundColor($r('app.color.surface'))
        .fontColor($r('app.color.text_primary'))
        .border({ width: 1, color: $r('app.color.border') })
        .onClick((): void => this.onPuppetDensity((this.puppetDensity + 1) % 3))
      Button('应用').fontSize(12).height(28).width(56)
        .backgroundColor($r('app.color.accent'))
        .fontColor($r('app.color.on_accent'))
        .onClick((): void => this.onPuppetApply())
      Button('取消').fontSize(12).height(28).width(56)
        .backgroundColor($r('app.color.surface'))
        .fontColor($r('app.color.text_primary'))
        .border({ width: 1, color: $r('app.color.border') })
        .onClick((): void => this.onPuppetCancel())
      Text(`钉 ${this.puppetPinCount} · 点击画布加钉，拖钉变形`)
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
    }
    .height(36)
  }

  private puppetModeLabel(): string {
    const labels: string[] = ['刚体', '正常', '扭曲'];
    return labels[this.puppetMode] ?? '正常';
  }

  private puppetDensityLabel(): string {
    const labels: string[] = ['更少', '正常', '较多'];
    return labels[this.puppetDensity] ?? '正常';
  }

  // M20b 液化选项：7 工具切换 + 直径/压力滑条
  @Builder
  liquifyOptions() {
    Row({ space: 10 }) {
      Button(this.liquifyToolLabel())
        .fontSize(12).height(28).width(96)
        .backgroundColor($r('app.color.surface'))
        .fontColor($r('app.color.text_primary'))
        .border({ width: 1, color: $r('app.color.border') })
        .onClick((): void => this.onLiquifyTool((this.liquifyTool + 1) % 8))
      OptionSlider({ label: '直径', value: $liquifySize, min: 10, max: 600, step: 2,
        suffix: 'px', onCommitted: (_v: number): void => {} })
      OptionSlider({ label: '压力', value: $liquifyPressure, min: 1, max: 100, step: 1,
        suffix: '%', onCommitted: (_v: number): void => {} })
      Text('拖拽变形 · 每笔一步历史')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
    }
    .height(36)
  }

  private liquifyToolLabel(): string {
    const labels: string[] = ['向前变形', '重建', '平滑', '顺时针扭曲', '逆时针扭曲', '褶皱', '膨胀', '左推'];
    return labels[this.liquifyTool] ?? '向前变形';
  }

  // M19a 污点修复选项：三模式切换 + 提示（无源，提交时计算）
  @Builder
  spotHealOptions() {
    Row({ space: 10 }) {
      Row({ space: 6 }) {
        Button('内容识别').fontSize(12).height(28)
          .backgroundColor(this.spotHealMode === 0 ? $r('app.color.accent') : $r('app.color.surface'))
          .fontColor(this.spotHealMode === 0 ? $r('app.color.on_accent') : $r('app.color.text_primary'))
          .onClick((): void => this.onSpotHealMode(0))
        Button('创建纹理').fontSize(12).height(28)
          .backgroundColor(this.spotHealMode === 1 ? $r('app.color.accent') : $r('app.color.surface'))
          .fontColor(this.spotHealMode === 1 ? $r('app.color.on_accent') : $r('app.color.text_primary'))
          .onClick((): void => this.onSpotHealMode(1))
        Button('接近匹配').fontSize(12).height(28)
          .backgroundColor(this.spotHealMode === 2 ? $r('app.color.accent') : $r('app.color.surface'))
          .fontColor(this.spotHealMode === 2 ? $r('app.color.on_accent') : $r('app.color.text_primary'))
          .onClick((): void => this.onSpotHealMode(2))
      }
      .height(28)
      Text('涂抹污点，松手即修复')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
    }
    .height(36)
  }

  // M19a 修补工具选项：提示
  @Builder
  patchOptions() {
    Row({ space: 10 }) {
      Text('先建选区 → 在选区内拖向纹理源 → 松手应用')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
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
          this.shapeKind = (this.shapeKind + 1) % 6;
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
      Text('填充=前景 · 描边=背景 · 拖拽建形状层')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
      if (this.shapeLiveRadius > 0) {
        OptionSlider({
          label: '圆角',
          min: 0,
          max: 200,
          step: 1,
          value: $shapeLiveRadius,
          suffix: 'px',
          onCommitted: (v: number): void => {
            this.onShapeRadiusCommit(v);
          }
        })
      }
    }
    .height(36)
  }

  // M18c 钢笔选项：锚点计数 + 闭合/提交/存路径/取消
  @Builder
  penOptions() {
    Row({ space: 10 }) {
      Text(`锚点 ${Math.floor(this.penPending / 2)}${this.penClosed ? ' · 已闭合' : ''}`)
        .fontSize(12)
        .fontColor($r('app.color.text_secondary'))
      Button('闭合')
        .fontSize(12).height(28)
        .onClick((): void => {
          this.onPenClose();
        })
      Button('成形状层')
        .fontSize(12).height(28)
        .backgroundColor($r('app.color.accent'))
        .fontColor($r('app.color.on_accent'))
        .onClick((): void => {
          this.onPenCommit();
        })
      Button('存为路径')
        .fontSize(12).height(28)
        .onClick((): void => {
          this.onPenSave();
        })
      Button('取消')
        .fontSize(12).height(28)
        .onClick((): void => {
          this.onPenCancel();
        })
      Text('点击加锚点 · 点首点闭合')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
    }
    .height(36)
  }

  // M18.1 直接选择选项：选中计数 + 删除锚点 + 提示
  @Builder
  dsOptions() {
    Row({ space: 10 }) {
      Text(`选中 ${this.dsSelectedCount} 锚点`)
        .fontSize(12)
        .fontColor($r('app.color.text_secondary'))
      Button('删除锚点')
        .fontSize(12).height(28)
        .enabled(this.dsSelectedCount > 0)
        .onClick((): void => {
          this.onDsDelete();
        })
      Text('拖锚点/手柄/段 · 空白框选 · Shift 加选 · Alt+拖柄断开平滑')
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
    }
    .height(36)
  }
  // M11e 效果工具选项：直径/不透明度 + 提示（强度 v1 固定 50%）
  @Builder
  effectOptions() {
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
      Text(this.effectHint())
        .fontSize(11)
        .fontColor($r('app.color.text_secondary'))
    }
    .height(36)
  }

  private effectHint(): string {
    const hints: Record<string, string> = {
      'blur': '模糊：柔化涂抹区',
      'sharpen': '锐化：增强边缘',
      'smudge': '涂抹：拖颜料',
      'dodge': '减淡：提亮',
      'burn': '加深：压暗',
      'sponge': '海绵：去饱和',
    };
    const v = hints[this.tool];
    return v !== undefined ? v + ' · Shift+同键循环' : '';
  }

  // M11f 历史画笔选项：源步数
  @Builder
  historyBrushOptions() {
    Row({ space: 10 }) {
      OptionSlider({
        label: '回溯',
        min: 1,
        max: 10,
        step: 1,
        value: $historyBrushSteps,
        suffix: ' 步'
      })
      Text(`从 ${this.historyBrushSteps} 步前的快照恢复当前层涂抹`)
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
      } else if (this.tool === 'blur' || this.tool === 'sharpen' || this.tool === 'smudge'
        || this.tool === 'dodge' || this.tool === 'burn' || this.tool === 'sponge') {
        this.effectOptions()
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
      } else if (this.tool === 'pen') {
        this.penOptions()
      } else if (this.tool === 'directSelect') {
        this.dsOptions()
      } else if (this.tool === 'stamp') {
        this.stampOptions()
      } else if (this.tool === 'historyBrush') {
        this.historyBrushOptions()
      } else if (this.tool === 'liquify') {
        this.liquifyOptions()
      } else if (this.tool === 'puppet') {
        this.puppetOptions()
      } else if (this.tool === 'perspective') {
        this.perspectiveOptions()
      } else if (this.tool === 'heal') {
        this.healOptions()
      } else if (this.tool === 'spotHeal') {
        this.spotHealOptions()
      } else if (this.tool === 'patch') {
        this.patchOptions()
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
        Row({ space: 10 }) {
          this.moveAlignOptions()
          Row({ space: 6 }) {
            Button('普通').fontSize(12).height(28)
              .backgroundColor(this.caMoveMode === 0 ? $r('app.color.accent') : $r('app.color.surface'))
              .fontColor(this.caMoveMode === 0 ? $r('app.color.on_accent') : $r('app.color.text_primary'))
              .onClick((): void => this.onCaMoveMode(0))
            Button('感知移动').fontSize(12).height(28)
              .backgroundColor(this.caMoveMode === 1 ? $r('app.color.accent') : $r('app.color.surface'))
              .fontColor(this.caMoveMode === 1 ? $r('app.color.on_accent') : $r('app.color.text_primary'))
              .onClick((): void => this.onCaMoveMode(1))
            Button('感知扩展').fontSize(12).height(28)
              .backgroundColor(this.caMoveMode === 2 ? $r('app.color.accent') : $r('app.color.surface'))
              .fontColor(this.caMoveMode === 2 ? $r('app.color.on_accent') : $r('app.color.text_primary'))
              .onClick((): void => this.onCaMoveMode(2))
          }
          .height(28)
          Text(this.caMoveMode === 0 ? '拖拽移动图层 · 方向键微调（Shift=10px）'
            : '选区内拖拽 → 松手感知应用（需选区）')
            .fontSize(11)
            .fontColor($r('app.color.text_secondary'))
        }
        .height(36)
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
