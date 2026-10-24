// Secondary entry points that the unwinder and setjmp/longjmp can enter without going through a function start:
//  * C++ landing pads: the call-site tables of every FDE's LSDA (gcc_except_table, found through .eh_frame)
//  * setjmp-like call sites: the return address after a call to setjmp (longjmp resumes there)
#pragma once
#include "elf_image.h"
#include "fde_scan.h"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct EhScan {
  // function start -> landing pad addresses (image vaddrs) inside it
  std::map<uint64_t, std::vector<uint64_t>> landingPads;
  uint64_t fdes = 0, fdesWithLsda = 0, callSites = 0, pads = 0, badLsda = 0;
  std::string firstError;
};

// Parses the .eh_frame FDEs (their positions are in Func::fde) of the given functions.
void scanLandingPads(const ElfImage& img, const std::vector<Func>& funcs, EhScan& out);

// Call targets that return twice: the image's setjmp-like functions, found by name in a boot-linked image (the
// import table names the NID; the binding gives the native export in libc.prx) plus the PLT stubs that jump
// through their GOT slots. Empty when the image has no import information (a plain eboot.elf).
struct ReturnsTwice {
  std::set<uint64_t> targets;   // function entries and PLT stubs (image vaddrs)
  std::set<uint64_t> gotSlots;  // GOT slots of the imports (image vaddrs): `call [rip+slot]`
  std::vector<std::string> names; // what was found, for the report
};
ReturnsTwice findReturnsTwice(const ElfImage& img);
