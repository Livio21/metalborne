// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <sched.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread.h>
#endif

namespace BbThreads {

inline unsigned Tid() {
#ifdef __APPLE__
    uint64_t tid=0;
    pthread_threadid_np(nullptr, &tid);
    return static_cast<unsigned>(tid);
#else
    return static_cast<unsigned>(gettid());
#endif
}

/// Hardware threads available to the process.
inline unsigned Available() {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
#endif
    return std::max(1u, std::thread::hardware_concurrency());
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#else
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
#endif
}

} // namespace BbThreads
