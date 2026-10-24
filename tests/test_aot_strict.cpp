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
#include "aot_test_services.h"

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

    // ---- A miss: message, status 22, the log line with the backtrace ----
    {
        std::remove(logpath);
        ML.tried = false;
        std::string err;
        const int rc = InChild([&] {
            uint64_t frame[2] = {0, E.base + 0x150};            // rbp chain: [rbp] = next (0), [rbp+8] = a return address
            uint64_t stack[2] = {E.base + 0x110, 0};
            GuestState s;
            s.gregs[RSP] = reinterpret_cast<uint64_t>(stack);
            s.gregs[RBP] = reinterpret_cast<uint64_t>(frame);
            s.gregs[RDI] = 1;
            bbr_dispatch(&s, E.base + 0x3f0);                   // no unit, not in a granular function: a miss
        }, err);
        assert(rc == 22);
        assert(err.find("native code is missing an entry at +0x3f0; please report this") != std::string::npos);
        const std::string log = ReadFile(logpath);
        assert(log.find("miss +0x3f0 kind=call") != std::string::npos && log.find("backtrace=+0x110,+0x150") != std::string::npos);
        assert(log.find("# bb aot misses") != std::string::npos);
        // A tail jump into the middle of a unit; a patched function; an address outside the image.
        std::string e2, e3, e4;
        assert(InChild([&] { GuestState s; uint64_t st[2] = {0, 0}; s.gregs[RSP] = reinterpret_cast<uint64_t>(st); s.rip = E.base + 0x230; bbr_dispatch_tail(&s); }, e2) == 22);
        assert(e2.find("missing an entry at +0x230") != std::string::npos && ReadFile(logpath).find("miss +0x230 kind=jump") != std::string::npos);
        assert(InChild([&] { GuestState s; uint64_t st[2] = {0, 0}; s.gregs[RSP] = reinterpret_cast<uint64_t>(st); Miss(&s, E.base + 0x200, Current(), true); }, e3) == 22);
        assert(e3.find("patched or hooked function has no variant at +0x200") != std::string::npos);
        assert(InChild([&] { GuestState s; uint64_t st[2] = {0, 0}; s.gregs[RSP] = reinterpret_cast<uint64_t>(st); bbr_dispatch(&s, 0x1000); }, e4) == 22);
        assert(e4.find("outside the image") != std::string::npos);
    }
    // ---- The game's panic trap: `call print ; movl $0xdeadba, 0x0` is not a missing entry ----
    {
        static const uint8_t trap[] = {0xc7, 0x04, 0x25, 0, 0, 0, 0, 0xba, 0xad, 0xde, 0x00};   // movl $0xdeadba, 0x0
        static const uint8_t call[] = {0xe8, 0x10, 0x20, 0x30, 0x40};                            // call rel32 (its return address is the next byte)
        std::memcpy(image + 0x2fb, call, 5); std::memcpy(image + 0x300, trap, sizeof trap);      // after a call
        std::memset(image + 0x31b, 0x90, 5); std::memcpy(image + 0x320, trap, sizeof trap);      // after a nop
        static const uint8_t reg[] = {0x48, 0x89, 0x04, 0x25, 0, 0, 0, 0};                       // mov [0], rax
        std::memcpy(image + 0x33b, call, 5); std::memcpy(image + 0x340, reg, sizeof reg);
        static const uint8_t other[] = {0xc7, 0x04, 0x25, 0x08, 0, 0, 0, 1, 0, 0, 0};            // movl $1, 0x8: not address 0
        std::memcpy(image + 0x35b, call, 5); std::memcpy(image + 0x360, other, sizeof other);
        assert(StoresToAddressZero(image + 0x300) && StoresToAddressZero(image + 0x320) && StoresToAddressZero(image + 0x340) && !StoresToAddressZero(image + 0x360));
        E.panic_text = [](char* out, size_t size) -> size_t { return size_t(std::snprintf(out, size, "[Dantelion2 Panic] DLRegularHeap.cpp(710) | given memory block seems to be improper")); };
        std::remove(logpath);
        ML.tried = false;
        std::string e1, e2, e3, e4, e5;
        auto stk = [](GuestState& s, uint64_t* st) { st[0] = 0; st[1] = 0; s.gregs[RSP] = reinterpret_cast<uint64_t>(st); };
        // a call's return address, reached by a dispatch (a host function returned there) / as an unwind (kind=unwind, what the device logged)
        assert(InChild([&] { GuestState s; uint64_t st[2]; stk(s, st); bbr_dispatch(&s, E.base + 0x300); }, e1) == 22);
        assert(e1.find("STOP: the game stopped itself after an internal error (panic) at +0x300: [Dantelion2 Panic] DLRegularHeap.cpp(710) | given memory block seems to be improper") != std::string::npos);
        assert(e1.find("no native code is missing") != std::string::npos && e1.find("missing an entry") == std::string::npos);
        const std::string log = ReadFile(logpath);
        assert(log.find("panic +0x300 kind=call") != std::string::npos && log.find("text=\"[Dantelion2 Panic]") != std::string::npos && log.find("miss +0x300") == std::string::npos);
        assert(InChild([&] { GuestState s; uint64_t st[2]; stk(s, st); NoteMiss(&s, E.base + 0x320, Current(), MissKind::Unwind); }, e2) == 22);   // an unwind needs no call before it
        assert(e2.find("the game stopped itself after an internal error (panic) at +0x320") != std::string::npos);
        assert(ReadFile(logpath).find("panic +0x320 kind=unwind") != std::string::npos);
        assert(InChild([&] { GuestState s; uint64_t st[2]; stk(s, st); bbr_dispatch(&s, E.base + 0x340); }, e3) == 22);   // a register store
        assert(e3.find("(panic) at +0x340") != std::string::npos);
        // not the trap: a jump (no call before it), a store to another address
        assert(InChild([&] { GuestState s; uint64_t st[2]; stk(s, st); bbr_dispatch(&s, E.base + 0x320); }, e4) == 22);
        assert(e4.find("native code is missing an entry at +0x320") != std::string::npos && e4.find("panic") == std::string::npos);
        assert(InChild([&] { GuestState s; uint64_t st[2]; stk(s, st); bbr_dispatch(&s, E.base + 0x360); }, e5) == 22);
        assert(e5.find("native code is missing an entry at +0x360") != std::string::npos);
        // no text captured: the message sends the reader to the game's own log lines
        E.panic_text = nullptr;
        std::string e6;
        assert(InChild([&] { GuestState s; uint64_t st[2]; stk(s, st); bbr_dispatch(&s, E.base + 0x300); }, e6) == 22);
        assert(e6.find("(panic) at +0x300; its own message is above in the log") != std::string::npos);
    }
    std::remove(logpath);
    std::puts("aot strict test: ok");
    return 0;
}
#endif
