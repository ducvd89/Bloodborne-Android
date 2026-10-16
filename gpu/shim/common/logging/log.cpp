// bbport: see log.h.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "common/logging/log.h"

namespace Common::Log {
static Level threshold() {
    static const Level level = [] {
        const char* v = std::getenv("BB_GPU_LOG");
        if (!v) return Level::warn;
        static const char* names[] = {"trace", "debug", "info", "warning", "error", "critical"};
        for (int i = 0; i < 6; ++i)
            if (!std::strcmp(v, names[i])) return Level(i);
        return Level::warn;
    }();
    return level;
}
bool ShouldLog(Level level) { return level >= threshold(); }
void Write(std::string_view log_class, Level level, const char* file, int line, const char* function,
           std::string&& message) {
    static std::mutex mutex;
    static const char* names[] = {"Trace", "Debug", "Info", "Warning", "Error", "Critical"};
    // Full debug build for issue #15: BB_GPU_LOG=debug can be chatty and the log goes into a report zip with a
    // size limit. A budget per kind of line: the first 20000 trace/debug/info lines and the first 40000
    // warnings/errors are written, after that one line in 1000 (with a count of those left out).
    static std::atomic<unsigned long long> verbose_lines{0}, serious_lines{0};
    const bool serious = level >= Level::warn;
    const unsigned long long n = (serious ? serious_lines : verbose_lines).fetch_add(1) + 1;
    const unsigned long long budget = serious ? 40000 : 20000;
    if (n > budget) {
        if ((n - budget) % 1000 != 1) return;
        std::fprintf(stderr, "GPU [log budget] %llu %s lines in all, %llu of them not written (one in 1000 is)\n", n,
                     serious ? "warning/error" : "debug/info", n - budget);
    }
    const char* base = std::strrchr(file, '/');
    std::scoped_lock lock{mutex};
    std::fprintf(stderr, "GPU [%.*s] <%s> %s:%d %s: %s\n", int(log_class.size()), log_class.data(),
                 names[int(level)], base ? base + 1 : file, line, function, message.c_str());
}
} // namespace Common::Log
