#include "fdpass.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void test_send_receive_fd(void) {
    int control[2];
    int pipefd[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, control) == 0);
    CHECK(pipe(pipefd) == 0);

    CHECK(fdpass_send_one_fd(control[0], pipefd[1]) == 0);
    int received = fdpass_recv_one_fd(control[1]);
    CHECK(received >= 0);

    const char payload[] = "ok";
    CHECK(write(received, payload, sizeof(payload) - 1) == (ssize_t)(sizeof(payload) - 1));

    char buf[8];
    ssize_t n = read(pipefd[0], buf, sizeof(buf));
    CHECK(n == 2);
    CHECK(memcmp(buf, "ok", 2) == 0);

    close(received);
    close(pipefd[0]);
    close(pipefd[1]);
    close(control[0]);
    close(control[1]);
}

static void test_missing_fd_is_error(void) {
    int control[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, control) == 0);
    CHECK(send(control[0], "x", 1, 0) == 1);
    CHECK(fdpass_recv_one_fd(control[1]) < 0);
    close(control[0]);
    close(control[1]);
}

int main(void) {
    test_send_receive_fd();
    test_missing_fd_is_error();

    if (failures != 0) {
        fprintf(stderr, "%d test failure(s)\n", failures);
        return 1;
    }
    puts("fdpass tests passed");
    return 0;
}
