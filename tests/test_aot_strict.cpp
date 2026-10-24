// SPDX-License-Identifier: GPL-2.0-or-later
// cpu/guest_cpu_aot.cpp: granular entries (the dispatcher asks a granular function's units one by one with the resume flag), the misses log, strict
// mode (the message, the exit status, the log line with thread and guest backtrace) and the import-trap peephole, on synthetic tables.
//   c++ -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -Icpu -Isrc tests/test_aot_strict.cpp -ldl -o out/aot-strict-test
#undef NDEBUG
#include "../cpu/guest_cpu_aot.cpp"

#include <cassert>
#include <sys/mman.h>
#include <sys/wait.h>

#if !defined(__x86_64__)
int main() { std::puts("aot strict test: x86-64 only"); return 0; }
#else
extern "C" {
void bb_fb_init(void* (*)(size_t, int)) {}
void bb_fb_code(uintptr_t, size_t) {}
uintptr_t bb_fb_host_function(void* fn) { return uintptr_t(fn); }
uint64_t bb_fb_call(uintptr_t, unsigned, const uint64_t*) { return 0; }
void bb_fb_thread_stack(void*, size_t) {}
void bb_fb_set_gs(void*) {}
void bb_fb_abandon(void) {}
void bb_fb_thread_end(void) {}
uintptr_t bb_fb_return_address(uintptr_t n) { return n; }
uintptr_t bb_fb_stack_pointer(void) { return 0; }
int bb_fb_signal_regs(const void*, GuestRegs*) { return 0; }
uintptr_t bb_fb_host_pc(const void*) { return 0; }
int bb_fb_handle_fault(int, siginfo_t*, void*) { return 0; }
int bb_fb_hook(uintptr_t, size_t, GuestHook) { return 0; }
void bb_fb_invalidate(uintptr_t, size_t) {}
uint64_t bb_fb_tsc(void) { return 0; }
uint64_t bb_fb_tsc_frequency(void) { return 1; }
}

namespace {
constexpr uint64_t ImageSize = 0x1000;
uint8_t* image;
uint8_t* traps;

// A granular unit: on a resume request it takes the addresses it lists (clears the flag, marks rax with the address offset) and leaves the flag set
// for any other address; an ordinary entry marks rax with 1.
struct GUnit { uint64_t a, b; };
template <int K> void GranularUnit(GuestState* s) {
    static constexpr GUnit mine[2] = {{0x104, 0x108}, {0x124, 0x128}};
    if (s->_pad1) {
        const uint64_t off = s->rip - E.base;
        if (off == mine[K].a || off == mine[K].b) { s->_pad1 = 0; s->gregs[RAX] = off; return; }
        return;   // not here: the flag stays set
    }
    s->gregs[RAX] = 1;
}
void Plain(GuestState* s) { s->gregs[RAX] = 2; }

UnitEntry units[3];
uint64_t ends[3] = {0x120, 0x140, 0x240}, ehashes[3], hashes[3];
uint8_t extra[3] = {0, 1, 0}, dirty[3];
GranRec grecs[2];
uint8_t gflags[2] = {1, 0};

std::string ReadFile(const char* path) {
    std::string out;
    if (FILE* f = std::fopen(path, "r")) { char b[512]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) out.append(b, n); std::fclose(f); }
    return out;
}

// Runs `fn` in a forked child with stderr captured: returns the exit status, `err` the child's stderr.
template <class F> int InChild(F fn, std::string& err) {
    int fds[2];
    assert(pipe(fds) == 0);
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 2); close(fds[0]); close(fds[1]);
        fn();
        _exit(0);
    }
    close(fds[1]);
    char b[1024]; ssize_t n;
    while ((n = read(fds[0], b, sizeof b)) > 0) err.append(b, size_t(n));
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
}  // namespace

int main() {
    image = static_cast<uint8_t*>(mmap(nullptr, ImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    traps = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(image != MAP_FAILED && traps != MAP_FAILED);
    // F0 (granular): unit 0 at 0x100 and a case-body unit at 0x120, end 0x140; F1 (not granular): unit at 0x200.
    units[0] = {0x100, GranularUnit<0>}; units[1] = {0x120, GranularUnit<1>}; units[2] = {0x200, Plain};
    grecs[0] = {0x100, 0x140, 0, 2}; grecs[1] = {0x200, 0x240, 2, 1};
    G.funcs = grecs; G.nfuncs = 2; G.flags = gflags;
    E.base = reinterpret_cast<uintptr_t>(image);
    E.size = ImageSize;
    E.units = units; E.count = 3; E.extra = extra; E.ends = ends; E.ehashes = ehashes; E.hashes = hashes;
    E.text_end = ImageSize;
    E.dirty_flags = dirty;
    E.on = true;
    assert(BuildTable());
    E.ranges.emplace_back(E.base, ImageSize);
    E.ranges.emplace_back(reinterpret_cast<uintptr_t>(traps), 4096);
    char logpath[] = "/tmp/aot-strict-test-XXXXXX";
    const int lfd = mkstemp(logpath);
    assert(lfd >= 0);
    close(lfd);
    E.miss_log = logpath;

    // ---- Granular entries ----
    {
        GuestState s;
        s.gregs[RAX] = 0;
        bbr_dispatch(&s, E.base + 0x100);                       // a unit entry: the ordinary path
        assert(s.gregs[RAX] == 1);
        s.gregs[RAX] = 0;
        bbr_dispatch(&s, E.base + 0x104);                       // not a unit entry, a boundary of unit 0
        assert(s.gregs[RAX] == 0x104 && s._pad1 == 0);
        s.gregs[RAX] = 0;
        s.rip = E.base + 0x124;
        bbr_dispatch_tail(&s);                                  // a boundary of unit 1: unit 0 declines (flag stays), unit 1 takes it
        assert(s.gregs[RAX] == 0x124 && s._pad1 == 0);
        s.gregs[RAX] = 0;
        Thread* t = Current();
        assert(!GranularEntry(&s, E.base + 0x130, t) && s._pad1 == 0);   // inside the function but nobody's boundary: declined, flag cleared
        assert(!GranularEntry(&s, E.base + 0x204, t));          // F1 is not granular
        assert(!GranularEntry(&s, E.base + 0x900, t));          // in no function
    }

    // ---- The import-trap peephole ----
    {
        unsigned char* t = traps + 64;
        const uint32_t index = 77;
        const uint64_t handler = 0x7000123456ull;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; t[3] = 0xbf; std::memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; std::memcpy(t + 10, &handler, 8); t[18] = 0xff; t[19] = 0xe0;
        GuestState s;
        s.gregs[RDI] = 0xdead;
        assert(TrapHandler(&s, reinterpret_cast<uint64_t>(t)) == handler && s.gregs[RSI] == 0xdead && s.gregs[RDI] == 77);
        t[19] = 0xe1;                                           // not the pattern: not ours
        assert(TrapHandler(&s, reinterpret_cast<uint64_t>(t)) == 0);
        assert(TrapHandler(&s, reinterpret_cast<uint64_t>(t) + 8) == 0);   // not a 32-byte record start
    }

    // ---- Non-strict: the miss is logged once per address, the program goes on ----
    {
        GuestState s;
        uint64_t stack[4] = {E.base + 0x110, 0, 0, 0};
        s.gregs[RSP] = reinterpret_cast<uint64_t>(stack);
        assert(!E.strict);
        NoteMiss(&s, E.base + 0x3f0, Current(), MissKind::Jump);
        NoteMiss(&s, E.base + 0x3f0, Current(), MissKind::Jump);
        NoteMiss(&s, E.base + 0x3f8, Current(), MissKind::Call);
        const std::string log = ReadFile(logpath);
        assert(log.find("miss +0x3f0 kind=jump thread=") != std::string::npos);
        assert(log.find("backtrace=+0x110") != std::string::npos);
        assert(log.find("miss +0x3f8 kind=call") != std::string::npos);
        assert(log.find("miss +0x3f0", log.find("miss +0x3f0") + 1) == std::string::npos);   // the second miss at +0x3f0 was not logged again
    }

    // ---- Strict: message, status 22, the log line with the backtrace ----
    {
        std::remove(logpath);
        ML.tried = false; ML.seen.clear();
        std::string err;
        const int rc = InChild([&] {
            E.strict = true;
            uint64_t frame[2] = {0, E.base + 0x150};            // rbp chain: [rbp] = next (0), [rbp+8] = a return address
            uint64_t stack[2] = {E.base + 0x110, 0};
            GuestState s;
            s.gregs[RSP] = reinterpret_cast<uint64_t>(stack);
            s.gregs[RBP] = reinterpret_cast<uint64_t>(frame);
            s.gregs[RDI] = 1;
            bbr_dispatch(&s, E.base + 0x3f0);                   // no unit, not in a granular function: a miss
        }, err);
        assert(rc == 22);
        assert(err.find("native code is missing an entry at +0x3f0; please report this, or turn Native CPU off") != std::string::npos);
        const std::string log = ReadFile(logpath);
        assert(log.find("miss +0x3f0 kind=call") != std::string::npos && log.find("backtrace=+0x110,+0x150") != std::string::npos);
        assert(log.find("# bb aot misses") != std::string::npos);
        // A tail jump into the middle of a unit; a patched function; an address outside the image.
        std::string e2, e3, e4;
        assert(InChild([&] { E.strict = true; GuestState s; uint64_t st[2] = {0, 0}; s.gregs[RSP] = reinterpret_cast<uint64_t>(st); s.rip = E.base + 0x230; bbr_dispatch_tail(&s); }, e2) == 22);
        assert(e2.find("missing an entry at +0x230") != std::string::npos && ReadFile(logpath).find("miss +0x230 kind=jump") != std::string::npos);
        assert(InChild([&] { E.strict = true; GuestState s; uint64_t st[2] = {0, 0}; s.gregs[RSP] = reinterpret_cast<uint64_t>(st); Miss(&s, E.base + 0x200, Current(), true); }, e3) == 22);
        assert(e3.find("patched or hooked function has no variant at +0x200") != std::string::npos);
        assert(InChild([&] { E.strict = true; GuestState s; uint64_t st[2] = {0, 0}; s.gregs[RSP] = reinterpret_cast<uint64_t>(st); bbr_dispatch(&s, 0x1000); }, e4) == 22);
        assert(e4.find("outside the image") != std::string::npos);
    }
    std::remove(logpath);
    std::puts("aot strict test: ok");
    return 0;
}
#endif
