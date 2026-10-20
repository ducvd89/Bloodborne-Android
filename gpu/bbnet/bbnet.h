/* bbport online module (libbbnet.so, gpu/bbnet): the PS4 network and PSN libraries with the
 * shadNet client. A separate library the runtime loads only for online play (BB_ONLINE=1,
 * src/runtime_net.c); offline the port never touches it, and a build without it runs offline.
 * Its C++ inside is private (hidden symbols, -Bsymbolic): these two calls are all it exports. */
#ifndef BBNET_H
#define BBNET_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BBNET_ABI_VERSION 3 /* 3: thread_spawn_guest and thread_attach (bbport-android) */

/* The calling convention of the game's functions as the runtime's host code sees them (runtime.h ABI): System V
 * on x86-64 hosts; elsewhere the host's own (the guest CPU converts, src/guest_cpu.h). bbport-android. */
#if defined(__x86_64__)
#define BBNET_SYSV __attribute__((sysv_abi))
#else
#define BBNET_SYSV
#endif

/* What the runtime gives the module: the game's identity (param.sfo, its folder for
 * sce_sys/npbind.dat) and guest threads for the NP libraries' callbacks into the game. */
typedef struct BbNetHost {
    uint32_t abi;           /* BBNET_ABI_VERSION */
    const char *app0;       /* the game's folder */
    const char *serial;     /* TITLE_ID */
    const char *title;      /* TITLE */
    const char *app_ver;    /* APP_VER */
    /* 0 or a positive errno value. entry is HOST code (the module's own thread function, in the host's calling
     * convention): the runtime runs it on a native thread that has a guest TCB and guest stack, so it can call
     * the game's callbacks (GuestCall); it never runs as guest code. */
    int32_t (*thread_spawn)(void **thread, void *(BBNET_SYSV *entry)(void *),
                            void *argument, uint64_t stack_size, const char *name);
    /* The same for a guest function (the start routine the game itself hands to sceNpCreateThread), which runs as
     * guest code on the guest CPU. Where the guest CPU is the host's they are the same. */
    int32_t (*thread_spawn_guest)(void **thread, void *(BBNET_SYSV *entry)(void *), void *argument,
                                  uint64_t stack_size, const char *name);
    int32_t (*thread_join)(void *thread, void **result);
    /* The calling host thread (one of the libraries' own std::threads) gets a guest TCB and stack, so the module can
     * call the game's callbacks from it; a thread that has them already is left as it is. */
    void (*thread_attach)(const char *name);
    /* Ends the calling guest thread (pthread_exit of the NP libraries' threads); does not return. */
    void (*thread_exit)(void *value);
    /* Nonzero when [address, address+size) is mapped guest memory (safe to read). */
    int (*memory_is_mapped)(uintptr_t address, uint64_t size);
} BbNetHost;

/* Registers the libraries; 0 on success, else the module cannot run (the game stays offline). */
typedef int (*BbNetInitFn)(const BbNetHost *host);
int bbnet_init(const BbNetHost *host);

/* Optional: the game's image is mapped and patched, before its first instruction runs (game
 * modules of the online module install their hooks here: BBSC for Bloodborne). */
typedef void (*BbNetImageReadyFn)(uint64_t image_base, uint64_t image_size);
void bbnet_image_ready(uint64_t image_base, uint64_t image_size);

/* Optional: the game's image is mapped and relocated but not yet patched (modules that read the
 * original code, such as the Hunter's Dream backend, look at it here). */
typedef void (*BbNetImageMappedFn)(uint8_t *image, uint64_t image_size);
void bbnet_image_mapped(uint8_t *image, uint64_t image_size);

/* Optional: the game is loaded, its first instruction is next (online sessions start here). */
typedef void (*BbNetStartFn)(void);
void bbnet_start(void);

/* The module's function for an imported NID ("NID#lib#mod"), or 0. */
typedef uintptr_t (*BbNetResolveFn)(const char *scoped_nid);
uintptr_t bbnet_resolve(const char *scoped_nid);

#ifdef __cplusplus
}
#endif
#endif
