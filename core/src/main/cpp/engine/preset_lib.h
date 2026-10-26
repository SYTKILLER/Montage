#ifndef MONTAGE_ENGINE_PRESET_LIB_H
#define MONTAGE_ENGINE_PRESET_LIB_H

// P1 预设库进程级像素池：图案库（TileGrid，供图案图章/油漆桶图案填充/填充对话框/纹理
// 叠加/形状图案填充引用）+ 笔刷采样 tip 库（灰度位图，供缩略图；位图盖章登记后续批）。
// 函数级静态单例（io/cms lutfile LookupSlots 同模式）：进程级、不入 .montage/History、
// 关闭文档保留；v1 不持久化（跨进程重启丢失，色板/渐变/笔刷参数走 TS Preferences，登记）。
// 容量：图案 ≤64、tip ≤64（超出拒绝导入，UI 提示删除旧项后重试）；单边 ≤4096。

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {

class PresetLib {
  public:
    static constexpr size_t kMaxPatterns = 64;
    static constexpr size_t kMaxTips = 64;
    static constexpr uint32_t kMaxEdge = 4096;

    static PresetLib& get();

    std::mutex mtx;

    struct LibPattern {
        std::string id;   // "libpat_<n>"（与文档图案 pat_<n> 命名空间区分）
        std::string name;
        uint32_t w = 0, h = 0;
        std::shared_ptr<const TileGrid> pixels;  // 直 alpha
    };
    struct LibTip {
        std::string id;  // "libtip_<n>"
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> gray;  // 8-bit 灰度（16 位导入取高字节），行主序
    };

    // 导入图案（rgba = 直 alpha w×h×4）。id 与 name 生成；满/超限返回 false + err。
    bool addPattern(const std::string& name, uint32_t w, uint32_t h, const uint8_t* rgba,
                    std::string& idOut, std::string& err);
    // 导入笔刷 tip（gray = 8-bit 灰度 w×h）。
    bool addTip(const std::string& name, uint32_t w, uint32_t h, const uint8_t* gray,
                std::string& idOut, std::string& err);
    bool dropPattern(const std::string& id);
    bool dropTip(const std::string& id);
    const LibPattern* findPattern(const std::string& id) const;   // 须持 mtx
    const LibTip* findTip(const std::string& id) const;           // 须持 mtx

    std::vector<LibPattern> patterns;
    std::vector<LibTip> tips;

    // 缩略图（≤128 边 box 下采样；kind 0=pattern → RGBA，1=tip → 灰度转 RGBA 白底）。
    // 须持 mtx。返回 false = id 不存在。
    bool thumb(const std::string& id, bool isTip, uint32_t& w, uint32_t& h,
               std::vector<uint8_t>& rgba) const;

  private:
    PresetLib() = default;
    uint64_t seq_ = 0;
};

}  // namespace montage

#endif  // MONTAGE_ENGINE_PRESET_LIB_H
