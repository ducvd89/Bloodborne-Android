/* UMA granules (runtime_memory.c, BB_UMA): promotion CPU -> SHARED with a fake allocator. Each "granule"
 * is a memfd of the granule's size standing in for the GPU library's dma-buf; the test keeps a dup of it
 * (what the GPU would see) next to the runtime's.
 *
 * Standalone: needs only runtime_memory.c (stubs for three symbols of the rest of the runtime below).
 *   cc -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_uma_granules.c src/runtime_memory.c \
 *      -o out/uma-granules-test && out/uma-granules-test [granule KiB, 256..2048, default 256]
 * (run it with 256 and 2048). */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* The parts of the runtime runtime_memory.c calls. */
int32_t *runtime_errno(void) { static __thread int32_t e; return &e; }
void runtime_guest_call_sites(uint64_t out[3]) { out[0]=out[1]=out[2]=0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i=0;i<count;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
void runtime_memory_trap(uintptr_t address, uint64_t size, unsigned reason, int on);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size);
void *runtime_memory_backing_pointer(uint64_t phys);
void runtime_memory_set_guest_chunk_allocator(int (*alloc)(uint64_t phys, uint64_t size));

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr,"%s:%d: CHECK failed: %s\n",__FILE__,__LINE__,#cond); abort(); } } while (0)
typedef int32_t (ABI *Allocate)(int64_t, int64_t, uint64_t, uint64_t, int, int64_t *);
typedef int32_t (ABI *Map)(void **, uint64_t, int, int, int64_t, uint64_t);
typedef int32_t (ABI *Unmap)(void *, uint64_t);
typedef int32_t (ABI *Release)(uint64_t, uint64_t);
typedef int32_t (ABI *Protect)(const void *, uint64_t, int);
static Allocate allocate_direct; static Map map_direct; static Unmap unmap_direct; static Release release_direct;
static Protect guest_mprotect;
#define MIB (UINT64_C(1)<<20)
#define PS4_PAGE UINT64_C(16384)
static uint64_t gsz; /* granule size */

/* ---- the fake GPU library ---- */
#define MAX_GRANULES 8192
static int gpu_fd[MAX_GRANULES]; /* the GPU side's own fd of each granule it allocated, -1 none */
static int alloc_count, release_count;
static uint64_t last_release_phys, fail_phys=UINT64_MAX;
static int fake_alloc(uint64_t phys, uint64_t size) {
    if (phys==fail_phys) return -1;
    const int fd=memfd_create("fake-dmabuf",MFD_CLOEXEC);
    CHECK(fd>=0 && !ftruncate(fd,(off_t)size));
    CHECK(gpu_fd[phys/gsz]<0);
    gpu_fd[phys/gsz]=dup(fd);
    ++alloc_count;
    return fd;
}
static void fake_release(uint64_t phys, uint64_t size) {
    (void)size;
    CHECK(gpu_fd[phys/gsz]>=0);
    close(gpu_fd[phys/gsz]); gpu_fd[phys/gsz]=-1;
    last_release_phys=phys;
    ++release_count;
}
/* What the GPU sees at phys: a mapping of the granule's dma-buf. */
static unsigned char *gpu_view(uint64_t phys) {
    static unsigned char *views[MAX_GRANULES];
    const uint64_t g=phys/gsz;
    CHECK(gpu_fd[g]>=0);
    if (!views[g]) { views[g]=mmap(NULL,gsz,PROT_READ|PROT_WRITE,MAP_SHARED,gpu_fd[g],0); CHECK(views[g]!=MAP_FAILED); }
    return views[g]+phys%gsz;
}

/* ---- faults ---- */
static __thread sigjmp_buf *probe;
static _Atomic int handler_calls;
static void on_segv(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)context;
    atomic_fetch_add(&handler_calls,1);
    if (runtime_memory_promotion_fault(info->si_addr)) return;
    if (probe) siglongjmp(*probe,1);
    static const char message[]="uma test: unexpected fault\n";
    ssize_t w=write(2,message,sizeof(message)-1); (void)w;
    _exit(99);
}
/* 1 if the access faults (and is not a promotion's freeze). */
static int faults(const volatile unsigned char *p, int write) {
    sigjmp_buf jump;
    probe=&jump;
    if (sigsetjmp(jump,1)) { probe=NULL; return 1; }
    if (write) { const unsigned char v=*p; *(volatile unsigned char *)p=v; } else (void)*p;
    probe=NULL;
    return 0;
}

/* ---- helpers ---- */
static uint64_t next_low; /* direct memory is never reused: a granule shared stays shared */
static uint64_t alloc_direct(uint64_t size) {
    int64_t phys=-1;
    CHECK(allocate_direct((int64_t)next_low,(int64_t)(8192*MIB),size,gsz,3,&phys)==0 && (uint64_t)phys%gsz==0);
    next_low=(uint64_t)phys+size;
    return (uint64_t)phys;
}
static unsigned char *map_at(uint64_t phys, uint64_t size, int prot) {
    void *p=NULL;
    CHECK(map_direct(&p,size,prot,0,(int64_t)phys,0)==0);
    return p;
}
static unsigned char pattern(uint64_t i, unsigned seed) { return (unsigned char)(i*131+seed*17+(i>>9)); }
static void fill(unsigned char *p, uint64_t n, unsigned seed) { for (uint64_t i=0;i<n;++i) p[i]=pattern(i,seed); }
static int same(const unsigned char *p, uint64_t n, unsigned seed, uint64_t base) {
    for (uint64_t i=0;i<n;++i) if (p[i]!=pattern(base+i,seed)) return 0;
    return 1;
}
static RuntimeUmaStats stats(void) { RuntimeUmaStats s; runtime_memory_uma_stats(&s); return s; }

static void off_by_default(void) {
    CHECK(runtime_memory_promote(0,4096)==0 && runtime_memory_granule_shared(0)==0);
    CHECK(runtime_memory_io_begin(0x1000,64)==NULL && runtime_memory_promotion_fault((void *)0x1000)==0);
    runtime_memory_io_end(NULL);
    CHECK(stats().granule_bytes==0);
    /* the registration refuses what it cannot do */
    CHECK(runtime_memory_set_granule_allocator(fake_alloc,fake_release,128<<10)==-1);
    CHECK(runtime_memory_set_granule_allocator(fake_alloc,fake_release,(384u<<10))==-1);
    CHECK(runtime_memory_set_granule_allocator(fake_alloc,fake_release,128*MIB)==-1);
    CHECK(runtime_memory_set_granule_allocator(fake_alloc,fake_release,0)==-1);
    CHECK(runtime_memory_set_granule_allocator(NULL,fake_release,2*MIB)==-1);
    /* chunk mode excludes it (own process: the chunk allocator is global) */
    const pid_t child=fork();
    CHECK(child>=0);
    if (!child) {
        runtime_memory_set_guest_chunk_allocator((int (*)(uint64_t,uint64_t))fake_alloc);
        _exit(runtime_memory_set_granule_allocator(fake_alloc,fake_release,2*MIB)==-1 ? 0 : 1);
    }
    int status=0;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    /* registered before any direct memory exists: nothing to promote yet, and nothing breaks */
    const pid_t early=fork();
    CHECK(early>=0);
    if (!early) {
        if (runtime_memory_set_granule_allocator(fake_alloc,fake_release,gsz)) _exit(2);
        _exit(runtime_memory_promote(0,gsz)==0 && alloc_count==0 ? 0 : 1);
    }
    CHECK(waitpid(early,&status,0)==early && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void lazy_and_promotion(void) {
    const uint64_t region=gsz*4>8*MIB ? gsz*4 : 8*MIB, phys=alloc_direct(region);
    unsigned char *a=map_at(phys,region,3), *b=map_at(phys+gsz,2*gsz,3), *c=map_at(phys+3*gsz,gsz,1);
    /* mappings that cover part of a granule, and one across two */
    unsigned char *part=map_at(phys+gsz/4,gsz/2,3), *straddle=map_at(phys+gsz-32768,65536,3);
    fill(a,region,1);
    const int allocs0=alloc_count;
    const uint64_t promotions0=stats().promotions;
    CHECK(!runtime_memory_granule_shared(phys) && !runtime_memory_granule_shared(phys+region-1)); /* lazy */
    /* spans granules 0 and 1 */
    CHECK(runtime_memory_promote(phys+gsz/2,gsz)==1);
    CHECK(alloc_count==allocs0+2 && runtime_memory_granule_shared(phys) && runtime_memory_granule_shared(phys+gsz) &&
          !runtime_memory_granule_shared(phys+2*gsz) && !runtime_memory_granule_shared(phys+4*gsz-1));
    CHECK(same(a,region,1,0) && same(b,2*gsz,1,gsz) && same(c,gsz,1,3*gsz));  /* data survived, in every alias */
    CHECK(same(part,gsz/2,1,gsz/4) && same(straddle,65536,1,gsz-32768));
    CHECK(same(gpu_view(phys),gsz,1,0) && same(gpu_view(phys+gsz),gsz,1,gsz)); /* and the GPU sees it */
    unsigned char *back=runtime_memory_backing_pointer(phys);
    CHECK(same(back,2*gsz,1,0));
    part[5]=0x99; straddle[40000]=0x98;
    CHECK(a[gsz/4+5]==0x99 && gpu_view(phys+gsz/4)[5]==0x99 && a[gsz+7232]==0x98 && gpu_view(phys+gsz)[7232]==0x98);
    /* one memory: a write through one alias shows in the other, in the backing view and for the GPU */
    a[gsz+5]=0xa5;
    CHECK(b[5]==0xa5 && back[gsz+5]==0xa5 && gpu_view(phys+gsz)[5]==0xa5);
    b[gsz+7]=0x5a; /* granule 2: still CPU, same alias rules */
    CHECK(a[2*gsz+7]==0x5a && back[2*gsz+7]==0x5a);
    unsigned char v=0x3c;
    CHECK(runtime_memory_write_backing((uintptr_t)a+10,&v,1)==1 && a[10]==0x3c && gpu_view(phys)[10]==0x3c);
    unsigned char got[4];
    runtime_memory_read_backing((uintptr_t)a+gsz+4,got,4);
    CHECK(got[1]==0xa5);
    gpu_view(phys+gsz)[9]=0x77; /* the GPU writes: the game sees it */
    CHECK(a[gsz+9]==0x77 && b[9]==0x77);
    CHECK(faults(c,1) && !faults(c,0)); /* the read-only alias is still read-only */
    /* again: nothing to do */
    const int allocs=alloc_count;
    CHECK(runtime_memory_promote(phys,2*gsz)==1 && alloc_count==allocs);
    const RuntimeUmaStats s=stats();
    CHECK(s.granule_bytes==gsz && s.promotions==promotions0+2 && s.shared_granules==s.promotions && s.shared_bytes==s.promotions*gsz && s.promotion_failures==0);
    CHECK(s.promote_ns_total>0 && s.promote_ns_max>0 && s.promote_ns_max<=s.promote_ns_total);
    /* a mapping made now spans CPU and SHARED granules in one go */
    unsigned char *d=map_at(phys,region,3);
    CHECK(d[gsz+5]==0xa5 && d[2*gsz+7]==0x5a && same(d+3*gsz+16,gsz-16,1,3*gsz+16));
    d[gsz+100]=0x11; d[3*gsz+100]=0x22;
    CHECK(a[gsz+100]==0x11 && gpu_view(phys+gsz)[100]==0x11 && c[100]==0x22);
    CHECK(unmap_direct(d,region)==0);
    /* releasing the memory clears a shared granule (a dma-buf cannot be hole punched) */
    CHECK(release_direct(phys,region)==0);
    for (uint64_t i=0;i<region;i+=4096) CHECK(!back[i] && !back[i+4095]);
    for (uint64_t i=0;i<2*gsz;i+=4096) CHECK(!gpu_view(phys+i)[0]);
    CHECK(runtime_memory_granule_shared(phys)); /* TODO phase 4: demotion */
    /* the same range allocated and mapped again starts empty, from the shared granules */
    int64_t again=-1;
    CHECK(allocate_direct((int64_t)phys,(int64_t)(phys+region),region,gsz,3,&again)==0 && (uint64_t)again==phys);
    unsigned char *e=map_at(phys,region,3);
    CHECK(!e[0] && !e[gsz+5] && !e[2*gsz+7] && !e[region-1]);
    e[gsz+3]=0x6b;
    CHECK(gpu_view(phys+gsz)[3]==0x6b && back[gsz+3]==0x6b);
    CHECK(release_direct(phys,region)==0);
}

static void refusals_and_failures(void) {
    /* executable mappings: the granule stays CPU */
    uint64_t phys=alloc_direct(2*gsz);
    unsigned char *x=map_at(phys,gsz,7), *w=map_at(phys+gsz,gsz,3);
    x[0]=1; w[0]=2;
    RuntimeUmaStats before=stats();
    int allocs=alloc_count;
    CHECK(runtime_memory_promote(phys,gsz)==0 && !runtime_memory_granule_shared(phys));
    CHECK(stats().refused_exec==before.refused_exec+1 && alloc_count==allocs && x[0]==1);
    CHECK(runtime_memory_promote(phys,2*gsz)==0 && runtime_memory_granule_shared(phys+gsz) && w[0]==2); /* the rest goes */
    /* too many aliases */
    phys=alloc_direct(gsz);
    unsigned char *alias[17];
    for (int i=0;i<17;++i) { alias[i]=map_at(phys,gsz,3); alias[i][i]=(unsigned char)(i+1); }
    before=stats();
    CHECK(runtime_memory_promote(phys,gsz)==0 && stats().refused_busy==before.refused_busy+1 && !runtime_memory_granule_shared(phys));
    CHECK(unmap_direct(alias[16],gsz)==0);
    CHECK(runtime_memory_promote(phys,gsz)==1);
    for (int i=0;i<16;++i) for (int j=0;j<16;++j) CHECK(alias[j][i]==i+1);
    /* an allocator that cannot: stays CPU, nothing lost, nothing to release */
    phys=alloc_direct(2*gsz);
    unsigned char *f=map_at(phys,2*gsz,3);
    fill(f,2*gsz,4);
    fail_phys=phys+gsz;
    before=stats();
    const int releases=release_count;
    CHECK(runtime_memory_promote(phys,2*gsz)==0);
    CHECK(runtime_memory_granule_shared(phys) && !runtime_memory_granule_shared(phys+gsz));
    CHECK(stats().promotion_failures==before.promotion_failures+1 && release_count==releases && same(f,2*gsz,4,0));
    fail_phys=UINT64_MAX;
    CHECK(runtime_memory_promote(phys+gsz,1)==1 && same(f,2*gsz,4,0) && same(gpu_view(phys+gsz),gsz,4,gsz));
}

static void *promote_thread(void *arg) {
    const uint64_t *range=arg;
    return (void *)(uintptr_t)runtime_memory_promote(range[0],range[1]);
}
static void io_guard(void) {
    const uint64_t phys=alloc_direct(4*gsz);
    unsigned char *a=map_at(phys,4*gsz,3);
    fill(a,4*gsz,5);
    /* a promotion waits for a kernel write in flight */
    void *token=runtime_memory_io_begin((uintptr_t)a+100,4096);
    CHECK(token);
    uint64_t range[2]={phys,gsz};
    pthread_t t;
    RuntimeUmaStats before=stats();
    CHECK(pthread_create(&t,NULL,promote_thread,range)==0);
    usleep(20000);
    CHECK(!runtime_memory_granule_shared(phys));
    runtime_memory_io_end(token);
    void *result;
    CHECK(pthread_join(t,&result)==0 && result==(void *)1);
    CHECK(runtime_memory_granule_shared(phys) && stats().io_waits==before.io_waits+1 && same(a,4*gsz,5,0));
    /* ... one elsewhere is no reason to wait */
    token=runtime_memory_io_begin((uintptr_t)a+gsz+100,4096);
    before=stats();
    CHECK(token && runtime_memory_promote(phys+2*gsz,gsz)==1 && stats().io_waits==before.io_waits);
    runtime_memory_io_end(token);
    /* one that never ends: given up after a while, the granule handed back */
    token=runtime_memory_io_begin((uintptr_t)a+3*gsz+100,4096);
    before=stats();
    const int allocs=alloc_count, releases=release_count;
    const uint64_t t0=runtime_wait_clock();
    CHECK(runtime_memory_promote(phys+3*gsz,gsz)==0);
    const uint64_t waited=runtime_wait_clock()-t0;
    CHECK(waited>=40000000ull && waited<2000000000ull);
    CHECK(alloc_count==allocs+1 && release_count==releases+1 && last_release_phys==phys+3*gsz && gpu_fd[phys/gsz+3]<0);
    CHECK(!runtime_memory_granule_shared(phys+3*gsz) && stats().refused_busy==before.refused_busy+1 && same(a,4*gsz,5,0));
    runtime_memory_io_end(token);
    CHECK(runtime_memory_promote(phys+3*gsz,gsz)==1 && same(a,4*gsz,5,0));
    /* guarded kernel reads against promotions run in hammer() */
}

/* Pages come back with the protection they had: the GPU page manager's, the traps', the game's. */
static void protections(void) {
    const uint64_t phys=alloc_direct(gsz);
    unsigned char *p=map_at(phys,gsz,3);
    fill(p,gsz,6);
    enum { PAGES=26 };
    const uintptr_t at=(uintptr_t)p;
    runtime_memory_gpu_protect(at,4096,1,0);                               /* 0: read-only (page manager) */
    runtime_memory_gpu_protect(at+4096,4096,0,0);                          /* 1: no access */
    runtime_memory_trap(at+2*4096,4096,1,1);                               /* 2: trap, read-only */
    runtime_memory_trap(at+3*4096,4096,16,1);                              /* 3: trap, no access */
    runtime_memory_gpu_protect(at+4*4096,4096,1,0);                        /* 4: restricted, then freed */
    runtime_memory_gpu_protect(at+4*4096,4096,1,1);
    runtime_memory_trap(at+5*4096,4096,1,1);                               /* 5: trapped, then freed */
    runtime_memory_trap(at+5*4096,4096,1,0);
    runtime_memory_gpu_protect(at+6*4096,4096,1,0);                        /* 6: the page manager's, then a trap that ends */
    runtime_memory_trap(at+6*4096,4096,1,1);
    runtime_memory_trap(at+6*4096,4096,1,0);                               /* 7: untouched */
    runtime_memory_gpu_protect(at+2*PS4_PAGE,PS4_PAGE,1,0);                /* 8-11: restricted, then the game's mprotect: free */
    CHECK(guest_mprotect(p+2*PS4_PAGE,PS4_PAGE,3)==0);
    CHECK(guest_mprotect(p+3*PS4_PAGE,PS4_PAGE,1)==0);                     /* 12-15: the game's read-only */
    CHECK(guest_mprotect(p+4*PS4_PAGE,PS4_PAGE,0)==0);                     /* 16-19: the game's no access */
    runtime_memory_gpu_protect(at+5*PS4_PAGE,PS4_PAGE,1,0);                /* 20-23: both: read-only */
    CHECK(guest_mprotect(p+5*PS4_PAGE,PS4_PAGE,1)==0);
    runtime_memory_trap(at+24*4096,4096,1,1);                              /* 24: a trap, then the page manager lifts: free */
    runtime_memory_gpu_protect(at+24*4096,4096,1,1);
    runtime_memory_gpu_protect(at+25*4096,4096,1,0);                       /* 25: the page manager's, then a trap: read-only */
    runtime_memory_trap(at+25*4096,4096,1,1);
    static const unsigned char expect[PAGES][2]={ /* read faults, write faults */
        {0,1},{1,1},{0,1},{1,1},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
        {0,1},{0,1},{0,1},{0,1},{1,1},{1,1},{1,1},{1,1},{0,1},{0,1},{0,1},{0,1},{0,0},{0,1}};
    for (int i=0;i<PAGES;++i) CHECK(faults(p+i*4096,0)==expect[i][0] && faults(p+i*4096,1)==expect[i][1]);
    CHECK(runtime_memory_promote(phys,gsz)==1);
    for (int i=0;i<PAGES;++i) {
        CHECK(faults(p+i*4096,0)==expect[i][0]);
        CHECK(faults(p+i*4096,1)==expect[i][1]);
    }
    CHECK(same(p+32*4096,gsz-32*4096,6,32*4096) && same(p+4*4096,4*4096,6,4*4096));
    /* the restrictions still belong to their owners: lifting them works as before the promotion */
    runtime_memory_gpu_protect(at,2*4096,1,1);
    runtime_memory_trap(at+2*4096,4096,1,0);
    runtime_memory_trap(at+3*4096,4096,16,0);
    for (int i=0;i<4;++i) CHECK(!faults(p+i*4096,0) && !faults(p+i*4096,1));
    CHECK(same(p+32*4096,gsz-32*4096,6,32*4096) && same(p,4*4096,6,0));
    /* a fault on a page a freeze covered a moment ago (a thread preempted between its fault and the
     * handler) is retried once, then it is the fault of whoever asked: the read-only alias below fails,
     * after one retry; once the window has passed, without */
    const uint64_t ro_phys=alloc_direct(gsz);
    unsigned char *rw=map_at(ro_phys,gsz,3), *ro=map_at(ro_phys,gsz,1);
    rw[0]=1;
    CHECK(runtime_memory_promote(ro_phys,gsz)==1);
    int calls=atomic_load(&handler_calls);
    const uint64_t stalled=stats().freeze_faults;
    CHECK(faults(ro+32,1) && atomic_load(&handler_calls)==calls+2 && stats().freeze_faults==stalled);
    usleep(300000);
    calls=atomic_load(&handler_calls);
    CHECK(faults(ro+32,1) && atomic_load(&handler_calls)==calls+1);
}

/* ---- writers and kernel reads against promotions ---- */
typedef struct { unsigned char *base; uint64_t pages, first, step; volatile int *stop; uint64_t *expected; uint64_t writes; } Writer;
static void *writer(void *arg) {
    Writer *w=arg;
    for (uint64_t i=1;!*w->stop;++i) {
        const uint64_t page=w->first+(i%(w->pages/w->step))*w->step;
        /* one writer per page, counting: a write lost between the freeze and the remap would stay missing */
        volatile uint64_t *word=(volatile uint64_t *)(w->base+page*4096);
        *word=*word+1;
        ++w->expected[page];
        ++w->writes;
    }
    return NULL;
}
typedef struct { unsigned char *buffer; uint64_t size; int fd; volatile int *stop; uint64_t reads, short_reads; } Reader;
static void *reader(void *arg) {
    Reader *r=arg;
    for (uint64_t i=0;!*r->stop;++i) {
        unsigned char *at=r->buffer+(i*4096*3)%(r->size-65536);
        void *token=runtime_memory_io_begin((uintptr_t)at,65536);
        const ssize_t n=pread(r->fd,at,65536,0);
        runtime_memory_io_end(token);
        ++r->reads;
        if (n!=65536) { ++r->short_reads; fprintf(stderr,"read: %zd (errno %d)\n",n,errno); }
        else if (!same(at,65536,8,0)) { ++r->short_reads; fputs("read: wrong data\n",stderr); }
        usleep(30);
    }
    return NULL;
}
static void hammer(void) {
    const uint64_t half=gsz*4>4*MIB ? gsz*4 : 4*MIB, region=2*half, phys=alloc_direct(region);
    unsigned char *a=map_at(phys,region,3), *b=map_at(phys,region,3);
    const uint64_t pages=half/4096;
    uint64_t *expected=calloc(region/4096,sizeof(*expected));
    CHECK(expected);
    for (uint64_t p=0;p<pages;++p) *(uint64_t *)(a+p*4096)=0;
    char path[]="/tmp/uma-test-XXXXXX";
    const int fd=mkstemp(path);
    CHECK(fd>=0);
    unlink(path);
    unsigned char *file=malloc(65536);
    fill(file,65536,8);
    CHECK(write(fd,file,65536)==65536);
    free(file);
    volatile int stop=0;
    /* two writers, one through each alias, on the pages of the first half; the second half takes kernel reads */
    Writer w0={a,pages,0,2,&stop,expected,0}, w1={b,pages,1,2,&stop,expected,0};
    Reader r={a+half,half,fd,&stop,0,0};
    pthread_t t0,t1,t2;
    CHECK(pthread_create(&t0,NULL,writer,&w0)==0 && pthread_create(&t1,NULL,writer,&w1)==0 && pthread_create(&t2,NULL,reader,&r)==0);
    usleep(20000);
    const RuntimeUmaStats before=stats();
    int failed=0;
    for (uint64_t at=0;at<region;at+=gsz) {
        if (runtime_memory_promote(phys+at,gsz)!=1) ++failed;
        usleep(300);
    }
    usleep(20000);
    stop=1;
    CHECK(pthread_join(t0,NULL)==0 && pthread_join(t1,NULL)==0 && pthread_join(t2,NULL)==0);
    const RuntimeUmaStats after=stats();
    CHECK(!failed && after.promotions==before.promotions+region/gsz);
    CHECK(r.short_reads==0 && r.reads>0);
    uint64_t lost=0;
    for (uint64_t p=0;p<pages;++p) {
        if (!expected[p]) continue;
        const uint64_t v=*(uint64_t *)(a+p*4096);
        if (v!=expected[p] || *(uint64_t *)(b+p*4096)!=v) ++lost;
        unsigned char *backing=runtime_memory_backing_pointer(phys)+p*4096;
        if (*(uint64_t *)backing!=v || *(uint64_t *)gpu_view(phys+p*4096)!=v) ++lost;
    }
    fprintf(stderr,"uma test: %" PRIu64 " + %" PRIu64 " writes, %" PRIu64 " kernel reads, %" PRIu64 " promotions (%" PRIu64
           " stalled writers, %" PRIu64 " io waits), frozen %.3f ms each, %.2f ms longest, %" PRIu64 " lost writes\n",
           w0.writes,w1.writes,r.reads,after.promotions-before.promotions,after.freeze_faults-before.freeze_faults,
           after.io_waits-before.io_waits,(double)(after.promote_ns_total-before.promote_ns_total)/1e6/(double)(after.promotions-before.promotions),
           (double)after.promote_ns_max/1e6,lost);
    CHECK(lost==0 && w0.writes>1000 && w1.writes>1000);
    free(expected);
    close(fd);
}

/* Protections changing and mappings coming and going while granules are promoted: every page ends in the
 * state of the last operation on it. States: 0 free, 1 read-only, 2 no access (what the GPU page manager
 * and the traps make of them; the model is "the last one applied counts", as in the runtime). */
#define CHAOS_PAGES 32 /* per granule */
typedef struct { unsigned char *base; uint64_t granules; volatile int *stop; unsigned char *state, *trap_on; uint64_t ops; } Chaos;
static uint64_t chaos_offset(uint64_t page) { return (page/CHAOS_PAGES)*gsz+(page%CHAOS_PAGES)*4096; }
static void *chaos_protect(void *arg) {
    Chaos *c=arg;
    uint64_t x=88172645463325252ull;
    while (!*c->stop) {
        x^=x<<13; x^=x>>7; x^=x<<17;
        const uint64_t page=x%(c->granules*CHAOS_PAGES);
        const uintptr_t at=(uintptr_t)c->base+chaos_offset(page);
        switch ((x>>32)%5) {
        case 0: runtime_memory_gpu_protect(at,4096,1,1); c->state[page]=0; break;
        case 1: runtime_memory_gpu_protect(at,4096,1,0); c->state[page]=1; break;
        case 2: runtime_memory_gpu_protect(at,4096,0,0); c->state[page]=2; break;
        case 3: if (!c->trap_on[page]) { runtime_memory_trap(at,4096,1,1); c->trap_on[page]=1; c->state[page]=1; } break;
        default: if (c->trap_on[page]) { runtime_memory_trap(at,4096,1,0); c->trap_on[page]=0; c->state[page]=0; }
        }
        ++c->ops;
    }
    return NULL;
}
typedef struct { uint64_t phys, granules; volatile int *stop; unsigned char *expected; uint64_t maps; } Mapper;
static void *chaos_mapper(void *arg) {
    Mapper *m=arg;
    uint64_t x=0x9e3779b97f4a7c15ull;
    while (!*m->stop) {
        x^=x<<13; x^=x>>7; x^=x<<17;
        const uint64_t g=x%m->granules;
        void *p=NULL;
        CHECK(map_direct(&p,gsz,3,0,(int64_t)(m->phys+g*gsz),0)==0);
        unsigned char *byte=(unsigned char *)p+CHAOS_PAGES*4096+8;
        *byte=(unsigned char)(x>>40);
        m->expected[g]=*byte;
        CHECK(unmap_direct(p,gsz)==0);
        ++m->maps;
    }
    return NULL;
}
static void chaos(void) {
    const uint64_t granules=64, phys=alloc_direct(granules*gsz);
    unsigned char *a=map_at(phys,granules*gsz,3);
    unsigned char *state=calloc(granules*CHAOS_PAGES,1), *trap_on=calloc(granules*CHAOS_PAGES,1), *expected=calloc(granules,1);
    CHECK(state && trap_on && expected);
    for (uint64_t g=0;g<granules;++g) a[g*gsz+CHAOS_PAGES*4096+8]=0;
    volatile int stop=0;
    Chaos c={a,granules,&stop,state,trap_on,0};
    Mapper m={phys,granules,&stop,expected,0};
    pthread_t t0,t1;
    CHECK(pthread_create(&t0,NULL,chaos_protect,&c)==0 && pthread_create(&t1,NULL,chaos_mapper,&m)==0);
    const RuntimeUmaStats before=stats();
    for (uint64_t g=0;g<granules;++g) {
        usleep(200+(unsigned)(g*37)%400);
        CHECK(runtime_memory_promote(phys+g*gsz,gsz)==1);
    }
    usleep(5000);
    stop=1;
    CHECK(pthread_join(t0,NULL)==0 && pthread_join(t1,NULL)==0);
    CHECK(stats().promotions==before.promotions+granules);
    for (uint64_t page=0;page<granules*CHAOS_PAGES;++page) {
        const unsigned char *at=a+chaos_offset(page);
        CHECK(faults(at,0)==(state[page]==2) && faults(at,1)==(state[page]!=0));
    }
    for (uint64_t g=0;g<granules;++g) {
        const unsigned char *byte=a+g*gsz+CHAOS_PAGES*4096+8;
        CHECK(*byte==expected[g] && *gpu_view(phys+g*gsz+CHAOS_PAGES*4096+8)==expected[g]);
    }
    fprintf(stderr,"uma test: chaos: %" PRIu64 " protection changes, %" PRIu64 " alias maps against %" PRIu64 " promotions\n",c.ops,m.maps,granules);
    free(state); free(trap_on); free(expected);
}

int main(int argc, char **argv) {
    gsz=(argc>1 ? strtoull(argv[1],NULL,10) : 256)<<10;
    if (!freopen("/dev/null","w",stdout)) return 1; /* the runtime logs every map; the test reports on stderr */
    struct sigaction sa={0};
    sa.sa_sigaction=on_segv; sa.sa_flags=SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV,&sa,NULL);
    allocate_direct=(Allocate)runtime_memory_resolve("rTXw65xmLIA#p#J");
    map_direct=(Map)runtime_memory_resolve("L-Q3LEjIbgA#p#J");
    unmap_direct=(Unmap)runtime_memory_resolve("cQke9UuBQOk#p#J");
    release_direct=(Release)runtime_memory_resolve("MBuItvba6z8#p#J");
    guest_mprotect=(Protect)runtime_memory_resolve("sceKernelMprotect");
    CHECK(allocate_direct && map_direct && unmap_direct && release_direct && guest_mprotect);
    for (int i=0;i<MAX_GRANULES;++i) gpu_fd[i]=-1;
    CHECK(gsz>=256<<10 && gsz<=2*MIB);
    off_by_default();
    /* memory the game mapped before the GPU library registered is promoted like any other */
    int64_t early=-1;
    void *early_map=NULL;
    CHECK(allocate_direct(0,(int64_t)(8192*MIB),gsz,gsz,3,&early)==0 && early==0);
    next_low=gsz;
    CHECK(map_direct(&early_map,gsz,3,0,early,0)==0);
    memset(early_map,0x42,4096);
    CHECK(runtime_memory_set_granule_allocator(fake_alloc,fake_release,gsz)==0);
    CHECK(runtime_memory_set_granule_allocator(fake_alloc,fake_release,gsz)==-1); /* once */
    CHECK(stats().granule_bytes==gsz && alloc_count==0);
    CHECK(runtime_memory_promote(0,gsz)==1);
    CHECK(((unsigned char *)early_map)[100]==0x42 && ((unsigned char *)early_map)[4095]==0x42 && ((unsigned char *)early_map)[4096]==0 &&
          gpu_view(0)[4095]==0x42);
    lazy_and_promotion();
    refusals_and_failures();
    io_guard();
    protections();
    hammer();
    chaos();
    RuntimeUmaStats s=stats();
    fprintf(stderr,"uma test: ok (granule %" PRIu64 " KiB: %" PRIu64 " promotions, %" PRIu64 " failures, %" PRIu64 " exec / %" PRIu64 " busy refusals)\n",
           gsz>>10,s.promotions,s.promotion_failures,s.refused_exec,s.refused_busy);
    return 0;
}
