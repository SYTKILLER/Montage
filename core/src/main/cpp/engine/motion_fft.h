#ifndef MONTAGE_ENGINE_MOTION_FFT_H
#define MONTAGE_ENGINE_MOTION_FFT_H

// P5① 动感模糊 FFT（photocraft blur/motion_fft.rs C++ 翻译，MIT 语义对拍）：
// 大距离（dist ≥ 64）动模糊核改频域实现——合并系数核（PR #902 f32 几何：沿线
// t=i/steps-0.5 双线性展开、首见序合并、norm=1/(steps+1)）频域逐点相乘，overlap-save
// 512 瓦片 + 核包络 halo，变换尺寸 = 2^a·3^b·5^c ≥ needed（≤8192）。变换用 f64 复数，
// 预乘 alpha 直到最后输出。求和序与行核不同（photocraft 同注）——宿主 oracle 以容差对拍。

#include <cstdint>
#include <string>

namespace montage {
namespace filters {

// 频域动模糊（src/dst = w×h RGBA8888 直 alpha，stride=w*4）。dist < crossover 或资源
// 超限（fft 侧 >8192 / 点数 >4M / 内存 >128MB）返回 false（调用方回退行采样路径）。
bool motionBlurFft(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h, float angleDeg,
                   float distance, std::string& err);

// FFT/行采样跨接点（photocraft worthwhile 的保守化：行核 dist·ch < 1500·pairs 留行核；
// 本仓 4 通道恒在 → dist ≥ 64 走 FFT，与 UI 距离滑杆中段对齐）。
inline constexpr int kMotionFftCrossover = 64;

}  // namespace filters
}  // namespace montage

#endif  // MONTAGE_ENGINE_MOTION_FFT_H
