/**
 * M8.1 自由变换手柄 overlay（03 §6 分工：GL 像素真相 + ArkTS 几何交互）。
 * 初始 bbox 的变换框 + 12 手柄（4 角缩放/4 边单轴缩放·Ctrl 斜切/4 外侧旋转柄）+ 框内移动；
 * 状态 (tx, ty, sx, sy, rot, kx, ky)，线性部分 M' = R(rot)·Sk(kx,ky)·S（doc Y 向下，
 * 正角度 = 视觉顺时针）；拖拽全部相对起始态解算（无起步跳变），实时回调矩阵驱动 GL 预览。
 * P2（M8.1 登记）：旋转/斜切手柄补齐——旋转柄在四角外侧沿对角方向，Shift 吸附 15°；
 * Ctrl+边柄 = 斜切（被拖边沿自身方向滑动、对边固定）；Shift+角柄 = 等比。
 * 数值路径（自由变换对话框）保留。
 */

export interface XformState {
  tx: number;
  ty: number;
  sx: number;
  sy: number;
  rot: number;  // 度（doc Y 向下，正 = 视觉顺时针）
  kx: number;   // H 斜切 tan（局部 x 随 y 剪切）
  ky: number;   // V 斜切 tan（局部 y 随 x 剪切）
}

const ZERO_STATE: XformState = { tx: 0, ty: 0, sx: 1, sy: 1, rot: 0, kx: 0, ky: 0 };

@Component
export struct TransformOverlay {
  @Prop ovLeft: number = 0;
  @Prop ovTop: number = 0;
  @Prop ovWidth: number = 0;
  @Prop ovHeight: number = 0;
  // 审查修复：变换会话中框外触摸会平移/缩放画布（穿透导航），zoom/pan 变化后手柄
  // Canvas 不重绘 → 与 GL 画布错位。视图相关 @Prop 全部挂 @Watch 重绘。
  @Prop @Watch('onViewChanged') zoom: number = 1;
  @Prop @Watch('onViewChanged') panX: number = 0;
  @Prop @Watch('onViewChanged') panY: number = 0;
  @Prop @Watch('onViewChanged') pxPerVp: number = 3;
  onMatrix: (tx: number, ty: number, sx: number, sy: number, rot: number,
    kx: number, ky: number) => void =
    (_tx: number, _ty: number, _sx: number, _sy: number, _rot: number,
      _kx: number, _ky: number) => {};
  // 修饰键实时读取（Index 维护 keyCode held 态；触摸事件不带修饰键）
  getMods: () => boolean[] = (): boolean[] => [false, false, false];
  private settings: RenderingContextSettings = new RenderingContextSettings(true);
  private ctx: CanvasRenderingContext2D =
    new CanvasRenderingContext2D(this.settings);
  @State private cur: XformState = ZERO_STATE;
  private canvasReady: boolean = false;

  private onViewChanged(): void {
    if (this.canvasReady && !this.dragging) {
      this.redraw();
    }
  }
  private dragging: boolean = false;
  // 0..7 手柄（角0,2,4,6/边1,3,5,7）；8..11 旋转柄（对应角 0..3）；12 = 框内
  private dragKind: number = -1;
  private startPtrX: number = 0;
  private startPtrY: number = 0;
  private startState: XformState = ZERO_STATE;

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

  // 线性部分 R·Sk·S（行主序 [a b; c d]）——M' = R(rot)·Sk(kx,ky)·S
  private lin(s: XformState): number[] {
    const r = s.rot * 3.14159265358979323846 / 180.0;
    const c = Math.cos(r);
    const sn = Math.sin(r);
    return [s.sx * (c - sn * s.ky), s.sy * (c * s.kx - sn),
      s.sx * (sn + c * s.ky), s.sy * (sn * s.kx + c)];
  }

  // 纯旋转矩阵（行主序）
  private rotM(deg: number): number[] {
    const r = deg * 3.14159265358979323846 / 180.0;
    return [Math.cos(r), -Math.sin(r), Math.sin(r), Math.cos(r)];
  }

  private applyM(m: number[], vx: number, vy: number): number[] {
    return [m[0] * vx + m[1] * vy, m[2] * vx + m[3] * vy];
  }

  // 状态 s 下角 i 的 doc 坐标
  private cornerAt(i: number, s: XformState): number[] {
    const b = this.baseCorner(i);
    const v = this.applyM(this.lin(s), b[0] - this.c0x(), b[1] - this.c0y());
    return [this.c0x() + v[0] + s.tx, this.c0y() + v[1] + s.ty];
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

  // 旋转柄 h（8..11）屏幕坐标：角沿「角−中心」方向外推
  private rotHandleScreen(i: number): number[] {
    const c = this.docToScreen(this.c0x(), this.c0y());
    const p = this.docToScreen(this.cornerAt(i, this.cur)[0], this.cornerAt(i, this.cur)[1]);
    const dx = p[0] - c[0];
    const dy = p[1] - c[1];
    const len = Math.hypot(dx, dy);
    if (len < 1e-6) {
      return [p[0], p[1] - 30];
    }
    const off = 30.0;
    return [p[0] + dx / len * off, p[1] + dy / len * off];
  }

  private docToScreen(x: number, y: number): number[] {
    const k = this.pxPerVp;
    return [(x - this.panX) * this.zoom / k, (y - this.panY) * this.zoom / k];
  }

  private screenToDoc(xVp: number, yVp: number): number[] {
    const k = this.pxPerVp;
    return [this.panX + xVp * k / this.zoom, this.panY + yVp * k / this.zoom];
  }

  private hitHandleScreen(sx: number, sy: number): number {
    for (let h = 0; h < 8; ++h) {
      const hp = this.handleAt(h, this.cur);
      const sp = this.docToScreen(hp[0], hp[1]);
      if (Math.abs(sp[0] - sx) <= 16 && Math.abs(sp[1] - sy) <= 16) {
        return h;
      }
    }
    for (let i = 0; i < 4; ++i) {
      const rp = this.rotHandleScreen(i);
      if (Math.hypot(rp[0] - sx, rp[1] - sy) <= 16) {
        return 8 + i;
      }
    }
    return -1;
  }

  // 凸四边形内（偶奇射线法；旋转/斜切后 bbox 判定会误吸框外区域）
  private inBoxScreen(sx: number, sy: number): boolean {
    const pts: number[][] = [];
    for (let i = 0; i < 4; ++i) {
      pts.push(this.docToScreen(this.cornerAt(i, this.cur)[0], this.cornerAt(i, this.cur)[1]));
    }
    let inside = false;
    for (let i = 0, j = 3; i < 4; j = i++) {
      const xi = pts[i][0];
      const yi = pts[i][1];
      const xj = pts[j][0];
      const yj = pts[j][1];
      if ((yi > sy) !== (yj > sy) && sx < (xj - xi) * (sy - yi) / (yj - yi) + xi) {
        inside = !inside;
      }
    }
    return inside;
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
    // 旋转柄：角外圆点 + 连接短杆
    for (let i = 0; i < 4; ++i) {
      const cp = this.docToScreen(this.cornerAt(i, this.cur)[0], this.cornerAt(i, this.cur)[1]);
      const rp = this.rotHandleScreen(i);
      c.beginPath();
      c.moveTo(cp[0], cp[1]);
      c.lineTo(rp[0], rp[1]);
      c.stroke();
      c.beginPath();
      c.arc(rp[0], rp[1], 6, 0, 6.2832);
      c.fill();
      c.stroke();
    }
  }

  // 缩放公共解算：沿起止态投影差定标（连续无跳变），返回围绕定点 vF 的平移补偿
  private scaleAxis(e: number[], ptrDoc: number[], h0Doc: number[]): number {
    const e2 = e[0] * e[0] + e[1] * e[1];
    if (e2 < 1e-6) {
      return 1.0;
    }
    const pNow = (ptrDoc[0] - e[2]) * e[0] + (ptrDoc[1] - e[3]) * e[1];
    const p0 = (h0Doc[0] - e[2]) * e[0] + (h0Doc[1] - e[3]) * e[1];
    return 1.0 + (pNow - p0) / e2;
  }

  private applyDrag(ptrSx: number, ptrSy: number): void {
    const s0 = this.startState;
    const mods = this.getMods();
    if (this.dragKind === 12) {
      // 屏幕位移 → doc 位移：delta_doc = delta_screen * pxPerVp / zoom
      const kMove = this.pxPerVp / this.zoom;
      this.cur = { tx: s0.tx + (ptrSx - this.startPtrX) * kMove,
        ty: s0.ty + (ptrSy - this.startPtrY) * kMove, sx: s0.sx, sy: s0.sy,
        rot: s0.rot, kx: s0.kx, ky: s0.ky };
      this.emit();
      return;
    }
    if (this.dragKind < 0) {
      return;
    }
    const ptrDoc = this.screenToDoc(ptrSx, ptrSy);
    const ptr0Doc = this.screenToDoc(this.startPtrX, this.startPtrY);

    if (this.dragKind >= 8) {
      // 旋转：指针绕框中心的角度差（屏幕空间匀比缩放角不变）；Shift 吸附 15°
      const cS = this.docToScreen(this.c0x(), this.c0y());
      const a0 = Math.atan2(this.startPtrY - cS[1], this.startPtrX - cS[0]);
      const a1 = Math.atan2(ptrSy - cS[1], ptrSx - cS[0]);
      let rot = s0.rot + (a1 - a0) * 180.0 / 3.14159265358979323846;
      if (mods[1]) {
        rot = Math.round(rot / 15.0) * 15.0;
      }
      this.cur = { tx: s0.tx, ty: s0.ty, sx: s0.sx, sy: s0.sy, rot: rot,
        kx: s0.kx, ky: s0.ky };
      this.emit();
      return;
    }

    if (this.dragKind % 2 === 1 && mods[0]) {
      // 斜切（Ctrl+边柄）：被拖边沿自身方向滑动 δ，对边固定（解见 NOTICE P2 批）。
      const i = (this.dragKind - 1) / 2;                    // 边 = 角 i..i+1
      const bh = this.handleAt(this.dragKind, ZERO_STATE);  // base 边中点
      const vHx = bh[0] - this.c0x();
      const vHy = bh[1] - this.c0y();
      const horizontal = i === 0 || i === 2;                // 顶/底边（局部横向）
      // 边方向 = L(s0)·局部方向（起态真实边向）
      const l0 = this.lin(s0);
      const dirLocal = horizontal ? [1, 0] : [0, 1];
      let ex = l0[0] * dirLocal[0] + l0[1] * dirLocal[1];
      let ey = l0[2] * dirLocal[0] + l0[3] * dirLocal[1];
      const elen = Math.hypot(ex, ey);
      if (elen < 1e-6) {
        return;
      }
      ex /= elen;
      ey /= elen;
      const dproj = (ptrDoc[0] - ptr0Doc[0]) * ex + (ptrDoc[1] - ptr0Doc[1]) * ey;
      let kx = s0.kx;
      let ky = s0.ky;
      if (horizontal) {
        const denom = 2.0 * s0.sy * vHy;   // 带符号（顶边为负）
        if (Math.abs(denom) > 1e-6) {
          kx = s0.kx + dproj / denom;
        }
      } else {
        const denom = 2.0 * s0.sx * vHx;
        if (Math.abs(denom) > 1e-6) {
          ky = s0.ky + dproj / denom;
        }
      }
      kx = Math.max(-4.0, Math.min(4.0, kx));
      ky = Math.max(-4.0, Math.min(4.0, ky));
      // 平移补偿保持对边固定：t = t0 + R·(kxΔ·(S0·vH).y, kyΔ·(S0·vH).x)
      const r0 = this.rotM(s0.rot);
      const cvec = this.applyM(r0, (kx - s0.kx) * s0.sy * vHy, (ky - s0.ky) * s0.sx * vHx);
      this.cur = { tx: s0.tx + cvec[0], ty: s0.ty + cvec[1], sx: s0.sx, sy: s0.sy,
        rot: s0.rot, kx: kx, ky: ky };
      this.emit();
      return;
    }

    // 缩放（角柄/边柄）：沿起止态投影差定标；对角/对边固定平移补偿
    let sx = s0.sx;
    let sy = s0.sy;
    let vFx = 0.0;
    let vFy = 0.0;
    if (this.dragKind % 2 === 0) {
      const i = this.dragKind / 2;
      const F = this.cornerAt((i + 2) % 4, s0);
      // base 对角坐标
      const bf = this.baseCorner((i + 2) % 4);
      vFx = bf[0] - this.c0x();
      vFy = bf[1] - this.c0y();
      // sx 轴边端点（与对角 F 同局部 y、异 x）：偶角 = (i+3)%4，奇角 = (i+1)%4
      const exEdge = this.cornerAt(i % 2 === 0 ? (i + 3) % 4 : (i + 1) % 4, s0);
      const eyEdge = this.cornerAt(i % 2 === 0 ? (i + 1) % 4 : (i + 3) % 4, s0);
      const e1 = [exEdge[0] - F[0], exEdge[1] - F[1], F[0], F[1]];
      const e2 = [eyEdge[0] - F[0], eyEdge[1] - F[1], F[0], F[1]];
      const h0 = this.cornerAt(i, s0);
      const fx = this.scaleAxis(e1, ptrDoc, h0);
      const fy = this.scaleAxis(e2, ptrDoc, h0);
      sx = Math.max(0.02, s0.sx * fx);
      if (mods[1]) {
        sy = Math.max(0.02, sx * s0.sy / Math.max(1e-6, s0.sx));  // Shift 等比
      } else {
        sy = Math.max(0.02, s0.sy * fy);
      }
    } else {
      const F = this.handleAt((this.dragKind + 4) % 8, s0);
      const h0 = this.handleAt(this.dragKind, s0);
      const vertical = ((this.dragKind - 1) / 2) === 0 || ((this.dragKind - 1) / 2) === 2;
      const e = [h0[0] - F[0], h0[1] - F[1], F[0], F[1]];
      const f = this.scaleAxis(e, ptrDoc, h0);
      if (vertical) {
        sy = Math.max(0.02, s0.sy * f);
      } else {
        sx = Math.max(0.02, s0.sx * f);
      }
      const bh = this.handleAt((this.dragKind + 4) % 8, ZERO_STATE);
      vFx = bh[0] - this.c0x();
      vFy = bh[1] - this.c0y();
    }
    // t = t0 + RSk(s0)·((S0−S)·vF)
    const dfx = (s0.sx - sx) * vFx;
    const dfy = (s0.sy - sy) * vFy;
    const comp = this.applyM(this.lin(s0), dfx, dfy);
    this.cur = { tx: s0.tx + comp[0], ty: s0.ty + comp[1], sx: sx, sy: sy,
      rot: s0.rot, kx: s0.kx, ky: s0.ky };
    this.emit();
  }

  private emit(): void {
    this.onMatrix(this.cur.tx, this.cur.ty, this.cur.sx, this.cur.sy,
      this.cur.rot, this.cur.kx, this.cur.ky);
  }

  build() {
    Canvas(this.ctx)
      .width('100%')
      .height('100%')
      .onReady((): void => {
        this.canvasReady = true;
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
            this.dragKind = 12;
          } else {
            return;  // 框外：穿透给画布导航
          }
          this.dragging = true;
          this.startPtrX = t.x;
          this.startPtrY = t.y;
          this.startState = { tx: this.cur.tx, ty: this.cur.ty, sx: this.cur.sx,
            sy: this.cur.sy, rot: this.cur.rot, kx: this.cur.kx, ky: this.cur.ky };
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
