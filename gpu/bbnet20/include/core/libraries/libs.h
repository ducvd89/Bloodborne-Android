// bbport: shadPS4 registers HLE functions with LIB_FUNCTION(nid, library,
// version, module, fn). Here the registrations fill a table that the C
// loader queries by NID (see bbgpu.cpp: bbgpu_resolve).
#pragma once
#include <string>
#include "common/types.h"
#include "common/assert.h"
#include "core/emulator_settings.h"

namespace Core::Loader {
enum class SymbolType { Function, Object };
class SymbolsResolver {
public:
    void AddSymbol(const char* nid, const char* library, const char* module, SymbolType type, u64 address);
};
} // namespace Core::Loader

// bbport-android: guest code (FEXCore) has no unwind data either: a C++ exception must not unwind into its frames.
#if defined(_WIN32) || defined(__ANDROID__)
#include <cstdio>
#include <exception>
#include <type_traits>
namespace Core::Loader {
/// bbport: guest code calls these entries directly. A C++ exception cannot unwind through guest
/// frames (no unwind data), so it ends the process with libunwind's "pc not in table"; the guard
/// reports it and returns an error to the guest instead.
template <auto F, typename Signature = decltype(F)>
struct GuestEntry;
template <auto F, typename R, typename... A>
struct GuestEntry<F, R(PS4_SYSV_ABI*)(A...)> {
    static R PS4_SYSV_ABI Call(A... args) {
        try {
            return F(args...);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "HLE: C++ exception in %s: %s\n", __PRETTY_FUNCTION__, e.what());
        } catch (...) {
            std::fprintf(stderr, "HLE: C++ exception in %s\n", __PRETTY_FUNCTION__);
        }
        if constexpr (std::is_void_v<R>) {
            return;
        } else if constexpr (std::is_same_v<R, s32> || std::is_same_v<R, int>) {
            return static_cast<R>(0x80020005); // ORBIS_KERNEL_ERROR_EIO
        } else {
            return R{};
        }
    }
};
} // namespace Core::Loader
#define LIB_FUNCTION(nid, lib, libversion, mod, function)                                          \
    sym->AddSymbol(nid, lib, mod, Core::Loader::SymbolType::Function,                             \
                   reinterpret_cast<u64>(&Core::Loader::GuestEntry<&function>::Call))
#else
#define LIB_FUNCTION(nid, lib, libversion, mod, function)                                          \
    sym->AddSymbol(nid, lib, mod, Core::Loader::SymbolType::Function,                             \
                   reinterpret_cast<u64>(function))
#endif
#define LIB_OBJ(nid, lib, libversion, mod, obj)                                                    \
    sym->AddSymbol(nid, lib, mod, Core::Loader::SymbolType::Object, reinterpret_cast<u64>(obj))
