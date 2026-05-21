CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -pedantic
CPPFLAGS ?= -Iinclude
LDFLAGS ?=

BUILD_DIR := build
TARGET := $(BUILD_DIR)/rinha-api
SOURCES := src/main.c src/raw_http.c src/responses.c src/fastvector.c
OBJECTS := $(SOURCES:%.c=$(BUILD_DIR)/%.o)
RAW_HTTP_TEST_TARGET := $(BUILD_DIR)/test_raw_http
RAW_HTTP_TEST_SOURCES := tests/test_raw_http.c src/raw_http.c src/responses.c
FASTVECTOR_TEST_TARGET := $(BUILD_DIR)/test_fastvector
FASTVECTOR_TEST_SOURCES := tests/test_fastvector.c src/fastvector.c
IVF8_INDEX_TEST_TARGET := $(BUILD_DIR)/test_ivf8_index
IVF8_INDEX_TEST_SOURCES := tests/test_ivf8_index.c src/ivf8_index.c
VECTORIZE_C_TARGET := $(BUILD_DIR)/vectorize_c
VECTORIZE_C_SOURCES := tools/vectorize_c.c src/fastvector.c
INSPECT_INDEX_TARGET := $(BUILD_DIR)/inspect_index
INSPECT_INDEX_SOURCES := tools/inspect_index.c src/ivf8_index.c

.PHONY: all clean test tools

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(BUILD_DIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(RAW_HTTP_TEST_TARGET): $(RAW_HTTP_TEST_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(RAW_HTTP_TEST_SOURCES) -o $@

$(FASTVECTOR_TEST_TARGET): $(FASTVECTOR_TEST_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FASTVECTOR_TEST_SOURCES) -o $@

$(IVF8_INDEX_TEST_TARGET): $(IVF8_INDEX_TEST_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(IVF8_INDEX_TEST_SOURCES) -o $@

$(VECTORIZE_C_TARGET): $(VECTORIZE_C_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VECTORIZE_C_SOURCES) -o $@

$(INSPECT_INDEX_TARGET): $(INSPECT_INDEX_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INSPECT_INDEX_SOURCES) -o $@

tools: $(VECTORIZE_C_TARGET) $(INSPECT_INDEX_TARGET)

test: $(RAW_HTTP_TEST_TARGET) $(FASTVECTOR_TEST_TARGET) $(IVF8_INDEX_TEST_TARGET) tools
	./$(RAW_HTTP_TEST_TARGET)
	./$(FASTVECTOR_TEST_TARGET)
	./$(IVF8_INDEX_TEST_TARGET)

clean:
	rm -rf $(BUILD_DIR)
