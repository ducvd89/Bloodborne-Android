// SPDX-License-Identifier: GPL-2.0-or-later
// cpu/guest_cpu_aot.cpp's patch variants and native hook slots (cpu/guest_cpu_aot_hooks.inc), on synthetic tables: no .so, no game.
// The engine source is included, so its internals can be set up by hand: a few "units" (functions that leave a marker in rax), an image
// whose bytes the test changes, and the tables a bb-recomp .so would export (bbr_funcs, bbr_vfuncs, bbr_vunits, bbr_hook_sites).
//   c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -Icpu -Isrc tests/test_aot_variants.cpp -ldl -o out/aot-variants-test
#undef NDEBUG
#include "../cpu/guest_cpu_aot.cpp"

#include <cassert>
#include <sys/mman.h>

#if !defined(__x86_64__)
int main() { std::puts("aot variants test: x86-64 only"); return 0; }
#else

#include "aot_test_services.h"

namespace {
constexpr uint64_t ImageSize = 0x1000;
uint8_t* image;

// Units: each leaves its marker in rax. Originals 1x, variants 1xx.
#define MARK(name, v) void name(GuestState* s) { s->gregs[RAX] = v; }
MARK(orig0, 10) MARK(orig1, 11) MARK(orig2, 20) MARK(orig3, 30)
MARK(var0, 110) MARK(var1, 111) MARK(var0b, 120)
#undef MARK

UnitEntry units[4];
uint64_t ends[4], ehashes[4], hashes[4];
uint8_t extra[4] = {0, 1, 0, 0}, dirty[4];
FuncRec funcs[3];
VFuncRec vfuncs[2];
VUnitRec vunits[3];
HookSiteRec sites[2];
uint64_t resume[2] = {0, 0};
void* slots[2];

void SetBytes(uint64_t at, uint8_t value) { image[at] = value; }

void Setup() {
    image = static_cast<uint8_t*>(mmap(nullptr, ImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(image != MAP_FAILED);
    for (unsigned i = 0; i < ImageSize; ++i) image[i] = uint8_t(0x90 + (i & 1));
    // F0: unit 0 at 0x100 (+ unit 1, a case body at 0x108), end 0x110; F1: unit 2 at 0x200; F2: unit 3 at 0x300 (the tool started it dirty)
    units[0] = {0x100, orig0}; units[1] = {0x108, orig1}; units[2] = {0x200, orig2}; units[3] = {0x300, orig3};
    ends[0] = 0x110; ends[1] = 0x110; ends[2] = 0x210; ends[3] = 0x310;
    dirty[3] = 1;
    funcs[0] = {0x100, 0x110, 0, 2}; funcs[1] = {0x200, 0x210, 2, 1}; funcs[2] = {0x300, 0x310, 3, 1};
    E.base = reinterpret_cast<uintptr_t>(image);
    E.size = ImageSize;
    E.units = units; E.count = 4; E.extra = extra; E.ends = ends; E.ehashes = ehashes; E.hashes = hashes;
    E.text_end = ImageSize;
    E.dirty_flags = dirty;
    E.on = true;
    assert(BuildTable());
    for (unsigned i = 0; i < 4; ++i) {
        ehashes[i] = HashBytes(image + units[i].vaddr, ends[i] - units[i].vaddr);
        if (dirty[i]) { E.dirty[i] = 1; ++E.trap_units; }
    }
    V.funcs = funcs; V.nfuncs = 3;
    V.redirect.assign(4, nullptr); V.locked.assign(3, 0); V.active.assign(3, -1); V.state.assign(3, 0); V.epoch.assign(3, 0);
    V.locked[2] = 1;
    V.enabled = true;
    // The variant of F0 for the bytes 0x91 at 0x104: its units 0x100 and 0x108. Another one, for 0x92 at 0x104, has only unit 0x100.
    SetBytes(0x104, 0x91);
    vunits[0] = {0x100, 0x110, HashBytes(image + 0x100, 0x10), var0, 0};
    vunits[1] = {0x108, 0x110, HashBytes(image + 0x108, 0x8), var1, 1};
    SetBytes(0x104, 0x92);
    vunits[2] = {0x100, 0x110, HashBytes(image + 0x100, 0x10), var0b, 0};
    SetBytes(0x104, uint8_t(0x90 + (0x104 & 1)));
    vfuncs[0] = {0x100, 0, 2, 0, 0}; vfuncs[1] = {0x100, 2, 1, 0, 0};
    V.vfuncs = vfuncs; V.nvfuncs = 2; V.vunits = vunits;
    V.byFunc = {{0x100, 0}, {0x100, 1}};
    // Hook sites: 0x108 (in F0) and 0x200 (the entry of F1, a function entry: skip allowed)
    sites[0] = {0x108, 4, HookCompiled, 0, 0};
    sites[1] = {0x200, 6, HookCompiled | HookSkip, 0, 0};
    V.sites = sites; V.nsites = 2; V.resume = resume; V.slots = slots;
    std::vector<std::atomic<HookRec*>> recs(2);
    V.recs.swap(recs);
}

uint64_t Run(uint64_t vaddr) {  // the entry of a unit, as a direct call reaches it: through its dirty check
    GuestState s;
    s.gregs[RAX] = 0;
    const int64_t i = TableIndex(vaddr);
    assert(i >= 0);
    if (E.dirty_flags[i]) bbr_dirty_entry(&s, vaddr);
    else units[i].fn(&s);
    return s.gregs[RAX];
}
uint64_t Dispatched(uint64_t vaddr) {  // through the dispatch table (an indirect call)
    GuestState s;
    s.gregs[RAX] = 0;
    UnitFn fn = Lookup(vaddr);
    assert(fn);
    fn(&s);
    return s.gregs[RAX];
}
void Changed(uint64_t at, uint8_t value) {  // the host writes a byte of code and tells the guest CPU
    image[at] = value;
    guest_cpu_invalidate(E.base + at, 1);
}
}  // namespace

static uint32_t g_ex_act = GUEST_HOOK_CONTINUE;
static GuestRegsEx g_ex_seen;
static uint32_t ExHook(void* context, GuestRegsEx* r) {
    assert(context == reinterpret_cast<void*>(0x42));
    g_ex_seen = *r;
    r->gpr[GUEST_RBX] += 5;
    r->ymm[3][0] = 0x33;
    if (g_ex_act == GUEST_HOOK_JUMP) r->rip = E.base + 0x20c;
    return g_ex_act;
}
static void RegsHook(GuestRegs* r) { r->gpr[GUEST_RDX] = 99; r->rip = E.base + 0x10c; }

int main() {
    Setup();
    // The pristine image: everything runs as built, F2 (a trap unit) stays dirty.
    {
        std::vector<uint64_t> none;
        std::lock_guard<std::mutex> g(E.lock);
        ResolveUnits(none);
    }
    assert(Run(0x100) == 10 && Run(0x200) == 20 && E.dirty[3] && E.dirty_flags[3]);

    // A patch the catalog has a variant for: the bytes at 0x104 become 0x91 (the guest CPU is told afterwards).
    Changed(0x104, 0x91);
    assert(V.state[0] == 1 && V.active[0] == 0);
    assert(E.dirty_flags[0] && E.dirty_flags[1]);              // the original units redirect
    assert(Run(0x100) == 110 && Run(0x108) == 111);           // direct calls reach the variant through bbr_dirty_entry
    assert(Dispatched(0x100) == 110 && Dispatched(0x108) == 111);  // the dispatch table goes there at once
    assert(Run(0x200) == 20);                                  // other functions are untouched

    // The same function with the other variant's bytes.
    Changed(0x104, 0x92);
    assert(V.state[0] == 1 && V.active[0] == 1);
    assert(Run(0x100) == 120);
    assert(E.fns[1] == units[1].fn && E.dirty_flags[1]);       // the variant lacks unit 0x108: it has no native code (dirty, no redirect)
    assert(VariantRedirect(0x108) == nullptr);

    // A patch nobody built a variant for: the whole function is dirty.
    Changed(0x104, 0x55);
    assert(V.state[0] == 2 && E.dirty_flags[0] && E.dirty_flags[1] && VariantRedirect(0x100) == nullptr && E.fns[0] == units[0].fn);

    // Back to a variant, and then the original bytes: the dirty bytes are cleared, the units run again.
    Changed(0x104, 0x91);
    assert(V.state[0] == 1 && Run(0x100) == 110);
    Changed(0x104, uint8_t(0x90 + (0x104 & 1)));
    assert(V.state[0] == 0 && !E.dirty_flags[0] && !E.dirty_flags[1] && Run(0x100) == 10 && Run(0x108) == 11 && Dispatched(0x108) == 11);

    // A function with a unit the tool started dirty is never touched.
    Changed(0x304, 0x77);
    assert(V.state[2] == 0 && E.dirty_flags[3]);
    Changed(0x304, uint8_t(0x90 + (0x304 & 1)));
    assert(E.dirty_flags[3]);

    // A function that the start-up check finds patched: resolved once for all its bad units.
    image[0x104] = 0x91;
    {
        std::vector<uint64_t> bad = {0, 1};
        std::lock_guard<std::mutex> g(E.lock);
        ResolveUnits(bad);
    }
    assert(V.state[0] == 1 && Run(0x108) == 111);
    Changed(0x104, uint8_t(0x90 + (0x104 & 1)));
    assert(V.state[0] == 0);

    // ---- Hook slots ----
    assert(guest_cpu_has_hook_slot(E.base + 0x108) && guest_cpu_has_hook_slot(E.base + 0x200));
    assert(!guest_cpu_has_hook_slot(E.base + 0x104));         // not a site
    assert(!guest_cpu_has_hook_slot(E.base + 0x300));         // not a site (and its function is dirty)
    g_ex_act = GUEST_HOOK_CONTINUE;
    assert(guest_cpu_hook_slot(E.base + 0x108, 4, ExHook, reinterpret_cast<void*>(0x42)) == 1);
    assert(slots[0] == reinterpret_cast<void*>(static_cast<SlotFn>(&RunSlot)) && slots[1] == nullptr);
    {
        GuestState s;
        s.gregs[RBX] = 7; s.gregs[RSP] = 0x1000; s.xmm.sse.data[3][0] = 0; s.avx_high[2][1] = 0xabc; s.xmm.sse.data[1][1] = 0xdef;
        const uint64_t act = RunSlot(&s, 0);
        assert(act == GUEST_HOOK_CONTINUE);
        assert(g_ex_seen.rip == E.base + 0x108 && g_ex_seen.gpr[GUEST_RBX] == 7 && g_ex_seen.gpr[GUEST_RSP] == 0x1000);
        uint64_t hi; std::memcpy(&hi, g_ex_seen.ymm[2] + 24, 8);
        uint64_t lo; std::memcpy(&lo, g_ex_seen.ymm[1] + 8, 8);
        assert(hi == 0xabc && lo == 0xdef);                    // the ymm: xmm low half, avx_high upper
        assert(s.gregs[RBX] == 12 && s.xmm.sse.data[3][0] == 0x33);   // changes come back
        g_ex_act = GUEST_HOOK_JUMP;
        assert(RunSlot(&s, 0) == GUEST_HOOK_JUMP && s.rip == E.base + 0x20c);
        g_ex_act = GUEST_HOOK_RETURN;
        uint64_t stack[2] = {0x1234, 0};
        s.gregs[RSP] = reinterpret_cast<uint64_t>(stack);
        assert(RunSlot(&s, 0) == GUEST_HOOK_RETURN && s.rip == 0x1234 && s.gregs[RSP] == reinterpret_cast<uint64_t>(stack) + 8);
    }
    // guest_cpu_hook (a GuestHook that replaces the instruction): the slot is set, no code written, rip set by the hook means a jump.
    {
        assert(guest_cpu_hook(E.base + 0x200, 2, RegsHook) == 1);
        GuestState s;
        assert(RunSlot(&s, 1) == GUEST_HOOK_JUMP && s.rip == E.base + 0x10c && s.gregs[RDX] == 99);
    }
    // A hook at a site that is no slot is refused (logged): the game's code is never rewritten.
    {
        assert(guest_cpu_hook(E.base + 0x204, 2, RegsHook) == 0);
    }
    // A function without native code refuses a slot; when its code is a variant or original again the slot works (and was kept).
    guest_cpu_hook_slot_clear(E.base + 0x108);
    Changed(0x104, 0x55);
    assert(V.state[0] == 2 && !guest_cpu_has_hook_slot(E.base + 0x108));
    assert(guest_cpu_hook_slot(E.base + 0x108, 4, ExHook, reinterpret_cast<void*>(0x42)) == 0);
    Changed(0x104, 0x91);
    assert(V.state[0] == 1 && guest_cpu_has_hook_slot(E.base + 0x108));   // variants carry the slot tests too
    // Cleared slots do nothing.
    guest_cpu_hook_slot_clear(E.base + 0x200);
    {
        GuestState s;
        assert(RunSlot(&s, 1) == GUEST_HOOK_CONTINUE);
    }
    std::puts("aot variants test: ok");
    return 0;
}
#endif
