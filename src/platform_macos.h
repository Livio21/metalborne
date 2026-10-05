#ifndef BB_PLATFORM_MACOS_H
#define BB_PLATFORM_MACOS_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ucontext.h>

void bb_macos_set_tcb(void *base);
void *bb_macos_malloc(size_t size);
void *bb_macos_calloc(size_t count, size_t size);
void *bb_macos_realloc(void *pointer, size_t size);
void *bb_macos_aligned_alloc(size_t alignment, size_t size);
void bb_macos_free(void *pointer);
int bb_macos_memory_fd(void);
int bb_macos_timed_lock(void *lock, const struct timespec *deadline, int kind);
void *bb_macos_map_no_replace(uintptr_t address, size_t size, int protection);
int bb_macos_cond_wait_until(void *condition, void *mutex, const struct timespec *deadline);
int bb_macos_sleep_until(const struct timespec *deadline);

#define BB_CONTEXT_REG(uc, name) BB_CONTEXT_##name(uc)
#define BB_CONTEXT_RIP(uc) ((uc)->uc_mcontext->__ss.__rip)
#define BB_CONTEXT_RBP(uc) ((uc)->uc_mcontext->__ss.__rbp)
#define BB_CONTEXT_RDI(uc) ((uc)->uc_mcontext->__ss.__rdi)
#define BB_STAT_ATIME(st) ((st).st_atimespec)
#define BB_STAT_MTIME(st) ((st).st_mtimespec)
#define BB_STAT_CTIME(st) ((st).st_ctimespec)

#ifndef BB_PLATFORM_ALLOCATOR_IMPL
/* Guest-visible runtime objects must fit PS4's 40-bit pointer fields. */
#define malloc(size) bb_macos_malloc(size)
#define calloc(count, size) bb_macos_calloc(count, size)
#define realloc(pointer, size) bb_macos_realloc(pointer, size)
#define aligned_alloc(alignment, size) bb_macos_aligned_alloc(alignment, size)
#define free(pointer) bb_macos_free(pointer)
#endif
#endif
