/* Threads for the online module's host code (runtime_thread_spawn and friends, src/runtime_thread.c): a host
 * entry runs as a host function on a thread that has a guest TCB, ends by returning or by
 * runtime_thread_exit, and joins with its result; attaching an unknown host thread names it once. */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

/* What runtime_thread.c takes from the rest of the runtime. */
void *runtime_low_map(size_t size, int prot) {
    void *p=mmap(NULL,size,prot,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    return p==MAP_FAILED ? NULL : p;
}
void runtime_thread_keys_cleanup(void) {}
void bbgpu_adpf_thread_named(const char *name) { (void)name; }

static volatile int ran;
static void *returns(void *argument) {
    ran=1;
    return (char *)argument+1;
}
static void *exits(void *argument) {
    (void)argument;
    runtime_thread_exit((void *)(uintptr_t)42);
}
/* Many threads at once, each with a small stack the way the game asks for its NP threads. */
static void *deep(void *argument) {
    char buffer[512*1024];
    memset(buffer,(int)(uintptr_t)argument,sizeof(buffer)); /* a host stack of 128 KiB would not hold this */
    return (void *)(uintptr_t)buffer[sizeof(buffer)-1];
}
static void *attached_name(void *argument) {
    (void)argument;
    runtime_thread_attach_host("later"); /* already a guest thread: keeps its name */
    return NULL;
}

int main(void) {
    void *thread=NULL, *result=NULL;
    assert(runtime_thread_spawn(&thread,returns,(void *)100,0,"test:return")==0);
    assert(thread && runtime_thread_join_spawned(thread,&result)==0);
    assert(ran==1 && result==(void *)101);

    assert(runtime_thread_spawn(&thread,exits,NULL,64*1024,"test:exit")==0);
    assert(runtime_thread_join_spawned(thread,&result)==0 && result==(void *)42);

    assert(runtime_thread_spawn(&thread,deep,(void *)7,128*1024,"test:deep")==0);
    assert(runtime_thread_join_spawned(thread,&result)==0 && result==(void *)7);

    assert(runtime_thread_spawn(&thread,attached_name,NULL,0,"test:attach")==0);
    assert(runtime_thread_join_spawned(thread,&result)==0);

    /* A bad thread handle is an error, not a crash. */
    assert(runtime_thread_join_spawned((void *)8,&result)!=0);
    printf("PASS: host entries run as threads with a guest TCB, exit, return and join\n");
    return 0;
}
