#ifndef MONTAGE_RENDER_SHADERS_H
#define MONTAGE_RENDER_SHADERS_H

// 着色器清单（03 §9）：M1 用 quad.vert / checker.frag / tile.frag。
// 视口变换（R3.4）：交互 vp 基准在 ArkTS，GL 内 px 基准；screen = (doc - pan) * zoom，Y 向下。

namespace montage {
namespace shaders {

inline const char* kQuadVert = R"(#version 300 es
layout(location=0) in vec2 aPos;              // 单位 quad 0..1
uniform vec4 uRect;                           // 文档空间 x,y,w,h
uniform vec4 uView;                           // zoom, panX, panY, -
uniform vec2 uViewport;                       // 视口 px
out vec2 vDoc;
void main() {
  vec2 doc = uRect.xy + aPos * uRect.zw;
  vec2 screen = (doc - uView.yz) * uView.x;
  vec2 clip = vec2(screen.x / uViewport.x * 2.0 - 1.0, 1.0 - screen.y / uViewport.y * 2.0);
  gl_Position = vec4(clip, 0.0, 1.0);
  vDoc = doc;
}
)";

// 程序化棋盘格透明底（03 §6：不生成纹理；gl_FragCoord 屏幕空间 8px 格）
inline const char* kCheckerFrag = R"(#version 300 es
precision mediump float;
out vec4 o;
void main() {
  float ch = mod(floor(gl_FragCoord.x / 8.0) + floor(gl_FragCoord.y / 8.0), 2.0);
  float g = mix(0.78, 0.92, ch);
  o = vec4(g, g, g, 1.0);
}
)";

// 瓦片采样（M1 无蒙版；半纹素内缩防 LINEAR 跨瓦片接缝；≥200% 由 CPU 侧切 NEAREST，03 §6）
inline const char* kTileFrag = R"(#version 300 es
precision mediump float;
uniform sampler2D uTex;
in vec2 vDoc;
uniform vec4 uRect;
out vec4 o;
void main() {
  vec2 uv = (vDoc - uRect.xy) / uRect.zw;
  const float texel = 1.0 / 256.0;
  uv = clamp(uv, texel * 0.5, 1.0 - texel * 0.5);
  o = texture(uTex, uv);
}
)";

}  // namespace shaders
}  // namespace montage

#endif  // MONTAGE_RENDER_SHADERS_H
