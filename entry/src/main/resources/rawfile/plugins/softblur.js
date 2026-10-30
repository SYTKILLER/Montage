// M26 内置示例插件：柔化（overlap=3 的 3×3 盒模糊——band 边距缝合链路验证）。
function montage_manifest() {
  return {
    id: 'com.montage.softblur',
    name: '柔化（插件）',
    version: '1.0.0',
    description: '3x3 盒模糊（overlap 边距 band 缝合示例）',
    author: 'Montage',
    kind: 'filter',
    area: 'content',
    overlap: 3,
    params: {
      strength: { type: 'number', min: 0, max: 100, default: 100 },
    },
  };
}

function montage_apply(api) {
  const src = new Float32Array(api.pixels);
  const w = api.image.width;
  const h = api.image.height;
  const ov = api.image.overlap;
  const k = api.params.strength / 100.0;
  const x0 = ov;
  const y0 = ov;
  const x1 = w - ov;
  const y1 = h - ov;
  const bw = x1 - x0;
  const bh = y1 - y0;
  if (bw <= 0 || bh <= 0) {
    return;
  }
  const out = new Float32Array(bw * bh * 4);
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) {
      const di = ((y - y0) * bw + (x - x0)) * 4;
      for (let c = 0; c < 4; c++) {
        const sum = src[((y - 1) * w + (x - 1)) * 4 + c] + src[((y - 1) * w + x) * 4 + c] +
          src[((y - 1) * w + (x + 1)) * 4 + c] + src[(y * w + (x - 1)) * 4 + c] +
          src[(y * w + x) * 4 + c] + src[(y * w + (x + 1)) * 4 + c] +
          src[((y + 1) * w + (x - 1)) * 4 + c] + src[((y + 1) * w + x) * 4 + c] +
          src[((y + 1) * w + (x + 1)) * 4 + c];
        const b = sum / 9.0;
        const si = (y * w + x) * 4 + c;
        out[di + c] = src[si] + (b - src[si]) * k;
      }
    }
    if ((y & 63) === 0) {
      api.tick();
    }
  }
  for (let y = 0; y < bh; y++) {
    for (let x = 0; x < bw; x++) {
      const di = (y * bw + x) * 4;
      const si = ((y + y0) * w + (x + x0)) * 4;
      src[si] = out[di];
      src[si + 1] = out[di + 1];
      src[si + 2] = out[di + 2];
      src[si + 3] = out[di + 3];
    }
  }
}
