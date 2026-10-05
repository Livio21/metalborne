// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef __APPLE__
#define BB_PLATFORM_ALLOCATOR_IMPL
#include "platform_macos.h"
#include <architecture/i386/table.h>
#include <i386/user_ldt.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

/* macOS TLS layout follows shadPS4 src/core/tls.cpp (GPL-2.0-or-later).
 * Darwin owns GS. FS selects a low LDT page whose first word is the guest TCB. */
__asm__(".zerofill TCB_SPACE,TCB_SPACE,__bb_tcb_space,0x3FC000\n"
        ".no_dead_strip __bb_tcb_space");
enum { TCB_BASE=0x4000, TCB_SPAN=0x3FC000, TCB_PAGE=0x1000, TCB_SLOTS=1019 };
typedef struct { void *tcb; unsigned short index; } TcbPage;
static pthread_once_t tcb_once=PTHREAD_ONCE_INIT;
static pthread_key_t tcb_key;
static pthread_mutex_t tcb_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char tcb_used[TCB_SLOTS];

static void tcb_release(void *raw) {
    TcbPage *page=raw;
    pthread_mutex_lock(&tcb_lock);
    tcb_used[page->index-8]=0;
    pthread_mutex_unlock(&tcb_lock);
}
static void tcb_init(void) {
    if (mprotect((void *)(uintptr_t)TCB_BASE,TCB_SPAN,PROT_READ|PROT_WRITE) ||
        pthread_key_create(&tcb_key,tcb_release)) {
        perror("STOP: macOS guest TLS initialization"); exit(21);
    }
}
void bb_macos_set_tcb(void *base) {
    pthread_once(&tcb_once,tcb_init);
    TcbPage *page=pthread_getspecific(tcb_key);
    if (page) { page->tcb=base; return; }
    pthread_mutex_lock(&tcb_lock);
    unsigned slot;
    for (slot=0;slot<TCB_SLOTS && tcb_used[slot];++slot) {}
    if (slot==TCB_SLOTS) { fputs("STOP: macOS guest TLS slots exhausted\n",stderr); exit(21); }
    tcb_used[slot]=1;
    pthread_mutex_unlock(&tcb_lock);
    uintptr_t address=TCB_BASE+(uintptr_t)slot*TCB_PAGE;
    unsigned short index=(unsigned short)(8+slot);
    union ldt_entry descriptor={0};
    descriptor.data.limit00=TCB_PAGE-1;
    descriptor.data.base00=(unsigned short)address;
    descriptor.data.base16=(unsigned char)(address>>16);
    descriptor.data.type=DESC_DATA_WRITE;
    descriptor.data.dpl=USER_PRIV;
    descriptor.data.present=1;
    descriptor.data.stksz=DESC_DATA_32B;
    descriptor.data.base24=(unsigned char)(address>>24);
    if (i386_set_ldt(index,&descriptor,1)!=index) {
        perror("STOP: macOS guest TLS descriptor"); exit(21);
    }
    sel_t selector={.rpl=USER_PRIV,.ti=SEL_LDT,.index=index};
    __asm__ volatile("mov %0, %%fs" :: "r"(selector));
    page=(TcbPage *)address; page->tcb=base; page->index=index;
    if (pthread_setspecific(tcb_key,page)) { fputs("STOP: macOS TLS key\n",stderr); exit(21); }
}

void *bb_macos_map_no_replace(uintptr_t address,size_t size,int protection) {
    mach_vm_address_t target=address;
    kern_return_t result=mach_vm_allocate(mach_task_self(),&target,size,VM_FLAGS_FIXED);
    if (result!=KERN_SUCCESS) { errno=result==KERN_NO_SPACE ? EEXIST : ENOMEM; return MAP_FAILED; }
    if (mprotect((void *)target,size,protection)) {
        int saved=errno; mach_vm_deallocate(mach_task_self(),target,size); errno=saved; return MAP_FAILED;
    }
    return (void *)target;
}

/* Keep small runtime handles in reusable low-address slabs; do not change the
 * allocator used by SDL, Vulkan or the C++ renderer. */
typedef struct Allocation { struct Allocation *next; size_t capacity, requested, mapping; } Allocation;
enum { HEAP_BINS=15, HEADER_SIZE=64, SLAB_SIZE=1024*1024 };
static Allocation *heap_bins[HEAP_BINS];
static Allocation *heap_large_free;
static pthread_mutex_t heap_lock=PTHREAD_MUTEX_INITIALIZER;
static uintptr_t heap_next=UINT64_C(0x400000000);
static unsigned char *slab_next;
static size_t slab_remaining;
static void *heap_map(size_t bytes) {
    bytes=(bytes+16383)&~(size_t)16383;
    while (heap_next+bytes<=UINT64_C(0x800000000)) {
        uintptr_t address=heap_next; heap_next+=bytes;
        void *p=bb_macos_map_no_replace(address,bytes,PROT_READ|PROT_WRITE);
        if (p!=MAP_FAILED) return p;
    }
    errno=ENOMEM; return NULL;
}
void *bb_macos_malloc(size_t size) {
    if (size>SIZE_MAX-HEADER_SIZE-16383) { errno=ENOMEM; return NULL; }
    size_t capacity=64; unsigned bin=0;
    while (capacity<size && bin<HEAP_BINS-1) { capacity*=2; ++bin; }
    pthread_mutex_lock(&heap_lock);
    Allocation *allocation=NULL;
    if (capacity>=size) {
        allocation=heap_bins[bin];
        if (allocation) heap_bins[bin]=allocation->next;
        else {
            size_t bytes=HEADER_SIZE+capacity;
            if (bytes>SLAB_SIZE) allocation=heap_map(bytes);
            else {
                if (slab_remaining<bytes) { slab_next=heap_map(SLAB_SIZE); slab_remaining=slab_next ? SLAB_SIZE : 0; }
                if (slab_next) { allocation=(Allocation *)slab_next; slab_next+=bytes; slab_remaining-=bytes; }
            }
        }
        if (allocation) *allocation=(Allocation){.capacity=capacity,.requested=size};
    } else {
        size_t mapping=(size+HEADER_SIZE+16383)&~(size_t)16383;
        Allocation **available=&heap_large_free;
        while (*available && (*available)->mapping<mapping) available=&(*available)->next;
        if (*available) {
            allocation=*available; *available=allocation->next;
            allocation->requested=size; allocation->next=NULL;
        } else {
            allocation=heap_map(mapping);
            if (allocation) *allocation=(Allocation){.capacity=mapping-HEADER_SIZE,.requested=size,.mapping=mapping};
        }
    }
    pthread_mutex_unlock(&heap_lock);
    return allocation ? (unsigned char *)allocation+HEADER_SIZE : NULL;
}
void bb_macos_free(void *pointer) {
    if (!pointer) return;
    Allocation *allocation=(Allocation *)((unsigned char *)pointer-HEADER_SIZE);
    if (allocation->mapping) {
        /* Reuse low virtual addresses; discard payload pages while retaining
         * the header. Repeated large allocations must not exhaust the range. */
        if (allocation->mapping>16384)
            madvise((unsigned char *)allocation+16384,allocation->mapping-16384,MADV_DONTNEED);
        pthread_mutex_lock(&heap_lock);
        allocation->next=heap_large_free; heap_large_free=allocation;
        pthread_mutex_unlock(&heap_lock);
        return;
    }
    unsigned bin=0; size_t capacity=64;
    while (capacity<allocation->capacity && bin<HEAP_BINS-1) { capacity*=2; ++bin; }
    pthread_mutex_lock(&heap_lock);
    allocation->next=heap_bins[bin]; heap_bins[bin]=allocation;
    pthread_mutex_unlock(&heap_lock);
}
void *bb_macos_calloc(size_t count,size_t size) {
    if (size && count>SIZE_MAX/size) { errno=ENOMEM; return NULL; }
    void *pointer=bb_macos_malloc(count*size);
    if (pointer) memset(pointer,0,count*size);
    return pointer;
}
void *bb_macos_realloc(void *pointer,size_t size) {
    if (!pointer) return bb_macos_malloc(size);
    if (!size) { bb_macos_free(pointer); return NULL; }
    Allocation *allocation=(Allocation *)((unsigned char *)pointer-HEADER_SIZE);
    if (size<=allocation->capacity) { allocation->requested=size; return pointer; }
    void *next=bb_macos_malloc(size);
    if (next) { memcpy(next,pointer,allocation->requested); bb_macos_free(pointer); }
    return next;
}
void *bb_macos_aligned_alloc(size_t alignment,size_t size) {
    if (!alignment || alignment>64 || (alignment&(alignment-1)) || size%alignment) { errno=EINVAL; return NULL; }
    return bb_macos_malloc(size);
}
int bb_macos_memory_fd(void) {
    char path[32];
    snprintf(path,sizeof(path),"/bb-dmem-%d-%08x",getpid(),arc4random());
    int fd=shm_open(path,O_RDWR|O_CREAT|O_EXCL,0600);
    if (fd>=0) { shm_unlink(path); fcntl(fd,F_SETFD,FD_CLOEXEC); }
    return fd;
}
int bb_macos_cond_wait_until(void *condition,void *mutex,const struct timespec *deadline) {
    struct timespec now,relative;
    clock_gettime(CLOCK_MONOTONIC,&now);
    relative.tv_sec=deadline->tv_sec-now.tv_sec;
    relative.tv_nsec=deadline->tv_nsec-now.tv_nsec;
    if (relative.tv_nsec<0) { --relative.tv_sec; relative.tv_nsec+=1000000000; }
    if (relative.tv_sec<0) return ETIMEDOUT;
    return pthread_cond_timedwait_relative_np(condition,mutex,&relative);
}
int bb_macos_timed_lock(void *lock,const struct timespec *deadline,int kind) {
    long delay=10000;
    for (;;) {
        int result=kind==0 ? pthread_mutex_trylock(lock) :
                   kind==1 ? pthread_rwlock_tryrdlock(lock) : pthread_rwlock_trywrlock(lock);
        if (result!=EBUSY) return result;
        struct timespec now;
        if (clock_gettime(CLOCK_REALTIME,&now)) return errno;
        int64_t remaining=(int64_t)(deadline->tv_sec-now.tv_sec)*1000000000+deadline->tv_nsec-now.tv_nsec;
        if (remaining<=0) return ETIMEDOUT;
        struct timespec nap={0,remaining<delay ? (long)remaining : delay};
        nanosleep(&nap,NULL);
        if (delay<1000000) delay*=2;
        if (delay>1000000) delay=1000000;
    }
}
int bb_macos_sleep_until(const struct timespec *deadline) {
    for (;;) {
        struct timespec now,relative;
        clock_gettime(CLOCK_MONOTONIC,&now);
        relative.tv_sec=deadline->tv_sec-now.tv_sec; relative.tv_nsec=deadline->tv_nsec-now.tv_nsec;
        if (relative.tv_nsec<0) { --relative.tv_sec; relative.tv_nsec+=1000000000; }
        if (relative.tv_sec<0) return 0;
        if (!nanosleep(&relative,NULL)) return 0;
        if (errno!=EINTR) return errno;
    }
}
#endif
