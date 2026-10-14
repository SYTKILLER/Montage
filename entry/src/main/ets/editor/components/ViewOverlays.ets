/**
 * M15a 视图覆盖层：标尺条（RulerStrip）+ 网格/参考线 Canvas（GridGuidesOverlay）。
 * 均为纯视觉层（画布交互仍走 XComponent 的 onTouch 分发；标尺条自身挂 onTouch 由 Index 处理）。
 * 文档坐标 ↔ 视口换算与 Index 同式：screenVp = (doc - pan) * zoom / pxPerVp。
 */

const TICK_MIN_VP: number = 48;      // 主刻度最小间距（vp）
export const RULER_VP: number = 18;  // 标尺条宽（vp；Index 布局与触摸换算共用）
export const RULER_BG: string = '#E8E8E8';
const RULER_TICK: string = '#5A5A5A';
const RULER_TEXT: string = '#3A3A3A';
const GUIDE_COLOR: string = '#00AEEF';
const GRID_COLOR: string = '#9A9AA0';

@Component
export struct RulerStrip {
  horizontal: boolean = true;
  @Prop @Watch('redraw') zoom: number = 1;
  @Prop @Watch('redraw') panX: number = 0;
  @Prop @Watch('redraw') panY: number = 0;
  @Prop @Watch('redraw') pxPerVp: number = 3;
  @Prop @Watch('redraw') docW: number = 0;
  @Prop @Watch('redraw') docH: number = 0;
  @Prop @Watch('redraw') unit: number = 0;         // 0 = px、1 = 百分比（A-2）
  @Prop @Watch('redraw') previewPos: number = -1;  // 拖拽预览（文档 px；<0 = 无）
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(
    new RenderingContextSettings(true));
  private ready: boolean = false;
  private w: number = 0;
  private h: number = 0;

  redraw(): void {
    if (this.ready) {
      this.draw();
    }
  }

  private draw(): void {
    const c = this.ctx;
    c.clearRect(0, 0, this.w, this.h);
    c.fillStyle = RULER_BG;
    c.fillRect(0, 0, this.w, this.h);
    if (this.docW <= 0 || this.docH <= 0) {
      return;
    }
    // 主刻度步长：文档域候选中取首个屏上 ≥ TICK_MIN_VP 的（PS 同族自适应）
    const candidates: number[] = [1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000];
    const k = Math.max(0.5, this.pxPerVp);
    let step = candidates[candidates.length - 1];
    for (const s of candidates) {
      if (s * this.zoom / k >= TICK_MIN_VP) {
        step = s;
        break;
      }
    }
    c.strokeStyle = RULER_TICK;
    c.fillStyle = RULER_TEXT;
    c.lineWidth = 1;
    c.font = '9px sans-serif';
    if (this.horizontal) {
      c.textAlign = 'left';
      c.textBaseline = 'top';
      const docStart = Math.max(0, this.panX);
      const docEnd = this.panX + (this.w * k) / this.zoom;
      const minor = step / 5;
      for (let pos = Math.floor(docStart / minor) * minor; pos <= docEnd; pos += minor) {
        if (pos < 0) {
          continue;
        }
        const sx = (pos - this.panX) * this.zoom / k;
        const isMajor = Math.abs(pos % step) < 0.001 || Math.abs(pos % step - step) < 0.001;
        const len = isMajor ? 9 : 4;
        c.beginPath();
        c.moveTo(sx, this.h);
        c.lineTo(sx, this.h - len);
        c.stroke();
        if (isMajor) {
          const label = this.unit === 1
            ? `${Math.round(pos / this.docW * 100)}%`
            : `${Math.round(pos)}`;
          c.fillText(label, sx + 2, 1);
        }
      }
    } else {
      c.textAlign = 'left';
      c.textBaseline = 'middle';
      const docStart = Math.max(0, this.panY);
      const docEnd = this.panY + (this.h * k) / this.zoom;
      const minor = step / 5;
      for (let pos = Math.floor(docStart / minor) * minor; pos <= docEnd; pos += minor) {
        if (pos < 0) {
          continue;
        }
        const sy = (pos - this.panY) * this.zoom / k;
        const isMajor = Math.abs(pos % step) < 0.001 || Math.abs(pos % step - step) < 0.001;
        const len = isMajor ? 9 : 4;
        c.beginPath();
        c.moveTo(this.w, sy);
        c.lineTo(this.w - len, sy);
        c.stroke();
        if (isMajor) {
          const label = this.unit === 1
            ? `${Math.round(pos / this.docH * 100)}%`
            : `${Math.round(pos)}`;
          // 竖排标签：平移到刻度处再旋转 90°
          c.save();
          c.translate(2, sy);
          c.rotate(-Math.PI / 2);
          c.fillText(label, 2, 0);
          c.restore();
        }
      }
    }
    // 拖拽预览标记（horizontal = 顶部标尺，预览水平参考线的 y → 横向线滑动）
    if (this.previewPos >= 0) {
      c.strokeStyle = GUIDE_COLOR;
      c.lineWidth = 1.5;
      if (this.horizontal) {
        const sy = (this.previewPos - this.panY) * this.zoom / k;
        c.beginPath();
        c.moveTo(0, sy);
        c.lineTo(this.w, sy);
        c.stroke();
      } else {
        const sx = (this.previewPos - this.panX) * this.zoom / k;
        c.beginPath();
        c.moveTo(sx, 0);
        c.lineTo(sx, this.h);
        c.stroke();
      }
    }
  }

  build() {
    Canvas(this.ctx)
      .width('100%')
      .height('100%')
      .onReady((): void => {
        this.ready = true;
        this.draw();
      })
      .onAreaChange((_o: Area, n: Area): void => {
        this.w = Number(n.width);
        this.h = Number(n.height);
        this.redraw();
      })
  }
}

@Component
export struct GridGuidesOverlay {
  @Prop @Watch('redraw') zoom: number = 1;
  @Prop @Watch('redraw') panX: number = 0;
  @Prop @Watch('redraw') panY: number = 0;
  @Prop @Watch('redraw') pxPerVp: number = 3;
  @Prop @Watch('redraw') docW: number = 0;
  @Prop @Watch('redraw') docH: number = 0;
  @Prop @Watch('redraw') gridOn: boolean = false;
  @Prop @Watch('redraw') gridSpacing: number = 100;
  @Prop @Watch('redraw') guidesOn: boolean = true;
  @Prop @Watch('redraw') guidesH: number[] = [];
  @Prop @Watch('redraw') guidesV: number[] = [];
  @Prop @Watch('redraw') preview: number[] = [];  // [orient, posDocPx]；空 = 无
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(
    new RenderingContextSettings(true));
  private ready: boolean = false;
  private w: number = 0;
  private h: number = 0;

  redraw(): void {
    if (this.ready) {
      this.draw();
    }
  }

  private draw(): void {
    const c = this.ctx;
    c.clearRect(0, 0, this.w, this.h);
    if (this.docW <= 0 || this.docH <= 0) {
      return;
    }
    const k = Math.max(0.5, this.pxPerVp);
    const toSX = (doc: number): number => (doc - this.panX) * this.zoom / k;
    const toSY = (doc: number): number => (doc - this.panY) * this.zoom / k;
    const vpPerDoc = this.zoom / k;
    // 文档域可视范围
    const visX0 = Math.max(0, this.panX);
    const visY0 = Math.max(0, this.panY);
    const visX1 = Math.min(this.docW, this.panX + this.w / vpPerDoc);
    const visY1 = Math.min(this.docH, this.panY + this.h / vpPerDoc);
    c.lineWidth = 1;
    // 网格（只在文档域内画）
    if (this.gridOn && this.gridSpacing > 0) {
      c.strokeStyle = GRID_COLOR;
      c.globalAlpha = 0.55;
      c.beginPath();
      for (let x = Math.ceil(visX0 / this.gridSpacing) * this.gridSpacing; x <= visX1; x += this.gridSpacing) {
        c.moveTo(toSX(x), Math.max(0, toSY(0)));
        c.lineTo(toSX(x), Math.min(this.h, toSY(this.docH)));
      }
      for (let y = Math.ceil(visY0 / this.gridSpacing) * this.gridSpacing; y <= visY1; y += this.gridSpacing) {
        c.moveTo(Math.max(0, toSX(0)), toSY(y));
        c.lineTo(Math.min(this.w, toSX(this.docW)), toSY(y));
      }
      c.stroke();
      c.globalAlpha = 1;
    }
    // 参考线（贯穿全画布，PS 语义；实线青色）
    if (this.guidesOn) {
      c.strokeStyle = GUIDE_COLOR;
      c.globalAlpha = 0.9;
      c.beginPath();
      for (const y of this.guidesH) {
        c.moveTo(0, toSY(y));
        c.lineTo(this.w, toSY(y));
      }
      for (const x of this.guidesV) {
        c.moveTo(toSX(x), 0);
        c.lineTo(toSX(x), this.h);
      }
      c.stroke();
      c.globalAlpha = 1;
    }
    // 拖拽预览（虚线）
    if (this.preview.length === 2) {
      c.strokeStyle = GUIDE_COLOR;
      c.setLineDash([4, 3]);
      c.beginPath();
      if (this.preview[0] === 0) {
        c.moveTo(0, toSY(this.preview[1]));
        c.lineTo(this.w, toSY(this.preview[1]));
      } else {
        c.moveTo(toSX(this.preview[1]), 0);
        c.lineTo(toSX(this.preview[1]), this.h);
      }
      c.stroke();
      c.setLineDash([]);
    }
  }

  build() {
    Canvas(this.ctx)
      .width('100%')
      .height('100%')
      .hitTestBehavior(HitTestMode.None)
      .onReady((): void => {
        this.ready = true;
        this.redraw();
      })
      .onAreaChange((_o: Area, n: Area): void => {
        this.w = Number(n.width);
        this.h = Number(n.height);
        this.redraw();
      })
  }
}
