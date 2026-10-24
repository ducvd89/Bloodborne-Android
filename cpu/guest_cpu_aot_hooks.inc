// SPDX-License-Identifier: GPL-2.0-or-later
// Included by guest_cpu_aot.cpp, inside its anonymous namespace, after the dirty-unit helpers (DirtyOne, UnitEnd): patch variants and
// native hook slots (docs/RECOMP_ABI.md, "Variants" and "Hook slots").
//
// Variants. A function whose bytes differ from the image the .so was built from is not simply dirty: the .so may hold a variant of it, lowered
// from the patched bytes (bbr_vfuncs[] / bbr_vunits[], keyed by function start and the hash of each unit's bytes). Every time the bytes of a
// function may have changed (the start-up check, guest_cpu_invalidate) the function is classified again, by hashing its units:
//   0 original: every unit hashes as built; the units run (their dirty bytes are cleared again: a patch that was undone, a toggle switched off)
//   1 variant:  the bytes match a variant: the units' dirty bytes stay set, and each unit's entry (bbr_dirty_entry), and the dispatch table, go
//               to the variant's unit of the same address; units the variant lacks run on the fallback
//   2 dirty:    anything else: the whole function runs on the fallback, as before
// A direct unit-to-unit call reaches the variant through the original unit's dirty entry: the dirty byte is set, bbr_dirty_entry finds the
// redirect pointer of the unit. The .so's text is never written.
//
// Hook slots. bbr_hook_sites[] lists the instruction addresses the .so has a slot test at; the unit calls bbr_hook_slot[k] (set here to
// RunSlot) with the guest state and k. A hook is installed by setting its record and the slot, no code is written.

struct FuncRec { uint32_t start, end, first, count; };                 // bbr_funcs[]
struct VFuncRec { uint64_t funcStart; uint32_t first, count, nameOff, pad; };  // bbr_vfuncs[]
struct VUnitRec { uint64_t vaddr, end, ehash; UnitFn fn; int64_t orig; };       // bbr_vunits[]
struct HookSiteRec { uint64_t vaddr; uint32_t len, flags, resumeFirst, resumeCount; };  // bbr_hook_sites[]
constexpr uint32_t HookCompiled = 1, HookSkip = 2, HookRegs = 4;
using SlotFn = uint64_t (*)(GuestState*, uint64_t);

struct HookRec {
    GuestHook regs = nullptr;
    GuestHookEx ex = nullptr;
    void* context = nullptr;
    uintptr_t address = 0;
};

struct VarState {
    bool enabled = false;                    // variants and un-dirtying: needs bbr_funcs, exact unit hashes, and the units' dirty byte
    const FuncRec* funcs = nullptr;
    uint64_t nfuncs = 0;
    const VFuncRec* vfuncs = nullptr;
    uint64_t nvfuncs = 0;
    const VUnitRec* vunits = nullptr;
    const char* vnames = nullptr;
    std::vector<std::pair<uint64_t, uint32_t>> byFunc;   // (function start, variant index), sorted
    std::vector<UnitFn> redirect;                         // per unit: the variant unit entries are redirected to (read without the lock)
    std::vector<uint8_t> locked;                          // per function: a unit the tool started dirty: never touched
    std::vector<int32_t> active;                          // per function: the variant in use, or -1
    std::vector<uint8_t> state;                           // per function: 0 original, 1 variant, 2 dirty
    std::vector<uint32_t> epoch;                          // per function: the resolve epoch it was last classified in
    uint32_t current_epoch = 1;
    std::atomic<uint64_t> to_variant {0}, to_original {0}, to_dirty {0}, redirects {0};
    // hook slots
    const HookSiteRec* sites = nullptr;
    uint64_t nsites = 0;
    const uint64_t* resume = nullptr;
    void** slots = nullptr;
    std::vector<std::atomic<HookRec*>> recs;
    std::atomic<uint64_t> slot_hits {0}, hooks_set {0}, hooks_refused {0};
} V;

uint64_t HashBytes(const unsigned char* p, uint64_t n) {   // the tool's hash: FNV-1a 64, 0x65 hashed as 0x64 (bb-prepare turns fs: into gs:)
    uint64_t h = 1469598103934665603ull;
    for (uint64_t i = 0; i < n; ++i) {
        unsigned char b = p[i];
        if (b == 0x65) b = 0x64;
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}
bool BytesHash(uint64_t vaddr, uint64_t end, uint64_t hash) {
    if (end <= vaddr || end > E.size) return false;
    return HashBytes(reinterpret_cast<const unsigned char*>(E.base + vaddr), end - vaddr) == hash;
}

int64_t TableIndex(uint64_t offset) {  // the unit with this entry address, or -1 (as Lookup)
    const uint32_t key = uint32_t(offset) + 1;
    uint32_t h = (key * 0x9E3779B1u) >> E.shift;
    for (;;) {
        const uint32_t k = E.table[h].key.load(std::memory_order_relaxed);
        if (k == key) return E.table[h].index;
        if (k == 0) return -1;
        h = (h + 1) & E.mask;
    }
}

// The function (index in bbr_funcs) that holds unit i, or -1.
int64_t FuncOfUnit(uint64_t i) {
    const FuncRec* end = V.funcs + V.nfuncs;
    const FuncRec* it = std::upper_bound(V.funcs, end, i, [](uint64_t v, const FuncRec& f) { return v < f.first; });
    if (it == V.funcs) return -1;
    --it;
    return i < uint64_t(it->first) + it->count ? it - V.funcs : -1;
}
int64_t FuncOfAddress(uint64_t vaddr) {
    const FuncRec* end = V.funcs + V.nfuncs;
    const FuncRec* it = std::upper_bound(V.funcs, end, vaddr, [](uint64_t v, const FuncRec& f) { return v < f.start; });
    if (it == V.funcs) return -1;
    --it;
    return vaddr < it->end ? it - V.funcs : -1;
}

void SetUnitsBack(uint64_t f) {  // E.lock held: the original units run again (or the fallback runs them: the caller sets the dirty bytes)
    const FuncRec& fr = V.funcs[f];
    for (uint32_t u = fr.first; u < fr.first + fr.count; ++u) {
        __atomic_store_n(&V.redirect[u], UnitFn(nullptr), __ATOMIC_RELEASE);
        __atomic_store_n(&E.fns[u], E.units[u].fn, __ATOMIC_RELEASE);
    }
    V.active[f] = -1;
}

void SetOriginal(uint64_t f) {  // E.lock held
    const FuncRec& fr = V.funcs[f];
    SetUnitsBack(f);
    for (uint32_t u = fr.first; u < fr.first + fr.count; ++u) {
        if (!E.dirty[u]) continue;
        __atomic_store_n(&E.dirty_flags[u], uint8_t(0), __ATOMIC_RELEASE);
        E.dirty[u] = 0;
        E.dirty_count.fetch_sub(1);
    }
    if (V.state[f] != 0) V.to_original.fetch_add(1);
    V.state[f] = 0;
}

void NoteHooksInactive(uint64_t f) {  // slot hooks in a function that now runs on the fallback do not fire: say so, once per site
    const FuncRec& fr = V.funcs[f];
    for (uint64_t k = 0; k < V.nsites; ++k) {
        if (V.sites[k].vaddr < fr.start || V.sites[k].vaddr >= fr.end || !V.recs[k].load()) continue;
        static std::atomic<uint64_t> told {0};
        if (told.fetch_add(1) < 16)
            std::fprintf(stderr, "AOT: the hook at +%#llx is inactive: its function was patched and runs on the fallback CPU\n", (unsigned long long)V.sites[k].vaddr);
    }
}

void SetDirty(uint64_t f) {  // E.lock held
    const FuncRec& fr = V.funcs[f];
    SetUnitsBack(f);
    for (uint32_t u = fr.first; u < fr.first + fr.count; ++u) DirtyOne(u);
    if (V.state[f] != 2) { V.to_dirty.fetch_add(1); if (V.nsites) NoteHooksInactive(f); }
    V.state[f] = 2;
}

bool VariantMatches(uint32_t vi) {
    const VFuncRec& vf = V.vfuncs[vi];
    for (uint32_t i = 0; i < vf.count; ++i) {
        const VUnitRec& u = V.vunits[vf.first + i];
        if (!BytesHash(u.vaddr, u.end, u.ehash)) return false;
    }
    return true;
}

void Activate(uint64_t f, uint32_t vi) {  // E.lock held
    const FuncRec& fr = V.funcs[f];
    const VFuncRec& vf = V.vfuncs[vi];
    const VUnitRec* vlo = V.vunits + vf.first;
    const VUnitRec* vhi = vlo + vf.count;
    for (uint32_t u = fr.first; u < fr.first + fr.count; ++u) {
        const uint64_t va = E.units[u].vaddr;
        const VUnitRec* it = std::lower_bound(vlo, vhi, va, [](const VUnitRec& x, uint64_t v) { return x.vaddr < v; });
        UnitFn fn = it != vhi && it->vaddr == va ? it->fn : nullptr;
        __atomic_store_n(&V.redirect[u], fn, __ATOMIC_RELEASE);   // the redirect first, then the dirty byte that makes the unit use it
        __atomic_store_n(&E.fns[u], fn ? fn : E.units[u].fn, __ATOMIC_RELEASE);
        DirtyOne(u);
    }
    if (V.state[f] != 1 || V.active[f] != int32_t(vi)) V.to_variant.fetch_add(1);
    V.state[f] = 1;
    V.active[f] = int32_t(vi);
}

// Classifies function f from the bytes in memory now (E.lock held).
void ResolveFunction(uint64_t f) {
    if (V.locked[f]) return;
    if (V.epoch[f] == V.current_epoch) return;
    V.epoch[f] = V.current_epoch;
    const FuncRec& fr = V.funcs[f];
    bool original = true;
    for (uint32_t u = fr.first; u < fr.first + fr.count && original; ++u)
        if (!BytesHash(E.units[u].vaddr, UnitEnd(u), E.ehashes[u])) original = false;
    if (original) { if (V.state[f] != 0 || E.dirty[fr.first]) SetOriginal(f); return; }
    auto it = std::lower_bound(V.byFunc.begin(), V.byFunc.end(), std::make_pair(uint64_t(fr.start), uint32_t(0)));
    for (; it != V.byFunc.end() && it->first == fr.start; ++it)
        if (VariantMatches(it->second)) { Activate(f, it->second); return; }
    SetDirty(f);
}

// The code of unit i may have changed (E.lock held).
void ResolveUnit(uint64_t i) {
    if (!V.enabled) { DirtyUnit(i); return; }
    const int64_t f = FuncOfUnit(i);
    if (f < 0) { DirtyUnit(i); return; }
    ResolveFunction(uint64_t(f));
}
void NewResolveEpoch() { if (V.enabled) ++V.current_epoch; }
// The start-up check: every unit that differs from the .so's image.
void ResolveUnits(const std::vector<uint64_t>& units) {  // E.lock held
    NewResolveEpoch();
    for (uint64_t i : units) ResolveUnit(i);
    if (V.enabled)
        std::printf("AOT: %llu functions run as patch variants, %llu on the fallback CPU (patched without a variant)\n",
                    (unsigned long long)V.to_variant.load(), (unsigned long long)V.to_dirty.load());
}

// bbr_dirty_entry: the variant unit of a dirty unit, or null (the fallback runs it).
inline UnitFn VariantRedirect(uint64_t vaddr) {
    if (!V.enabled) return nullptr;
    const int64_t i = TableIndex(vaddr);
    if (i < 0) return nullptr;
    return __atomic_load_n(&V.redirect[i], __ATOMIC_ACQUIRE);
}

// Reads the .so's variant and hook tables (after the dispatch table was built and the units' dirty bytes were read).
void LoadVariantTables(void* so, uint64_t abi_flags) {
    const auto* funcs = static_cast<const FuncRec*>(dlsym(so, "bbr_funcs"));
    const auto* nfuncs = static_cast<const uint64_t*>(dlsym(so, "bbr_func_count"));
    if (funcs && nfuncs && E.dirty_flags && E.ehashes && E.ends) {
        V.funcs = funcs;
        V.nfuncs = *nfuncs;
        V.redirect.assign(E.count, nullptr);
        V.locked.assign(V.nfuncs, 0);
        V.active.assign(V.nfuncs, -1);
        V.state.assign(V.nfuncs, 0);
        V.epoch.assign(V.nfuncs, 0);
        for (uint64_t u = 0; u < E.count; ++u)
            if (E.dirty[u]) { const int64_t f = FuncOfUnit(u); if (f >= 0) V.locked[f] = 1; }
        V.enabled = true;
    }
    if ((abi_flags & 4) && V.enabled) {
        const auto* vfuncs = static_cast<const VFuncRec*>(dlsym(so, "bbr_vfuncs"));
        const auto* nv = static_cast<const uint64_t*>(dlsym(so, "bbr_vfunc_count"));
        const auto* vunits = static_cast<const VUnitRec*>(dlsym(so, "bbr_vunits"));
        const auto* nvu = static_cast<const uint64_t*>(dlsym(so, "bbr_vunit_count"));
        if (vfuncs && nv && vunits && nvu) {
            V.vfuncs = vfuncs; V.nvfuncs = *nv; V.vunits = vunits;
            V.vnames = static_cast<const char*>(dlsym(so, "bbr_vnames"));
            for (uint64_t i = 0; i < V.nvfuncs; ++i) V.byFunc.push_back({vfuncs[i].funcStart, uint32_t(i)});
            std::sort(V.byFunc.begin(), V.byFunc.end());
            // The variant units are code of the .so too: a fault in one maps to its guest address.
            for (uint64_t i = 0; i < *nvu; ++i) {
                const auto fn = reinterpret_cast<uintptr_t>(vunits[i].fn);
                E.by_address.push_back({fn, uint32_t(vunits[i].vaddr)});
                E.so_lo = std::min(E.so_lo, fn);
                E.so_hi = std::max(E.so_hi, fn + (1 << 20));
            }
            std::sort(E.by_address.begin(), E.by_address.end(), [](const Engine::Where& a, const Engine::Where& b) { return a.fn < b.fn; });
        }
    }
    if (abi_flags & 8) {
        const auto* sites = static_cast<const HookSiteRec*>(dlsym(so, "bbr_hook_sites"));
        const auto* n = static_cast<const uint64_t*>(dlsym(so, "bbr_hook_count"));
        const auto* resume = static_cast<const uint64_t*>(dlsym(so, "bbr_hook_resume"));
        void** slots = static_cast<void**>(dlsym(so, "bbr_hook_slot"));
        if (sites && n && resume && slots && V.enabled) {
            V.sites = sites; V.nsites = *n; V.resume = resume; V.slots = slots;
            std::vector<std::atomic<HookRec*>> recs(*n);
            V.recs.swap(recs);
        }
    }
    std::printf("AOT: patch variants %s (%llu in the .so); native hook slots %s (%llu sites)\n", V.enabled && V.nvfuncs ? "on" : "off",
                (unsigned long long)V.nvfuncs, V.nsites ? "on" : "off", (unsigned long long)V.nsites);
}

// ------------------------------------------------------------------------------------------------ hook slots ----
// The slot function of every installed hook: the unit passes the site index.
uint64_t RunSlot(GuestState* s, uint64_t k) {
    HookRec* r = k < V.nsites ? V.recs[k].load(std::memory_order_acquire) : nullptr;
    if (!r) return GUEST_HOOK_CONTINUE;
    if (E.stats) V.slot_hits.fetch_add(1, std::memory_order_relaxed);
    const uint64_t site = E.base + V.sites[k].vaddr;
    uint64_t sp;
    if (r->regs) {
        GuestRegs regs;
        std::memcpy(regs.gpr, s->gregs, sizeof(regs.gpr));
        regs.rip = site;
        r->regs(&regs);
        std::memcpy(s->gregs, regs.gpr, sizeof(regs.gpr));
        s->rip = regs.rip;
        return regs.rip == site ? uint64_t(GUEST_HOOK_CONTINUE) : uint64_t(GUEST_HOOK_JUMP);
    }
    GuestRegsEx x;
    std::memcpy(x.gpr, s->gregs, sizeof(x.gpr));
    x.rip = site;
    x.rflags = 0x202;
    x.mxcsr = s->mxcsr;
    x.reserved = 0;
    for (unsigned i = 0; i < 16; ++i) {
        std::memcpy(x.ymm[i], s->xmm.sse.data[i], 16);
        std::memcpy(x.ymm[i] + 16, s->avx_high[i], 16);
    }
    const uint32_t act = r->ex(r->context, &x);
    std::memcpy(s->gregs, x.gpr, sizeof(x.gpr));
    for (unsigned i = 0; i < 16; ++i) {
        std::memcpy(s->xmm.sse.data[i], x.ymm[i], 16);
        std::memcpy(s->avx_high[i], x.ymm[i] + 16, 16);
    }
    switch (act) {
    case GUEST_HOOK_RETURN:
        sp = s->gregs[RSP];
        s->rip = *reinterpret_cast<const uint64_t*>(sp);
        s->gregs[RSP] = sp + 8;
        return GUEST_HOOK_RETURN;
    case GUEST_HOOK_JUMP:
        s->rip = x.rip;
        return x.rip == site ? uint64_t(GUEST_HOOK_CONTINUE) : uint64_t(GUEST_HOOK_JUMP);
    default:
        return GUEST_HOOK_CONTINUE;
    }
}

int64_t SiteIndex(uintptr_t address) {
    if (!V.nsites || address < E.base || address - E.base >= E.size) return -1;
    const uint64_t vaddr = address - E.base;
    const HookSiteRec* end = V.sites + V.nsites;
    const HookSiteRec* it = std::lower_bound(V.sites, end, vaddr, [](const HookSiteRec& x, uint64_t v) { return x.vaddr < v; });
    if (it == end || it->vaddr != vaddr || !(it->flags & HookCompiled)) return -1;
    return it - V.sites;
}

// Is the code at the site run by the units (not the fallback)? E.lock held; classifies the function first.
bool SiteRunsNatively(int64_t k) {
    const int64_t f = FuncOfAddress(V.sites[k].vaddr);
    if (f < 0 || V.locked[f]) return false;
    NewResolveEpoch();
    ResolveFunction(uint64_t(f));
    return V.state[f] != 2;
}

int InstallSlot(uintptr_t address, GuestHook regs, GuestHookEx ex, void* context) {
    if (!E.on || !V.nsites) return 0;
    const int64_t k = SiteIndex(address);
    if (k < 0) return 0;
    std::lock_guard<std::mutex> guard(E.lock);
    if (!SiteRunsNatively(k)) { V.hooks_refused.fetch_add(1); return 0; }
    auto* r = new HookRec;
    r->regs = regs; r->ex = ex; r->context = context; r->address = address;
    V.recs[k].store(r, std::memory_order_release);   // (a replaced record is not freed: a unit may be running it)
    __atomic_store_n(&V.slots[k], reinterpret_cast<void*>(static_cast<SlotFn>(&RunSlot)), __ATOMIC_RELEASE);
    V.hooks_set.fetch_add(1);
    return 1;
}
