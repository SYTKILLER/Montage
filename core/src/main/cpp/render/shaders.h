#ifndef MONTAGE_RENDER_SHADERS_H
#define MONTAGE_RENDER_SHADERS_H

// 着色器清单（03 §9）：quad.vert / checker.frag / tile.frag / plain.frag / blend.frag 族。
// 视口变换（R3.4）：交互 vp 基准在 ArkTS，GL 内 px 基准；screen = (doc - pan) * zoom，Y 向下。
// 混合公式（M2.1 全集，对拍源 SeparableBlend→CI 标准语义 = W3C Compositing 可分离公式，
// sRGB 非线性空间直算）：Cr=(1-αb)Cs+αb·B(Cb,Cs)；αo=αs+αb(1-αs)；直通 alpha 存储回除。
// 24 模式 = 独立 program（R3.3），frag 由 kBlendCommon（含 %%BODY%% 占位声明）+ kBlend*Body 拼装。

namespace montage {
namespace shaders {

inline const char* kQuadVert = R"(#version 300 es
layout(location=0) in vec2 aPos;              // 单位 quad 0..1
uniform vec4 uRect;                           // 文档空间 x,y,w,h
uniform vec4 uView;                           // zoom, panX, panY, -
uniform vec2 uViewport;                       // 视口 px
uniform mat3 uXform;                          // M8.1：doc→doc 仿射（单位阵=直通）
out vec2 vDoc;
void main() {
  vec2 doc = uRect.xy + aPos * uRect.zw;
  vDoc = doc;  // 纹理采样坐标 = 原始 doc（M8.1：几何变换不改变瓦片采样）
  vec2 xdoc = (uXform * vec3(doc, 1.0)).xy;
  vec2 screen = (xdoc - uView.yz) * uView.x;
  vec2 clip = vec2(screen.x / uViewport.x * 2.0 - 1.0, 1.0 - screen.y / uViewport.y * 2.0);
  gl_Position = vec4(clip, 0.0, 1.0);
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

// 瓦片采样（半纹素内缩防 LINEAR 跨瓦片接缝；≥200% 由 CPU 侧切 NEAREST，03 §6）
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

// 剪贴蒙版（M5b-2）：剪贴层 assemble 纹理 × 基底层 alpha（两者均在视口空间，straight alpha）
inline const char* kClipFrag = R"(#version 300 es
precision mediump float;
uniform sampler2D uSrc;
uniform sampler2D uClip;
uniform vec2 uViewport;
out vec4 o;
void main() {
  vec2 uv = gl_FragCoord.xy / uViewport;
  vec4 s = texture(uSrc, uv);
  float ca = texture(uClip, uv).a;
  o = vec4(s.rgb, s.a * ca);
}
)";

// 调整层（M6a）：acc 全帧经 256 LUT 调色（CPU 建表，RGBA 三通道同表），opacity 与原值 lerp
inline const char* kAdjustFrag = R"(#version 300 es
precision mediump float;
uniform sampler2D uDst;
uniform sampler2D uLut;
uniform float uOpacity;
uniform vec2 uViewport;
out vec4 o;
void main() {
  vec2 uv = gl_FragCoord.xy / uViewport;
  vec4 d = texture(uDst, uv);
  vec3 adj = vec3(
    texture(uLut, vec2(clamp(d.r, 0.004, 0.996), 0.5)).r,
    texture(uLut, vec2(clamp(d.g, 0.004, 0.996), 0.5)).g,
    texture(uLut, vec2(clamp(d.b, 0.004, 0.996), 0.5)).b);
  o = vec4(mix(d.rgb, adj, uOpacity), d.a);
}
)";

// 蚂蚁线（M7b）：段带状 ribbon（CPU 展开三角形，屏幕恒宽），aT = 沿段距离，
// uPhase 随帧推进 → 黑白 dash 行进动画
inline const char* kAntsVert = R"(#version 300 es
layout(location=0) in vec2 aP1;               // 段起点（doc px）
layout(location=1) in vec2 aP2;               // 段终点
layout(location=2) in float aT;               // 0..1 沿段参数
layout(location=3) in float aSide;            // -1/+1 法向侧
uniform vec4 uView;                           // zoom, panX, panY, -
uniform vec2 uViewport;
out float vT;
void main() {
  vec2 dir = aP2 - aP1;
  float len = max(1e-6, length(dir));
  vec2 perp = vec2(-dir.y, dir.x) / len;
  vec2 pos = mix(aP1, aP2, aT) + perp * (aSide * 1.6 / uView.x);
  vec2 screen = (pos - uView.yz) * uView.x;
  vec2 clip = vec2(screen.x / uViewport.x * 2.0 - 1.0, 1.0 - screen.y / uViewport.y * 2.0);
  gl_Position = vec4(clip, 0.0, 1.0);
  vT = aT * len;
}
)";

inline const char* kAntsFrag = R"(#version 300 es
precision mediump float;
in float vT;
uniform float uPhase;
out vec4 o;
void main() {
  float dash = fract(vT / 16.0 + uPhase);
  vec3 c = dash < 0.5 ? vec3(0.05) : vec3(1.0);
  o = vec4(c, 1.0);
}
)";

// Pass B 混合公共模板：%%BODY%% 占位声明由 buildBlendProgram 按模式替换为完整函数。
inline const char* kBlendCommon = R"(#version 300 es
precision mediump float;
uniform sampler2D uSrc;    // 图层纹理（straight alpha）
uniform sampler2D uDst;    // acc 纹理（straight alpha）
uniform float uOpacity;    // 图层不透明度
uniform vec2 uViewport;
out vec4 o;
float lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }
vec3 clipColor(vec3 c) {
  float l = lum(c);
  float n = min(c.r, min(c.g, c.b));
  float x = max(c.r, max(c.g, c.b));
  if (x > 1.0) c = l + (c - l) * ((1.0 - l) / max(1e-6, x - l));
  if (n < 0.0) c = l + (c - l) * (l / max(1e-6, l - n));
  return c;
}
vec3 setLum(vec3 c, float l) { return clipColor(c + (l - lum(c))); }
float sat(vec3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }
vec3 setSat(vec3 c, float s) {
  float mn = min(c.r, min(c.g, c.b));
  float mx = max(c.r, max(c.g, c.b));
  float md = c.r + c.g + c.b - mn - mx;
  float nv = mx > mn ? (md - mn) * s / (mx - mn) : 0.0;
  if (c.r == mx) return vec3(s, nv, 0.0);
  if (c.g == mx) return vec3(0.0, s, nv);
  return vec3(nv, 0.0, s);
}
vec3 blendB(vec3 Cb, vec3 Cs);   // %%BODY%%
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

inline const char* kBlendNormalBody = R"(  return Cs;)";
inline const char* kBlendDarkenBody = R"(  return min(Cb, Cs);)";
inline const char* kBlendMultiplyBody = R"(  return Cb * Cs;)";
inline const char* kBlendColorBurnBody = R"(  return max(vec3(1.0) - min(vec3(1.0), (vec3(1.0) - Cb) / max(vec3(1e-5), Cs)), vec3(0.0));)";
inline const char* kBlendLinearBurnBody = R"(  return max(Cb + Cs - vec3(1.0), vec3(0.0));)";
inline const char* kBlendLightenBody = R"(  return max(Cb, Cs);)";
inline const char* kBlendScreenBody = R"(  return Cb + Cs - Cb * Cs;)";
inline const char* kBlendColorDodgeBody = R"(  return max(min(vec3(1.0), Cb / max(vec3(1e-5), vec3(1.0) - Cs)), vec3(0.0));)";
inline const char* kBlendLinearDodgeBody = R"(  return min(Cb + Cs, vec3(1.0));)";
inline const char* kBlendOverlayBody = R"(  return mix(2.0 * Cb * Cs, 1.0 - 2.0 * (1.0 - Cb) * (1.0 - Cs), step(vec3(0.5), Cb));)";
inline const char* kBlendSoftLightBody = R"(  return mix(Cb + (2.0 * Cs - 1.0) * (mix((16.0 * Cb - 12.0) * Cb + 4.0, sqrt(Cb), step(vec3(0.25), Cb)) - Cb), Cb - (1.0 - 2.0 * Cs) * Cb * (1.0 - Cb), step(vec3(0.5), Cs));)";
inline const char* kBlendHardLightBody = R"(  return mix(2.0 * Cs * Cb, 1.0 - 2.0 * (1.0 - Cs) * (1.0 - Cb), step(vec3(0.5), Cs));)";
inline const char* kBlendVividLightBody = R"(  return max(mix(vec3(1.0) - min(vec3(1.0), (vec3(1.0) - Cb) / max(vec3(1e-5), 2.0 * Cs)), min(vec3(1.0), Cb / max(vec3(1e-5), 2.0 * (vec3(1.0) - Cs))), step(vec3(0.5), Cs)), vec3(0.0));)";
inline const char* kBlendLinearLightBody = R"(  return clamp(Cb + 2.0 * Cs - vec3(1.0), vec3(0.0), vec3(1.0));)";
inline const char* kBlendPinLightBody = R"(  return mix(min(Cb, 2.0 * Cs), max(Cb, 2.0 * Cs - vec3(1.0)), step(vec3(0.5), Cs));)";
inline const char* kBlendHardMixBody = R"(  return step(vec3(0.5), mix(min(Cb, 2.0 * Cs), max(Cb, 2.0 * Cs - vec3(1.0)), step(vec3(0.5), Cs)));)";
inline const char* kBlendDifferenceBody = R"(  return abs(Cb - Cs);)";
inline const char* kBlendExclusionBody = R"(  return Cb + Cs - 2.0 * Cb * Cs;)";
inline const char* kBlendSubtractBody = R"(  return max(Cb - Cs, vec3(0.0));)";
inline const char* kBlendDivideBody = R"(  return min(vec3(1.0), Cs / max(vec3(1e-5), vec3(1.0) - Cb));)";
inline const char* kBlendHueBody = R"(  return setLum(setSat(Cs, sat(Cb)), lum(Cb));)";
inline const char* kBlendSaturationBody = R"(  return setLum(setSat(Cb, sat(Cs)), lum(Cb));)";
inline const char* kBlendColorBody = R"(  return setLum(Cs, lum(Cb));)";
inline const char* kBlendLuminosityBody = R"(  return setLum(Cb, lum(Cs));)";

}  // namespace shaders
}  // namespace montage

#endif  // MONTAGE_RENDER_SHADERS_H
