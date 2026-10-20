// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-android: the game's callbacks (function pointers it handed to sceNp*/sceNet*) run on the guest CPU
// (FEXCore): a plain call from this library would run x86-64 code as arm64. BbGuestCall goes through
// guest_cpu_call (gpu/shim/bbport_guest_call.h) and first makes sure the calling thread has a guest TCB and
// guest stack (the module's own threads are started with them; std::threads of the libraries are attached here
// on their first call). Integer, enum and pointer arguments and results only.
#pragma once

#include "bbport_guest_call.h"

namespace BbNet {
/// The calling host thread can run guest code (runtime_thread_attach_host; once per thread).
void EnsureGuestThread();
} // namespace BbNet

template <typename R, typename... P, typename... A>
inline R BbGuestCall(R (*fn)(P...), A... args) {
    BbNet::EnsureGuestThread();
    return GuestCall(fn, args...);
}
