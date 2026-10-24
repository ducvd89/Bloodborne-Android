// bb-recomp-difftest: game functions against themselves. Each function of the eboot is run natively (the original
// x86-64 bytes, mapped in this process at the lift base) and lowered (FEX IR -> LLVM -> x86-64 through ORC) with the
// same random arguments, registers and buffers; the return registers, the callee-saved registers and the buffers must
// agree. Native x86 is the oracle, which is stronger than FEX (docs/RECOMP_ABI.md).
//
// Safety: the native run happens in a forked grandchild under a seccomp filter that kills it on any system call, with
// a timer; the image is mapped read-only, so a function that writes globals faults (and is skipped). Inputs on which
// either side faults or times out are skipped. Only functions without call exits are run (calls would leave the
// function: imports, syscalls); tail jumps and switch tables stay inside it.
#include "jit_host.h"

#include <FEXCore/Core/X86Enums.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ucontext.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <map>
#include <random>
#include <sstream>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct NativeRegs {
  uint64_t gpr[16];          // x86 encoding order = FEX order: rax rcx rdx rbx rsp rbp rsi rdi r8..r15
  uint8_t xmm[16][16];
  uint64_t fn;
  uint64_t savedRsp;
  volatile int status;       // 0 not run, 1 returned, 2 signal, 3 timeout
  volatile int sig;
  volatile uint64_t faultAddr, faultRip;
};
constexpr int OffXmm = 16 * 8, OffFn = OffXmm + 16 * 16, OffSaved = OffFn + 8;
static_assert(offsetof(NativeRegs, xmm) == OffXmm && offsetof(NativeRegs, fn) == OffFn && offsetof(NativeRegs, savedRsp) == OffSaved, "asm offsets");

extern "C" NativeRegs* g_nat_regs;
NativeRegs* g_nat_regs = nullptr;
extern "C" void bbr_native_call();

// Loads every register from *g_nat_regs, runs fn on the stack in gpr[4] (call pushes the return address), stores the
// registers back. r11 = fn on entry, as in the lowered run.
asm(R"(
  .text
  .globl bbr_native_call
  .type bbr_native_call,@function
bbr_native_call:
  .intel_syntax noprefix
  push rbp
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov r11, qword ptr [rip + g_nat_regs]
  mov [r11 + 392], rsp
  movups xmm0, [r11 + 128]
  movups xmm1, [r11 + 144]
  movups xmm2, [r11 + 160]
  movups xmm3, [r11 + 176]
  movups xmm4, [r11 + 192]
  movups xmm5, [r11 + 208]
  movups xmm6, [r11 + 224]
  movups xmm7, [r11 + 240]
  movups xmm8, [r11 + 256]
  movups xmm9, [r11 + 272]
  movups xmm10, [r11 + 288]
  movups xmm11, [r11 + 304]
  movups xmm12, [r11 + 320]
  movups xmm13, [r11 + 336]
  movups xmm14, [r11 + 352]
  movups xmm15, [r11 + 368]
  mov rax, [r11 + 0]
  mov rcx, [r11 + 8]
  mov rdx, [r11 + 16]
  mov rbx, [r11 + 24]
  mov rbp, [r11 + 40]
  mov rsi, [r11 + 48]
  mov rdi, [r11 + 56]
  mov r8,  [r11 + 64]
  mov r9,  [r11 + 72]
  mov r10, [r11 + 80]
  mov r12, [r11 + 96]
  mov r13, [r11 + 104]
  mov r14, [r11 + 112]
  mov r15, [r11 + 120]
  mov rsp, [r11 + 32]
  mov r11, [r11 + 384]
  call r11
  mov r11, qword ptr [rip + g_nat_regs]
  mov [r11 + 0], rax
  mov [r11 + 8], rcx
  mov [r11 + 16], rdx
  mov [r11 + 24], rbx
  mov [r11 + 32], rsp
  mov [r11 + 40], rbp
  mov [r11 + 48], rsi
  mov [r11 + 56], rdi
  mov [r11 + 64], r8
  mov [r11 + 72], r9
  mov [r11 + 80], r10
  mov [r11 + 96], r12
  mov [r11 + 104], r13
  mov [r11 + 112], r14
  mov [r11 + 120], r15
  movups [r11 + 128], xmm0
  movups [r11 + 144], xmm1
  movups [r11 + 160], xmm2
  movups [r11 + 176], xmm3
  movups [r11 + 192], xmm4
  movups [r11 + 208], xmm5
  movups [r11 + 224], xmm6
  movups [r11 + 240], xmm7
  movups [r11 + 256], xmm8
  movups [r11 + 272], xmm9
  movups [r11 + 288], xmm10
  movups [r11 + 304], xmm11
  movups [r11 + 320], xmm12
  movups [r11 + 336], xmm13
  movups [r11 + 352], xmm14
  movups [r11 + 368], xmm15
  mov rsp, [r11 + 392]
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  pop rbp
  ret
  .att_syntax prefix
  .size bbr_native_call, .-bbr_native_call
)");

constexpr uint64_t BufA = 0x500000000ull, BufB = 0x500100000ull, BufSize = 0x2000, StackBase = 0x500200000ull, StackSize = 0x10000;
constexpr uint64_t Sentinel = 0x7000deadb000ull;

struct Input {
  uint64_t gpr[16];
  uint8_t xmm[16][16];
  std::vector<uint8_t> a, b;
  uint64_t stackArgs[12];
};

uint64_t rnd64(std::mt19937_64& r) { return r(); }

Input makeInput(std::mt19937_64& r, uint64_t fn) {
  Input in {};
  // arguments: mostly valid pointers into the two buffers (so structures can be walked), small counts, a few random values
  auto pick = [&]() -> uint64_t {
    const unsigned c = r() % 100;
    if (c < 2) return 0;
    if (c < 10) return r() % 64;                                // small counts and indices
    if (c < 13) return (uint64_t)(int64_t)(int32_t)r();         // sign extended 32-bit
    if (c < 48) return BufA + (r() % (BufSize / 2)) / 8 * 8;
    if (c < 78) return BufB + (r() % (BufSize / 2)) / 8 * 8;
    if (c < 85) return BufA + (r() % 256);
    return r();
  };
  for (unsigned i = 0; i < 16; ++i) in.gpr[i] = rnd64(r);
  in.gpr[0] = r() % 8;                          // al (vector register count of varargs)
  for (unsigned i : {1u, 2u, 6u, 7u, 8u, 9u}) in.gpr[i] = pick(); // the six argument registers
  in.gpr[11] = fn;
  in.gpr[4] = StackBase + StackSize - 0x1000;   // 16-byte aligned top of the guest stack
  for (unsigned i = 0; i < 16; ++i) {
    // floats and doubles: ordinary values only (no NaNs: their payloads are not part of the contract)
    double d = ((double)(int64_t)r() / 4.0e15);
    float f0 = (float)d, f1 = (float)(d * 3.7);
    if (r() % 3 == 0) memcpy(in.xmm[i], &d, 8), memcpy(in.xmm[i] + 8, &f1, 4), memcpy(in.xmm[i] + 12, &f0, 4);
    else { float v[4] = {f0, f1, (float)(d / 7.0), (float)(int32_t)r()}; memcpy(in.xmm[i], v, 16); }
  }
  // buffer words: 40 % pointers back into the buffers, 25 % small numbers, the rest random
  in.a.resize(BufSize); in.b.resize(BufSize);
  auto fill = [&](std::vector<uint8_t>& v) {
    for (size_t i = 0; i + 8 <= v.size(); i += 8) {
      const unsigned c = r() % 20;
      const uint64_t w = c < 5 ? BufA + (r() % (BufSize - 16)) / 8 * 8 : c < 10 ? BufB + (r() % (BufSize - 16)) / 8 * 8 : c < 14 ? (r() % 256) : r();
      memcpy(&v[i], &w, 8);
    }
  };
  fill(in.a); fill(in.b);
  for (unsigned i = 0; i < 12; ++i) in.stackArgs[i] = pick();   // stack arguments above the return address
  return in;
}

NativeRegs* g_shared = nullptr;

// The image is read-only; a write to its data segment is allowed (the page is unprotected) so functions that touch
// globals can run. The worker restores the dirty pages from a snapshot after every lowered run; a native run happens
// in a grandchild that exits, so it needs no restoring.
struct DataTracker {
  uint64_t lo = 0, hi = 0;
  std::vector<uint8_t> snap;
  uint64_t dirty[65536]; unsigned nDirty = 0;
} g_data;

bool dataWriteFault(int sig, siginfo_t* si) {
  const uint64_t a = (uint64_t)si->si_addr;
  if (sig != SIGSEGV || a < g_data.lo || a >= g_data.hi || g_data.nDirty >= 65536) return false;
  const uint64_t page = a & ~0xfffull;
  if (mprotect((void*)page, 4096, PROT_READ | PROT_WRITE)) return false;
  g_data.dirty[g_data.nDirty++] = page;
  return true;
}
void restoreData() {
  for (unsigned i = 0; i < g_data.nDirty; ++i) {
    const uint64_t page = g_data.dirty[i];
    memcpy((void*)page, g_data.snap.data() + (page - g_data.lo), 4096);
    mprotect((void*)page, 4096, PROT_READ);
  }
  g_data.nDirty = 0;
}
void nativeSignal(int sig, siginfo_t* si, void* uc) {
  if (dataWriteFault(sig, si)) return;
  g_nat_regs->faultAddr = (uint64_t)si->si_addr; g_nat_regs->faultRip = ((ucontext_t*)uc)->uc_mcontext.gregs[REG_RIP];
  g_nat_regs->status = sig == SIGALRM ? 3 : 2; g_nat_regs->sig = sig; _exit(1);
}
void workerSignal(int sig, siginfo_t* si, void*) {
  if (dataWriteFault(sig, si)) return;
  g_haltKind = 4; g_trapArg = sig;
  siglongjmp(g_halt, 1);
}

void installSeccomp() {
  prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
  struct sock_filter f[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_exit_group, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
  };
  struct sock_fprog prog = {sizeof f / sizeof f[0], f};
  prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
}

// Native run in a grandchild. 0 = returned (regs in *g_shared), else the status (2 signal, 3 timeout, 4 killed by seccomp).
int runNative(const Input& in) {
  std::memset(g_shared, 0, sizeof *g_shared);
  memcpy(g_shared->gpr, in.gpr, sizeof in.gpr);
  memcpy(g_shared->xmm, in.xmm, sizeof in.xmm);
  g_shared->fn = in.gpr[11];
  memcpy((void*)BufA, in.a.data(), BufSize);
  memcpy((void*)BufB, in.b.data(), BufSize);
  memset((void*)StackBase, 0, StackSize);
  memcpy((void*)(in.gpr[4] + 0), in.stackArgs, sizeof in.stackArgs); // [rsp+8..] after the call pushes the return address
  const pid_t pid = fork();
  if (pid == 0) {
    g_nat_regs = g_shared;
    struct sigaction sa {}; sa.sa_sigaction = nativeSignal; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGALRM}) sigaction(s, &sa, nullptr);
    struct itimerval tv {}; tv.it_value.tv_usec = 20000; setitimer(ITIMER_REAL, &tv, nullptr);
    installSeccomp();
    bbr_native_call();
    g_shared->status = 1;
    _exit(0);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  if (WIFSIGNALED(st)) return 4;
  return g_shared->status == 1 ? 0 : (g_shared->status ? g_shared->status : 4);
}

struct Result { uint64_t gpr[16]; uint8_t xmm0[16], xmm1[16]; std::vector<uint8_t> a, b; };

Result captureNative() {
  Result r {};
  memcpy(r.gpr, g_shared->gpr, sizeof r.gpr);
  memcpy(r.xmm0, g_shared->xmm[0], 16); memcpy(r.xmm1, g_shared->xmm[1], 16);
  r.a.assign((uint8_t*)BufA, (uint8_t*)BufA + BufSize);
  r.b.assign((uint8_t*)BufB, (uint8_t*)BufB + BufSize);
  return r;
}

// The lowered run. 0 = returned, else a status like runNative's (5: left the function).
int runLowered(Jit& jit, uint64_t fn, const Input& in, Result& out, uint64_t fsBase) {
  memcpy((void*)BufA, in.a.data(), BufSize);
  memcpy((void*)BufB, in.b.data(), BufSize);
  memset((void*)StackBase, 0, StackSize);
  memcpy((void*)(in.gpr[4] + 0), in.stackArgs, sizeof in.stackArgs);
  alignas(64) static CPUState st;
  new (&st) CPUState();
  static CPUState::gdt_segment gdt[32];
  st.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = gdt;
  st.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = gdt;
  st.cs_idx = CPUState::DEFAULT_USER_CS << 3;
  auto* cs = CPUState::GetSegmentFromIndex(st, st.cs_idx);
  CPUState::SetGDTLimit(cs, 0xFFFFFu); cs->L = 1; cs->D = 0;
  for (unsigned i = 0; i < 16; ++i) st.gregs[i] = in.gpr[i];
  for (unsigned i = 0; i < 16; ++i) { memcpy(&st.xmm.sse.data[i][0], in.xmm[i], 16); }
  st.fs_cached = fsBase;
  uint64_t rsp = in.gpr[4] - 8;
  *(uint64_t*)rsp = Sentinel;
  st.gregs[4] = rsp;
  st.rip = fn;
  g_haltKind = 0;
  struct itimerval tv {}; tv.it_value.tv_usec = 20000; setitimer(ITIMER_REAL, &tv, nullptr);
  int rc = 0;
  if (sigsetjmp(g_halt, 1) == 0) {
    Jit::dispatch(&st, fn); // finds (or compiles) and runs the unit
    while (st.rip != Sentinel) Jit::dispatch(&st, st.rip);
  } else {
    rc = g_haltKind == 4 ? (g_trapArg == SIGALRM ? 3 : 2) : 5;
  }
  struct itimerval off {}; setitimer(ITIMER_REAL, &off, nullptr);
  restoreData();
  memcpy(out.gpr, st.gregs, sizeof out.gpr);
  memcpy(out.xmm0, &st.xmm.sse.data[0][0], 16); memcpy(out.xmm1, &st.xmm.sse.data[1][0], 16);
  out.a.assign((uint8_t*)BufA, (uint8_t*)BufA + BufSize);
  out.b.assign((uint8_t*)BufB, (uint8_t*)BufB + BufSize);
  return rc;
}

std::string compare(const Result& n, const Result& l) {
  static const char* nm[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
  for (unsigned i : {0u, 2u, 3u, 4u, 5u, 12u, 13u, 14u, 15u}) // return value, callee-saved, rsp
    if (n.gpr[i] != l.gpr[i]) { char b[96]; snprintf(b, sizeof b, "%s native %#llx lowered %#llx", nm[i], (unsigned long long)n.gpr[i], (unsigned long long)l.gpr[i]); return b; }
  if (memcmp(n.xmm0, l.xmm0, 16)) return "xmm0";
  if (memcmp(n.xmm1, l.xmm1, 16)) return "xmm1";
  for (size_t i = 0; i < n.a.size(); ++i) if (n.a[i] != l.a[i]) return "buffer A byte " + std::to_string(i);
  for (size_t i = 0; i < n.b.size(); ++i) if (n.b[i] != l.b[i]) return "buffer B byte " + std::to_string(i);
  return "";
}

// rdtsc (0f 31) and rdtscp (0f 01 f9): the value cannot agree between two runs
bool hasTimestamp(const uint8_t* p, size_t n) {
  for (size_t i = 0; i + 1 < n; ++i)
    if (p[i] == 0x0f && (p[i + 1] == 0x31 || (p[i + 1] == 0x01 && i + 2 < n && p[i + 2] == 0xf9))) return true;
  return false;
}

uint64_t fsBaseOf() { uint64_t v = 0; syscall(SYS_arch_prctl, 0x1003 /*ARCH_GET_FS*/, &v); return v; }

} // namespace

int main(int argc, char** argv) {
  setpriority(PRIO_PROCESS, 0, 19);
  struct rlimit nocore {0, 0};
  setrlimit(RLIMIT_CORE, &nocore); // the seccomp kill would otherwise dump a 500 MB core (0.5 s)
  std::string elf = "out/eboot.elf", outFile = "out/recomp/difftest.txt", bootFile, prxDir = "/home/cid/BloodbornePC/game/CUSA03173/sce_module", moduleName;
  uint64_t base = 0x200000000ull, limit = 0, every = 1, only = ~0ull;
  unsigned inputs = 6, jobs = 22, seed = 1;
  uint64_t dumpLo = 0, dumpHi = 0; std::string dumpFile; // --dump-range <lo> <hi> <file>: the image bytes of [lo, hi) (for objdump -b binary), then exit
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* s) { return !strcmp(argv[i], s); };
    auto arg = [&]() -> const char* { if (i + 1 >= argc) { fprintf(stderr, "missing argument\n"); exit(2); } return argv[++i]; };
    if (is("--elf")) elf = arg();
    else if (is("--boot")) bootFile = arg();
    else if (is("--prx-dir")) prxDir = arg();
    else if (is("--module")) moduleName = arg();
    else if (is("--dump-range")) { dumpLo = strtoull(arg(), nullptr, 16); dumpHi = strtoull(arg(), nullptr, 16); dumpFile = arg(); }
    else if (is("--limit")) limit = strtoull(arg(), nullptr, 0);
    else if (is("--every")) every = std::max<uint64_t>(1, strtoull(arg(), nullptr, 0));
    else if (is("--inputs")) inputs = atoi(arg());
    else if (is("--seed")) seed = atoi(arg());
    else if (is("--jobs")) jobs = std::min(22, std::max(1, atoi(arg())));
    else if (is("--func")) only = strtoull(arg(), nullptr, 16);
    else if (is("--out")) outFile = arg();
    else if (is("-v")) verbose = true;
    else { fprintf(stderr, "usage: bb-recomp-difftest --elf <eboot.elf> [--limit n] [--every n] [--inputs n] [--seed n] [--func hex] [--jobs n] [--out file] [-v]\n"
                    "  --boot <boot-linked.bin> [--prx-dir <dir>] [--module <name>]: the runtime's image (eboot + libc.prx + libSceFios2.prx); --module keeps one module's functions\n"); return 2; }
  }
  ElfImage img; std::string err;
  if (bootFile.empty()) {
    if (!img.load(elf, base, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
  } else if (!img.loadBoot(bootFile, elf, prxDir, base, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
  std::vector<Func> funcs;
  if (!(bootFile.empty() ? scanFdes(img, funcs, err) : scanAllFdes(img, funcs, err))) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
  uint64_t modLo = 0, modHi = ~0ull; // --module: only this module's functions are tested (calls may still leave it)
  if (!moduleName.empty()) {
    const ModuleInfo* mod = nullptr;
    for (auto& m : img.modules) if (m.name == moduleName) mod = &m;
    if (!mod) { fprintf(stderr, "no module '%s' in the image\n", moduleName.c_str()); return 1; }
    modLo = mod->base; modHi = mod->base + mod->size;
  }
  if (!dumpFile.empty()) {
    FILE* f = fopen(dumpFile.c_str(), "wb");
    if (!f) { perror(dumpFile.c_str()); return 1; }
    fwrite((const void*)(base + dumpLo), 1, dumpHi - dumpLo, f);
    fclose(f);
    return 0;
  }
  // read-only image (writes fault), the text segment executable
  for (auto& s : img.loads) {
    const uint64_t a = (base + s.vaddr) & ~0x3fffull, e = (base + s.vaddr + s.memsz + 0x3fff) & ~0x3fffull;
    mprotect((void*)a, e - a, PROT_READ | ((s.flags & 1) ? PROT_EXEC : 0));
  }
  std::vector<size_t> todo;
  for (size_t i = 0; i < funcs.size(); i += every) {
    if (only != ~0ull && !(only >= funcs[i].start && only < funcs[i].end)) continue;
    if (funcs[i].start < modLo || funcs[i].start >= modHi) continue;
    if (limit && todo.size() >= limit) break;
    todo.push_back(i);
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  printf("difftest: %zu functions, %u inputs each, %u workers\n", todo.size(), inputs, jobs);

  // One pipe per worker; each worker writes one line per function.
  struct Worker { pid_t pid; std::string path; };
  std::vector<Worker> ws;
  char tmpl[] = "/tmp/bbr-difftest-XXXXXX";
  if (!mkdtemp(tmpl)) return 1;
  for (unsigned w = 0; w < jobs; ++w) {
    const std::string path = std::string(tmpl) + "/w" + std::to_string(w);
    const pid_t pid = fork();
    if (pid == 0) {
      FILE* out = fopen(path.c_str(), "w");
      // buffers, stack and the register block are shared with this worker's own grandchildren only
      for (auto& sg : img.loads) if (sg.flags & 2) {
    const uint64_t lo = (base + sg.vaddr) & ~0xfffull, hi = (base + sg.vaddr + sg.memsz + 0xfff) & ~0xfffull;
    g_data.lo = g_data.lo ? std::min(g_data.lo, lo) : lo; g_data.hi = std::max(g_data.hi, hi);
  }
  if (g_data.hi) g_data.snap.assign((uint8_t*)g_data.lo, (uint8_t*)g_data.hi);
  for (uint64_t a : {BufA, BufB, StackBase})
        if (mmap((void*)a, a == StackBase ? StackSize : BufSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != (void*)a) { fprintf(stderr, "mmap failed\n"); _exit(3); }
      g_shared = (NativeRegs*)mmap(nullptr, sizeof(NativeRegs), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
      LiftOptions lo; lo.gapUnits = true; lo.multiblock = true;
      TargetSpec ts; ts.aarch64 = false; ts.opt = 1;
      // a fresh JIT every few hundred functions: ORC slows down with thousands of modules
      std::unique_ptr<Jit> jitp;
      auto newJit = [&] {
        jitp = std::make_unique<Jit>(img, lo, ts);
        g_jit = jitp.get();
        jitp->liftBase = base;
        for (auto& sg : img.loads) jitp->imageSize = std::max(jitp->imageSize, sg.vaddr + sg.memsz);
        jitp->calleeSaved = false;
      };
      newJit();
      g_allowSyscalls = false;
      const uint64_t fsb = fsBaseOf();
      struct sigaction sa {}; sa.sa_sigaction = workerSignal; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
      static char alt[1 << 16];
      stack_t ss {}; ss.ss_sp = alt; ss.ss_size = sizeof alt; sigaltstack(&ss, nullptr);
      for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGALRM}) sigaction(s, &sa, nullptr);
      unsigned sinceNew = 0;
      for (size_t k = w; k < todo.size(); k += jobs) {
        if (++sinceNew > 150) { newJit(); sinceNew = 0; }
        Jit& jit = *jitp;
        const Func& f = funcs[todo[k]];
        if (getenv("DT_DEBUG")) fprintf(stderr, "[w%u] func %llx\n", w, (unsigned long long)f.start);
        const uint64_t fn = base + f.start;
        std::string line;
        char head[64]; snprintf(head, sizeof head, "%llx-%llx ", (unsigned long long)f.start, (unsigned long long)f.end);
        // a dispatch (a call, a tail jump, a switch table) compiles the function that holds the target
        jit.rangeEnd = [&](uint64_t t) -> uint64_t {
          if (t < base) return 0;
          auto it = std::upper_bound(funcs.begin(), funcs.end(), t - base, [](uint64_t v, const Func& g) { return v < g.start; });
          if (it == funcs.begin()) return 0;
          --it;
          return t - base < it->end ? base + it->end : 0;
        };
        if (!jit.find(fn) && !jit.compile(fn, base + f.end)) { // (already compiled: an earlier function called it)
          if (!jit.unsupported.empty()) { line = "unsupported"; for (auto& u : jit.unsupported) line += " " + u; }
          else { line = "liftfail " + jit.liftFailure + jit.err; for (auto& c : line) if (c == '\n') c = ' '; }
        } else if (hasTimestamp((const uint8_t*)fn, f.end - f.start)) {
          line = "skip-rdtsc";
        } else {
          std::mt19937_64 rng(((uint64_t)seed << 40) ^ f.start);
          unsigned ok = 0, bad = 0, skipped = 0, faultMismatch = 0;
          std::string why;
          for (unsigned i = 0; i < inputs; ++i) {
            Input in = makeInput(rng, fn);
            if (getenv("DT_DEBUG")) fprintf(stderr, "  input %u native\n", i);
            struct timespec t0, t1, t2; clock_gettime(CLOCK_MONOTONIC, &t0);
            const int nrc = runNative(in);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (getenv("DT_DEBUG") && nrc) fprintf(stderr, "  native rc %d sig %d addr %llx rip %llx\n", nrc, g_shared->sig, (unsigned long long)g_shared->faultAddr, (unsigned long long)g_shared->faultRip);
            Result nat;
            if (nrc == 0) nat = captureNative();
            Result low {};
            const int lrc = runLowered(jit, fn, in, low, fsb);
            clock_gettime(CLOCK_MONOTONIC, &t2);
            if (getenv("DT_TIME")) fprintf(stderr, "native %.2f ms (rc %d) lowered %.2f ms (rc %d)\n", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6, nrc, (t2.tv_sec - t1.tv_sec) * 1e3 + (t2.tv_nsec - t1.tv_nsec) / 1e6, lrc);
            if (nrc != 0 || lrc != 0) { if ((nrc == 0) != (lrc == 0)) ++faultMismatch; ++skipped; if (nrc == 3) break; continue; } // a native timeout: do not wait on this function again
            const std::string d = compare(nat, low);
            if (!d.empty() && getenv("DT_DEBUG")) { fprintf(stderr, "  rdi=%llx rsi=%llx rdx=%llx; mem[rdi+8]=%llx; native rax=%llx lowered rax=%llx\n", (unsigned long long)in.gpr[7], (unsigned long long)in.gpr[6], (unsigned long long)in.gpr[2], (unsigned long long)(in.gpr[7] >= BufA && in.gpr[7] < BufA + BufSize - 16 ? *(uint64_t*)&in.a[in.gpr[7] - BufA + 8] : in.gpr[7] >= BufB && in.gpr[7] < BufB + BufSize - 16 ? *(uint64_t*)&in.b[in.gpr[7] - BufB + 8] : 0), (unsigned long long)nat.gpr[0], (unsigned long long)low.gpr[0]); }
            if (!d.empty() && getenv("DT_DEBUG")) { auto h = [](const uint8_t* x) { fprintf(stderr, "%08x %08x %08x %08x", ((uint32_t*)x)[0], ((uint32_t*)x)[1], ((uint32_t*)x)[2], ((uint32_t*)x)[3]); }; fprintf(stderr, "  xmm0 nat "); h(nat.xmm0); fprintf(stderr, "\n       low "); h(low.xmm0); fprintf(stderr, "\n  xmm1 nat "); h(nat.xmm1); fprintf(stderr, "\n       low "); h(low.xmm1); fprintf(stderr, "\n"); }
            if (d.empty()) ++ok; else { ++bad; if (why.empty()) why = d; }
          }
          char b[160];
          if (bad) snprintf(b, sizeof b, "MISMATCH %s (%u ok, %u bad)", why.c_str(), ok, bad);
          else if (ok) snprintf(b, sizeof b, "ok %u/%u (%u skipped, %u fault-mismatch)", ok, inputs, skipped, faultMismatch);
          else snprintf(b, sizeof b, "skip-faults (%u fault-mismatch)", faultMismatch);
          line = b;
        }
        fprintf(out, "%s%s\n", head, line.c_str());
        fflush(out);
      }
      _exit(0);
    }
    ws.push_back({pid, path});
  }
  std::map<std::string, unsigned> counts;
  std::vector<std::string> bad;
  unsigned done = 0, crashedWorkers = 0;
  for (auto& w : ws) {
    int st; waitpid(w.pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) ++crashedWorkers;
  }
  for (auto& w : ws) {
    FILE* in = fopen(w.path.c_str(), "r");
    if (!in) continue;
    char buf[512];
    while (fgets(buf, sizeof buf, in)) {
      std::string l(buf);
      while (!l.empty() && l.back() == '\n') l.pop_back();
      ++done;
      const std::string kind = l.substr(l.find(' ') + 1);
      std::string key = kind.rfind("ok", 0) == 0 ? "ok" : kind.rfind("MISMATCH", 0) == 0 ? "MISMATCH" : kind.rfind("skip-faults", 0) == 0 ? "skip-faults" : kind.rfind("liftfail", 0) == 0 ? "liftfail" : kind.rfind("unsupported", 0) == 0 ? "unsupported" : kind;
      ++counts[key];
      if (key == "MISMATCH") bad.push_back(l);
      if (verbose) puts(l.c_str());
    }
    fclose(in);
    remove(w.path.c_str());
  }
  rmdir(tmpl);
  std::ostringstream o;
  o << "functions " << todo.size() << ", reported " << done << ", workers lost " << crashedWorkers << "\n";
  for (auto& [k, n] : counts) o << "  " << k << ": " << n << "\n";
  o << "mismatches:\n";
  for (auto& l : bad) o << "  " << l << "\n";
  fputs(o.str().c_str(), stdout);
  std::string dir = outFile.substr(0, outFile.rfind('/'));
  if (!dir.empty() && dir != outFile) { std::string cmd = "mkdir -p " + dir; if (system(cmd.c_str())) {} }
  FILE* f = fopen(outFile.c_str(), "w");
  if (f) { fputs(o.str().c_str(), f); fclose(f); }
  return 0;
}
