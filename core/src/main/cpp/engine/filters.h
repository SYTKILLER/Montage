#ifndef MONTAGE_ENGINE_FILTERS_H
#define MONTAGE_ENGINE_FILTERS_H

// M13 滤镜核（纯函数，宿主机单测直测）：连续 RGBA8888 直 alpha 域（stride = w*4）。
// 算法语义对拍 photocraft algo crate（MIT，07 §3.4 注入）：高斯 = σ≤4 精确可分离 /
// σ>4 三 box 近似（boxes_for_gauss）；卷积一律预乘域；USM 阈值域 0..1；噪声坐标哈希
// 确定性（hash01）；Min/Max 方形窗口或圆盘（van Herk 行窗）。域外取样 = 透明
//（全域 bbox 处理语义，等价 photocraft Halo::Bounds）。

#include <cstdint>
#include <string>

namespace montage {
namespace filters {

// FilterKind（索引即 UI Select 序号；与 ArkTS FILTER_CATALOG 顺序对齐）
enum FilterKind : int32_t {
    FGaussianBlur = 0,
    FBoxBlur,
    FMotionBlur,
    FUnsharpMask,
    FSmartSharpen,
    FHighPass,
    FAddNoise,
    FMedian,
    FDustScratches,
    FReduceNoise,
    FMosaic,
    FCrystallize,
    FMezzotint,
    FFragment,
    FPointillize,
    FColorHalftone,
    FFindEdges,
    FEmboss,
    FWind,
    FTiles,
    FExposure,
    FClouds,
    FDifferenceClouds,
    FLensFlare,
    FMinimum,
    FMaximum,
    FOffset,
    FCustom,
    FLensCorrection,  // M20d：镜头校正（lens_ops correct；p0..p5 见下）
    FCameraRaw,       // M23c：Camera Raw 9 级链（cameraJson 承载全部参数）
    FColorLookup,     // M22：颜色查找（LutFile 采样；p0 = 表选择：0..N-1 内置外观，
                      //       100+ = 进程级已导入的用户表）
};
constexpr int32_t kFilterKindCount = 31;

// 参数槽（语义随 kind；与 06 §4 M13 对话框 schema 对齐）：
//   GaussianBlur   p0=σ(0.1..500)
//   BoxBlur        p0=r(1..500)
//   MotionBlur     p0=angle°(-360..360) p1=distance(1..999)
//   UnsharpMask    p0=amount%(1..500) p1=radius(0.1..100) p2=threshold(0..255)
//   SmartSharpen   p0=amount% p1=radius p2=reduceNoise%
//   HighPass       p0=radius(0.1..100)
//   AddNoise       p0=amount%(0..400) p1=distribution(0均匀/1高斯) p2=mono(0/1)
//   Median         p0=radius(1..10)
//   DustScratches  p0=radius(1..10) p1=threshold(0..255)
//   ReduceNoise    p0=radius(1..10)  [双边近似，登记]
//   Mosaic         p0=cellSize(2..200)
//   Crystallize    p0=cellSize p1=seed
//   Mezzotint      p0=type(0细/1中/2粗) p1=seed
//   Fragment       —
//   Pointillize    p0=cellSize p1=seed
//   ColorHalftone  p0=maxRadius(4..127)
//   FindEdges      —
//   Emboss         p0=angle° p1=height(1..100) p2=amount%(1..500)
//   Wind           p0=direction(0左/1右) p1=strength(1..100)
//   Tiles          p0=tileW p1=tileH p2=offset%(0..99) p3=seed
//   Exposure       —
//   Clouds         —（云彩用前景/背景色由管线注入 p3..p5? 用 colorFg/colorBg 字段）
//   DiffClouds     —
//   LensFlare      p0=centerX p1=centerY p2=brightness%(10..300)
//   Minimum/Maximum p0=radius(1..100) p1=preserve(0方形/1圆盘)
//   Offset         p0=dx p1=dy
//   Custom         kernel[25]（5×5 行主序）+ p0=scale p1=offset
//   LensCorrection p0=RemoveDistortion(−100..100) p1=垂直透视 p2=水平透视 p3=角度°
//                  p4=缩放%(50..150) p5=暗角数量(−100..100)；色差/中点 v1 固定默认（登记）
struct FilterParams {
    int32_t kind = 0;
    float p0 = 0.0f;
    float p1 = 0.0f;
    float p2 = 0.0f;
    float p3 = 0.0f;
    float p4 = 0.0f;
    float p5 = 0.0f;
    uint32_t seed = 0;        // 噪声/云彩确定性（同参数同结果）
    float colorFg[3] = {0.0f, 0.0f, 0.0f};  // 云彩前景色（直 alpha 0..1）
    float colorBg[3] = {1.0f, 1.0f, 1.0f};  // 云彩背景色
    float kernel[25] = {0.0f};
    float scale = 1.0f;
    float offset = 0.0f;
    std::string cameraJson;  // M23c：FCameraRaw 专用（CameraRaw camelCase JSON）
};

// 核执行：src/dst 均 w×h RGBA8888 直 alpha（stride = w*4）。域外取样 = 透明。
bool applyFilter(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h,
                 const FilterParams& p, std::string& err);

}  // namespace filters
}  // namespace montage

#endif  // MONTAGE_ENGINE_FILTERS_H
