// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_occlusion.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "bblayer_write_traps.h"
#include "bbport_toggles.h"

#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/host_shaders/occlusion_resolve_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {
struct Push {
    u32 count;
    u32 events;
};

struct EventEntry {
    u32 address_lo;
    u32 address_hi;
    u32 pairs;
    u32 upto;
};

/// Everything before it before everything after it (resolves are a few per frame).
void FullBarrier(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eMemoryRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}
} // namespace

OcclusionQueries::OcclusionQueries(const Instance& instance_, Scheduler& scheduler_,
                                   VideoCore::BufferCache& buffer_cache_)
    : instance{instance_}, scheduler{scheduler_}, buffer_cache{buffer_cache_} {
    const auto device = instance.GetDevice();
    pool = Check(device.createQueryPoolUnique({
        .queryType = vk::QueryType::eOcclusion,
        .queryCount = NumSlots,
    }));
    results = std::make_unique<VideoCore::Buffer>(instance, 0, NumSlots * sizeof(u64),
                                                  VideoCore::MemoryType::DeviceLocal,
                                                  "Occlusion results");
    total = std::make_unique<VideoCore::Buffer>(instance, 0, 256, VideoCore::MemoryType::DeviceLocal,
                                                "Occlusion total");
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = vk::DescriptorType::eStorageBuffer,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(Push)};
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto module = CompileSPV(OCCLUSION_RESOLVE_COMP, device);
    pipeline = Check(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(module);
    std::printf("GPU: the game's occlusion queries are Vulkan occlusion queries (%s counts)\n",
                instance.IsOcclusionQueryPrecise() ? "exact" : "approximate");
}

OcclusionQueries::~OcclusionQueries() {
    scheduler.SetCarriedScope(nullptr);
}

void OcclusionQueries::BeginSegment(bool inside_render_pass) {
    if (begun == reset_until) {
        if (inside_render_pass) {
            ++starved; // no reset can be recorded in a render pass: these samples go uncounted
            return;
        }
        ResetAhead();
    }
    const u32 slot = static_cast<u32>(begun % NumSlots);
    const bool precise = instance.IsOcclusionQueryPrecise();
    scheduler.Record([pool = *pool, slot, precise](vk::CommandBuffer cmdbuf) {
        cmdbuf.beginQuery(pool, slot,
                          precise ? vk::QueryControlFlagBits::ePrecise : vk::QueryControlFlags{});
    });
    ++begun;
    active = true;
}

void OcclusionQueries::EndSegment() {
    const u32 slot = static_cast<u32>((begun - 1) % NumSlots);
    scheduler.Record([pool = *pool, slot](vk::CommandBuffer cmdbuf) { cmdbuf.endQuery(pool, slot); });
    active = false;
}

void OcclusionQueries::ResetAhead() {
    // Slots from `resolved` on hold results not copied yet; those below `begun` were used.
    reset_until = std::max(reset_until, begun);
    const u64 target = std::min<u64>(begun + ResetBatch, resolved + NumSlots);
    if (target <= reset_until || reset_until - begun >= ResetBatch / 2) {
        return;
    }
    const u32 first = static_cast<u32>(reset_until % NumSlots);
    const u32 count = static_cast<u32>(target - reset_until);
    const u32 head = std::min(count, NumSlots - first);
    scheduler.Record([pool = *pool, first, count, head](vk::CommandBuffer cmdbuf) {
        cmdbuf.resetQueryPool(pool, first, head);
        if (count > head) {
            cmdbuf.resetQueryPool(pool, 0, count - head);
        }
    });
    reset_until = target;
}

void OcclusionQueries::Resolve() {
    const u64 ended = active ? begun - 1 : begun;
    if (pending.empty() && ended - resolved < NumSlots / 2) {
        return; // nothing to write, and room left
    }
    const u32 count = static_cast<u32>(ended - resolved);
    const u32 first = static_cast<u32>(resolved % NumSlots);
    std::vector<EventEntry> entries;
    entries.reserve(std::max<size_t>(pending.size(), 1));
    const u64 tick = scheduler.CurrentTick();
    for (const auto& event : pending) {
        entries.push_back({u32(event.address), u32(event.address >> 32), event.pairs,
                           u32(event.upto - resolved)});
        if (BbStats::enabled && event.guest_address != 0) {
            written_at[event.guest_address] = tick;
        }
        if (event.id != 0 && ReadTrace()) {
            NoteResolved(event.guest_address, event.id, tick);
        }
    }
    const u32 num_events = static_cast<u32>(entries.size());
    if (entries.empty()) {
        entries.push_back({}); // a binding to bind
    }
    auto& stream = buffer_cache.GetStreamBuffer();
    const u64 entries_size = entries.size() * sizeof(EventEntry);
    const u64 entries_offset =
        stream.Copy(entries.data(), entries_size, instance.StorageMinAlignment());
    const bool clear_total = !total_cleared;
    total_cleared = true;
    scheduler.Record([this, first, count, num_events, clear_total, stream = stream.Handle(),
                      entries_offset, entries_size](vk::CommandBuffer cmdbuf) {
        if (clear_total) {
            cmdbuf.fillBuffer(total->Handle(), 0, 8, 0);
        }
        const u32 head = std::min(count, NumSlots - first);
        if (head != 0) {
            cmdbuf.copyQueryPoolResults(*pool, first, head, results->Handle(), 0, sizeof(u64),
                                        vk::QueryResultFlagBits::e64 |
                                            vk::QueryResultFlagBits::eWait);
        }
        if (count > head) {
            cmdbuf.copyQueryPoolResults(*pool, 0, count - head, results->Handle(),
                                        head * sizeof(u64), sizeof(u64),
                                        vk::QueryResultFlagBits::e64 |
                                            vk::QueryResultFlagBits::eWait);
        }
        FullBarrier(cmdbuf);
        const std::array<vk::DescriptorBufferInfo, 3> infos{{
            {results->Handle(), 0, NumSlots * sizeof(u64)},
            {total->Handle(), 0, 8},
            {stream, entries_offset, entries_size},
        }};
        std::array<vk::WriteDescriptorSet, 3> writes{};
        for (u32 i = 0; i < writes.size(); ++i) {
            writes[i] = {.dstBinding = i,
                         .descriptorCount = 1,
                         .descriptorType = vk::DescriptorType::eStorageBuffer,
                         .pBufferInfo = &infos[i]};
        }
        const Push push{count, num_events};
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
        cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                             &push);
        cmdbuf.dispatch(1, 1, 1);
        FullBarrier(cmdbuf);
    });
    resolved = ended;
    pending.clear();
}

void OcclusionQueries::WriteBeforeLabel() {
    std::scoped_lock lk{mutex};
    if (pending.empty()) {
        return;
    }
    const bool in_pass = scheduler.IsRendering();
    ++(in_pass ? labels_ahead_in_pass : labels_ahead);
    // A label ahead of results not written yet would let the CPU see the label before them. (Once
    // taken for the cause of slow launches; that was volatile blocks' refresh copies breaking
    // render passes, BufferCache::LayerVolatileAllowed. The game reads its results 2-10 ms after
    // the event and finds them written either way: BB_OCCLUSION_READ_TRACE.)
    static const bool ordered = [] {
        const char* env = std::getenv("BB_OCCLUSION_LABEL_ORDER");
        return !env || env[0] != '0';
    }();
    if (!ordered) {
        return;
    }
    scheduler.EndRendering();
    if (active) {
        EndSegment();
    }
    Resolve();
    ResetAhead();
    BeginSegment(false);
}

void OcclusionQueries::Suspend(bool inside_render_pass) {
    std::scoped_lock lk{mutex};
    if (!started) {
        return;
    }
    if (active) {
        EndSegment();
    }
    if (!inside_render_pass) {
        Resolve();
        ResetAhead();
    }
}

void OcclusionQueries::Resume(bool inside_render_pass) {
    std::scoped_lock lk{mutex};
    if (started && !active) {
        BeginSegment(inside_render_pass);
    }
}

void OcclusionQueries::Event(VAddr address, u32 pairs) {
    std::scoped_lock lk{mutex};
    if (!started) {
        // The first event: from here on a segment is always active (slots reset outside a
        // render pass first).
        scheduler.EndRendering();
        ResetAhead();
        started = true;
        scheduler.SetCarriedScope(this);
    }
    // BB_OCCLUSION_TRACE=N (diagnostics): the first N events, with the frame and the render pass.
    static const u64 trace = [] {
        const char* env = std::getenv("BB_OCCLUSION_TRACE");
        return env ? std::strtoull(env, nullptr, 10) : 0ull;
    }();
    if (events < trace) {
        const auto& state = scheduler.GetRenderState();
        std::printf("Occlusion event %llu: %#llx pairs %u, frame %llu, %s %ux%u (%u colors, "
                    "depth %s), %zu pending\n",
                    (unsigned long long)events, (unsigned long long)address, pairs,
                    (unsigned long long)BbStats::frame_number.load(std::memory_order_relaxed),
                    scheduler.IsRendering() ? "in a render pass" : "outside the last pass",
                    state.width, state.height, state.num_color_attachments,
                    state.depth_stencil_attachment.image_view ? "yes" : "no", pending.size());
    }
    // The counters, written by the GPU through their device address; in place (the CPU reads
    // them, a VRAM copy would show it stale ones).
    const u32 size = (pairs - 1) * 16 + 8;
    buffer_cache.force_writes_in_place = true;
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(address, size, true);
    buffer_cache.force_writes_in_place = false;
    if (active) {
        EndSegment();
    }
    if (const auto it = BbStats::enabled ? written_at.find(address) : written_at.end();
        it != written_at.end()) {
        ++reused;
        reused_late += !scheduler.IsFree(it->second);
        // A query's begin counters at 16-byte steps, its end counters 8 bytes after them: what the
        // game read from its last use (statistics: none passed, some passed, not valid).
        if (address % 16 == 0 && scheduler.IsFree(it->second) && !ReadTrace()) {
            constexpr u64 Valid = 1ull << 63;
            u64 samples = 0;
            bool valid = true;
            for (u32 i = 0; i < pairs; ++i) {
                const u64 begin = *reinterpret_cast<const volatile u64*>(address + i * 16);
                const u64 end = *reinterpret_cast<const volatile u64*>(address + i * 16 + 8);
                valid = valid && (begin & Valid) && (end & Valid);
                samples += (end & ~Valid) - (begin & ~Valid);
            }
            ++(!valid ? queries_invalid : samples ? queries_passed : queries_hidden);
        }
    }
    pending.push_back({buffer->BufferDeviceAddress() + offset, pairs, begun, address, events + 1});
    if (ReadTrace()) {
        WatchReads(address, events + 1);
    }
    BeginSegment(scheduler.IsRendering());
    ++events;
    if (!scheduler.IsRendering() && pending.size() >= 64) {
        if (active) {
            EndSegment();
        }
        Resolve();
        ResetAhead();
        BeginSegment(false);
    }
    // BB_FRAME_STATS: how often the game measures (every 5 s).
    static auto last_report = std::chrono::steady_clock::now();
    static u64 reported = 0;
    if (const auto now = std::chrono::steady_clock::now();
        BbStats::enabled && now - last_report >= std::chrono::seconds(5)) {
        std::printf("Occlusion queries: %llu events in %.0f s, %llu in all; %llu segments, "
                    "%llu not counted; labels after unwritten events: %llu (%llu in a render "
                    "pass); counters reused before their results were written: %llu of %llu; queries "
                    "read: %llu none passed, %llu some passed, %llu not valid\n",
                    (unsigned long long)(events - reported),
                    std::chrono::duration<double>(now - last_report).count(),
                    (unsigned long long)events, (unsigned long long)begun,
                    (unsigned long long)starved, (unsigned long long)(labels_ahead + labels_ahead_in_pass),
                    (unsigned long long)labels_ahead_in_pass, (unsigned long long)reused_late,
                    (unsigned long long)reused, (unsigned long long)queries_hidden,
                    (unsigned long long)queries_passed, (unsigned long long)queries_invalid);
        labels_ahead = labels_ahead_in_pass = 0;
        reused = reused_late = 0;
        queries_hidden = queries_passed = queries_invalid = 0;
        last_report = now;
        reported = events;
        if (ReadTrace()) {
            PrintReadTrace();
        }
    }
}

bool OcclusionQueries::ReadTrace() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_OCCLUSION_READ_TRACE");
        return env && env[0] == '1';
    }();
    return enabled;
}

void OcclusionQueries::WatchReads(VAddr address, u64 id) {
    const u64 page = address & ~u64{4095};
    std::scoped_lock lk{watch_mutex};
    auto& list = watched[page];
    if (list.empty()) {
        BbLayer::WriteTraps::Set(page, 4096, BbLayer::WriteTraps::QueryReads, true);
    }
    list.push_back({id, 0, std::chrono::steady_clock::now()});
}

void OcclusionQueries::NoteResolved(VAddr address, u64 id, u64 tick) {
    std::scoped_lock lk{watch_mutex};
    if (const auto it = watched.find(address & ~u64{4095}); it != watched.end()) {
        for (auto& entry : it->second) {
            if (entry.id == id) {
                entry.resolve_tick = tick;
            }
        }
    }
}

bool OcclusionQueries::OnAccess(VAddr address, u64 rip, bool write, bool gpu_thread) {
    const u64 page = address & ~u64{4095};
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lk{watch_mutex};
    if (const auto it = watched.find(page); it != watched.end()) {
        ++accesses;
        accesses_by_port += gpu_thread;
        access_writes += write;
        ++access_rips[rip];
        for (const auto& entry : it->second) {
            const int state = entry.resolve_tick == 0                ? 0
                              : !scheduler.IsFree(entry.resolve_tick) ? 1
                                                                      : 2;
            ++access_state[state];
            const double ms = std::chrono::duration<double, std::milli>(now - entry.at).count();
            ++access_age[ms < 2 ? 0 : ms < 5 ? 1 : ms < 10 ? 2 : ms < 20 ? 3 : 4];
        }
        watched.erase(it);
    } else if (!(BbLayer::WriteTraps::Reasons(address) & BbLayer::WriteTraps::QueryReads)) {
        return false;
    }
    BbLayer::WriteTraps::Set(page, 4096, BbLayer::WriteTraps::QueryReads, false);
    return true;
}

void OcclusionQueries::PrintReadTrace() {
    std::scoped_lock lk{watch_mutex};
    std::vector<std::pair<u64, u64>> rips(access_rips.begin(), access_rips.end());
    std::sort(rips.begin(), rips.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string code;
    for (size_t i = 0; i < std::min<size_t>(rips.size(), 6); ++i) {
        char item[48];
        std::snprintf(item, sizeof(item), " %#llx x%llu", (unsigned long long)rips[i].first,
                      (unsigned long long)rips[i].second);
        code += item;
    }
    std::printf("Occlusion counters, first access after events: %llu pages (%llu by the port, "
                "%llu writes), %zu still watched; events: results not recorded %llu, GPU not "
                "there %llu, on the CPU %llu; after <2 ms %llu, <5 %llu, <10 %llu, <20 %llu, "
                "more %llu; code:%s\n",
                (unsigned long long)accesses, (unsigned long long)accesses_by_port,
                (unsigned long long)access_writes, watched.size(),
                (unsigned long long)access_state[0], (unsigned long long)access_state[1],
                (unsigned long long)access_state[2], (unsigned long long)access_age[0],
                (unsigned long long)access_age[1], (unsigned long long)access_age[2],
                (unsigned long long)access_age[3], (unsigned long long)access_age[4], code.c_str());
    accesses = accesses_by_port = access_writes = 0;
    access_state = {};
    access_age = {};
    access_rips.clear();
}

} // namespace Vulkan
