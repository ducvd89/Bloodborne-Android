// SPDX-License-Identifier: GPL-3.0-or-later
// The Windows build replaces operator new and delete (which LLVM's allocators and containers use) with mimalloc's: 22 compile threads that all
// allocate and free large blocks scale badly on the C runtime's heap (under Wine the code generation took 26 times the CPU time of Linux with 22
// threads, 2 times with 8). Only compiled with BB_RECOMP_MIMALLOC_DIR.
#include <mimalloc-new-delete.h>
