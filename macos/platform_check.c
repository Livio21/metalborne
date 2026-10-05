#include "runtime.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>

void runtime_restart(void) { abort(); }
static void *main_tcb;
static void *read_tcb(void) {
    void *tcb;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(tcb));
    return tcb;
}
static void *worker(void *unused) {
    (void)unused;
    runtime_thread_attach_host("macOS check");
    void *tcb=read_tcb();
    assert(tcb && tcb!=main_tcb && *(void **)tcb==tcb);
    assert((uintptr_t)tcb<UINT64_C(0x10000000000));
    pthread_key_t key;
    assert(pthread_key_create(&key,NULL)==0);
    assert(pthread_setspecific(key,tcb)==0 && pthread_getspecific(key)==tcb);
    assert(pthread_key_delete(key)==0);
    return NULL;
}
int main(void) {
    unsigned char *object=calloc(1,128);
    assert(object && (uintptr_t)object<UINT64_C(0x10000000000));
    assert(!((uintptr_t)object&63));
    object[127]=42;
    object=realloc(object,8192);
    assert(object && object[127]==42);
    free(object);
    errno=0;
    assert(!calloc(SIZE_MAX,2) && errno==ENOMEM);
    runtime_thread_attach_main();
    main_tcb=read_tcb();
    assert(main_tcb && *(void **)main_tcb==main_tcb);
    pthread_t thread;
    for (int i=0;i<16;++i) {
        assert(pthread_create(&thread,NULL,worker,NULL)==0);
        assert(pthread_join(thread,NULL)==0);
        assert(read_tcb()==main_tcb);
    }
    unsigned char *mapping=runtime_low_map(16384,PROT_READ|PROT_WRITE);
    assert(mapping); mapping[0]=123;
    assert(bb_macos_map_no_replace((uintptr_t)mapping,16384,PROT_READ|PROT_WRITE)==MAP_FAILED);
    assert(mapping[0]==123);
    puts("macOS/Rosetta: low heap, host TLS, guest FS TLS, thread isolation and non-overwriting maps PASS");
    return 0;
}
