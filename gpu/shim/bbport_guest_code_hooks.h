// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: observers in a game's own code (any game). At a byte-checked run of whole instructions
// (at least 5 bytes, nothing relative to the instruction pointer) a jump goes to a trampoline that
// saves every general register, the flags, MXCSR and the YMM registers into a snapshot, calls a
// host function with it, puts the (possibly changed) registers back, runs the replaced
// instructions and jumps back. With a skip flag (function entries only) the hook may set it to
// return from the game's function at once.
//
// With ahead-of-time recompiled code (BB_AOT, cpu/guest_cpu_aot.cpp) a site the recompiler has a native hook slot for (tools/recomp/variants/
// hooks-bbsc.txt) is hooked there instead: no trampoline, no code written, the hook is called by the recompiled units (src/guest_cpu.h,
// guest_cpu_hook_slot); on Android other sites are refused, on an x86-64 host they keep the trampoline.
//
// The interface is shadPS4's (core/cpu_patches.h of the shadp2p/BBSC forks), so game modules
// written against it (BBSC's bloodborne_re.cpp) build unchanged.
//
// Where the game runs: on an x86-64 host the host CPU runs it and the trampoline calls the hook directly. On Android the game runs as ahead-of-time
// recompiled units (libbbaot.so) and the game's code is never rewritten: a hook there is a native slot of the library or it is refused (logged), and a
// write into the game's code (GuestCodeWritten) makes the function that holds it be classified again (original, patch variant, or no native code).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#if defined(__x86_64__)
#define BB_GUEST_HOOK_ABI __attribute__((sysv_abi))
#else
// The hook is host code the guest CPU calls with the host ABI (guest_cpu_host_function).
#define BB_GUEST_HOOK_ABI
#endif

namespace Core {

struct alignas(16) GuestRegisterSnapshot {
    std::uint64_t rax;
    std::uint64_t rbx;
    std::uint64_t rcx;
    std::uint64_t rdx;
    std::uint64_t rsi;
    std::uint64_t rdi;
    std::uint64_t rbp;
    std::uint64_t rsp;
    std::uint64_t r8;
    std::uint64_t r9;
    std::uint64_t r10;
    std::uint64_t r11;
    std::uint64_t r12;
    std::uint64_t r13;
    std::uint64_t r14;
    std::uint64_t r15;
    std::uint64_t rflags;
    std::uint32_t mxcsr;
    alignas(16) std::array<std::array<std::uint8_t, 32>, 16> ymm;
};

/// Called on the game's thread, on its stack (keep it small), with the System V ABI.
using GuestCodeHook = void(BB_GUEST_HOOK_ABI*)(std::uint64_t tag,
                                                const GuestRegisterSnapshot* registers);

/// Installs a hook at `address` over `expected_instructions` (checked byte for byte). False: the
/// bytes differ (another build of the game, or a port patch there: the log says which), the span
/// is not whole position-independent instructions, it overlaps a hook, or no trampoline memory is
/// in reach. Nothing is written then.
bool InstallGuestCodeHook(void* address, std::span<const std::uint8_t> expected_instructions,
                          std::uint64_t tag, GuestCodeHook hook,
                          const std::uint8_t* skip_flag = nullptr);

/// Told of a hook refused because the game's bytes are not the expected ones: returns what is known of
/// who changed them (a port patch), appended to the log line; empty when nothing is known.
using GuestCodeMismatchExplainer = std::string (*)(std::uintptr_t address, std::size_t size);
void SetGuestCodeMismatchExplainer(GuestCodeMismatchExplainer function);
/// Hooks installed and refused so far (every refusal is logged with its reason).
unsigned GuestCodeHooksInstalled();
unsigned GuestCodeHooksRefused();

/// The protection the game's code goes back to after the port patches it: read and execute, or
/// read, write and execute once a game module writes its own code (BBSC's byte patches, some
/// switched while the game runs) and calls KeepGuestCodeWritable.
int GuestCodeProtection();
void KeepGuestCodeWritable();

/// The host has just written [address, address + size) of the game's code (a byte patch): the
/// guest CPU drops what it translated of it. Nothing to do on an x86-64 host. Every write of code
/// that can run later needs this (the hooks' own writes do it themselves).
void GuestCodeWritten(const void* address, std::size_t size);

/// Writes `bytes` over the game's code (the pages must be writable: KeepGuestCodeWritable) and
/// calls GuestCodeWritten.
void WriteGuestCode(void* address, const void* bytes, std::size_t size);

/// A call of the game's code with float arguments, which guest_cpu_call (integer arguments only) cannot
/// make: the function at `fn` (System V ABI) gets `ints` (at most 8: six in registers, two on the stack)
/// and `floats` (at most 8, in xmm0-7, as single-precision scalars); returns RAX. Goes through a small
/// guest stub that loads the registers from an argument block (guest_cpu_call of the stub). The calling
/// thread must be able to run guest code (as for guest_cpu_call).
std::uint64_t CallGuestWithFloats(std::uintptr_t fn, std::span<const std::uint64_t> ints,
                                  std::span<const float> floats);

} // namespace Core
