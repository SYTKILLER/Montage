/**
 * M8.1 自由变换手柄 overlay（03 §6 分工：GL 像素真相 + ArkTS 几何交互）。
 * 初始 bbox 的变换框 + 8 手柄（4 角缩放/4 边单轴）+ 框内移动；
 * 状态 (tx, ty, sx, sy)，M = T(t)·T(C0)·S·T(-C0)；拖拽实时回调矩阵驱动 GL 预览。
 * 旋转/斜切 v1 走数值对话框（手柄版登记）。
 */

export interface XformState {
  tx: number;
  ty: number;
  sx: number;
  sy: number;
}

@Component
export struct TransformOverlay {
  @Prop ovLeft: number = 0;
  @Prop ovTop: number = 0;
  @Prop ovWidth: number = 0;
  @Prop ovHeight: number = 0;
  @Prop zoom: number = 1;
  @Prop panX: number = 0;
  @Prop panY: number = 0;
  @Prop pxPerVp: number = 3;
  onMatrix: (tx: number, ty: number, sx: number, sy: number) => void =
    (_tx: number, _ty: number, _sx: number, _sy: number) => {};
  private settings: RenderingContextSettings = new RenderingContextSettings(true);
  private ctx: CanvasRenderingContext2D =
    new CanvasRenderingContext2D(this.settings);
  @State private cur: XformState = { tx: 0, ty: 0, sx: 1, sy: 1 };
  private dragging: boolean = false;
  private dragKind: number = -1;   // 0..7 手柄（角0,2,4,6/边1,3,5,7）；8 = 框内
  private startPtrX: number = 0;
  private startPtrY: number = 0;
  private startState: XformState = { tx: 0, ty: 0, sx: 1, sy: 1 };

  private c0x(): number {
    return this.ovLeft + this.ovWidth / 2;
  }

  private c0y(): number {
    return this.ovTop + this.ovHeight / 2;
  }

  // base（未变换）框的角（i=0..3 顺时针 LT RT RB LB）
  private baseCorner(i: number): number[] {
    const lx = i === 0 || i === 3 ? this.ovLeft : this.ovLeft + this.ovWidth;
    const ly = i === 0 || i === 1 ? this.ovTop : this.ovTop + this.ovHeight;
    return [lx, ly];
  }

  // 状态 s 下角 i 的 doc 坐标
  private cornerAt(i: number, s: XformState): number[] {
    const b = this.baseCorner(i);
    return [this.c0x() + s.sx * (b[0] - this.c0x()) + s.tx,
      this.c0y() + s.sy * (b[1] - this.c0y()) + s.ty];
  }

  // 状态 s 下手柄 h（0..7）的 doc 坐标
  private handleAt(h: number, s: XformState): number[] {
    if (h % 2 === 0) {
      return this.cornerAt(h / 2, s);
    }
    const a = this.cornerAt((h - 1) / 2, s);
    const b = this.cornerAt(((h - 1) / 2 + 1) % 4, s);
    return [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2];
  }

  private docToScreen(x: number, y: number): number[] {
    const k = this.pxPerVp;
    return [(x - this.panX) * this.zoom / k, (y - this.panY) * this.zoom / k];
  }

  private screenToDoc(xVp: number, yVp: number): number[] {
    const k = this.pxPerVp;
    return [this.panX + xVp * k / this.zoom, this.panY + yVp * k / this.zoom];
  }

  private hitHandle(docX: number, docY: number): number {
    // 命中用屏幕半径（15vp ≈ 足够手指/鼠标）
    for (let h = 0; h < 8; ++h) {
      const p = this.docToScreen(this.handleAt(h, this.cur)[0], this.handleAt(h, this.cur)[1]);
      if (Math.abs(p[0] - docX * 0) < 1e9 && false) {
        continue;  // 占位（逻辑见下）
      }
    }
    // 屏幕空间命中
    for (let h = 0; h < 8; ++h) {
      const hp = this.handleAt(h, this.cur);
      const sp = this.docToScreen(hp[0], hp[1]);
      const ptr = [docX * 0 + docX, docY];  // docX/Y 这里实为屏幕坐标（调用方换算）
      void ptr;
    }
    return this.hitHandleScreen(docX, docY);
  }

  private hitHandleScreen(sx: number, sy: number): number {
    for (let h = 0; h < 8; ++h) {
      const hp = this.handleAt(h, this.cur);
      const sp = this.docToScreen(hp[0], hp[1]);
      if (Math.abs(sp[0] - sx) <= 16 && Math.abs(sp[1] - sy) <= 16) {
        return h;
      }
    }
    return -1;
  }

  private inBoxScreen(sx: number, sy: number): boolean {
    let minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
    for (let i = 0; i < 4; ++i) {
      const p = this.docToScreen(this.cornerAt(i, this.cur)[0], this.cornerAt(i, this.cur)[1]);
      minX = Math.min(minX, p[0]);
      minY = Math.min(minY, p[1]);
      maxX = Math.max(maxX, p[0]);
      maxY = Math.max(maxY, p[1]);
    }
    return sx >= minX && sx <= maxX && sy >= minY && sy <= maxY;
  }

  private redraw(): void {
    const c = this.ctx;
    c.clearRect(0, 0, c.width, c.height);
    c.strokeStyle = '#00E5FF';
    c.lineWidth = 1.5;
    c.beginPath();
    for (let i = 0; i < 4; ++i) {
      const p = this.docToScreen(this.cornerAt(i, this.cur)[0], this.cornerAt(i, this.cur)[1]);
      if (i === 0) {
        c.moveTo(p[0], p[1]);
      } else {
        c.lineTo(p[0], p[1]);
      }
    }
    c.closePath();
    c.stroke();
    c.fillStyle = '#FFFFFF';
    for (let h = 0; h < 8; ++h) {
      const p = this.docToScreen(this.handleAt(h, this.cur)[0], this.handleAt(h, this.cur)[1]);
      c.fillRect(p[0] - 5, p[1] - 5, 10, 10);
      c.strokeRect(p[0] - 5, p[1] - 5, 10, 10);
    }
  }

  private applyDrag(ptrSx: number, ptrSy: number): void {
    const s0 = this.startState;
    if (this.dragKind === 8) {
      // 屏幕位移 → doc 位移：delta_doc = delta_screen * pxPerVp / zoom
      const kMove = this.pxPerVp / this.zoom;
      this.cur = { tx: s0.tx + (ptrSx - this.startPtrSx()) * kMove,
        ty: s0.ty + (ptrSy - this.startPtrSy()) * kMove, sx: s0.sx, sy: s0.sy };
      this.emit();
      return;
    }
    if (this.dragKind < 0) {
      return;
    }
    // 手柄缩放：对角（start 态）固定
    const oppH = (this.dragKind + 4) % 8;
    const F = this.handleAt(oppH, s0);
    const H0 = this.handleAt(this.dragKind, s0);
    let sx = s0.sx;
    let sy = s0.sy;
    const denomX = H0[0] - F[0];
    const denomY = H0[1] - F[1];
    // 指针是屏幕坐标 → doc
    const k = this.pxPerVp / this.zoom;
    const ptrDocX = this.panX + ptrSx * k;
    const ptrDocY = this.panY + ptrSy * k;
    if (Math.abs(denomX) > 1) {
      sx = (ptrDocX - F[0]) / denomX * s0.sx;
    }
    if (Math.abs(denomY) > 1) {
      sy = (ptrDocY - F[1]) / denomY * s0.sy;
    }
    // 对角固定约束（围绕 F 的缩放）：t = F - C0 - S·(F - C0)
    const tx = F[0] - this.c0x() - sx * (F[0] - this.c0x());
    const ty = F[1] - this.c0y() - sy * (F[1] - this.c0y());
    this.cur = { tx: tx, ty: ty, sx: Math.max(0.02, sx), sy: Math.max(0.02, sy) };
    this.emit();
  }

  private startPtrSx(): number {
    return this.startPtrX;
  }

  private startPtrSy(): number {
    return this.startPtrY;
  }

  private emit(): void {
    this.onMatrix(this.cur.tx, this.cur.ty, this.cur.sx, this.cur.sy);
  }

  build() {
    Canvas(this.ctx)
      .width('100%')
      .height('100%')
      .onReady((): void => {
        this.redraw();
      })
      .onTouch((e: TouchEvent): void => {
        if (e.touches.length < 1 && e.changedTouches.length < 1) {
          return;
        }
        const t: TouchObject = e.changedTouches.length > 0 ? e.changedTouches[0] : e.touches[0];
        if (e.type === TouchType.Down) {
          const hit = this.hitHandleScreen(t.x, t.y);
          if (hit >= 0) {
            this.dragKind = hit;
          } else if (this.inBoxScreen(t.x, t.y)) {
            this.dragKind = 8;
          } else {
            return;  // 框外：穿透给画布导航
          }
          this.dragging = true;
          this.startPtrX = t.x;
          this.startPtrY = t.y;
          this.startState = { tx: this.cur.tx, ty: this.cur.ty, sx: this.cur.sx, sy: this.cur.sy };
        } else if (e.type === TouchType.Move && this.dragging) {
          this.applyDrag(t.x, t.y);
          this.redraw();
        } else if ((e.type === TouchType.Up || e.type === TouchType.Cancel) && this.dragging) {
          this.dragging = false;
          this.applyDrag(t.x, t.y);
          this.redraw();
        }
      })
  }
}
