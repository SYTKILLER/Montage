#ifndef MONTAGE_ENGINE_SMART_OPS_H
#define MONTAGE_ENGINE_SMART_OPS_H

// M21 智能对象与智能滤镜（07 §3.12；photocraft engine/src/smart_cmds.rs 渲染管线 C++ 翻译，
// MIT OR Apache-2.0）：
//   源字节 ──decode──► 子文档 ──flatten──► 源网格 ──place(仿射)──► placed
//          ──逐可见智能滤镜(applyFilter+blend/opacity 直混)──► filtered
//          ──滤镜蒙版 mix──► smartCache
// 缓存两级（photocraft decode cache / stack_cache 同构，v1 不做逐滤镜阶段缓存）：
// 源解码缓存（contentsId+sourceHash 键）+ placed 缓存（源 hash+transform 键）。
// 渲染指纹命中即跳过重渲；重活全部锁外（Engine 级缓存独立互斥，不碰 docMutex）。

#include <cstdint>
#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {
namespace smart {

// contentsId 进程内单调（duplicate 共享、via copy 独立；.montage v13 持久化、open 回填防撞号）
uint64_t nextContentsId();
void bumpContentsSeqAbove(uint64_t v);

// 渲染指纹（源 hash + 放置仿射 + 滤镜栈 + 滤镜蒙版 revision；源 hash 为 0 时现算）
uint64_t fingerprint(const SmartLayerState& sm);

// 源解码 → 子文档扁平合成（源域 strip，直 alpha RGBA8888，行距 = w*4）。
// 分发按扩展名：.montage → 内存工程打开 + flatten；.psd → readPsd + 组装 + flatten；
// .png → 自研确定性解码；.jpg/.jpeg/.bmp/.webp 等 → 系统解码（CreateFromData，
// mimeType 驱动 swapRB——M16a 校准纪律）。失败返回 false + err。
bool decodeSourceFlat(const std::string& fileName, const std::vector<uint8_t>& bytes,
                      int stackMode, uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba,
                      std::string& err);

// M21 收口：堆栈模式统计核（photocraft algo/stack.rs 翻译；宿主 oracle 直测）。
// stackStat：0 熵 1 峰度 2 最大 3 均值 4 中值 5 最小 6 极差 7 偏度 8 标准差 9 总和 10 方差。
float stackStat(std::vector<float>& v, int s);
std::vector<uint8_t> stackCombine(const std::vector<std::vector<uint8_t>>& frames, int mode,
                                  size_t len);

// 重渲一层（指纹命中跳过；蒙版合成跟随）。false = 源不可用（缓存保持，photocraft 同语义：
// 链接缺失/解码失败时保留旧像素）。重渲回填 transform.origin = 放置域原点。
bool renderLayer(Layer& l, std::string& err);

// 全文档智能层重渲（restyleDocument 挂载点；平铺序，先于样式派生）
void renderDocument(Document& doc);

// 层 → 智能对象（photocraft layers_to_smart 语义）：成员层（含子树段）移入单层顶子文档
// → .montage 内存编码为嵌入源；smartCache = 现渲染外观（文档观感不变）。成员从原栈移除
// 由调用侧负责。members 内组层必须携带其连续子层段（平铺不变式）。
bool layersToSmart(const std::string& name, LayerId newId, std::vector<Layer>& members,
                   Layer& out, std::string& err);

// 便捷单层版（name 取层名；外观态——可见/不透明/混合/剪贴——保留到 SO wrapper）
bool layerToSmart(const Layer& l, LayerId newId, Layer& out, std::string& err);

// 源预解码入缓存（置入/替换命令用：取源尺寸算放置，后续 renderLayer 缓存命中不二次解码）
bool ensureSourceCached(const SmartLayerState& sm, uint32_t& w, uint32_t& h, std::string& err);

// 会话缓存清理（Engine::resetDocument 挂载点：源解码/placed 缓存随文档替换清空）
void clearSessionCaches();

// 会话缓存字节量（诊断用）
size_t cacheBytes();

}  // namespace smart
}  // namespace montage

#endif  // MONTAGE_ENGINE_SMART_OPS_H
