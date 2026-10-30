// M26 内置示例插件：双色调（参数型：number/int/bool/choice 四型全覆盖）。
function montage_manifest() {
  return {
    id: 'com.montage.duotone',
    name: '双色调（插件）',
    version: '1.0.0',
    description: '按亮度量化映射到色调色（参数型示例）',
    author: 'Montage',
    kind: 'filter',
    area: 'content',
    overlap: 0,
    params: {
      amount: { type: 'number', min: 0, max: 100, default: 80 },
      steps: { type: 'int', min: 2, max: 16, default: 8 },
      reverse: { type: 'bool', default: false },
      tint: { type: 'choice', options: ['red', 'green', 'blue'], default: 'red' },
    },
  };
}

function montage_apply(api) {
  const d = new Float32Array(api.pixels);
  const p = api.params;
  const mix = p.amount / 100.0;
  const steps = p.steps;
  let tint = [0.9, 0.15, 0.15];
  if (p.tint === 'green') {
    tint = [0.1, 0.85, 0.2];
  } else if (p.tint === 'blue') {
    tint = [0.15, 0.25, 0.9];
  }
  const w = api.image.width;
  const h = api.image.height;
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      let lum = 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];
      if (p.reverse) {
        lum = 1.0 - lum;
      }
      lum = Math.round(lum * (steps - 1)) / (steps - 1);
      d[i] = d[i] + (tint[0] * lum - d[i]) * mix;
      d[i + 1] = d[i + 1] + (tint[1] * lum - d[i + 1]) * mix;
      d[i + 2] = d[i + 2] + (tint[2] * lum - d[i + 2]) * mix;
    }
    if ((y & 63) === 0) {
      api.tick();
    }
  }
}
