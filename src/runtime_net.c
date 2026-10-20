/* Online play (BB_ONLINE=1): the game's network and PSN imports go to the online module
 * (gpu/bbnet20: libbbnet20.so beside libbbgpu.so), loaded here on the first such import. Offline,
 * or when the module is missing or fails to start, they stay with the runtime's offline
 * versions (runtime_services.c). Trophies, voice and the store/profile dialogs always stay here. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbnet/bbnet.h"
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char game_folder[PATH_MAX], game_serial[16], game_title[128], game_version[16];

void runtime_net_configure(const char *app0, const char *serial, const char *title, const char *version) {
    snprintf(game_folder,sizeof(game_folder),"%s",app0 ? app0 : "");
    snprintf(game_serial,sizeof(game_serial),"%s",serial ? serial : "");
    snprintf(game_title,sizeof(game_title),"%s",title ? title : "");
    snprintf(game_version,sizeof(game_version),"%s",version ? version : "");
}

int runtime_net_enabled(void) {
    static int enabled=-1;
    if (enabled<0) { const char *v=getenv("BB_ONLINE"); enabled=v && v[0]=='1'; }
    return enabled;
}

/* Imports the module answers: network, HTTP/SSL and NP, except what stays in the runtime. */
int runtime_net_symbol(const char *symbol) {
    static const char *const online[]={"sceNet","sceNp","sceHttp","sceSsl"};
    static const char *const local[]={"sceNpTrophy","sceNpCommerce","sceNpProfileDialog"};
    if (!symbol) return 0;
    int match=0;
    for (size_t i=0;i<sizeof(online)/sizeof(*online);++i)
        if (!strncmp(symbol,online[i],strlen(online[i]))) match=1;
    for (size_t i=0;i<sizeof(local)/sizeof(*local);++i)
        if (!strncmp(symbol,local[i],strlen(local[i]))) match=0;
    return match;
}

static void *module_handle;

/* The game's image is ready (probe.c): the module's hooks in it go in now. */
void runtime_net_image_ready(uintptr_t base, uint64_t size) {
    if (!module_handle) return;
    const BbNetImageReadyFn ready=(BbNetImageReadyFn)dlsym(module_handle,"bbnet_image_ready");
    if (ready) ready(base,size);
}

/* The game's image is mapped and relocated, its code not yet patched (probe.c). */
void runtime_net_image_mapped(uint8_t *image, uint64_t size) {
    if (!module_handle) return;
    const BbNetImageMappedFn mapped=(BbNetImageMappedFn)dlsym(module_handle,"bbnet_image_mapped");
    if (mapped) mapped(image,size);
}

/* The game is loaded and about to run (probe.c): online sessions start. */
void runtime_net_start(void) {
    if (!module_handle) return;
    const BbNetStartFn start=(BbNetStartFn)dlsym(module_handle,"bbnet_start");
    if (start) start();
}

/* The module: BB_NET_MODULE, else libbbnet20.so where libbbgpu.so is: lib/ beside bin/ (the Android runtime
 * layout, bionic/bin/bb-probe + bionic/lib/), gpu/ or the executable's own folder (the PC build tree). */
static BbNetResolveFn module_resolve(void) {
    static int tried;
    static BbNetResolveFn resolve;
    if (tried) return resolve;
    tried=1;
    char path[PATH_MAX];
    const char *forced=getenv("BB_NET_MODULE");
    if (forced && forced[0]) {
        snprintf(path,sizeof(path),"%s",forced);
    } else {
        char exe[PATH_MAX-64];
        const ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
        if (n<=0) return NULL;
        exe[n]=0;
        char *slash=strrchr(exe,'/');
        if (slash) *slash=0;
        static const char *const folders[]={"../lib","gpu","."};
        path[0]=0;
        for (size_t i=0;i<sizeof(folders)/sizeof(*folders);++i) {
            snprintf(path,sizeof(path),"%s/%s/libbbnet20.so",exe,folders[i]);
            if (access(path,R_OK)==0) break;
        }
    }
    /* RTLD_LOCAL: its C++ (network and NP libraries) stays out of the process's symbol scope. */
    void *module=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if (!module) {
        printf("Online: the online module is not available (%s): offline play\n",dlerror());
        return NULL;
    }
    module_handle=module;
    const BbNetInitFn init=(BbNetInitFn)dlsym(module,"bbnet_init");
    const BbNetResolveFn found=(BbNetResolveFn)dlsym(module,"bbnet_resolve");
    const BbNetHost host={
        .abi=BBNET_ABI_VERSION, .app0=game_folder, .serial=game_serial, .title=game_title,
        .app_ver=game_version, .thread_spawn=runtime_thread_spawn, .thread_spawn_guest=runtime_thread_spawn_guest,
        .thread_join=runtime_thread_join_spawned, .thread_attach=runtime_thread_attach_host,
        .thread_exit=runtime_thread_exit, .memory_is_mapped=runtime_memory_is_mapped,
    };
    if (!init || !found || init(&host)!=0) {
        printf("Online: %s did not start: offline play\n",path);
        return NULL;
    }
    printf("Online: module %s\n",path);
    resolve=found;
    return resolve;
}

uintptr_t runtime_net_resolve(const char *name) {
    if (!runtime_net_enabled()) return 0;
    const char *symbol=runtime_symbol(name);
    if (!runtime_net_symbol(symbol)) return 0;
    const BbNetResolveFn resolve=module_resolve();
    const uintptr_t address=resolve ? resolve(name) : 0;
    if (resolve && !address) printf("Online: %s has no online version; the offline one is used\n",symbol);
    return address;
}
