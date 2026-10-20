// bbport: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include "common/types.h"
#include "core/libraries/kernel/threads/pthread.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        Stop();
        std::scoped_lock lock{mutex};
        thread = std::jthread([func = std::move(func)](std::stop_token stop) {
            runtime_thread_attach_host("bb:hle");
            func(stop);
        });
    }
    // AvPlayer threads stop their own Thread objects while the owner may be stopping them too
    // (AvPlayerSource::Stop). The jthread is taken out under the lock, so exactly one caller
    // ends it: the thread itself detaches, anyone else joins.
    void Join() { Finish(Take()); }
    bool Joinable() const {
        std::scoped_lock lock{mutex};
        return thread.joinable();
    }
    void Stop() {
        std::jthread taken = Take();
        if (taken.joinable()) taken.request_stop();
        Finish(std::move(taken));
    }

private:
    std::jthread Take() {
        std::scoped_lock lock{mutex};
        return std::move(thread);
    }
    static void Finish(std::jthread taken) {
        if (!taken.joinable()) return;
        if (taken.get_id() == std::this_thread::get_id()) taken.detach();
        else taken.join();
    }

    mutable std::mutex mutex;
    std::jthread thread;
};
} // namespace Libraries::Kernel
