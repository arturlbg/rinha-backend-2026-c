CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -pedantic
CPPFLAGS ?= -Iinclude
LDFLAGS ?=

BUILD_DIR := build
TARGET := $(BUILD_DIR)/rinha-api
SOURCES := src/main.c src/raw_http.c src/responses.c
OBJECTS := $(SOURCES:%.c=$(BUILD_DIR)/%.o)
TEST_TARGET := $(BUILD_DIR)/test_raw_http
TEST_SOURCES := tests/test_raw_http.c src/raw_http.c src/responses.c

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(BUILD_DIR)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TEST_TARGET): $(TEST_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(TEST_SOURCES) -o $@

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -rf $(BUILD_DIR)
