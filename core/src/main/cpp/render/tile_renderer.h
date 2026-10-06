#ifndef MONTAGE_RENDER_TILE_RENDERER_H
#define MONTAGE_RENDER_TILE_RENDERER_H

// 图层合成渲染（03 §4 定案）：每层两趟（assemble 进 layerFBO + blend 进 acc ping-pong），
// 4 混合模式独立 program（R3.3）；acc 优先 RGBA16F（探测 EXT_color_buffer_float，回退 RGBA8）；
// 瓦片纹理按需上传 + LRU（缺省 1024 片 ≈ 256MB）；≥200% 切 NEAREST（crispZoom）。
// 所有 GL 调用只在渲染线程（vsync 回调线程）发生。

#include <cstdint>
#include <map>
#include <unordered_map>

#include <GLES3/gl3.h>

#include "engine/document.h"

namespace montage {

class TileRenderer {
  public:
    // GL 线程首帧惰性初始化（编译 program / 建 quad VBO）
    void ensureInit();
    // 绘制中的笔画瓦片覆盖（直通 RGBA，逐帧直传不进 LRU）
    struct StrokeOverlay {
        bool active = false;
        LayerId layerId = 0;
        uint32_t gridCols = 0;
        uint32_t gridRows = 0;
        const std::map<uint32_t, std::vector<uint8_t>>* tiles = nullptr;
    };
    // 绘制一帧（背板 + 棋盘格 + 图层栈合成）。vw/vh = 视口 px。
    void drawFrame(const Document& doc, const Viewport& vp, int32_t vw, int32_t vh,
                   const StrokeOverlay* stroke);
    // 上下文销毁/丢失时清空全部 GL 资源（03 R3.9：派生缓存语义）
    void invalidate();

  private:
    struct CacheEntry {
        uint32_t tex = 0;
        const Tile* tilePtr = nullptr;  // 身份键：指针变了就重传
        bool nearest = false;
        uint64_t lastFrame = 0;
    };

    static GLuint buildProgram(const char* vertSrc, const char* fragSrc);
    static GLuint buildBlendProgram(int mode);
    bool ensureFbos(int32_t vw, int32_t vh);
    void destroyFbos();
    void drawQuad(GLuint program, float rx, float ry, float rw, float rh, float zoom, float panX,
                  float panY, float vw, float vh);
    void drawTiles(const Layer& layer, float zoom, float panX, float panY, float vw, float vh,
                   const StrokeOverlay* stroke);
    void uploadLut(const float* lut256);  // M6a：调整层 LUT 上传
    std::unordered_map<uint64_t, uint32_t> strokeTex_;  // key → 临时纹理（逐帧刷新）

    GLuint checkerProg_ = 0;
    GLuint tileProg_ = 0;
    GLuint plainProg_ = 0;
    GLuint clipProg_ = 0;  // M5b-2：剪贴 alpha 乘法
    GLuint adjustProg_ = 0;  // M6a：调整层 LUT
    GLuint blendProgs_[kBlendModeCount] = {0};
    GLuint vbo_ = 0;
    GLuint vao_ = 0;
    bool inited_ = false;

    // 合成 FBO（视口分辨率；acc 双缓冲 ping-pong + 单层 assemble）
    GLuint accFbo_[2] = {0, 0};
    GLuint accTex_[2] = {0, 0};
    GLuint layerFbo_ = 0;
    GLuint layerTex_ = 0;
    GLuint clipFbo_ = 0;  // 剪贴层 × 基 alpha 乘积
    GLuint clipTex_ = 0;
    GLuint baseFbo_ = 0;  // 剪贴基的 assemble alpha 快照
    GLuint baseTex_ = 0;
    GLuint lutTex_ = 0;   // M6a：调整层 256×1 LUT
    GLint fboW_ = 0;
    GLint fboH_ = 0;
    bool float16_ = false;
    bool float16Probed_ = false;

    std::unordered_map<uint64_t, CacheEntry> cache_;  // key = layerId<<32 | tileIndex
    uint64_t frame_ = 0;
    static constexpr size_t kCacheCap = 1024;  // 1024×256KB = 256MB，与 02 Limits 缓存预算一致
};

}  // namespace montage

#endif  // MONTAGE_RENDER_TILE_RENDERER_H
