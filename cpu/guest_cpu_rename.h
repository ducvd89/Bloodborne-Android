/* SPDX-License-Identifier: GPL-2.0-or-later
 * The fallback guest CPU (FEXCore, or the host CPU running the game's x86 code natively) is compiled with this header
 * included before guest_cpu.h: its guest_cpu_* functions become bb_fb_*, and cpu/guest_cpu_aot.cpp, which exports the
 * real guest_cpu_*, calls them for whatever the ahead-of-time units do not cover. Without BB_AOT_FALLBACK (tests that
 * link the fallback alone) nothing is renamed. */
#ifndef BB_GUEST_CPU_RENAME_H
#define BB_GUEST_CPU_RENAME_H
#ifdef BB_AOT_FALLBACK
#define guest_cpu_init bb_fb_init
#define guest_cpu_code bb_fb_code
#define guest_cpu_host_function bb_fb_host_function
#define guest_cpu_call bb_fb_call
#define guest_cpu_thread_stack bb_fb_thread_stack
#define guest_cpu_set_gs bb_fb_set_gs
#define guest_cpu_abandon bb_fb_abandon
#define guest_cpu_thread_end bb_fb_thread_end
#define guest_cpu_return_address bb_fb_return_address
#define guest_cpu_stack_pointer bb_fb_stack_pointer
#define guest_cpu_signal_regs bb_fb_signal_regs
#define guest_cpu_host_pc bb_fb_host_pc
#define guest_cpu_handle_fault bb_fb_handle_fault
#define guest_cpu_hook bb_fb_hook
#define guest_cpu_invalidate bb_fb_invalidate
#define guest_cpu_tsc bb_fb_tsc
#define guest_cpu_tsc_frequency bb_fb_tsc_frequency
#endif
#endif
