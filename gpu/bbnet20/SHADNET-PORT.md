# Native shadNet bridge

The network-only source in `gpu/bbnet/vendor/shadps4/` is shadPS4's own, unchanged except
where marked `bbport`:
https://github.com/shadps4-emu/shadPS4/tree/v.0.20.0
(529ceb32f5546b820588f194b903cc39d08015fe): `src/core/libraries/net`, `network`, `np`
(without trophies, commerce and the profile and Facebook dialogs), `src/core/net` and
`src/shadnet`. These portions are GPL-2.0-or-later, with their original notices.
`third_party/httplib/httplib.h` and `third_party/wepoll/` retain their embedded
third-party license notices.

The bbport changes: the shadNet session starts once the game is loaded
(`NpManager::InitializeRuntime`, called by `bbnet_initialize`), and the client reports
itself to the server as this port (`BB_CLIENT_VERSION`), not as shadPS4.

The renderer, shaders and upscalers were not imported. `bbnet_glue.cpp` and `include/`
implement what these libraries need from the rest of shadPS4 (kernel threads, user
service, settings) on this project's runtime. Online play is off unless `BB_ONLINE=1`.

## The Hunter's Dream backend (`thd/`)

`thd/` is the co-op pool for The Hunter's Dream (`online_pool=huntersdream`, which sets
`BB_NET_BACKEND=thd`). It adapts droogie/bbhost's network side
(https://github.com/droogie/bbhost/tree/566184a74f071166299bb656c4af479e5bbe2771):
`src/hle/net.cpp`, `src/hle/np_matching2.cpp`, `src/hle/np_signaling.cpp`, the NP parts of
`src/hle/system.cpp`, and `src/net/{http,account,session,stun}.cpp`. These portions are
GPL-3.0-or-later; copyright remains with the bbhost contributors. With that pool the game's NP and
sceNet imports are these, ahead of the shadPS4 ones; the shadNet pool is unchanged. sceHttp stays
shadPS4's, with two `bbport` hooks that act only in that pool: `*.example.com` host-override keys
and the account token and ruleset headers.

## BBSC (Seamless Hunt co-op) under FEXCore (`bbsc/`, `BB_NET_BBSC=ON`, run with `BB_BBSC=1`)

BBSC's game code (`bbsc/core/`, upstream's, GPL-2.0-or-later) patches the game's x86-64 code with hooks and byte
patches. Here the game's code runs in FEXCore, so four things differ from a native x86-64 host (every change is in
`android-changes.diff`):
- Hooks (`gpu/shim/bbport_guest_code_hooks.{h,cpp}`, libbbgpu): the site's span (byte-checked, whole
  position-independent instructions) is replaced by a jump to a trampoline that saves the registers, flags, MXCSR
  and YMM registers into a `GuestRegisterSnapshot`, calls the hook, restores them, runs the replaced instructions
  and jumps back (a skip flag, function entries only, returns from the function at once). The hook is arm64 code
  the guest cannot call: the trampoline calls the guest-callable stub of one host function
  (`guest_cpu_host_function`), which calls the hook (`tests/run_guest_code_hooks_test.sh` runs the trampoline on a
  PC, both ways of calling). `BB_GUEST_HOOK_STATS=1` tells every 20 s which sites run how often.
- FEXCore here has no page tracking of the game's code (its `mtrack` lives in FEX's Linux layer, which is not
  linked): every write of code is followed by `guest_cpu_invalidate` (`Core::GuestCodeWritten`; BBSC's patch tables
  and hook installs do it), so BBSC's runtime switches take effect on the next run of the code. The code pages are
  read-write-execute while BBSC is on (`Core::KeepGuestCodeWritable`; a per-write unprotect would race the threads
  that switch patches).
- Calls of the game's functions (`BbGuestCall`; with float arguments, `Core::CallGuestWithFloats`).
- No SDL: the cinematic camera's event filter is never called.
A hook whose site differs from what BBSC expects is refused (nothing is written) and logged with who changed the
bytes: `tools/android/dev/bbsc_sites.sh` runs BBSC's install code against the 1.09 eboot, pristine and with the port's
patches applied.

## Online status for the app (`bbnet_status.cpp`, `BB_NET_STATUS_FILE`)

The app shows whether the session is up. The module rewrites the file `BB_NET_STATUS_FILE` names (written under another
name and renamed) with one line `state|detail` on every change; state is `connecting`, `online`, `offline`, `rejected`
or `error`, and the same line goes to the log ("Online status: ..."). Reported from `np_handler.cpp` (the connect, the
login and its failures, a dropped connection, `ClientRejected`), `shadnet/client.cpp` (connect retries),
`signaling_handler.cpp` (established P2P transports: "· N players connected") and `thd/thd_session.cpp` (The Hunter's
Dream has no connection of its own: the session service's answers, the 401 refusal, and its peer table). `Rejected`
is final for the run. The stubbed shadNet notification and invitation layers write their texts to `BB_NET_BANNER_FILE`
(the app's chip).
