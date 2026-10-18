/* Textures homed in the GPU (runtime_memory.c, BB_TEX_HOME; docs/ANDROID_UNIFIED_MEMORY_PLAN.md "Plan A"):
 * the runtime side, with this test standing in for the GPU library (it keeps the bytes a texture would
 * be re-tiled into, and restores them from its fault handler and the home hook).
 *
 * Standalone: needs only runtime_memory.c (stubs for the rest of the runtime below).
 *   cc -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_tex_home.c src/runtime_memory.c \
 *      -o out/tex-home-test && out/tex-home-test */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
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
unsigned runtime_memory_trap_reasons(uintptr_t address);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size);
void *runtime_memory_backing_pointer(uint64_t phys);
void runtime_memory_set_prepare_write_hook(void (*hook)(uintptr_t, uint64_t));
void runtime_memory_set_page_protected(uintptr_t address, uint64_t size, int is_protected);

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr,"%s:%d: CHECK failed: %s\n",__FILE__,__LINE__,#cond); abort(); } } while (0)
typedef int32_t (ABI *Allocate)(int64_t, int64_t, uint64_t, uint64_t, int, int64_t *);
typedef int32_t (ABI *Map)(void **, uint64_t, int, int, int64_t, uint64_t);
typedef int32_t (ABI *Unmap)(void *, uint64_t);
typedef int32_t (ABI *Release)(uint64_t, uint64_t);
typedef int32_t (ABI *Protect)(const void *, uint64_t, int);
static Allocate allocate_direct; static Map map_direct; static Unmap unmap_direct; static Release release_direct;
static Protect guest_mprotect;
#define KIB UINT64_C(1024)
#define MIB (UINT64_C(1)<<20)

/* ---- the stand-in GPU library: one texture's bytes per ticket ---- */
#define MAX_HOMES 16
static struct { uint32_t ticket; const unsigned char *bytes; uint64_t size; } textures[MAX_HOMES];
static _Atomic int restores_by_fault, restores_by_hook;
static int rebuild(uint32_t ticket) {
    for (int i=0;i<MAX_HOMES;++i)
        if (textures[i].ticket==ticket) {
            const int r=runtime_memory_home_restore(ticket,textures[i].bytes,textures[i].size);
            textures[i].ticket=0;
            return r>=-1;
        }
    return runtime_memory_home_restore(ticket,NULL,0)>=-1; /* unknown: forgotten */
}
static int hook(uint32_t ticket) { atomic_fetch_add(&restores_by_hook,1); return rebuild(ticket); }
static void keep(uint32_t ticket, const unsigned char *bytes, uint64_t size) {
    for (int i=0;i<MAX_HOMES;++i) if (!textures[i].ticket) { textures[i].ticket=ticket; textures[i].bytes=bytes; textures[i].size=size; return; }
    CHECK(!"too many homes");
}

/* ---- faults ---- */
static __thread sigjmp_buf *probe;
static _Atomic int rebuild_in_handler; /* the handler acts as the GPU library's (rebuilds) */
static void on_segv(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)context;
    const uintptr_t address=(uintptr_t)info->si_addr;
    if (atomic_load(&rebuild_in_handler) && (runtime_memory_trap_reasons(address) & RUNTIME_HOME_TRAP)) {
        const uint32_t ticket=runtime_memory_home_ticket(address);
        if (!ticket) { if (runtime_memory_home_unstale(address)) return; }
        else { atomic_fetch_add(&restores_by_fault,1); if (rebuild(ticket)) return; }
    }
    if (probe) siglongjmp(*probe,1);
    static const char message[]="tex home test: unexpected fault\n";
    ssize_t w=write(2,message,sizeof(message)-1); (void)w;
    _exit(99);
}
/* 1 if the access faults. */
static int faults(const volatile unsigned char *p, int write) {
    sigjmp_buf jump;
    probe=&jump;
    if (sigsetjmp(jump,1)) { probe=NULL; return 1; }
    if (write) { const unsigned char v=*p; *(volatile unsigned char *)p=v; } else (void)*p;
    probe=NULL;
    return 0;
}

/* ---- helpers ---- */
static uint64_t next_low;
static uint64_t alloc_direct(uint64_t size) {
    int64_t phys=-1;
    CHECK(allocate_direct((int64_t)next_low,(int64_t)(4096*MIB),size,64*KIB,3,&phys)==0);
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
static int resident(const void *p) {
    unsigned char v=0;
    CHECK(mincore((void *)((uintptr_t)p & ~(uintptr_t)4095),4096,&v)==0);
    return v & 1;
}
static RuntimeHomeStats stats(void) { RuntimeHomeStats s; runtime_memory_home_stats(&s); return s; }
static void prepare_hook(uintptr_t address, uint64_t size) { (void)address; (void)size; }

int main(void) {
    if (!freopen("/dev/null","w",stdout)) return 1; /* the runtime logs every map; the test reports on stderr */
    struct sigaction sa={0};
    sa.sa_sigaction=on_segv; sa.sa_flags=SA_SIGINFO|SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV,&sa,NULL);
    allocate_direct=(Allocate)runtime_memory_resolve("rTXw65xmLIA#p#J");
    map_direct=(Map)runtime_memory_resolve("L-Q3LEjIbgA#p#J");
    unmap_direct=(Unmap)runtime_memory_resolve("cQke9UuBQOk#p#J");
    release_direct=(Release)runtime_memory_resolve("MBuItvba6z8#p#J");
    guest_mprotect=(Protect)runtime_memory_resolve("sceKernelMprotect");
    CHECK(allocate_direct && map_direct && unmap_direct && release_direct && guest_mprotect);
    runtime_memory_set_prepare_write_hook(prepare_hook); /* the page manager's protections map (BB_FAULT_PREPARE) */
    const uint64_t hp=runtime_memory_home_granule();
    CHECK(hp>=4096 && !(hp & (hp-1)));
    CHECK(runtime_memory_homed_pages()==0 && runtime_memory_home_touches(0x1000000000,MIB)==0);

    const uint64_t region=MIB, phys=alloc_direct(region);
    unsigned char *a=map_at(phys,region,3), *b=map_at(phys,region,1);
    fill(a,region,1);
    const uint64_t at=16*KIB, size=256*KIB; /* the "texture's" whole pages */
    unsigned char *texture=malloc(size);
    memcpy(texture,a+at,size);

    /* refusals: alignment, bytes that differ (nothing changes), not direct memory */
    uint32_t ticket=0;
    CHECK(runtime_memory_home((uintptr_t)a+at+1,size,texture,&ticket)==RUNTIME_HOME_ARGS);
    texture[100]^=1;
    CHECK(runtime_memory_home((uintptr_t)a+at,size,texture,&ticket)==RUNTIME_HOME_MISMATCH);
    texture[100]^=1;
    CHECK(!faults(a+at,1) && !faults(b+at,0) && same(a,region,1,0) && runtime_memory_homed_pages()==0);
    CHECK(stats().refused[RUNTIME_HOME_MISMATCH]==1);
    typedef int32_t (ABI *MapFlexible)(void **, uint64_t, int, int);
    const MapFlexible map_flexible=(MapFlexible)runtime_memory_resolve("sceKernelMapFlexibleMemory");
    void *flexible=NULL;
    CHECK(map_flexible && map_flexible(&flexible,size,3,0)==0);
    memcpy(flexible,texture,size);
    CHECK(runtime_memory_home((uintptr_t)flexible,size,texture,&ticket)==RUNTIME_HOME_MAPPING); /* flexible memory */
    CHECK(runtime_memory_home((uintptr_t)a+region-hp,2*hp,texture,&ticket)==RUNTIME_HOME_MAPPING); /* past the mapping */

    /* homed: no access through any alias, the memfd pages gone, the neighbours as before */
    CHECK(runtime_memory_home((uintptr_t)a+at,size,texture,&ticket)==RUNTIME_HOME_OK && ticket);
    keep(ticket,texture,size);
    CHECK(runtime_memory_homed_pages()==size/4096 && runtime_memory_home_alive(ticket)==size/4096);
    CHECK(faults(a+at,0) && faults(a+at+size-1,0) && faults(b+at+4096,0));
    CHECK(!faults(a+at-1,1) && !faults(a+at+size,1) && !faults(b,0));
    CHECK(!resident((unsigned char *)runtime_memory_backing_pointer(phys+at)+8192));
    CHECK(runtime_memory_home_touches((uintptr_t)a,region) && runtime_memory_home_touches((uintptr_t)b+at+size-1,1));
    CHECK(!runtime_memory_home_touches((uintptr_t)a,at) && !runtime_memory_home_touches((uintptr_t)a+at+size,4096));
    CHECK(runtime_memory_home_ticket((uintptr_t)b+at+5)==ticket && runtime_memory_home_ticket((uintptr_t)b)==0);
    CHECK(runtime_memory_home((uintptr_t)a+at,hp,texture,&ticket)==RUNTIME_HOME_HOMED);
    uint32_t found[4];
    CHECK(runtime_memory_home_tickets((uintptr_t)a,region,found,4)==1 && found[0]==ticket);
    /* the page manager's protection and the game's mprotect leave them without access */
    runtime_memory_gpu_protect((uintptr_t)a,region,1,1);
    CHECK(faults(a+at,0) && !faults(a,1));
    CHECK(guest_mprotect(a,region,3)==0 && faults(a+at+8192,0) && !faults(a+at+size,1));
    /* other trap reasons come and go: still no access */
    runtime_memory_trap((uintptr_t)a+at,8192,2,1);
    runtime_memory_trap((uintptr_t)a+at,8192,2,0);
    CHECK(faults(a+at,0) && faults(a+at+4096,0));
    /* a new mapping of the pages gets the trap */
    unsigned char *c=map_at(phys,region,3);
    CHECK(faults(c+at,0) && !faults(c,1) && runtime_memory_home_ticket((uintptr_t)c+at)==ticket);

    /* the runtime's own reads through the backing view rebuild them first (home hook) */
    runtime_memory_set_home_hook(hook);
    unsigned char got[64];
    runtime_memory_read_backing((uintptr_t)a+at+1000,got,sizeof(got));
    CHECK(restores_by_hook==1 && same(got,sizeof(got),1,at+1000));
    CHECK(runtime_memory_homed_pages()==0 && runtime_memory_home_alive(ticket)==0);
    CHECK(!faults(a+at,1) && !faults(c+at+size-1,1) && !faults(b+at,0) && faults(b+at,1)); /* b stays read-only */
    CHECK(same(a,region,1,0) && same(c,region,1,0));
    CHECK(runtime_memory_home_restore(ticket,texture,size)==-1); /* gone */
    CHECK(!runtime_memory_home_touches((uintptr_t)a,region));
    RuntimeHomeStats s=stats();
    CHECK(s.homes==1 && s.restores==1 && s.restored_pages==size/4096 && s.homed_pages==0);

    /* a page the GPU page manager write-protects stays read-only when its trap goes */
    CHECK(runtime_memory_home((uintptr_t)a+at,size,texture,&ticket)==RUNTIME_HOME_OK);
    keep(ticket,texture,size);
    runtime_memory_set_page_protected((uintptr_t)a+at+4096,4096,1);
    runtime_memory_set_page_protected((uintptr_t)c+at+4096,4096,1);
    /* a guest write: the fault handler (here: the GPU library's) rebuilds, the write is retried */
    atomic_store(&rebuild_in_handler,1);
    a[at+20]=0x5a;
    CHECK(restores_by_fault==1 && a[at+20]==0x5a && c[at+20]==0x5a && same(a+at+21,size-21,1,at+21));
    atomic_store(&rebuild_in_handler,0);
    CHECK(faults(a+at+4096,1) && !faults(a+at+4096,0) && !faults(a+at,1) && !faults(a+at+8192,1));
    runtime_memory_set_page_protected((uintptr_t)a+at+4096,4096,0);
    runtime_memory_set_page_protected((uintptr_t)c+at+4096,4096,0);
    runtime_memory_gpu_protect((uintptr_t)a+at+4096,4096,1,1);
    a[at+20]=pattern(at+20,1);
    memcpy(texture,a+at,size);

    /* a write through the backing view (a GPU download) waits for the rebuild: its bytes are not lost */
    CHECK(runtime_memory_home((uintptr_t)a+at,size,texture,&ticket)==RUNTIME_HOME_OK);
    keep(ticket,texture,size);
    const unsigned char v=0xee;
    CHECK(runtime_memory_write_backing((uintptr_t)c+at+300,&v,1)==1);
    CHECK(a[at+300]==0xee && a[at+301]==pattern(at+301,1) && runtime_memory_homed_pages()==0);
    a[at+300]=pattern(at+300,1);
    memcpy(texture,a+at,size);

    /* released memory: the home is forgotten without a rebuild */
    CHECK(runtime_memory_home((uintptr_t)a+at,size,texture,&ticket)==RUNTIME_HOME_OK);
    keep(ticket,texture,size);
    const int hooks=restores_by_hook;
    CHECK(unmap_direct(c,region)==0 && unmap_direct(b,region)==0);
    CHECK(runtime_memory_home_alive(ticket)==size/4096); /* unmapping an alias keeps the home */
    CHECK(release_direct(phys,region)==0);
    CHECK(runtime_memory_home_alive(ticket)==0 && runtime_memory_homed_pages()==0 && restores_by_hook==hooks);
    CHECK(runtime_memory_home_restore(ticket,texture,size)==-1);
    CHECK(stats().released_pages==size/4096);
    for (int i=0;i<MAX_HOMES;++i) textures[i].ticket=0;

    /* a partial release: the rest stays homed and comes back with its bytes */
    const uint64_t phys2=alloc_direct(region);
    unsigned char *d=map_at(phys2,region,3);
    fill(d,region,2);
    memcpy(texture,d+at,size);
    CHECK(runtime_memory_home((uintptr_t)d+at,size,texture,&ticket)==RUNTIME_HOME_OK);
    CHECK(unmap_direct(d,region)==0);
    CHECK(release_direct(phys2,at+64*KIB)==0); /* the first 64 KiB of the home go */
    CHECK(runtime_memory_home_alive(ticket)==(size-64*KIB)/4096);
    unsigned char *e=map_at(phys2+at+64*KIB,region-at-64*KIB,3);
    CHECK(faults(e,0) && runtime_memory_home_restore(ticket,texture,size)==(int)((size-64*KIB)/4096));
    CHECK(same(e,size-64*KIB,2,at+64*KIB) && same(e+size-64*KIB,64*KIB,2,at+size));

    /* UMA: a granule with homed pages is not promoted, a shared one is not homed (own process) */
    const pid_t child=fork();
    CHECK(child>=0);
    if (!child) {
        static int fds;
        int alloc(uint64_t p, uint64_t n);
        CHECK(runtime_memory_set_granule_allocator(alloc,NULL,256*KIB)==0);
        const uint64_t phys3=alloc_direct(MIB);
        unsigned char *f=map_at(phys3,MIB,3);
        fill(f,MIB,3);
        memcpy(texture,f,size);
        CHECK(runtime_memory_home((uintptr_t)f,size,texture,&ticket)==RUNTIME_HOME_OK);
        CHECK(runtime_memory_promote(phys3,4096)==0 && stats().refused_promotions==1);
        CHECK(runtime_memory_home_restore(ticket,texture,size)==(int)(size/4096));
        CHECK(runtime_memory_promote(phys3,4096)==1 && runtime_memory_granule_shared(phys3));
        CHECK(same(f,MIB,3,0));
        CHECK(runtime_memory_home((uintptr_t)f,size,f+0,&ticket)==RUNTIME_HOME_SHARED);
        (void)fds;
        _exit(0);
    }
    int status=0;
    CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    fprintf(stderr,"tex home test: ok (host page %llu bytes)\n",(unsigned long long)hp);
    return 0;
}
int alloc(uint64_t p, uint64_t n) {
    (void)p;
    const int fd=memfd_create("fake-dmabuf",MFD_CLOEXEC);
    CHECK(fd>=0 && !ftruncate(fd,(off_t)n));
    return fd;
}
