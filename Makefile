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

.PHONY: all clean test

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

test: $(RAW_HTTP_TEST_TARGET) $(FASTVECTOR_TEST_TARGET)
	./$(RAW_HTTP_TEST_TARGET)
	./$(FASTVECTOR_TEST_TARGET)

clean:
	rm -rf $(BUILD_DIR)
