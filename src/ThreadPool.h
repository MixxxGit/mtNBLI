//==================================================================================================
//  minimal, dependency free "parallel for"  (C++11)
//==================================================================================================
#ifndef __THREAD_POOL_H__
#define __THREAD_POOL_H__

#include <thread>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <exception>

class ParallelFor {
public:
    // returns the number of threads that would be used for 'n_thread == 0'
    static unsigned defaultThreadCount () {
        unsigned n = std::thread::hardware_concurrency();
        return n ? n : 1;
    }

    // run fn(i) for i in [0,n) .  n_thread == 0 -> use all available hardware threads
    static void run (size_t n, unsigned n_thread, const std::function<void(size_t)> &fn) {
        if (n == 0) return;

        if (n_thread == 0) n_thread = defaultThreadCount();
        if (n_thread == 0) n_thread = 1;

        size_t nt = n;
        if ((size_t)n_thread < nt) nt = n_thread;

        if (nt <= 1) {
            for (size_t i=0; i<n; i++) fn(i);
            return;
        }

        std::atomic<size_t> next (0);
        std::mutex          mtx;

        std::vector<std::thread> ths;
        ths.reserve(nt);

        for (size_t t=0; t<nt; t++) {
            ths.push_back (std::thread ([&fn, &next, &mtx, n] () {
                for (;;) {
                    size_t i = next.fetch_add(1);
                    if (i >= n) break;
                    try {
                        fn(i);
                    } catch (...) {
                        std::lock_guard<std::mutex> g (mtx);
                    }
                }
            }));
        }

        for (size_t t=0; t<ths.size(); t++)
            ths[t].join();
    }
};

#endif // __THREAD_POOL_H__
