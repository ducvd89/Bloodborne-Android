/* Minimal x86-64 loader experiment (the game's code runs natively on x86-64 hosts, through
 * ahead-of-time recompiled native code elsewhere: guest_cpu.h). Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#if defined(__x86_64__)
#include <cpuid.h>
#endif
#include "runtime.h"
#include "guest_cpu.h"
#include "gpu/bbgpu.h"
#if !defined(__GNUC__) || (!defined(__x86_64__) && defined(_WIN32))
#error This prototype requires GCC or Clang, on x86-64 for MinGW.
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#ifdef __ANDROID__
#include <unwind.h> /* Bionic has no backtrace() */
#else
#include <execinfo.h>
#endif
#include <pthread.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/prctl.h>
#include <errno.h>
#endif

#if defined(__ANDROID__) && !defined(_WIN32)
typedef struct { void **frames; int count, max; } UnwindState;
static _Unwind_Reason_Code unwind_step(struct _Unwind_Context *context, void *argument) {
    UnwindState *state = argument;
    uintptr_t pc = _Unwind_GetIP(context);
    if (!pc) return _URC_END_OF_STACK;
    if (state->count >= state->max) return _URC_END_OF_STACK;
    state->frames[state->count++] = (void *)pc;
    return _URC_NO_REASON;
}
static int backtrace(void **frames, int max) {
    UnwindState state = {frames, 0, max};
    _Unwind_Backtrace(unwind_step, &state);
    return state.count;
}
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }

/* The game's code was compiled for the PS4's CPU (AMD Jaguar) and runs as it is: AVX, BMI1
 * (andn/bextr/blsr/tzcnt), MOVBE, LZCNT and POPCNT, thousands of each in eboot.bin. A CPU without
 * them stops at the first one with SIGILL (exit code 132, issue #26), and lzcnt/tzcnt even run as
 * bsr/bsf there, with other results. Said before the game starts; BB_SKIP_CPU_CHECK=1 skips it. */
#if defined(__x86_64__)
static void check_cpu(void) {
    const char *skip = getenv("BB_SKIP_CPU_CHECK");
    if (skip && !strcmp(skip, "1")) return;
    unsigned a, b, c, d, leaf1_c = 0, leaf7_b = 0, ext1_c = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) leaf1_c = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) leaf7_b = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) ext1_c = c;
    int avx = (leaf1_c & bit_AVX) && (leaf1_c & bit_OSXSAVE);
    if (avx) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        avx = (lo & 6) == 6; /* the OS saves SSE and AVX state */
    }
    const struct { int present; const char *name; } features[] = {
        {avx, "AVX"}, {(leaf7_b & bit_BMI) != 0, "BMI1"}, {(leaf1_c & bit_MOVBE) != 0, "MOVBE"},
        {(ext1_c & bit_LZCNT) != 0, "LZCNT"}, {(leaf1_c & bit_POPCNT) != 0, "POPCNT"},
        {(leaf1_c & bit_SSE4_2) != 0, "SSE4.2"},
    };
    char missing[64] = "";
    for (unsigned i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
        if (features[i].present) continue;
        if (missing[0]) strcat(missing, ", ");
        strcat(missing, features[i].name);
    }
    if (!missing[0]) return;
    fprintf(stderr,
            "ERROR: this CPU lacks %s. Bloodborne's code was compiled for the PS4's CPU and uses "
            "these instructions directly: it needs an Intel Haswell (4th generation Core, 2013) or "
            "newer, or an AMD Ryzen. BB_SKIP_CPU_CHECK=1 starts anyway.\n", missing);
    exit(1);
}
#else
/* Other hosts: the recompiled code does not use the host's x86 features. */
static void check_cpu(void) {}
#endif
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
#ifndef _WIN32
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#endif
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    uintptr_t caller=guest_cpu_return_address((uintptr_t)__builtin_return_address(0))-(uintptr_t)image;
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n", caller, argument);
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#if !defined(_WIN32) && defined(GUEST_CPU_NATIVE)
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
#ifndef _WIN32
/* ---- Fault entry (bb_fault_entry): faults the kernel would otherwise kill silently ----
 * A SIGSEGV/SIGBUS the kernel cannot deliver kills the process with no output (exit 139, no
 * tombstone: this handler replaced debuggerd's): one raised while the handler runs with the
 * signal blocked (a fault inside the chain below: the guest CPU, promotion wait, GPU page tracking, the
 * crash report itself), or one whose handler frame does not fit on the stack (an overflow).
 * - SA_NODEFER: a fault inside the handler re-enters bb_fault_entry, which sees this thread's
 *   depth and reports both faults with async-signal-safe writes (dladdr: acceptable when dying),
 *   then exits with 128 + signal as before.
 * - AArch64 Linux, also SA_ONSTACK: the kernel delivers on the thread's alternate signal stack
 *   (Bionic gives every thread a small one). There a stack overflow (no room left below the
 *   interrupted sp) or an overflow of the alternate stack itself is reported; any other fault is
 *   handled as before, on the thread's own stack just below the interrupted frame (where the
 *   kernel put the frame without SA_ONSTACK), with a spare alternate stack swapped in meanwhile so
 *   a nested signal finds one that is free (the thread's own still holds this signal's frame).
 *   BB_FAULT_ALTSTACK=0: SA_NODEFER only, on the thread's stack as before. */
enum { FAULT_STAGE_ENTRY = 1, FAULT_STAGE_GUEST_CPU, FAULT_STAGE_PROMOTION, FAULT_STAGE_GPU,
       FAULT_STAGE_RECOVER, FAULT_STAGE_REPORT };
static const char *const fault_stage_names[] = {
    "?", "entry", "guest CPU", "granule promotion wait", "GPU page tracking (bbgpu_handle_fault)",
    "recovery point", "crash report"};
static __thread int fault_depth; /* 1 while this thread runs the handler, 2 while it reports a nested fault */
static __thread int fault_stage, fault_outer_sig;
static __thread uintptr_t fault_outer_addr, fault_outer_pc;
#if defined(__aarch64__) && defined(__linux__)
#define FAULT_ALTSTACK 1
static int fault_altstack = 1; /* BB_FAULT_ALTSTACK=0: off (read before the handler is installed) */
static __thread unsigned char fault_spare_stack[32 * 1024] __attribute__((aligned(16)));
static __thread stack_t fault_thread_altstack; /* the thread's own, read at its first fault */
static __thread int fault_altstack_known;      /* 0 not read yet, 1 it has one, -1 none */
static __thread int fault_swapped;             /* the spare is installed (fault_leave restores) */
#endif
/* The handler is done on this thread (returning, or resuming a recovery point). */
static void fault_leave(void) {
#ifdef FAULT_ALTSTACK
    if (fault_swapped) {
        fault_swapped = 0;
        sigaltstack(&fault_thread_altstack, NULL); /* on the thread's stack here: allowed */
    }
#endif
    fault_stage = 0;
    fault_depth = 0;
}
static uintptr_t fault_uc_sp(const void *context) {
#if defined(__aarch64__) && defined(__linux__)
    return (uintptr_t)((const ucontext_t *)context)->uc_mcontext.sp;
#elif defined(__x86_64__) && defined(__linux__)
    return (uintptr_t)((const ucontext_t *)context)->uc_mcontext.gregs[REG_RSP];
#else
    (void)context; return 0;
#endif
}
static uintptr_t fault_uc_lr(const void *context) {
#if defined(__aarch64__) && defined(__linux__)
    return (uintptr_t)((const ucontext_t *)context)->uc_mcontext.regs[30];
#else
    (void)context; return 0;
#endif
}
/* A line built without stdio (async-signal-safe), written with write(2). */
typedef struct { char text[640]; size_t n; } FaultLine;
static void fl_str(FaultLine *l, const char *s) {
    while (s && *s && l->n < sizeof(l->text) - 2) l->text[l->n++] = *s++;
}
static void fl_hex(FaultLine *l, uint64_t v) {
    char d[19]; int i = 18; d[i] = 0;
    do { d[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    d[--i] = 'x'; d[--i] = '0';
    fl_str(l, d + i);
}
static void fl_dec(FaultLine *l, uint64_t v) {
    char d[21]; int i = 20; d[i] = 0;
    do { d[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    fl_str(l, d + i);
}
static void fl_where(FaultLine *l, uintptr_t pc) {
    Dl_info where;
    if (image && pc - (uintptr_t)image < 0x10000000) { fl_str(l, "guest +"); fl_hex(l, pc - (uintptr_t)image); return; }
    if (pc && dladdr((void *)pc, &where) && where.dli_fname) {
        const char *base = strrchr(where.dli_fname, '/');
        fl_str(l, base ? base + 1 : where.dli_fname); fl_str(l, "+"); fl_hex(l, pc - (uintptr_t)where.dli_fbase);
        if (where.dli_sname) { fl_str(l, " ("); fl_str(l, where.dli_sname); fl_str(l, ")"); }
        return;
    }
    fl_hex(l, pc);
    fl_str(l, " (in no shared object: JIT code or a freed mapping)");
}
static void fl_thread(FaultLine *l) {
    char name[17] = {0};
    fl_str(l, prctl(PR_GET_NAME, name, 0, 0, 0) == 0 ? name : "?");
}
static void fl_flush(FaultLine *l) {
    l->text[l->n++] = '\n';
    for (size_t at = 0; at < l->n;) {
        const ssize_t w = write(2, l->text + at, l->n - at);
        if (w <= 0) break;
        at += (size_t)w;
    }
    l->n = 0;
}
/* The interrupted code's pc, return address and sp. */
static void fl_context(FaultLine *l, const void *context) {
    fl_str(l, "  at "); fl_where(l, guest_cpu_host_pc(context)); fl_flush(l);
    if (fault_uc_lr(context)) { fl_str(l, "  return address "); fl_where(l, fault_uc_lr(context)); fl_flush(l); }
    fl_str(l, "  sp "); fl_hex(l, fault_uc_sp(context)); fl_flush(l);
}
/* A fault while this thread was already in the handler. */
__attribute__((noreturn)) static void fault_nested(int sig, siginfo_t *info, void *context) {
    if (fault_depth >= 2) {
        static const char text[] = "Fault inside the nested-fault report; exiting\n";
        ssize_t written_ = write(2, text, sizeof(text) - 1); (void)written_;
        _exit(128 + sig);
    }
    fault_depth = 2;
    FaultLine l = {.n = 0};
    const int stage = fault_stage >= 0 && fault_stage < (int)(sizeof(fault_stage_names) / sizeof(*fault_stage_names)) ? fault_stage : 0;
    fl_str(&l, "Nested fault (signal "); fl_dec(&l, (uint64_t)sig); fl_str(&l, ", address "); fl_hex(&l, (uintptr_t)info->si_addr);
    fl_str(&l, ") inside the fault handler, in its "); fl_str(&l, fault_stage_names[stage]); fl_str(&l, " stage; thread ");
    fl_thread(&l); fl_flush(&l);
    fl_context(&l, context);
    fl_str(&l, "  while handling signal "); fl_dec(&l, (uint64_t)fault_outer_sig); fl_str(&l, ", address "); fl_hex(&l, fault_outer_addr);
    fl_str(&l, ", at "); fl_where(&l, fault_outer_pc); fl_flush(&l);
    _exit(128 + sig);
}
#ifdef FAULT_ALTSTACK
/* A stack too small for the handler: reported from the alternate stack, then the process ends. */
__attribute__((noreturn)) static void fault_overflow(int sig, siginfo_t *info, void *context, const char *what) {
    fault_depth = 2; /* a fault in this report: the short message */
    FaultLine l = {.n = 0};
    fl_str(&l, "Stack overflow (signal "); fl_dec(&l, (uint64_t)sig); fl_str(&l, ", address "); fl_hex(&l, (uintptr_t)info->si_addr);
    fl_str(&l, "): "); fl_str(&l, what); fl_str(&l, " is exhausted; thread "); fl_thread(&l); fl_flush(&l);
    fl_context(&l, context);
    _exit(128 + sig);
}
/* At least `bytes` readable below sp (process_vm_readv reports EFAULT instead of faulting). A guard
 * page, a gap or the end of the stack mapping is unreadable. 1 when it cannot be told. */
static int fault_stack_room(uintptr_t sp, size_t bytes) {
    enum { PAGES = 4 };
    unsigned char sink[PAGES];
    struct iovec local[PAGES], remote[PAGES];
    const size_t pages = bytes / 4096 < PAGES ? bytes / 4096 : PAGES;
    for (size_t i = 0; i < pages; ++i) {
        local[i] = (struct iovec){sink + i, 1};
        remote[i] = (struct iovec){(void *)(sp - 1 - i * 4096), 1};
    }
    const ssize_t got = process_vm_readv(getpid(), local, pages, remote, pages, 0);
    if (got < 0) return errno != EFAULT;
    return (size_t)got == pages;
}
void bb_fault_call_on_stack(void (*fn)(int, siginfo_t *, void *), int sig, siginfo_t *info, void *context, uintptr_t top);
/* bb_fault_call_on_stack(fn, sig, info, context, top): fn(sig, info, context) with sp = top (16-aligned
 * down); a frame record and CFI, so the crash report's unwinder goes on through it. */
__asm__(".text\n"
        ".p2align 2\n"
        ".globl bb_fault_call_on_stack\n"
        ".type bb_fault_call_on_stack, %function\n"
        "bb_fault_call_on_stack:\n"
        ".cfi_startproc\n"
        " hint #34\n" /* BTI c */
        " stp x29, x30, [sp, #-16]!\n"
        ".cfi_def_cfa_offset 16\n"
        ".cfi_offset x29, -16\n"
        ".cfi_offset x30, -8\n"
        " mov x29, sp\n"
        ".cfi_def_cfa_register x29\n"
        " mov x9, x0\n"
        " mov w0, w1\n"
        " mov x1, x2\n"
        " mov x2, x3\n"
        " and x4, x4, #-16\n"
        " mov sp, x4\n"
        " blr x9\n"
        " mov sp, x29\n"
        ".cfi_def_cfa sp, 16\n"
        " ldp x29, x30, [sp], #16\n"
        ".cfi_def_cfa_offset 0\n"
        ".cfi_restore x29\n"
        ".cfi_restore x30\n"
        " ret\n"
        ".cfi_endproc\n"
        ".size bb_fault_call_on_stack, .-bb_fault_call_on_stack\n");
#endif
static void fault(int sig, siginfo_t *info, void *context) __attribute__((noinline));
void bb_fault_entry(int sig, siginfo_t *info, void *context);
/* cpu/guest_cpu_aot.cpp (BB_AOT): the AOT state and trace at a crash; absent in builds without it. */
void bbaot_crash_report(const void *ucontext) __attribute__((weak));
#ifdef FAULT_ALTSTACK
void bb_fault_on_thread_stack(int sig, siginfo_t *info, void *context);
void bb_fault_on_thread_stack(int sig, siginfo_t *info, void *context) {
    const stack_t spare = {.ss_sp = fault_spare_stack, .ss_flags = 0, .ss_size = sizeof(fault_spare_stack)};
    fault_swapped = sigaltstack(&spare, NULL) == 0;
    fault(sig, info, context);
    fault_leave();
}
#endif
/* The installed SIGSEGV/SIGBUS/SIGILL handler (SA_NODEFER; SA_ONSTACK on AArch64 Linux). */
void bb_fault_entry(int sig, siginfo_t *info, void *context) {
    if (fault_depth) fault_nested(sig, info, context);
    const int saved_errno = errno;
    fault_outer_sig = sig;
    fault_outer_addr = (uintptr_t)info->si_addr;
    fault_outer_pc = guest_cpu_host_pc(context);
#ifdef FAULT_ALTSTACK
    if (fault_altstack) {
        if (!fault_altstack_known) {
            stack_t ss;
            if (sigaltstack(NULL, &ss) == 0 && !(ss.ss_flags & SS_DISABLE) && ss.ss_size) {
                fault_thread_altstack = ss;
                fault_thread_altstack.ss_flags = 0;
                fault_altstack_known = 1;
            } else {
                fault_altstack_known = -1;
            }
        }
        const uintptr_t here = (uintptr_t)__builtin_frame_address(0);
        const uintptr_t base = (uintptr_t)fault_thread_altstack.ss_sp;
        if (fault_altstack_known > 0 && here - base < fault_thread_altstack.ss_size) {
            /* On the alternate stack: the interrupted code's stack has to take the handler. */
            const uintptr_t sp = fault_uc_sp(context), address = (uintptr_t)info->si_addr;
            if (sp < base && base - sp <= 65536)
                fault_overflow(sig, info, context, "the alternate signal stack (a signal handler ran past its end)");
            /* A fault on the stack itself (near sp; the game's memory and the handled traps are
             * far from any host stack) with no room left below sp. */
            const uintptr_t distance = address > sp ? address - sp : sp - address;
            if ((sig == SIGSEGV || sig == SIGBUS) && distance < (UINT64_C(1) << 20) && !fault_stack_room(sp, 16384))
                fault_overflow(sig, info, context, "the thread's stack");
            fault_depth = 1;
            fault_stage = FAULT_STAGE_ENTRY;
            /* Below the interrupted frame (AArch64 has no red zone), as the kernel would have put it. */
            bb_fault_call_on_stack(bb_fault_on_thread_stack, sig, info, context, sp - 256);
            errno = saved_errno;
            return;
        }
    }
#endif
    fault_depth = 1;
    fault_stage = FAULT_STAGE_ENTRY;
    fault(sig, info, context);
    fault_leave();
    errno = saved_errno;
}
static void fault(int sig, siginfo_t *info, void *context) {
    /* The guest CPU's own faults (unaligned atomics), then GPU page tracking (write-protected guest pages). */
    fault_stage = FAULT_STAGE_GUEST_CPU;
    if (guest_cpu_handle_fault(sig, info, context)) return;
    /* bbport UMA: a page a granule promotion froze: wait for it, the access is retried. */
    fault_stage = FAULT_STAGE_PROMOTION;
    if (sig == SIGSEGV && runtime_memory_promotion_fault(info->si_addr)) return;
    fault_stage = FAULT_STAGE_GPU;
    if (gpu_enabled && sig == SIGSEGV && bbgpu_handle_fault(context, info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    fault_stage = FAULT_STAGE_RECOVER;
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        fault_leave(); /* this thread's handler ends here (and its alternate stack comes back) */
        siglongjmp(*recover, 1);
    }
    fault_stage = FAULT_STAGE_REPORT;
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    GuestRegs regs;
    const int in_guest = guest_cpu_signal_regs(context, &regs);
    uintptr_t rip = in_guest == GUEST_CPU_IN_GUEST ? (uintptr_t)regs.rip : guest_cpu_host_pc(context);
    char line[512];
    Dl_info where;
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (in_guest == GUEST_CPU_IN_HOST_CALL && regs.rip - (uintptr_t)image < 0x10000000) {
        snprintf(line, sizeof(line), "  called by the game at guest offset 0x%lx\n", (unsigned long)(regs.rip - (uintptr_t)image));
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
#if defined(__aarch64__) && defined(__linux__)
    /* A host fault: the caller (link register) locates a call through a null pointer. */
    if (in_guest != GUEST_CPU_IN_GUEST) {
        const uintptr_t lr = (uintptr_t)((ucontext_t *)context)->uc_mcontext.regs[30];
        if (dladdr((void *)lr, &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  return address %s+0x%lx (%s)\n", where.dli_fname,
                     (unsigned long)(lr - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  return address %p\n", (void *)lr);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
#endif
    if (bbaot_crash_report) bbaot_crash_report(context);
    if (gpu_enabled) bbgpu_dump_guest_writes(context);
    /* Outside the image and any shared object (generated code, a freed mapping): the mapping
     * from /proc/self/maps, and the thread. */
    if (rip - (uintptr_t)image >= 0x10000000 && !(dladdr((void *)rip, &where) && where.dli_fname)) {
        char thread[32] = "?";
        pthread_getname_np(pthread_self(), thread, sizeof(thread));
        snprintf(line, sizeof(line), "  thread %s; mapping of RIP: ", thread);
        { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
        FILE *maps = fopen("/proc/self/maps", "r");
        int found = 0;
        while (maps && fgets(line, sizeof(line), maps)) {
            unsigned long from, to;
            if (sscanf(line, "%lx-%lx", &from, &to) == 2 && rip >= from && rip < to) {
                ssize_t written_=write(2, line, strlen(line)); (void)written_;
                found = 1;
                break;
            }
        }
        if (maps) fclose(maps);
        if (!found) { ssize_t written_=write(2, "none\n", 5); (void)written_; }
    }
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    /* From the frame after bb_fault_entry's (the kernel's sigreturn trampoline, then the faulting
     * code): the handler's own frames (fault, the stack switch) are not printed. */
    int first = 2;
    for (int i = 0; i < depth && i < 8; ++i)
        if (dladdr(frames[i], &where) && where.dli_saddr == (void *)bb_fault_entry) first = i + 1;
    for (int i = first; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    _exit(128 + sig);
}
#endif
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    /* Guest frames when the guest CPU is not the host's: the host PC first, then the guest's rbp chain. */
    GuestRegs regs;
    const int in_guest=guest_cpu_signal_regs(uc,&regs);
    uintptr_t rip=in_guest ? (uintptr_t)regs.rip : guest_cpu_host_pc(uc), rbp=in_guest ? (uintptr_t)regs.gpr[GUEST_RBP] : 0;
    uint64_t tid=(uint64_t)gettid();
    if (in_guest==GUEST_CPU_IN_HOST_CALL) {
        char host[]="  tid=0000000000000000 host pc=0000000000000000 host-relative=0000000000000000\n";
        write_hex(host+6,tid); write_hex(host+31,guest_cpu_host_pc(uc)); write_hex(host+62,guest_cpu_host_pc(uc)-exe_base);
        ssize_t written_=write(2,host,sizeof(host)-1); (void)written_;
    }
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,in_guest ? regs.gpr[GUEST_RDI] : 0);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    dump_frames(context);
    /* AOT (BB_AOT_TRACE): the thread's last transitions between units, the fallback and host functions. */
    if (bbaot_crash_report) bbaot_crash_report(context);
}
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
/* A segment's mapping: its size rounded to whole pages, as protect() maps it (segments start on a
 * page). The tail past its data is mapped with its permissions, zero: the cheats' code goes in the
 * code segment's (build_thor_patch_parts.py). */
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t size = round_page(segments[i].size);
        if (address >= segments[i].address && bytes <= size &&
            address - segments[i].address <= size - bytes) return 1;
    }
    return 0;
}
#ifndef _WIN32
/* bbport: live cheats (the Android app's Cheats menu). The cheat hooks (patch part cheats.bin,
 * build_thor_patch_parts.py) test flag bytes in the unused end of the data segment's last page;
 * BB_CHEATS_FILE holds them as digits ("10": infinite health on, items off). The file is read a
 * few times a second and the bytes follow it: the cheats switch while the game runs. A game started
 * online (BB_ONLINE=1) has them forced off for the whole session: the flags are zeroed once and the
 * file is never read, so nothing turns them on until the game restarts offline. */
enum { CheatFlags = 0x56d3f00, CheatCount = 2 };
static void *cheat_poller(void *arg) {
    const char *path = arg;
    char last[CheatCount + 1] = "";
    for (;;) {
        char now[CheatCount + 1] = "";
        FILE *f = fopen(path, "r");
        if (f) {
            size_t n = fread(now, 1, CheatCount, f);
            fclose(f);
            now[n] = 0;
        }
        if (strcmp(now, last) != 0) {
            for (int i = 0; i < CheatCount; ++i)
                __atomic_store_n(image + CheatFlags + i, (unsigned char)(now[i] == '1'), __ATOMIC_RELAXED);
            printf("Cheats: infinite health %s, infinite items %s\n", now[0] == '1' ? "on" : "off",
                   now[1] == '1' ? "on" : "off");
            fflush(stdout);
            memcpy(last, now, sizeof(last));
        }
        struct timespec pause = {0, 250 * 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    return NULL;
}
static void start_cheats(Segment *segments, uint64_t ns) {
    const char *path = getenv("BB_CHEATS_FILE");
    const char *online = getenv("BB_ONLINE");
    int offline = !(online && online[0] == '1');
    if (offline && (!path || !*path)) return;
    if (!mapped(segments, ns, CheatFlags, CheatCount)) {
        puts("Cheats: no flag bytes in this image");
        return;
    }
    if (!offline) {
        for (int i = 0; i < CheatCount; ++i) __atomic_store_n(image + CheatFlags + i, (unsigned char)0, __ATOMIC_RELAXED);
        puts("Cheats: off while playing online");
        fflush(stdout);
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, cheat_poller, (void *)path) == 0) pthread_detach(thread);
}
#endif
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    const char *owner=strrchr(path,'/'); /* the patch file's name, for runtime_code_patch_owner (argv: static) */
    owner=owner ? owner+1 : path;
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        runtime_code_patch_note((uintptr_t)(image+offset),length,owner);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
volatile int runtime_restarting;
void runtime_restart(void) {
    fflush(NULL);
    puts("Runtime: restarting through run.sh");
    /* The GPU threads still run until exec: their Vulkan calls fail once the device fd is
     * closed below, and an assertion there must not end the process (exit 23) before exec. */
    runtime_restarting = 1;
    __sync_synchronize();
#ifndef _WIN32
#ifdef __ANDROID__
    /* Android's app seccomp filter may kill close_range (SIGSYS): close the fds one by one. */
    { long max = sysconf(_SC_OPEN_MAX); if (max < 0 || max > 65536) max = 65536; for (int fd = 3; fd < max; ++fd) close(fd); }
#else
    syscall(SYS_close_range, 3u, ~0u, 0u);
#endif
#ifdef __ANDROID__
    execl("/system/bin/sh", "sh", "run.sh", (char *)NULL); /* TODO(phase2): the app restarts the process */
#else
    execlp("bash", "bash", "run.sh", (char *)NULL);
#endif
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef _WIN32
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
#ifdef __ANDROID__
    /* Bionic (Scudo) has neither knob and no brk heap. TODO(phase2): host objects the guest keeps
     * pointers to must come from low memory (runtime_low_map) when the kernel's VA is above 40
     * bits; heap pointer tagging (top byte) would also break the guest's 40-bit packing. */
    mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL,M_HEAP_TAGGING_LEVEL_NONE);
#else
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    guest_cpu_init(runtime_low_map);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    check_cpu();
    int cpu_only = 0, strict_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL;
    /* --patches may repeat (the Thor's settings choose pre-compiled parts): applied in order. */
    const char *patch_files[64]; int patch_count=0;
    /* --fallback-patches may repeat: applied after the others only when the frame rate hooks did
     * not install (bbgpu_frame_hooks_installed: a site of the game's code differs, or they are off). */
    const char *fallback_files[8]; int fallback_count=0;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) {
            if (patch_count==(int)(sizeof(patch_files)/sizeof(patch_files[0]))) fail("too many patch files");
            patch_files[patch_count++]=argv[++i];
        }
        else if (!strcmp(argv[i], "--fallback-patches") && i+1<argc) {
            if (fallback_count==(int)(sizeof(fallback_files)/sizeof(fallback_files[0]))) fail("too many fallback patch files");
            fallback_files[fallback_count++]=argv[++i];
        }
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file]... [--fallback-patches file]... [--timeout seconds] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
        char serial[16]="", title[128]="", version[16]="";
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"APP_VER",version,sizeof(version),NULL);
        runtime_net_configure(app0,serial,title,version);
    }
    bbgpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    /* SA_NODEFER (and SA_ONSTACK on AArch64 Linux): see bb_fault_entry. */
    struct sigaction sa = {0}; sa.sa_sigaction = bb_fault_entry; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
#ifdef FAULT_ALTSTACK
    {
        const char *altstack = getenv("BB_FAULT_ALTSTACK");
        fault_altstack = !(altstack && altstack[0] == '0');
        if (fault_altstack) sa.sa_flags |= SA_ONSTACK;
    }
#endif
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* BBPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    image = allocate(round_page(size));
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (!cpu_only) {
        char title[128]="Bloodborne", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    guest_cpu_code((uintptr_t)image, round_page(size));
    guest_cpu_code((uintptr_t)traps, round_page((import_count + 1) * 32));
    const uintptr_t handler = guest_cpu_host_function((void *)unresolved);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved && relocs[i].kind == 1) value = guest_cpu_host_function((void *)resolved);
            else if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    runtime_net_image_mapped(image, size); /* online modules that read the original code */
    for (int i=0;i<patch_count;++i) apply_patches(patch_files[i], segments, ns, relocs, nr);
    if (!cpu_only) {
        bbgpu_patch_image(image, size);
        if (fallback_count && !bbgpu_frame_hooks_installed()) {
            printf("Frame rate: hooks not installed: the fallback patches (the game's frame rate patch) apply\n");
            for (int i=0;i<fallback_count;++i) apply_patches(fallback_files[i], segments, ns, relocs, nr);
        }
    }
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    runtime_net_image_ready((uintptr_t)image, size); /* online game modules (BBSC) hook it now */
#ifndef _WIN32
    start_cheats(segments, ns);
    runtime_atlas_start(image, size);
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    guest_cpu_thread_stack(stack+MAIN_STACK-64,MAIN_STACK-64);
#endif
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            const uint64_t init_args[3]={0,0,0};
            int result=(int)guest_cpu_call((uintptr_t)(image+modules[m].init),3,init_args);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    if (gpu_enabled) bbgpu_adpf_register_main(); /* the game runs on this thread: the performance hint session */
    runtime_net_start(); /* online sessions start with the game */
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    /* Static: the host stack may lie above 47-bit guest addresses (arm64 hosts). */
    static struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
#else
#ifdef GUEST_CPU_NATIVE
    if (guest_cpu_is_native()) enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
    else
#endif
    {
        const uint64_t entry_args[2]={(uint64_t)(uintptr_t)&params,guest_cpu_host_function((void *)guest_exit)};
        guest_cpu_call((uintptr_t)(image+entry),2,entry_args);
    }
#endif
    fail("entry unexpectedly returned");
}
