// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <cstring>
#include <string>
#include <pthread.h>
#include <dlfcn.h>
#include "bbport_backtrace.h"
#include <unistd.h>
#include <functional>

#include "bbport_copy.h"
#include "bbport_cpu.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "bbport_toggles.h"
#include "bbport_wait_trace.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_threads.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

namespace {
// bbport: ProducerScope state of this thread: the producer function it is in (any scheduler).
thread_local const char* producer_scope_where = nullptr;
std::atomic<u32> producer_reports{0};

/// Names a thread of this process (its comm).
std::string ThreadName(u32 tid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%u/comm", tid);
    std::string name = "?";
    if (FILE* file = std::fopen(path, "r")) {
        char buffer[32]{};
        if (std::fgets(buffer, sizeof(buffer), file)) {
            name = buffer;
            if (!name.empty() && name.back() == '\n') {
                name.pop_back();
            }
        }
        std::fclose(file);
    }
    return name;
}

void ReportProducer(const char* what, u32 other, const char* other_where, const char* where) {
    if (producer_reports.fetch_add(1, std::memory_order_relaxed) >= 8) {
        return;
    }
    const u32 self = u32(gettid());
    std::fprintf(stderr,
                 "Scheduler: %s: thread %u (%s) entering %s while thread %u (%s) is in %s\n",
                 what, self, ThreadName(self).c_str(), where, other, ThreadName(other).c_str(),
                 other_where ? other_where : "?");
    void* frames[24];
    const int depth = bb_backtrace(frames, 24);
    bb_backtrace_symbols_fd(frames, depth, 2);
}
} // namespace

void Scheduler::ProducerScope::Enter(const char* where) noexcept {
    previous_where = producer_scope_where;
    static thread_local const u32 tid = u32(gettid());
    producer_scope_where = where;
    u32 expected = 0;
    if (scheduler.producer_tid.compare_exchange_strong(expected, tid,
                                                       std::memory_order_acq_rel)) {
        outer = true;
        scheduler.producer_where.store(where, std::memory_order_relaxed);
        return;
    }
    if (expected != tid) {
        ReportProducer("CONCURRENT recording", expected,
                       scheduler.producer_where.load(std::memory_order_relaxed), where);
    }
}

void Scheduler::ProducerScope::Leave() noexcept {
    producer_scope_where = previous_where;
    if (outer) {
        scheduler.producer_tid.store(0, std::memory_order_release);
    }
}

namespace {

/// bbport: Vulkan recording threads: BB_VK_RECORD_THREADS, else 2 from 8 hardware threads and 3
/// from 12. One thread records ~75% of a core at 200 FPS (4 cores / 8 threads); the segments
/// let the stream use more than one.
u32 RecordingThreadCount() {
    if (const char* env = std::getenv("BB_VK_RECORD_THREADS")) {
        return std::clamp(std::atoi(env), 1, 8);
    }
    const unsigned available = BbThreads::Available();
    return available >= 12 ? 3 : available >= 8 ? 2 : 1;
}

vk::CommandBuffer BeginCommandBuffer(CommandPool& pool) {
    const vk::CommandBuffer cmdbuf = pool.Commit();
    Check(cmdbuf.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
    return cmdbuf;
}

} // namespace

Scheduler::Scheduler(const Instance& instance, bool threaded_recording)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    crumb_stream = Breadcrumbs::NewStream(
        instance, threaded_recording ? "draws: game and presenter" : "presentation");
    // bbport: BB_VK_RECORD_THREAD=0 records on the calling thread.
    const char* env = std::getenv("BB_VK_RECORD_THREAD");
    if (threaded_recording && !(env && env[0] == '0')) {
        record_chunk = AcquireChunk();
        ordered_chunk = AcquireChunk();
        // A segment of ~64 KiB of closures is ~60 draws: a frame of ~700 draws gives each
        // thread several segments, and the render passes a cut reopens stay few.
        const char* segment_kb = std::getenv("BB_VK_SEGMENT_KB");
        split_bytes = size_t(segment_kb ? std::max(1, std::atoi(segment_kb)) : 64) * 1024;
        const u32 count = RecordingThreadCount();
        for (u32 i = 0; i < count; ++i) {
            auto worker = std::make_unique<Worker>();
            if (i == 0) {
                worker->pool = &command_pool;
            } else {
                worker->own_pool = std::make_unique<CommandPool>(instance, &work_semaphore);
                worker->pool = worker->own_pool.get();
            }
            workers.push_back(std::move(worker));
        }
        for (u32 i = 0; i < count; ++i) {
            workers[i]->thread =
                std::jthread(std::bind_front(&Scheduler::RecorderThread, this), i);
        }
        std::printf("GPU: %u Vulkan recording thread%s, segments of %zu KiB\n", count,
                    count == 1 ? "" : "s", split_bytes / 1024);
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    // bbport BB_ASYNC_SUBMIT (default on; 0: off): submissions go out from the recording threads
    // once their segments are recorded, instead of this thread waiting for them (~65 us each).
    if (!workers.empty()) {
        const char* async = std::getenv("BB_ASYNC_SUBMIT");
        async_submit = !async || async[0] != '0';
        if (async_submit) {
            std::printf("GPU: submissions go out from the recording threads (BB_ASYNC_SUBMIT)\n");
        }
    }
    AllocateWorkerCommandBuffers();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    if (!workers.empty()) {
        SyncRecording();
        for (auto& worker : workers) {
            worker->thread.request_stop(); // wakes its wait on worker->cv
        }
        for (auto& worker : workers) {
            worker->thread.join();
        }
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    // bbport: a cut of the command stream (MaybeSplit) closed this render pass; it continues
    // in the next command buffer without clearing its attachments again.
    const bool resume = resume_rendering && !is_rendering && render_state == new_state;
    if (!is_rendering && render_state == new_state && pass_end_caller) {
        TracePassBreak(pass_end_caller);
    }
    resume_rendering = false;
    EndRendering();
    CarrySuspend(false);
    is_rendering = true;
    render_state = new_state;
    // bbport: buffer copies of this pass's draws may go in front of it (TryHoist).
    OpenHoistWindow();

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear && !resume ? vk::AttachmentLoadOp::eClear
                                             : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear && !resume ? vk::AttachmentLoadOp::eClear
                                            : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear && !resume ? vk::AttachmentLoadOp::eClear
                                              : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    if (workers.empty()) {
        current_cmdbuf.beginRendering(rendering_info);
        CarryResume(true);
        return;
    }
    // The attachment infos live on this stack frame: the recorded closure keeps copies.
    // bbport: a breadcrumb (when on) says whether the GPU got to the pass and into it.
    const Breadcrumbs::Crumb crumb{
        .kind = Breadcrumbs::Kind::Pass,
        .name = "begin rendering",
        .hash = {u64(db.has_depth) | u64(db.has_stencil) << 1 | u64(db.depth_clear) << 2 |
                     u64(db.stencil_clear) << 3,
                 render_state.num_layers},
        .count = {render_state.width, render_state.height, render_state.num_color_attachments},
    };
    RecordCrumb(crumb, [info = rendering_info, color_attachments, depth_attachment,
            stencil_attachment](vk::CommandBuffer cmdbuf) mutable {
        info.pColorAttachments = color_attachments.data();
        if (info.pDepthAttachment) {
            info.pDepthAttachment = &depth_attachment;
        }
        if (info.pStencilAttachment) {
            info.pStencilAttachment = &stencil_attachment;
        }
        cmdbuf.beginRendering(info);
    });
    CarryResume(true);
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    static const bool trace = [] {
        const char* env = std::getenv("BB_PASS_BREAK_TRACE");
        return env && env[0] == '1';
    }();
    if (trace) {
        pass_end_caller = __builtin_return_address(0);
    }
    CarrySuspend(true);
    is_rendering = false;
    Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
    CarryResume(false);
    CloseHoistWindow();
}

void Scheduler::TracePassBreak(void* caller) {
    static std::mutex mutex;
    static std::unordered_map<void*, u64> callers;
    static u64 breaks = 0;
    static auto last = std::chrono::steady_clock::now();
    std::scoped_lock lk{mutex};
    ++callers[caller];
    ++breaks;
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) {
        return;
    }
    std::vector<std::pair<u64, void*>> top;
    for (const auto& [address, count] : callers) {
        top.emplace_back(count, address);
    }
    std::ranges::sort(top, std::greater{});
    std::printf("Render pass breaks (the same pass begun again): %llu in %.0f s\n",
                static_cast<unsigned long long>(breaks),
                std::chrono::duration<double>(now - last).count());
    for (size_t i = 0; i < std::min<size_t>(top.size(), 10); ++i) {
        Dl_info info{};
        dladdr(top[i].second, &info);
        std::printf("Render pass break caller: %llu x %s+0x%lx\n",
                    static_cast<unsigned long long>(top[i].first),
                    info.dli_fname ? info.dli_fname : "?",
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) -
                                               reinterpret_cast<uintptr_t>(info.dli_fbase)));
    }
    callers.clear();
    breaks = 0;
    last = now;
}

namespace {

// bbport: copy hoisting statistics (BB_FRAME_STATS or BB_PASS_BREAK_TRACE=1), printed every 5 s.
enum HoistFallback : u32 {
    HoistNotDeferred,      ///< recording is not deferred (direct mode)
    HoistWindowClosed,     ///< a submission, cut or wait closed the pass's window, or none opened
    HoistDstReadByPass,    ///< the destination is read or written by the pass's draws
    HoistSrcWrittenByPass, ///< the source is written by the pass's draws
    HoistTooManyRanges,    ///< the pass touched too many separate ranges to track
    HoistFallbackCount,
};
std::atomic<u64> hoisted_commands{0};
std::atomic<u64> hoist_fallbacks[HoistFallbackCount]{};
std::atomic<u64> hoist_windows{0}, hoist_windows_spliced{0};

bool HoistEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_HOIST_COPIES");
        const bool on = !(env && env[0] == '0');
        std::printf("GPU: buffer copies inside render passes %s (BB_HOIST_COPIES=0: off)\n",
                    on ? "are hoisted in front of the pass when they cannot conflict"
                       : "end the pass");
        return on;
    }();
    return enabled;
}

bool HoistStatsEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_PASS_BREAK_TRACE");
        return BbStats::enabled || (env && env[0] == '1');
    }();
    return enabled;
}

/// BB_HOIST_HOLD_SIGNALS=1: fences inside a hoist window wait for it to close instead of forcing
/// the hand-over. Off by default: on the Thor it hoisted 7x more copies, but the later submissions
/// left the GPU idle longer (28 instead of 34 FPS).
bool HoistHoldSignals() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_HOIST_HOLD_SIGNALS");
        return env && env[0] == '1';
    }();
    return enabled;
}

/// BB_HOIST_HOLD_US (default 2000): how long a held signal waits for the next KickRecording to
/// release it.
std::chrono::microseconds HoistHoldTime() {
    static const auto time = std::chrono::microseconds([] {
        const char* env = std::getenv("BB_HOIST_HOLD_US");
        return env ? std::max<long long>(0, std::strtoll(env, nullptr, 10)) : 2000ll;
    }());
    return time;
}

/// BB_HOIST_SPLIT_MULT (default 2, as without a window): a pass with a hoist window is cut into
/// segments after this many times the segment length. Longer segments held the recording back
/// (with BB_HOIST_HOLD_SIGNALS, slower on the Thor).
size_t HoistSplitMult() {
    static const size_t mult = [] {
        const char* env = std::getenv("BB_HOIST_SPLIT_MULT");
        return env ? size_t(std::max(2, std::atoi(env))) : size_t(2);
    }();
    return mult;
}

std::atomic<u64> hoist_closed_valve{0}, hoist_closed_cap{0}, hoist_closed_normal{0};

void PrintHoistStats() {
    static std::mutex mutex;
    static auto last = std::chrono::steady_clock::now();
    static u64 printed_hoisted = 0, printed_windows = 0, printed_spliced = 0;
    static u64 printed_valve = 0, printed_cap = 0, printed_normal = 0;
    static u64 printed_fallbacks[HoistFallbackCount]{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) {
        return;
    }
    std::scoped_lock lk{mutex};
    if (now - last < std::chrono::seconds(5)) {
        return;
    }
    const u64 hoisted = hoisted_commands.load(std::memory_order_relaxed);
    const u64 windows = hoist_windows.load(std::memory_order_relaxed);
    const u64 spliced = hoist_windows_spliced.load(std::memory_order_relaxed);
    u64 fallbacks[HoistFallbackCount];
    for (u32 i = 0; i < HoistFallbackCount; ++i) {
        fallbacks[i] = hoist_fallbacks[i].load(std::memory_order_relaxed);
    }
    std::printf("Copy hoisting (%.0f s): %llu copies hoisted, %llu windows spliced of %llu opened; "
                "not hoisted: %llu dst read by pass, %llu src written by pass, %llu window "
                "closed, %llu not deferred, %llu too many ranges\n",
                std::chrono::duration<double>(now - last).count(),
                static_cast<unsigned long long>(hoisted - printed_hoisted),
                static_cast<unsigned long long>(spliced - printed_spliced),
                static_cast<unsigned long long>(windows - printed_windows),
                static_cast<unsigned long long>(fallbacks[HoistDstReadByPass] -
                                                printed_fallbacks[HoistDstReadByPass]),
                static_cast<unsigned long long>(fallbacks[HoistSrcWrittenByPass] -
                                                printed_fallbacks[HoistSrcWrittenByPass]),
                static_cast<unsigned long long>(fallbacks[HoistWindowClosed] -
                                                printed_fallbacks[HoistWindowClosed]),
                static_cast<unsigned long long>(fallbacks[HoistNotDeferred] -
                                                printed_fallbacks[HoistNotDeferred]),
                static_cast<unsigned long long>(fallbacks[HoistTooManyRanges] -
                                                printed_fallbacks[HoistTooManyRanges]));
    const u64 valve = hoist_closed_valve.load(std::memory_order_relaxed);
    const u64 cap = hoist_closed_cap.load(std::memory_order_relaxed);
    const u64 normal = hoist_closed_normal.load(std::memory_order_relaxed);
    std::printf("Copy hoisting windows closed: %llu normally (pass end, submission, cut, wait), "
                "%llu by the idle valve (held signals), %llu by the time cap\n",
                static_cast<unsigned long long>(normal - printed_normal),
                static_cast<unsigned long long>(valve - printed_valve),
                static_cast<unsigned long long>(cap - printed_cap));
    printed_valve = valve;
    printed_cap = cap;
    printed_normal = normal;
    printed_hoisted = hoisted;
    printed_windows = windows;
    printed_spliced = spliced;
    for (u32 i = 0; i < HoistFallbackCount; ++i) {
        printed_fallbacks[i] = fallbacks[i];
    }
    last = now;
}

// BB_HOIST_TRACE=1 (diagnostics): who closes a hoist window while its pass still runs, and why
// windows do not open. Call stacks as library offsets, every 5 s.
bool HoistTraceEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_HOIST_TRACE");
        return env && env[0] == '1';
    }();
    return enabled;
}

void TraceHoist(const char* what) {
    static std::mutex mutex;
    static std::unordered_map<std::string, u64> stacks;
    static auto last = std::chrono::steady_clock::now();
    std::string key = what;
    if (what[0] == 'c') { // "closed early": the call stack
        void* frames[8];
        const int count = bb_backtrace(frames, 8);
        for (int i = 2; i < count; ++i) {
            Dl_info info{};
            dladdr(frames[i], &info);
            const char* name = info.dli_fname ? std::strrchr(info.dli_fname, '/') : nullptr;
            char part[96];
            std::snprintf(part, sizeof(part), " %s+0x%lx", name ? name + 1 : "?",
                          static_cast<unsigned long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                     reinterpret_cast<uintptr_t>(info.dli_fbase)));
            key += part;
        }
    }
    std::scoped_lock lk{mutex};
    ++stacks[key];
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) {
        return;
    }
    last = now;
    std::vector<std::pair<u64, std::string>> top;
    for (const auto& [stack, n] : stacks) {
        top.emplace_back(n, stack);
    }
    std::ranges::sort(top, std::greater{});
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
        std::printf("Hoist trace: %llu x %s\n", static_cast<unsigned long long>(top[i].first),
                    top[i].second.c_str());
    }
    stacks.clear();
}

void CountFallback(HoistFallback reason) {
    if (HoistStatsEnabled()) {
        hoist_fallbacks[reason].fetch_add(1, std::memory_order_relaxed);
    }
}

/// One barrier, recorded inside the prepass: `src` stages and writes to `dst` stages and accesses.
vk::MemoryBarrier2 PrepassBarrier(vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                  vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    return {
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
    };
}

constexpr vk::PipelineStageFlags2 CopyStages =
    vk::PipelineStageFlagBits2::eAllTransfer | vk::PipelineStageFlagBits2::eComputeShader;
constexpr vk::AccessFlags2 CopyAccess =
    vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite |
    vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite;
constexpr vk::AccessFlags2 CopyWrites =
    vk::AccessFlagBits2::eTransferWrite | vk::AccessFlagBits2::eShaderStorageWrite;
constexpr vk::AccessFlags2 AllAccess =
    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;

} // namespace

bool Scheduler::HoistRanges::Overlaps(const void* buffer, u64 start, u64 end) const noexcept {
    for (const Range& range : tail) {
        if (range.buffer == buffer && range.start < end && start < range.end) {
            return true;
        }
    }
    // The first range after the query's start, and the one before it (which may reach over it).
    const Range key{buffer, start, start};
    const auto next = std::ranges::upper_bound(merged, key, Before);
    if (next != merged.end() && next->buffer == buffer && next->start < end) {
        return true;
    }
    if (next != merged.begin()) {
        const Range& before = *(next - 1);
        return before.buffer == buffer && before.end > start;
    }
    return false;
}

bool Scheduler::HoistRanges::Add(const void* buffer, u64 start, u64 end) {
    // Neighbours of one binding stream (ring allocations, a draw's buffers in turn) merge.
    const size_t first = tail.size() > 4 ? tail.size() - 4 : 0;
    for (size_t i = tail.size(); i-- > first;) {
        Range& range = tail[i];
        if (range.buffer == buffer && range.start <= end && start <= range.end) {
            range.start = std::min(range.start, start);
            range.end = std::max(range.end, end);
            return true;
        }
    }
    tail.push_back({buffer, start, end});
    if (tail.size() >= 256) {
        MergeTail();
        return merged.size() <= MaxRanges;
    }
    return true;
}

void Scheduler::HoistRanges::MergeTail() {
    std::ranges::sort(tail, Before);
    std::vector<Range> all(merged.size() + tail.size());
    std::ranges::merge(merged, tail, all.begin(), Before);
    size_t out = 0;
    for (const Range& range : all) {
        if (out != 0 && all[out - 1].buffer == range.buffer && range.start <= all[out - 1].end) {
            all[out - 1].end = std::max(all[out - 1].end, range.end);
        } else {
            all[out++] = range;
        }
    }
    all.resize(out);
    merged = std::move(all);
    tail.clear();
}

void Scheduler::NoteDrawRange(const void* buffer, u64 start, u64 end, bool write) {
    if (pass_overflow) {
        return;
    }
    if (!(write ? pass_writes : pass_reads).Add(buffer, start, end)) {
        pass_overflow = true;
    }
}

void Scheduler::OpenHoistWindow() {
    if (!IsRecordingDeferred() || carried_scope || !HoistEnabled()) {
        if (HoistTraceEnabled()) {
            TraceHoist(!IsRecordingDeferred() ? "not opened: not deferred"
                       : carried_scope        ? "not opened: carried scope (occlusion query)"
                                              : "not opened: off");
        }
        return;
    }
    // The pass starts at a chunk boundary: the commands of the draw so far (its state) stay in
    // front, the pass's own chunks follow, and the prepass goes between them.
    if (!CurrentChunk().Empty()) {
        RetireChunk();
    }
    hoist_open = true;
    hoist_split = full_chunks.size();
    hoist_bytes0 = segment_bytes;
    prepass_started = false;
    pass_global_read = pass_global_write = pass_overflow = false;
    pass_reads.Clear();
    pass_writes.Clear();
    prepass_reads.Clear();
    prepass_writes.Clear();
    if (HoistStatsEnabled()) {
        hoist_windows.fetch_add(1, std::memory_order_relaxed);
    }
}

void Scheduler::CloseHoistWindow() {
    if (!hoist_open) {
        return;
    }
    if (HoistTraceEnabled() && is_rendering) {
        TraceHoist("closed early:");
    } else if (HoistTraceEnabled() && resume_rendering) {
        TraceHoist("cut: segment split inside the pass");
    }
    hoist_open = false;
    if (HoistStatsEnabled()) {
        (hoist_close_reason == HoistClose::IdleValve ? hoist_closed_valve
         : hoist_close_reason == HoistClose::TimeCap ? hoist_closed_cap
                                                      : hoist_closed_normal)
            .fetch_add(1, std::memory_order_relaxed);
    }
    hoist_close_reason = HoistClose::Normal;
    if (prepass_started) {
        // The hoisted commands' writes are visible to everything after them.
        RecordHoisted([barrier = PrepassBarrier(CopyStages, CopyWrites,
                                                vk::PipelineStageFlagBits2::eAllCommands,
                                                AllAccess)](vk::CommandBuffer cmdbuf) {
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        });
        prepass_full.push_back(std::move(prepass_chunk));
        prepass_chunk.reset();
        // In front of the pass's first chunk. HandOver counts them with the others (handed,
        // after), so the recording threads see one run of chunks.
        size_t bytes = 0;
        for (const auto& chunk : prepass_full) {
            bytes += chunk->Size();
        }
        segment_bytes += bytes;
        const auto at = full_chunks.begin() + std::min(hoist_split, full_chunks.size());
        full_chunks.insert(at, std::make_move_iterator(prepass_full.begin()),
                           std::make_move_iterator(prepass_full.end()));
        prepass_full.clear();
        prepass_started = false;
        if (HoistStatsEnabled()) {
            hoist_windows_spliced.fetch_add(1, std::memory_order_relaxed);
        }
    }
    pass_reads.Clear();
    pass_writes.Clear();
    prepass_reads.Clear();
    prepass_writes.Clear();
    if (HoistStatsEnabled()) {
        PrintHoistStats();
    }
}

bool Scheduler::TryHoist(std::span<const HoistAccess> reads, std::span<const HoistAccess> writes) {
    // Outside a pass nothing ends (and the copy is recorded in stream order as it always was).
    if (!is_rendering || !HoistEnabled()) {
        return false;
    }
    if (!IsRecordingDeferred()) {
        CountFallback(HoistNotDeferred);
        return false;
    }
    if (!hoist_open) {
        CountFallback(HoistWindowClosed);
        return false;
    }
    ProducerScope producer{*this, "TryHoist"};
    if (pass_overflow) {
        CountFallback(HoistTooManyRanges);
        return false;
    }
    // The copy moves in front of the draws already in the pass: it must not change what they
    // read or write (its destination), nor read what they write (its source).
    for (const HoistAccess& write : writes) {
        if (pass_global_read || pass_global_write ||
            pass_reads.Overlaps(write.buffer, write.start, write.end) ||
            pass_writes.Overlaps(write.buffer, write.start, write.end)) {
            CountFallback(HoistDstReadByPass);
            return false;
        }
    }
    for (const HoistAccess& read : reads) {
        if (pass_global_write || pass_writes.Overlaps(read.buffer, read.start, read.end)) {
            CountFallback(HoistSrcWrittenByPass);
            return false;
        }
    }
    if (!prepass_started) {
        // Everything before the pass is done with the memory the copies touch.
        prepass_started = true;
        RecordHoisted([barrier = PrepassBarrier(vk::PipelineStageFlagBits2::eAllCommands, AllAccess,
                                                CopyStages, CopyAccess)](vk::CommandBuffer cmdbuf) {
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        });
    }
    // The prepass runs its commands without barriers in between: one that depends on an earlier
    // hoisted one (an upload and the copy of what it uploaded) gets one.
    bool conflict = prepass_reads.Size() + prepass_writes.Size() > 4096;
    for (const HoistAccess& write : writes) {
        conflict = conflict || prepass_reads.Overlaps(write.buffer, write.start, write.end) ||
                   prepass_writes.Overlaps(write.buffer, write.start, write.end);
    }
    for (const HoistAccess& read : reads) {
        conflict = conflict || prepass_writes.Overlaps(read.buffer, read.start, read.end);
    }
    if (conflict) {
        RecordHoisted([barrier = PrepassBarrier(CopyStages, CopyWrites, CopyStages,
                                                CopyAccess)](vk::CommandBuffer cmdbuf) {
            cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
        });
        prepass_reads.Clear();
        prepass_writes.Clear();
    }
    for (const HoistAccess& read : reads) {
        prepass_reads.Add(read.buffer, read.start, read.end);
    }
    for (const HoistAccess& write : writes) {
        prepass_writes.Add(write.buffer, write.start, write.end);
    }
    if (HoistStatsEnabled()) {
        hoisted_commands.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void Scheduler::TraceDirectRecording(void* caller) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_RECORDER_TRACE");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_map<void*, u64> callers;
    static u64 calls;
    std::scoped_lock lk{mutex};
    ++callers[caller];
    if (++calls % 2000) {
        return;
    }
    std::vector<std::pair<u64, void*>> top;
    for (const auto& [address, count] : callers) {
        top.emplace_back(count, address);
    }
    std::ranges::sort(top, std::greater{});
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
        Dl_info info{};
        dladdr(top[i].second, &info);
        std::printf("Recorder sync caller: %llu x %s+0x%lx\n",
                    static_cast<unsigned long long>(top[i].first),
                    info.dli_fname ? info.dli_fname : "?",
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) -
                                               reinterpret_cast<uintptr_t>(info.dli_fbase)));
    }
    callers.clear();
}

std::unique_ptr<RecordChunk> Scheduler::AcquireChunk() {
    std::scoped_lock lk{recorder_mutex};
    if (free_chunks.empty()) {
        return std::make_unique<RecordChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

void Scheduler::SignalAfterHostCopies(std::function<void()> signal) {
    // A direct segment queues it too: signals queued before it must not be overtaken.
    if (!IsRecordingDeferred() && !direct_segment) {
        WaitHostCopies();
        signal();
        return;
    }
    BbCopy::FlushBatch();
    deferred_signals_issued.fetch_add(1, std::memory_order_relaxed);
    RecordOrdered([signal = std::move(signal), done = deferred_signals_done]() mutable {
        BbCopy::AfterCopies([signal = std::move(signal), done = std::move(done)] {
            signal();
            done->fetch_add(1, std::memory_order_release);
        });
    });
    // bbport: an open hoist window keeps its chunks, and the signal stays queued with them (the
    // game's ~44 fences per frame cut every pass otherwise). See ReleaseHeldSignals.
    if (hoist_open && HoistHoldSignals()) {
        if (!held_signals) {
            held_signals = true;
            held_since = std::chrono::steady_clock::now();
        }
        return;
    }
    KickRecording(true);
}

void Scheduler::ReleaseHeldSignals() {
    if (!held_signals) {
        return;
    }
    if (hoist_open) {
        hoist_close_reason = HoistClose::IdleValve;
    }
    KickRecording(true);
}

void Scheduler::WaitDeferredSignals() {
    const u64 issued = deferred_signals_issued.load(std::memory_order_relaxed);
    if (deferred_signals_done->load(std::memory_order_acquire) >= issued ||
        BbToggle::Disabled(BbToggle::OrderedGuestWrites)) {
        return;
    }
    BbStats::WaitTimer timer{BbStats::host_copies_wait_ns};
    KickRecording(true);
    while (deferred_signals_done->load(std::memory_order_acquire) < issued) {
        // Helps the copy threads the signals wait for.
        BbCopy::WaitAsync();
        std::this_thread::yield();
    }
}

namespace {
constexpr u32 HostCopyGranuleShift = 16;
constexpr u64 HostCopyMaxGranules = 64; // larger notes and checks are treated as everywhere
u32 HostCopySlotOf(u64 granule, u32 slots) {
    return u32((granule * 0x9e3779b97f4a7c15ull) >> 40) % slots;
}
} // namespace

void Scheduler::NoteHostCopySource(u64 address, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 first = address >> HostCopyGranuleShift;
    const u64 last = (address + size - 1) >> HostCopyGranuleShift;
    std::scoped_lock lk{host_copy_sources_mutex};
    const u64 seq = ++host_copy_source_seq;
    if (last - first >= HostCopyMaxGranules) {
        host_copy_big_seq = seq;
        return;
    }
    for (u64 g = first; g <= last; ++g) {
        auto& slot = host_copy_slots[HostCopySlotOf(g, HostCopySlots)];
        if (slot.seq > host_copy_sources_done && slot.granule != g) {
            slot.granule = HostCopyWildcard; // two pending granules share it
        } else if (slot.granule != HostCopyWildcard || slot.seq <= host_copy_sources_done) {
            slot.granule = g;
        }
        slot.seq = seq;
    }
}

void Scheduler::WaitHostCopiesFor(u64 address, u64 size) {
    {
        std::scoped_lock lk{host_copy_sources_mutex};
        bool overlap = host_copy_big_seq > host_copy_sources_done;
        const u64 first = address >> HostCopyGranuleShift;
        const u64 last = (address + std::max<u64>(size, 1) - 1) >> HostCopyGranuleShift;
        if (last - first >= HostCopyMaxGranules) {
            overlap = true;
        }
        for (u64 g = first; !overlap && g <= last; ++g) {
            const auto& slot = host_copy_slots[HostCopySlotOf(g, HostCopySlots)];
            overlap = slot.seq > host_copy_sources_done &&
                      (slot.granule == g || slot.granule == HostCopyWildcard);
        }
        if (!overlap) {
            BbStats::host_copy_waits_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    WaitHostCopies();
}

void Scheduler::WaitHostCopies() {
    u64 sources_seen;
    {
        std::scoped_lock lk{host_copy_sources_mutex};
        sources_seen = host_copy_source_seq;
    }
    // Copies noted after this point may still run when the waits below return.
    struct Forget {
        Scheduler& s;
        u64 seen;
        ~Forget() {
            std::scoped_lock lk{s.host_copy_sources_mutex};
            s.host_copy_sources_done = std::max(s.host_copy_sources_done, seen);
        }
    } forget{*this, sources_seen};
    if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
        BbStats::WaitTimer timer{BbStats::host_copies_wait_ns};
        BbStats::host_copy_waits.fetch_add(1, std::memory_order_relaxed);
        KickRecording(true);
        while (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
            std::this_thread::yield();
        }
    }
    BbStats::WaitTimer timer{BbStats::copy_threads_wait_ns};
    BbCopy::WaitAsync();
}

void Scheduler::KickRecording(bool force) {
    ProducerScope producer{*this, "KickRecording"};
    if (workers.empty()) {
        return;
    }
    // Callers kick where nobody holds the raw command buffer: deferral resumes.
    if (direct_segment) {
        LeaveDirectSegment();
    }
    direct_mode = false;
    // bbport: held signals (SignalAfterHostCopies) go out once their window is gone, or when
    // they have waited long enough.
    bool release = false;
    if (held_signals && !force) {
        if (!hoist_open) {
            release = true;
        } else if (std::chrono::steady_clock::now() - held_since >= HoistHoldTime()) {
            release = true;
            hoist_close_reason = HoistClose::TimeCap;
        }
    }
    if (!force) {
        MaybeSplit();
    }
    // bbport: the chunks of a pass with an open hoist window stay here: a copy hoisted later
    // goes in front of the pass's first chunk, which must not have been handed over.
    // A pass that never ends nor is cut (one recording thread) hands over after a while: past
    // the length where MaybeSplit cuts.
    if (!force && !release && hoist_open &&
        segment_bytes - hoist_bytes0 + CurrentChunk().Size() < (HoistSplitMult() + 2) * split_bytes) {
        return;
    }
    // Batches of tens of KiB keep the queue handoff cheap relative to the work it carries.
    if (!force && !release && full_chunks.empty() && CurrentChunk().Size() < 32 * 1024) {
        return;
    }
    HandOver();
}

void Scheduler::MaybeSplit() {
    if (workers.size() < 2 || current_segment + 1 >= MaxSegments || !IsRecordingDeferred() ||
        BbToggle::Disabled(BbToggle::ParallelRecording)) {
        return;
    }
    // Cutting inside a render pass closes and reopens it: only after a longer stretch.
    const size_t bytes = segment_bytes + CurrentChunk().Size();
    // (A pass with a hoist window is cut later: a cut ends the window and reopens the pass.)
    const size_t pass_mult = hoist_open ? HoistSplitMult() : 2;
    if (bytes < (is_rendering ? pass_mult * split_bytes : split_bytes)) {
        return;
    }
    if (is_rendering) {
        CarrySuspend(true);
        Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
        is_rendering = false;
        resume_rendering = true;
    } else {
        CarrySuspend(false);
    }
    if (async_submit) {
        // Its recording thread ends the segment's command buffer (the pool is that thread's).
        Record([](vk::CommandBuffer cmdbuf) { Check(cmdbuf.end()); });
    }
    HandOver();
    ++current_segment;
    batch->segments[current_segment] = vk::CommandBuffer{};
    segment_bytes = 0;
    active_worker.store(current_segment % workers.size(), std::memory_order_relaxed);
    // The next command buffer starts without dynamic state: the next draw sets all of it.
    // Everything else (pipeline, vertex and index buffers, descriptors, push constants) every
    // draw and dispatch records itself.
    dynamic_state.Invalidate();
    CarryResume(false);
}

void Scheduler::HandOver() {
    // Entered from inside Record*() on this thread: a fault handler recording mid-command.
    if (const char* inside = producer_scope_where;
        inside && (std::strcmp(inside, "Record") == 0 || std::strcmp(inside, "RecordData") == 0 ||
                   std::strcmp(inside, "RecordOrdered") == 0 ||
                   std::strcmp(inside, "RetireChunk") == 0)) {
        ReportProducer("REENTERED recording", u32(gettid()), inside, "HandOver");
    }
    ProducerScope producer{*this, "HandOver"};
    // bbport: whatever hands chunks over (a submission, a cut, a wait for the recording threads)
    // ends the hoist window first: the prepass joins the chunks handed over, in front of the
    // pass's.
    CloseHoistWindow();
    held_signals = false; // the ordered chunks below carry them
    if (!record_chunk || !ordered_chunk) {
        // Left behind by another HandOver interrupted between handing a chunk over and
        // taking a new one (see ProducerScope). Recover instead of dereferencing null.
        ReportProducer("HandOver without a current chunk", producer_tid.load(), "?", "HandOver");
        if (!record_chunk) {
            record_chunk = AcquireChunk();
        }
        if (!ordered_chunk) {
            ordered_chunk = AcquireChunk();
        }
    }
    const bool commands = !full_chunks.empty() || !record_chunk->Empty();
    const bool ordered = !ordered_full.empty() || !ordered_chunk->Empty();
    if (!commands && !ordered) {
        return;
    }
    // The chunks that take the current ones' places, taken first (AcquireChunk locks
    // recorder_mutex) and swapped in below: record_chunk and ordered_chunk are never null, not
    // even for a fault handler recording on this thread meanwhile (issue #100).
    std::unique_ptr<RecordChunk> next_record = record_chunk->Empty() ? nullptr : AcquireChunk();
    std::unique_ptr<RecordChunk> next_ordered = ordered_chunk->Empty() ? nullptr : AcquireChunk();
    Worker& worker = *workers[current_segment % workers.size()];
    bool wake = false;
    {
        std::scoped_lock lk{recorder_mutex};
        if (commands) {
            for (auto& chunk : full_chunks) {
                chunk->segment = current_segment;
                chunk->batch = batch;
                worker.queue.push_back(std::move(chunk));
                ++batch->handed[current_segment];
            }
            if (next_record) {
                record_chunk.swap(next_record); // next_record: the chunk handed over
                segment_bytes += next_record->Size();
                next_record->segment = current_segment;
                next_record->batch = batch;
                worker.queue.push_back(std::move(next_record));
                ++batch->handed[current_segment];
            }
            worker.queued.store(worker.queue.size(), std::memory_order_release);
            wake = worker.sleeping;
        }
        if (ordered) {
            // They run after the commands handed over with them (recorded before or among them).
            const auto queue = [&](std::unique_ptr<RecordChunk> chunk) {
                chunk->segment = current_segment;
                chunk->batch = batch;
                chunk->after = batch->handed[current_segment];
                ordered_queue.push_back(std::move(chunk));
            };
            for (auto& chunk : ordered_full) {
                queue(std::move(chunk));
            }
            if (next_ordered) {
                ordered_chunk.swap(next_ordered); // next_ordered: the chunk handed over
                queue(std::move(next_ordered));
            }
            ordered_queued.store(ordered_queue.size(), std::memory_order_release);
            // Any thread may run them; the current segment's thread is woken for them (when it
            // sleeps): it spins for work anyway, and waking another thread per small copy cost
            // a few percent of the frame rate on 4 cores / 8 threads. A thread that is awake
            // picks them up by itself.
            if (!ordered_running) {
                wake |= worker.sleeping;
            }
        }
    }
    full_chunks.clear();
    ordered_full.clear();
    // A busy thread picks the new chunks up by itself: waking it is a syscall per draw.
    if (wake) {
        worker.cv.notify_one();
    }
}

bool Scheduler::RecordingIdle() const {
    if (!ordered_queue.empty() || ordered_running) {
        return false;
    }
    for (const auto& worker : workers) {
        if (!worker->queue.empty() || worker->busy) {
            return false;
        }
    }
    return true;
}

bool Scheduler::OrderedReady() const {
    if (ordered_queue.empty() || ordered_running) {
        return false;
    }
    const RecordChunk& next = *ordered_queue.front();
    for (u32 i = 0; i < next.segment; ++i) {
        if (next.batch->recorded[i] != next.batch->handed[i]) {
            return false;
        }
    }
    return next.batch->recorded[next.segment] >= next.after;
}

void Scheduler::SyncRecording() {
    if (workers.empty()) {
        return;
    }
    KickRecording(true);
    BbStats::WaitTimer timer{BbStats::sync_recording_ns};
    std::unique_lock lk{recorder_mutex};
    recorder_idle_cv.wait(lk, [this] { return RecordingIdle(); });
}

void Scheduler::EnterDirectMode() {
    // bbport: with submissions going out from the recording threads, the commands recorded
    // here get a segment of their own, from this thread's pool, after the segments handed over:
    // the recording threads go on with those instead of this thread waiting for them (~90 us,
    // the upscaler's passes every frame). BB_DIRECT_SEGMENT=0: the wait.
    static const bool own_segment = [] {
        const char* env = std::getenv("BB_DIRECT_SEGMENT");
        return !(env && env[0] == '0');
    }();
    if (own_segment && async_submit && current_segment + 2 < MaxSegments &&
        !BbToggle::Disabled(BbToggle::ThreadedRecording)) {
        CloseHoistWindow();
        if (is_rendering) {
            CarrySuspend(true);
            Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRendering(); });
            is_rendering = false;
            resume_rendering = true;
        } else {
            CarrySuspend(false);
        }
        // A segment nothing was handed to yet is not begun: it becomes the direct one.
        const bool untouched =
            batch->handed[current_segment] == 0 && full_chunks.empty() && CurrentChunk().Empty();
        if (!untouched) {
            // Its recording thread ends the segment's command buffer (the pool is that thread's).
            Record([](vk::CommandBuffer cmdbuf) { Check(cmdbuf.end()); });
            HandOver();
            ++current_segment;
        }
        if (!direct_pool) {
            direct_pool = std::make_unique<CommandPool>(instance, &work_semaphore);
        }
        current_cmdbuf = BeginCommandBuffer(*direct_pool);
        batch->segments[current_segment] = current_cmdbuf;
        segment_bytes = 0;
        direct_mode = true;
        direct_segment = true;
        dynamic_state.Invalidate();
        CarryResume(false);
        return;
    }
    SyncRecording();
    direct_mode = true;
    // The recording threads are idle: this thread may use the segment's command pool.
    auto& cmdbuf = batch->segments[current_segment];
    if (!cmdbuf) {
        cmdbuf = BeginCommandBuffer(*workers[current_segment % workers.size()]->pool);
    }
    current_cmdbuf = cmdbuf;
}

void Scheduler::LeaveDirectSegment() {
    if (is_rendering) {
        CarrySuspend(true);
        current_cmdbuf.endRendering();
        is_rendering = false;
        resume_rendering = true;
    } else {
        CarrySuspend(false);
    }
    Check(current_cmdbuf.end());
    current_cmdbuf = vk::CommandBuffer{};
    direct_mode = false;
    direct_segment = false;
    ++current_segment;
    batch->segments[current_segment] = vk::CommandBuffer{};
    segment_bytes = 0;
    active_worker.store(current_segment % workers.size(), std::memory_order_relaxed);
    dynamic_state.Invalidate();
    CarryResume(false);
}

void Scheduler::RecorderThread(std::stop_token stoken, u32 index) {
    if (index == 0) {
        Common::SetCurrentThreadName("bb:VkRecorder");
    } else {
        char name[16];
        std::snprintf(name, sizeof(name), "bb:VkRecorder%u", index);
        Common::SetCurrentThreadName(name);
    }
    Worker& worker = *workers[index];
    std::vector<std::unique_ptr<RecordChunk>> ordered;
    const auto has_work = [&] {
        return !worker.queue.empty() || OrderedReady();
    };
    while (true) {
        // The thread of the segment being recorded spins briefly before sleeping: its next
        // chunk usually follows within microseconds, and a sleeping thread costs the producer
        // a wake-up syscall per kick. With few hardware threads (Steam Deck: 8) the spin would
        // take time from guest threads. The others sleep until a new segment reaches them.
        if (active_worker.load(std::memory_order_relaxed) == index) {
            static const auto spin_time =
                std::chrono::microseconds(BbThreads::Available() >= 12 ? 200 : 20);
            const auto spin_until = std::chrono::steady_clock::now() + spin_time;
            for (u32 spins = 1; worker.queued.load(std::memory_order_acquire) == 0 &&
                                ordered_queued.load(std::memory_order_acquire) == 0;
                 ++spins) {
                BbCpu::Pause();
                // The clock is read every 256 pauses, not per iteration.
                if (!(spins & 255) &&
                    (stoken.stop_requested() || std::chrono::steady_clock::now() >= spin_until)) {
                    break;
                }
            }
        }
        std::unique_ptr<RecordChunk> chunk;
        {
            std::unique_lock lk{recorder_mutex};
            if (!has_work()) {
                worker.sleeping = true;
                worker.cv.wait(lk, stoken, has_work);
                worker.sleeping = false;
                if (!has_work()) {
                    return; // stop requested
                }
            }
            if (OrderedReady()) {
                // Every chunk whose commands are recorded, in order.
                do {
                    ordered.push_back(std::move(ordered_queue.front()));
                    ordered_queue.pop_front();
                } while (OrderedReady());
                ordered_running = true;
                ordered_queued.store(ordered_queue.size(), std::memory_order_release);
            } else {
                chunk = std::move(worker.queue.front());
                worker.queue.pop_front();
                worker.queued.store(worker.queue.size(), std::memory_order_release);
                worker.busy = true;
            }
        }
        if (!ordered.empty()) {
            for (auto& task : ordered) {
                task->Execute({});
            }
            std::scoped_lock lk{recorder_mutex};
            for (auto& task : ordered) {
                free_chunks.push_back(std::move(task));
            }
            ordered.clear();
            ordered_running = false;
            recorder_idle_cv.notify_all();
            continue;
        }
        // The segment's command buffer is begun by the first chunk; it is ended at the
        // submission, after SyncRecording(). Only this thread uses the pool meanwhile.
        auto& cmdbuf = chunk->batch->segments[chunk->segment];
        if (!cmdbuf) {
            cmdbuf = BeginCommandBuffer(*worker.pool);
        }
        chunk->Execute(cmdbuf);
        {
            std::scoped_lock lk{recorder_mutex};
            ++chunk->batch->recorded[chunk->segment];
            free_chunks.push_back(std::move(chunk));
            worker.busy = false;
            if (worker.queue.empty()) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    Wait(presubmit_tick);
}

namespace {
/// The last submissions of every scheduler (they share the queue): reported by a stalled wait.
struct SubmissionTrace {
    const Scheduler* scheduler = nullptr;
    u64 tick = 0;
    vk::Result result{};
    u32 num_waits = 0;
    std::array<vk::Semaphore, 4> waits{};
    std::array<u64, 4> wait_ticks{};
    u32 stream = 0;       ///< breadcrumb stream of the scheduler
    u32 num_signals = 0;
    double ms = 0;        ///< since the first submission
};
std::array<SubmissionTrace, 32> submission_trace{};
u32 submission_trace_next = 0;
std::mutex submission_trace_mutex;
} // namespace

void Scheduler::Wait(u64 tick) {
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    BbWaitTrace::Scope trace;
    BbStats::WaitTimer timer{BbStats::tick_wait_ns};
    // bbport: a wait that takes seconds is a stuck GPU or submission: say what is outstanding.
    if (!work_semaphore.WaitFor(tick, 5'000'000'000ull)) {
        char thread[16] = {};
        pthread_getname_np(pthread_self(), thread, sizeof(thread));
        std::printf("GPU wait stalled 5 s on %s: tick %llu, GPU at %llu, submitted %llu, next "
                    "%llu\n",
                    thread, static_cast<unsigned long long>(tick),
                    static_cast<unsigned long long>(work_semaphore.KnownGpuTick()),
                    static_cast<unsigned long long>(submitted_tick.load()),
                    static_cast<unsigned long long>(work_semaphore.CurrentTick()));
        {
            std::scoped_lock lk{submission_trace_mutex};
            for (u32 n = 0; n < submission_trace.size(); ++n) {
                const auto& trace =
                    submission_trace[(submission_trace_next + n) % submission_trace.size()];
                if (trace.tick == 0) {
                    continue;
                }
                const bool own = trace.scheduler == this;
                if (own && trace.tick < work_semaphore.KnownGpuTick()) {
                    continue;
                }
                // The other scheduler's timeline, read now.
                const u64 done = own ? work_semaphore.KnownGpuTick()
                                     : instance.GetDevice()
                                           .getSemaphoreCounterValue(
                                               trace.scheduler->work_semaphore.Handle())
                                           .value;
                std::printf("  %s submission %llu (%s, its timeline at %llu) waits:",
                            own ? "this" : "other", static_cast<unsigned long long>(trace.tick),
                            vk::to_string(trace.result).c_str(),
                            static_cast<unsigned long long>(done));
                for (u32 i = 0; i < trace.num_waits; ++i) {
                    u64 value = 0;
                    // Binary semaphores (tick 1) cannot be queried.
                    const bool timeline = trace.wait_ticks[i] > 1;
                    if (timeline) {
                        value = instance.GetDevice().getSemaphoreCounterValue(trace.waits[i]).value;
                    }
                    std::printf(" %p@%llu (at %s)", static_cast<void*>(VkSemaphore(trace.waits[i])),
                                static_cast<unsigned long long>(trace.wait_ticks[i]),
                                timeline ? std::to_string(value).c_str() : "binary");
                }
                std::printf("\n");
            }
        }
        std::fflush(stdout);
        Breadcrumbs::ReportStuck("a wait stalled 5 s");
        work_semaphore.Wait(tick);
    }
}


void Scheduler::NoteSubmission(u64 tick, const SubmitInfo& info, vk::Result result) {
    if (result != vk::Result::eSuccess) {
        std::printf("GPU submission %llu refused by the driver: %s\n",
                    static_cast<unsigned long long>(tick), vk::to_string(result).c_str());
    }
    std::scoped_lock lk{submission_trace_mutex};
    auto& trace = submission_trace[submission_trace_next++ % submission_trace.size()];
    trace.scheduler = this;
    trace.tick = tick;
    trace.result = result;
    trace.stream = crumb_stream;
    trace.num_signals = info.num_signal_semas;
    static const auto start = std::chrono::steady_clock::now();
    trace.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // Debug build for issue #15: BB_SUBMIT_TRACE=1 prints the first 400 submissions as they go out.
    static const bool print = [] {
        const char* env = std::getenv("BB_SUBMIT_TRACE");
        return env && env[0] == '1';
    }();
    static u32 printed = 0;
    if (print && printed < 400) {
        ++printed;
        std::printf("GPU submit #%u: stream %u tick %llu at %.1f ms (%s), %u waits, %u signals\n", printed,
                    trace.stream, static_cast<unsigned long long>(tick), trace.ms,
                    vk::to_string(result).c_str(), info.num_wait_semas, info.num_signal_semas);
        std::fflush(stdout);
    }
    trace.num_waits = std::min<u32>(info.num_wait_semas, u32(trace.waits.size()));
    for (u32 i = 0; i < trace.num_waits; ++i) {
        trace.waits[i] = info.wait_semas[i];
        trace.wait_ticks[i] = info.wait_ticks[i];
    }
}

/// Debug build for issue #15: the last submissions of all schedulers, printed at device lost (no Vulkan calls:
/// the device may be gone).
void PrintSubmissionTrace() {
    std::scoped_lock lk{submission_trace_mutex};
    std::printf("GPU last submissions (oldest first):\n");
    for (u32 n = 0; n < submission_trace.size(); ++n) {
        const auto& trace = submission_trace[(submission_trace_next + n) % submission_trace.size()];
        if (trace.tick == 0) {
            continue;
        }
        std::printf("  stream %u tick %llu at %.1f ms (%s), %u waits, %u signals:", trace.stream,
                    static_cast<unsigned long long>(trace.tick), trace.ms,
                    vk::to_string(trace.result).c_str(), trace.num_waits, trace.num_signals);
        for (u32 i = 0; i < trace.num_waits; ++i) {
            std::printf(" %p@%llu", static_cast<void*>(VkSemaphore(trace.waits[i])),
                        static_cast<unsigned long long>(trace.wait_ticks[i]));
        }
        std::printf("\n");
    }
    std::fflush(stdout);
}

void Scheduler::PopPendingOperations() {
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return; // every draw comes here
    }
    std::unique_lock lk(pending_ops_mutex);
    // bbport: this runs on every draw and dispatch. Querying the timeline semaphore is an
    // ioctl, so it is skipped when nothing waits and done once per 32 calls (~0.3 ms; reading
    // the clock per draw instead was itself a hot spot).
    if (pending_ops.empty()) {
        return;
    }
    if (!work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        if ((++pending_polls & 31) != 0 && !BbToggle::Disabled(BbToggle::PendingPollLimit)) {
            return;
        }
        work_semaphore.Refresh();
    }
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
}

SubmissionBatch* Scheduler::AcquireBatch() {
    std::scoped_lock lk{recorder_mutex};
    SubmissionBatch* next;
    if (free_batches.empty()) {
        next = all_batches.emplace_back(std::make_unique<SubmissionBatch>()).get();
    } else {
        next = free_batches.back();
        free_batches.pop_back();
    }
    next->Reset();
    return next;
}

void Scheduler::AllocateWorkerCommandBuffers() {
    if (async_submit) {
        // BB_ASYNC_SUBMIT: the earlier submissions may still be recorded (and their pools
        // used) on the recording threads: segment 0 is begun there too, on its first chunk,
        // or by EnterDirectMode once they are idle.
        batch = AcquireBatch();
        current_cmdbuf = vk::CommandBuffer{};
        direct_mode = false;
    } else {
        const vk::CommandBufferBeginInfo begin_info = {
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        };
        current_cmdbuf = command_pool.Commit();
        Check(current_cmdbuf.begin(begin_info));
        // bbport: the first segment (worker 0, whose pool this is) of the next submission.
        if (!batch) {
            batch = AcquireBatch();
        }
        batch->Reset();
        batch->segments[0] = current_cmdbuf;
    }
    current_segment = 0;
    segment_bytes = 0;
    active_worker.store(0, std::memory_order_relaxed);

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    ProducerScope producer{*this, "SubmitExecution"};
    if (async_submit) {
        SubmitAsync(info);
        return;
    }
    std::scoped_lock lk{submit_mutex};
    const u64 signal_value = work_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, batch->segments[0]);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    EndRendering();
    CarrySuspend(false);
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        // Until the next submission's first timestamp: mostly the GPU waiting for it.
        profiler->Mark(0x5B317ull, [] { return std::string{"(between submissions: GPU idle)"}; });
    }
    SyncRecording();
    // Guest memory copies into staging read by this submission (copy threads).
    WaitHostCopies();
    resume_rendering = false;
    // bbport: the segments' command buffers in stream order (the recording threads are idle).
    std::array<vk::CommandBuffer, MaxSegments> cmdbufs;
    u32 num_cmdbufs = 0;
    if (workers.empty()) {
        cmdbufs[num_cmdbufs++] = current_cmdbuf;
    } else {
        for (u32 i = 0; i <= current_segment; ++i) {
            if (batch->segments[i]) {
                cmdbufs[num_cmdbufs++] = batch->segments[i];
            }
        }
        recorded_segments.fetch_add(num_cmdbufs, std::memory_order_relaxed);
        recorded_submissions.fetch_add(1, std::memory_order_relaxed);
    }
    for (u32 i = 0; i < num_cmdbufs; ++i) {
        Check(cmdbufs[i].end());
    }

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    // Four like SubmitAsync: SubmitInfo takes up to four waits (the third and fourth are the extra
    // binary semaphores of the presenter, e.g. Lossless Scaling's batch-complete).
    static constexpr std::array<vk::PipelineStageFlags, 4> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eAllCommands,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = num_cmdbufs,
        .pCommandBuffers = cmdbufs.data(),
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    for (auto& operation : info.before_submit) {
        operation();
    }
    ImGui::Core::TextureManager::Submit();
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    NoteSubmission(signal_value, info, submit_result);
    if (submit_result == vk::Result::eErrorDeviceLost) {
        Breadcrumbs::ReportDeviceLost("submit");
    }
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    submitted_tick.store(signal_value, std::memory_order_release);
    submitted_tick.notify_all();

    // bbport: no semaphore query here (an ioctl per submission, ~3% of the recording thread):
    // PopPendingOperations asks when an operation waits, the GPU signal thread on every fence.
    AllocateWorkerCommandBuffers();
    CarryResume(false);

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::SubmitAsync(SubmitInfo& info) {
    const u64 signal_value = work_semaphore.NextTick();
    if (on_submit) {
        on_submit(info);
    }
    EndRendering();
    CarrySuspend(false);
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        profiler->Mark(0x5B317ull, [] { return std::string{"(between submissions: GPU idle)"}; });
    }
    resume_rendering = false;
    // Every segment's command buffer is ended by its recording thread (the pool is its own);
    // MaybeSplit ended the earlier ones. A direct segment is this thread's.
    if (direct_segment) {
        Check(current_cmdbuf.end());
        current_cmdbuf = vk::CommandBuffer{};
        direct_mode = false;
        direct_segment = false;
    } else {
        Record([](vk::CommandBuffer cmdbuf) { Check(cmdbuf.end()); });
    }
    // Guest memory copies into staging this submission reads: on the copy threads by then.
    BbCopy::FlushBatch();
    info.AddSignal(work_semaphore.Handle(), signal_value);
    // Goes out once the commands handed over before it are recorded (an ordered task: in order
    // with the other submissions and after the copies and signals queued before it).
    RecordOrdered([this, submitted = batch, info = std::move(info), signal_value]() mutable {
        BbCopy::WaitAsync();
        std::array<vk::CommandBuffer, MaxSegments> cmdbufs;
        u32 num_cmdbufs = 0;
        for (u32 i = 0; i < MaxSegments; ++i) {
            if (submitted->segments[i]) {
                cmdbufs[num_cmdbufs++] = submitted->segments[i];
            }
        }
        recorded_segments.fetch_add(num_cmdbufs, std::memory_order_relaxed);
        recorded_submissions.fetch_add(1, std::memory_order_relaxed);
        static constexpr std::array<vk::PipelineStageFlags, 4> wait_stage_masks = {
            vk::PipelineStageFlagBits::eAllCommands,
            vk::PipelineStageFlagBits::eColorAttachmentOutput,
            vk::PipelineStageFlagBits::eAllCommands,
            vk::PipelineStageFlagBits::eAllCommands,
        };
        const vk::TimelineSemaphoreSubmitInfo timeline_si = {
            .waitSemaphoreValueCount = info.num_wait_semas,
            .pWaitSemaphoreValues = info.wait_ticks.data(),
            .signalSemaphoreValueCount = info.num_signal_semas,
            .pSignalSemaphoreValues = info.signal_ticks.data(),
        };
        const vk::SubmitInfo submit_info = {
            .pNext = &timeline_si,
            .waitSemaphoreCount = info.num_wait_semas,
            .pWaitSemaphores = info.wait_semas.data(),
            .pWaitDstStageMask = wait_stage_masks.data(),
            .commandBufferCount = num_cmdbufs,
            .pCommandBuffers = cmdbufs.data(),
            .signalSemaphoreCount = info.num_signal_semas,
            .pSignalSemaphores = info.signal_semas.data(),
        };
        {
            std::scoped_lock lk{submit_mutex};
            for (auto& operation : info.before_submit) {
                operation();
            }
            const auto result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
            NoteSubmission(signal_value, info, result);
            if (result == vk::Result::eErrorDeviceLost) {
                Breadcrumbs::ReportDeviceLost("submit");
            }
            ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost during submit");
        }
        submitted_tick.store(signal_value, std::memory_order_release);
        submitted_tick.notify_all();
        std::scoped_lock lk{recorder_mutex};
        free_batches.push_back(submitted);
    });
    KickRecording(true);
    AllocateWorkerCommandBuffers();
    CarryResume(false);
    PopPendingOperations();
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    // A null command buffer only updates the dirty flags, exactly as recording would.
    CommitWith(instance.IsDepthBoundsSupported(), instance.IsDynamicColorWriteMaskSupported(),
               instance.IsAttachmentFeedbackLoopLayoutSupported(), [&](auto&& command) {
                   if (cmdbuf) {
                       command(cmdbuf);
                   }
               });
}

} // namespace Vulkan
