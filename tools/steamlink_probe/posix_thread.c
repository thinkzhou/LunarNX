#include "ihs_thread.h"

#include <pthread.h>
#include <stdlib.h>

struct IHS_Thread {
    pthread_t handle;
};

struct IHS_Mutex {
    pthread_mutex_t handle;
};

struct IHS_Cond {
    pthread_cond_t handle;
};

struct ThreadStart {
    IHS_ThreadFunction *function;
    void *context;
};

static void *thread_entry(void *arg) {
    struct ThreadStart *start = arg;
    IHS_ThreadFunction *function = start->function;
    void *context = start->context;
    free(start);
    function(context);
    return NULL;
}

IHS_Thread *IHS_ThreadCreate(IHS_ThreadFunction *function, const char *name, void *context) {
    (void) name;
    IHS_Thread *thread = calloc(1, sizeof(*thread));
    struct ThreadStart *start = calloc(1, sizeof(*start));
    if (thread == NULL || start == NULL) {
        free(thread);
        free(start);
        return NULL;
    }
    start->function = function;
    start->context = context;
    if (pthread_create(&thread->handle, NULL, thread_entry, start) != 0) {
        free(thread);
        free(start);
        return NULL;
    }
    return thread;
}

void IHS_ThreadJoin(IHS_Thread *thread) {
    if (thread == NULL) return;
    pthread_join(thread->handle, NULL);
    free(thread);
}

IHS_Mutex *IHS_MutexCreate() {
    IHS_Mutex *mutex = calloc(1, sizeof(*mutex));
    if (mutex == NULL || pthread_mutex_init(&mutex->handle, NULL) != 0) {
        free(mutex);
        return NULL;
    }
    return mutex;
}

void IHS_MutexDestroy(IHS_Mutex *mutex) {
    if (mutex == NULL) return;
    pthread_mutex_destroy(&mutex->handle);
    free(mutex);
}

bool IHS_MutexLock(IHS_Mutex *mutex) {
    return mutex != NULL && pthread_mutex_lock(&mutex->handle) == 0;
}

bool IHS_MutexUnlock(IHS_Mutex *mutex) {
    return mutex != NULL && pthread_mutex_unlock(&mutex->handle) == 0;
}

IHS_Cond *IHS_CondCreate() {
    IHS_Cond *cond = calloc(1, sizeof(*cond));
    if (cond == NULL || pthread_cond_init(&cond->handle, NULL) != 0) {
        free(cond);
        return NULL;
    }
    return cond;
}

void IHS_CondDestroy(IHS_Cond *cond) {
    if (cond == NULL) return;
    pthread_cond_destroy(&cond->handle);
    free(cond);
}

void IHS_CondSignal(IHS_Cond *cond) {
    if (cond != NULL) pthread_cond_signal(&cond->handle);
}

bool IHS_CondWait(IHS_Cond *cond, IHS_Mutex *mutex) {
    return cond != NULL && mutex != NULL &&
           pthread_cond_wait(&cond->handle, &mutex->handle) == 0;
}
