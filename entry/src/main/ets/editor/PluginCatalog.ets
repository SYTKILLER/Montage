/**
 * M26 插件目录（P8，docs/40-方案设计/14）：参数 schema 解析（PluginDialog 视图模型）
 * + 内置插件 rawfile 清单。加载/注册在 Index（需 context + engine）。
 */

/** manifest 参数 schema 串（bridge pluginParamsJson 产物）的展开项。 */
export interface PluginParamVM {
  key: string;
  type: string;        // 'number' | 'int' | 'bool' | 'choice'
  min: number;
  max: number;
  def: number;         // number/int 默认
  boolDef: boolean;
  options: string[];   // choice 选项
  choiceDef: number;   // choice 默认索引（-1 = 取首项）
}

/** 内置插件清单（rawfile/plugins/；新增内置插件 = 加文件 + 补清单）。 */
export const BUILTIN_PLUGIN_FILES: string[] = [
  'plugins/invert.js',
  'plugins/duotone.js',
  'plugins/softblur.js',
];

/**
 * 解析参数 schema 串 → 对话框视图模型。手写扫描（同态：bridge 产出形态固定
 * {"k":{"type":"number","min":..,"max":..,"default":..},...}；畸形键跳过）。
 */
export function parsePluginParams(paramsJson: string): PluginParamVM[] {
  const out: PluginParamVM[] = [];
  if (paramsJson.length === 0) {
    return out;
  }
  // 顶层键扫描：找 "key":{...} 对（深度 1），值域大括号配对
  let i = 0;
  const n = paramsJson.length;
  while (i < n) {
    if (paramsJson[i] !== '"') {
      i++;
      continue;
    }
    const keyStart = i + 1;
    const keyEnd = paramsJson.indexOf('"', keyStart);
    if (keyEnd < 0) {
      break;
    }
    let j = keyEnd + 1;
    while (j < n && (paramsJson[j] === ' ' || paramsJson[j] === ':')) {
      j++;
    }
    if (j >= n || paramsJson[j] !== '{') {
      i = keyEnd + 1;
      continue;
    }
    // 值对象配对（字符串内引号跳过）
    let depth = 0;
    let k = j;
    let inStr = false;
    for (; k < n; k++) {
      const ch = paramsJson[k];
      if (inStr) {
        if (ch === '\\') {
          k++;
          continue;
        }
        if (ch === '"') {
          inStr = false;
        }
        continue;
      }
      if (ch === '"') {
        inStr = true;
      } else if (ch === '{') {
        depth++;
      } else if (ch === '}') {
        depth--;
        if (depth === 0) {
          break;
        }
      }
    }
    if (k >= n) {
      break;
    }
    const body = paramsJson.substring(j, k + 1);
    const vm = parseParamBody(paramsJson.substring(keyStart, keyEnd), body);
    if (vm !== null) {
      out.push(vm);
    }
    i = k + 1;
  }
  return out;
}

function strField(body: string, key: string): string {
  const pat = `"${key}"`;
  const at = body.indexOf(pat);
  if (at < 0) {
    return '';
  }
  const colon = body.indexOf(':', at + pat.length);
  if (colon < 0) {
    return '';
  }
  const q1 = body.indexOf('"', colon + 1);
  if (q1 < 0) {
    return '';
  }
  const q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) {
    return '';
  }
  return body.substring(q1 + 1, q2);
}

function numField(body: string, key: string, defVal: number): number {
  const pat = `"${key}"`;
  const at = body.indexOf(pat);
  if (at < 0) {
    return defVal;
  }
  const colon = body.indexOf(':', at + pat.length);
  if (colon < 0) {
    return defVal;
  }
  let j = colon + 1;
  let start = -1;
  while (j < body.length) {
    const ch = body[j];
    if (start < 0 && (ch === '-' || ch === '.' || (ch >= '0' && ch <= '9'))) {
      start = j;
    } else if (start >= 0 && !(ch === '.' || ch === 'e' || ch === 'E' || ch === '+' ||
               ch === '-' || (ch >= '0' && ch <= '9'))) {
      break;
    }
    j++;
  }
  if (start < 0) {
    return defVal;
  }
  const v = parseFloat(body.substring(start, j));
  return isNaN(v) ? defVal : v;
}

function boolField(body: string, key: string): boolean {
  const at = body.indexOf(`"${key}"`);
  if (at < 0) {
    return false;
  }
  return body.indexOf('true', at) >= 0 && body.indexOf('true', at) < body.indexOf('}', at);
}

function parseParamBody(key: string, body: string): PluginParamVM | null {
  const type = strField(body, 'type');
  if (type.length === 0) {
    return null;
  }
  const vm: PluginParamVM = {
    key: key,
    type: type,
    min: numField(body, 'min', 0),
    max: numField(body, 'max', 100),
    def: numField(body, 'default', 0),
    boolDef: boolField(body, 'default'),
    options: [],
    choiceDef: -1,
  };
  if (type === 'choice') {
    // "options":["a","b",...]
    const at = body.indexOf('"options"');
    if (at >= 0) {
      const lb = body.indexOf('[', at);
      const rb = body.indexOf(']', lb);
      if (lb >= 0 && rb > lb) {
        const arrBody = body.substring(lb + 1, rb);
        let q = arrBody.indexOf('"');
        while (q >= 0) {
          const qe = arrBody.indexOf('"', q + 1);
          if (qe < 0) {
            break;
          }
          vm.options.push(arrBody.substring(q + 1, qe));
          q = arrBody.indexOf('"', qe + 1);
        }
      }
    }
    const defStr = strField(body, 'default');
    if (defStr.length > 0) {
      vm.choiceDef = vm.options.indexOf(defStr);
    }
    if (vm.options.length === 0) {
      vm.options.push('-');
    }
  }
  return vm;
}
