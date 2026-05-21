CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -pedantic
CPPFLAGS ?= -Iinclude
LDFLAGS ?=
AVX2_CFLAGS ?= -mavx2

BUILD_DIR := build
TARGET := $(BUILD_DIR)/rinha-api
SOURCES := src/main.c src/raw_http.c src/responses.c src/fastvector.c src/ivf8_index.c src/ivf8_search.c src/fdpass.c src/fd_queue.c src/metrics.c
AVX2_OBJECT := $(BUILD_DIR)/src/ivf8_search_avx2.o
OBJECTS := $(SOURCES:%.c=$(BUILD_DIR)/%.o) $(AVX2_OBJECT)
RAW_HTTP_TEST_TARGET := $(BUILD_DIR)/test_raw_http
RAW_HTTP_TEST_SOURCES := tests/test_raw_http.c src/raw_http.c src/responses.c src/fastvector.c src/ivf8_search.c src/metrics.c
FASTVECTOR_TEST_TARGET := $(BUILD_DIR)/test_fastvector
FASTVECTOR_TEST_SOURCES := tests/test_fastvector.c src/fastvector.c
IVF8_INDEX_TEST_TARGET := $(BUILD_DIR)/test_ivf8_index
IVF8_INDEX_TEST_SOURCES := tests/test_ivf8_index.c src/ivf8_index.c
IVF8_SEARCH_TEST_TARGET := $(BUILD_DIR)/test_ivf8_search
IVF8_SEARCH_TEST_SOURCES := tests/test_ivf8_search.c src/ivf8_search.c
FDPASS_TEST_TARGET := $(BUILD_DIR)/test_fdpass
FDPASS_TEST_SOURCES := tests/test_fdpass.c src/fdpass.c src/fd_queue.c src/raw_http.c src/responses.c src/fastvector.c src/ivf8_search.c src/metrics.c
FD_QUEUE_TEST_TARGET := $(BUILD_DIR)/test_fd_queue
FD_QUEUE_TEST_SOURCES := tests/test_fd_queue.c src/fd_queue.c
VECTORIZE_C_TARGET := $(BUILD_DIR)/vectorize_c
VECTORIZE_C_SOURCES := tools/vectorize_c.c src/fastvector.c
INSPECT_INDEX_TARGET := $(BUILD_DIR)/inspect_index
INSPECT_INDEX_SOURCES := tools/inspect_index.c src/ivf8_index.c
EVALUATE_C_TARGET := $(BUILD_DIR)/evaluate_c
EVALUATE_C_SOURCES := tools/evaluate_c.c src/fastvector.c src/ivf8_index.c src/ivf8_search.c
BENCH_SEARCH_TARGET := $(BUILD_DIR)/bench_search
BENCH_SEARCH_SOURCES := tools/bench_search.c src/fastvector.c src/ivf8_index.c src/ivf8_search.c src/metrics.c

.PHONY: all clean test tools

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -pthread $(LDFLAGS) -o $@

$(AVX2_OBJECT): src/ivf8_search_avx2.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(AVX2_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(RAW_HTTP_TEST_TARGET): $(RAW_HTTP_TEST_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(RAW_HTTP_TEST_SOURCES) $(AVX2_OBJECT) -o $@

$(FASTVECTOR_TEST_TARGET): $(FASTVECTOR_TEST_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FASTVECTOR_TEST_SOURCES) -o $@

$(IVF8_INDEX_TEST_TARGET): $(IVF8_INDEX_TEST_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(IVF8_INDEX_TEST_SOURCES) -o $@

$(IVF8_SEARCH_TEST_TARGET): $(IVF8_SEARCH_TEST_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(IVF8_SEARCH_TEST_SOURCES) $(AVX2_OBJECT) -o $@

$(FDPASS_TEST_TARGET): $(FDPASS_TEST_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(FDPASS_TEST_SOURCES) $(AVX2_OBJECT) -o $@

$(FD_QUEUE_TEST_TARGET): $(FD_QUEUE_TEST_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(FD_QUEUE_TEST_SOURCES) -o $@

$(VECTORIZE_C_TARGET): $(VECTORIZE_C_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VECTORIZE_C_SOURCES) -o $@

$(INSPECT_INDEX_TARGET): $(INSPECT_INDEX_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INSPECT_INDEX_SOURCES) -o $@

$(EVALUATE_C_TARGET): $(EVALUATE_C_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EVALUATE_C_SOURCES) $(AVX2_OBJECT) -o $@

$(BENCH_SEARCH_TARGET): $(BENCH_SEARCH_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BENCH_SEARCH_SOURCES) $(AVX2_OBJECT) -o $@

tools: $(VECTORIZE_C_TARGET) $(INSPECT_INDEX_TARGET) $(EVALUATE_C_TARGET) $(BENCH_SEARCH_TARGET)

test: $(RAW_HTTP_TEST_TARGET) $(FASTVECTOR_TEST_TARGET) $(IVF8_INDEX_TEST_TARGET) $(IVF8_SEARCH_TEST_TARGET) $(FDPASS_TEST_TARGET) $(FD_QUEUE_TEST_TARGET) tools
	./$(RAW_HTTP_TEST_TARGET)
	./$(FASTVECTOR_TEST_TARGET)
	./$(IVF8_INDEX_TEST_TARGET)
	./$(IVF8_SEARCH_TEST_TARGET)
	./$(FDPASS_TEST_TARGET)
	./$(FD_QUEUE_TEST_TARGET)

clean:
	rm -rf $(BUILD_DIR)
