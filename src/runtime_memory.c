/* PS4 virtual memory: direct (physical) pool, flexible memory, reservations,
 * protection changes and queries. One sparse memfd backs the whole direct
 * pool, so any mapping of any physical range aliases the same storage.
 * Guest addresses are placed below 1 TiB (PS4 user range): GPU descriptors
 * encode 40-bit addresses, so host-default 0x7f... addresses would not fit. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef SEEK_DATA
#define SEEK_DATA 3
#define SEEK_HOLE 4
#endif
/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. BB_DMEM_MB raises it (the resolution
 * patches above 1080p need about 4 GiB more; run.sh sets it). */
static uint64_t pool_size_bytes(void) {
    static uint64_t size;
    if (!size) {
        const char *env = getenv("BB_DMEM_MB");
        uint64_t mb = env ? strtoull(env, NULL, 10) : 0;
        if (mb < 5056 || mb > 16384) mb = 5056;
        size = mb * 1024 * 1024;
    }
    return size;
}
#define POOL_SIZE pool_size_bytes()
#define FLEXIBLE_SIZE (UINT64_C(448) * 1024 * 1024)
#define PAGE UINT64_C(16384)
#define USER_MIN UINT64_C(0x1000000000)
#define USER_MAX UINT64_C(0xfc00000000)
#define LIMIT 4096
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define ACCESS ((int32_t)UINT32_C(0x8002000d))
#define NO_SPACE ((int32_t)UINT32_C(0x80020023))
#define MAP_FIXED_FLAG 0x10
#define MAP_NO_OVERWRITE 0x80
enum { KIND_RESERVED=1, KIND_DIRECT, KIND_FLEXIBLE };
typedef struct { uint64_t start, size; int type, used; } Block;
typedef struct { uintptr_t start, end; int kind, prot, type; uint64_t phys; } Vma;
typedef struct {
    uintptr_t start, end; uint64_t offset; int32_t protection, memory_type;
    uint32_t flags; char name[32];
} VirtualQueryInfo; /* flags: flexible, direct, stack, pooled, committed */
typedef struct { void *start; uint64_t offset, length; int8_t prot, type; int16_t reserved; int32_t operation; } BatchEntry;
_Static_assert(sizeof(VirtualQueryInfo)==72,"PS4 virtual query info layout");
_Static_assert(sizeof(BatchEntry)==32,"PS4 batch map entry layout");

/* Region table lock. Lookups (GPU per-draw queries, VirtualQuery, GPU page
 * protection) share it; map/unmap/protect take it exclusively. Both nest: code
 * under the lock may write guest memory, and a GPU-tracked page then faults into
 * runtime_memory_gpu_protect on the same thread. Readers nest because glibc's
 * default rwlock prefers readers; a thread holding it exclusively skips relocking. */
static pthread_rwlock_t lock=PTHREAD_RWLOCK_INITIALIZER;
static __thread unsigned exclusive_depth;
/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */
static uint64_t table_generation;
static void write_lock(void) {
    if (!exclusive_depth++) { pthread_rwlock_wrlock(&lock); __atomic_add_fetch(&table_generation,1,__ATOMIC_ACQ_REL); }
}
static void write_unlock(void) {
    if (!--exclusive_depth) { __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE); pthread_rwlock_unlock(&lock); }
}
static void read_lock(void) { if (!exclusive_depth) pthread_rwlock_rdlock(&lock); }
static void read_unlock(void) { if (!exclusive_depth) pthread_rwlock_unlock(&lock); }
static Block blocks[LIMIT];
static Vma *vmas; static size_t vma_count, vma_capacity;
static int pool_fd=-1;
/* bbport: direct memory in GPU-visible system memory the GPU library allocates (dma-buf chunks,
 * BB_GUEST_GPU_MEMORY=1): the GPU reads the game's data in place instead of the CPU copying it
 * into staging buffers first. Chunks are allocated when the game allocates direct memory in them;
 * a chunk the GPU library cannot provide stays in the memfd. */
#define CHUNK (UINT64_C(256) << 20)
typedef int (*GuestChunkAlloc)(uint64_t phys, uint64_t size);
static GuestChunkAlloc chunk_alloc;
static int *chunk_fds; /* per CHUNK of direct memory: -1 not decided, -2 memfd, -3 memfd the GPU
                        * imported (host memory: VK_EXT_external_memory_host), else a dma-buf fd */
#define CHUNK_IMPORTED (-3)
/* Released memory in a chunk the GPU uses is cleared, not punched out: the GPU holds those pages. */
#define GPU_CHUNK(fd) ((fd)>=0 || (fd)==CHUNK_IMPORTED)
/* Drivers whose dma-buf maps at offset 0 only (NVIDIA): a chunk per direct allocation instead of
 * the CHUNK grid. The game maps each of its allocations once, whole, so its mappings and the
 * backing view start at offset 0 of their chunk. */
static unsigned char *backing_base; /* second view of the pool: host writes bypass guest/GPU page protection */
static int whole_chunks;
typedef struct { uint64_t phys, size; int fd; } Region;
#define MAX_REGIONS 256
static Region regions[MAX_REGIONS];
static size_t region_count;
/* The region holding phys (sorted by phys), or NULL; *next: start of the next region above. */
static const Region *region_at(uint64_t phys, uint64_t *next) {
    *next=POOL_SIZE;
    for (size_t i=0;i<region_count;++i) {
        if (phys<regions[i].phys) { *next=regions[i].phys; return NULL; }
        if (phys<regions[i].phys+regions[i].size) return &regions[i];
    }
    return NULL;
}
static void add_region(uint64_t phys, uint64_t size) {
    if (region_count==MAX_REGIONS) return;
    int fd=chunk_alloc(phys,size);
    if (fd>=0 && mmap(backing_base+phys,size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0)==MAP_FAILED) {
        close(fd);
        fd=-1;
    }
    size_t i=region_count;
    while (i>0 && regions[i-1].phys>phys) { regions[i]=regions[i-1]; --i; }
    regions[i]=(Region){phys,size,fd>=0 || fd==CHUNK_IMPORTED ? fd : -2};
    ++region_count;
}
/* bbport: UMA granules (BB_UMA, runtime_memory_set_granule_allocator): the third mode. Direct
 * memory is split into granules; each is CPU (the memfd, lazily committed, as without UMA) or SHARED
 * (a GPU-visible dma-buf the GPU library allocated when the GPU first needed the memory in place;
 * promotion, below). Nothing is allocated up front: direct_allocate/ensure_chunks do nothing here,
 * the chunk and whole-allocation code above never runs. States change under the write lock only;
 * runtime_memory_granule_shared reads them without it. */
typedef int (*GranuleAlloc)(uint64_t phys, uint64_t size);
typedef void (*GranuleRelease)(uint64_t phys, uint64_t size);
static GranuleAlloc granule_alloc;
static GranuleRelease granule_release;
static uint64_t granule_size;
static _Atomic(int) *granule_fd; /* per granule: -1 CPU (memfd), else the SHARED dma-buf fd; NULL: UMA off */
static int uma_fd(uint64_t g) { return atomic_load_explicit(&granule_fd[g],memory_order_acquire); }
/* End of granule g in direct memory (the last one may be shorter). */
static uint64_t uma_end(uint64_t g) {
    const uint64_t end=(g+1)*granule_size;
    return end<POOL_SIZE ? end : POOL_SIZE;
}
/* The run of granules from phys (below end) in the state of the first one: its length, *shared set.
 * A SHARED run is one granule (each has its own fd) unless merge_shared (clearing, not mapping). */
static uint64_t uma_run(uint64_t phys, uint64_t end, int *shared, int merge_shared) {
    uint64_t g=phys/granule_size, at=uma_end(g);
    *shared=uma_fd(g)>=0;
    while (at<end && (!*shared || merge_shared) && (uma_fd(at/granule_size)>=0)==*shared) at=uma_end(at/granule_size);
    return (at<end ? at : end)-phys;
}
/* The GPU page manager's last restriction of each 4 KiB page below 2^40 (runtime_memory_gpu_protect
 * only called mprotect before): 0 none recorded, else 4|read|write<<1. Promotion needs it to give a
 * remapped page the protection it had. Only kept in UMA mode; whatever else changes a page's
 * protection (a trap, the game's mprotect, a new mapping) drops the record (gpu_forget): the record
 * is the restriction that is in force. */
#define GPU_STATE_LIMIT (1ull<<40)
static uint8_t *gpu_state;
static void gpu_forget(uintptr_t start, uintptr_t end) {
    if (!gpu_state || start>=GPU_STATE_LIMIT) return;
    if (end>GPU_STATE_LIMIT) end=GPU_STATE_LIMIT;
    memset(gpu_state+(start>>12),0,((end+4095)>>12)-(start>>12));
}
#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)
static uint64_t flex_bitmap[FLEX_SPAN/PAGE/64];
/* GPU hooks (bbgpu): notified outside the lock, in order, after each operation. */
typedef void (*GpuRange)(uintptr_t address, uint64_t size);
static GpuRange hook_map, hook_unmap, hook_invalidate;
static GpuRange hook_note_write; /* bbport: data written into GPU memory (runtime_memory_note_write) */
enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };
typedef struct { int kind; uintptr_t address; uint64_t size; } PendingHook;
static PendingHook pending[64]; static size_t pending_count;
static size_t allocations, maps, flexible_maps, protects, queries;
static uint64_t live_bytes, flexible_bytes;

static int valid_alignment(uint64_t a) { return a >= PAGE && a <= POOL_SIZE && !(a & (a-1)); }
static uint64_t align_up(uint64_t n, uint64_t a) { return (n+a-1) & ~(a-1); }
static int host_prot(int prot) {
    /* GPU read/write bits (0x10/0x20) need host access for the future GPU backend. */
    return ((prot & 0x11) ? PROT_READ : 0) | ((prot & 0x22) ? PROT_WRITE|PROT_READ : 0) | ((prot & 4) ? PROT_EXEC|PROT_READ : 0);
}
/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (pool_fd>=0) return 0;
    pool_fd=memfd_create("bb-guest-memory", MFD_CLOEXEC);
    if (pool_fd<0 || ftruncate(pool_fd,(off_t)(POOL_SIZE+FLEX_SPAN))) return -1;
    void *view=mmap(NULL,POOL_SIZE+FLEX_SPAN,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,pool_fd,0);
    if (view==MAP_FAILED) return -1;
    backing_base=view;
    chunk_fds=malloc((POOL_SIZE/CHUNK+1)*sizeof(int));
    if (!chunk_fds) return -1;
    for (uint64_t c=0;c<=POOL_SIZE/CHUNK;++c) chunk_fds[c]=-1;
    return 0;
}
/* Size of direct memory chunk c (the last one may be shorter). */
static uint64_t chunk_size(uint64_t c) {
    const uint64_t start=c*CHUNK;
    return POOL_SIZE-start<CHUNK ? POOL_SIZE-start : CHUNK;
}
/* Decides where the chunks of [phys, phys+size) of direct memory live (lock held). */
static void ensure_chunks(uint64_t phys, uint64_t size) {
    if (!chunk_fds || phys>=POOL_SIZE || granule_fd) return; /* UMA granules are lazy */
    if (whole_chunks) {
        if (!chunk_alloc) return;
        const uint64_t end=phys+size<POOL_SIZE ? phys+size : POOL_SIZE;
        for (uint64_t at=phys;at<end;) {
            uint64_t next;
            const Region *r=region_at(at,&next);
            if (r) { at=r->phys+r->size; continue; }
            const uint64_t gap_end=next<end ? next : end;
            add_region(at,gap_end-at);
            at=gap_end;
        }
        return;
    }
    const uint64_t last=(phys+size-1<POOL_SIZE ? phys+size-1 : POOL_SIZE-1)/CHUNK;
    for (uint64_t c=phys/CHUNK;c<=last;++c) {
        if (chunk_fds[c]!=-1) continue;
        int fd=chunk_alloc ? chunk_alloc(c*CHUNK,chunk_size(c)) : -1;
        if (fd>=0 && mmap(backing_base+c*CHUNK,chunk_size(c),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0)==MAP_FAILED) {
            close(fd);
            fd=-1;
        }
        chunk_fds[c]=fd>=0 || fd==CHUNK_IMPORTED ? fd : -2;
    }
}
/* UMA mode: one mmap per run of CPU granules (the memfd, as ever), one per SHARED granule it covers
 * (its dma-buf at the offset inside the granule). */
static void *map_uma(uintptr_t address, uint64_t size, int prot, uint64_t phys) {
    for (uint64_t done=0;done<size;) {
        const uint64_t at=phys+done;
        int shared;
        const uint64_t n=uma_run(at,phys+size,&shared,0);
        const int fd=shared ? uma_fd(at/granule_size) : pool_fd;
        if (mmap((void *)(address+done),n,prot,MAP_SHARED|MAP_FIXED,fd,(off_t)(shared ? at%granule_size : at))==MAP_FAILED) {
            static int reported;
            if (reported++ < 4)
                fprintf(stderr,"Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n",
                        at,n,shared ? "a shared granule (dma-buf)" : "the memfd",strerror(errno));
            return MAP_FAILED;
        }
        done+=n;
    }
    return (void *)address;
}
/* mmap of direct or flexible memory at phys, split where it crosses chunks. */
static void *map_phys(uintptr_t address, uint64_t size, int prot, uint64_t phys) {
    if (!chunk_fds || phys>=POOL_SIZE)
        return mmap((void *)address,size,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)phys);
    if (granule_fd) return map_uma(address,size,prot,phys);
    ensure_chunks(phys,size);
    for (uint64_t done=0;whole_chunks && done<size;) {
        const uint64_t at=phys+done;
        uint64_t next;
        const Region *r=region_at(at,&next);
        const uint64_t room=r ? r->phys+r->size-at : next-at, n=size-done<room ? size-done : room;
        void *p = r && r->fd>=0
            ? mmap((void *)(address+done),n,prot,MAP_SHARED|MAP_FIXED,r->fd,(off_t)(at-r->phys))
            : mmap((void *)(address+done),n,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)at);
        if (p==MAP_FAILED) {
            static int reported;
            if (reported++ < 4)
                fprintf(stderr,"Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n",
                        at,n,r && r->fd>=0 ? "a GPU-visible dma-buf (whole-allocation chunk)" : "the memfd",strerror(errno));
            return MAP_FAILED;
        }
        done+=n;
        if (done==size) return (void *)address;
    }
    for (uint64_t done=0;done<size;) {
        const uint64_t at=phys+done, c=at/CHUNK;
        const uint64_t room=c*CHUNK+chunk_size(c)-at, n=size-done<room ? size-done : room;
        void *p = chunk_fds[c]>=0
            ? mmap((void *)(address+done),n,prot,MAP_SHARED|MAP_FIXED,chunk_fds[c],(off_t)(at-c*CHUNK))
            : mmap((void *)(address+done),n,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)at);
        if (p==MAP_FAILED) {
            static int reported;
            if (reported++ < 4)
                fprintf(stderr,"Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n",
                        at,n,chunk_fds[c]>=0 ? "a GPU-visible dma-buf chunk" : "the memfd",strerror(errno));
            return MAP_FAILED;
        }
        done+=n;
    }
    return (void *)address;
}
/* Returns released physical memory to zero: hole punching in the memfd, a clear in a chunk or a shared granule. */
static void zero_phys(uint64_t phys, uint64_t size) {
    for (uint64_t done=0;done<size;) {
        const uint64_t at=phys+done;
        if (!chunk_fds || at>=POOL_SIZE) {
            fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)at,(off_t)(size-done));
            return;
        }
        if (granule_fd) {
            /* bbport UMA: SHARED granules are cleared (dma-buf pages cannot be punched out). The GPU side
             * demotes a granule with nothing allocated in it (runtime_memory_demote): back to CPU, its
             * pages freed (zero pages are not copied back). */
            int shared;
            const uint64_t n=uma_run(at,phys+size<POOL_SIZE ? phys+size : POOL_SIZE,&shared,1);
            if (shared) memset(backing_base+at,0,n);
            else fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)at,(off_t)n);
            done+=n;
            continue;
        }
        if (whole_chunks) {
            uint64_t next;
            const Region *r=region_at(at,&next);
            const uint64_t room=r ? r->phys+r->size-at : next-at, n=size-done<room ? size-done : room;
            if (r && GPU_CHUNK(r->fd)) memset(backing_base+at,0,n);
            else fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)at,(off_t)n);
            done+=n;
            continue;
        }
        const uint64_t c=at/CHUNK, room=c*CHUNK+chunk_size(c)-at, n=size-done<room ? size-done : room;
        if (GPU_CHUNK(chunk_fds[c])) memset(backing_base+at,0,n);
        else fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)at,(off_t)n);
        done+=n;
    }
}
static void queue_hook(int kind, uintptr_t address, uint64_t size) {
    GpuRange hook = kind==HOOK_MAP ? hook_map : kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
    if (!hook) return;
    if (pending_count==sizeof(pending)/sizeof(*pending)) { fputs("STOP: GPU hook queue overflow\n",stderr); exit(21); }
    pending[pending_count++]=(PendingHook){kind,address,size};
}
/* Runs queued GPU notifications; callers invoke it after dropping the lock. */
static void flush_hooks(void) {
    for (;;) {
        write_lock();
        if (!pending_count) { write_unlock(); return; }
        PendingHook h=pending[0];
        memmove(pending,pending+1,--pending_count*sizeof(*pending));
        GpuRange hook = h.kind==HOOK_MAP ? hook_map : h.kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
        write_unlock();
        if (hook) hook(h.address,h.size);
    }
}
static int flex_test(uint64_t page) { return (int)((flex_bitmap[page/64]>>(page%64))&1); }
static void flex_set(uint64_t first, uint64_t count, int used) {
    for (uint64_t p=first;p<first+count;++p) {
        if (used) flex_bitmap[p/64]|=UINT64_C(1)<<(p%64); else flex_bitmap[p/64]&=~(UINT64_C(1)<<(p%64));
    }
}
static uint64_t flex_alloc(uint64_t size) {
    uint64_t pages=size/PAGE, total=FLEX_SPAN/PAGE, run=0;
    for (uint64_t p=0;p<total;++p) {
        run = flex_test(p) ? 0 : run+1;
        if (run==pages) { flex_set(p+1-pages,pages,1); return POOL_SIZE+(p+1-pages)*PAGE; }
    }
    return UINT64_MAX;
}
static void flex_free(uint64_t phys, uint64_t size) {
    flex_set((phys-POOL_SIZE)/PAGE,size/PAGE,0);
    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size);
}
static size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    size_t lo=0, hi=vma_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (vmas[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static int vma_insert(size_t at, Vma v) {
    if (vma_count==vma_capacity) {
        size_t capacity=vma_capacity ? vma_capacity*2 : 256;
        Vma *next=realloc(vmas,capacity*sizeof(*vmas));
        if (!next) return -1;
        vmas=next; vma_capacity=capacity;
    }
    memmove(vmas+at+1,vmas+at,(vma_count-at)*sizeof(*vmas));
    vmas[at]=v; ++vma_count; return 0;
}
static void vma_erase(size_t at, size_t n) {
    memmove(vmas+at,vmas+at+n,(vma_count-at-n)*sizeof(*vmas)); vma_count-=n;
}
static int split_at(uintptr_t a) {
    size_t i=vma_index(a);
    if (i==vma_count || vmas[i].start>=a) return 0;
    Vma right=vmas[i];
    right.start=a;
    if (right.kind==KIND_DIRECT) right.phys+=a-vmas[i].start;
    vmas[i].end=a;
    return vma_insert(i+1,right);
}
/* Range [start,end) split on its edges; returns the first index inside it. */
static size_t carve(uintptr_t start, uintptr_t end, int *error) {
    *error = split_at(start) || split_at(end);
    return vma_index(start);
}
static int covered(uintptr_t start, uintptr_t end, int allow_reserved) {
    size_t i=vma_index(start); uintptr_t at=start;
    for (; at<end; ++i) {
        if (i==vma_count || vmas[i].start>at || (!allow_reserved && vmas[i].kind==KIND_RESERVED)) return 0;
        at=vmas[i].end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (!ignore_reserved || vmas[i].kind!=KIND_RESERVED) return 1;
    return 0;
}
/* First-fit search in the PS4 user range, starting from the hint. */
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    uintptr_t at=align_up(hint<USER_MIN ? USER_MIN : hint, alignment);
    for (size_t i=vma_index(at); at+size<=USER_MAX; ++i) {
        if (i==vma_count || vmas[i].start>=at+size) return at;
        at=align_up(vmas[i].end,alignment);
    }
    return 0;
}
static void drop_range(uintptr_t start, uintptr_t end) {
    int error; size_t i=carve(start,end,&error), n=0;
    while (i+n<vma_count && vmas[i+n].end<=end && vmas[i+n].start>=start) {
        if (vmas[i+n].kind==KIND_FLEXIBLE) {
            flexible_bytes-=vmas[i+n].end-vmas[i+n].start;
            flex_free(vmas[i+n].phys,vmas[i+n].end-vmas[i+n].start);
        }
        ++n;
    }
    vma_erase(i,n);
}
/* bbport: write traps (runtime_memory_trap, below), kept across the game's own changes. */
static void trap_reapply(uintptr_t start, uintptr_t end);
static void trap_forget(uintptr_t start, uintptr_t end);
/* bbport BB_TEX_HOME: textures homed in the GPU (below, "Textures homed in the GPU"). */
static _Atomic uint64_t home_pages; /* direct memory pages (4 KiB) homed now; 0: none, every hook is one load */
static void home_map(uintptr_t address, uint64_t size, uint64_t phys);
static void home_release(uint64_t phys, uint64_t size);
static int home_collect_va(uintptr_t address, uint64_t size, uint32_t *out, int max);
static void home_rehydrate_va(uintptr_t address, uint64_t size);
static int home_mprotect(uintptr_t start, uintptr_t end, int prot);
static int home_any_phys(uint64_t phys, uint64_t size);
static void home_va_forget(uintptr_t start, uintptr_t end);
/* Places a host mapping; with MAP_FIXED it replaces what the guest had there. */
static int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment,
                     int kind, int type, uint64_t phys) {
    uintptr_t address=(uintptr_t)*inout;
    if (flags & MAP_FIXED_FLAG) {
        if (!address || address%PAGE) return INVALID;
        if ((flags & MAP_NO_OVERWRITE) && overlaps(address,address+size,1)) return NO_MEMORY;
    } else {
        address=find_free(address,size,alignment);
        if (!address) return NO_MEMORY;
    }
    void *mapped = kind!=KIND_RESERVED
        ? map_phys(address,size,host_prot(prot),phys)
        : mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED,-1,0);
    if (mapped==MAP_FAILED) return NO_MEMORY;
    drop_range(address,address+size);
    trap_forget(address,address+size); /* a new mapping starts without traps */
    gpu_forget(address,address+size);  /* ... and without the GPU page manager's restriction */
    if (vma_insert(vma_index(address),(Vma){address,address+size,kind,prot,type,phys})) return NO_MEMORY;
    if (kind==KIND_DIRECT && atomic_load_explicit(&home_pages,memory_order_relaxed)) home_map(address,size,phys);
    if (kind==KIND_FLEXIBLE) flexible_bytes+=size;
    if (kind!=KIND_RESERVED) queue_hook(HOOK_MAP,address,size);
    *inout=mapped;
    return 0;
}
static int block_type(uint64_t phys) {
    for (int i=0;i<LIMIT;++i)
        if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) return blocks[i].type;
    return -1;
}
static int direct_range_allocated(uint64_t phys, uint64_t size) {
    for (uint64_t at=phys; at<phys+size;) {
        int found=0;
        for (int i=0;i<LIMIT;++i) if (blocks[i].used && at>=blocks[i].start && at-blocks[i].start<blocks[i].size) {
            at=blocks[i].start+blocks[i].size; found=1; break;
        }
        if (!found) return 0;
    }
    return 1;
}

static ABI uint64_t direct_size(void) { return POOL_SIZE; }
static ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t size,
                                  uint64_t alignment, int type, int64_t *out) {
    if (!alignment) alignment = PAGE;
    if (!out || low < 0 || high < 0 || !size || size % PAGE || !valid_alignment(alignment) || type < 0 || type > 10)
        return INVALID;
    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE;
    if ((uint64_t)low >= end || size > end - (uint64_t)low) return NO_SPACE;
    write_lock();
    if (pool()) { write_unlock(); return NO_MEMORY; }
    uint64_t start = align_up((uint64_t)low, alignment);
    int slot = -1;
    for (int i=0; i<LIMIT; ++i) if (!blocks[i].used) { slot=i; break; }
    for (;;) {
        if (slot<0 || start > end || size > end-start) { write_unlock(); return NO_SPACE; }
        int overlap = 0;
        for (int i=0; i<LIMIT; ++i) {
            Block b = blocks[i];
            if (b.used && start < b.start+b.size && b.start < start+size) {
                start = align_up(b.start+b.size, alignment); overlap = 1; break;
            }
        }
        if (!overlap) break;
    }
    blocks[slot] = (Block){start, size, type, 1}; *out = (int64_t)start;
    ensure_chunks(start,size);
    ++allocations; live_bytes += size;
    write_unlock();
    printf("Runtime: direct allocation offset=0x%" PRIx64 ", size=%" PRIu64 " bytes, type=%d\n", start, size, type);
    return 0;
}
static int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!out || physical < 0 || (uint64_t)physical % PAGE || !size || size % PAGE || !valid_alignment(alignment) ||
        (prot & ~0x37)) return INVALID;
    if (!direct_range_allocated((uint64_t)physical,size)) return INVALID;
    int32_t result=place(out,size,prot,flags,alignment,KIND_DIRECT,block_type((uint64_t)physical),(uint64_t)physical);
    if (!result) ++maps;
    return result;
}
static ABI int32_t direct_map(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    write_lock();
    int32_t result=map_direct_locked(out,size,prot,flags,physical,alignment);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *out, size, prot);
    else printf("Runtime: mapping direct memory %#" PRIx64 ", size=%" PRIu64 " failed (error %#x)\n", (uint64_t)physical, size, (unsigned)result);
    return result;
}
static ABI int32_t direct_map_named(void **out, uint64_t size, int prot, int flags, int64_t physical,
                                    uint64_t alignment, const char *name) {
    (void)name; return direct_map(out,size,prot,flags,physical,alignment);
}
static int32_t unmap_locked(uintptr_t start, uint64_t size) {
    if (start%PAGE || !size) return INVALID;
    uint64_t end=start+align_up(size,PAGE);
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (vmas[i].kind!=KIND_RESERVED) {
            uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
            queue_hook(HOOK_UNMAP,a,b-a);
        }
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i) {
        uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
        if (munmap((void *)a,b-a)) return INVALID;
    }
    drop_range(start,end);
    trap_forget(start,end);
    gpu_forget(start,end);
    return 0;
}
static ABI int32_t direct_unmap(void *address, uint64_t size) {
    /* The GPU forgets the range before it disappears (as shadPS4's early GPU unmap). */
    if (hook_unmap && !((uintptr_t)address%PAGE) && size) hook_unmap((uintptr_t)address,align_up(size,PAGE));
    write_lock();
    size_t before=pending_count;
    int32_t result=unmap_locked((uintptr_t)address,size);
    pending_count=before; /* already notified above */
    write_unlock();
    return result;
}
static ABI int32_t direct_release(uint64_t start, uint64_t size) {
    if (start % PAGE || size % PAGE) return INVALID;
    if (!size) return 0;
    write_lock();
    if (!direct_range_allocated(start,size)) { write_unlock(); return INVALID; }
    /* Mappings of released physical pages disappear with them. */
    for (size_t i=0; i<vma_count;) {
        Vma v=vmas[i];
        uint64_t length=v.end-v.start;
        if (v.kind==KIND_DIRECT && v.phys<start+size && start<v.phys+length) {
            uint64_t a=v.phys>start ? v.phys : start, b=v.phys+length<start+size ? v.phys+length : start+size;
            unmap_locked(v.start+(a-v.phys),b-a);
            i=vma_index(v.start);
            continue;
        }
        ++i;
    }
    for (int i=0; i<LIMIT; ++i) {
        Block *b=&blocks[i];
        if (!b->used || b->start>=start+size || start>=b->start+b->size) continue;
        uint64_t a=b->start>start ? b->start : start, e=b->start+b->size<start+size ? b->start+b->size : start+size;
        Block left=*b, right=*b;
        left.size=a-b->start; right.start=e; right.size=b->start+b->size-e;
        b->used=0;
        if (left.size) *b=left;
        if (right.size) for (int j=0;j<LIMIT;++j) if (!blocks[j].used) { blocks[j]=right; break; }
        live_bytes-=e-a;
    }
    if (atomic_load_explicit(&home_pages,memory_order_relaxed)) home_release(start,size); /* no rebuild: the data goes */
    zero_phys(start,size); /* zero on reuse */
    write_unlock();
    flush_hooks();
    return 0;
}
static int32_t map_flexible_locked(void **inout, uint64_t size, int prot, int flags) {
    if (!inout || !size || size%PAGE || (prot & ~0x37)) return INVALID;
    if (flexible_bytes+size>FLEXIBLE_SIZE || pool()) return NO_MEMORY;
    uint64_t phys=flex_alloc(size);
    if (phys==UINT64_MAX) return NO_MEMORY;
    int32_t result=place(inout,size,prot,flags,PAGE,KIND_FLEXIBLE,0,phys);
    if (result) flex_free(phys,size); else ++flexible_maps;
    return result;
}
static ABI int32_t map_flexible(void **inout, uint64_t size, int prot, int flags) {
    write_lock();
    int32_t result=map_flexible_locked(inout,size,prot,flags);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped flexible memory %p, size=%" PRIu64 ", prot=0x%x\n", *inout, size, prot);
    else fprintf(stderr,"Runtime: flexible map of %" PRIu64 " bytes failed (0x%x), in use=%" PRIu64 "\n",
                 size, (unsigned)result, flexible_bytes);
    return result;
}
static ABI int32_t map_flexible_named(void **inout, uint64_t size, int prot, int flags, const char *name) {
    (void)name; return map_flexible(inout,size,prot,flags);
}
static ABI int32_t release_flexible(void *address, uint64_t size) { return direct_unmap(address,size); }
/* POSIX mmap: the game's libc grows its malloc heap with anonymous private mappings once the
 * heap it started with is full (seen after minutes of area streaming). They come from flexible
 * memory as sceKernelMapFlexibleMemory does; files are not mapped. MAP_FAILED (-1) and errno on
 * failure, as POSIX. */
static _Atomic uint64_t heap_growths;
uint64_t runtime_heap_growths(void) { return atomic_load(&heap_growths); }
static ABI void *posix_mmap(void *addr, uint64_t size, int prot, int flags, int fd, int64_t offset) {
    atomic_fetch_add(&heap_growths,1);
    enum { MAP_ANON_FLAG=0x1000, POSIX_ENOMEM=12, POSIX_ENODEV=19, POSIX_EINVAL=22 };
    if (!(flags & MAP_ANON_FLAG) || fd!=-1 || offset) { *runtime_errno()=POSIX_ENODEV; return (void *)-1; }
    if (!size) { *runtime_errno()=POSIX_EINVAL; return (void *)-1; }
    void *out=(flags & MAP_FIXED_FLAG) ? addr : NULL;
    const int32_t result=map_flexible(&out,align_up(size,PAGE),prot & 0x37,flags & MAP_FIXED_FLAG);
    uint64_t sites[3];
    runtime_guest_call_sites(sites);
    printf("Runtime: mmap %#" PRIx64 " bytes for the game (its heap grows) from +%#" PRIx64 " < +%#" PRIx64 " < +%#" PRIx64 "\n",
           size,sites[0],sites[1],sites[2]);
    if (result) { *runtime_errno()=result==NO_MEMORY ? POSIX_ENOMEM : POSIX_EINVAL; return (void *)-1; }
    return out;
}
static ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!inout || !size || size%PAGE || !valid_alignment(alignment)) return INVALID;
    write_lock();
    if ((flags & MAP_FIXED_FLAG) && overlaps((uintptr_t)*inout,(uintptr_t)*inout+size,0)) {
        write_unlock(); return NO_MEMORY;
    }
    int32_t result=place(inout,size,0,flags,alignment,KIND_RESERVED,0,0);
    write_unlock();
    if (!result) printf("Runtime: reserved virtual range %p, size=%" PRIu64 "\n", *inout, size);
    return result;
}
static int32_t protect_locked(uintptr_t start, uint64_t size, int prot, int type) {
    uintptr_t end=start+align_up(size,PAGE);
    start&=~(uintptr_t)(PAGE-1);
    if ((prot & ~0x37) || !covered(start,end,0)) return INVALID;
    /* bbport BB_TEX_HOME: homed pages (holes in the memfd) never readable, not even until trap_reapply */
    if (atomic_load_explicit(&home_pages,memory_order_relaxed) ? home_mprotect(start,end,host_prot(prot))
                                                               : mprotect((void *)start,end-start,host_prot(prot))) return INVALID;
    gpu_forget(start,end); /* the game's protection replaced the page manager's */
    trap_reapply(start,end); /* pages with write traps stay read-only */
    int error; size_t i=carve(start,end,&error);
    if (error) return NO_MEMORY;
    for (; i<vma_count && vmas[i].start<end; ++i) { vmas[i].prot=prot; if (type>=0) vmas[i].type=type; }
    queue_hook(HOOK_INVALIDATE,start,end-start);
    ++protects;
    return 0;
}
static ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot) {
    write_lock();
    int32_t result=protect_locked((uintptr_t)address,size,prot,-1);
    write_unlock();
    flush_hooks();
    return result;
}
static ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || vmas[i].start>(uintptr_t)address || vmas[i].kind==KIND_RESERVED) {
        read_unlock(); return ACCESS;
    }
    /* Report the whole contiguous run with identical protection. */
    size_t first=i, last=i;
    while (first && vmas[first-1].end==vmas[first].start && vmas[first-1].kind!=KIND_RESERVED && vmas[first-1].prot==vmas[i].prot) --first;
    while (last+1<vma_count && vmas[last+1].start==vmas[last].end && vmas[last+1].kind!=KIND_RESERVED && vmas[last+1].prot==vmas[i].prot) ++last;
    if (start) *start=(void *)vmas[first].start;
    if (end) *end=(void *)vmas[last].end;
    if (prot) *prot=(uint32_t)vmas[i].prot;
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info, uint64_t info_size) {
    if (!info || info_size!=sizeof(*info)) return INVALID;
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || (vmas[i].start>(uintptr_t)address && !(flags & 1))) { read_unlock(); return ACCESS; }
    Vma v=vmas[i];
    memset(info,0,sizeof(*info));
    info->start=v.start; info->end=v.end;
    info->offset=v.kind==KIND_DIRECT ? v.phys : 0;
    info->protection=v.prot; info->memory_type=v.type;
    info->flags=(v.kind==KIND_FLEXIBLE ? 1u : 0) | (v.kind==KIND_DIRECT ? 2u : 0) | (v.kind!=KIND_RESERVED ? 16u : 0);
    strcpy(info->name, v.kind==KIND_RESERVED ? "reserved" : v.kind==KIND_DIRECT ? "direct" : "flexible");
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {
    read_lock();
    for (int i=0;i<LIMIT;++i) if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) {
        if (type) *type=blocks[i].type;
        if (start) *start=(void *)(uintptr_t)blocks[i].start;
        if (end) *end=(void *)(uintptr_t)(blocks[i].start+blocks[i].size);
        read_unlock(); return 0;
    }
    read_unlock();
    return ACCESS;
}
static ABI int32_t batch_map2(BatchEntry *entries, int count, int *processed, int flags) {
    if (!entries || count<0) return INVALID;
    int32_t result=0; int done=0;
    write_lock();
    for (; done<count; ++done) {
        BatchEntry *e=&entries[done];
        switch (e->operation) {
        case 0: result=map_direct_locked(&e->start,e->length,(uint8_t)e->prot,flags,(int64_t)e->offset,0); break;
        case 1: result=unmap_locked((uintptr_t)e->start,e->length); break;
        case 2: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,-1); break;
        case 3: result=map_flexible_locked(&e->start,e->length,(uint8_t)e->prot,flags); break;
        case 4: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,e->type); break;
        default: result=INVALID;
        }
        if (result) break;
    }
    write_unlock();
    flush_hooks();
    if (processed) *processed=done;
    return result;
}
static ABI int32_t batch_map(BatchEntry *entries, int count, int *processed) {
    return batch_map2(entries,count,processed,MAP_FIXED_FLAG);
}
static const RuntimeExport exports[]={
    {"sceKernelGetDirectMemorySize",direct_size}, {"sceKernelAllocateDirectMemory",direct_allocate},
    {"sceKernelMapDirectMemory",direct_map}, {"sceKernelMapNamedDirectMemory",direct_map_named},
    {"sceKernelReleaseDirectMemory",direct_release}, {"sceKernelMunmap",direct_unmap}, {"munmap",direct_unmap},
    {"sceKernelMapFlexibleMemory",map_flexible}, {"sceKernelMapNamedFlexibleMemory",map_flexible_named},
    {"sceKernelReleaseFlexibleMemory",release_flexible}, {"sceKernelReserveVirtualRange",reserve_range},
    {"sceKernelMprotect",kernel_mprotect}, {"sceKernelQueryMemoryProtection",query_protection},
    {"sceKernelVirtualQuery",virtual_query}, {"sceKernelGetDirectMemoryType",direct_memory_type},
    {"sceKernelBatchMap",batch_map}, {"sceKernelBatchMap2",batch_map2},
    {"mmap",posix_mmap},
};
uintptr_t runtime_memory_resolve(const char *name) {
    /* Direct-memory NIDs are exercised by test_runtime.c before names are loaded. */
    if (!strcmp(name, "pO96TwzOm5E#p#J")) return (uintptr_t)direct_size;
    if (!strcmp(name, "rTXw65xmLIA#p#J")) return (uintptr_t)direct_allocate;
    if (!strcmp(name, "L-Q3LEjIbgA#p#J")) return (uintptr_t)direct_map;
    if (!strcmp(name, "MBuItvba6z8#p#J")) return (uintptr_t)direct_release;
    if (!strcmp(name, "cQke9UuBQOk#p#J")) return (uintptr_t)direct_unmap;
    return RUNTIME_LOOKUP(exports,name);
}
/* Host-owned memory the guest can see (image, stacks, trampolines) lives in
 * [LOW_MIN, USER_MIN): PS4 code packs pointers into 40-bit fields. */
#define LOW_MIN UINT64_C(0x0800000000)
static uintptr_t low_next=LOW_MIN;
/* bbport: what is mapped in the low region and with which protection (the image's segments, stacks,
 * trampolines), for runtime_memory_low_info: the online module reads the game's static pointers
 * there (#282: BBSC took every one for unmapped, no hunter's kit, "?GoodsName?" items). */
typedef struct { uintptr_t start, end; int prot; } LowRange;
#define MAX_LOW_RANGES 2048
static LowRange low_ranges[MAX_LOW_RANGES];
static size_t low_range_count;
/* [start, end) now has `prot` (write lock held); ranges it splits keep theirs around it. */
static void low_record(uintptr_t start, uintptr_t end, int prot) {
    static LowRange kept[MAX_LOW_RANGES]; /* under the write lock; guest threads have small stacks */
    size_t n=0;
    for (size_t i=0;i<low_range_count;++i) {
        const LowRange r=low_ranges[i];
        if (r.end<=start || r.start>=end) { if (n<MAX_LOW_RANGES) kept[n++]=r; continue; }
        if (r.start<start && n<MAX_LOW_RANGES) kept[n++]=(LowRange){r.start,start,r.prot};
        if (r.end>end && n<MAX_LOW_RANGES) kept[n++]=(LowRange){end,r.end,r.prot};
    }
    if (n<MAX_LOW_RANGES) kept[n++]=(LowRange){start,end,prot};
    /* Sorted by start (few entries: insertion sort). */
    for (size_t i=1;i<n;++i) {
        LowRange r=kept[i]; size_t j=i;
        while (j>0 && kept[j-1].start>r.start) { kept[j]=kept[j-1]; --j; }
        kept[j]=r;
    }
    /* Neighbours with the same protection join: every loader mprotect of one page left a range
     * of its own, and past MAX_LOW_RANGES the copy below ran over the variables after
     * low_ranges (Windows: trap_reasons, the write trap table, pointed into the low region). */
    size_t joined=0;
    for (size_t i=0;i<n;++i) {
        if (joined && kept[joined-1].end==kept[i].start && kept[joined-1].prot==kept[i].prot) kept[joined-1].end=kept[i].end;
        else kept[joined++]=kept[i];
    }
    n=joined;
    memcpy(low_ranges,kept,n*sizeof(*kept));
    low_range_count=n;
}
/* bbport: the loader changed the protection of part of the low region (mmap PROT_ bits). */
void runtime_low_protect(uintptr_t start, size_t size, int prot) {
    if (start<LOW_MIN || start>=USER_MIN || !size) return;
    write_lock();
    low_record(start,start+align_up(size,PAGE),prot);
    write_unlock();
}
/* bbport: the low region's mapping holding `address`: 1 with its PS4 protection bits (1 read,
 * 2 write, 4 execute) and its end, 0 when it is not mapped (gaps between segments included). */
int runtime_memory_low_info(uintptr_t address, int *prot, uintptr_t *end) {
    if (address<LOW_MIN || address>=USER_MIN) return 0;
    int found=0;
    read_lock();
    for (size_t i=0;i<low_range_count;++i) {
        const LowRange r=low_ranges[i];
        if (address>=r.start && address<r.end) {
            if (r.prot & (PROT_READ|PROT_WRITE|PROT_EXEC)) {
                *prot=((r.prot & PROT_READ) ? 1 : 0) | ((r.prot & PROT_WRITE) ? 2 : 0) | ((r.prot & PROT_EXEC) ? 4 : 0);
                *end=r.end;
                found=1;
            }
            break;
        }
    }
    read_unlock();
    return found;
}
void *runtime_low_map(size_t size, int prot) {
    size=align_up(size,PAGE);
    write_lock();
    void *p=MAP_FAILED;
    while (low_next+size<=USER_MIN) {
        p=mmap((void *)low_next,size,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        low_next+=size+PAGE; /* unmapped gap catches overruns */
        if (p!=MAP_FAILED) break;
    }
    if (p!=MAP_FAILED) low_record((uintptr_t)p,(uintptr_t)p+size,prot);
    write_unlock();
    return p==MAP_FAILED ? NULL : p;
}
/* ---- GPU library interface (gpu/shim/bbgpu.cpp) ---- */
/* Optimizations switched off at run time (diagnostics): the number in the file named by
 * BB_TOGGLE_FILE, re-read every 250 ms. Bits: 1 region cache, 2 fetch shader cache,
 * 4 page tracking early exit, 8 pending-op poll limit, 16 threaded Vulkan recording,
 * 32 texture descriptor cache, 64 lock-free upload check, 128 image lookup cache,
 * 256 buffer uploads on the recording thread, 512 barrier tracker insert memo,
 * 1024 texture binding memo, 2048 page-granular read tracking for barriers,
 * 4096 resource sharps read by the draw-preparation workers,
 * 8192 prepared draws from the draw-preparation workers,
 * 16384 small read-only buffer copies on the recording thread,
 * 32768 hot pages (opt-in with BB_HOT_PAGES=1),
 * 65536 unprotect the 256 KiB window around a guest write fault (BB_FAULT_WINDOW KiB),
 * 131072 large guest memory copies split across copy threads (BB_COPY_THREADS),
 * 262144 with BB_ASYNC_FENCES=1: wait for guest copies at fences again,
 * 524288 small guest copies batched for the copy threads instead of the recording thread,
 * 1073741824 the lock-free UpdateImage path for clean, tracked images. */
/* Bits 32 and up: the draw pipeline and related GPU thread work (gpu/shim/bbport_toggles.h). */
uint64_t runtime_disabled_optimizations;
/* bbport: a second number in BB_TOGGLE_FILE: temporary experiment bits (BbToggle::Experiment). */
uint64_t runtime_experiment_bits;
/* Speculative readers of guest memory (GPU draw-preparation workers) register a recovery
 * point: a fault on that thread jumps back to it instead of terminating (probe.c). */
__thread sigjmp_buf *runtime_fault_recover;
static void *toggle_watcher(void *path) {
    for (unsigned long long last=ULLONG_MAX;;) {
        FILE *f=fopen(path,"r");
        unsigned long long value=0, experiment=0;
        if (f) { if (fscanf(f,"%llu %llu",&value,&experiment)<1) value=0; fclose(f); }
        __atomic_store_n(&runtime_experiment_bits,(uint64_t)experiment,__ATOMIC_RELEASE);
        if (value!=last) {
            __atomic_store_n(&runtime_disabled_optimizations,(uint64_t)value,__ATOMIC_RELEASE);
            printf("Runtime: disabled optimizations mask=%llu\n",value);
            last=value;
        }
        struct timespec t={0,250000000}; nanosleep(&t,NULL);
    }
    return NULL;
}
/* bbport: the GPU library provides direct memory chunks (see CHUNK). */
void runtime_memory_set_guest_chunk_whole(int whole) { whole_chunks=whole; }
/* bbport: the host address of direct memory phys in the backing view (host memory import). */
void *runtime_memory_backing_pointer(uint64_t phys) { return backing_base ? backing_base+phys : NULL; }
void runtime_memory_set_guest_chunk_allocator(int (*alloc)(uint64_t phys, uint64_t size)) {
    write_lock();
    chunk_alloc=alloc;
    write_unlock();
}
void runtime_memory_set_gpu_hooks(GpuRange map, GpuRange unmap, GpuRange invalidate) {
    const char *toggles=getenv("BB_TOGGLE_FILE");
    static pthread_t watcher;
    if (toggles && !watcher) pthread_create(&watcher,NULL,toggle_watcher,(void *)toggles);
    write_lock();
    hook_map=map; hook_unmap=unmap; hook_invalidate=invalidate;
    for (size_t i=0;i<vma_count;++i) if (vmas[i].kind!=KIND_RESERVED) queue_hook(HOOK_MAP,vmas[i].start,vmas[i].end-vmas[i].start);
    write_unlock();
    flush_hooks();
}
/* bbport: the CPU is about to write [address, address+size) without the guest's code doing it
 * (a file read into it): the GPU side is told first (Rasterizer::InvalidateMemory), for the parts
 * the GPU can access, as a driver is told of a resource update. Its write tracking then has
 * nothing to catch there. */
void runtime_memory_set_note_write_hook(GpuRange note) { hook_note_write=note; }
void runtime_memory_note_write(uintptr_t address, uint64_t size) {
    if (atomic_load_explicit(&home_pages,memory_order_relaxed)) home_rehydrate_va(address,size); /* the texture's bytes first */
    if (!hook_invalidate || !size) return;
    uintptr_t starts[8], ends[8];
    int n=0;
    read_lock();
    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size && n<8; ++i) {
        if (vmas[i].kind==KIND_RESERVED || !(vmas[i].prot & 0x30)) continue;
        starts[n]=vmas[i].start>address ? vmas[i].start : address;
        ends[n]=vmas[i].end<address+size ? vmas[i].end : address+size;
        ++n;
    }
    read_unlock();
    GpuRange hook=hook_note_write ? hook_note_write : hook_invalidate;
    for (int i=0;i<n;++i) hook(starts[i],ends[i]-starts[i]);
}
/* bbport BB_GUEST_IN_PLACE without page protection: pages whose CPU writes the GPU side must hear
 * of (a VRAM copy of them, a texture made from them), one bit per 4 KiB page below 2^40. The GPU
 * side sets and clears them where it used to protect; the writers it cannot see coming (libc
 * imports, file reads, hooked game code) check them after writing and tell it, as a driver is
 * told of a resource update. */
#define WATCH_LIMIT (1ull<<40)
static _Atomic(uint64_t) *write_watch;
static GpuRange hook_cpu_write;
void runtime_memory_set_cpu_write_hook(GpuRange hook) {
    if (!write_watch) {
        void *bits=mmap(NULL,WATCH_LIMIT>>15,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
        if (bits==MAP_FAILED) { perror("bbport: write watch bitmap"); return; }
        write_watch=bits;
    }
    hook_cpu_write=hook;
}
void runtime_memory_set_write_watch(uintptr_t address, uint64_t size, int watch) {
    if (!write_watch || !size || address>=WATCH_LIMIT) return;
    uint64_t first=address>>12, last=(address+size-1)>>12;
    if (last>=(WATCH_LIMIT>>12)) last=(WATCH_LIMIT>>12)-1;
    for (uint64_t word=first>>6; word<=last>>6; ++word) {
        const uint64_t lo=word==first>>6 ? first&63 : 0, hi=word==last>>6 ? last&63 : 63;
        const uint64_t mask=(hi==63 ? ~0ull : (2ull<<hi)-1) & ~((1ull<<lo)-1);
        if (watch) atomic_fetch_or_explicit(&write_watch[word],mask,memory_order_release);
        else atomic_fetch_and_explicit(&write_watch[word],~mask,memory_order_release);
    }
}
/* bbport BB_FAULT_PREPARE (default on): the pages the GPU side's page manager write-protects (the
 * buffer and texture caches' watchers, copy memory model), one bit per 4 KiB page below 2^40, kept by
 * the page manager next to its mprotect calls. A bulk write the runtime does for the game (memcpy,
 * memset, memmove) asks it first (runtime_memory_prepare_cpu_write): when a page of the range is
 * protected, the GPU side does once, for the range, what each page's write fault would do, and the
 * copy runs without faults. A stale bit costs one call, a missing one the fault as before. */
static _Atomic(uint64_t) *protect_map;
static GpuRange hook_prepare_write;
void runtime_memory_set_prepare_write_hook(GpuRange hook) {
    if (!protect_map) {
        void *bits=mmap(NULL,WATCH_LIMIT>>15,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
        if (bits==MAP_FAILED) { perror("bbport: protected pages bitmap"); return; }
        protect_map=bits;
    }
    hook_prepare_write=hook;
}
void runtime_memory_set_page_protected(uintptr_t address, uint64_t size, int is_protected) {
    if (!protect_map || !size || address>=WATCH_LIMIT) return;
    uint64_t first=address>>12, last=(address+size-1)>>12;
    if (last>=(WATCH_LIMIT>>12)) last=(WATCH_LIMIT>>12)-1;
    for (uint64_t word=first>>6; word<=last>>6; ++word) {
        const uint64_t lo=word==first>>6 ? first&63 : 0, hi=word==last>>6 ? last&63 : 63;
        const uint64_t mask=(hi==63 ? ~0ull : (2ull<<hi)-1) & ~((1ull<<lo)-1);
        if (is_protected) atomic_fetch_or_explicit(&protect_map[word],mask,memory_order_release);
        else atomic_fetch_and_explicit(&protect_map[word],~mask,memory_order_release);
    }
}
static int any_page_protected(uintptr_t address, uint64_t size) {
    if (!protect_map || !size || address>=WATCH_LIMIT || size>WATCH_LIMIT-address) return 0;
    const uint64_t first=address>>12, last=(address+size-1)>>12;
    for (uint64_t word=first>>6; word<=last>>6; ++word) {
        const uint64_t lo=word==first>>6 ? first&63 : 0, hi=word==last>>6 ? last&63 : 63;
        const uint64_t mask=(hi==63 ? ~0ull : (2ull<<hi)-1) & ~((1ull<<lo)-1);
        if (atomic_load_explicit(&protect_map[word],memory_order_acquire) & mask) return 1;
    }
    return 0;
}
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size) {
    if (!write_watch || !size || address>=WATCH_LIMIT || address+size>WATCH_LIMIT) return;
    const uint64_t first=address>>12, last=(address+size-1)>>12;
    for (uint64_t word=first>>6; word<=last>>6; ++word) {
        const uint64_t lo=word==first>>6 ? first&63 : 0, hi=word==last>>6 ? last&63 : 63;
        const uint64_t mask=(hi==63 ? ~0ull : (2ull<<hi)-1) & ~((1ull<<lo)-1);
        if (atomic_load_explicit(&write_watch[word],memory_order_acquire) & mask) {
            hook_cpu_write(address,size);
            return;
        }
    }
}
/* bbport: write traps the GPU side keeps on guest pages, by reason (bits: a VRAM copy of the page,
 * an image made from it, ...). A page with any reason is read-only for the game even where its
 * own protection allows writes; the fault goes to the GPU library's handlers, and each clears
 * only its own reason. One owner for these protections: the layer's VRAM copies and the texture
 * cache trap the same pages, and one's unprotect must not drop the other's trap. Reasons from 16 up
 * trap reads too (no access: the page's data is newer elsewhere, a VRAM copy). Lock order: the
 * runtime's lock, then trap_mutex (as kernel_mprotect, which keeps the traps). */
#define TRAP_LIMIT (1ull<<40)
#define TRAP_NO_ACCESS 0xf0u
static uint8_t *trap_reasons; /* one byte per 4 KiB page below TRAP_LIMIT (reserved, not committed) */
static pthread_mutex_t trap_mutex=PTHREAD_MUTEX_INITIALIZER;
/* A page's trap level: 0 none, 1 read-only, 2 no access. */
static int trap_level(uint8_t reasons) { return !reasons ? 0 : (reasons & TRAP_NO_ACCESS) ? 2 : 1; }
/* The pages [a, b) to the game's protection, without write at level 1, without access at 2
 * (runtime lock held). */
static void trap_apply(uintptr_t a, uintptr_t b, int level) {
    for (size_t i=vma_index(a); i<vma_count && vmas[i].start<b; ++i) {
        if (vmas[i].kind==KIND_RESERVED) continue;
        const uintptr_t s=vmas[i].start>a ? vmas[i].start : a, e=vmas[i].end<b ? vmas[i].end : b;
        const int prot=host_prot(vmas[i].prot);
        mprotect((void *)s,e-s,level==2 ? PROT_NONE : level ? (prot & ~PROT_WRITE) : prot);
        gpu_forget(s,e); /* the trap level is what is in force now */
    }
}
/* No traps in [start, end) any more: a mapping replaced or gone (runtime lock held). */
static void trap_forget(uintptr_t start, uintptr_t end) {
    if (!trap_reasons || start>=TRAP_LIMIT) return;
    if (end>TRAP_LIMIT) end=TRAP_LIMIT;
    pthread_mutex_lock(&trap_mutex);
    if (atomic_load_explicit(&home_pages,memory_order_relaxed)) home_va_forget(start,end);
    memset(trap_reasons+(start>>12),0,((end+4095)>>12)-(start>>12));
    pthread_mutex_unlock(&trap_mutex);
}
/* Pages with traps in [start, end) read-only (or no access) again after the game changed their
 * protection (runtime lock held). */
static void trap_reapply(uintptr_t start, uintptr_t end) {
    if (!trap_reasons || start>=TRAP_LIMIT) return;
    if (end>TRAP_LIMIT) end=TRAP_LIMIT;
    pthread_mutex_lock(&trap_mutex);
    uintptr_t run=0; int run_level=0;
    for (uintptr_t p=start>>12; p<(end+4095)>>12; ++p) {
        const int level=trap_level(trap_reasons[p]);
        if (run && level!=run_level) { trap_apply(run,p<<12,run_level); run=0; }
        if (level && !run) { run=p<<12; run_level=level; }
    }
    if (run) trap_apply(run,end,run_level);
    pthread_mutex_unlock(&trap_mutex);
}
/* The trap table, made on first use (NULL: no memory for it). */
static uint8_t *trap_table(void) {
    if (!__atomic_load_n(&trap_reasons,__ATOMIC_ACQUIRE)) {
        void *bytes=mmap(NULL,TRAP_LIMIT>>12,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
        if (bytes==MAP_FAILED) { perror("bbport: write trap table"); return NULL; }
        uint8_t *expected=NULL;
        if (!__atomic_compare_exchange_n(&trap_reasons,&expected,(uint8_t *)bytes,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
            munmap(bytes,TRAP_LIMIT>>12);
    }
    return __atomic_load_n(&trap_reasons,__ATOMIC_ACQUIRE);
}
void runtime_memory_trap(uintptr_t address, uint64_t size, unsigned reason, int on) {
    if (!size || address>=TRAP_LIMIT) return;
    if (!trap_table()) return;
    uintptr_t end=address+size>TRAP_LIMIT ? TRAP_LIMIT : address+size;
    read_lock();
    pthread_mutex_lock(&trap_mutex);
    /* Runs of pages whose trap level changes, one mprotect each. */
    uintptr_t run=0; int run_level=0;
    const uintptr_t first=address>>12, last=(end-1)>>12;
    for (uintptr_t p=first; p<=last; ++p) {
        const uint8_t old=trap_reasons[p], now=on ? (uint8_t)(old|reason) : (uint8_t)(old & ~reason);
        if (old!=now) trap_reasons[p]=now;
        const int level=trap_level(now), change=trap_level(old)!=level;
        if (run && (!change || level!=run_level)) { trap_apply(run,p<<12,run_level); run=0; }
        if (change && !run) { run=p<<12; run_level=level; }
    }
    if (run) trap_apply(run,(last+1)<<12,run_level);
    pthread_mutex_unlock(&trap_mutex);
    read_unlock();
}
/* bbport: a libc write about to land on pages with write traps: the GPU side is told first and
 * lifts its traps, instead of the write faulting (a fault handler waits for the draws the GPU
 * thread has queued; libc copies into small textures every frame made the game's frames late).
 * Its announcement after the write (runtime_memory_note_cpu_write) stays as it was. */
int runtime_memory_home_touches(uintptr_t address, uint64_t size);
void runtime_memory_prepare_cpu_write(uintptr_t address, uint64_t size) {
    /* bbport BB_TEX_HOME: homed pages rebuilt first (the write would fault on them otherwise) */
    if (atomic_load_explicit(&home_pages,memory_order_relaxed) && runtime_memory_home_touches(address,size))
        home_rehydrate_va(address,size);
    /* The page manager's protected pages first (BB_FAULT_PREPARE): the range's write faults in one call. */
    if (hook_prepare_write && any_page_protected(address,size)) hook_prepare_write(address,size);
    const uint8_t *table=__atomic_load_n(&trap_reasons,__ATOMIC_ACQUIRE);
    if (!table || !size || address>=TRAP_LIMIT || !hook_cpu_write) return;
    uint64_t last=(address+size-1)>>12;
    if (last>=(TRAP_LIMIT>>12)) last=(TRAP_LIMIT>>12)-1;
    for (uint64_t p=address>>12; p<=last; ++p)
        if (__atomic_load_n(&table[p],__ATOMIC_RELAXED)) { hook_cpu_write(address,size); return; }
}
unsigned runtime_memory_trap_reasons(uintptr_t address) {
    const uint8_t *table=__atomic_load_n(&trap_reasons,__ATOMIC_ACQUIRE);
    return table && address<TRAP_LIMIT ? __atomic_load_n(&table[address>>12],__ATOMIC_ACQUIRE) : 0;
}
/* bbport: where direct memory at a guest address lives (BB_GUEST_IN_PLACE: the GPU uses the game's
 * memory where it is): its offset in direct memory and the end of the mapping, which is contiguous
 * there. 0 for anything else (flexible memory, reserved or unmapped addresses). */
int runtime_memory_direct_phys(uintptr_t address, uint64_t *phys, uintptr_t *end) {
    read_lock();
    const size_t i=vma_index(address);
    const int ok = i<vma_count && vmas[i].start<=address && address<vmas[i].end && vmas[i].kind==KIND_DIRECT;
    if (ok) {
        *phys=vmas[i].phys+(address-vmas[i].start);
        *end=vmas[i].end;
    }
    read_unlock();
    return ok;
}
/* bbport: protection and direct memory type of the mapping at address (type -1: not direct
 * memory); 0 when nothing is mapped there. */
int runtime_memory_vma_info(uintptr_t address, int *prot, int *type, uintptr_t *end) {
    read_lock();
    const size_t i=vma_index(address);
    const int ok = i<vma_count && vmas[i].start<=address && vmas[i].kind!=KIND_RESERVED;
    if (ok) {
        *prot=vmas[i].prot;
        *type=vmas[i].kind==KIND_DIRECT ? vmas[i].type : -1;
        *end=vmas[i].end;
    }
    read_unlock();
    return ok;
}
/* Writes through the backing view; 0 when part of the range has no backing. */
/* Guest-visible writes from the GPU side (fence labels, WriteData, downloads): every byte is
 * stored once. glibc's memcpy copies 4-7 bytes as two overlapping 4-byte stores (the same
 * 4 bytes twice for a 4-byte label): a thread preempted between them stored the label again
 * after the guest had seen it and freed the block that held it (a free-list link ending in
 * 0x00000004: guest fault 0x263b8e7). Aligned 2, 4 and 8-byte pieces are single stores. */
static void store_once(unsigned char *dst, const unsigned char *src, uint64_t n) {
    if (n > 64) { memcpy(dst,src,n); return; }
    while (n) {
        const uintptr_t at=(uintptr_t)dst;
        if (n>=8 && !(at&7)) { uint64_t v; memcpy(&v,src,8); __atomic_store_n((uint64_t *)dst,v,__ATOMIC_RELEASE); dst+=8; src+=8; n-=8; }
        else if (n>=4 && !(at&3)) { uint32_t v; memcpy(&v,src,4); __atomic_store_n((uint32_t *)dst,v,__ATOMIC_RELEASE); dst+=4; src+=4; n-=4; }
        else if (n>=2 && !(at&1)) { uint16_t v; memcpy(&v,src,2); __atomic_store_n((uint16_t *)dst,v,__ATOMIC_RELEASE); dst+=2; src+=2; n-=2; }
        else { __atomic_store_n(dst,*src,__ATOMIC_RELEASE); ++dst; ++src; --n; }
    }
}
/* bbport BB_TEX_HOME: the runtime lock (shared) taken with no homed page in [address, address+size):
 * homed ones are rebuilt first, outside the lock (their texture is re-tiled by the GPU side), and the
 * lock keeps them from being homed again while it is held. After a few rounds (a rebuild that cannot
 * be done on this thread) it goes on with what is there. */
static void read_lock_unhomed(uintptr_t address, uint64_t size) {
    for (int round=0;;++round) {
        read_lock();
        if (!atomic_load_explicit(&home_pages,memory_order_relaxed) || round==4) return;
        uint32_t tickets[8];
        if (!home_collect_va(address,size,tickets,8)) return;
        read_unlock();
        home_rehydrate_va(address,size);
    }
}
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    read_lock_unhomed(address,size);
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        store_once(backing_base+vmas[i].phys+(at-vmas[i].start),(const unsigned char *)data+(at-address),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* Reads through the backing view: the host's copies of guest memory (uploads), which the GPU's
 * read protection (BB_READBACKS=2: GPU-written pages) must not stop. A fault there would ask the
 * GPU thread for a readback while it waits for these very copies (a hang at start). Gaps read
 * as zeros. */
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size) {
    unsigned char *out=data;
    read_lock_unhomed(address,size);
    for (uintptr_t at=address, end=address+size; at<end;) {
        const size_t i=vma_index(at);
        uint64_t n;
        if (i<vma_count && vmas[i].start<=at && vmas[i].kind!=KIND_RESERVED) {
            n=(vmas[i].end<end ? vmas[i].end : end)-at;
            memcpy(out+(at-address),backing_base+vmas[i].phys+(at-vmas[i].start),n);
        } else {
            const uintptr_t next=i==vma_count ? end : vmas[i].start>at ? vmas[i].start : vmas[i].end;
            n=(next<end ? next : end)-at;
            memset(out+(at-address),0,n);
        }
        at+=n;
    }
    read_unlock();
}
/* Per-thread cache of recently found regions for the GPU's per-draw queries:
 * entries hold for as long as the table generation they were read at. */
typedef struct { uint64_t generation; uintptr_t start, end; int kind; } CachedRegion;
static __thread CachedRegion region_cache[8];
static __thread unsigned region_cache_next;
static const CachedRegion *cached_region(uintptr_t address) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (generation & 1) return NULL;
    for (unsigned i=0;i<8;++i) {
        const CachedRegion *c=&region_cache[i];
        if (c->generation==generation && c->end && c->start<=address && address<c->end) return c;
    }
    return NULL;
}
/* Called under the read lock after a lookup of address. */
static void cache_region(size_t i) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (i==vma_count || (generation & 1)) return;
    region_cache[region_cache_next++ % 8]=(CachedRegion){generation,vmas[i].start,vmas[i].end,vmas[i].kind};
}
/* The mapping table's generation (odd while it changes): callers cache regions against it. */
const uint64_t *runtime_memory_generation(void) { return &table_generation; }
/* Mapped length from address, up to size, across adjacent mappings. */
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return size;
    read_lock();
    { size_t i=vma_index(address); if (i<vma_count && vmas[i].start<=address) cache_region(i); }
    uint64_t length=0;
    for (size_t i=vma_index(address); i<vma_count && length<size; ++i) {
        if (vmas[i].start>address+length || vmas[i].kind==KIND_RESERVED) break;
        length=vmas[i].end-address;
    }
    read_unlock();
    return length<size ? length : size;
}
/* Region around address: [start,end) mapped or a gap; 0 past the last mapping. */
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c) { *start=c->start; *end=c->end; *mapped=c->kind!=KIND_RESERVED; return 1; }
    read_lock();
    size_t i=vma_index(address);
    int found=i<vma_count;
    if (found && vmas[i].start<=address) {
        cache_region(i);
        *start=vmas[i].start; *end=vmas[i].end; *mapped=vmas[i].kind!=KIND_RESERVED;
    } else if (found) {
        *start=i ? vmas[i-1].end : 0; *end=vmas[i].start; *mapped=0;
    }
    read_unlock();
    return found;
}
/* GPU page tracking: restrict host access, never beyond the guest's own protection. */
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    read_lock();
    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size; ++i) {
        if (vmas[i].kind==KIND_RESERVED) continue;
        uintptr_t a=vmas[i].start>address ? vmas[i].start : address;
        uintptr_t b=vmas[i].end<address+size ? vmas[i].end : address+size;
        int guest=host_prot(vmas[i].prot);
        int want=(read ? PROT_READ : 0)|(write ? PROT_WRITE|PROT_READ : 0)|(guest & PROT_EXEC);
        /* bbport BB_TEX_HOME: homed pages (holes) keep no access whatever the page manager wants */
        const int failed=atomic_load_explicit(&home_pages,memory_order_relaxed) ? home_mprotect(a,b,guest & want)
                                                                                 : mprotect((void *)a,b-a,guest & want);
        if (!failed && gpu_state && a<GPU_STATE_LIMIT) {
            /* bbport UMA: kept for promotion (a remapped granule gets its pages' protection back) */
            const uintptr_t e=b<GPU_STATE_LIMIT ? b : GPU_STATE_LIMIT;
            memset(gpu_state+(a>>12),4|(read ? 1 : 0)|(write ? 2 : 0),((e+4095)>>12)-(a>>12));
        }
    }
    read_unlock();
}
/* ---- bbport BB_TEX_HOME: textures homed in the GPU (docs/ANDROID_UNIFIED_MEMORY_PLAN.md, "Plan A") ----
 * A texture the game no longer touches exists twice: its tiled bytes here (the memfd) and the GPU's
 * image. The GPU library re-tiles the image, and when the bytes equal the game's it homes the pages
 * (runtime_memory_home): every guest alias of them gets the HOME_TRAP trap (no access), then their
 * memfd range is punched out. Any access gets them back first (runtime_memory_home_restore with the
 * bytes the GPU side re-tiled again): a guest access faults into the GPU library's handler (HOME_TRAP
 * among the reasons); the runtime's own users of the backing view (read/write_backing), its write
 * announcements (note_write, prepare_cpu_write) and kernel I/O (io_begin) call the home hook first,
 * outside the runtime lock. A home is a record of a run of direct memory pages (one ticket); every
 * homed 4 KiB page names its ticket. Released pages leave their record (no rebuild: the data goes
 * with them); new mappings of homed pages get the trap. Lock order: the runtime lock, home_mutex,
 * trap_mutex. Nothing here costs anything while no page is homed (home_pages 0). */
#define HOME_TRAP 64u /* trap reason (>=16: no access); BbLayer::WriteTraps::TexHome */
#define HOME_SLOTS 4096
#define HOME_MAX_ALIASES 16
#define HOME_REGION_SHIFT 21 /* home_region_pages: per 2 MiB of guest addresses */
typedef struct { uint64_t phys, size, pages; uint32_t ticket; } Home; /* pages: still homed; ticket 0: free */
static Home homes[HOME_SLOTS];
static uint32_t home_generation;
static _Atomic(uint32_t) *home_owner;          /* per 4 KiB page of direct memory: its ticket, 0 not homed */
static _Atomic(uint16_t) *home_region_pages;   /* per 2 MiB of guest addresses: pages with HOME_TRAP */
static pthread_mutex_t home_mutex=PTHREAD_MUTEX_INITIALIZER;
static int (*home_hook)(uint32_t ticket);
static RuntimeHomeStats home_stats;
#define HOME_ADD(field,n) __atomic_fetch_add(&home_stats.field,(uint64_t)(n),__ATOMIC_RELAXED)
static uint64_t host_page(void) {
    static uint64_t page;
    if (!page) { const long v=sysconf(_SC_PAGESIZE); page=v>4096 ? (uint64_t)v : 4096; }
    return page;
}
static int home_tables(void) {
    if (__atomic_load_n(&home_owner,__ATOMIC_ACQUIRE)) return 0;
    pthread_mutex_lock(&home_mutex);
    if (!home_owner) {
        void *owners=mmap(NULL,(POOL_SIZE>>12)*sizeof(uint32_t),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
        void *regions=mmap(NULL,(TRAP_LIMIT>>HOME_REGION_SHIFT)*sizeof(uint16_t),PROT_READ|PROT_WRITE,
                           MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
        if (owners==MAP_FAILED || regions==MAP_FAILED) {
            if (owners!=MAP_FAILED) munmap(owners,(POOL_SIZE>>12)*sizeof(uint32_t));
            if (regions!=MAP_FAILED) munmap(regions,(TRAP_LIMIT>>HOME_REGION_SHIFT)*sizeof(uint16_t));
            pthread_mutex_unlock(&home_mutex);
            return -1;
        }
        home_region_pages=regions;
        __atomic_store_n(&home_owner,(_Atomic(uint32_t) *)owners,__ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&home_mutex);
    return 0;
}
static uint32_t home_owner_at(uint64_t phys) {
    _Atomic(uint32_t) *owners=__atomic_load_n(&home_owner,__ATOMIC_ACQUIRE);
    return owners && phys<POOL_SIZE ? atomic_load_explicit(&owners[phys>>12],memory_order_acquire) : 0;
}
static Home *home_find(uint32_t ticket) { /* home_mutex held */
    Home *h=&homes[ticket%HOME_SLOTS];
    return ticket && h->ticket==ticket ? h : NULL;
}
static int home_any_phys(uint64_t phys, uint64_t size) {
    for (uint64_t at=phys&~UINT64_C(4095);at<phys+size && at<POOL_SIZE;at+=4096)
        if (home_owner_at(at)) return 1;
    return 0;
}
/* HOME_TRAP bits of [start, end) about to be cleared with the rest of their reasons (trap_mutex held). */
static void home_va_forget(uintptr_t start, uintptr_t end) {
    if (!trap_reasons || !home_region_pages) return;
    for (uintptr_t p=start>>12;p<(end+4095)>>12;++p)
        if (trap_reasons[p] & HOME_TRAP) atomic_fetch_sub_explicit(&home_region_pages[p>>(HOME_REGION_SHIFT-12)],1,memory_order_relaxed);
}
/* HOME_TRAP set or cleared on the guest pages [a, b) (runtime lock held, both tables made). Each page
 * gets the protection its reasons want; one whose last reason goes stays read-only where the GPU page
 * manager write-protects it (BB_FAULT_PREPARE's map of its protections), so the texture's own write
 * tracking survives the rebuild (a plain trap lift gives the game's protection back). */
static void home_trap(uintptr_t a, uintptr_t b, int on) {
    if (a>=TRAP_LIMIT) return;
    if (b>TRAP_LIMIT) b=TRAP_LIMIT;
    uint8_t *t=trap_reasons;
    pthread_mutex_lock(&trap_mutex);
    for (uintptr_t p=a>>12;p<(b+4095)>>12;++p) {
        const uint8_t old=t[p], now=on ? (uint8_t)(old|HOME_TRAP) : (uint8_t)(old & ~HOME_TRAP);
        if (old==now) continue;
        __atomic_store_n(&t[p],now,__ATOMIC_RELEASE);
        if (on) atomic_fetch_add_explicit(&home_region_pages[p>>(HOME_REGION_SHIFT-12)],1,memory_order_relaxed);
        else atomic_fetch_sub_explicit(&home_region_pages[p>>(HOME_REGION_SHIFT-12)],1,memory_order_relaxed);
    }
    for (size_t i=vma_index(a); i<vma_count && vmas[i].start<b; ++i) {
        if (vmas[i].kind==KIND_RESERVED) continue;
        const uintptr_t s=vmas[i].start>a ? vmas[i].start : a, e=vmas[i].end<b ? vmas[i].end : b;
        const int prot=host_prot(vmas[i].prot);
        uintptr_t run=s; int run_prot=-1;
        for (uintptr_t p=s;p<e;p+=4096) {
            const int level=trap_level(t[p>>12]);
            const int guarded=!level && any_page_protected(p,4096);
            const int want=level==2 ? PROT_NONE : (level || guarded) ? (prot & ~PROT_WRITE) : prot;
            if (gpu_state && p<GPU_STATE_LIMIT) gpu_state[p>>12]=guarded ? (uint8_t)(4|1) : 0; /* the restriction in force */
            if (want==run_prot) continue;
            if (run_prot>=0) mprotect((void *)run,p-run,run_prot);
            run=p; run_prot=want;
        }
        if (run_prot>=0) mprotect((void *)run,e-run,run_prot);
    }
    pthread_mutex_unlock(&trap_mutex);
}
/* home_trap on every guest alias of direct memory [phys, phys+size) (runtime lock held). */
static void home_trap_phys(uint64_t phys, uint64_t size, int on) {
    for (size_t i=0;i<vma_count;++i) {
        const Vma *v=&vmas[i];
        if (v->kind!=KIND_DIRECT) continue;
        const uint64_t length=v->end-v->start, lo=v->phys>phys ? v->phys : phys,
                       hi=v->phys+length<phys+size ? v->phys+length : phys+size;
        if (lo<hi) home_trap(v->start+(lo-v->phys),v->start+(hi-v->phys),on);
    }
}
/* mprotect of [start, end) to prot, homed pages left as they are (no access) (runtime lock held). */
static int home_mprotect(uintptr_t start, uintptr_t end, int prot) {
    const uint8_t *t=__atomic_load_n(&trap_reasons,__ATOMIC_ACQUIRE);
    if (!t || start>=TRAP_LIMIT) return mprotect((void *)start,end-start,prot);
    int failed=0;
    uintptr_t run=0;
    for (uintptr_t p=start;p<end;p+=4096) {
        const int homed=p<TRAP_LIMIT && (__atomic_load_n(&t[p>>12],__ATOMIC_ACQUIRE) & HOME_TRAP);
        if (!homed && !run) run=p;
        if (homed && run) { failed|=mprotect((void *)run,p-run,prot); run=0; }
    }
    if (run) failed|=mprotect((void *)run,end-run,prot);
    return failed ? -1 : 0;
}
/* A new mapping of direct memory phys at address: its homed pages get the trap (runtime lock held). */
static void home_map(uintptr_t address, uint64_t size, uint64_t phys) {
    if (!home_owner || !trap_reasons) return;
    for (uint64_t at=0;at<size;) {
        if (!home_owner_at(phys+at)) { at+=4096; continue; }
        uint64_t end=at;
        while (end<size && home_owner_at(phys+end)) end+=4096;
        home_trap(address+at,address+end,1);
        at=end;
    }
}
/* Direct memory [phys, phys+size) released (runtime lock held exclusively, its mappings gone). */
static void home_release(uint64_t phys, uint64_t size) {
    if (!home_owner) return;
    pthread_mutex_lock(&home_mutex);
    uint64_t released=0;
    for (uint64_t at=phys;at<phys+size && at<POOL_SIZE;at+=4096) {
        const uint32_t ticket=home_owner_at(at);
        if (!ticket) continue;
        atomic_store_explicit(&home_owner[at>>12],0,memory_order_release);
        Home *h=home_find(ticket);
        if (h && !--h->pages) h->ticket=0;
        ++released;
    }
    pthread_mutex_unlock(&home_mutex);
    if (released) { atomic_fetch_sub(&home_pages,released); HOME_ADD(released_pages,released); }
}
/* The tickets of homed pages behind guest [address, address+size), at most max (runtime lock held). */
static int home_collect_va(uintptr_t address, uint64_t size, uint32_t *out, int max) {
    int n=0;
    if (!home_owner || !size) return 0;
    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size && n<max; ++i) {
        if (vmas[i].kind!=KIND_DIRECT) continue;
        const uintptr_t s=vmas[i].start>address ? vmas[i].start : address, e=vmas[i].end<address+size ? vmas[i].end : address+size;
        for (uintptr_t p=s&~(uintptr_t)4095;p<e && n<max;p+=4096) {
            const uint32_t ticket=home_owner_at(vmas[i].phys+(p-vmas[i].start));
            if (!ticket) continue;
            int seen=0;
            for (int k=0;k<n && !seen;++k) seen=out[k]==ticket;
            if (!seen) out[n++]=ticket;
        }
    }
    return n;
}
/* Homed pages behind guest [address, address+size) rebuilt through the home hook (no lock held). */
static void home_rehydrate_va(uintptr_t address, uint64_t size) {
    if (!home_hook || !size) return;
    for (int round=0;round<8;++round) {
        uint32_t tickets[16];
        read_lock();
        const int n=home_collect_va(address,size,tickets,16);
        read_unlock();
        if (!n) return;
        int progress=0;
        for (int k=0;k<n;++k) { HOME_ADD(hook_calls,1); progress|=home_hook(tickets[k]); }
        if (!progress) return; /* this thread cannot have them rebuilt (logged by the GPU side) */
    }
}
uint64_t runtime_memory_home_granule(void) { return host_page(); }
uint64_t runtime_memory_homed_pages(void) { return atomic_load_explicit(&home_pages,memory_order_relaxed); }
void runtime_memory_set_home_hook(int (*rehydrate)(uint32_t ticket)) { home_hook=rehydrate; }
int runtime_memory_home(uintptr_t address, uint64_t size, const void *expected, uint32_t *ticket) {
    const uint64_t hp=host_page();
    if (!size || address%hp || size%hp || !expected || !ticket || address>=TRAP_LIMIT || size>TRAP_LIMIT-address)
        return RUNTIME_HOME_ARGS;
    if (!trap_table() || home_tables()) return RUNTIME_HOME_ARGS;
    write_lock();
    int rc=RUNTIME_HOME_OK;
    uint64_t phys=0;
    const size_t i=vma_index(address);
    if (!backing_base || i==vma_count || vmas[i].start>address || vmas[i].end<address+size || vmas[i].kind!=KIND_DIRECT)
        rc=RUNTIME_HOME_MAPPING;
    else {
        phys=vmas[i].phys+(address-vmas[i].start);
        if (phys>=POOL_SIZE || size>POOL_SIZE-phys) rc=RUNTIME_HOME_MAPPING;
    }
    if (!rc && chunk_alloc) rc=RUNTIME_HOME_SHARED; /* GPU-visible chunks (v1, BB_GUEST_GPU_MEMORY) */
    if (!rc && granule_fd)
        for (uint64_t g=phys/granule_size;g<=(phys+size-1)/granule_size;++g)
            if (uma_fd(g)>=0) { rc=RUNTIME_HOME_SHARED; break; }
    if (!rc && home_any_phys(phys,size)) rc=RUNTIME_HOME_HOMED;
    if (!rc) {
        int aliases=0;
        for (size_t k=0;k<vma_count;++k)
            if (vmas[k].kind==KIND_DIRECT && vmas[k].phys<phys+size && phys<vmas[k].phys+(vmas[k].end-vmas[k].start)) ++aliases;
        if (aliases>HOME_MAX_ALIASES) rc=RUNTIME_HOME_ALIASES;
    }
    size_t slot=HOME_SLOTS;
    if (!rc) {
        pthread_mutex_lock(&home_mutex);
        for (size_t k=0;k<HOME_SLOTS;++k) if (!homes[k].ticket) { slot=k; break; }
        pthread_mutex_unlock(&home_mutex);
        if (slot==HOME_SLOTS) rc=RUNTIME_HOME_FULL;
    }
    if (!rc) {
        /* No access through any alias from here (mprotect returns once every CPU sees it); the bytes are
         * then the game's last ones, compared through the backing view, which only the runtime uses
         * (its writers wait for this lock). */
        home_trap_phys(phys,size,1);
        if (memcmp(backing_base+phys,expected,size)) {
            home_trap_phys(phys,size,0);
            rc=RUNTIME_HOME_MISMATCH;
        } else {
            pthread_mutex_lock(&home_mutex);
            uint32_t generation=++home_generation % (UINT32_MAX/HOME_SLOTS);
            if (!generation) generation=++home_generation % (UINT32_MAX/HOME_SLOTS);
            const uint32_t t=generation*HOME_SLOTS+(uint32_t)slot;
            homes[slot]=(Home){phys,size,size>>12,t};
            for (uint64_t at=phys;at<phys+size;at+=4096) atomic_store_explicit(&home_owner[at>>12],t,memory_order_release);
            pthread_mutex_unlock(&home_mutex);
            atomic_fetch_add(&home_pages,size>>12);
            fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size); /* the RAM goes */
            *ticket=t;
            HOME_ADD(homes,1);
        }
    }
    write_unlock();
    if (rc>0 && rc<RUNTIME_HOME_CODES) HOME_ADD(refused[rc],1);
    return rc;
}
int runtime_memory_home_restore(uint32_t ticket, const void *data, uint64_t size) {
    if (!ticket || !__atomic_load_n(&home_owner,__ATOMIC_ACQUIRE)) return -1;
    read_lock();
    pthread_mutex_lock(&home_mutex);
    Home *h=home_find(ticket);
    if (!h || (data && size!=h->size)) {
        pthread_mutex_unlock(&home_mutex);
        read_unlock();
        return h ? -2 : -1;
    }
    uint64_t restored=0;
    for (uint64_t at=h->phys, end=h->phys+h->size; at<end;) {
        if (home_owner_at(at)!=ticket) { at+=4096; continue; } /* released meanwhile */
        uint64_t run_end=at;
        while (run_end<end && home_owner_at(run_end)==ticket) run_end+=4096;
        /* The bytes first, through the backing view (not trapped), then the pages are the game's again. */
        if (data) memcpy(backing_base+at,(const unsigned char *)data+(at-h->phys),run_end-at);
        for (uint64_t p=at;p<run_end;p+=4096) atomic_store_explicit(&home_owner[p>>12],0,memory_order_release);
        home_trap_phys(at,run_end-at,0);
        restored+=(run_end-at)>>12;
        at=run_end;
    }
    h->ticket=0;
    pthread_mutex_unlock(&home_mutex);
    read_unlock();
    atomic_fetch_sub(&home_pages,restored);
    HOME_ADD(restores,1);
    if (data) HOME_ADD(restored_pages,restored); else HOME_ADD(forgotten_pages,restored);
    return (int)restored;
}
uint64_t runtime_memory_home_alive(uint32_t ticket) {
    pthread_mutex_lock(&home_mutex);
    const Home *h=home_find(ticket);
    const uint64_t pages=h ? h->pages : 0;
    pthread_mutex_unlock(&home_mutex);
    return pages;
}
uint32_t runtime_memory_home_ticket(uintptr_t address) {
    if (!__atomic_load_n(&home_owner,__ATOMIC_ACQUIRE)) return 0;
    read_lock();
    const size_t i=vma_index(address);
    const uint32_t ticket = i<vma_count && vmas[i].start<=address && vmas[i].kind==KIND_DIRECT
        ? home_owner_at(vmas[i].phys+(address-vmas[i].start)) : 0;
    read_unlock();
    return ticket;
}
int runtime_memory_home_tickets(uintptr_t address, uint64_t size, uint32_t *out, int max) {
    if (!atomic_load_explicit(&home_pages,memory_order_relaxed)) return 0;
    read_lock();
    const int n=home_collect_va(address,size,out,max);
    read_unlock();
    return n;
}
int runtime_memory_home_unstale(uintptr_t address) {
    if (!__atomic_load_n(&trap_reasons,__ATOMIC_ACQUIRE) || address>=TRAP_LIMIT) return 1;
    const uintptr_t page=address&~(uintptr_t)4095;
    read_lock();
    const size_t i=vma_index(address);
    const int owned = i<vma_count && vmas[i].start<=address && vmas[i].kind==KIND_DIRECT &&
                      home_owner_at(vmas[i].phys+(page-vmas[i].start));
    if (!owned && (trap_reasons[page>>12] & HOME_TRAP) && home_region_pages) home_trap(page,page+4096,0);
    read_unlock();
    return !owned;
}
int runtime_memory_home_touches(uintptr_t address, uint64_t size) {
    if (!atomic_load_explicit(&home_pages,memory_order_relaxed) || !size || address>=TRAP_LIMIT) return 0;
    const uint8_t *t=__atomic_load_n(&trap_reasons,__ATOMIC_ACQUIRE);
    _Atomic(uint16_t) *regions=home_region_pages;
    if (!t || !regions) return 0;
    const uintptr_t end=size<TRAP_LIMIT-address ? address+size : TRAP_LIMIT;
    for (uintptr_t r=address>>HOME_REGION_SHIFT;r<=(end-1)>>HOME_REGION_SHIFT;++r) {
        if (!atomic_load_explicit(&regions[r],memory_order_relaxed)) continue;
        const uintptr_t lo=r<<HOME_REGION_SHIFT>address ? r<<HOME_REGION_SHIFT : address;
        const uintptr_t hi=(r+1)<<HOME_REGION_SHIFT<end ? (r+1)<<HOME_REGION_SHIFT : end;
        for (uintptr_t p=lo>>12;p<=(hi-1)>>12;++p)
            if (__atomic_load_n(&t[p],__ATOMIC_ACQUIRE) & HOME_TRAP) return 1;
    }
    return 0;
}
void runtime_memory_home_stats(RuntimeHomeStats *out) {
    const uint64_t *from=(const uint64_t *)&home_stats;
    uint64_t *to=(uint64_t *)out;
    for (size_t i=0;i<sizeof(home_stats)/sizeof(uint64_t);++i) to[i]=__atomic_load_n(&from[i],__ATOMIC_RELAXED);
    out->homed_pages=atomic_load_explicit(&home_pages,memory_order_relaxed);
}
/* ---- bbport: UMA granules, promotion CPU -> SHARED (docs/ANDROID_UNIFIED_MEMORY_PLAN.md) ----
 * The GPU library asks for a granule to be shared (runtime_memory_promote) when it wants to bind it as
 * a buffer in place. The game's pages must change from the memfd to the dma-buf without losing a
 * write or a moment of access it should have: the writers are frozen (every guest alias of the
 * granule read-only, a fault on it waits for the end), the data is copied, the dma-buf is mapped over
 * the backing view and over each alias, the protections the pages had are given back, the writers go on.
 * Lock order: promote_mutex, the runtime lock (exclusive), then io_mutex or trap_mutex; the callbacks
 * into the GPU library (alloc, release) are never called with the runtime lock held. */
#define UMA_MAX_ALIASES 16
#define UMA_IO_SLOTS 64
#define UMA_FROZEN_SLOTS 128
#define UMA_IO_WAIT_NS (50ull*1000*1000)
#define UMA_RETRY_NS (250ull*1000*1000)
typedef struct { uintptr_t va; uint64_t offset, length; int prot; } Alias; /* offset in the granule */
static pthread_mutex_t promote_mutex=PTHREAD_MUTEX_INITIALIZER;
static RuntimeUmaStats uma;
#define UMA_ADD(field,n) __atomic_fetch_add(&uma.field,(uint64_t)(n),__ATOMIC_RELAXED)
_Static_assert(sizeof(RuntimeUmaStats)==11*sizeof(uint64_t),"RuntimeUmaStats is eleven counters");

/* Kernel writes into guest memory in flight (a file read into a buffer): a frozen page would give the
 * kernel EFAULT, or a short read, instead of a fault our handler waits out. io_mutex covers the slots,
 * and the check of the frozen ranges in io_begin against the check of the slots in freeze_publish, so
 * one of the two always sees the other. */
static pthread_mutex_t io_mutex=PTHREAD_MUTEX_INITIALIZER;
static struct { uintptr_t start, end; } io_ranges[UMA_IO_SLOTS]; /* end 0: free */

/* The pages frozen by promotions, readable without a lock (the fault handler): a ring of ranges. A
 * range stays after the thaw (done = when): a thread whose write faulted may only reach the handler
 * after the freeze ended (preempted in between), and would otherwise take that for a real fault. The
 * handler lets it retry once, then the fault is anyone's again. freeze_seq is odd while pages are
 * frozen; the range's gen is the odd value of its freeze. */
typedef struct { _Atomic uintptr_t start, end; _Atomic uint64_t done; _Atomic uint32_t gen; } Frozen;
static Frozen frozen[UMA_FROZEN_SLOTS];
static unsigned frozen_next; /* runtime lock */
static _Atomic uint32_t freeze_seq;
static _Atomic uint64_t thaw_ns; /* when the last freeze ended; 0: none yet */
static __thread int freezing; /* this thread holds a freeze: its own faults cannot wait */
static __thread uintptr_t retry_page;
static __thread uint32_t retry_gen;

static void futex_wait(uint32_t value) {
#ifdef SYS_futex
    const struct timespec t={0,2000000};
    syscall(SYS_futex,&freeze_seq,128 /* FUTEX_WAIT_PRIVATE */,value,&t,NULL,0);
#else
    sched_yield(); (void)value;
#endif
}
static void futex_wake(void) {
#ifdef SYS_futex
    syscall(SYS_futex,&freeze_seq,129 /* FUTEX_WAKE_PRIVATE */,INT_MAX,NULL,NULL,0);
#endif
}
static int frozen_overlaps(uintptr_t start, uintptr_t end) { /* io_mutex held */
    for (int i=0;i<UMA_FROZEN_SLOTS;++i) {
        const uintptr_t e=atomic_load_explicit(&frozen[i].end,memory_order_acquire);
        if (e && !atomic_load_explicit(&frozen[i].done,memory_order_relaxed) &&
            start<e && atomic_load_explicit(&frozen[i].start,memory_order_relaxed)<end) return 1;
    }
    return 0;
}
/* Publishes the aliases as frozen (runtime lock held); 0 when a kernel write into one is in flight. */
static int freeze_publish(const Alias *a, int n) {
    pthread_mutex_lock(&io_mutex);
    for (int i=0;i<n;++i)
        for (int s=0;s<UMA_IO_SLOTS;++s)
            if (io_ranges[s].end && a[i].va<io_ranges[s].end && io_ranges[s].start<a[i].va+a[i].length) {
                pthread_mutex_unlock(&io_mutex);
                return 0;
            }
    const uint32_t seq=atomic_load_explicit(&freeze_seq,memory_order_relaxed)+1; /* odd */
    for (int i=0;i<n;++i) {
        Frozen *f=&frozen[frozen_next++ % UMA_FROZEN_SLOTS];
        atomic_store_explicit(&f->end,0,memory_order_relaxed); /* not a match while it is rewritten */
        atomic_store_explicit(&f->start,a[i].va,memory_order_relaxed);
        atomic_store_explicit(&f->gen,seq,memory_order_relaxed);
        atomic_store_explicit(&f->done,0,memory_order_relaxed);
        atomic_store_explicit(&f->end,a[i].va+a[i].length,memory_order_release);
    }
    atomic_store_explicit(&freeze_seq,seq,memory_order_release);
    pthread_mutex_unlock(&io_mutex);
    return 1;
}
/* The freeze ends: its ranges stay in the ring as history, the waiters go on (runtime lock held). */
static void freeze_thaw(void) {
    const uint64_t now=runtime_wait_clock();
    for (int i=0;i<UMA_FROZEN_SLOTS;++i)
        if (atomic_load_explicit(&frozen[i].end,memory_order_relaxed) && !atomic_load_explicit(&frozen[i].done,memory_order_relaxed))
            atomic_store_explicit(&frozen[i].done,now,memory_order_release);
    atomic_store_explicit(&thaw_ns,now,memory_order_release);
    atomic_store_explicit(&freeze_seq,atomic_load_explicit(&freeze_seq,memory_order_relaxed)+1,memory_order_release);
    futex_wake();
}
/* bbport: called from the fault chain (probe.c) after the guest CPU's own handler: a fault on a page a
 * promotion froze waits for the end and the write is retried (1). Async-signal-safe: atomics, a futex,
 * the clock. Everything else is 0 and costs one load until the first promotion. */
int runtime_memory_promotion_fault(void *address) {
    uint32_t seq=atomic_load_explicit(&freeze_seq,memory_order_acquire);
    const uint64_t thawed=atomic_load_explicit(&thaw_ns,memory_order_acquire);
    if (!(seq & 1) && !thawed) return 0;
    if (freezing) return 0;
    const int saved_errno=errno;
    const uint64_t now=runtime_wait_clock();
    int result=0;
    if ((seq & 1) || thawed>=now || now-thawed<=UMA_RETRY_NS) {
        const uintptr_t at=(uintptr_t)address;
        for (int i=0;i<UMA_FROZEN_SLOTS && !result;++i) {
            Frozen *f=&frozen[i];
            const uintptr_t end=atomic_load_explicit(&f->end,memory_order_acquire);
            if (!end || at>=end || at<atomic_load_explicit(&f->start,memory_order_relaxed)) continue;
            const uint64_t done=atomic_load_explicit(&f->done,memory_order_acquire);
            if (done && done<now && now-done>UMA_RETRY_NS) continue;
            const uint32_t gen=atomic_load_explicit(&f->gen,memory_order_relaxed);
            if (retry_gen==gen && retry_page==(at>>12)) continue; /* retried once already: this fault is another's */
            retry_gen=gen; retry_page=at>>12;
            if (!done) { /* the freeze is still on: this write waits for it (a late arrival only retries) */
                __atomic_fetch_add(&uma.freeze_faults,1,__ATOMIC_RELAXED);
                while (!atomic_load_explicit(&f->done,memory_order_acquire)) {
                    seq=atomic_load_explicit(&freeze_seq,memory_order_acquire);
                    if (!(seq & 1)) break;
                    futex_wait(seq);
                }
            }
            result=1;
        }
    }
    errno=saved_errno;
    return result;
}
/* bbport: the kernel is about to write [address, address+size) of guest memory for a syscall (a file
 * read): no promotion freezes those pages until runtime_memory_io_end(token). Waits while a freeze
 * covers the range. Off (NULL, no cost) without UMA; NULL as well when every slot is taken (the
 * read is then unguarded, the old behaviour). */
void *runtime_memory_io_begin(uintptr_t address, uint64_t size) {
    /* bbport BB_TEX_HOME: the kernel cannot fault homed pages in: rebuilt first */
    if (atomic_load_explicit(&home_pages,memory_order_relaxed) && size) home_rehydrate_va(address,size);
    if (!__atomic_load_n(&granule_fd,__ATOMIC_ACQUIRE) || !size) return NULL;
    pthread_mutex_lock(&io_mutex);
    for (;;) {
        const uint32_t seq=atomic_load_explicit(&freeze_seq,memory_order_acquire);
        if (!(seq & 1) || !frozen_overlaps(address,address+size)) break;
        pthread_mutex_unlock(&io_mutex);
        futex_wait(seq);
        pthread_mutex_lock(&io_mutex);
    }
    for (int s=0;s<UMA_IO_SLOTS;++s)
        if (!io_ranges[s].end) {
            io_ranges[s].start=address; io_ranges[s].end=address+size;
            pthread_mutex_unlock(&io_mutex);
            return (void *)(uintptr_t)(s+1);
        }
    pthread_mutex_unlock(&io_mutex);
    return NULL;
}
void runtime_memory_io_end(void *token) {
    if (!token) return;
    pthread_mutex_lock(&io_mutex);
    io_ranges[(uintptr_t)token-1].start=io_ranges[(uintptr_t)token-1].end=0;
    pthread_mutex_unlock(&io_mutex);
}

/* The host protection a page of a mapping with guest protection `guest` has to have: the guest's, the
 * GPU page manager's recorded restriction or the write traps' level (whichever was applied last:
 * gpu_forget keeps only one of them) -- what runtime_memory_gpu_protect and trap_apply made of it. */
static int page_prot(int guest, uintptr_t page) {
    const int prot=host_prot(guest);
    const uint8_t g=gpu_state && page<(GPU_STATE_LIMIT>>12) ? gpu_state[page] : 0;
    if (g) return prot & ((g&1 ? PROT_READ : 0)|(g&2 ? PROT_WRITE|PROT_READ : 0)|(prot & PROT_EXEC));
    const uint8_t *traps=__atomic_load_n(&trap_reasons,__ATOMIC_ACQUIRE);
    if (traps && page<(TRAP_LIMIT>>12)) {
        const int level=trap_level(traps[page]);
        if (level==2) return PROT_NONE;
        if (level) return prot & ~PROT_WRITE;
    }
    return prot;
}
/* The pages of a guest alias to the protection they have to have (freeze: without write), one
 * mprotect per run of pages that agree (runtime lock held, so no trap or GPU protection changes). */
static void alias_protect(const Alias *a, int freeze) {
    const uintptr_t end=a->va+a->length;
    uintptr_t run=a->va; int run_prot=-1;
    for (uintptr_t p=a->va;p<end;p+=4096) {
        int prot=page_prot(a->prot,p>>12);
        if (freeze) prot&=~PROT_WRITE;
        if (prot==run_prot) continue;
        if (run_prot>=0) mprotect((void *)run,p-run,run_prot);
        run=p; run_prot=prot;
    }
    if (run_prot>=0) mprotect((void *)run,end-run,run_prot);
}
/* The direct mappings of granule [gs,ge) (runtime lock held): *refusal 1 if one is executable (code in
 * a dma-buf mapping is not what the GPU wants shared), 2 if there are too many to freeze. */
static int collect_aliases(uint64_t gs, uint64_t ge, Alias *out, int *refusal) {
    int n=0;
    *refusal=0;
    for (size_t i=0;i<vma_count;++i) {
        const Vma *v=&vmas[i];
        if (v->kind!=KIND_DIRECT) continue;
        const uint64_t length=v->end-v->start, lo=v->phys>gs ? v->phys : gs, hi=v->phys+length<ge ? v->phys+length : ge;
        if (lo>=hi) continue;
        if (v->prot & 4) { *refusal=1; return n; }
        if (n==UMA_MAX_ALIASES) { *refusal=2; return n; }
        out[n++]=(Alias){v->start+(lo-v->phys),lo-gs,hi-lo,v->prot};
    }
    return n;
}
/* DMA_BUF_IOCTL_SYNC around the CPU's copy into a dma-buf (flags: write; end): the caches are cleaned
 * for the device when the kernel's heap needs it. A no-op for coherent memory or what is no dma-buf. */
static void dmabuf_sync_flags(int fd, uint64_t flags) { /* 1 READ, 2 WRITE, 4 END (0: START) */
    const int saved_errno=errno;
    ioctl(fd,_IOW('b',0,uint64_t) /* DMA_BUF_IOCTL_SYNC */,&flags);
    errno=saved_errno;
}
static void dmabuf_sync(int fd, int end) {
    const uint64_t flags=2 /* DMA_BUF_SYNC_WRITE */ | (end ? 4 : 0) /* DMA_BUF_SYNC_END */;
    const int saved_errno=errno;
    ioctl(fd,_IOW('b',0,uint64_t) /* DMA_BUF_IOCTL_SYNC */,&flags);
    errno=saved_errno;
}
/* The granule's bytes from the memfd into dst. Holes (never written, or released) read as zeros
 * without committing them in the memfd, and are written as zeros: a dma-buf is not always cleared. */
static void copy_granule(unsigned char *dst, uint64_t gs, uint64_t len) {
    const uint64_t ge=gs+len;
    for (uint64_t at=gs;at<ge;) {
        const off_t data=lseek(pool_fd,(off_t)at,SEEK_DATA);
        if (data<0 && errno!=ENXIO) { memcpy(dst+(at-gs),backing_base+at,ge-at); return; }
        if (data<0 || (uint64_t)data>=ge) { memset(dst+(at-gs),0,ge-at); return; }
        if ((uint64_t)data>at) memset(dst+(at-gs),0,(uint64_t)data-at);
        const off_t hole=lseek(pool_fd,data,SEEK_HOLE);
        const uint64_t end=hole<0 || (uint64_t)hole>ge ? ge : (uint64_t)hole;
        memcpy(dst+((uint64_t)data-gs),backing_base+data,end-(uint64_t)data);
        at=end;
    }
}
static void hand_back(uint64_t gs, uint64_t len, int fd) {
    close(fd);
    if (granule_release) granule_release(gs,len);
}
/* Backing view and aliases of granule [gs,gs+len) from the memfd again (a promotion that failed in the
 * middle): the bytes are the same, nobody wrote. A failed MAP_FIXED may have dropped what it replaced,
 * so the piece that failed is mapped again as well. */
static void views_to_memfd(uint64_t gs, uint64_t len, const Alias *a, int count) {
    int ok=mmap(backing_base+gs,len,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)gs)!=MAP_FAILED;
    for (int i=0;i<count && ok;++i)
        ok=mmap((void *)a[i].va,a[i].length,PROT_NONE,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)(gs+a[i].offset))!=MAP_FAILED;
    if (!ok) { fprintf(stderr,"STOP: UMA promotion could not restore granule %#" PRIx64 ": %s\n",gs,strerror(errno)); exit(22); }
}
static int promote_granule(uint64_t g) {
    const uint64_t gs=g*granule_size, len=uma_end(g)-gs;
    if (uma_fd(g)>=0) return 1;
    Alias a[UMA_MAX_ALIASES];
    int refusal=0, n=0;
    /* The refusals first, from a snapshot: no granule is allocated to be given back at once. */
    read_lock();
    const int pooled=backing_base!=NULL; /* no direct memory yet: nothing to promote */
    collect_aliases(gs,gs+len,a,&refusal);
    read_unlock();
    if (!pooled) return 0;
    if (refusal) { if (refusal==1) UMA_ADD(refused_exec,1); else UMA_ADD(refused_busy,1); return 0; }
    const int fd=granule_alloc(gs,len);
    if (fd<0) { UMA_ADD(promotion_failures,1); return 0; }
    /* populated now: the writers are not frozen while its pages are faulted in */
    unsigned char *scratch=mmap(NULL,len,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_POPULATE,fd,0);
    if (scratch==MAP_FAILED) { hand_back(gs,len,fd); UMA_ADD(promotion_failures,1); return 0; }
    /* The runtime lock, and the pages frozen, unless a kernel write is landing in them: that one is
     * waited for (without the lock: it needs none), as long as UMA_IO_WAIT_NS. */
    const uint64_t deadline=runtime_wait_clock()+UMA_IO_WAIT_NS;
    int waited=0, locked=0;
    for (;;) {
        write_lock();
        if (uma_fd(g)>=0) { write_unlock(); munmap(scratch,len); hand_back(gs,len,fd); return 1; }
        /* bbport BB_TEX_HOME: homed pages (their data is in a texture only) are not copied in */
        if (atomic_load_explicit(&home_pages,memory_order_relaxed) && home_any_phys(gs,len)) { write_unlock(); refusal=3; break; }
        n=collect_aliases(gs,gs+len,a,&refusal); /* what is mapped now, not before the allocation */
        if (!refusal && freeze_publish(a,n)) { locked=1; break; }
        write_unlock();
        if (refusal) break;
        if (!waited++) UMA_ADD(io_waits,1);
        if (runtime_wait_clock()>deadline) { refusal=2; break; }
        const struct timespec t={0,50000};
        nanosleep(&t,NULL);
    }
    if (!locked) {
        munmap(scratch,len);
        hand_back(gs,len,fd);
        if (refusal==1) UMA_ADD(refused_exec,1);
        else if (refusal==3) __atomic_fetch_add(&home_stats.refused_promotions,1,__ATOMIC_RELAXED);
        else UMA_ADD(refused_busy,1);
        return 0;
    }
    /* Frozen: no guest thread writes the granule, the runtime's own writers wait for the lock. */
    const uint64_t started=runtime_wait_clock();
    freezing=1;
    for (int i=0;i<n;++i) alias_protect(&a[i],1);
    dmabuf_sync(fd,0);
    copy_granule(scratch,gs,len);
    dmabuf_sync(fd,1);
    int mapped=mmap(backing_base+gs,len,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0)!=MAP_FAILED, done=0;
    for (;mapped && done<n;++done) /* PROT_NONE until all views are switched: no thread sees only some of them */
        mapped=mmap((void *)a[done].va,a[done].length,PROT_NONE,MAP_SHARED|MAP_FIXED,fd,(off_t)a[done].offset)!=MAP_FAILED;
    if (mapped) {
        fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)gs,(off_t)len); /* the RAM moved */
        atomic_store_explicit(&granule_fd[g],fd,memory_order_release);
    } else {
        views_to_memfd(gs,len,a,done); /* done: the pieces mapped, and the one that failed */
    }
    for (int i=0;i<n;++i) alias_protect(&a[i],0); /* the protection each page had */
    const uint64_t frozen_ns=runtime_wait_clock()-started;
    freezing=0;
    freeze_thaw();
    write_unlock();
    munmap(scratch,len);
    if (!mapped) { hand_back(gs,len,fd); UMA_ADD(promotion_failures,1); return 0; }
    UMA_ADD(promotions,1); UMA_ADD(shared_granules,1); UMA_ADD(shared_bytes,len); UMA_ADD(promote_ns_total,frozen_ns);
    if (frozen_ns>uma.promote_ns_max) __atomic_store_n(&uma.promote_ns_max,frozen_ns,__ATOMIC_RELAXED); /* promote_mutex */
    return 1;
}
int runtime_memory_promote(uint64_t phys, uint64_t size) {
    if (!__atomic_load_n(&granule_fd,__ATOMIC_ACQUIRE) || exclusive_depth || !size || phys>=POOL_SIZE) return 0;
    const uint64_t end=size<POOL_SIZE-phys ? phys+size : POOL_SIZE;
    int all=1;
    pthread_mutex_lock(&promote_mutex);
    for (uint64_t g=phys/granule_size;g<=(end-1)/granule_size;++g)
        if (!promote_granule(g)) all=0;
    pthread_mutex_unlock(&promote_mutex);
    return all;
}
int runtime_memory_granule_shared(uint64_t phys) {
    _Atomic(int) *fds=__atomic_load_n(&granule_fd,__ATOMIC_ACQUIRE);
    return fds && phys<POOL_SIZE && atomic_load_explicit(&fds[phys/granule_size],memory_order_acquire)>=0;
}
void runtime_memory_uma_stats(RuntimeUmaStats *out) {
    const uint64_t *from=(const uint64_t *)&uma;
    uint64_t *to=(uint64_t *)out;
    for (size_t i=0;i<sizeof(uma)/sizeof(uint64_t);++i) to[i]=__atomic_load_n(&from[i],__ATOMIC_RELAXED);
    out->granule_bytes=granule_size;
}
/* ---- bbport UMA: VMA compaction of SHARED granules (runtime.h, runtime_memory_compact_granule) ----
 * A SHARED granule's mappings are dma-buf mappings, which the kernel never merges (VM_PFNMAP/VM_IO:
 * "special" VMAs): each mprotect of part of one (the GPU page manager's 4 KiB write tracking, the
 * traps) splits it for good, and giving the pages their old protection back does not join the pieces.
 * Seen on the Thor: 30000 dma-buf VMAs, most of them 4 KiB, within minutes; at vm.max_map_count every
 * mmap fails (Turnip: vkQueueSubmit -> VK_ERROR_MEMORY_MAP_FAILED). A compaction maps the dma-buf again
 * over each run of the granule's guest aliases that is contiguous in both the guest's addresses and the
 * granule (one mapping, with the protection every page of it allows at least), then gives the pages
 * whose protection allows more theirs, one mprotect per run (page_prot: what promotion gives back). No
 * byte moves: the data is in the dma-buf, before and after. Under the runtime lock (exclusive: no
 * protection changes meanwhile) and a freeze as a promotion's: an access the narrower mapping refuses
 * faults, waits in runtime_memory_promotion_fault and is retried; kernel writes in flight (io_begin) make
 * the granule wait for the next round, and new ones wait for the freeze to end. Lock order: compact_mutex,
 * the runtime lock, io_mutex. */
#define UMA_COMPACT_ALIASES 64
#define UMA_COMPACT_ROUND 32         /* granules per round (a scan of /proc/self/maps each) */
#define UMA_COMPACT_TICK_MAX 256     /* granules per 2 s tick, at most */
#define UMA_COMPACT_MIN_GAIN 16      /* mappings a granule has to have gained since its last compaction */
static pthread_mutex_t compact_mutex=PTHREAD_MUTEX_INITIALIZER;
static uint32_t *compact_floor; /* per granule: splits its last compaction left (compact_mutex) */
static RuntimeUmaCompactStats compact_stats;
static _Atomic uint64_t maps_sampled;
#define COMPACT_ADD(field,n) __atomic_fetch_add(&compact_stats.field,(uint64_t)(n),__ATOMIC_RELAXED)
/* Compacts granule g (compact_mutex held, runtime lock not): 1 done (*runs: the mappings left over its
 * aliases, *groups: the mmaps it took), 0 not SHARED, -1 skipped for now (kernel I/O in flight), -2
 * skipped while its mappings stay as they are (an executable alias, too many aliases). */
static int compact_one(uint64_t g, int remap_backing, uint64_t *runs, uint64_t *groups) {
    const uint64_t gs=g*granule_size, ge=uma_end(g);
    Alias a[UMA_COMPACT_ALIASES], grp[UMA_COMPACT_ALIASES];
    int n=0, m=0;
    *runs=*groups=0;
    write_lock();
    const int fd=uma_fd(g);
    if (fd<0) { write_unlock(); return 0; }
    for (size_t i=0;i<vma_count;++i) { /* the aliases, in address order */
        const Vma *v=&vmas[i];
        if (v->kind!=KIND_DIRECT) continue;
        const uint64_t length=v->end-v->start, lo=v->phys>gs ? v->phys : gs, hi=v->phys+length<ge ? v->phys+length : ge;
        if (lo>=hi) continue;
        if ((v->prot & 4) || n==UMA_COMPACT_ALIASES) { write_unlock(); COMPACT_ADD(skipped,1); return -2; }
        a[n++]=(Alias){v->start+(lo-v->phys),lo-gs,hi-lo,v->prot};
    }
    /* Pieces of the runtime's table that follow each other in both spaces: one mmap. */
    for (int i=0;i<n;++i) {
        if (m && grp[m-1].va+grp[m-1].length==a[i].va && grp[m-1].offset+grp[m-1].length==a[i].offset) grp[m-1].length+=a[i].length;
        else grp[m++]=a[i];
    }
    if (m && !freeze_publish(grp,m)) { write_unlock(); COMPACT_ADD(skipped,1); return -1; }
    const uint64_t started=runtime_wait_clock();
    freezing=1;
    for (int k=0,i=0;k<m;++k) {
        /* The protection every page of the group allows: nobody gets an access the page refuses. */
        const uintptr_t gend=grp[k].va+grp[k].length;
        int base=PROT_READ|PROT_WRITE;
        for (int j=i;j<n && a[j].va<gend;++j)
            for (uintptr_t p=a[j].va;p<a[j].va+a[j].length && base;p+=4096) base&=page_prot(a[j].prot,p>>12);
        int tries=0;
        while (mmap((void *)grp[k].va,grp[k].length,base,MAP_SHARED|MAP_FIXED,fd,(off_t)grp[k].offset)==MAP_FAILED) {
            if (++tries==8) {
                fprintf(stderr,"STOP: UMA compaction could not map granule %#" PRIx64 " at %#" PRIxPTR "+%#" PRIx64 ": %s\n",
                        gs,grp[k].va,grp[k].length,strerror(errno));
                exit(23);
            }
            const struct timespec t={0,1000000};
            nanosleep(&t,NULL); /* ENOMEM at the map count limit: another thread's munmap may come */
        }
        /* The pages that allow more than the base: one mprotect per run (one mapping each). */
        uintptr_t run=grp[k].va; int run_prot=base;
        uint64_t count=1;
        for (;i<n && a[i].va<gend;++i)
            for (uintptr_t p=a[i].va;p<a[i].va+a[i].length;p+=4096) {
                const int prot=page_prot(a[i].prot,p>>12);
                if (prot==run_prot) continue;
                if (run_prot!=base) mprotect((void *)run,p-run,run_prot);
                run=p; run_prot=prot; ++count;
            }
        if (run_prot!=base) mprotect((void *)run,gend-run,run_prot);
        *runs+=count;
    }
    *groups=(uint64_t)m;
    /* The backing view: nothing protects it, but should anything have split it, it is one mapping again
     * (the same pages, read-write as before: its users never notice). */
    if (remap_backing && backing_base &&
        mmap(backing_base+gs,ge-gs,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0)==MAP_FAILED) {
        fprintf(stderr,"STOP: UMA compaction could not map the backing view of granule %#" PRIx64 ": %s\n",gs,strerror(errno));
        exit(23);
    }
    const uint64_t frozen_ns=runtime_wait_clock()-started;
    freezing=0;
    if (m) freeze_thaw();
    write_unlock();
    COMPACT_ADD(granules,1); COMPACT_ADD(compact_ns_total,frozen_ns);
    if (frozen_ns>compact_stats.compact_ns_max) __atomic_store_n(&compact_stats.compact_ns_max,frozen_ns,__ATOMIC_RELAXED);
    if (compact_floor) compact_floor[g]=(uint32_t)(*runs-*groups);
    return 1;
}
int runtime_memory_compact_granule(uint64_t phys) {
    if (!__atomic_load_n(&granule_fd,__ATOMIC_ACQUIRE) || exclusive_depth || phys>=POOL_SIZE) return exclusive_depth ? -1 : 0;
    pthread_mutex_lock(&compact_mutex);
    uint64_t runs, groups;
    const int rc=compact_one(phys/granule_size,1,&runs,&groups);
    pthread_mutex_unlock(&compact_mutex);
    return rc>0 ? (int)(runs+1) : rc<0 ? -1 : 0; /* + the backing view */
}
/* Mapping start addresses (of /proc/self/maps) in the guest's range and in the backing view. */
typedef struct { uintptr_t *v; size_t n, cap; } MapStarts;
/* Lines of /proc/self/maps, 0 when it cannot be read; with starts, the start addresses that matter. */
static uint64_t maps_scan(char *buffer, size_t size, MapStarts *starts) {
    const int fd=open("/proc/self/maps",O_RDONLY|O_CLOEXEC);
    if (fd<0) return 0;
    const uintptr_t back_lo=(uintptr_t)backing_base, back_hi=back_lo+(backing_base ? POOL_SIZE : 0);
    uint64_t lines=0; uintptr_t value=0; int in_address=1;
    if (starts) starts->n=0;
    for (;;) {
        const ssize_t got=read(fd,buffer,size);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) break;
        if (!starts) {
            for (const char *p=buffer, *end=buffer+got; (p=memchr(p,'\n',(size_t)(end-p)));++p) ++lines;
            continue;
        }
        for (ssize_t k=0;k<got;++k) {
            const char c=buffer[k];
            if (c=='\n') { ++lines; value=0; in_address=1; continue; }
            if (!in_address) continue;
            if (c>='0' && c<='9') value=value<<4|(uintptr_t)(c-'0');
            else if (c>='a' && c<='f') value=value<<4|(uintptr_t)(c-'a'+10);
            else {
                in_address=0;
                if ((value>=USER_MIN && value<USER_MAX) || (value>=back_lo && value<back_hi)) {
                    if (starts->n==starts->cap) {
                        const size_t cap=starts->cap ? starts->cap*2 : 65536;
                        uintptr_t *grown=realloc(starts->v,cap*sizeof(*grown));
                        if (!grown) { close(fd); return lines; }
                        starts->v=grown; starts->cap=cap;
                    }
                    starts->v[starts->n++]=value;
                }
            }
        }
    }
    close(fd);
    return lines;
}
/* Mappings starting strictly inside (lo, hi): the splits of what was mapped there in one piece. */
static uint64_t splits_inside(const MapStarts *s, uintptr_t lo, uintptr_t hi) {
    size_t a=0, b=s->n;
    while (a<b) { const size_t mid=(a+b)/2; if (s->v[mid]<=lo) a=mid+1; else b=mid; }
    size_t c=a, d=s->n;
    while (c<d) { const size_t mid=(c+d)/2; if (s->v[mid]<hi) c=mid+1; else d=mid; }
    return c-a;
}
typedef struct { uint64_t gain, g; int backing; } CompactPick;
static int pick_order(const void *x, const void *y) {
    const CompactPick *a=x, *b=y;
    return a->gain<b->gain ? 1 : a->gain>b->gain ? -1 : (a->g>b->g)-(a->g<b->g);
}
/* The most fragmented SHARED granules by starts (a scan of /proc/self/maps), compacted (compact_mutex held). */
static size_t compact_from_scan(const MapStarts *starts, size_t max, uint64_t min_gain) {
    const uint64_t count=(POOL_SIZE+granule_size-1)/granule_size;
    if (!compact_floor && !(compact_floor=calloc(count,sizeof(*compact_floor)))) return 0;
    uint32_t *splits=calloc(count,sizeof(*splits));
    unsigned char *backing=calloc(count,1);
    CompactPick *picks=NULL;
    size_t picked=0, done=0;
    if (!splits || !backing) goto out;
    read_lock();
    for (size_t i=0;i<vma_count;++i) {
        const Vma *v=&vmas[i];
        if (v->kind!=KIND_DIRECT || v->phys>=POOL_SIZE) continue;
        const uint64_t length=v->end-v->start, pend=v->phys+length<POOL_SIZE ? v->phys+length : POOL_SIZE;
        for (uint64_t g=v->phys/granule_size;g*granule_size<pend;++g) {
            if (uma_fd(g)<0) continue;
            const uint64_t gs=g*granule_size, ge=uma_end(g), lo=v->phys>gs ? v->phys : gs, hi=pend<ge ? pend : ge;
            const uint64_t s=splits_inside(starts,v->start+(lo-v->phys),v->start+(hi-v->phys));
            splits[g]=splits[g]+s>UINT32_MAX ? UINT32_MAX : (uint32_t)(splits[g]+s);
        }
    }
    for (uint64_t g=0;g<count && backing_base;++g)
        if (uma_fd(g)>=0 && splits_inside(starts,(uintptr_t)backing_base+g*granule_size,(uintptr_t)backing_base+uma_end(g))) {
            backing[g]=1;
            if (splits[g]<UINT32_MAX) ++splits[g];
        }
    read_unlock();
    for (uint64_t g=0;g<count;++g) if (splits[g]>compact_floor[g] && splits[g]-compact_floor[g]>=min_gain) ++picked;
    if (!picked || !(picks=malloc(picked*sizeof(*picks)))) goto out;
    picked=0;
    for (uint64_t g=0;g<count;++g)
        if (splits[g]>compact_floor[g] && splits[g]-compact_floor[g]>=min_gain) picks[picked++]=(CompactPick){splits[g]-compact_floor[g],g,backing[g]};
    qsort(picks,picked,sizeof(*picks),pick_order);
    for (size_t k=0;k<picked && done<max;++k) {
        uint64_t runs, groups;
        const int rc=compact_one(picks[k].g,picks[k].backing,&runs,&groups);
        if (rc>0) ++done;
        else if (rc==-2) compact_floor[picks[k].g]=splits[picks[k].g]; /* again once it gained min_gain more */
        const struct timespec t={0,300000};
        nanosleep(&t,NULL); /* the writers a freeze held go on before the next one */
    }
out:
    free(splits); free(backing); free(picks);
    return done;
}
static MapStarts compact_starts;
static char *compact_buffer;
#define COMPACT_BUFFER (256u<<10)
size_t runtime_memory_compact_shared(size_t max, uint64_t min_gain) {
    if (!__atomic_load_n(&granule_fd,__ATOMIC_ACQUIRE) || exclusive_depth || !max) return 0;
    pthread_mutex_lock(&compact_mutex);
    size_t done=0;
    if (compact_buffer || (compact_buffer=malloc(COMPACT_BUFFER))) {
        const uint64_t lines=maps_scan(compact_buffer,COMPACT_BUFFER,&compact_starts);
        if (lines) {
            atomic_store_explicit(&maps_sampled,lines,memory_order_relaxed);
            done=compact_from_scan(&compact_starts,max,min_gain);
        }
    }
    pthread_mutex_unlock(&compact_mutex);
    return done;
}
void runtime_memory_uma_compact_stats(RuntimeUmaCompactStats *out) {
    const uint64_t *from=(const uint64_t *)&compact_stats;
    uint64_t *to=(uint64_t *)out;
    for (size_t i=0;i<sizeof(compact_stats)/sizeof(uint64_t);++i) to[i]=__atomic_load_n(&from[i],__ATOMIC_RELAXED);
    out->maps_last=atomic_load_explicit(&maps_sampled,memory_order_relaxed);
}
uint64_t runtime_memory_maps_count(void) { return atomic_load_explicit(&maps_sampled,memory_order_relaxed); }
static uint64_t compact_threshold(void) {
    const char *env=getenv("BB_UMA_COMPACT_AT");
    return env && *env ? strtoull(env,NULL,0) : 20000;
}
/* Every 2 s: the process's mapping count; above the threshold, rounds of compactions of the most
 * fragmented granules until 3/4 of it (at most UMA_COMPACT_TICK_MAX granules a tick, the rest next tick). */
static void *uma_compactor(void *arg) {
    const uint64_t at=(uint64_t)(uintptr_t)arg, low=at-at/4;
#if defined(__linux__)
    pthread_setname_np(pthread_self(),"BbUmaCompact");
    setpriority(PRIO_PROCESS,(id_t)syscall(SYS_gettid),10); /* this thread only */
#endif
    char *buffer=malloc(COMPACT_BUFFER);
    if (!buffer) return NULL;
    printf("UMA: mapping compaction above %" PRIu64 " mappings (BB_UMA_COMPACT_AT)\n",at);
    for (;;) {
        const struct timespec tick={2,0};
        nanosleep(&tick,NULL);
        uint64_t lines=maps_scan(buffer,COMPACT_BUFFER,NULL);
        if (!lines) continue;
        atomic_store_explicit(&maps_sampled,lines,memory_order_relaxed);
        if (lines<at) continue;
        const uint64_t before=lines;
        size_t tick_done=0;
        pthread_mutex_lock(&compact_mutex);
        for (;;) { /* a scan, a round; the last scan is the count after */
            const uint64_t scanned=maps_scan(buffer,COMPACT_BUFFER,&compact_starts);
            if (!scanned) break;
            lines=scanned;
            atomic_store_explicit(&maps_sampled,lines,memory_order_relaxed);
            if (lines<low || tick_done>=UMA_COMPACT_TICK_MAX) break;
            const size_t done=compact_from_scan(&compact_starts,UMA_COMPACT_ROUND,UMA_COMPACT_MIN_GAIN);
            if (!done) break; /* nothing gained enough since its last compaction */
            tick_done+=done;
        }
        pthread_mutex_unlock(&compact_mutex);
        if (!tick_done) continue;
        const uint64_t rounds=__atomic_add_fetch(&compact_stats.rounds,1,__ATOMIC_RELAXED);
        __atomic_store_n(&compact_stats.maps_before,before,__ATOMIC_RELAXED);
        __atomic_store_n(&compact_stats.maps_after,lines,__ATOMIC_RELAXED);
        if (!(rounds & (rounds-1)))
            printf("UMA: compacted %zu granules, VMAs %" PRIu64 " -> %" PRIu64 " (round %" PRIu64 ", %" PRIu64 " granules in all, %.2f ms longest freeze)\n",
                   tick_done,before,lines,rounds,__atomic_load_n(&compact_stats.granules,__ATOMIC_RELAXED),
                   (double)__atomic_load_n(&compact_stats.compact_ns_max,__ATOMIC_RELAXED)/1e6);
    }
    return NULL;
}
static void compactor_start(void) {
    const uint64_t at=compact_threshold();
    if (!at) { puts("UMA: mapping compaction off (BB_UMA_COMPACT_AT=0)"); return; }
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread,&attr,uma_compactor,(void *)(uintptr_t)at)) fputs("UMA: no mapping compaction thread\n",stderr);
    pthread_attr_destroy(&attr);
}
/* ---- bbport UMA: demotion SHARED -> CPU (runtime.h, runtime_memory_demote) ----
 * The mirror of promote_granule: the writers frozen (every guest alias read-only; a fault waits in
 * runtime_memory_promotion_fault and is retried on the new mapping; the runtime's own writers wait for its
 * lock; kernel writes in flight are waited for, new ones wait for the thaw), the granule's bytes copied from
 * the dma-buf into the memfd range (punched since the promotion) through a scratch mapping of the memfd,
 * all-zero pages left as holes, the memfd mapped over the backing view and every alias (PROT_NONE until all
 * are switched), every page's protection given back (page_prot), the state CPU, the fd closed. The GPU
 * library frees its own handle of the memory itself, after its last use of it. Lock order as promotion:
 * promote_mutex, the runtime lock, io_mutex; then compact_mutex alone. */
static RuntimeUmaDemoteStats demote_stats;
#define DEMOTE_ADD(field,n) __atomic_fetch_add(&demote_stats.field,(uint64_t)(n),__ATOMIC_RELAXED)
static int page_all_zero(const unsigned char *p) {
    const uint64_t *w=(const uint64_t *)p;
    for (size_t i=0;i<4096/8;i+=4) if (w[i]|w[i+1]|w[i+2]|w[i+3]) return 0;
    return 1;
}
/* len bytes from src into dst, runs of non-zero 4 KiB pages only (dst is a hole elsewhere); returns them. */
static uint64_t copy_nonzero(unsigned char *dst, const unsigned char *src, uint64_t len) {
    uint64_t kept=0;
    for (uint64_t at=0;at<len;) {
        if (page_all_zero(src+at)) { at+=4096; continue; }
        uint64_t end=at+4096;
        while (end<len && !page_all_zero(src+end)) end+=4096;
        memcpy(dst+at,src+at,end-at);
        kept+=end-at;
        at=end;
    }
    return kept;
}
static int range_any_allocated(uint64_t start, uint64_t end) { /* runtime lock held */
    for (int i=0;i<LIMIT;++i)
        if (blocks[i].used && blocks[i].start<end && start<blocks[i].start+blocks[i].size) return 1;
    return 0;
}
/* Every direct mapping of [gs,ge), executable ones too (runtime lock held); *too_many past UMA_COMPACT_ALIASES. */
static int demote_aliases(uint64_t gs, uint64_t ge, Alias *out, int *too_many) {
    int n=0;
    *too_many=0;
    for (size_t i=0;i<vma_count;++i) {
        const Vma *v=&vmas[i];
        if (v->kind!=KIND_DIRECT) continue;
        const uint64_t length=v->end-v->start, lo=v->phys>gs ? v->phys : gs, hi=v->phys+length<ge ? v->phys+length : ge;
        if (lo>=hi) continue;
        if (n==UMA_COMPACT_ALIASES) { *too_many=1; return n; }
        out[n++]=(Alias){v->start+(lo-v->phys),lo-gs,hi-lo,v->prot};
    }
    return n;
}
/* A demotion that failed in the middle: the backing view and the aliases switched so far (and the one
 * that failed) back to the dma-buf, whose bytes nobody changed (frozen). */
static void views_to_dmabuf(int fd, uint64_t gs, uint64_t len, const Alias *a, int count) {
    int ok=mmap(backing_base+gs,len,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0)!=MAP_FAILED;
    for (int i=0;i<count && ok;++i)
        ok=mmap((void *)a[i].va,a[i].length,PROT_NONE,MAP_SHARED|MAP_FIXED,fd,(off_t)a[i].offset)!=MAP_FAILED;
    if (!ok) { fprintf(stderr,"STOP: UMA demotion could not restore granule %#" PRIx64 ": %s\n",gs,strerror(errno)); exit(22); }
}
static int demote_granule(uint64_t g) {
    const uint64_t gs=g*granule_size, len=uma_end(g)-gs;
    if (uma_fd(g)<0) return 1;
    unsigned char *scratch=mmap(NULL,len,PROT_READ|PROT_WRITE,MAP_SHARED,pool_fd,(off_t)gs);
    if (scratch==MAP_FAILED) { DEMOTE_ADD(failures,1); return 0; }
    Alias a[UMA_COMPACT_ALIASES];
    int n=0, too_many=0, locked=0, waited=0, fd=-1;
    const uint64_t deadline=runtime_wait_clock()+UMA_IO_WAIT_NS;
    for (;;) {
        write_lock();
        fd=uma_fd(g);
        if (fd<0) { write_unlock(); munmap(scratch,len); return 1; }
        n=demote_aliases(gs,gs+len,a,&too_many);
        if (too_many) { write_unlock(); break; }
        if (freeze_publish(a,n)) { locked=1; break; }
        write_unlock();
        if (!waited++) UMA_ADD(io_waits,1);
        if (runtime_wait_clock()>deadline) break;
        const struct timespec t={0,50000};
        nanosleep(&t,NULL);
    }
    if (!locked) { munmap(scratch,len); DEMOTE_ADD(busy,1); return 0; }
    /* Frozen: no guest thread writes the granule, the runtime's own writers wait for the lock, and the GPU
     * never writes shared memory (written bindings take copies; DMA into it is done by the CPU). */
    const uint64_t started=runtime_wait_clock();
    freezing=1;
    for (int i=0;i<n;++i) alias_protect(&a[i],1);
    const int allocated=range_any_allocated(gs,gs+len);
    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)gs,(off_t)len); /* holes since the promotion */
    uint64_t kept=0;
    if (allocated) { /* released memory reads as zeros: nothing to keep when none is allocated */
        dmabuf_sync_flags(fd,1 /* READ, START */);
        kept=copy_nonzero(scratch,backing_base+gs,len);
        dmabuf_sync_flags(fd,1|4 /* READ, END */);
    }
    int ok=mmap(backing_base+gs,len,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)gs)!=MAP_FAILED, done=0;
    for (;ok && done<n;++done) /* PROT_NONE until all views are switched: no thread sees only some of them */
        ok=mmap((void *)a[done].va,a[done].length,PROT_NONE,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)(gs+a[done].offset))!=MAP_FAILED;
    if (ok) atomic_store_explicit(&granule_fd[g],-1,memory_order_release);
    else views_to_dmabuf(fd,gs,len,a,done);
    for (int i=0;i<n;++i) alias_protect(&a[i],0); /* the protection each page had */
    const uint64_t frozen_ns=runtime_wait_clock()-started;
    freezing=0;
    freeze_thaw();
    write_unlock();
    munmap(scratch,len);
    if (!ok) {
        fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)gs,(off_t)len); /* the copy goes */
        DEMOTE_ADD(failures,1);
        return 0;
    }
    close(fd);
    __atomic_fetch_sub(&uma.shared_granules,1,__ATOMIC_RELAXED);
    __atomic_fetch_sub(&uma.shared_bytes,len,__ATOMIC_RELAXED);
    DEMOTE_ADD(demotions,1); DEMOTE_ADD(kept_bytes,kept); DEMOTE_ADD(demoted_bytes,len); DEMOTE_ADD(demote_ns_total,frozen_ns);
    if (!allocated) DEMOTE_ADD(released,1);
    if (frozen_ns>demote_stats.demote_ns_max) __atomic_store_n(&demote_stats.demote_ns_max,frozen_ns,__ATOMIC_RELAXED); /* promote_mutex */
    pthread_mutex_lock(&compact_mutex);
    if (compact_floor) compact_floor[g]=0; /* a new promotion starts from one mapping per alias */
    pthread_mutex_unlock(&compact_mutex);
    return 1;
}
int runtime_memory_demote(uint64_t phys, uint64_t size) {
    if (!__atomic_load_n(&granule_fd,__ATOMIC_ACQUIRE) || exclusive_depth || !size || phys>=POOL_SIZE) return 0;
    const uint64_t end=size<POOL_SIZE-phys ? phys+size : POOL_SIZE;
    int all=1;
    pthread_mutex_lock(&promote_mutex);
    for (uint64_t g=phys/granule_size;g<=(end-1)/granule_size;++g)
        if (!demote_granule(g)) all=0;
    pthread_mutex_unlock(&promote_mutex);
    return all;
}
size_t runtime_memory_granule_scan(uint8_t *out, size_t count) {
    if (!__atomic_load_n(&granule_fd,__ATOMIC_ACQUIRE)) return 0;
    const uint64_t total=(POOL_SIZE+granule_size-1)/granule_size;
    if (!out || !count) return (size_t)total;
    const uint64_t n=count<total ? count : total;
    memset(out,0,(size_t)n);
    read_lock();
    for (uint64_t g=0;g<n;++g) if (uma_fd(g)>=0) out[g]|=RUNTIME_GRANULE_SHARED;
    for (int i=0;i<LIMIT;++i) {
        if (!blocks[i].used || !blocks[i].size || blocks[i].start>=POOL_SIZE) continue;
        const uint64_t last=(blocks[i].start+blocks[i].size-1)/granule_size;
        for (uint64_t g=blocks[i].start/granule_size;g<=last && g<n;++g) out[g]|=RUNTIME_GRANULE_ALLOCATED;
    }
    for (size_t i=0;i<vma_count;++i) {
        const Vma *v=&vmas[i];
        if (v->kind!=KIND_DIRECT || v->phys>=POOL_SIZE || v->end<=v->start) continue;
        const uint64_t last=(v->phys+(v->end-v->start)-1)/granule_size;
        for (uint64_t g=v->phys/granule_size;g<=last && g<n;++g) out[g]|=RUNTIME_GRANULE_MAPPED;
    }
    read_unlock();
    return (size_t)total;
}
void runtime_memory_uma_demote_stats(RuntimeUmaDemoteStats *out) {
    const uint64_t *from=(const uint64_t *)&demote_stats;
    uint64_t *to=(uint64_t *)out;
    for (size_t i=0;i<sizeof(demote_stats)/sizeof(uint64_t);++i) to[i]=__atomic_load_n(&from[i],__ATOMIC_RELAXED);
}
int runtime_memory_set_granule_allocator(int (*alloc)(uint64_t phys, uint64_t size),
                                         void (*release)(uint64_t phys, uint64_t size), uint64_t granule) {
    if (!alloc || granule<(UINT64_C(256)<<10) || granule>(UINT64_C(64)<<20) || (granule & (granule-1)) || granule%65536) return -1;
    write_lock();
    if (granule_fd || chunk_alloc) { write_unlock(); return -1; }
    const uint64_t count=(POOL_SIZE+granule-1)/granule;
    _Atomic(int) *fds=malloc(count*sizeof(*fds));
    void *table=mmap(NULL,GPU_STATE_LIMIT>>12,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
    if (!fds || table==MAP_FAILED) {
        free(fds);
        if (table!=MAP_FAILED) munmap(table,GPU_STATE_LIMIT>>12);
        write_unlock();
        return -1;
    }
    for (uint64_t g=0;g<count;++g) atomic_init(&fds[g],-1);
    gpu_state=table;
    granule_alloc=alloc; granule_release=release; granule_size=granule;
    __atomic_store_n(&granule_fd,fds,__ATOMIC_RELEASE);
    write_unlock();
    compactor_start();
    return 0;
}
/* Host view of a guest range, for the GPU backend: the range must be mapped. */
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return 1;
    read_lock();
    int result=covered(address,address+size,0);
    read_unlock();
    return result;
}
void runtime_memory_report(void) {
    printf("Runtime: direct memory allocations=%zu, maps=%zu, live=%" PRIu64 ", budget=%" PRIu64 "\n",
           allocations, maps, live_bytes, POOL_SIZE);
    printf("Runtime: flexible maps=%zu, in use=%" PRIu64 ", protects=%zu, queries=%zu, regions=%zu\n",
           flexible_maps, flexible_bytes, protects, queries, vma_count);
    if (home_stats.homes) {
        RuntimeHomeStats h;
        runtime_memory_home_stats(&h);
        printf("Runtime: texture homes=%" PRIu64 ", homed now %" PRIu64 " pages, restores=%" PRIu64 " (%" PRIu64 " pages, %" PRIu64
               " forgotten), released %" PRIu64 " pages, refused: mismatch %" PRIu64 " mapping %" PRIu64 " shared %" PRIu64
               " homed %" PRIu64 " aliases %" PRIu64 " full %" PRIu64 ", promotions refused %" PRIu64 ", hook calls %" PRIu64 "\n",
               h.homes, h.homed_pages, h.restores, h.restored_pages, h.forgotten_pages, h.released_pages,
               h.refused[RUNTIME_HOME_MISMATCH], h.refused[RUNTIME_HOME_MAPPING], h.refused[RUNTIME_HOME_SHARED],
               h.refused[RUNTIME_HOME_HOMED], h.refused[RUNTIME_HOME_ALIASES], h.refused[RUNTIME_HOME_FULL],
               h.refused_promotions, h.hook_calls);
    }
    if (granule_fd) {
        RuntimeUmaStats u;
        runtime_memory_uma_stats(&u);
        printf("Runtime: UMA granule=%" PRIu64 " KiB, shared=%" PRIu64 " (%" PRIu64 " MiB), promotions=%" PRIu64 ", failures=%" PRIu64
               ", refused exec=%" PRIu64 " busy=%" PRIu64 ", freeze faults=%" PRIu64 ", io waits=%" PRIu64
               ", frozen %.1f ms total, %.2f ms longest\n",
               u.granule_bytes>>10, u.shared_granules, u.shared_bytes>>20, u.promotions, u.promotion_failures, u.refused_exec,
               u.refused_busy, u.freeze_faults, u.io_waits, (double)u.promote_ns_total/1e6, (double)u.promote_ns_max/1e6);
        RuntimeUmaCompactStats c;
        runtime_memory_uma_compact_stats(&c);
        printf("Runtime: UMA mapping compaction: %" PRIu64 " rounds, %" PRIu64 " granules, %" PRIu64 " skipped, mappings now %" PRIu64
               " (last round %" PRIu64 " -> %" PRIu64 "), frozen %.1f ms total, %.2f ms longest\n",
               c.rounds, c.granules, c.skipped, c.maps_last, c.maps_before, c.maps_after,
               (double)c.compact_ns_total/1e6, (double)c.compact_ns_max/1e6);
        RuntimeUmaDemoteStats d;
        runtime_memory_uma_demote_stats(&d);
        printf("Runtime: UMA demotions=%" PRIu64 " (%" PRIu64 " MiB; released %" PRIu64 "), kept %" PRIu64 " MiB of non-zero pages, busy=%" PRIu64
               ", failures=%" PRIu64 ", frozen %.1f ms total, %.2f ms longest\n",
               d.demotions, d.demoted_bytes>>20, d.released, d.kept_bytes>>20, d.busy, d.failures,
               (double)d.demote_ns_total/1e6, (double)d.demote_ns_max/1e6);
    }
}
#else
void *runtime_low_map(size_t size, int prot) { (void)size; (void)prot; return NULL; }
void runtime_low_protect(uintptr_t start, size_t size, int prot) { (void)start; (void)size; (void)prot; }
int runtime_memory_low_info(uintptr_t address, int *prot, uintptr_t *end) { (void)address; (void)prot; (void)end; return 0; }
uintptr_t runtime_memory_resolve(const char *name) { (void)name; return 0; }
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) { (void)address; (void)size; return 0; }
void runtime_memory_report(void) { puts("Runtime: Windows direct-memory backend not implemented"); }
int runtime_memory_set_granule_allocator(int (*alloc)(uint64_t, uint64_t), void (*release)(uint64_t, uint64_t), uint64_t granule) {
    (void)alloc; (void)release; (void)granule; return -1;
}
int runtime_memory_promote(uint64_t phys, uint64_t size) { (void)phys; (void)size; return 0; }
int runtime_memory_granule_shared(uint64_t phys) { (void)phys; return 0; }
void runtime_memory_uma_stats(RuntimeUmaStats *out) { memset(out,0,sizeof(*out)); }
int runtime_memory_promotion_fault(void *address) { (void)address; return 0; }
int runtime_memory_compact_granule(uint64_t phys) { (void)phys; return 0; }
size_t runtime_memory_compact_shared(size_t max, uint64_t min_gain) { (void)max; (void)min_gain; return 0; }
void runtime_memory_uma_compact_stats(RuntimeUmaCompactStats *out) { memset(out,0,sizeof(*out)); }
uint64_t runtime_memory_maps_count(void) { return 0; }
int runtime_memory_demote(uint64_t phys, uint64_t size) { (void)phys; (void)size; return 0; }
size_t runtime_memory_granule_scan(uint8_t *out, size_t count) { (void)out; (void)count; return 0; }
void runtime_memory_uma_demote_stats(RuntimeUmaDemoteStats *out) { memset(out,0,sizeof(*out)); }
void *runtime_memory_io_begin(uintptr_t address, uint64_t size) { (void)address; (void)size; return NULL; }
void runtime_memory_io_end(void *token) { (void)token; }
int runtime_memory_home(uintptr_t address, uint64_t size, const void *expected, uint32_t *ticket) {
    (void)address; (void)size; (void)expected; (void)ticket; return RUNTIME_HOME_ARGS;
}
int runtime_memory_home_restore(uint32_t ticket, const void *data, uint64_t size) { (void)ticket; (void)data; (void)size; return -1; }
uint64_t runtime_memory_home_alive(uint32_t ticket) { (void)ticket; return 0; }
uint32_t runtime_memory_home_ticket(uintptr_t address) { (void)address; return 0; }
int runtime_memory_home_tickets(uintptr_t address, uint64_t size, uint32_t *out, int max) { (void)address; (void)size; (void)out; (void)max; return 0; }
int runtime_memory_home_touches(uintptr_t address, uint64_t size) { (void)address; (void)size; return 0; }
int runtime_memory_home_unstale(uintptr_t address) { (void)address; return 1; }
uint64_t runtime_memory_home_granule(void) { return 4096; }
uint64_t runtime_memory_homed_pages(void) { return 0; }
void runtime_memory_set_home_hook(int (*rehydrate)(uint32_t ticket)) { (void)rehydrate; }
void runtime_memory_home_stats(RuntimeHomeStats *out) { memset(out,0,sizeof(*out)); }
#endif
