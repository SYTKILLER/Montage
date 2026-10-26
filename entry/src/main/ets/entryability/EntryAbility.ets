import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';

const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'Failed to set colorMode. Cause: %{public}s', JSON.stringify(err));
    }
    // M23a：调试直通打开（模拟器嵌套菜单注入被吞的替代 E2E 通道）：
    // aa start --ps debugOpenB64 <文件字节 base64> → Index aboutToAppear 消费
    // M23c：大素材分段通道 —— debugB64Part 追加进累积串，debugB64Flush=1 时
    // 拼出完整 debugOpenB64 并清累积（Windows 命令行 32KB 上限的分段绕行）。
    try {
      const dbg = want.parameters?.debugOpenB64;
      if (typeof dbg === 'string' && dbg.length > 0) {
        AppStorage.setOrCreate('debugOpenB64', dbg);
      }
      const part = want.parameters?.debugB64Part;
      if (typeof part === 'string' && part.length > 0) {
        const prev = AppStorage.get<string>('debugB64Accum') ?? '';
        AppStorage.setOrCreate('debugB64Accum', prev + part);
      }
      const flush = want.parameters?.debugB64Flush;
      if (typeof flush === 'string' && flush === '1') {
        const acc = AppStorage.get<string>('debugB64Accum') ?? '';
        if (acc.length > 0) {
          AppStorage.setOrCreate('debugOpenB64', acc);
          AppStorage.setOrCreate('debugB64Accum', '');
        }
      }
      // P1：预设资源导入通道（debugPreKind 0=aco/ase 1=grd 2=pat 3=abr + debugPreName +
      // debugPrePart/debugPreFlush 分段；flush 拼 "kind|name|b64" → Index 消费入四库）
      const preKind = want.parameters?.debugPreKind;
      if (typeof preKind === 'string' && preKind.length > 0) {
        AppStorage.setOrCreate('debugPreKind', preKind);
      }
      const preName = want.parameters?.debugPreName;
      if (typeof preName === 'string' && preName.length > 0) {
        AppStorage.setOrCreate('debugPreName', preName);
      }
      const prePart = want.parameters?.debugPrePart;
      if (typeof prePart === 'string' && prePart.length > 0) {
        const prev2 = AppStorage.get<string>('debugPreAccum') ?? '';
        AppStorage.setOrCreate('debugPreAccum', prev2 + prePart);
      }
      const preFlush = want.parameters?.debugPreFlush;
      if (typeof preFlush === 'string' && preFlush === '1') {
        const acc2 = AppStorage.get<string>('debugPreAccum') ?? '';
        if (acc2.length > 0) {
          const kind2 = AppStorage.get<string>('debugPreKind') ?? '0';
          const name2 = AppStorage.get<string>('debugPreName') ?? 'sample.aco';
          AppStorage.setOrCreate('debugPresetB64', `${kind2}|${name2}|${acc2}`);
          AppStorage.setOrCreate('debugPreAccum', '');
        }
      }
      // M22：调试命令直通（runCommand；uitest 注入退化时的 E2E 旁路）
      const dbgCmd = want.parameters?.debugCmd;
      if (typeof dbgCmd === 'string' && dbgCmd.length > 0) {
        AppStorage.setOrCreate('debugCmd', dbgCmd);
      }
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'debugOpenB64 param: %{public}s', JSON.stringify(err));
    }
    // M24c：控制通道门控 —— aa start --ps debugControl 1 [--ps debugControlPort 18787]
    // （debug-only TCP 测试通道；Index aboutToAppear 消费并启动 ControlServer）
    try {
      const ctl = want.parameters?.debugControl;
      if (typeof ctl === 'string' && ctl === '1') {
        AppStorage.setOrCreate('debugControl', '1');
      }
      const ctlPort = want.parameters?.debugControlPort;
      if (typeof ctlPort === 'string' && ctlPort.length > 0) {
        AppStorage.setOrCreate('debugControlPort', ctlPort);
      }
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'debugControl param: %{public}s', JSON.stringify(err));
    }
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onCreate');
  }

  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    try {
      const dbg = want.parameters?.debugOpenB64;
      if (typeof dbg === 'string' && dbg.length > 0) {
        AppStorage.setOrCreate('debugOpenB64', dbg);
      }
      const part = want.parameters?.debugB64Part;
      if (typeof part === 'string' && part.length > 0) {
        const prev = AppStorage.get<string>('debugB64Accum') ?? '';
        AppStorage.setOrCreate('debugB64Accum', prev + part);
      }
      const flush = want.parameters?.debugB64Flush;
      if (typeof flush === 'string' && flush === '1') {
        const acc = AppStorage.get<string>('debugB64Accum') ?? '';
        if (acc.length > 0) {
          AppStorage.setOrCreate('debugOpenB64', acc);
          AppStorage.setOrCreate('debugB64Accum', '');
        }
      }
      const preKind = want.parameters?.debugPreKind;
      if (typeof preKind === 'string' && preKind.length > 0) {
        AppStorage.setOrCreate('debugPreKind', preKind);
      }
      const preName = want.parameters?.debugPreName;
      if (typeof preName === 'string' && preName.length > 0) {
        AppStorage.setOrCreate('debugPreName', preName);
      }
      const prePart = want.parameters?.debugPrePart;
      if (typeof prePart === 'string' && prePart.length > 0) {
        const prev2 = AppStorage.get<string>('debugPreAccum') ?? '';
        AppStorage.setOrCreate('debugPreAccum', prev2 + prePart);
      }
      const preFlush = want.parameters?.debugPreFlush;
      if (typeof preFlush === 'string' && preFlush === '1') {
        const acc2 = AppStorage.get<string>('debugPreAccum') ?? '';
        if (acc2.length > 0) {
          const kind2 = AppStorage.get<string>('debugPreKind') ?? '0';
          const name2 = AppStorage.get<string>('debugPreName') ?? 'sample.aco';
          AppStorage.setOrCreate('debugPresetB64', `${kind2}|${name2}|${acc2}`);
          AppStorage.setOrCreate('debugPreAccum', '');
        }
      }
      const dbgCmd = want.parameters?.debugCmd;
      if (typeof dbgCmd === 'string' && dbgCmd.length > 0) {
        AppStorage.setOrCreate('debugCmd', dbgCmd);
      }
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'debugOpenB64 param(new): %{public}s', JSON.stringify(err));
    }
    try {
      const ctl = want.parameters?.debugControl;
      if (typeof ctl === 'string' && ctl === '1') {
        AppStorage.setOrCreate('debugControl', '1');
      }
      const ctlPort = want.parameters?.debugControlPort;
      if (typeof ctlPort === 'string' && ctlPort.length > 0) {
        AppStorage.setOrCreate('debugControlPort', ctlPort);
      }
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'debugControl param(new): %{public}s', JSON.stringify(err));
    }
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // Main window is created, set main page for this ability
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageCreate');

    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'testTag', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, 'testTag', 'Succeeded in loading the content.');
    });
  }

  onWindowStageDestroy(): void {
    // Main window is destroyed, release UI related resources
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
    // Ability has brought to foreground
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onForeground');
  }

  onBackground(): void {
    // Ability has back to background
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onBackground');
  }
}