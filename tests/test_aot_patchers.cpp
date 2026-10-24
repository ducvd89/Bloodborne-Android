// SPDX-License-Identifier: GPL-2.0-or-later
// The native hook slot callbacks of the port's code patchers (gpu/shim/bbport_frame_rate.cpp, bbport_display.cpp, bbport_guest_guards.cpp), each
// compared with what the x86 stub it replaces does (docs/RECOMP_HOOKS.md): the stubs' semantics are written out here from their instruction
// listings, the callbacks are called as the engine calls them (a GuestRegsEx in, a result and a changed GuestRegsEx out). The sources are included, so
// the callbacks in their anonymous namespaces can be called; the guest CPU is a recording fake.
//   c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -Igpu/shim -Igpu/shadps4 -Isrc tests/test_aot_patchers.cpp -o out/aot-patchers-test
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <map>

#include "../gpu/shim/bbport_frame_rate.cpp"
#include "../gpu/shim/bbport_display.cpp"
#include "../gpu/shim/bbport_guest_guards.cpp"

// ---- The guest CPU and the rest of the runtime, as fakes ----
static std::map<std::uint64_t, std::pair<GuestHookEx, void*>> g_slots;
static std::vector<std::vector<std::uint64_t>> g_guest_calls;
static std::uint64_t g_guest_call_fn;
extern "C" {
void guest_cpu_code(uintptr_t, size_t) {}
uintptr_t guest_cpu_host_function(void* fn) { return uintptr_t(fn); }
uint64_t guest_cpu_call(uintptr_t fn, unsigned count, const uint64_t* args) {
    g_guest_call_fn = fn;
    g_guest_calls.emplace_back(args, args + count);
    return 0;
}
int guest_cpu_has_hook_slot(uintptr_t) { return 1; }
int guest_cpu_hook_slot(uintptr_t address, size_t, GuestHookEx fn, void* context) { g_slots[address] = {fn, context}; return 1; }
int runtime_memory_is_mapped(uintptr_t, uint64_t) { return 0; }
}
bool BbSubmitFxInitZero(std::uint64_t, std::uint32_t) { return false; }

using BbFrameRate::live;
static GuestRegsEx Regs() { GuestRegsEx r{}; return r; }
static float F(std::uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }

int main() {
    constexpr std::size_t ImageSize = 0x56d4000 + 0x1000;
    auto* image = static_cast<unsigned char*>(mmap(nullptr, ImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    assert(image != MAP_FAILED);
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(image);

    // ---- Frame rate: live in the image's data tail, the operand redirects' displacement is the same bytes in every run ----
    BbFrameRate::image_base = base;
    BbFrameRate::live = new (image + BbFrameRate::LiveImageOffset) BbFrameRate::Live{};
    assert(reinterpret_cast<std::uint64_t>(BbFrameRate::live) == base + 0x56d3f40 && BbFrameRate::LiveImageOffset % 64 == 0);
    {
        // A VEX `vmulss xmm0, xmm0, [rip + disp]` at 0x1000 reading the constant at 0x2000: redirected to live->cloth.
        const std::uint64_t site = 0x1000, constant = 0x2000;
        unsigned char* code = image + site;
        const unsigned char head[] = {0xc5, 0xfa, 0x59, 0x05};
        std::memcpy(code, head, 4);
        const std::int32_t disp = std::int32_t(constant - (site + 8));
        std::memcpy(code + 4, &disp, 4);
        BbFrameRate::committing = true;
        assert(BbFrameRate::RedirectOperand({site, constant, offsetof(BbFrameRate::Live, cloth)}));
        std::int32_t now;
        std::memcpy(&now, code + 4, 4);
        assert(base + site + 8 + now == base + 0x56d3f40 + offsetof(BbFrameRate::Live, cloth));
        const std::int32_t fixed = now;  // the same displacement however the image is mapped: it depends on offsets only
        assert(fixed == std::int32_t(0x56d3f40 + offsetof(BbFrameRate::Live, cloth) - (site + 8)));
        BbFrameRate::committing = false;
    }
    {
        // The pacer's mode: [r12+0x10]=1, byte [r12+0x14]=0, [r12+0x18]=shortest, qword [r12+0x268]=0; the game's lea runs next.
        alignas(16) unsigned char pacer[0x300];
        std::memset(pacer, 0xaa, sizeof(pacer));
        GuestRegsEx r = Regs();
        r.gpr[GUEST_R12] = reinterpret_cast<std::uint64_t>(pacer);
        assert(BbFrameRate::PacerModeSlot(nullptr, &r) == GUEST_HOOK_CONTINUE);
        std::uint32_t d10, d18; std::uint64_t q;
        std::memcpy(&d10, pacer + 0x10, 4); std::memcpy(&d18, pacer + 0x18, 4); std::memcpy(&q, pacer + 0x268, 8);
        assert(d10 == 1 && pacer[0x14] == 0 && pacer[0x15] == 0xaa && F(d18) == BbFrameRate::live->shortest && q == 0 && pacer[0x11] == 0 && pacer[0x1c] == 0xaa);
        // The main loop's step: FrameStep(rbx) (reads the measured frame time at +0x264), then [rbp-0x38] = dt; rip past the 7 bytes.
        const float measured = 0.02f;
        std::memcpy(pacer + 0x264, &measured, 4);
        unsigned char frame[0x80] = {};
        r = Regs();
        r.gpr[GUEST_RBX] = reinterpret_cast<std::uint64_t>(pacer);
        r.gpr[GUEST_RBP] = reinterpret_cast<std::uint64_t>(frame + 0x60);
        r.rip = base + 0x2018e36;
        assert(BbFrameRate::StepSlot(nullptr, &r) == GUEST_HOOK_JUMP && r.rip == base + 0x2018e36 + 7);
        float dt;
        std::memcpy(&dt, frame + 0x60 - 0x38, 4);
        assert(dt == 0.02f && BbFrameRate::live->dt == 0.02f);
        assert(std::abs(BbFrameRate::live->decay08 - std::pow(0.8f, 0.6f)) < 1e-6f && BbFrameRate::live->step_up == 0.1f * (0.02f * 30.0f));
        // A fixed step: [rbp + offset] = dt.
        r = Regs();
        r.gpr[GUEST_RBP] = reinterpret_cast<std::uint64_t>(frame + 0x60);
        r.rip = base + 0x17f9b71;
        assert(BbFrameRate::FixedStepSlot(reinterpret_cast<void*>(std::intptr_t(-0x48)), &r) == GUEST_HOOK_JUMP && r.rip == base + 0x17f9b71 + 7);
        std::memcpy(&dt, frame + 0x60 - 0x48, 4);
        assert(dt == 0.02f);
    }

    // ---- Guards ----
    BbGuestGuards::image_base = base;
    {
        // The clamp: edi < 3 (signed 32-bit) becomes 3 (mov edi, 3 clears the upper half), anything else stays.
        for (const std::int32_t edi : {2, 0, -1, 3, 4, 100}) {
            GuestRegsEx r = Regs();
            r.gpr[GUEST_RDI] = std::uint64_t(std::uint32_t(edi)) | (edi == 100 ? 0x1234ull << 32 : 0);
            assert(BbGuestGuards::ClampSlot(nullptr, &r) == GUEST_HOOK_CONTINUE);
            if (edi < 3) assert(r.gpr[GUEST_RDI] == 3);
            else assert(std::int32_t(r.gpr[GUEST_RDI]) == edi);
        }
        assert(BbGuestGuards::clamps.load() == 3);
        // The GX node: with a node `mov [r12+8], r13` then on; without, told and past the post.
        std::uint64_t node[4] = {};
        GuestRegsEx r = Regs();
        r.gpr[GUEST_R12] = reinterpret_cast<std::uint64_t>(node);
        r.gpr[GUEST_R13] = 0xdeadbeef;
        assert(BbGuestGuards::GxNodeSlot(nullptr, &r) == GUEST_HOOK_JUMP && node[1] == 0xdeadbeef && r.rip == base + 0x2199315);
        r = Regs();
        r.gpr[GUEST_R13] = 5;
        assert(BbGuestGuards::GxNodeSlot(nullptr, &r) == GUEST_HOOK_JUMP && r.rip == base + 0x219934e && BbGuestGuards::gx_null_nodes.load() == 1);
        // The effect clear: the call's four arguments reach FxInitClear, which (the other thread, nothing mappable) calls the game's function as the call did; on after the call.
        g_guest_calls.clear();
        r = Regs();
        r.rip = base + 0xebbc86;
        r.gpr[GUEST_RDI] = 1; r.gpr[GUEST_RSI] = 2; r.gpr[GUEST_RDX] = 3; r.gpr[GUEST_RCX] = 4;
        assert(BbGuestGuards::FxInitSlot(nullptr, &r) == GUEST_HOOK_JUMP && r.rip == base + 0xebbc86 + 5);
        assert(g_guest_calls.size() == 1 && g_guest_call_fn == base + 0x2168630 && g_guest_calls[0] == std::vector<std::uint64_t>({1, 2, 3, 4}));
    }

    // ---- Display ----
    BbDisplay::image_base = base;
    alignas(64) static unsigned char data_page[4096];
    BbDisplay::data = new (data_page) BbDisplay::Data{};
    {
        const float aspect = 21.0f / 9.0f;
        std::memcpy(&BbDisplay::data->camera_aspect_bits, &aspect, 4);
        const float factor = 1.25f;
        std::memcpy(&BbDisplay::data->fov_factor_bits, &factor, 4);
        // vmovss [rbx+0x5c], xmm0 then the aspect into +0x54 and the widened fov 2 atan(tan(fov/2) * factor) in +0x50.
        alignas(16) unsigned char camera[0x80] = {};
        const float fov = 1.0f, near_plane = 0.1f;
        std::memcpy(camera + 0x50, &fov, 4);
        GuestRegsEx r = Regs();
        r.gpr[GUEST_RBX] = reinterpret_cast<std::uint64_t>(camera);
        r.rip = base + 0x1436c2c;
        std::memcpy(r.ymm[0], &near_plane, 4);
        assert(BbDisplay::CameraSlot(nullptr, &r) == GUEST_HOOK_JUMP && r.rip == base + 0x1436c2c + 5);
        float got_far, got_aspect, got_fov;
        std::memcpy(&got_far, camera + 0x5c, 4); std::memcpy(&got_aspect, camera + 0x54, 4); std::memcpy(&got_fov, camera + 0x50, 4);
        assert(got_far == near_plane && got_aspect == aspect);
        assert(got_fov == float(2.0 * std::atan(std::tan(1.0 * 0.5) * double(factor))));
        // vmovups [rbx+0x50], xmm1: 16 bytes, then the same.
        std::memset(camera, 0, sizeof(camera));
        float block[4] = {2.0f, 7.0f, 0.5f, 9.0f};
        r = Regs();
        r.gpr[GUEST_RBX] = reinterpret_cast<std::uint64_t>(camera);
        std::memcpy(r.ymm[1], block, 16);
        assert(BbDisplay::CameraSlot(reinterpret_cast<void*>(1), &r) == GUEST_HOOK_JUMP);
        std::memcpy(&got_aspect, camera + 0x54, 4); std::memcpy(&got_fov, camera + 0x50, 4);
        float zw[2]; std::memcpy(zw, camera + 0x58, 8);
        assert(got_aspect == aspect && zw[0] == 0.5f && zw[1] == 9.0f && got_fov == float(2.0 * std::atan(std::tan(2.0 * 0.5) * double(factor))));
        // The markers: xmm0 * now + the game's bias (single precision), upper half of ymm0 cleared; rip past the 16 bytes.
        const float bias[4] = {0.5f, 0.5f, 0.0f, 0.0f};
        std::memcpy(image + BbDisplay::MarkerSites[0].bias, bias, 16);
        const float now[4] = {0.5f * 1.3f, -0.5f, 1.0f, 1.0f};
        std::memcpy(BbDisplay::data->markers[0].now, now, 16);
        const float x[4] = {0.3f, 0.7f, 5.0f, 1.0f};
        r = Regs();
        std::memcpy(r.ymm[0], x, 16);
        std::memset(r.ymm[0] + 16, 0xff, 16);
        r.rip = base + 0x1644379;
        assert(BbDisplay::MarkerSlot(reinterpret_cast<void*>(0), &r) == GUEST_HOOK_JUMP && r.rip == base + 0x1644379 + 16);
        float y[4];
        std::memcpy(y, r.ymm[0], 16);
        for (int k = 0; k < 4; ++k) { const float p = x[k] * now[k]; assert(y[k] == p + bias[k]); }
        for (int k = 16; k < 32; ++k) assert(r.ymm[0][k] == 0);
        // The viewport: the full-screen movie's rect gets the pixel aspect at +0x30; any other rect is left.
        std::uint32_t viewport[16] = {1920, 1080, 0, 0, 1920, 1080};
        const float pixel = 2.37f / (16.0f / 9.0f);
        std::uint32_t bits; std::memcpy(&bits, &pixel, 4);
        r = Regs();
        r.gpr[GUEST_RSI] = reinterpret_cast<std::uint64_t>(viewport);
        assert(BbDisplay::ViewportSlot(reinterpret_cast<void*>(std::uintptr_t(bits)), &r) == GUEST_HOOK_CONTINUE && viewport[12] == bits);
        viewport[12] = 0; viewport[4] = 1280;
        BbDisplay::ViewportSlot(reinterpret_cast<void*>(std::uintptr_t(bits)), &r);
        assert(viewport[12] == 0);
        // The scene size: `mov eax, imm32` (the new size in the low 24 bits, the template's top byte): rax = imm32, flags and the rest untouched, on after 5 bytes.
        r = Regs();
        r.gpr[GUEST_RAX] = 0xffffffffffffffffull;
        r.rip = base + 0x1d96a6b;
        assert(BbDisplay::SceneImmediateSlot(reinterpret_cast<void*>(std::uintptr_t(1707)), &r) == GUEST_HOOK_JUMP && r.gpr[GUEST_RAX] == 1707 && r.rip == base + 0x1d96a6b + 5);
    }
    std::puts("aot patchers test: ok");
    return 0;
}
