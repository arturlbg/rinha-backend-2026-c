#ifndef RINHA_FD_QUEUE_H
#define RINHA_FD_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int fd;
    uint64_t enqueued_ns;
} FdQueueItem;

typedef struct {
    FdQueueItem *items;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    bool closed;
    void *mutex_storage;
    void *cond_storage;
} FdQueue;

int fd_queue_init(FdQueue *queue, uint32_t capacity);
void fd_queue_destroy(FdQueue *queue);
bool fd_queue_push(FdQueue *queue, int fd, uint64_t enqueued_ns);
bool fd_queue_pop(FdQueue *queue, FdQueueItem *out);
void fd_queue_close(FdQueue *queue);

#endif
