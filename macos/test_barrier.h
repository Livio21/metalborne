/* Darwin lacks POSIX barriers; this is only used by the upstream runtime check. */
#include <errno.h>
#include <pthread.h>
#define PTHREAD_BARRIER_SERIAL_THREAD 1
typedef struct { pthread_mutex_t lock; pthread_cond_t event; unsigned count, waiting, generation; } pthread_barrier_t;
static int pthread_barrier_init(pthread_barrier_t *barrier,const void *attributes,unsigned count) {
    (void)attributes;
    if (!count) return EINVAL;
    int result=pthread_mutex_init(&barrier->lock,NULL);
    if (result) return result;
    result=pthread_cond_init(&barrier->event,NULL);
    if (result) { pthread_mutex_destroy(&barrier->lock); return result; }
    barrier->count=count; barrier->waiting=barrier->generation=0;
    return 0;
}
static int pthread_barrier_wait(pthread_barrier_t *barrier) {
    pthread_mutex_lock(&barrier->lock);
    unsigned generation=barrier->generation;
    if (++barrier->waiting==barrier->count) {
        barrier->waiting=0; ++barrier->generation;
        pthread_cond_broadcast(&barrier->event);
        pthread_mutex_unlock(&barrier->lock);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    while (generation==barrier->generation) pthread_cond_wait(&barrier->event,&barrier->lock);
    pthread_mutex_unlock(&barrier->lock);
    return 0;
}
static int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    int result=pthread_cond_destroy(&barrier->event);
    return result ? result : pthread_mutex_destroy(&barrier->lock);
}
