#ifndef MONTAGE_ENGINE_MAXFLOW_H
#define MONTAGE_ENGINE_MAXFLOW_H

// P5③：二值图割的最大流（Boykov–Kolmogorov，photocraft segment/maxflow.rs C++ 直译，
// IEEE PAMI 26(9) 2004）。两棵搜索树 S/T 相向生长，S–T 接触给增广路；饱和树边产生
// 孤儿节点再收养。搜索树在增广间复用（图像网格短路径场景快）。
// 登记：photocraft 的 ≥4096 节点双终端 FIFO push/relabel 分支未翻译——本仓图割只服
// auto_sampling（≤384² 降采样网格），BK 语义等价仅性能差异。

#include <cstdint>
#include <deque>
#include <vector>

namespace montage {

class MaxflowGraph {
  public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    static constexpr uint32_t kTerminal = 0xFFFFFFFEu;
    static constexpr uint32_t kOrphan = 0xFFFFFFFDu;
    static constexpr uint32_t kInfDist = 0xFFFFFFFFu;
    static constexpr float kEps = 1e-5f;

    explicit MaxflowGraph(size_t nodes, size_t edgesHint = 0);

    // 源/汇容量（累加；公共部分记为必过流）。
    void addTweights(size_t i, float capSource, float capSink);
    // 有向边 i→j 容量 cap，反向 j→i 容量 revCap（成对存储 a/a^1）。
    void addEdge(size_t i, size_t j, float cap, float revCap);
    // 最大流（= 最小割容量）。返回流值。
    double maxflow();
    // maxflow 后：节点 i 是否在最小割源侧（双侧不可达归汇侧 = 最小源集）。
    bool inSource(size_t i) const;

  private:
    void bk();
    void augment(size_t m, std::deque<uint32_t>& orphans);
    void adopt(size_t i, uint32_t time, std::deque<uint32_t>& queue,
               std::deque<uint32_t>& orphans);

    // 节点
    std::vector<uint32_t> first_;
    std::vector<float> trCap_;
    std::vector<uint32_t> parent_;
    std::vector<uint8_t> isSink_;
    std::vector<uint32_t> ts_;
    std::vector<uint32_t> dist_;
    std::vector<uint8_t> active_;
    // 弧（成对：a 与 a^1 互逆）
    std::vector<uint32_t> head_;
    std::vector<uint32_t> next_;
    std::vector<float> rCap_;
    double flow_ = 0.0;
};

}  // namespace montage

#endif  // MONTAGE_ENGINE_MAXFLOW_H
