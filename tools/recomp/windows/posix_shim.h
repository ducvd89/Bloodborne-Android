// SPDX-License-Identifier: GPL-3.0-or-later
// Force-included (-include) into every bb-recomp source file of the Windows build (llvm-mingw): the few POSIX names the shared sources use.
// lifter.cpp's crash containment (a signal or a FEX assert while lifting jumps back and marks the function Crashed) is not available
// on Windows: installCrashHandlers() installs no signal handlers there, so the jumps are never taken. The game's code lifts without a
// single failure (docs/NATIVE_CPU.md), so nothing is lost for the tool's one job.
#pragma once
#ifdef _WIN32
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __cplusplus
#define sigjmp_buf jmp_buf
#define sigsetjmp(b, m) setjmp(b)
#define siglongjmp(b, v) longjmp(b, v)
static inline const char* strsignal(int) { return "signal"; }
// memmem (link.cpp)
static inline void* memmem(const void* h, size_t hn, const void* n, size_t nn) {
  if (nn == 0) return (void*)h;
  if (hn < nn) return nullptr;
  for (size_t i = 0; i + nn <= hn; ++i) if (!memcmp((const char*)h + i, n, nn)) return (void*)((const char*)h + i);
  return nullptr;
}
#define lstat stat
#endif
#endif
