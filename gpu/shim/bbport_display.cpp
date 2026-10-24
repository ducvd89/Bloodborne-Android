// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the output's aspect ratio by hooks in the game's code (bbport_display.h). Ported from
// deadinside28/bloodborne_pc (testing, 72e5c8c "0.6-pre1", bbport_guest_hooks.cpp) to the Bionic build,
// where the game's x86-64 code runs in FEXCore. Differences from upstream's detours:
//  - the stubs are guest code on a page registered with guest_cpu_code; those that need no C++
//    (camera aspect with x87, the markers' vector, the viewport's pixel aspect) are pure x86 memory
//    operations and call nothing;
//  - the HUD anchoring needs C++ (Scaleform's ObjectInterface is called): its stub calls HudFrame
//    through guest_cpu_host_function (as bbport_frame_rate.cpp does for FrameStep), and HudFrame calls
//    the game's GetMember/SetMember/Release through guest_cpu_call; the Scaleform values it passes live
//    on a page below 1 TiB, next to the image (not on the host stack);
//  - every site is checked before the first byte is written (the check pass writes nothing), the
//    camera and viewport sites are required, markers and HUD are optional.
// Nothing is installed at 16:9 or with BB_ASPECT_HOOKS=0.
#include "common/types.h"
#include "bbport_display.h"
#include "bbport_guest_code.h"
#include "bbport_hook_sites.h"
#include "bbport_patch_notes.h"
#include "../../src/guest_cpu.h"

#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace BbDisplay {
namespace {
using BbGuestCode::Emitter;
using u64 = std::uint64_t;
using u8 = std::uint8_t;
using u32 = std::uint32_t;

constexpr float GameAspect = 16.0f / 9.0f;

u64 image_base = 0;
bool committing = false;
std::vector<std::pair<u64, u64>> claimed;
u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t PageSize = 4096;

// ---- Scaleform values as the game has them (the game's own copies: 0x1adb830, lookup 0x1f68aa5) ----
struct GfxValue {
    u64 links[2] = {~u64(0), ~u64(0)};
    void* object_interface = nullptr;
    u32 type = 0, pad = 0;
    union {
        double number;
        void* data;
    } value{};
    u64 aux = 0;
};
static_assert(sizeof(GfxValue) == 0x30);

/// The data the stubs read, and the HUD code's scratch, on a page next to the image (below 1 TiB).
struct alignas(64) Data {
    u32 camera_aspect_bits = 0; ///< the output's aspect ratio as a float; 0: the game's
    u32 pad0[3] = {};
    u32 fov_factor_bits = 0;    ///< float: the tangent's factor; 0: the game's field of view (read at +0)
    float half = 0.5f;          ///< (read at +4)
    u32 pad1[2] = {};
    struct alignas(16) Marker {
        float game[4];  ///< the game's constant
        float now[4];   ///< what the detour multiplies by
    } markers[3] = {};
    GfxValue values[4]; ///< the HUD code's: the root's PlayerHUD, a clip, a temporary, a number to set
    char names[8][16] = {};
};
static_assert(sizeof(Data) <= PageSize);
Data* data = nullptr;

float hud_dx = 0.0f, hud_dy = 0.0f;
bool hud_anchor_on = true;

// ---- Check / commit installation (as bbport_frame_rate.cpp) ----
void Writable(u64 address, std::size_t size) {
    BbPatchNote(address, size, "display hooks (bbport_display)");
    const u64 page = u64(sysconf(_SC_PAGESIZE));
    const u64 start = address & ~(page - 1);
    mprotect(reinterpret_cast<void*>(start), address + size - start, PROT_READ | PROT_WRITE | PROT_EXEC);
}

bool Claim(u64 address, std::size_t size) {
    for (const auto& [start, end] : claimed) {
        if (address < end && start < address + size) return false;
    }
    claimed.emplace_back(address, address + size);
    return true;
}

/// `e` into the stub page; a jump to it at `site` over `length` bytes (the rest int3) if the site
/// holds `expected`.
bool Detour(u64 site, const u8* expected, std::size_t length, const Emitter& e) {
    u8* code = reinterpret_cast<u8*>(image_base + site);
    if (length < 5 || std::memcmp(code, expected, length) != 0 || e.overflow ||
        stub_used + e.size > PageSize) {
        return false;
    }
    u8* stub = stub_page + stub_used;
    const std::int64_t rel = std::int64_t(reinterpret_cast<u64>(stub)) - std::int64_t(image_base + site + 5);
    if (rel != std::int32_t(rel)) return false;
    stub_used += (e.size + 15) & ~std::size_t(15);
    if (!committing) return Claim(image_base + site, length);
    std::memcpy(stub, e.code.data(), e.size);
    const std::int32_t rel32 = std::int32_t(rel);
    Writable(reinterpret_cast<u64>(code), length);
    code[0] = 0xe9;
    std::memcpy(code + 1, &rel32, 4);
    for (std::size_t i = 5; i < length; ++i) code[i] = 0xcc;
    return true;
}

u8* MapNear() {
    for (u64 k = 1; k <= 64; ++k) {
        const u64 hint = image_base - (64 + k) * (8ull << 20); // away from bbport_frame_rate's
        void* p = mmap(reinterpret_cast<void*>(hint), PageSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) return static_cast<u8*>(p);
    }
    return nullptr;
}

// ---- The camera ----
// The camera manager (0x1436b..) copies the active camera's projection parameters into the current
// camera every frame (+0x50 vertical field of view, +0x54 aspect ratio, +0x58 near, +0x5c far); the
// projection (0x1d16830), the culling and the HUD's markers read them there. After the copy the output's
// aspect ratio replaces the game's 1.777; taller than 16:9 the field of view is widened too:
// 2 atan(tan(fov / 2) * 16:9 / aspect), x87.
struct ParamStore {
    u64 offset;
    std::array<u8, 5> code;
};
constexpr std::array<ParamStore, 5> CameraParamStores = {{
    {0x1436c2c, {0xc5, 0xfa, 0x11, 0x43, 0x5c}}, // vmovss %xmm0, 0x5c(%rbx): the follow camera
    {0x1436d8a, {0xc5, 0xfa, 0x11, 0x43, 0x5c}},
    {0x1436e68, {0xc5, 0xf8, 0x11, 0x4b, 0x50}}, // vmovups %xmm1, 0x50(%rbx): a blend of two
    {0x1436eec, {0xc5, 0xfa, 0x11, 0x43, 0x5c}},
    {0x1436f4b, {0xc5, 0xfa, 0x11, 0x43, 0x5c}},
}};

bool HookCamera(const ParamStore& site) {
    Emitter e;
    for (const u8 b : site.code) e.Bytes({b});
    e.Bytes({0x9c, 0x50});                         // pushfq; push rax
    e.Bytes({0x48, 0xb8});                         // movabs rax, &camera_aspect_bits
    e.U64(reinterpret_cast<u64>(&data->camera_aspect_bits));
    e.Bytes({0x8b, 0x00});                         // mov eax, [rax]
    e.Bytes({0x85, 0xc0, 0x74, 0x03});             // test eax, eax; je over the store
    e.Bytes({0x89, 0x43, 0x54});                   // mov [rbx + 0x54], eax
    e.Bytes({0x48, 0xb8});                         // movabs rax, &fov_factor_bits
    e.U64(reinterpret_cast<u64>(&data->fov_factor_bits));
    e.Bytes({0x83, 0x38, 0x00, 0x74, 0x15});       // cmp dword [rax], 0; je over the x87 part
    e.Bytes({0xd9, 0x43, 0x50});                   // fld dword [rbx + 0x50]: fov
    e.Bytes({0xd8, 0x48, 0x04});                   // fmul dword [rax + 4]: fov / 2
    e.Bytes({0xd9, 0xf2, 0xdd, 0xd8});             // fptan; fstp st0 (its 1.0)
    e.Bytes({0xd8, 0x08});                         // fmul dword [rax]: tan * factor
    e.Bytes({0xd9, 0xe8, 0xd9, 0xf3});             // fld1; fpatan: atan(tan * factor)
    e.Bytes({0xd8, 0xc0});                         // fadd st0, st0
    e.Bytes({0xd9, 0x5b, 0x50});                   // fstp dword [rbx + 0x50]
    e.Bytes({0x58, 0x9d});                         // pop rax; popfq
    e.JmpAbs(image_base + site.offset + site.code.size());
    return Detour(site.offset, site.code.data(), site.code.size(), e);
}

// ---- The HUD's markers over characters (lock-on, enemies' health bars, damage numbers) ----
// Projected with the current camera, then placed in the game's 1920x1080 UI by `* (0.5, -0.5, ..) +
// (0.5, 0.5, ..)` (three places). The output shows that UI in the middle of a wider (taller) view at its
// height (width): a marker's 0.5 becomes 0.5 * aspect / 16:9 (16:9 / aspect for y). The detour multiplies
// by the port's vector instead of the game's constant.
struct MarkerSite {
    u64 offset;
    u64 scale, bias;
};
constexpr std::array<MarkerSite, 3> MarkerSites = {{
    {0x1644379, 0x48f9a00, 0x48f9a10},
    {0x1644c45, 0x48f9a60, 0x48f9a70},
    {0x16452b7, 0x48f9ad0, 0x48f9ae0},
}};

/// `vmulps disp(%rip), %xmm0, %xmm0; vaddps disp(%rip), %xmm0, %xmm0` reading `scale`, `bias`.
bool MarkerCodeAt(const MarkerSite& site) {
    const auto* p = reinterpret_cast<const u8*>(image_base + site.offset);
    const auto reads = [&](int at, u8 opcode, u64 target) {
        std::int32_t disp;
        std::memcpy(&disp, p + at + 4, 4);
        return p[at] == 0xc5 && p[at + 1] == 0xf8 && p[at + 2] == opcode && p[at + 3] == 0x05 &&
               site.offset + at + 8 + disp == target;
    };
    return reads(0, 0x59, site.scale) && reads(8, 0x58, site.bias);
}

bool HookMarker(std::size_t i, float kx, float ky) {
    const MarkerSite& site = MarkerSites[i];
    if (!MarkerCodeAt(site)) return false;
    Data::Marker& m = data->markers[i];
    std::memcpy(m.game, reinterpret_cast<const void*>(image_base + site.scale), sizeof(m.game));
    const float now[4] = {m.game[0] * kx, m.game[1] * ky, m.game[2], m.game[3]};
    if (committing) std::memcpy(m.now, now, sizeof(now));
    Emitter e;
    e.Bytes({0x50});                               // push rax
    e.Bytes({0x48, 0xb8});                         // movabs rax, &now
    e.U64(reinterpret_cast<u64>(&m.now));
    e.Bytes({0xc5, 0xf8, 0x59, 0x00});             // vmulps xmm0, xmm0, [rax]
    e.Bytes({0x48, 0xb8});                         // movabs rax, the game's bias
    e.U64(image_base + site.bias);
    e.Bytes({0xc5, 0xf8, 0x58, 0x00});             // vaddps xmm0, xmm0, [rax]
    e.Bytes({0x58});                               // pop rax
    e.JmpAbs(image_base + site.offset + 16);
    u8 expected[16];
    std::memcpy(expected, reinterpret_cast<const void*>(image_base + site.offset), 16);
    return Detour(site.offset, expected, 16, e);
}

// ---- The UI (Scaleform movies) for the output's aspect ratio ----
// The menu system sets each movie's viewport every frame (0x1f59100 -> MovieImpl::SetViewport 0x1f510,
// a GFx::Viewport in rsi: buffer 1920x1080, rect 0,0 1920x1080 for the full-screen movies). Given the
// output's pixel aspect ratio (Viewport::AspectRatio at +48: (output width / height) / 16:9) a movie
// shows its 16:9 screen in the middle of a wider (taller) view squeezed into the 1920x1080 frame, which
// the output stretches back (UiComposition::Scale). Pure x86: the checks and the store are in the stub.
constexpr u64 SetViewportSite = 0x1f510;
constexpr u8 FunctionEntry[6] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}; // push rbp; mov rbp, rsp; push r15

bool HookViewport(float pixel_aspect) {
    Emitter e;
    std::array<std::size_t, 6> jumps{};
    const auto check = [&](std::initializer_list<u8> compare, std::size_t k) {
        e.Bytes(compare);
        e.Bytes({0x75, 0x00}); // jne done (patched below)
        jumps[k] = e.size - 1;
    };
    check({0x81, 0x3e, 0x80, 0x07, 0x00, 0x00}, 0);       // cmp dword [rsi], 1920
    check({0x81, 0x7e, 0x04, 0x38, 0x04, 0x00, 0x00}, 1); // cmp dword [rsi + 4], 1080
    check({0x83, 0x7e, 0x08, 0x00}, 2);                   // cmp dword [rsi + 8], 0
    check({0x83, 0x7e, 0x0c, 0x00}, 3);                   // cmp dword [rsi + 12], 0
    check({0x81, 0x7e, 0x10, 0x80, 0x07, 0x00, 0x00}, 4); // cmp dword [rsi + 16], 1920
    check({0x81, 0x7e, 0x14, 0x38, 0x04, 0x00, 0x00}, 5); // cmp dword [rsi + 20], 1080
    u32 bits;
    std::memcpy(&bits, &pixel_aspect, 4);
    e.Bytes({0xc7, 0x46, 0x30});                          // mov dword [rsi + 48], aspect
    e.U32(bits);
    const std::size_t done = e.size;
    for (const std::size_t j : jumps) {
        if (!e.overflow) e.code[j] = u8(done - (j + 1));
    }
    for (const u8 b : FunctionEntry) e.Bytes({b});
    e.JmpAbs(image_base + SetViewportSite + sizeof(FunctionEntry));
    return Detour(SetViewportSite, FunctionEntry, sizeof(FunctionEntry), e);
}

// ---- The HUD anchored to the output's edges ----
// The game lays its HUD (menu/fe.gfx, Scaleform) out for 1920x1080; on a wider output that screen is
// drawn in the middle and the HUD's edges would stay there. The HUD's groups are moved as the game moves
// its own clips: through the movie's members (ObjectInterface::GetMember +0x28, SetMember +0x30, Release
// +0x18 of the vtable). Bars, items and gauges go to the left edge, blood echoes and insight and the
// autosave icon to the right; on a taller output the top ones go up and the boss bar down.
constexpr u32 GfxManaged = 0x40, GfxNumber = 5, GfxDisplayObject = 10;
constexpr std::size_t SlotRelease = 0x18, SlotGetMember = 0x28, SlotSetMember = 0x30;
constexpr u64 HudFrameSite = 0x1bf9e10;
constexpr u64 ScreenHudOffset = 0xa80;
constexpr u64 HudWrapperVtable = 0x539a070;

/// `name` copied to the data page (below 1 TiB, where the game's code is used to strings).
const char* Low(const char* name) {
    for (auto& slot : data->names) {
        if (!slot[0]) {
            std::strncpy(slot, name, sizeof(slot) - 1);
            return slot;
        }
        if (std::strcmp(slot, name) == 0) return slot;
    }
    return name;
}
u64 GuestMethod(void* oi, std::size_t slot) {
    return *reinterpret_cast<const u64*>(*reinterpret_cast<const u64*>(oi) + slot);
}
bool GfxIsObject(const GfxValue& v) {
    return v.object_interface && (v.type & 0x8c) == 8;
}
void GfxRelease(GfxValue& v) {
    if (v.object_interface && (v.type & GfxManaged)) {
        const u64 args[3] = {u64(v.object_interface), u64(&v), u64(v.value.data)};
        guest_cpu_call(GuestMethod(v.object_interface, SlotRelease), 3, args);
    }
    v = GfxValue{};
}
bool GfxGet(const GfxValue& object, const char* name, GfxValue& out) {
    out = GfxValue{};
    if (!GfxIsObject(object)) return false;
    const bool isdobj = (object.type & 0x8f) == GfxDisplayObject;
    const u64 args[5] = {u64(object.object_interface), u64(object.value.data), u64(Low(name)), u64(&out), isdobj};
    return (guest_cpu_call(GuestMethod(object.object_interface, SlotGetMember), 5, args) & 0xff) != 0;
}
bool GfxGetNumber(const GfxValue& object, const char* name, double& number) {
    GfxValue& v = data->values[2];
    const bool ok = GfxGet(object, name, v) && (v.type & 0x8f) == GfxNumber;
    if (ok) number = v.value.number;
    GfxRelease(v);
    return ok;
}
bool GfxSetNumber(const GfxValue& object, const char* name, double number) {
    if (!GfxIsObject(object)) return false;
    GfxValue& v = data->values[3];
    v = GfxValue{};
    v.type = GfxNumber;
    v.value.number = number;
    const bool isdobj = (object.type & 0x8f) == GfxDisplayObject;
    const u64 args[5] = {u64(object.object_interface), u64(object.value.data), u64(Low(name)), u64(&v), isdobj};
    return (guest_cpu_call(GuestMethod(object.object_interface, SlotSetMember), 5, args) & 0xff) != 0;
}

struct HudItem {
    int parent; ///< 0 the root, 1 PlayerHUD
    const char* name;
    float fx, fy; ///< how it follows the edges: x by dx, y by dy
    double base_x = 0, base_y = 0;
    double set_x = 0, set_y = 0;
    bool known = false;
};
std::array<HudItem, 5> hud_items = {{
    {0, "PlayerHUD", -1.0f, -1.0f},
    {1, "Soul", 2.0f, 0.0f}, // in PlayerHUD: twice, for its move to the left
    {1, "HeroPoint", 2.0f, 0.0f},
    {0, "AutoSaveIcon", 1.0f, -1.0f},
    {0, "BossTop", 0.0f, 1.0f},
}};

/// Each frame, from the game's in-game screen update (rdi the screen): the HUD groups where the
/// output's edges are. The HUD (a value wrapper whose movie's root is at +0x30) is at screen + 0xa80;
/// its values are got and released within the frame (the HUD movie is unloaded with areas).
void PS4_SYSV_ABI HudFrame(u64 screen) {
    static unsigned frames = 0;
    static u64 last_hud = 0;
    static float applied_dx = 0.0f, applied_dy = 0.0f;
    const u64 hud = screen ? *reinterpret_cast<const u64*>(screen + ScreenHudOffset) : 0;
    if (!hud) return;
    if (*reinterpret_cast<const u64*>(hud) != image_base + HudWrapperVtable) {
        static bool told = false;
        if (!told) {
            told = true;
            std::printf("HUD: unexpected object at the screen's HUD: not anchored\n");
        }
        return;
    }
    const GfxValue& root = *reinterpret_cast<const GfxValue*>(hud + 0x30);
    if (hud != last_hud) {
        last_hud = hud; // a new HUD (another area): where the game put its groups is read again
        for (HudItem& item : hud_items) item.known = false;
        static bool told = false;
        if (!told) {
            told = true;
            std::printf("HUD: anchored to the output's edges\n");
        }
    }
    bool all_known = true;
    for (const HudItem& item : hud_items) all_known = all_known && item.known;
    // Rechecked every half second: the game may put a group back itself.
    if (hud_dx == applied_dx && hud_dy == applied_dy && all_known && ++frames % 32 != 0) return;
    if (!GfxIsObject(root)) return;
    GfxValue& player = data->values[0];
    GfxValue& clip = data->values[1];
    GfxGet(root, "PlayerHUD", player);
    for (HudItem& item : hud_items) {
        if (!GfxGet(item.parent ? player : root, item.name, clip) || !GfxIsObject(clip)) {
            GfxRelease(clip);
            continue;
        }
        double x = 0, y = 0;
        if (GfxGetNumber(clip, "x", x) && GfxGetNumber(clip, "y", y)) {
            // Positions are in twips (1/20 pixel): a read back is that close.
            const auto moved = [](double a, double b) { return std::abs(a - b) > 0.06; };
            if (!item.known || moved(x, item.set_x) || moved(y, item.set_y)) {
                item.base_x = x; // first seen, or moved by the game since
                item.base_y = y;
                item.known = true;
            }
            const double nx = item.base_x + item.fx * hud_dx, ny = item.base_y + item.fy * hud_dy;
            if (moved(nx, x)) GfxSetNumber(clip, "x", nx);
            if (moved(ny, y)) GfxSetNumber(clip, "y", ny);
            double check_x = nx, check_y = ny;
            GfxGetNumber(clip, "x", check_x);
            GfxGetNumber(clip, "y", check_y);
            item.set_x = check_x;
            item.set_y = check_y;
            if (moved(check_x, nx) || moved(check_y, ny)) {
                static bool told = false;
                if (!told) {
                    told = true;
                    std::printf("HUD: %s did not take its place (%.1f, %.1f instead of %.1f, %.1f)\n",
                                item.name, check_x, check_y, nx, ny);
                }
            }
        }
        GfxRelease(clip);
    }
    GfxRelease(player);
    applied_dx = hud_dx;
    applied_dy = hud_dy;
}

bool HookHud() {
    Emitter e;
    e.SaveVolatile();
    e.SaveXmm();
    // rdi (the screen) is as the game passed it.
    e.CallAbs(reinterpret_cast<const void*>(guest_cpu_host_function(reinterpret_cast<void*>(&HudFrame))));
    e.RestoreXmm();
    e.RestoreVolatile();
    for (const u8 b : FunctionEntry) e.Bytes({b});
    e.JmpAbs(image_base + HudFrameSite + sizeof(FunctionEntry));
    return Detour(HudFrameSite, FunctionEntry, sizeof(FunctionEntry), e);
}

struct Counts {
    int camera = 0, markers = 0;
    bool viewport = false, hud = false;
};

/// All the hooks in one pass (written when `committing`, else only checked). False: a required site is
/// not the expected code (`what` names it).
bool InstallAll(Counts& c, std::string& what, float aspect) {
    c = {};
    stub_used = 0;
    claimed.clear();
    float kx = 1.0f, ky = 1.0f; // the markers' scale: the UI is as high (wide) as the output
    float fov = 0.0f;
    if (aspect < GameAspect) {
        ky = GameAspect / aspect;
        fov = ky;
    } else {
        kx = aspect / GameAspect;
    }
    if (committing) {
        std::memcpy(&data->camera_aspect_bits, &aspect, 4);
        std::memcpy(&data->fov_factor_bits, &fov, 4);
        hud_dx = 960.0f * (kx - 1.0f);
        hud_dy = 540.0f * (ky - 1.0f);
        if (const char* env = std::getenv("BB_HUD_DX"); env && *env) hud_dx = std::strtof(env, nullptr); // diagnostics
    }
    for (const ParamStore& site : CameraParamStores) {
        if (!HookCamera(site)) {
            char text[80];
            std::snprintf(text, sizeof(text), "the camera's parameters (%#llx) are not the expected code",
                          (unsigned long long)site.offset);
            what = text;
            return false;
        }
        ++c.camera;
    }
    c.viewport = HookViewport(aspect / GameAspect);
    if (!c.viewport) {
        what = "the UI's viewport (0x1f510) is not the expected code";
        return false;
    }
    for (std::size_t i = 0; i < MarkerSites.size(); ++i) c.markers += HookMarker(i, kx, ky);
    if (hud_anchor_on) c.hud = HookHud();
    return true;
}


// ---- Native hook slots (the game's code runs as ahead-of-time recompiled units, docs/RECOMP_HOOKS.md) ----
// Every stub above as a C++ callback of a hook slot: nothing is written into the game's code, no stub page. What a callback does is what its stub did,
// with the registers the hook gives it (the stubs saved and restored flags and registers; the callbacks leave them alone).

/// The camera stub: the game's store, the output's aspect ratio into +0x54, and the widened field of view (x87 in the stub, in double precision here: the
/// reduced x87 FEX and the recompiled code run is double precision too). The context is the store: 0 `vmovss [rbx + 0x5c], xmm0`, 1 `vmovups [rbx + 0x50], xmm1`.
std::uint32_t CameraSlot(void* store, GuestRegsEx* r) {
    u8* camera = reinterpret_cast<u8*>(r->gpr[GUEST_RBX]);
    if (store == nullptr) std::memcpy(camera + 0x5c, r->ymm[0], 4);
    else std::memcpy(camera + 0x50, r->ymm[1], 16);
    if (data->camera_aspect_bits) std::memcpy(camera + 0x54, &data->camera_aspect_bits, 4);
    if (data->fov_factor_bits) {
        float fov, factor;
        std::memcpy(&fov, camera + 0x50, 4);
        std::memcpy(&factor, &data->fov_factor_bits, 4);
        double t = std::tan(double(fov) * double(data->half)); // fld fov; fmul [half]; fptan; fstp st0
        t = t * double(factor);                                 // fmul [factor]
        const double angle = std::atan2(t, 1.0);                // fld1; fpatan
        const float widened = float(angle + angle);             // fadd st0, st0; fstp dword
        std::memcpy(camera + 0x50, &widened, 4);
    }
    r->rip += 5;
    return GUEST_HOOK_JUMP;
}

/// A marker's `vmulps xmm0, xmm0, [scale]; vaddps xmm0, xmm0, [bias]` with the port's vector for the scale (the context is the marker's index).
std::uint32_t MarkerSlot(void* index, GuestRegsEx* r) {
    const std::size_t i = reinterpret_cast<std::uintptr_t>(index);
    float x[4], bias[4], y[4];
    std::memcpy(x, r->ymm[0], 16);
    std::memcpy(bias, reinterpret_cast<const void*>(image_base + MarkerSites[i].bias), 16);
    for (int k = 0; k < 4; ++k) {
        const float product = x[k] * data->markers[i].now[k];
        y[k] = product + bias[k];
    }
    std::memcpy(r->ymm[0], y, 16);
    std::memset(r->ymm[0] + 16, 0, 16); // (VEX.128 clears the upper half)
    r->rip += 16;
    return GUEST_HOOK_JUMP;
}

/// SetViewport's stub: the pixel aspect into the full-screen movies' viewport (the context holds the float's bits).
std::uint32_t ViewportSlot(void* bits, GuestRegsEx* r) {
    u8* viewport = reinterpret_cast<u8*>(r->gpr[GUEST_RSI]);
    u32 v[6];
    std::memcpy(v, viewport, sizeof(v));
    if (v[0] == 1920 && v[1] == 1080 && v[2] == 0 && v[3] == 0 && v[4] == 1920 && v[5] == 1080) {
        const u32 aspect = u32(reinterpret_cast<std::uintptr_t>(bits));
        std::memcpy(viewport + 0x30, &aspect, 4);
    }
    return GUEST_HOOK_CONTINUE;
}

std::uint32_t HudSlot(void*, GuestRegsEx* r) {
    HudFrame(r->gpr[GUEST_RDI]);
    return GUEST_HOOK_CONTINUE;
}

/// All the hooks as slots, in one pass as InstallAll: a required site that is not the expected code, or has no slot, fails.
bool InstallAllSlots(Counts& c, std::string& what, float aspect) {
    c = {};
    claimed.clear();
    float kx = 1.0f, ky = 1.0f;
    float fov = 0.0f;
    if (aspect < GameAspect) {
        ky = GameAspect / aspect;
        fov = ky;
    } else {
        kx = aspect / GameAspect;
    }
    if (committing) {
        std::memcpy(&data->camera_aspect_bits, &aspect, 4);
        std::memcpy(&data->fov_factor_bits, &fov, 4);
        hud_dx = 960.0f * (kx - 1.0f);
        hud_dy = 540.0f * (ky - 1.0f);
        if (const char* env = std::getenv("BB_HUD_DX"); env && *env) hud_dx = std::strtof(env, nullptr); // diagnostics
    }
    const auto slot = [&](u64 site, const u8* expected, std::size_t length, GuestHookEx fn, void* context,
                          std::initializer_list<u64> resume) {
        if (std::memcmp(reinterpret_cast<const u8*>(image_base + site), expected, length) != 0) return false;
        if (!committing) return Claim(image_base + site, length) && BbHookSites::Available(image_base + site);
        return BbHookSites::Slot(image_base + site, image_base, length, fn, context, resume);
    };
    for (const ParamStore& site : CameraParamStores) {
        if (!slot(site.offset, site.code.data(), site.code.size(), CameraSlot, site.code[1] == 0xfa ? nullptr : reinterpret_cast<void*>(1),
                  {image_base + site.offset + site.code.size()})) {
            char text[80];
            std::snprintf(text, sizeof(text), "the camera's parameters (%#llx) are not the expected code", (unsigned long long)site.offset);
            what = text;
            return false;
        }
        ++c.camera;
    }
    {
        float pixel_aspect = aspect / GameAspect;
        u32 bits;
        std::memcpy(&bits, &pixel_aspect, 4);
        c.viewport = slot(SetViewportSite, FunctionEntry, sizeof(FunctionEntry), ViewportSlot, reinterpret_cast<void*>(std::uintptr_t(bits)), {});
        if (!c.viewport) {
            what = "the UI's viewport (0x1f510) is not the expected code";
            return false;
        }
    }
    for (std::size_t i = 0; i < MarkerSites.size(); ++i) {
        const MarkerSite& site = MarkerSites[i];
        if (!MarkerCodeAt(site)) continue;
        Data::Marker& m = data->markers[i];
        std::memcpy(m.game, reinterpret_cast<const void*>(image_base + site.scale), sizeof(m.game));
        const float now[4] = {m.game[0] * kx, m.game[1] * ky, m.game[2], m.game[3]};
        if (committing) std::memcpy(m.now, now, sizeof(now));
        u8 expected[16];
        std::memcpy(expected, reinterpret_cast<const void*>(image_base + site.offset), 16);
        c.markers += slot(site.offset, expected, 16, MarkerSlot, reinterpret_cast<void*>(i), {image_base + site.offset + 16});
    }
    if (hud_anchor_on) c.hud = slot(HudFrameSite, FunctionEntry, sizeof(FunctionEntry), HudSlot, nullptr, {});
    return true;
}

std::uint32_t SceneImmediateSlot(void* imm, GuestRegsEx* r) {
    r->gpr[GUEST_RAX] = u32(reinterpret_cast<std::uintptr_t>(imm));
    r->rip += 5;
    return GUEST_HOOK_JUMP;
}

/// The scene size (BB_RENDER_RES) into the resolution patch's four immediates... the scene's two (the UI's
/// stay 1920x1080): `mov eax, imm24` (0xb8 and the low three bytes of the 32-bit immediate) at the patch's
/// SCENE_WIDTH / SCENE_HEIGHT (scripts/patches.py). The patch file (res-scene.bin) has the 1280x720 template
/// there; sizes already in place (a res-WxH.bin) are left.
void PatchSceneSize(unsigned char* image) {
    const char* env = std::getenv("BB_RENDER_RES");
    unsigned w = 0, h = 0;
    if (!env || std::sscanf(env, "%ux%u", &w, &h) != 2 || !w || !h) return;
    struct Immediate {
        u64 offset;
        u8 opcode;
        unsigned template_value, value;
        const char* name;
    };
    const Immediate immediates[2] = {{0x02196A6B - 0x400000, 0xb8, 1280, w, "width"},
                                     {0x02196A7A - 0x400000, 0xb8, 720, h, "height"}};
    for (const Immediate& im : immediates) {
        u8* code = image + im.offset;
        const unsigned current = code[1] | code[2] << 8 | code[3] << 16;
        if (code[0] != im.opcode || (current != im.template_value && current != im.value)) {
            std::printf("Display: scene %s: unexpected code at %#llx (the resolution patch is missing?): the "
                        "game keeps %ux%u\n", im.name, (unsigned long long)(0x400000 + im.offset), w, h);
            return;
        }
    }
    if (w >= 1u << 24 || h >= 1u << 24) return;
    if (BbHookSites::Available(image_base + immediates[0].offset) && BbHookSites::Available(image_base + immediates[1].offset)) {
        // Native hook slots: the instructions stay as the resolution patch left them, the callback loads eax with the size (what the new
        // immediate would have done: `mov eax, imm32` clears the upper half of rax and leaves the flags).
        for (const Immediate& im : immediates) {
            const u8* code = image + im.offset;
            const u32 imm = u32(im.value) | u32(code[4]) << 24;
            BbHookSites::Slot(image_base + im.offset, image_base, 5, SceneImmediateSlot, reinterpret_cast<void*>(std::uintptr_t(imm)),
                              {image_base + im.offset + 5});
        }
        std::printf("Display: the game's scene size %ux%u (native hook slots at the resolution patch's immediates)\n", w, h);
        return;
    }
    for (const Immediate& im : immediates) {
        u8* code = image + im.offset;
        code[1] = u8(im.value);
        code[2] = u8(im.value >> 8);
        code[3] = u8(im.value >> 16);
    }
    std::printf("Display: the game's scene size %ux%u (the resolution patch's immediates)\n", w, h);
}
} // namespace

void OutputSize(std::uint32_t& width, std::uint32_t& height) {
    width = height = 0;
    const char* env = std::getenv("BB_OUTPUT_RES");
    unsigned w = 0, h = 0;
    if (env && std::sscanf(env, "%ux%u", &w, &h) == 2 && w && h) {
        width = w;
        height = h;
    }
}

float Aspect() {
    std::uint32_t w, h;
    OutputSize(w, h);
    return w && h ? float(w) / float(h) : GameAspect;
}

bool NonNative() {
    static const bool other = [] {
        const char* off = std::getenv("BB_ASPECT_HOOKS");
        if (off && off[0] == '0') return false;
        const float aspect = Aspect();
        return aspect > 0.5f && aspect < 8.0f && std::abs(aspect - GameAspect) > 0.001f;
    }();
    return other;
}

void PatchImage(unsigned char* image, std::uint64_t) {
    image_base = reinterpret_cast<u64>(image);
    PatchSceneSize(image);
    if (!NonNative()) return;
    const float aspect = Aspect();
    if (const char* env = std::getenv("BB_HUD_ANCHOR"); env && env[0] == '0') hud_anchor_on = false;
    u8* data_page = MapNear();
    bool slot_mode = BbHookSites::Available(image_base + SetViewportSite);
    for (const ParamStore& site : CameraParamStores) slot_mode = slot_mode && BbHookSites::Available(image_base + site.offset);
    if (!slot_mode) stub_page = MapNear(); // (the stubs' page: not needed with hook slots; the data page is below 1 TiB for the HUD's strings and values)
    const auto release = [&] {
        if (data_page) munmap(data_page, PageSize);
        if (stub_page) munmap(stub_page, PageSize);
        data_page = stub_page = nullptr;
        data = nullptr;
    };
    if (!data_page || (!slot_mode && !stub_page)) {
        release();
        std::printf("Display: aspect ratio %.3f: no memory next to the game's code, not installed\n", double(aspect));
        return;
    }
    // The stub page is guest code (registered with guest_cpu_code).
    if (stub_page) guest_cpu_code(reinterpret_cast<uintptr_t>(stub_page), PageSize);
    data = new (data_page) Data{};
    Counts counts;
    std::string what;
    committing = false;
    const auto install = [&] { return slot_mode ? InstallAllSlots(counts, what, aspect) : InstallAll(counts, what, aspect); };
    if (!install()) {
        release();
        std::printf("Display: aspect ratio %.3f not installed: %s\n", double(aspect), what.c_str());
        return;
    }
    committing = true;
    if (!install()) {
        std::fprintf(stderr, "STOP: Display: %s after the check\n", what.c_str());
        std::abort();
    }
    if (stub_page) mprotect(stub_page, PageSize, PROT_READ | PROT_EXEC);
    std::printf("Display: aspect ratio %.3f (%s%s): camera at %d of %zu places, the UI's viewport, HUD markers at "
                "%d of %zu, HUD groups %s\n", double(aspect),
                aspect > GameAspect ? "wider than 16:9" : "taller than 16:9", slot_mode ? ", native hook slots" : "", counts.camera,
                CameraParamStores.size(), counts.markers, MarkerSites.size(),
                counts.hud ? "at the output's edges" : "in the 16:9 middle");
}
} // namespace BbDisplay
