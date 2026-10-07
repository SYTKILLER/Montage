import { ThemeMode } from '../AppPrefs';

/** 关于信息（help.about 填充）。 */
export const ABOUT_TEXT: string =
  'Montage（蒙太奇）— 鸿蒙平台 Photoshop 级开源图像编辑器\n' +
  'PSD 中心 · ArkTS UI + C++ 引擎（GLES 合成 / 稀疏瓦片 / 24 混合模式）\n' +
  'M7.5 编辑器外壳 · 自有格式 .montage v3';

/** 快捷键表（help.shortcuts；与命令表同源语义，05 §3 v1 键位）。 */
export const SHORTCUT_ROWS: ShortcutRow[] = [
  { keys: 'V / M / L / W', desc: '移动 / 选框 / 套索 / 魔棒' },
  { keys: 'C / I', desc: '裁剪 / 吸管' },
  { keys: 'B / E', desc: '画笔 / 橡皮' },
  { keys: 'H / Z', desc: '抓手 / 缩放' },
  { keys: 'Ctrl+Z / Ctrl+Shift+Z', desc: '撤销 / 重做' },
  { keys: 'Ctrl+N / O / S', desc: '新建 / 打开 / 保存' },
  { keys: 'Ctrl+0 / Ctrl+1', desc: '适配画布 / 100%' },
  { keys: 'Ctrl+= / Ctrl+-', desc: '放大 / 缩小' },
  { keys: 'Ctrl+D', desc: '取消选区' },
  { keys: '[ / ]', desc: '笔刷缩小 / 放大' },
  { keys: '空格按住', desc: '临时抓手' },
];

export interface ShortcutRow {
  keys: string;
  desc: string;
}

export interface ThemeOption {
  mode: ThemeMode;
  label: string;
}

export const THEME_OPTIONS: ThemeOption[] = [
  { mode: 'system', label: '跟随系统' },
  { mode: 'light', label: '浅色' },
  { mode: 'dark', label: '深色' },
];

/**
 * 信息对话框（关于 / 快捷键表 / 主题选择，模态遮罩范式）。
 */
@Component
export struct InfoDialog {
  @Prop title: string = '';
  @State rows: ShortcutRow[] = [];
  @State showThemes: boolean = false;
  @Prop currentTheme: string = 'system';
  onPickTheme: (mode: ThemeMode) => void = (_m: ThemeMode) => {};
  onClose: () => void = () => {};

  build() {
    Stack() {
      Column()
        .width('100%')
        .height('100%')
        .backgroundColor('#66000000')
        .onClick((): void => this.onClose())
      Column({ space: 14 }) {
        Text(this.title)
          .fontSize(16)
          .fontWeight(FontWeight.Bold)
          .fontColor($r('app.color.text_primary'))
        if (this.rows.length > 0) {
          List({ space: 6 }) {
            ForEach(this.rows, (r: ShortcutRow) => {
              ListItem() {
                Row({ space: 16 }) {
                  Text(r.keys)
                    .fontSize(12)
                    .fontColor($r('app.color.text_primary'))
                    .width(190)
                  Text(r.desc)
                    .fontSize(12)
                    .fontColor($r('app.color.text_secondary'))
                  Blank()
                }
                .width('100%')
              }
            }, (r: ShortcutRow): string => r.keys)
          }
          .height(280)
          .scrollBar(BarState.Auto)
        }
        if (this.showThemes) {
          Column({ space: 8 }) {
            ForEach(THEME_OPTIONS, (t: ThemeOption) => {
              Row({ space: 10 }) {
                Radio({ value: t.mode, group: 'themePick' })
                  .checked(this.currentTheme === t.mode)
                  .onChange((on: boolean): void => {
                    if (on) {
                      this.onPickTheme(t.mode);
                    }
                  })
                Text(t.label).fontSize(13).fontColor($r('app.color.text_primary'))
              }
              .width('100%')
              .padding(6)
            }, (t: ThemeOption): string => t.mode)
          }
          .width('100%')
        }
        Button('关闭')
          .fontSize(14)
          .height(38)
          .width(96)
          .backgroundColor($r('app.color.accent'))
          .fontColor($r('app.color.on_accent'))
          .onClick((): void => this.onClose())
      }
      .padding(20)
      .borderRadius(14)
      .backgroundColor($r('app.color.surface'))
      .border({ width: 1, color: $r('app.color.border') })
      .width(460)
    }
    .width('100%')
    .height('100%')
  }
}
