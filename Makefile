CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -pedantic
CPPFLAGS ?= -Iinclude
LDFLAGS ?=
AVX2_CFLAGS ?= -mavx2
CFLAGS_PROFILE ?= current
RINHA_ENABLE_METRICS ?= 1
PGO_DIR ?= $(BUILD_DIR)/pgo

CPPFLAGS += -DRINHA_ENABLE_METRICS=$(RINHA_ENABLE_METRICS)

ifeq ($(CFLAGS_PROFILE),current)
else ifeq ($(CFLAGS_PROFILE),pre10b)
CFLAGS += -O3 -DNDEBUG -flto
LDFLAGS += -flto
else ifeq ($(CFLAGS_PROFILE),o3)
CFLAGS += -O3 -DNDEBUG -fomit-frame-pointer
else ifeq ($(CFLAGS_PROFILE),lto)
CFLAGS += -O3 -DNDEBUG -fomit-frame-pointer -flto
LDFLAGS += -flto
else ifeq ($(CFLAGS_PROFILE),v3)
CFLAGS += -O3 -DNDEBUG -fomit-frame-pointer -march=x86-64-v3
else ifeq ($(CFLAGS_PROFILE),pgo-generate)
CFLAGS += -O3 -DNDEBUG -fomit-frame-pointer -fprofile-generate=$(PGO_DIR)
LDFLAGS += -fprofile-generate=$(PGO_DIR)
else ifeq ($(CFLAGS_PROFILE),pgo-use)
CFLAGS += -O3 -DNDEBUG -fomit-frame-pointer -fprofile-use=$(PGO_DIR) -fprofile-correction -Wno-error=missing-profile -Wno-error=coverage-mismatch
LDFLAGS += -fprofile-use=$(PGO_DIR) -fprofile-correction
else
$(error unknown CFLAGS_PROFILE '$(CFLAGS_PROFILE)')
endif

BUILD_DIR := build
TARGET := $(BUILD_DIR)/rinha-api
FDLB_TARGET := $(BUILD_DIR)/rinha-fdlb
SOURCES := src/main.c src/raw_http.c src/responses.c src/fastvector.c src/ivf8_index.c src/ivf8_search.c src/kdprimary.c src/kdprimary2.c src/kdtree.c src/kdtree_repair.c src/fdpass.c src/fd_queue.c src/metrics.c
FDLB_SOURCES := src/fdlb_main.c src/fdlb.c
AVX2_OBJECT := $(BUILD_DIR)/src/ivf8_search_avx2.o
OBJECTS := $(SOURCES:%.c=$(BUILD_DIR)/%.o) $(AVX2_OBJECT)
RAW_HTTP_TEST_TARGET := $(BUILD_DIR)/test_raw_http
RAW_HTTP_TEST_SOURCES := tests/test_raw_http.c src/raw_http.c src/responses.c src/fastvector.c src/ivf8_search.c src/kdprimary.c src/kdprimary2.c src/kdtree.c src/kdtree_repair.c src/metrics.c
FASTVECTOR_TEST_TARGET := $(BUILD_DIR)/test_fastvector
FASTVECTOR_TEST_SOURCES := tests/test_fastvector.c src/fastvector.c
IVF8_INDEX_TEST_TARGET := $(BUILD_DIR)/test_ivf8_index
IVF8_INDEX_TEST_SOURCES := tests/test_ivf8_index.c src/ivf8_index.c
IVF8_SEARCH_TEST_TARGET := $(BUILD_DIR)/test_ivf8_search
IVF8_SEARCH_TEST_SOURCES := tests/test_ivf8_search.c src/ivf8_search.c
FDPASS_TEST_TARGET := $(BUILD_DIR)/test_fdpass
FDPASS_TEST_SOURCES := tests/test_fdpass.c src/fdpass.c src/fd_queue.c src/raw_http.c src/responses.c src/fastvector.c src/ivf8_search.c src/kdprimary.c src/kdprimary2.c src/kdtree.c src/kdtree_repair.c src/metrics.c
FDLB_TEST_TARGET := $(BUILD_DIR)/test_fdlb
FDLB_TEST_SOURCES := tests/test_fdlb.c src/fdlb.c
FD_QUEUE_TEST_TARGET := $(BUILD_DIR)/test_fd_queue
FD_QUEUE_TEST_SOURCES := tests/test_fd_queue.c src/fd_queue.c
KDTREE_TEST_TARGET := $(BUILD_DIR)/test_kdtree
KDTREE_TEST_SOURCES := tests/test_kdtree.c src/kdtree.c src/ivf8_search.c src/ivf8_index.c
KDTREE_REPAIR_TEST_TARGET := $(BUILD_DIR)/test_kdtree_repair
KDTREE_REPAIR_TEST_SOURCES := tests/test_kdtree_repair.c src/kdtree_repair.c
KDPRIMARY_TEST_TARGET := $(BUILD_DIR)/test_kdprimary
KDPRIMARY_TEST_SOURCES := tests/test_kdprimary.c src/kdprimary.c src/ivf8_search.c
KDPRIMARY2_TEST_TARGET := $(BUILD_DIR)/test_kdprimary2
KDPRIMARY2_TEST_SOURCES := tests/test_kdprimary2.c src/kdprimary2.c src/ivf8_search.c
VECTORIZE_C_TARGET := $(BUILD_DIR)/vectorize_c
VECTORIZE_C_SOURCES := tools/vectorize_c.c src/fastvector.c
INSPECT_INDEX_TARGET := $(BUILD_DIR)/inspect_index
INSPECT_INDEX_SOURCES := tools/inspect_index.c src/ivf8_index.c
EVALUATE_C_TARGET := $(BUILD_DIR)/evaluate_c
EVALUATE_C_SOURCES := tools/evaluate_c.c src/fastvector.c src/ivf8_index.c src/ivf8_search.c
BENCH_SEARCH_TARGET := $(BUILD_DIR)/bench_search
BENCH_SEARCH_SOURCES := tools/bench_search.c src/fastvector.c src/ivf8_index.c src/ivf8_search.c src/metrics.c
BUILD_KDTREE_TARGET := $(BUILD_DIR)/build_kdtree
BUILD_KDTREE_SOURCES := tools/build_kdtree.c src/kdtree.c src/ivf8_index.c src/ivf8_search.c
EVALUATE_KDTREE_TARGET := $(BUILD_DIR)/evaluate_kdtree
EVALUATE_KDTREE_SOURCES := tools/evaluate_kdtree.c src/kdtree.c src/fastvector.c src/ivf8_index.c src/ivf8_search.c
EVALUATE_HYBRID_TARGET := $(BUILD_DIR)/evaluate_hybrid
EVALUATE_HYBRID_SOURCES := tools/evaluate_hybrid.c src/kdtree.c src/kdtree_repair.c src/fastvector.c src/ivf8_index.c src/ivf8_search.c
ANALYZE_REPAIR_POLICY_TARGET := $(BUILD_DIR)/analyze_repair_policy
ANALYZE_REPAIR_POLICY_SOURCES := tools/analyze_repair_policy.c src/kdtree.c src/kdtree_repair.c src/fastvector.c src/ivf8_index.c src/ivf8_search.c
BUILD_KDPRIMARY_TARGET := $(BUILD_DIR)/build_kdprimary
BUILD_KDPRIMARY_SOURCES := tools/build_kdprimary.c src/kdprimary.c src/ivf8_index.c src/ivf8_search.c
EVALUATE_KDPRIMARY_TARGET := $(BUILD_DIR)/evaluate_kdprimary
EVALUATE_KDPRIMARY_SOURCES := tools/evaluate_kdprimary.c src/kdprimary.c src/fastvector.c src/ivf8_search.c
BUILD_KDPRIMARY2_TARGET := $(BUILD_DIR)/build_kdprimary2
BUILD_KDPRIMARY2_SOURCES := tools/build_kdprimary2.c src/kdprimary2.c src/ivf8_index.c src/ivf8_search.c
EVALUATE_KDPRIMARY2_TARGET := $(BUILD_DIR)/evaluate_kdprimary2
EVALUATE_KDPRIMARY2_SOURCES := tools/evaluate_kdprimary2.c src/kdprimary2.c src/fastvector.c src/ivf8_search.c

.PHONY: all clean fdlb fdlb-release test tools release

all: $(TARGET)

fdlb: $(FDLB_TARGET)

release:
	$(MAKE) clean
	$(MAKE) all CFLAGS_PROFILE=pre10b RINHA_ENABLE_METRICS=1

fdlb-release:
	$(MAKE) clean
	$(MAKE) fdlb CFLAGS_PROFILE=current RINHA_ENABLE_METRICS=1

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -pthread $(LDFLAGS) -o $@

$(FDLB_TARGET): $(FDLB_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FDLB_SOURCES) -o $@

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

$(FDLB_TEST_TARGET): $(FDLB_TEST_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FDLB_TEST_SOURCES) -o $@

$(FD_QUEUE_TEST_TARGET): $(FD_QUEUE_TEST_SOURCES)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(FD_QUEUE_TEST_SOURCES) -o $@

$(KDTREE_TEST_TARGET): $(KDTREE_TEST_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KDTREE_TEST_SOURCES) $(AVX2_OBJECT) -o $@

$(KDTREE_REPAIR_TEST_TARGET): $(KDTREE_REPAIR_TEST_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KDTREE_REPAIR_TEST_SOURCES) $(AVX2_OBJECT) -o $@

$(KDPRIMARY_TEST_TARGET): $(KDPRIMARY_TEST_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KDPRIMARY_TEST_SOURCES) $(AVX2_OBJECT) -o $@

$(KDPRIMARY2_TEST_TARGET): $(KDPRIMARY2_TEST_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(KDPRIMARY2_TEST_SOURCES) $(AVX2_OBJECT) -o $@

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

$(BUILD_KDTREE_TARGET): $(BUILD_KDTREE_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BUILD_KDTREE_SOURCES) $(AVX2_OBJECT) -o $@

$(EVALUATE_KDTREE_TARGET): $(EVALUATE_KDTREE_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EVALUATE_KDTREE_SOURCES) $(AVX2_OBJECT) -o $@

$(EVALUATE_HYBRID_TARGET): $(EVALUATE_HYBRID_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EVALUATE_HYBRID_SOURCES) $(AVX2_OBJECT) -lm -o $@

$(ANALYZE_REPAIR_POLICY_TARGET): $(ANALYZE_REPAIR_POLICY_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ANALYZE_REPAIR_POLICY_SOURCES) $(AVX2_OBJECT) -lm -o $@

$(BUILD_KDPRIMARY_TARGET): $(BUILD_KDPRIMARY_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BUILD_KDPRIMARY_SOURCES) $(AVX2_OBJECT) -o $@

$(EVALUATE_KDPRIMARY_TARGET): $(EVALUATE_KDPRIMARY_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EVALUATE_KDPRIMARY_SOURCES) $(AVX2_OBJECT) -o $@

$(BUILD_KDPRIMARY2_TARGET): $(BUILD_KDPRIMARY2_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BUILD_KDPRIMARY2_SOURCES) $(AVX2_OBJECT) -o $@

$(EVALUATE_KDPRIMARY2_TARGET): $(EVALUATE_KDPRIMARY2_SOURCES) $(AVX2_OBJECT)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(EVALUATE_KDPRIMARY2_SOURCES) $(AVX2_OBJECT) -o $@

tools: $(VECTORIZE_C_TARGET) $(INSPECT_INDEX_TARGET) $(EVALUATE_C_TARGET) $(BENCH_SEARCH_TARGET) $(BUILD_KDTREE_TARGET) $(EVALUATE_KDTREE_TARGET) $(EVALUATE_HYBRID_TARGET) $(ANALYZE_REPAIR_POLICY_TARGET) $(BUILD_KDPRIMARY_TARGET) $(EVALUATE_KDPRIMARY_TARGET) $(BUILD_KDPRIMARY2_TARGET) $(EVALUATE_KDPRIMARY2_TARGET)

test: $(RAW_HTTP_TEST_TARGET) $(FASTVECTOR_TEST_TARGET) $(IVF8_INDEX_TEST_TARGET) $(IVF8_SEARCH_TEST_TARGET) $(FDPASS_TEST_TARGET) $(FDLB_TEST_TARGET) $(FD_QUEUE_TEST_TARGET) $(KDTREE_TEST_TARGET) $(KDTREE_REPAIR_TEST_TARGET) $(KDPRIMARY_TEST_TARGET) $(KDPRIMARY2_TEST_TARGET) tools
	./$(RAW_HTTP_TEST_TARGET)
	./$(FASTVECTOR_TEST_TARGET)
	./$(IVF8_INDEX_TEST_TARGET)
	./$(IVF8_SEARCH_TEST_TARGET)
	./$(FDPASS_TEST_TARGET)
	./$(FDLB_TEST_TARGET)
	./$(FD_QUEUE_TEST_TARGET)
	./$(KDTREE_TEST_TARGET)
	./$(KDTREE_REPAIR_TEST_TARGET)
	./$(KDPRIMARY_TEST_TARGET)
	./$(KDPRIMARY2_TEST_TARGET)

clean:
	rm -rf $(BUILD_DIR)
