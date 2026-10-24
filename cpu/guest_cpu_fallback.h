/* SPDX-License-Identifier: GPL-2.0-or-later
 * What cpu/guest_cpu_aot.cpp needs from the fallback guest CPU (see guest_cpu_rename.h): the guest_cpu.h interface
 * under the names bb_fb_*, and on FEX builds a few more calls that share FEX's thread state with the ahead-of-time code. */
#ifndef BB_GUEST_CPU_FALLBACK_H
#define BB_GUEST_CPU_FALLBACK_H
#include "guest_cpu.h"
#include "guest_state.h"
#ifdef __cplusplus
extern "C" {
#endif
void bb_fb_init(void *(*map)(size_t size, int prot));
void bb_fb_code(uintptr_t base, size_t size);
uintptr_t bb_fb_host_function(void *fn);
uint64_t bb_fb_call(uintptr_t fn, unsigned count, const uint64_t *args);
void bb_fb_thread_stack(void *top, size_t size);
void bb_fb_set_gs(void *base);
void bb_fb_abandon(void);
void bb_fb_thread_end(void);
uintptr_t bb_fb_return_address(uintptr_t native);
uintptr_t bb_fb_stack_pointer(void);
int bb_fb_signal_regs(const void *ucontext, GuestRegs *regs);
uintptr_t bb_fb_host_pc(const void *ucontext);
int bb_fb_handle_fault(int sig, siginfo_t *info, void *ucontext);
int bb_fb_hook(uintptr_t address, size_t size, GuestHook hook);
void bb_fb_invalidate(uintptr_t address, size_t size);
uint64_t bb_fb_tsc(void);
uint64_t bb_fb_tsc_frequency(void);
#ifdef BB_AOT_FEX_STATE
/* FEX builds only. The calling host thread's guest state (its FEX thread is created on first use). */
GuestState *bb_fb_state(void);
/* 0: the state has the SSE xmm layout the units are compiled for; 1: SVE256 hosts keep it wide, no AOT there. */
int bb_fb_wide_xmm(void);
/* Runs guest code on FEX in `s` (bb_fb_state). `call`: `target` is called, with the return address already at [rsp]; on
 * return rip is that return address and rsp is past it; but when the guest returned elsewhere (longjmp, exception) rip
 * is bb_fb_sentinel(). Not `call`: the code at `target` runs in the frame the stack already holds, to the sentinel at
 * the bottom of the guest stack (a return to it ends the run), and rip is the sentinel. */
void bb_fb_run(GuestState *s, uint64_t target, int call);
/* Runs from `target` in the current frame (rsp unchanged) until the guest returns through the return address slot `slot`
 * (replaced by a trap meanwhile): 1, rip = that return address, rsp past the slot. 0: the guest left otherwise (to the
 * sentinel at the bottom of the stack), rip = bb_fb_sentinel(). */
int bb_fb_run_until(GuestState *s, uint64_t target, uint64_t slot);
/* A guest address that ends a bb_fb_run / returns control to the host when the guest jumps or returns to it. */
uint64_t bb_fb_sentinel(void);
/* The 16-byte host-function stubs: the host function of the stub at base + 16 * i is table[i] (NULL: none). */
void *const *bb_fb_stubs(uintptr_t *base, size_t *bytes);
void bb_fb_cpuid(uint64_t function, uint64_t leaf, uint32_t out[4]);
uint64_t bb_fb_xgetbv(uint64_t function);
/* SIGBUS BUS_ADRALN at an atomic instruction of ahead-of-time code: emulated (the signal's pc is advanced); 0: not handled. */
int bb_fb_handle_unaligned(void *ucontext);
#endif
#ifdef __cplusplus
}
#endif
#endif
