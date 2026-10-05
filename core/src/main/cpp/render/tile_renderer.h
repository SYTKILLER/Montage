#ifndef MONTAGE_RENDER_TILE_RENDERER_H
#define MONTAGE_RENDER_TILE_RENDERER_H

// 瓦片绘制（03 §6 定案）：纹理键=瓦片对象指针（不可变 ⇒ 指针即内容身份）；
// 只上传视口相交瓦片；LRU 预算（缺省 1024 片 ≈ 256MB）；≥200% 切 NEAREST。
// 所有 GL 调用只在渲染线程（vsync 回调线程）发生。

#include <cstdint>
#include <unordered_map>

#include <GLES3/gl3.h>

#include "engine/document.h"

namespace montage {

class TileRenderer {
  public:
    // GL 线程首帧惰性初始化（编译 program / 建 quad VBO）
    void ensureInit();
    // 绘制一帧（背景 + 棋盘格 + 可见瓦片）。vw/vh = 视口 px。
    void drawFrame(const Document& doc, const Viewport& vp, int32_t vw, int32_t vh);
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
    void drawQuad(GLuint program, float rx, float ry, float rw, float rh, float zoom, float panX,
                  float panY, float vw, float vh);

    GLuint checkerProg_ = 0;
    GLuint tileProg_ = 0;
    GLuint vbo_ = 0;
    GLuint vao_ = 0;
    bool inited_ = false;

    std::unordered_map<uint32_t, CacheEntry> cache_;
    uint64_t frame_ = 0;
    static constexpr size_t kCacheCap = 1024;  // 1024×256KB = 256MB，与 02 Limits 缓存预算一致
};

}  // namespace montage

#endif  // MONTAGE_RENDER_TILE_RENDERER_H
