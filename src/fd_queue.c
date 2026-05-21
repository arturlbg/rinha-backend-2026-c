#include "fd_queue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} FdQueueSync;

static FdQueueSync *queue_sync(FdQueue *queue) {
    if (queue == NULL) {
        return NULL;
    }
    return (FdQueueSync *)queue->mutex_storage;
}

int fd_queue_init(FdQueue *queue, uint32_t capacity) {
    if (queue == NULL || capacity == 0) {
        return -1;
    }
    memset(queue, 0, sizeof(*queue));
    queue->items = (FdQueueItem *)calloc(capacity, sizeof(FdQueueItem));
    FdQueueSync *sync = (FdQueueSync *)calloc(1u, sizeof(FdQueueSync));
    if (queue->items == NULL || sync == NULL) {
        free(queue->items);
        free(sync);
        memset(queue, 0, sizeof(*queue));
        return -1;
    }
    if (pthread_mutex_init(&sync->mutex, NULL) != 0) {
        free(queue->items);
        free(sync);
        memset(queue, 0, sizeof(*queue));
        return -1;
    }
    if (pthread_cond_init(&sync->cond, NULL) != 0) {
        pthread_mutex_destroy(&sync->mutex);
        free(queue->items);
        free(sync);
        memset(queue, 0, sizeof(*queue));
        return -1;
    }

    queue->capacity = capacity;
    queue->mutex_storage = sync;
    queue->cond_storage = sync;
    return 0;
}

void fd_queue_destroy(FdQueue *queue) {
    if (queue == NULL) {
        return;
    }
    FdQueueSync *sync = queue_sync(queue);
    if (sync != NULL) {
        pthread_cond_destroy(&sync->cond);
        pthread_mutex_destroy(&sync->mutex);
    }
    free(sync);
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}

bool fd_queue_push(FdQueue *queue, int fd, uint64_t enqueued_ns) {
    FdQueueSync *sync = queue_sync(queue);
    if (queue == NULL || sync == NULL || queue->items == NULL) {
        return false;
    }

    pthread_mutex_lock(&sync->mutex);
    if (queue->closed || queue->count == queue->capacity) {
        pthread_mutex_unlock(&sync->mutex);
        return false;
    }

    queue->items[queue->tail].fd = fd;
    queue->items[queue->tail].enqueued_ns = enqueued_ns;
    queue->tail = (queue->tail + 1u) % queue->capacity;
    queue->count++;
    pthread_cond_signal(&sync->cond);
    pthread_mutex_unlock(&sync->mutex);
    return true;
}

bool fd_queue_pop(FdQueue *queue, FdQueueItem *out) {
    FdQueueSync *sync = queue_sync(queue);
    if (queue == NULL || out == NULL || sync == NULL || queue->items == NULL) {
        return false;
    }

    pthread_mutex_lock(&sync->mutex);
    while (!queue->closed && queue->count == 0) {
        pthread_cond_wait(&sync->cond, &sync->mutex);
    }
    if (queue->count == 0) {
        pthread_mutex_unlock(&sync->mutex);
        return false;
    }

    *out = queue->items[queue->head];
    queue->head = (queue->head + 1u) % queue->capacity;
    queue->count--;
    pthread_mutex_unlock(&sync->mutex);
    return true;
}

void fd_queue_close(FdQueue *queue) {
    FdQueueSync *sync = queue_sync(queue);
    if (queue == NULL || sync == NULL) {
        return;
    }
    pthread_mutex_lock(&sync->mutex);
    queue->closed = true;
    pthread_cond_broadcast(&sync->cond);
    pthread_mutex_unlock(&sync->mutex);
}
