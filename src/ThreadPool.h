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
#include <cstdio>
#include <cstdlib>

class ParallelFor {
public:
    // How many CPUs can this process actually burn?  std::thread::hardware_concurrency() only
    // sees the CPUs the host exposes; inside a container with a CFS quota the truth is
    //   /sys/fs/cgroup/cpu.max  (cgroup v2)   or   cpu.cfs_quota_us / cpu.cfs_period_us  (v1)
    // Spawning 32 threads on a 4 CPU quota is not just pointless, it is measurably slower --
    // the cgroup gets throttled and pays for the extra context switches.
    // Returns 0 when no limit is set ("max" / "-1"), i.e. "unknown".
    static unsigned cgroupCpuLimit () {
        long quota = 0, period = 0;
        FILE *fp = fopen ("/sys/fs/cgroup/cpu.max", "r");
        if (fp) {                                          // cgroup v2 : "<quota> <period>"
            char a[64] = {0}, b[64] = {0};
            if (fscanf (fp, "%63s %63s", a, b) == 2) {
                if (a[0] != 'm' && a[0] != '-') { quota = atol (a); period = atol (b); }
            }
            fclose (fp);
        } else if ((fp = fopen ("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", "r")) != NULL) {
            if (fscanf (fp, "%ld", &quota) == 1) {         // cgroup v1
                fclose (fp);
                period = 0;
                if ((fp = fopen ("/sys/fs/cgroup/cpu/cpu.cfs_period_us", "r")) != NULL) {
                    if (fscanf (fp, "%ld", &period) != 1) period = 0;
                    fclose (fp);
                }
            } else fclose (fp);
        }
        if (quota <= 0 || period <= 0) return 0;
        unsigned n = (unsigned)((quota + period - 1) / period);
        return n ? n : 1;
    }

    // returns the number of threads that would be used for 'n_thread == 0'
    static unsigned defaultThreadCount () {
        unsigned n = std::thread::hardware_concurrency();
        if (!n) n = 1;
        unsigned q = cgroupCpuLimit();
        if (q && q < n) n = q;      // do not oversubscribe a throttled cgroup
        return n;
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
