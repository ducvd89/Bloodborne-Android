#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
/* Android (Bionic) builds run without SDL: audio goes to the app's FIFO, the pad comes from BB_PAD_FILE. */
#if defined(__ANDROID__) && !defined(BB_NO_SDL)
#define BB_NO_SDL 1
#endif
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#ifndef _WIN32
#include <setjmp.h>
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#endif
/* The guest's calling convention for host functions it calls. Elsewhere than x86-64 the guest CPU
 * (guest_cpu.h) converts guest calls to the host's convention. */
#if defined(__x86_64__)
#define ABI __attribute__((sysv_abi))
#else
#define ABI
#endif
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
/* Position and event-flag publisher for the Android app's map (src/runtime_atlas.c): with BB_ATLAS_FILE set,
 * starts a thread that reads the game's memory (the image, `size` bytes) and writes the shared file. */
void runtime_atlas_start(void *image, uint64_t size);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
/* The CPU is about to write the range outside guest code (a file read): tells the GPU side. */
void runtime_memory_note_write(uintptr_t address, uint64_t size);
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size);
void runtime_memory_prepare_cpu_write(uintptr_t address, uint64_t size);
/* bbport: the trap reasons (BbLayer::WriteTraps::Reason bits, e.g. 4 UmaWatch) the page holding `address` is trapped for; 0: none. */
unsigned runtime_memory_trap_reasons(uintptr_t address);
/* UMA granules (BB_UMA, src/runtime_memory.c): registering switches the runtime to lazy granules.
 * alloc(phys,size) returns a dma-buf fd of `size` bytes of GPU-visible memory for direct memory
 * [phys,phys+size), or -1 (the GPU side keeps its own record of it; the runtime owns the fd).
 * release(phys,size): the runtime gives back a granule it allocated but could not use (the GPU side
 * drops its record and frees it). granule: power of two, 256 KiB..64 MiB. Returns 0, or -1 (bad size,
 * already registered, chunk mode already active). Called once by the GPU library before the game maps
 * direct memory. Neither callback is called with the runtime lock held. */
int runtime_memory_set_granule_allocator(int (*alloc)(uint64_t phys, uint64_t size),
                                         void (*release)(uint64_t phys, uint64_t size), uint64_t granule);
/* Makes every granule overlapping direct memory [phys,phys+size) SHARED, promoting CPU ones. 1 when
 * all of them are shared afterwards, 0 otherwise (failed ones stay CPU). Blocking: it may call alloc
 * and release, wait up to 50 ms per granule for a kernel write into guest memory, and freezes the
 * game's writers of the granule for the copy. Never call it from a signal handler, from inside a
 * runtime callback that holds the runtime lock (returns 0 when this thread holds it exclusively), or
 * with a lock the allocator callbacks need. Promotions are serialized. */
int runtime_memory_promote(uint64_t phys, uint64_t size);
/* 1 if the granule holding phys is SHARED (lock-free, any thread). */
int runtime_memory_granule_shared(uint64_t phys);
typedef struct {
    uint64_t granule_bytes, shared_granules, shared_bytes, promotions, promotion_failures,
             refused_exec, refused_busy, freeze_faults, io_waits, promote_ns_total, promote_ns_max;
} RuntimeUmaStats; /* promote_ns: how long the game's writers were frozen */
void runtime_memory_uma_stats(RuntimeUmaStats *out);
/* Fault chain (probe.c): a fault on a page frozen by a promotion waits for it to finish and returns 1
 * (the access is retried); 0 for any other fault. Async-signal-safe. */
int runtime_memory_promotion_fault(void *address);
/* bbport UMA: VMA compaction of SHARED granules. The kernel never merges the mappings of a dma-buf
 * (special mappings, VM_PFNMAP/VM_IO), so every page protection change splits a granule's mappings for
 * good, and the process walks into vm.max_map_count. A compaction maps the granule's dma-buf again over
 * each run of its guest aliases (one mapping) and gives every page its protection back in runs: the
 * mappings left are the runs of different protections. No data moves; the game's writers wait as for a
 * promotion (frozen pages), kernel writes in flight make it skip the granule.
 * runtime_memory_compact_granule: the granule holding phys; returns the host mappings it left (>= 1),
 * 0 when the granule is not SHARED, -1 when it was skipped (kernel I/O in flight, executable or too many
 * aliases, or this thread holds the runtime lock).
 * runtime_memory_compact_shared: the (at most max) most fragmented SHARED granules, by /proc/self/maps,
 * those with at least min_gain more mappings than their last compaction left; returns how many.
 * A low-priority thread does this on its own: every 2 s it counts /proc/self/maps, and above
 * BB_UMA_COMPACT_AT mappings (default 20000, 0 off) compacts until 3/4 of that. Neither call is for a
 * signal handler or with the runtime lock held. */
int runtime_memory_compact_granule(uint64_t phys);
size_t runtime_memory_compact_shared(size_t max, uint64_t min_gain);
typedef struct {
    uint64_t rounds, granules, skipped, maps_last, maps_before, maps_after, compact_ns_total, compact_ns_max;
} RuntimeUmaCompactStats; /* maps_last: the last count of /proc/self/maps (0: none); before/after: last round */
void runtime_memory_uma_compact_stats(RuntimeUmaCompactStats *out);
/* The last count of /proc/self/maps by the compaction thread (0: not running, or no count yet). */
uint64_t runtime_memory_maps_count(void);
/* bbport UMA: demotion SHARED -> CPU, the mirror of promotion. Every SHARED granule overlapping direct
 * memory [phys,phys+size) goes back to the memfd: under the runtime lock and a freeze of its writers (as a
 * promotion's: every guest alias read-only, a fault waits and is retried, kernel writes in flight are waited
 * for up to 50 ms), its bytes are copied from the dma-buf into the memfd (pages that are all zero stay
 * holes: nothing committed for them; nothing at all when no direct memory is allocated there any more),
 * the memfd is mapped over the backing view and every alias, each page gets its exact protection back
 * (the guest's, the GPU page manager's, the traps'), the state goes to CPU and the runtime's fd is closed.
 * The GPU library's own handle of the memory is NOT released here: the caller frees it once the GPU is
 * done with it. Returns 1 when none of them is SHARED afterwards, 0 when one stayed SHARED (kernel I/O
 * in flight for too long, too many aliases, a mapping failed and was undone). Blocking, serialized with
 * promotions; not from a signal handler or with the runtime lock held (returns 0 then). */
int runtime_memory_demote(uint64_t phys, uint64_t size);
/* Per granule of direct memory, out[g] = RUNTIME_GRANULE_* bits (SHARED; some direct memory allocated in
 * it; some of it mapped). Returns the number of granules (writes min(count, that) entries); 0 without UMA. */
enum { RUNTIME_GRANULE_SHARED=1, RUNTIME_GRANULE_ALLOCATED=2, RUNTIME_GRANULE_MAPPED=4 };
size_t runtime_memory_granule_scan(uint8_t *out, size_t count);
typedef struct {
    uint64_t demotions, released, busy, failures, kept_bytes, demoted_bytes, demote_ns_total, demote_ns_max;
} RuntimeUmaDemoteStats; /* released: nothing allocated there (no copy); kept_bytes: non-zero pages copied back */
void runtime_memory_uma_demote_stats(RuntimeUmaDemoteStats *out);
/* bbport BB_TEX_HOME (src/runtime_memory.c, "Textures homed in the GPU"; docs/ANDROID_UNIFIED_MEMORY_PLAN.md,
 * "Plan A"): pages of direct memory whose bytes a GPU image holds exactly are dropped (no access on every
 * guest alias, the memfd range punched out) and rebuilt on any access. */
enum { RUNTIME_HOME_OK, RUNTIME_HOME_ARGS, RUNTIME_HOME_MAPPING, RUNTIME_HOME_SHARED, RUNTIME_HOME_HOMED,
       RUNTIME_HOME_ALIASES, RUNTIME_HOME_FULL, RUNTIME_HOME_MISMATCH, RUNTIME_HOME_CODES };
#define RUNTIME_HOME_TRAP 64u /* the trap reason of homed pages (runtime_memory_trap_reasons) */
/* Homes guest [address, address+size) (aligned to runtime_memory_home_granule(), in one direct memory
 * mapping, no page homed already, not in a shared UMA granule): traps every alias of its pages (no access),
 * compares them with `expected` and, when equal, punches them out of the memfd. 0 and *ticket set, or a
 * RUNTIME_HOME_* refusal (MISMATCH: the bytes differ; nothing changed). Takes the runtime lock exclusively. */
int runtime_memory_home(uintptr_t address, uint64_t size, const void *expected, uint32_t *ticket);
/* The pages of a home back: `data` (the home's whole range, `size` its size) written into the pages still
 * homed, their traps lifted (pages the GPU page manager write-protects stay read-only). data NULL: the pages
 * come back as zeros (forgotten). Returns the pages restored, -1 for a ticket that is gone (all released or
 * restored already), -2 for a wrong size. */
int runtime_memory_home_restore(uint32_t ticket, const void *data, uint64_t size);
uint64_t runtime_memory_home_alive(uint32_t ticket);      /* pages still homed, 0: gone */
uint32_t runtime_memory_home_ticket(uintptr_t address);   /* the home of the page at address, 0 none */
int runtime_memory_home_tickets(uintptr_t address, uint64_t size, uint32_t *out, int max); /* homes behind a range */
int runtime_memory_home_touches(uintptr_t address, uint64_t size); /* lock-free: a page of the range is homed */
/* A fault on a page with RUNTIME_HOME_TRAP but no home (stale): the trap goes, 1. 0: the page is homed. */
int runtime_memory_home_unstale(uintptr_t address);
uint64_t runtime_memory_home_granule(void);               /* the host page size */
uint64_t runtime_memory_homed_pages(void);                /* 4 KiB pages homed now */
/* Called with a ticket before the runtime touches homed pages itself (backing view reads and writes, write
 * announcements, kernel I/O), with no runtime lock held; returns 1 when the home is gone afterwards. */
void runtime_memory_set_home_hook(int (*rehydrate)(uint32_t ticket));
typedef struct {
    uint64_t homes, restores, restored_pages, forgotten_pages, released_pages, refused_promotions, hook_calls,
             homed_pages, refused[RUNTIME_HOME_CODES];
} RuntimeHomeStats;
void runtime_memory_home_stats(RuntimeHomeStats *out);
/* bbport: 1 when the calling thread runs the game's code (`ucontext`: at a signal; NULL: now, in a host
 * function the game called), 0 for host threads (src/runtime_kernel.c). */
int runtime_thread_is_guest(const void *ucontext);
/* File/IO reads the kernel performs into guest memory: no promotion may freeze those pages meanwhile. */
void *runtime_memory_io_begin(uintptr_t address, uint64_t size);
void runtime_memory_io_end(void *token);
/* bbport (BB_WAIT_PROFILE=1, off by default: it costs CPU): a guest thread was blocked `ns` in the
 * runtime (0 cond, 1 mutex, 2 sema, 3 sleep). Callers skip their clock reads unless
 * runtime_wait_profiling() says the profiler is on. */
int runtime_wait_profiling(void);
void runtime_wait_note(int kind, uint64_t ns);
void runtime_wait_report(double frames);
void runtime_guest_call_sites(uint64_t out[3]);
/* bbport: times the game's heap asked for more memory (posix_mmap): a sign it leaks. */
uint64_t runtime_heap_growths(void);
static inline uint64_t runtime_wait_clock(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec; }
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
/* Online play (runtime_net.c, BB_ONLINE=1): the online module answers the network and PSN imports. */
void runtime_net_configure(const char *app0, const char *serial, const char *title, const char *version);
int runtime_net_enabled(void);
int runtime_net_symbol(const char *symbol);
uintptr_t runtime_net_resolve(const char *name);
void runtime_net_image_ready(uintptr_t base, uint64_t size);
void runtime_net_image_mapped(uint8_t *image, uint64_t size);
void runtime_net_start(void);
/* Who wrote the game's code (runtime_patches.c): the patch files (probe.c) and the GPU library's hooks note each
 * write; a game module whose own hook finds other bytes than it expects asks (gpu/bbnet20: BBSC). `owner` has
 * static storage. The owner of the first noted write overlapping [address, address + size), or NULL. */
void runtime_code_patch_note(uintptr_t address, size_t size, const char *owner);
const char *runtime_code_patch_owner(uintptr_t address, size_t size);
__attribute__((noreturn)) void runtime_thread_exit(void *value);
/* Threads for the online module's host code (runtime_thread.c): `entry` is a HOST function, run on a native thread
 * that has a guest TCB and guest stack (it may call the game's code through guest_cpu_call / GuestCall) and ends
 * with runtime_thread_exit or by returning. 0 or a positive errno value. */
int32_t runtime_thread_spawn(void **thread, void *(ABI *entry)(void *), void *argument, uint64_t stack, const char *name);
int32_t runtime_thread_spawn_guest(void **thread, void *(ABI *entry)(void *), void *argument, uint64_t stack, const char *name);
int32_t runtime_thread_join_spawned(void *thread, void **result);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
/* The text of the game's last panic ("[Dantelion2 Panic] file(line) message", printed to fd 1/2 before its deliberate crash) as one line, or 0 (out is
 * then empty) when it printed none. Async-signal-safe. */
size_t runtime_panic_text(char *out, size_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
void runtime_low_protect(uintptr_t start, size_t size, int prot);
int runtime_memory_low_info(uintptr_t address, int *prot, uintptr_t *end);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
void runtime_thread_attach_host(const char *name);
#endif
