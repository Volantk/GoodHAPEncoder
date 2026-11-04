#pragma once
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace ghap {

// Runs fn(i) for every i in [0, n) on all hardware threads and returns when
// all are done. (Apple's libc++ has no std::execution::par.)
template <class Fn>
void parallelFor(int n, Fn&& fn) {
    if (n <= 0) return;
    int workers = int(std::min<unsigned>(std::max(1u, std::thread::hardware_concurrency()), unsigned(n)));
    if (workers == 1) {
        for (int i = 0; i < n; ++i) fn(i);
        return;
    }
    std::atomic<int> next{0};
    auto run = [&] {
        for (int i; (i = next.fetch_add(1)) < n;) fn(i);
    };
    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for (int t = 1; t < workers; ++t) pool.emplace_back(run);
    run();
    for (auto& t : pool) t.join();
}

} // namespace ghap
