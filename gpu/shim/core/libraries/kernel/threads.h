// bbport: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        std::scoped_lock lock{join_mutex};
        thread = std::jthread([this, func = std::move(func)](std::stop_token stop) {
            current_thread = this;
            runtime_thread_attach_host("bb:hle");
            func(stop);
            current_thread = nullptr;
        });
        stop_source = thread.get_stop_source();
    }
    // AvPlayer calls Join from its worker, the demuxer and the game thread.
    // A self-join is a no-op; an owner must still join the completed worker.
    // Serialize external joins so two callers cannot join the same pthread.
    void Join() {
        if (current_thread == this) return;
        std::scoped_lock lock{join_mutex};
        if (!thread.joinable()) return;
        thread.join();
    }
    bool Joinable() const {
        std::scoped_lock lock{join_mutex};
        return thread.joinable();
    }
    void Stop() {
        stop_source.request_stop();
        Join();
    }

private:
    std::jthread thread;
    std::stop_source stop_source;
    mutable std::mutex join_mutex;
    inline static thread_local Thread* current_thread{};
};
} // namespace Libraries::Kernel
