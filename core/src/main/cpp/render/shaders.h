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

// 终合成：acc 纹理（直通 alpha）画到屏幕棋盘格上，按 doc rect 裁剪
inline const char* kPlainFrag = R"(#version 300 es
precision mediump float;
uniform sampler2D uTex;
uniform vec2 uViewport;
out vec4 o;
void main() {
  o = texture(uTex, gl_FragCoord.xy / uViewport);
}
)";

// ---------- Pass B 混合（03 §4/§5）----------
// 公式对拍：源 SeparableBlend.swift 委托 Core Image 标准混合滤镜 = W3C Compositing 1.0
// 可分离混合公式，sRGB 非线性空间直接计算（与本管线存储一致，不做线性化）。
//   Cr = (1-αb)·Cs + αb·B(Cb,Cs)          （混合步）
//   αo = αs + αb·(1-αs)                    （source-over）
//   Co_premult = αs·Cr + (1-αs)·Cb·αb     （直通 alpha 存储，输出前除回）
// 4 模式 = 独立 program（R3.3），frag 由 kBlendCommon + kBlendB[mode] 拼装。
inline const char* kBlendCommon = R"(#version 300 es
precision mediump float;
uniform sampler2D uSrc;    // 图层纹理（straight alpha）
uniform sampler2D uDst;    // acc 纹理（straight alpha）
uniform float uOpacity;    // 图层不透明度
uniform vec2 uViewport;
out vec4 o;
vec3 blendB(vec3 Cb, vec3 Cs) {   // %%BLEND_B%% 各模式替换
  return Cs;
}
void main() {
  vec4 src = texture(uSrc, gl_FragCoord.xy / uViewport);
  vec4 dst = texture(uDst, gl_FragCoord.xy / uViewport);
  float as = src.a * uOpacity;
  float ab = dst.a;
  vec3 Cs = src.rgb;
  vec3 Cb = dst.rgb;
  vec3 B = blendB(Cb, Cs);
  vec3 Cr = (1.0 - ab) * Cs + ab * B;
  float ao = as + ab * (1.0 - as);
  vec3 coP = as * Cr + (1.0 - as) * Cb * ab;
  o = ao > 0.0001 ? vec4(coP / ao, ao) : vec4(0.0);
}
)";

inline const char* kBlendNormalBody = R"(
  return Cs;
)";
inline const char* kBlendMultiplyBody = R"(
  return Cb * Cs;
)";
inline const char* kBlendScreenBody = R"(
  return Cb + Cs - Cb * Cs;
)";
inline const char* kBlendOverlayBody = R"(
  // overlay = hard-light 交换参数：Cb<=0.5 → 2·Cb·Cs，否则 1-2·(1-Cb)·(1-Cs)
  vec3 lo = 2.0 * Cb * Cs;
  vec3 hi = 1.0 - 2.0 * (1.0 - Cb) * (1.0 - Cs);
  return mix(lo, hi, step(vec3(0.5), Cb));
)";

}  // namespace shaders
}  // namespace montage

#endif  // MONTAGE_RENDER_SHADERS_H
