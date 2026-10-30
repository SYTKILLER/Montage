// M26 内置示例插件：反相（点滤镜，无参数）。
// ABI 见 docs/40-方案设计/14：montage_manifest() + montage_apply(api)；
// api.pixels = ArrayBuffer（w×h×4 float32 直 alpha 归一化 0..1），api.image 域信息。
function montage_manifest() {
  return {
    id: 'com.montage.invert',
    name: '反相（插件）',
    version: '1.0.0',
    description: 'RGB 通道反转（点滤镜最小示例）',
    author: 'Montage',
    kind: 'filter',
    area: 'content',
    overlap: 0,
    params: {},
  };
}

function montage_apply(api) {
  const d = new Float32Array(api.pixels);
  const n = api.image.width * api.image.height * 4;
  for (let i = 0; i < n; i += 4) {
    d[i] = 1.0 - d[i];
    d[i + 1] = 1.0 - d[i + 1];
    d[i + 2] = 1.0 - d[i + 2];
    if ((i & 0xffff) === 0) {
      api.tick();
    }
  }
}
