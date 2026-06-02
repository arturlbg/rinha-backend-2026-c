#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_api_process_parse(void) {
    unsigned int processes = 0;

    assert(rinha_parse_api_processes(NULL, &processes));
    assert(processes == RINHA_DEFAULT_API_PROCESSES);

    assert(rinha_parse_api_processes("", &processes));
    assert(processes == RINHA_DEFAULT_API_PROCESSES);

    assert(rinha_parse_api_processes("1", &processes));
    assert(processes == 1u);

    assert(rinha_parse_api_processes("3", &processes));
    assert(processes == 3u);

    assert(!rinha_parse_api_processes("0", &processes));
    assert(!rinha_parse_api_processes("abc", &processes));
    assert(!rinha_parse_api_processes("999", &processes));
}

static void test_child_socket_single_process_keeps_base(void) {
    char out[RINHA_UNIX_SOCKET_PATH_MAX];

    assert(rinha_child_unix_socket_path("/sockets/api1.ctrl",
                                        0,
                                        1,
                                        out,
                                        sizeof(out)) == 0);
    assert(strcmp(out, "/sockets/api1.ctrl") == 0);
}

static void test_child_socket_inserts_index_before_extension(void) {
    char out[RINHA_UNIX_SOCKET_PATH_MAX];

    assert(rinha_child_unix_socket_path("/sockets/api1.ctrl",
                                        0,
                                        2,
                                        out,
                                        sizeof(out)) == 0);
    assert(strcmp(out, "/sockets/api1-0.ctrl") == 0);

    assert(rinha_child_unix_socket_path("/sockets/api1.ctrl",
                                        1,
                                        2,
                                        out,
                                        sizeof(out)) == 0);
    assert(strcmp(out, "/sockets/api1-1.ctrl") == 0);
}

static void test_child_socket_without_extension(void) {
    char out[RINHA_UNIX_SOCKET_PATH_MAX];

    assert(rinha_child_unix_socket_path("/sockets/api1",
                                        2,
                                        3,
                                        out,
                                        sizeof(out)) == 0);
    assert(strcmp(out, "/sockets/api1-2") == 0);
}

int main(void) {
    test_api_process_parse();
    test_child_socket_single_process_keeps_base();
    test_child_socket_inserts_index_before_extension();
    test_child_socket_without_extension();
    puts("config tests passed");
    return 0;
}
