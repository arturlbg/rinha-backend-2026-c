# AI Handoff: Architecture

## Main Modules

### API Entrypoint

- `src/main.c`
  - Parses environment variables.
  - Loads the selected index.
  - Builds `raw_http_app`.
  - Selects listen mode and execution mode.
  - Starts API as TCP, Unix HTTP, or fdpass service.
  - Supports search implementations: `scalar`, `avx2`, `kdprimary`, `kdprimary2`, `kdclass3`, `rf_kdclass3`.

### Raw HTTP Runtime

- `include/raw_http.h`
- `src/raw_http.c`
  - Raw HTTP parser and connection state machine.
  - `/ready`, `/fraud-score`, and `/debug/info`.
  - Direct TCP listen path.
  - Unix HTTP listen path.
  - fdpass epoll connection handling.
  - Optional metrics/debug timing.
  - Optional async request worker path, which is not the stable default.

### File Descriptor Passing API Path

- `src/fdpass.c`
  - API-side Unix control socket.
  - Receives accepted client TCP fds from LB via `SCM_RIGHTS`.
  - Registers received fds into raw HTTP epoll path or other execution modes.

### C Load Balancer

- `src/fdlb.c`
- `src/fdlb_main.c`
- `Dockerfile.fdlb`
  - Public listener on `:9999`.
  - Accepts TCP clients.
  - Does not parse HTTP and does not inspect payloads.
  - Sends accepted fds to API control sockets with `sendmsg + SCM_RIGHTS`.
  - Supports strategies: `round_robin`, `least_active`, `power_of_two`.
  - Stable/default strategy remains `round_robin`.
  - Contains experimental proxy code, but stable preview path is fdpass.

### Vectorization

- `src/fastvector.c`
- `include/fastvector.h`
  - Converts transaction JSON body into `int16_t vector[14]`.
  - This path is latency-sensitive and used by all search modes.

### IVF8 Index And Search

- `include/ivf8_index.h`
- `src/ivf8_index.c`
- `src/ivf8_search.c`
- `src/ivf8_search_avx2.c`
  - Historical fast approximate path.
  - AVX2 IVF8 is fast but imperfect: previously `FP/FN/Error = 13/14/0`.
  - Still useful for experiments and trace features.

### KD-primary2

- `include/kdprimary2.h`
- `src/kdprimary2.c`
- `tools/build_kdprimary2.c`
- `tools/evaluate_kdprimary2.c`
  - Self-contained exact KD-primary index.
  - Runtime does not need IVF8 mmap when `RINHA_SEARCH_IMPL=kdprimary2`.
  - Official checkpoint used KD-primary2 leaf64.
  - Detection is exact on official-local data.

### KD-class3

- `include/kdclass3.h`
- `src/kdclass3.c`
- `tools/build_kdclass3.c`
- `tools/evaluate_kdclass3.c`
  - Exact class-based KD index.
  - Computes enough nearest fraud and legit neighbors to decide by comparing:
    - third fraud distance (`F3`)
    - third legit distance (`L3`)
  - If `F3 < L3`, classify as fraud.
  - If `L3 < F3`, classify as legit.
  - Equal-distance ties require fallback unless configured otherwise.
  - Current official-local kdclass3-l64 result: no mismatches vs KD-primary2 and fallback count `0`.

### KD-class3 SIMD/Optimization Labs

- `src/kdclass3_simd.c`
- `src/kdclass3_bbox_avx2.c`
- `include/kdclass3_opt.h`
- `src/kdclass3_opt.c`
- `src/kdclass3_search_avx2.c`
- `tests/test_kdclass3_opt.c`
- `tools/evaluate_kdclass3_opt.c`
- `tools/evaluate_adaptive_probe.c`
  - Phase 20A/20B active lab files.
  - `RINHA_KDCLASS3_IMPL=simd_full` is an opt-in runtime canary.
  - Full SIMD bbox traversal improved offline timing.
  - Leaf checkpoint pruning worsened latency and should not be promoted.

### KD-tree Repair And Older Labs

- `include/kdtree.h`
- `src/kdtree.c`
- `src/kdtree_repair.c`
- `tools/build_kdtree.c`
- `tools/evaluate_hybrid.c`
  - Exact KD repair path used earlier to repair IVF8 boundary cases.
  - Perfect detection was possible, but KD repair was too slow as a tail path.

### Model Gate / RF Experiments

- `src/rf_gate_model.c`
- `tools/build_model_dataset.c`
- `tools/model_gate_study.py`
- `tools/build_rf_dataset.c`
  - Offline model-gate studies tried to answer easy cases without KD.
  - They did not meet the strict requirement of zero validation errors with fallback below 1%.
  - RF/kdclass3 canaries exist but are not stable defaults.

## Important Directories And Files

- `include/`: public internal headers for runtime modules.
- `src/`: production C runtime and LB implementation.
- `tests/`: unit tests.
- `tools/`: offline builders/evaluators/benchmarks.
- `scripts/`: benchmark and diagnostic PowerShell helpers.
- `release/`: local release artifacts copied into release images; `.bin` files are ignored by Git.
- `tmp/`: generated local data, indexes, results; ignored by Git.
- `Dockerfile`: normal build/test image.
- `Dockerfile.release`: API release image.
- `Dockerfile.fdlb`: C fdlb release image.
- `docker-compose.yml`: current stable official-compatible root compose.
- `docker-compose.preview-kdclass3-fdpass.yml`: local preview compose using local images.
- `docker-compose.preview-kdclass3.yml`: additional kdclass3 preview/diagnostic compose.

## Runtime Flow: Official-Compatible Stable Path

Stable root topology:

```text
k6/client
  -> TCP :9999
  -> C fdlb
  -> accept client fd
  -> send fd via SCM_RIGHTS to /sockets/api1.ctrl or /sockets/api2.ctrl
  -> C API fdpass receiver
  -> epoll reactor
  -> raw_http_conn parser
  -> fastvector JSON -> int16[14]
  -> kdclass3 exact classifier
  -> prebuilt HTTP response
  -> write response on original client fd
```

The LB does not parse HTTP. It only moves accepted file descriptors.

The API reactor owns socket reads and writes. Worker-thread modes exist but are not the stable default.

## Data Flow

### Request Data

Input:

- HTTP POST `/fraud-score`.
- JSON body from official local test data.

Transformation:

- `raw_http.c` parses HTTP framing.
- `fastvector.c` extracts fields and generates `int16_t vector[14]`.
- Selected search implementation returns fraud count or direct fraud/legit decision.

Output:

- Prebuilt response for approved or rejected classification.

### Index Data

Important external/generated files:

- `release/kdprimary2.bin`
- `release/kdclass3.bin`
- `tmp/kdprimary2/*.bin`
- `tmp/kdclass3/*.bin`

These are generated binary indexes and must not be tracked in Git.

Source data for builders:

- `C:\Users\Usuario\Documents\rinha-backend\rinha-2026-go\release\index.bin`
- This is the Go-generated reference index containing 3,000,000 vectors.

Official-local test data:

- `C:\Users\Usuario\Documents\rinha-backend\rinha-de-backend-2026\test\test-data.json`

## Critical Algorithms

### Fastvector

Maps transaction JSON to the same 14-dimensional integer feature space expected by the indexes. Any semantic change here can break parity and must be validated with full official-local evaluation.

### KD-primary2 Exact Search

Self-contained leaf-bucket KD index with exact top-5 behavior. It was the first official perfect-detection checkpoint, but runtime p99 remained above the 1ms target.

### KD-class3 Exact Classifier

Instead of recovering top-5 nearest neighbors directly, kdclass3 searches fraud and legit class trees until it can compare the third-nearest fraud and third-nearest legit distances. Because the response threshold is majority top-5 fraud, comparing `F3` and `L3` is enough:

- If three fraud points are closer than the third legit, top-5 majority is fraud.
- If three legit points are closer than the third fraud, top-5 majority is legit.
- Equal-distance edge cases need fallback.

On official-local data, kdclass3-l64 has matched KD-primary2 exactly with zero fallback.

### C fdlb

The LB accepts client connections and passes fds to APIs. It intentionally does not inspect payloads. This preserves official architecture while avoiding nginx/haproxy overhead.

## Important Environment Variables

### Search Selection

- `RINHA_SEARCH_IMPL=scalar|avx2|kdprimary|kdprimary2|kdclass3|rf_kdclass3`
- `RINHA_KDPRIMARY2_PATH=/app/resources/kdprimary2.bin`
- `RINHA_KDPRIMARY2_TOUCH=true|false`
- `RINHA_KDCLASS3_PATH=/app/resources/kdclass3.bin`
- `RINHA_KDCLASS3_TOUCH=true|false`
- `RINHA_KDCLASS3_FALLBACK=none|kdprimary2`
- `RINHA_KDCLASS3_IMPL=baseline|simd_full`
- `RINHA_KDCLASS3_POPULATE=true|false`
- `RINHA_KDCLASS3_MLOCK=true|false`
- `RINHA_KDCLASS3_MADVISE=off|willneed|random|sequential|hugepage|nohugepage`

### API Runtime

- `RINHA_ADDR=:8080`
- `RINHA_LISTEN_MODE=tcp|unix_http|fdpass`
- `RINHA_EXEC_MODE=per_connection|worker_pool|epoll`
- `RINHA_API_PROCESS_MODE=sync|async_worker`
- `RINHA_API_WORKERS=N`
- `RINHA_API_PROCESSES=N`
- `RINHA_FD_QUEUE_SIZE=N`
- `RINHA_UNIX_SOCKET=/sockets/api1.ctrl`
- `RINHA_METRICS_ENABLED=true|false`
- `RINHA_DEBUG_TIMING=true|false`

### Epoll Tuning

- `RINHA_EPOLL_IDLE_US`
- `RINHA_EPOLL_BUSY_POLL_US`
- `RINHA_EPOLL_BUSY_POLL_BUDGET`
- `RINHA_EPOLL_PREFER_BUSY_POLL`

### Load Balancer

- `RINHA_LB_ADDR=:9999`
- `RINHA_FDPASS_UPSTREAMS=/sockets/api1.ctrl,/sockets/api2.ctrl`
- `RINHA_FDLB_STRATEGY=round_robin|least_active|power_of_two`
- `RINHA_LB_CONNECT_RETRY_MS`
- `RINHA_LB_STARTUP_TIMEOUT_MS`

## External Dependencies

- Docker Desktop with linux/amd64 build support.
- GCC toolchain inside Docker.
- PowerShell on Windows host.
- `k6` available locally for benchmark runs.
- External Go-generated index file:
  - `C:\Users\Usuario\Documents\rinha-backend\rinha-2026-go\release\index.bin`
- Official local Rinha test directory:
  - `C:\Users\Usuario\Documents\rinha-backend\rinha-de-backend-2026\test`

## Submission Compatibility Notes

Do not use direct TCP single-API diagnostic mode as a final submission topology.

Do not use host networking, privileged containers, nginx, haproxy, or direct bind-mounted indexes for submission candidates.

Final images must embed required index files under `/app/resources/`.
