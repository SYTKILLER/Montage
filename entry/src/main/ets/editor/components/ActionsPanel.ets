/**
 * M24a 动作面板（photocraft Window › Actions 同位）：动作列表 + 步骤勾选/排序/删除 +
 * 录制/停止/播放。数据直读 automation 单例，rev 键驱动 ForEach 重建（@Prop 对象数组
 * 同步失效坑规避，M7.5 同范式）。
 */
import { automation, ActionStep } from 'core';

@Component
export struct ActionsPanel {
  @Prop @Watch('noop') rev: number = 0;          // Automation 变更代次（Index 维护）
  @Prop @Watch('noop') selected: number = -1;    // 选中动作索引
  @Prop @Watch('noop') docOpen: boolean = false;
  onSelect: (idx: number) => void = (_idx: number): void => {};
  onMessage: (msg: string) => void = (_msg: string): void => {};
  onMutated: () => void = (): void => {};

  noop(): void {
  }

  private apply(id: string, params: Record<string, Object>, okMsg: string): void {
    automation.execute(id, params).then((r): void => {
      if (!r.ok) {
        this.onMessage(r.error);
      } else {
        this.onMessage(okMsg);
        this.onMutated();
      }
    });
  }

  private stepRows(): ActionStep[] {
    const g = automation.getAction({ 'action': this.selected });
    if (!g.ok || g.value === undefined) {
      return [];
    }
    const v = g.value as Record<string, Object>;
    const steps = v['steps'] as Object as ActionStep[];
    return steps !== undefined && steps !== null ? steps : [];
  }

  private disabledRows(): number[] {
    const g = automation.getAction({ 'action': this.selected });
    if (!g.ok || g.value === undefined) {
      return [];
    }
    const v = g.value as Record<string, Object>;
    const dis = v['disabled'] as Object as number[];
    return dis !== undefined && dis !== null ? dis : [];
  }

  private actionRows(): Record<string, Object>[] {
    const r = automation.listActions();
    if (!r.ok || r.value === undefined) {
      return [];
    }
    const v = r.value as Record<string, Object>;
    const arr = v['actions'] as Object as Record<string, Object>[];
    return arr !== undefined && arr !== null ? arr : [];
  }

  private recordingRow(): string {
    const r = automation.listActions();
    if (r.ok && r.value !== undefined) {
      const v = r.value as Record<string, Object>;
      const rec = v['recording'];
      if (typeof rec === 'string') {
        return rec;
      }
    }
    return '';
  }

  build() {
    Column({ space: 4 }) {
      Row({ space: 6 }) {
        Text('动作').fontSize(12).fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary')).layoutWeight(1)
        if (this.recordingRow().length > 0) {
          Text(`● ${this.recordingRow()}`).fontSize(11).fontColor('#CC3333')
          Button('停止').fontSize(11).height(24).onClick((): void =>
            this.apply('actions.stop', {}, '录制已停止'))
        } else {
          Button('录制').fontSize(11).height(24)
            .enabled(this.docOpen && this.selected >= 0)
            .onClick((): void => this.apply('actions.record',
              { 'action': this.selected }, '开始录制'))
        }
        Button('播放').fontSize(11).height(24)
          .enabled(this.docOpen && this.selected >= 0)
          .onClick((): void => this.apply('actions.play', { 'action': this.selected }, '回放完成'))
        Button('+').fontSize(11).height(24)
          .onClick((): void => this.apply('actions.record', { 'name': '' }, '新建动作'))
      }
      .width('100%')

      List({ space: 2 }) {
        ForEach(this.actionRows(), (row: Record<string, Object>, idx: number) => {
          ListItem() {
            Row({ space: 6 }) {
              SymbolGlyph($r('sys.symbol.play_fill'))
                .fontSize(12)
                .fontColor([idx === this.selected ? '#0066CC' : '#8A8A8A'])
              Text(String(row['name']))
                .fontSize(12).maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
                .fontColor($r('app.color.text_primary')).layoutWeight(1)
              // P4：F 键绑定 chip（点击循环 0→F2..F12→0；跳过被占用键）
              Text(typeof row['fkey'] === 'number' && (row['fkey'] as number) > 0
                ? `F${row['fkey']}` : 'F·')
                .fontSize(10).fontColor('#8A8A8A')
                .borderRadius(4)
                .padding({ left: 4, right: 4, top: 1, bottom: 1 })
                .backgroundColor('#14006BD9')
                .onClick((): void => {
                  const r = automation.cycleFkey(idx);
                  if (r.ok) {
                    this.onMessage(r.value !== undefined
                      ? `F 键 → F${(r.value as Record<string, Object>)['key']}` : 'F 键已解绑');
                    this.onMutated();
                  } else {
                    this.onMessage(r.error);
                  }
                })
              Text(`${row['steps']}`).fontSize(11).fontColor('#8A8A8A')
            }
            .width('100%').padding({ left: 8, right: 6, top: 5, bottom: 5 })
            .borderRadius(6)
            .backgroundColor(idx === this.selected ? '#14006BD9' : '#00000000')
            .onClick((): void => this.onSelect(idx))
          }
        }, (row: Record<string, Object>, idx: number): string =>
          `a${idx}|${String(row['name'])}|${row['steps']}|${this.rev}`)
      }
      .width('100%')
      .height(110)
      .scrollBar(BarState.Auto)

      Divider().strokeWidth(1).color($r('app.color.divider'))

      List({ space: 2 }) {
        ForEach(this.stepRows(), (s: ActionStep, i: number) => {
          ListItem() {
            Row({ space: 4 }) {
              SymbolGlyph(!this.disabledRows().includes(i)
                ? $r('sys.symbol.checkmark_square_fill') : $r('sys.symbol.square'))
                .fontSize(13)
                .fontColor([!this.disabledRows().includes(i) ? '#0066CC' : '#9A9A9A'])
                .onClick((): void => this.apply('actions.toggle',
                  { 'action': this.selected, 'step': i }, '步骤已切换'))
              Text(automation.stepLabel(s))
                .fontSize(11).maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
                .fontColor(this.disabledRows().includes(i) ? '#9A9A9A' : $r('app.color.text_primary'))
                .layoutWeight(1)
              Text('↑').fontSize(12).fontColor('#666666').onClick((): void => {
                if (i > 0) {
                  this.apply('actions.move',
                    { 'action': this.selected, 'step': i, 'to': i - 1 }, '步骤已上移');
                }
              })
              Text('↓').fontSize(12).fontColor('#666666').onClick((): void => {
                if (i < this.stepRows().length - 1) {
                  this.apply('actions.move',
                    { 'action': this.selected, 'step': i, 'to': i + 1 }, '步骤已下移');
                }
              })
              SymbolGlyph($r('sys.symbol.trash'))
                .fontSize(12).fontColor(['#CC5555'])
                .onClick((): void => this.apply('actions.delete',
                  { 'action': this.selected, 'step': i }, '步骤已删除'))
            }
            .width('100%').padding({ left: 8, right: 6, top: 4, bottom: 4 })
            .borderRadius(6)
          }
        }, (s: ActionStep, i: number): string =>
          `s${i}|${s.id}|${this.stepRows().length}|${this.disabledRows().includes(i)}|${this.rev}`)
      }
      .width('100%')
      .layoutWeight(1)
      .scrollBar(BarState.Auto)

      if (this.selected >= 0 && this.selected < this.actionRows().length) {
        Row({ space: 6 }) {
          Button('重命名…').fontSize(11).height(26).layoutWeight(1)
            .onClick((): void => this.onMessage('__rename_action__'))
          Button('删除动作').fontSize(11).height(26).layoutWeight(1)
            .onClick((): void => this.apply('actions.delete',
              { 'action': this.selected }, '动作已删除'))
        }
        .width('100%')
      }
    }
    .width('100%')
    .padding(4)
    .borderRadius(10)
    .backgroundColor($r('app.color.bar_background'))
  }
}
