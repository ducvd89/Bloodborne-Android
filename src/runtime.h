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
/* File/IO reads the kernel performs into guest memory: no promotion may freeze those pages meanwhile. */
void *runtime_memory_io_begin(uintptr_t address, uint64_t size);
void runtime_memory_io_end(void *token);
/* bbport (frame stats): a guest thread was blocked `ns` in the runtime (0 cond, 1 mutex, 2 sema, 3 sleep). */
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
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
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
