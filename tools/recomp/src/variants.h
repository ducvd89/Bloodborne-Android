// Variant catalog: byte patches the runtime applies to the game's code at start-up or while the game runs, and the native
// hook sites. bb-recomp compiles the functions the patches touch a second time, with the patched bytes, and gives the
// runtime a table keyed by the bytes it finds (docs/RECOMP_ABI.md, "Variants"); every hook site gets a slot test in the
// units that hold it ("Hook slots"). No game code in the catalogs: addresses, patch bytes and our own hook declarations.
//
// Text format (tools/recomp/variants/*.txt, '#' starts a comment):
//   group <name> [excl=<set>]       a group of patches that are applied together (a switch of the port); groups with the
//                                   same excl set are alternatives and are never applied together
//   w <hex vaddr> <hex bytes>       the group overwrites the bytes at the image address (existing code)
//   c <hex vaddr> <hex bytes>       the group writes new code at the image address (a cave: no function covers it)
//   hook <hex vaddr> <len> <kind> [skip] [resume=<hex>,<hex>...]
//                                   a native hook site: kind `snap` (InstallGuestCodeHook: the callback runs before the
//                                   instruction, which then runs; `skip` allows the callback to return from the function at once)
//                                   or `regs` (guest_cpu_hook: the callback replaces the instruction(s) and sets rip; `resume` lists
//                                   the addresses it may set it to inside the function)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct VarWrite { uint64_t vaddr = 0; std::vector<uint8_t> bytes; bool code = false; };
struct VarGroup { std::string name, excl; std::vector<VarWrite> writes; };
struct HookDecl {
  uint64_t vaddr = 0;
  uint32_t len = 0;
  bool skip = false;
  bool regs = false;                 // kind regs (guest_cpu_hook), else snap
  std::vector<uint64_t> resume;
};
struct Catalog {
  std::vector<VarGroup> groups;
  std::vector<HookDecl> hooks;       // by vaddr, merged over the files
};

// Appends the catalog in `text` (named `name` in messages). False and `err` on a syntax error.
bool parseCatalog(const std::string& text, const std::string& name, Catalog& out, std::string& err);
// Reads the files (a directory: every *.txt in it). Sorts and merges the hook declarations.
bool loadCatalogs(const std::vector<std::string>& paths, Catalog& out, std::string& err);

// What the variant compile leaves for the table (docs/RECOMP_ABI.md, "Variants"): one VFunc per (function, set of groups).
struct VUnit { uint64_t vaddr = 0, end = 0, ehash = 0; std::string symbol; int64_t orig = -1; /* index in bbr_units[], -1: only in the variant */ };
struct VFunc {
  uint64_t funcStart = 0;            // the original function the variant replaces
  uint32_t firstUnit = 0, unitCount = 0;
  std::string name;                  // "<func hex>+<group>+<group>" (diagnostics)
};
struct VariantTable { std::vector<VFunc> funcs; std::vector<VUnit> units; };
