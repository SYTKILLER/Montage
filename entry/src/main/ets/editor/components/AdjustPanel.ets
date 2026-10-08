/**
 * M9 调整面板：kind 切换（8 类）+ 参数滑条 + 曲线编辑器（直方图背景+控制点拖拽）。
 * 受控组件：活动调整层 DTO 由父组件传入；操作回调发引擎命令。
 * 曲线拖拽用本地镜像（Up 提交一步撤销）；kind/滑条即时提交。
 */

export interface AdjKindDef {
  kind: number;
  label: string;
}

export const ADJ_KINDS: AdjKindDef[] = [
  { kind: 0, label: '色阶' },
  { kind: 1, label: '曲线' },
  { kind: 2, label: '亮度/对比度' },
  { kind: 3, label: '反相' },
  { kind: 4, label: '阈值' },
  { kind: 5, label: '色调分离' },
  { kind: 6, label: '色彩平衡' },
  { kind: 7, label: '色相/饱和度' },
];

@Component
export struct AdjustPanel {
  @Prop adjKind: number = 0;
  @Prop adjBlack: number = 0;    // 0-250 显示
  @Prop adjWhite: number = 255;
  @Prop adjGammaX100: number = 100;
  @Prop adjP0: number = 0;
  @Prop adjP1: number = 0;
  @Prop adjP2: number = 0;
  @Prop curveXs: number[] = [0, 1];   // 0..1
  @Prop curveYs: number[] = [0, 1];
  @Prop histLuma: number[] = [];
  @Prop isAdjustment: boolean = false;
  onSetKind: (kind: number) => void = (_k: number) => {};
  onLevels: (b: number, w: number, g: number) => void =
    (_b: number, _w: number, _g: number) => {};
  onParams: (p0: number, p1: number, p2: number) => void =
    (_p0: number, _p1: number, _p2: number) => {};
  onCurve: (xs: number[], ys: number[]) => void = (_xs: number[], _ys: number[]) => {};

  private curveCtx: CanvasRenderingContext2D = new CanvasRenderingContext2D(
    new RenderingContextSettings(true));
  private histCtx: CanvasRenderingContext2D = new CanvasRenderingContext2D(
    new RenderingContextSettings(true));
  @State private dragIdx: number = -1;
  @State private localXs: number[] = [0, 1];
  @State private localYs: number[] = [0, 1];

  private readonly P0: number = 24;
  private readonly PSZ: number = 152;

  private toCanvas(x: number, y: number): number[] {
    return [this.P0 + x * this.PSZ, this.P0 + (1 - y) * this.PSZ];
  }

  private toCurve(cx: number, cy: number): number[] {
    const x = (cx - this.P0) / this.PSZ;
    const y = 1 - (cy - this.P0) / this.PSZ;
    return [Math.min(1, Math.max(0, x)), Math.min(1.5, Math.max(-0.5, y))];
  }

  private drawCurve(): void {
    const c = this.curveCtx;
    c.clearRect(0, 0, 200, 200);
    c.strokeStyle = '#333333';
    c.lineWidth = 1;
    for (let i = 1; i < 4; ++i) {
      const p = this.P0 + this.PSZ * i / 4;
      c.beginPath();
      c.moveTo(p, this.P0);
      c.lineTo(p, this.P0 + this.PSZ);
      c.stroke();
      c.beginPath();
      c.moveTo(this.P0, p);
      c.lineTo(this.P0 + this.PSZ, p);
      c.stroke();
    }
    c.strokeStyle = '#555555';
    c.beginPath();
    const d0 = this.toCanvas(0, 0);
    const d1 = this.toCanvas(1, 1);
    c.moveTo(d0[0], d0[1]);
    c.lineTo(d1[0], d1[1]);
    c.stroke();
    c.strokeStyle = '#00E5FF';
    c.lineWidth = 2;
    c.beginPath();
    const xs = this.localXs;
    const ys = this.localYs;
    for (let i = 0; i <= 32; ++i) {
      const x = i / 32;
      let y = x;
      if (xs.length >= 2) {
        if (x <= xs[0]) {
          y = ys[0];
        } else if (x >= xs[xs.length - 1]) {
          y = ys[ys.length - 1];
        } else {
          for (let k = 0; k + 1 < xs.length; ++k) {
            if (x >= xs[k] && x <= xs[k + 1]) {
              const t = (x - xs[k]) / Math.max(1e-6, xs[k + 1] - xs[k]);
              y = ys[k] + (ys[k + 1] - ys[k]) * t;
              break;
            }
          }
        }
      }
      const p = this.toCanvas(x, y);
      if (i === 0) {
        c.moveTo(p[0], p[1]);
      } else {
        c.lineTo(p[0], p[1]);
      }
    }
    c.stroke();
    c.fillStyle = '#FFFFFF';
    for (let k = 0; k < xs.length; ++k) {
      const p = this.toCanvas(xs[k], ys[k]);
      c.beginPath();
      c.arc(p[0], p[1], 5, 0, 6.283);
      c.fill();
    }
  }

  private drawHist(): void {
    const c = this.histCtx;
    c.clearRect(0, 0, 200, 40);
    c.fillStyle = '#777777';
    let maxV = 1;
    for (let i = 0; i < 256 && i < this.histLuma.length; ++i) {
      maxV = Math.max(maxV, this.histLuma[i]);
    }
    for (let i = 0; i < 256 && i < this.histLuma.length; ++i) {
      const h = this.histLuma[i] / maxV * 38;
      c.fillRect(this.P0 + i * this.PSZ / 256, 38 - h, this.PSZ / 256 + 0.5, h);
    }
  }

  @Builder
  slider(label: string, min: number, max: number, value: number,
    onCommit: (v: number) => void, text: string) {
    Row({ space: 6 }) {
      Text(label).fontSize(11).fontColor($r('app.color.text_secondary')).width(50)
      Slider({ value: Math.min(max, Math.max(min, value)), min: min, max: max, step: 1 })
        .layoutWeight(1)
        .height(22)
        .onChange((v: number, mode: SliderChangeMode): void => {
          if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
            onCommit(Math.round(v));
          }
        })
      Text(text).fontSize(10).fontColor($r('app.color.text_primary')).width(36)
    }
    .width('100%')
  }

  build() {
    Column({ space: 8 }) {
      Text('调整')
        .fontSize(13)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .width('100%')
      Flex({ wrap: FlexWrap.Wrap }) {
        ForEach(ADJ_KINDS, (k: AdjKindDef) => {
          Text(k.label)
            .fontSize(10)
            .fontColor(this.adjKind === k.kind && this.isAdjustment
              ? $r('app.color.on_accent') : $r('app.color.text_primary'))
            .backgroundColor(this.adjKind === k.kind && this.isAdjustment
              ? $r('app.color.accent') : $r('app.color.surface'))
            .borderRadius(6)
            .padding({ left: 8, right: 8, top: 4, bottom: 4 })
            .margin(2)
            .onClick((): void => this.onSetKind(k.kind))
        }, (k: AdjKindDef): string => `${k.kind}`)
      }.width('100%')

      if (this.adjKind === 0) {
        this.slider('黑', 0, 250, this.adjBlack, (v: number): void => {
          this.onLevels(v, this.adjWhite, this.adjGammaX100 / 100);
        }, `${Math.round(this.adjBlack)}`)
        this.slider('白', 5, 255, this.adjWhite, (v: number): void => {
          this.onLevels(this.adjBlack, v, this.adjGammaX100 / 100);
        }, `${Math.round(this.adjWhite)}`)
        this.slider('γ', 10, 1000, this.adjGammaX100, (v: number): void => {
          this.onLevels(this.adjBlack, this.adjWhite, v / 100);
        }, (this.adjGammaX100 / 100).toFixed(2))
      } else if (this.adjKind === 1) {
        Canvas(this.histCtx)
          .width(200).height(40)
          .onReady((): void => this.drawHist())
        Canvas(this.curveCtx)
          .width(200).height(200)
          .onReady((): void => this.drawCurve())
          .onTouch((e: TouchEvent): void => this.curveTouch(e))
        Text('拖点调整；空白单击加点')
          .fontSize(10)
          .fontColor($r('app.color.text_secondary'))
      } else if (this.adjKind === 2) {
        this.slider('亮度', -150, 150, this.adjP0, (v: number): void => {
          this.onParams(v, this.adjP1, this.adjP2);
        }, `${Math.round(this.adjP0)}`)
        this.slider('对比度', -50, 100, this.adjP1, (v: number): void => {
          this.onParams(this.adjP0, v, this.adjP2);
        }, `${Math.round(this.adjP1)}`)
      } else if (this.adjKind === 3) {
        Text('反相：无参数（即时生效）')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
      } else if (this.adjKind === 4) {
        this.slider('阈值', 0, 255, this.adjP0, (v: number): void => {
          this.onParams(v, 0, 0);
        }, `${Math.round(this.adjP0)}`)
      } else if (this.adjKind === 5) {
        this.slider('色阶数', 2, 32, this.adjP0, (v: number): void => {
          this.onParams(v, 0, 0);
        }, `${Math.round(this.adjP0)}`)
      } else if (this.adjKind === 6) {
        this.slider('红', -100, 100, this.adjP0, (v: number): void => {
          this.onParams(v, this.adjP1, this.adjP2);
        }, `${Math.round(this.adjP0)}`)
        this.slider('绿', -100, 100, this.adjP1, (v: number): void => {
          this.onParams(this.adjP0, v, this.adjP2);
        }, `${Math.round(this.adjP1)}`)
        this.slider('蓝', -100, 100, this.adjP2, (v: number): void => {
          this.onParams(this.adjP0, this.adjP1, v);
        }, `${Math.round(this.adjP2)}`)
      } else if (this.adjKind === 7) {
        Text('色相/饱和度：GPU 路径后续批次')
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
      }
    }
    .width('100%')
    .padding(8)
  }

  private curveTouch(e: TouchEvent): void {
    if (e.touches.length < 1 && e.changedTouches.length < 1) {
      return;
    }
    const t: TouchObject = e.changedTouches.length > 0 ? e.changedTouches[0] : e.touches[0];
    const cx = t.x;
    const cy = t.y;
    if (e.type === TouchType.Down) {
      // 同步本地镜像
      this.localXs = this.curveXs.slice();
      this.localYs = this.curveYs.slice();
      let hit = -1;
      let best = 14;
      for (let k = 0; k < this.localXs.length; ++k) {
        const p = this.toCanvas(this.localXs[k], this.localYs[k]);
        const dist = Math.hypot(p[0] - cx, p[1] - cy);
        if (dist < best) {
          best = dist;
          hit = k;
        }
      }
      if (hit >= 0) {
        this.dragIdx = hit;
      } else {
        const p = this.toCurve(cx, cy);
        this.localXs.push(p[0]);
        this.localYs.push(p[1]);
        // 排序（x 单调）
        const order: number[] = this.localXs.map((v: number, idx: number): number => idx)
          .sort((a: number, b: number): number => this.localXs[a] - this.localXs[b]);
        this.localXs = order.map((idx: number): number => this.localXs[idx]);
        this.localYs = order.map((idx: number): number => this.localYs[idx]);
        this.dragIdx = this.localXs.indexOf(p[0]);
        this.drawCurve();
        this.onCurve(this.localXs.slice(), this.localYs.slice());  // 加点即时提交
      }
    } else if (e.type === TouchType.Move && this.dragIdx >= 0) {
      const p = this.toCurve(cx, cy);
      if (this.dragIdx > 0) {
        p[0] = Math.max(this.localXs[this.dragIdx - 1] + 0.02, p[0]);
      }
      if (this.dragIdx < this.localXs.length - 1) {
        p[0] = Math.min(this.localXs[this.dragIdx + 1] - 0.02, p[0]);
      }
      this.localXs[this.dragIdx] = p[0];
      this.localYs[this.dragIdx] = p[1];
      this.drawCurve();
    } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.dragIdx >= 0) {
      this.dragIdx = -1;
      this.onCurve(this.localXs.slice(), this.localYs.slice());  // Up 一步提交
      this.drawCurve();
    }
  }
}
