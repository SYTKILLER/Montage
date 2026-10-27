/**
 * H8 i18n 框架（D6-b 决策：框架先行，语言包按需）。
 * photocraft complete_menus 做法（MIT，2026-10-27 快照对拍）：命令面字符串全走键表 +
 * 强制覆盖测试——每个已注册语言包必须覆盖命令注册表全键，缺键即失败。
 * zh = 基线语言：键值内联在命令注册表的 zh 字段，无需包；其他语言注册包后可切换。
 * t(key, fallback) 三级回退：当前语言包 → fallback（=注册表 zh 值）→ 缺键登记供覆盖测试。
 * 范围登记：v1 覆盖命令面（菜单/注册表元数据）；对话框/toast 等散落 UI 串维持中文，
 * 语言包按需扩（07 §H8「语言包按需」原文语义）。
 */

/** 语言包：键 → 译文。键命名空间：cmd.<id> / group.<gid> / sub.<id>。 */
export type LangPack = Record<string, string>;

let currentLang = 'zh';
const packs: Map<string, LangPack> = new Map();
/** 缺键登记（lang → key 集合）：i18n.check / 覆盖测试数据源。 */
const missingKeys: Map<string, Set<string>> = new Map();

/** 注册语言包（重复注册覆盖；zh 为基线不注册包）。 */
export function registerPack(lang: string, pack: LangPack): void {
  packs.set(lang, pack);
  if (!missingKeys.has(lang)) {
    missingKeys.set(lang, new Set<string>());
  }
}

/** 切换语言（未注册语言拒绝并回退 zh——回退即基线）。 */
export function setLang(lang: string): void {
  currentLang = lang === 'zh' || packs.has(lang) ? lang : 'zh';
}

export function getLang(): string {
  return currentLang;
}

/** 已注册语言清单（zh 恒在首）。 */
export function registeredLangs(): string[] {
  const out: string[] = ['zh'];
  packs.forEach((_v: LangPack, k: string): void => {
    if (k !== 'zh') {
      out.push(k);
    }
  });
  return out;
}

/**
 * 取译文：当前语言包命中 → 包值；否则 fallback（注册表 zh 值）。
 * 非基线语言缺键登记进 missingKeys（覆盖测试断言其恒空）。
 */
export function t(key: string, fallback: string): string {
  if (currentLang === 'zh') {
    return fallback;
  }
  const pack = packs.get(currentLang);
  if (pack === undefined) {
    return fallback;
  }
  const v: Object = pack[key];
  if (typeof v === 'string' && (v as string).length > 0) {
    return v as string;
  }
  let set = missingKeys.get(currentLang);
  if (set === undefined) {
    set = new Set<string>();
    missingKeys.set(currentLang, set);
  }
  set.add(key);
  return fallback;
}

/** 当前语言（或指定语言）的缺失键清单（强制覆盖测试用；恒空 = 覆盖完整）。 */
export function missingFor(lang: string): string[] {
  const set = missingKeys.get(lang);
  const out: string[] = [];
  if (set !== undefined) {
    set.forEach((k: string): void => {
      out.push(k);
    });
  }
  return out;
}

/** 主动覆盖检查：keys 对指定包逐一核验，返回缺失清单（不命中不登记）。
 *  zh = 基线语言（键值即注册表 zh 字段），覆盖由构造保证，恒返回空。 */
export function coverage(lang: string, keys: string[]): string[] {
  if (lang === 'zh') {
    return [];
  }
  const pack = packs.get(lang);
  if (pack === undefined) {
    return [`<pack ${lang} not registered>`];
  }
  const out: string[] = [];
  for (const k of keys) {
    const v: Object = pack[k];
    if (typeof v !== 'string' || (v as string).length === 0) {
      out.push(k);
    }
  }
  return out;
}
