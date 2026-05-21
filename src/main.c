#include "config.h"
#include "raw_http.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const char *addr = getenv("RINHA_ADDR");
    if (addr == NULL || addr[0] == '\0') {
        addr = RINHA_DEFAULT_ADDR;
    }

    if (raw_http_serve(addr) != 0) {
        perror("raw_http_serve");
        return 1;
    }
    return 0;
}
