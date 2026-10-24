// Prints the patched bytes of BBSC's persistence tables (gpu/bbnet20/bbsc/core/bbsc_patch_tables.h) as variant catalog groups.
// Built and run by gen_bbsc.py:  c++ -std=c++20 -I gpu/bbnet20/bbsc gen_bbsc_tables.cpp
#include "core/bbsc_patch_tables.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace Core::Bbsc::PatchTables;

static const char* groupName(Group g) {
    static const char* n[] = {"Stage", "Revive", "Boss", "MatchCheck", "Lamp", "Tasks", "HostDead", "NoFogWalls", "LeaveLimit", "Spectate",
                              "Echoes", "GuestItems", "SolidNpcs", "Scaling", "KitBells", "SendBacklog", "Hunters"};
    return n[static_cast<unsigned>(g)];
}

int main() {
    struct Line { u64 site; std::vector<u8> bytes; };
    std::vector<std::vector<Line>> groups(17);
    auto add = [&](Group g, u64 site, const u8* b, size_t n) { groups[static_cast<unsigned>(g)].push_back({site, std::vector<u8>(b, b + n)}); };
    for (const auto& e : Retargets) { const auto b = EncodeRel32(e.opcode, e.site, e.target); add(e.group, e.site, b.data(), b.size()); }
    for (const auto& e : EntryReturns) { const u8 c3 = 0xC3; add(e.group, e.site, &c3, 1); }
    for (const auto& e : ByteEdits) add(e.group, e.site, e.replacement.data(), e.replacement.size());
    for (const auto& e : WideEdits) add(e.group, e.site, e.replacement.data(), e.replacement.size());
    for (const auto& e : EntryJumps) { const auto b = EncodeRel32(0xE9, e.site, e.target); add(e.group, e.site, b.data(), b.size()); }
    for (unsigned g = 0; g < groups.size(); ++g) {
        if (groups[g].empty()) continue;
        std::printf("\ngroup bbsc:%s\n", groupName(static_cast<Group>(g)));
        for (const auto& l : groups[g]) {
            std::printf("w %llx ", static_cast<unsigned long long>(l.site));
            for (u8 b : l.bytes) std::printf("%02x", b);
            std::printf("\n");
        }
    }
    return 0;
}
