import { preferences } from '@kit.ArkData';
import { common, ConfigurationConstant } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

const TAG: string = 'Montage.Prefs';
const PREF_NAME: string = 'montage_prefs';
const KEY_THEME: string = 'theme_mode';
const KEY_RECENTS: string = 'recent_projects';
const MAX_RECENTS: number = 10;

/** 主题三态（06 D5.1：深色/浅色/跟随系统）。 */
export type ThemeMode = 'system' | 'light' | 'dark';

/** 最近工程条目（欢迎态列表 + 打开/首存时登记）。 */
export interface RecentProject {
  name: string;
  uri: string;
  time: number;
}

/**
 * 应用级偏好（Preferences 同步 API）：主题模式 + 最近工程。
 * 存储失败一律降级为内存默认值，不阻塞启动。
 */
export class AppPrefs {
  private store: preferences.Preferences | null = null;
  private recentsFallback: RecentProject[] = [];

  init(context: common.Context): void {
    try {
      this.store = preferences.getPreferencesSync(context, { name: PREF_NAME });
    } catch (e) {
      hilog.error(0x4D30, TAG, 'prefs init failed');
      this.store = null;
    }
  }

  getThemeMode(): ThemeMode {
    if (this.store === null) {
      return 'system';
    }
    try {
      const v = this.store.getSync(KEY_THEME, 'system') as string;
      return v === 'light' || v === 'dark' ? v : 'system';
    } catch (e) {
      return 'system';
    }
  }

  setThemeMode(mode: ThemeMode): void {
    if (this.store === null) {
      return;
    }
    try {
      this.store.putSync(KEY_THEME, mode);
      this.store.flush();
    } catch (e) {
      hilog.error(0x4D30, TAG, 'setThemeMode failed');
    }
  }

  /** 应用主题到系统（ConfigurationConstant.ColorMode；与 resourceManager.ColorMode 不兼容，勿混用）。 */
  applyTheme(mode: ThemeMode, context: common.Context): void {
    try {
      const cm = mode === 'dark' ? ConfigurationConstant.ColorMode.COLOR_MODE_DARK
        : (mode === 'light' ? ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT
          : ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
      context.getApplicationContext().setColorMode(cm);
      hilog.info(0x4D30, TAG, 'theme → %{public}s', mode);
    } catch (e) {
      hilog.error(0x4D30, TAG, 'applyTheme failed');
    }
  }

  getRecents(): RecentProject[] {
    if (this.store === null) {
      return this.recentsFallback;
    }
    try {
      const raw = this.store.getSync(KEY_RECENTS, '[]') as string;
      const arr = JSON.parse(raw) as object[];
      const out: RecentProject[] = [];
      for (const o of arr) {
        const r = o as Record<string, Object>;
        const name = r['name'];
        const uri = r['uri'];
        const time = r['time'];
        if (typeof name === 'string' && typeof uri === 'string' && typeof time === 'number') {
          out.push({ name: name, uri: uri, time: time });
        }
      }
      return out;
    } catch (e) {
      hilog.error(0x4D30, TAG, 'recents parse failed');
      return [];
    }
  }

  /** 登记/置顶一条最近工程（去重、上限 10），返回新列表。 */
  addRecent(name: string, uri: string): RecentProject[] {
    let list: RecentProject[] = this.getRecents().filter((r: RecentProject): boolean => r.uri !== uri);
    list.unshift({ name: name, uri: uri, time: Date.now() });
    if (list.length > MAX_RECENTS) {
      list.splice(MAX_RECENTS);
    }
    if (this.store !== null) {
      try {
        this.store.putSync(KEY_RECENTS, JSON.stringify(list));
        this.store.flush();
      } catch (e) {
        hilog.error(0x4D30, TAG, 'addRecent persist failed');
      }
    }
    this.recentsFallback = list;
    return list;
  }

  /** 打开失败时移除失效项（文件被移动/删除）。 */
  removeRecent(uri: string): RecentProject[] {
    const list: RecentProject[] = this.getRecents().filter((r: RecentProject): boolean => r.uri !== uri);
    if (this.store !== null) {
      try {
        this.store.putSync(KEY_RECENTS, JSON.stringify(list));
        this.store.flush();
      } catch (e) {
        // 忽略：下次打开失败仍会重试移除
      }
    }
    this.recentsFallback = list;
    return list;
  }
}
