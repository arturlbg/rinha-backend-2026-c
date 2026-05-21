#include "fd_queue.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void test_push_pop_order(void) {
    FdQueue queue;
    CHECK(fd_queue_init(&queue, 2) == 0);
    CHECK(fd_queue_push(&queue, 10, 100));
    CHECK(fd_queue_push(&queue, 11, 200));
    CHECK(!fd_queue_push(&queue, 12, 300));

    FdQueueItem item;
    CHECK(fd_queue_pop(&queue, &item));
    CHECK(item.fd == 10);
    CHECK(item.enqueued_ns == 100);
    CHECK(fd_queue_pop(&queue, &item));
    CHECK(item.fd == 11);
    CHECK(item.enqueued_ns == 200);
    fd_queue_close(&queue);
    CHECK(!fd_queue_pop(&queue, &item));
    fd_queue_destroy(&queue);
}

static void test_closed_queue_rejects_push(void) {
    FdQueue queue;
    CHECK(fd_queue_init(&queue, 1) == 0);
    fd_queue_close(&queue);
    CHECK(!fd_queue_push(&queue, 10, 100));
    fd_queue_destroy(&queue);
}

int main(void) {
    test_push_pop_order();
    test_closed_queue_rejects_push();

    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }
    puts("fd_queue tests passed");
    return 0;
}
