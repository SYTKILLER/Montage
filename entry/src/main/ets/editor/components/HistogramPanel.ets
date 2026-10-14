/**
 * M15b 直方图面板（06 规划 M15）：M9a getHistogram 的 luma 256 桶可视化。
 * 归一化跳过首尾空档（0/255 常见孤立尖峰），容器高度固定 90vp。
 */
@Component
export struct HistogramPanel {
  @Prop @Watch('redraw') luma: number[] = [];
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
    c.fillStyle = '#F4F4F4';
    c.fillRect(0, 0, this.w, this.h);
    if (this.luma.length !== 256 || this.w <= 0) {
      return;
    }
    // 峰值归一化：跳过 0/255 桶（黑/白场孤立尖峰压扁其余分布）
    let max = 1;
    for (let i = 1; i < 255; ++i) {
      if (this.luma[i] > max) {
        max = this.luma[i];
      }
    }
    const barW = this.w / 256;
    c.fillStyle = '#4A4A4A';
    for (let i = 0; i < 256; ++i) {
      const v = Math.min(1, this.luma[i] / max);
      const bh = v * (this.h - 6);
      c.fillRect(i * barW, this.h - bh, Math.max(1, barW - 0.5), bh);
    }
  }

  build() {
    Column({ space: 4 }) {
      Text('直方图（明度）')
        .fontSize(12)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary'))
        .width('100%')
        .padding({ left: 8 })
      Canvas(this.ctx)
        .width('100%')
        .height(90)
        .borderRadius(6)
        .backgroundColor('#F4F4F4')
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
    .width('100%')
    .padding(4)
    .borderRadius(10)
    .backgroundColor($r('app.color.bar_background'))
  }
}
