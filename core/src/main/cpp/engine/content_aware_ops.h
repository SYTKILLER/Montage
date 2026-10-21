#ifndef MONTAGE_ENGINE_CONTENT_AWARE_OPS_H
#define MONTAGE_ENGINE_CONTENT_AWARE_OPS_H

// 内容感知填充（M19b，photocraft algo/content_aware.rs C++ 翻译，MIT OR Apache-2.0）：
// fill() = 受限源补全——hole 只从 source 掩码内像素拷贝；适配变体（镜像/旋转/缩放的源
// 双线性变换副本）以「马赛克拼装」（本体在左、变体列在右、gap=Void 隔离）喂给
// completeMasked，无变体时直接补全本体；无源可拷回退膜填充。
// levelTransform/colorAmount = 修补/内容感知移动的 Color 选项（PS 25.4 实测：shift ≤ 0.04·color）。
// 登记遗留：auto_sampling（PS Auto 采样区）依赖 segment crate 图割，挂后续批。
// 宿主测试：.debug/m19/test_ca.cpp（photocraft content_aware.rs oracle 直迁）。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "engine/inpaint_ops.h"

namespace montage {
namespace content_aware {

// 取消谓词（取消后返回空向量，结果不完整）。
using CancelFn = std::function<bool()>;

// fill() 选项（默认 = colorLevel("default")）。
struct FillOptions {
    inpaint::Adapt colorAdaptation;   // 拷贝的色适配
    std::vector<float> rotations;     // 额外源旋转（度，PS 适配档位）
    bool scale = false;               // 加缩放源副本（0.8 / 1.25）
    bool mirror = false;              // 加水平镜像源副本
    uint64_t seed = 1;

    static FillOptions makeDefault();
};

// Photoshop 适配档位 → 旋转组（none/low/medium/high/full）。
std::vector<float> rotationLevel(const char* level);

// Photoshop 色适配档位 → Adapt（none/default/high/veryHigh）。
inpaint::Adapt colorLevel(const char* level);

// Patch/内容感知移动 Color 选项（0–10）→ 色适配（实测 shift ≤ 0.04·color）。
inpaint::Adapt colorAmount(uint8_t color);

// 逐通道 gain/bias：把 from 样本拟合到 to 的均值/对比（限幅内）。
std::vector<std::pair<float, float>> levelTransform(size_t ch, const std::vector<float>& from,
                                                    const std::vector<float>& to,
                                                    inpaint::Adapt a);

// 受限源内容感知填充：hole 只从 source（且非 hole）像素拷贝。返回孔外等于输入的全图缓冲；
// 取消返回空向量。
std::vector<float> fill(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                        const std::vector<bool>& hole, const std::vector<bool>& source,
                        const FillOptions& opts, const CancelFn& cancel = nullptr);

// ---- 以下暴露给宿主测试 ----

// 源的变换副本（绕中心旋转 deg 度/缩放 scale/镜像）；ok = 双线性四 tap 全可用。
struct Variant {
    size_t w = 0, h = 0;
    std::vector<float> img;
    std::vector<bool> ok;
};
Variant variantOf(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                  const std::vector<bool>& srcOk, float deg, float scale, bool mirror);

}  // namespace content_aware
}  // namespace montage

#endif  // MONTAGE_ENGINE_CONTENT_AWARE_OPS_H
