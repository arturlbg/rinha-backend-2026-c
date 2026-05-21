#define _POSIX_C_SOURCE 200809L

#include "fastvector.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_vector(const int16_t vector[FASTVECTOR_DIMENSIONS]) {
    for (int i = 0; i < FASTVECTOR_DIMENSIONS; i++) {
        if (i != 0) {
            putchar(' ');
        }
        printf("%d", vector[i]);
    }
    putchar('\n');
}

static int vectorize_one(const char *data, size_t len) {
    int16_t vector[FASTVECTOR_DIMENSIONS];
    if (!fastvector_vectorize(data, len, vector)) {
        fprintf(stderr, "vectorize_c: invalid payload\n");
        return 1;
    }
    print_vector(vector);
    return 0;
}

static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "vectorize_c: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "vectorize_c: seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    long size = ftell(file);
    if (size < 0) {
        fprintf(stderr, "vectorize_c: tell %s failed\n", path);
        fclose(file);
        return 1;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "vectorize_c: seek %s failed\n", path);
        fclose(file);
        return 1;
    }
    char *buffer = (char *)malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fprintf(stderr, "vectorize_c: out of memory\n");
        fclose(file);
        return 1;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        fprintf(stderr, "vectorize_c: short read %s\n", path);
        free(buffer);
        return 1;
    }
    buffer[got] = '\0';
    *out = buffer;
    *out_len = got;
    return 0;
}

static int vectorize_jsonl(FILE *file) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = 0;
    size_t line_no = 0;
    int status = 0;

    while ((n = getline(&line, &cap, file)) >= 0) {
        line_no++;
        size_t len = (size_t)n;
        while (len > 0 && (line[len - 1u] == '\n' || line[len - 1u] == '\r')) {
            len--;
        }
        if (len == 0) {
            continue;
        }
        int16_t vector[FASTVECTOR_DIMENSIONS];
        if (!fastvector_vectorize(line, len, vector)) {
            fprintf(stderr, "vectorize_c: invalid payload at line %zu\n", line_no);
            status = 1;
            break;
        }
        print_vector(vector);
    }

    free(line);
    return status;
}

int main(int argc, char **argv) {
    int jsonl = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--jsonl") == 0) {
            jsonl = 1;
        } else if (path == NULL) {
            path = argv[i];
        } else {
            fprintf(stderr, "usage: vectorize_c [--jsonl] [path]\n");
            return 2;
        }
    }

    if (jsonl) {
        if (path == NULL) {
            return vectorize_jsonl(stdin);
        }
        FILE *file = fopen(path, "rb");
        if (file == NULL) {
            fprintf(stderr, "vectorize_c: open %s: %s\n", path, strerror(errno));
            return 1;
        }
        int status = vectorize_jsonl(file);
        fclose(file);
        return status;
    }

    char *buffer = NULL;
    size_t len = 0;
    if (path != NULL) {
        if (read_file(path, &buffer, &len) != 0) {
            return 1;
        }
    } else {
        size_t cap = 8192;
        buffer = (char *)malloc(cap);
        if (buffer == NULL) {
            fprintf(stderr, "vectorize_c: out of memory\n");
            return 1;
        }
        int ch = 0;
        while ((ch = getchar()) != EOF) {
            if (len == cap) {
                size_t next = cap * 2u;
                char *grown = (char *)realloc(buffer, next);
                if (grown == NULL) {
                    fprintf(stderr, "vectorize_c: out of memory\n");
                    free(buffer);
                    return 1;
                }
                buffer = grown;
                cap = next;
            }
            buffer[len++] = (char)ch;
        }
    }

    int status = vectorize_one(buffer, len);
    free(buffer);
    return status;
}

