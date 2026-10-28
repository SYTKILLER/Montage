#ifndef MONTAGE_ENGINE_PARALLEL_H
#define MONTAGE_ENGINE_PARALLEL_H

// P5②：轻量并行 for（滤镜分块/FFT 瓦片共用）。纯数据分片、无共享写——块间独立，
// 主线程聚合。threads ≤ 1 或 work 不足时串行内联（开销最小路径）。

#include <atomic>
#include <thread>
#include <vector>

namespace montage {

// [begin, end) 按块均分给 threads 线程；fn(begin, end) 每段调用一次。
// threads ≤ 1 或 total < minPerThread 时退化为单线程（同线程直接执行）。
template <typename Fn>
void parallelFor(size_t begin, size_t end, size_t minPerThread, int threads, Fn&& fn) {
    const size_t total = end > begin ? end - begin : 0;
    if (total == 0) {
        return;
    }
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) {
        hw = 1;
    }
    const int cap = threads > 0 ? std::min<int>(threads, static_cast<int>(hw))
                                : static_cast<int>(hw);
    size_t nThreads = static_cast<size_t>(cap > 0 ? cap : 1);
    while (nThreads > 1 && total < nThreads * minPerThread) {
        --nThreads;
    }
    if (nThreads <= 1) {
        fn(begin, end);
        return;
    }
    const size_t chunk = (total + nThreads - 1) / nThreads;
    std::vector<std::thread> pool;
    pool.reserve(nThreads - 1);
    for (size_t t = 1; t < nThreads; ++t) {
        const size_t lo = begin + t * chunk;
        const size_t hi = std::min(end, lo + chunk);
        if (lo >= hi) {
            continue;
        }
        pool.emplace_back([&fn, lo, hi]() { fn(lo, hi); });
    }
    fn(begin, std::min(end, begin + chunk));
    for (auto& th : pool) {
        th.join();
    }
}

}  // namespace montage

#endif  // MONTAGE_ENGINE_PARALLEL_H
