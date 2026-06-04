#include "fdlb.h"

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int recv_one_fd_for_test(int control_fd) {
    char data[1];
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov;
    struct msghdr msg;

    memset(control, 0, sizeof(control));
    memset(&iov, 0, sizeof(iov));
    memset(&msg, 0, sizeof(msg));

    iov.iov_base = data;
    iov.iov_len = sizeof(data);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t n;
    do {
        n = recvmsg(control_fd, &msg, 0);
    } while (n < 0 && errno == EINTR);
    assert(n == 1);

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int fd = -1;
            memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
            return fd;
        }
    }

    return -1;
}

static void test_parse_upstreams(void) {
    char text[] = " /sockets/a.ctrl ,/sockets/b.ctrl,/sockets/c.ctrl ";
    char *paths[4];
    size_t count = 0;

    assert(fdlb_parse_upstreams(text, paths, 4, &count) == 0);
    assert(count == 3);
    assert(strcmp(paths[0], "/sockets/a.ctrl") == 0);
    assert(strcmp(paths[1], "/sockets/b.ctrl") == 0);
    assert(strcmp(paths[2], "/sockets/c.ctrl") == 0);

    char many[] = "/sockets/api1-0.ctrl,/sockets/api1-1.ctrl,/sockets/api2-0.ctrl,/sockets/api2-1.ctrl";
    count = 0;
    assert(fdlb_parse_upstreams(many, paths, 4, &count) == 0);
    assert(count == 4);
    assert(strcmp(paths[0], "/sockets/api1-0.ctrl") == 0);
    assert(strcmp(paths[1], "/sockets/api1-1.ctrl") == 0);
    assert(strcmp(paths[2], "/sockets/api2-0.ctrl") == 0);
    assert(strcmp(paths[3], "/sockets/api2-1.ctrl") == 0);

    char bad[] = "/sockets/a.ctrl,,/sockets/b.ctrl";
    count = 0;
    assert(fdlb_parse_upstreams(bad, paths, 4, &count) != 0);

    char proxy[] = "/sockets/api1.sock,/sockets/api2.sock";
    count = 0;
    assert(fdlb_parse_upstreams(proxy, paths, 4, &count) == 0);
    assert(count == 2);
    assert(strcmp(paths[0], "/sockets/api1.sock") == 0);
    assert(strcmp(paths[1], "/sockets/api2.sock") == 0);
}

static void test_round_robin(void) {
    size_t cursor = 0;
    assert(fdlb_round_robin_next(&cursor, 2) == 0);
    assert(fdlb_round_robin_next(&cursor, 2) == 1);
    assert(fdlb_round_robin_next(&cursor, 2) == 0);
    assert(cursor == 1);
}

static void test_mode_parse(void) {
    FdlbMode mode = FDLB_MODE_PROXY;
    assert(fdlb_parse_mode(NULL, &mode));
    assert(mode == FDLB_MODE_FDPASS);
    assert(fdlb_parse_mode("fdpass", &mode));
    assert(mode == FDLB_MODE_FDPASS);
    assert(fdlb_parse_mode("proxy", &mode));
    assert(mode == FDLB_MODE_PROXY);
    assert(!fdlb_parse_mode("http", &mode));
}

static void test_strategy_parse(void) {
    FdlbStrategy strategy = FDLB_STRATEGY_ROUND_ROBIN;
    assert(fdlb_parse_strategy(NULL, &strategy));
    assert(strategy == FDLB_STRATEGY_ROUND_ROBIN);
    assert(fdlb_parse_strategy("least_active", &strategy));
    assert(strategy == FDLB_STRATEGY_LEAST_ACTIVE);
    assert(fdlb_parse_strategy("power_of_two", &strategy));
    assert(strategy == FDLB_STRATEGY_POWER_OF_TWO);
    assert(!fdlb_parse_strategy("bogus", &strategy));
}

static void test_strategy_select(void) {
    uint32_t active[3] = {5, 1, 1};
    size_t cursor = 0;
    assert(fdlb_select_upstream(FDLB_STRATEGY_LEAST_ACTIVE, &cursor, active, 3) == 1);
    assert(cursor == 2);
    assert(fdlb_select_upstream(FDLB_STRATEGY_LEAST_ACTIVE, &cursor, active, 3) == 2);

    active[0] = 9;
    active[1] = 2;
    cursor = 0;
    assert(fdlb_select_upstream(FDLB_STRATEGY_POWER_OF_TWO, &cursor, active, 2) == 1);
}

static void test_feedback_decrement(void) {
    uint32_t active = 2;
    fdlb_decrement_active(&active);
    assert(active == 1);
    fdlb_decrement_active(&active);
    assert(active == 0);
    fdlb_decrement_active(&active);
    assert(active == 0);
}

static void test_send_fd(void) {
    int sv[2];
    int pipefd[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(pipe(pipefd) == 0);

    assert(fdlb_send_one_fd(sv[0], pipefd[1]) == 0);
    int received = recv_one_fd_for_test(sv[1]);
    assert(received >= 0);

    close(pipefd[1]);
    const char byte = 'x';
    assert(write(received, &byte, 1) == 1);

    char got = '\0';
    assert(read(pipefd[0], &got, 1) == 1);
    assert(got == 'x');

    close(received);
    close(pipefd[0]);
    close(sv[0]);
    close(sv[1]);
}

int main(void) {
    test_parse_upstreams();
    test_round_robin();
    test_mode_parse();
    test_strategy_parse();
    test_strategy_select();
    test_feedback_decrement();
    test_send_fd();
    return 0;
}
