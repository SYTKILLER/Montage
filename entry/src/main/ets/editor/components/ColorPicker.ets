/**
 * P2 HSV 拾色器（M9/M11 登记项「HSV 色域盘」收口）：
 * 色域盘 = SV 双向渐变 Canvas（白→色相横向 + 透明→黑纵向）+ 色相条 + 预览/hex/RGB
 * + 色板库快选（P1 PresetLib 色板扁平传入）。目标（前景/背景/文字色）由父组件定，
 * 确认回调回传 hex。纯 UI 组件，不触引擎。
 */

// ---------- HSV ↔ RGB / hex（本地纯函数） ----------

export function hsvToRgb(h: number, s: number, v: number): number[] {
  const c = v * s;
  const hp = (h % 360.0 + 360.0) % 360.0 / 60.0;
  const x = c * (1 - Math.abs(hp % 2 - 1));
  let r = 0;
  let g = 0;
  let b = 0;
  if (hp < 1) {
    r = c; g = x;
  } else if (hp < 2) {
    r = x; g = c;
  } else if (hp < 3) {
    g = c; b = x;
  } else if (hp < 4) {
    g = x; b = c;
  } else if (hp < 5) {
    r = x; b = c;
  } else {
    r = c; b = x;
  }
  const m = v - c;
  return [Math.round((r + m) * 255), Math.round((g + m) * 255), Math.round((b + m) * 255)];
}

export function rgbToHsv(r: number, g: number, b: number): number[] {
  const rf = r / 255;
  const gf = g / 255;
  const bf = b / 255;
  const mx = Math.max(rf, gf, bf);
  const mn = Math.min(rf, gf, bf);
  const d = mx - mn;
  let h = 0;
  if (d > 1e-9) {
    if (mx === rf) {
      h = 60 * (((gf - bf) / d) % 6);
    } else if (mx === gf) {
      h = 60 * ((bf - rf) / d + 2);
    } else {
      h = 60 * ((rf - gf) / d + 4);
    }
  }
  if (h < 0) {
    h += 360;
  }
  const s = mx <= 1e-9 ? 0 : d / mx;
  return [h, s, mx];
}

export function rgbToHex(r: number, g: number, b: number): string {
  const hx = (v: number): string => v.toString(16).padStart(2, '0').toUpperCase();
  return `#${hx(r)}${hx(g)}${hx(b)}`;
}

export function hexToRgb(hex: string): number[] {
  const s = hex.replace('#', '');
  if (s.length !== 6) {
    return [0, 0, 0];
  }
  const v = parseInt(s, 16);
  if (Number.isNaN(v)) {
    return [0, 0, 0];
  }
  return [(v >> 16) & 255, (v >> 8) & 255, v & 255];
}

const SV_W = 232;
const SV_H = 180;
const HUE_H = 22;

@Component
export struct HsvPicker {
  @Prop initHex: string = '#000000';
  @Prop title: string = '拾色器';
  @Prop libSwatches: string[] = [];   // 色板库快选（父组件从 PresetLib 扁平化）
  onConfirm: (hex: string) => void = (_hex: string) => {};
  onCancel: () => void = () => {};

  @State private h: number = 0;
  @State private s: number = 0;
  @State private v: number = 0;
  @State private hexBox: string = '';
  private svSettings: RenderingContextSettings = new RenderingContextSettings(true);
  private svCtx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.svSettings);
  private hueSettings: RenderingContextSettings = new RenderingContextSettings(true);
  private hueCtx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.hueSettings);
  private svReady: boolean = false;
  private hueReady: boolean = false;

  aboutToAppear(): void {
    const rgb = hexToRgb(this.initHex);
    const hsv = rgbToHsv(rgb[0], rgb[1], rgb[2]);
    this.h = hsv[0];
    this.s = hsv[1];
    this.v = hsv[2];
    this.hexBox = this.initHex.toUpperCase();
  }

  private curHex(): string {
    const rgb = hsvToRgb(this.h, this.s, this.v);
    return rgbToHex(rgb[0], rgb[1], rgb[2]);
  }

  private curRgb(): number[] {
    return hsvToRgb(this.h, this.s, this.v);
  }

  private setFromHex(hex: string): void {
    const rgb = hexToRgb(hex);
    const hsv = rgbToHsv(rgb[0], rgb[1], rgb[2]);
    this.h = hsv[0];
    this.s = hsv[1];
    this.v = hsv[2];
    this.hexBox = this.curHex();
    this.drawAll();
  }

  private drawAll(): void {
    if (this.svReady) {
      this.drawSv();
    }
    if (this.hueReady) {
      this.drawHue();
    }
  }

  private drawSv(): void {
    const c = this.svCtx;
    c.clearRect(0, 0, SV_W, SV_H);
    const rgb = hsvToRgb(this.h, 1, 1);
    const hueCss = rgbToHex(rgb[0], rgb[1], rgb[2]);
    // 底 = 纯色相
    c.fillStyle = hueCss;
    c.fillRect(0, 0, SV_W, SV_H);
    // 横向：色相 → 白
    const gw = c.createLinearGradient(0, 0, SV_W, 0);
    gw.addColorStop(0, '#FFFFFF');
    gw.addColorStop(1, '#00FFFFFF');
    c.fillStyle = gw;
    c.fillRect(0, 0, SV_W, SV_H);
    // 纵向：透明 → 黑
    const gb = c.createLinearGradient(0, 0, 0, SV_H);
    gb.addColorStop(0, '#00000000');
    gb.addColorStop(1, '#FF000000');
    c.fillStyle = gb;
    c.fillRect(0, 0, SV_W, SV_H);
    // 游标
    const mx = this.s * SV_W;
    const my = (1 - this.v) * SV_H;
    c.beginPath();
    c.arc(mx, my, 6, 0, 6.2832);
    c.strokeStyle = '#FFFFFF';
    c.lineWidth = 2;
    c.stroke();
    c.beginPath();
    c.arc(mx, my, 7.5, 0, 6.2832);
    c.strokeStyle = '#000000';
    c.lineWidth = 1;
    c.stroke();
  }

  private drawHue(): void {
    const c = this.hueCtx;
    c.clearRect(0, 0, SV_W, HUE_H);
    const g = c.createLinearGradient(0, 0, SV_W, 0);
    const stops: string[] = ['#FF0000', '#FFFF00', '#00FF00', '#00FFFF', '#0000FF', '#FF00FF', '#FF0000'];
    for (let i = 0; i < stops.length; ++i) {
      g.addColorStop(i / (stops.length - 1), stops[i]);
    }
    c.fillStyle = g;
    c.fillRect(0, 0, SV_W, HUE_H);
    // 游标
    const mx = this.h / 360 * SV_W;
    c.beginPath();
    c.moveTo(mx, 0);
    c.lineTo(mx, HUE_H);
    c.strokeStyle = '#FFFFFF';
    c.lineWidth = 2;
    c.stroke();
    c.beginPath();
    c.moveTo(mx - 1.5, 0);
    c.lineTo(mx - 1.5, HUE_H);
    c.strokeStyle = '#000000';
    c.lineWidth = 1;
    c.stroke();
  }

  build() {
    Column({ space: 10 }) {
      Text(this.title)
        .fontSize(16)
        .fontWeight(FontWeight.Bold)
        .fontColor($r('app.color.text_primary'))
        .width('100%')

      // SV 色域盘
      Canvas(this.svCtx)
        .width(SV_W)
        .height(SV_H)
        .borderRadius(6)
        .onReady((): void => {
          this.svReady = true;
          this.drawAll();
        })
        .onTouch((e: TouchEvent): void => {
          if (e.type !== TouchType.Down && e.type !== TouchType.Move) {
            return;
          }
          const t: TouchObject = e.changedTouches.length > 0 ? e.changedTouches[0] : e.touches[0];
          this.s = Math.min(1, Math.max(0, t.x / SV_W));
          this.v = Math.min(1, Math.max(0, 1 - t.y / SV_H));
          this.hexBox = this.curHex();
          this.drawAll();
        })

      // 色相条
      Canvas(this.hueCtx)
        .width(SV_W)
        .height(HUE_H)
        .borderRadius(4)
        .onReady((): void => {
          this.hueReady = true;
          this.drawAll();
        })
        .onTouch((e: TouchEvent): void => {
          if (e.type !== TouchType.Down && e.type !== TouchType.Move) {
            return;
          }
          const t: TouchObject = e.changedTouches.length > 0 ? e.changedTouches[0] : e.touches[0];
          this.h = Math.min(359.9, Math.max(0, t.x / SV_W * 360));
          this.hexBox = this.curHex();
          this.drawAll();
        })

      // 预览 + hex 输入 + RGB 读数
      Row({ space: 10 }) {
        Column()
          .width(40)
          .height(40)
          .borderRadius(6)
          .backgroundColor(this.curHex())
          .border({ width: 1, color: $r('app.color.border') })
        TextInput({ text: this.hexBox })
          .fontSize(12)
          .width(90)
          .height(34)
          .onChange((v: string): void => {
            this.hexBox = v;
          })
          .onSubmit((): void => {
            this.setFromHex(this.hexBox);
          })
        Text(`R${this.curRgb()[0]} G${this.curRgb()[1]} B${this.curRgb()[2]}`)
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
      }
      .width('100%')

      // 色板库快选（P1 库扁平；空 = 未导入）
      if (this.libSwatches.length > 0) {
        Flex({ wrap: FlexWrap.Wrap }) {
          ForEach(this.libSwatches, (c: string) => {
            Column()
              .width(18)
              .height(18)
              .borderRadius(9)
              .backgroundColor(c)
              .border({ width: 1, color: $r('app.color.border') })
              .margin(2)
              .onClick((): void => this.setFromHex(c))
          }, (c: string): string => c)
        }
        .width('100%')
      }

      Row({ space: 10 }) {
        Blank()
        Button('取消')
          .fontSize(12).height(32)
          .backgroundColor($r('app.color.surface'))
          .fontColor($r('app.color.text_primary'))
          .onClick((): void => this.onCancel())
        Button('确定')
          .fontSize(12).height(32)
          .backgroundColor($r('app.color.accent'))
          .fontColor($r('app.color.on_accent'))
          .onClick((): void => this.onConfirm(this.hexBox.length === 7 ? this.hexBox : this.curHex()))
      }
      .width('100%')
    }
    .padding(16)
    .borderRadius(14)
    .backgroundColor($r('app.color.surface'))
    .border({ width: 1, color: $r('app.color.border') })
    .width(280)
  }
}
