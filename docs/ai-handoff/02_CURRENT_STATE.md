# AI Handoff: Current State

## What Currently Works

### Stable Official-Compatible Runtime

The repository has a working official-compatible topology:

- `docker-compose.yml`
- C fdlb service.
- Two C API services.
- Bridge network.
- Public port `9999` exposed only by LB.
- Shared `sockets` volume for fdpass control sockets.
- API mode: fdpass + epoll + sync.
- Search mode in root compose: `kdclass3`.

Root compose currently uses public images:

- LB: `ghcr.io/arturlbg/rinha-backend-2026-c:fdlb`
- API: `ghcr.io/arturlbg/rinha-backend-2026-c:kdclass3-l64`

Resource split in root compose:

- LB: `0.16 CPU / 30MB`
- API1: `0.42 CPU / 160MB`
- API2: `0.42 CPU / 160MB`

### Release Images

`Dockerfile.release` builds the API release image and can embed either kdprimary2 or kdclass3:

- Default build arg: `RINHA_RELEASE_INDEX=kdprimary2`
- kdclass3 build:
  - `--build-arg RINHA_RELEASE_INDEX=kdclass3`

`Dockerfile.fdlb` builds the C load balancer image.

### Exact Search/Classifiers

Working exact paths:

- `kdprimary2`: official checkpoint path, exact on official-local data.
- `kdclass3`: exact on official-local data, fallback count zero in known evaluations.

### Unit And Tool Coverage

Tests and tools exist for:

- Raw HTTP parser.
- Fastvector.
- IVF8.
- KD tree.
- KD-primary.
- KD-primary2.
- KD-class3.
- fdlb/fdpass basics.
- Config parsing.
- Offline evaluators/builders.

### Current Known Good Validation From Recent Phases

Phase 20B validation passed:

- Docker test image build.
- Mounted test suite.
- kdclass3 official-local evaluation.
- kdclass3 perturb/reference evaluation.
- API release image build for kdclass3.
- API release image build for kdprimary2 default.
- C fdlb image build.
- `docker compose config` for stable preview/root compose.
- `git diff --check` with only CRLF warnings.

## What Is Partially Implemented

### Phase 20B kdclass3 SIMD Runtime Canary

Partially implemented and uncommitted at handoff:

- `RINHA_KDCLASS3_IMPL=baseline|simd_full`
- `src/kdclass3_simd.c`
- `src/kdclass3_bbox_avx2.c`
- `include/kdclass3_opt.h`
- `tests/test_kdclass3_opt.c`
- `tools/evaluate_kdclass3_opt.c`

Status:

- Offline official-local evaluation: exact, zero fallback, faster than baseline.
- Perturb/reference evaluation: exact, faster than baseline.
- Noisy local k6: did not beat baseline; do not promote.
- Keep as opt-in canary only.

### KD-class3 Optimization Lab

Phase 20A explored several variants:

- Full 14-dim SIMD bbox lower bound: useful.
- Leaf checkpoint pruning: worsened latency.
- Adaptive probe experiments: not promoted.

These files are experimental and should not be made default without fresh validation.

### Async Worker Mode

`RINHA_API_PROCESS_MODE=async_worker` exists.

Status:

- Implemented as request-level workers, not socket-owning workers.
- Validation showed worse p99 than sync.
- Keep `sync` as stable default.

### fdlb Scheduling Strategies

`RINHA_FDLB_STRATEGY` supports:

- `round_robin`
- `least_active`
- `power_of_two`

Status:

- Strategies were tested.
- None beat round-robin consistently.
- Keep `round_robin` as stable default.

### Model/RF Gates

Model-gate and RF experiments exist but did not satisfy runtime-promising criteria.

Status:

- No model gate reached zero validation errors with fallback below 1%.
- Do not integrate as default.
- Treat as offline research unless better ML tooling is introduced.

## What Is Not Implemented Or Not Recommended

Not implemented as stable/default:

- XGBoost or LightGBM runtime gate.
- C inference model gate with proven fallback below 1%.
- KD-class3 SIMD as default.
- Direct TCP single API as final topology.
- nginx/haproxy topology.
- Host networking.
- Privileged mode.
- Payload lookup or request memorization.

Not recommended based on previous evidence:

- IVF8 as final detector: fast but imperfect.
- KD repair as tail path: perfect but p99 tail too high.
- KD-primary2 as the only p99 path if kdclass3 is available.
- Async worker mode for current workload.
- fdlb least-active or power-of-two as default.
- Leaf checkpoint pruning in kdclass3.
- LTO/v3/PGO release profiles unless k6 proves them in the current environment.

## Current Branch And Worktree Assumptions

Current branch observed at handoff:

```text
main...origin/main
```

Current dirty files observed:

```text
 M Makefile
 M include/config.h
 M include/kdclass3.h
 M include/raw_http.h
 M src/fdpass.c
 M src/kdclass3.c
 M src/main.c
 M src/raw_http.c
 M tests/test_config.c
 M tests/test_kdclass3.c
 M tools/evaluate_kdclass3.c
?? include/epoll_tuning.h
?? include/kdclass3_opt.h
?? src/kdclass3_bbox_avx2.c
?? src/kdclass3_opt.c
?? src/kdclass3_search_avx2.c
?? src/kdclass3_simd.c
?? tests/test_kdclass3_opt.c
?? tools/evaluate_adaptive_probe.c
?? tools/evaluate_kdclass3_opt.c
```

These changes are likely Phase 20A/20B active work. Do not revert or delete them without explicit approval.

## Current Known Behavior

### Official Checkpoint Behavior

The known official checkpoint used KD-primary2:

- Official p99: `1.82ms`
- Final score: `5740.81`
- Detection: `3000`
- `FP/FN/Error = 0/0/0`
- HTTP errors: `0`

### kdclass3 Behavior

Known kdclass3 behavior:

- Offline official-local: exact vs KD-primary2.
- Fallback count: `0`.
- Runtime app-internal search is much faster than KD-primary2.
- Local k6 can be noisy and may not reflect offline improvements.

### Local k6 Behavior

Local k6 is noisy and can produce much worse p99 than official runs. Do not make algorithm decisions from one local k6 run.

When comparing candidates:

- Alternate A/B order.
- Run at least 2 times, preferably 3.
- Compare same-session baselines.
- Track `http_req_waiting` p99.
- Confirm HTTP errors and restarts.

## Unresolved Questions

- Should Phase 20B `RINHA_KDCLASS3_IMPL=simd_full` be kept, committed, or discarded?
- Should `Dockerfile.release` default move from kdprimary2 to kdclass3, or stay conservative?
- Is the public `ghcr.io/arturlbg/rinha-backend-2026-c:kdclass3-l64` image exactly aligned with current source and release artifacts?
- Can kdclass3 consistently beat the KD-primary2 official checkpoint under official infrastructure?
- Is remaining p99 dominated by LB/network scheduling, Docker runtime, API write path, or test environment variance?
- Should root compose remain public-image based, or should local preview compose be the primary dev workflow?

## Phase 22/23: Worktree Cleaned and Committed (2026-06-04)

Three clean commits on top of origin/main:

1. `preserve kdclass3 simd canary` — All Phase 20 SIMD files (opt-in, `RINHA_KDCLASS3_IMPL=simd_full`)
2. `reduce fdlb tail blocking on upstream handoff` — Non-blocking sendmsg + lean EAGAIN fallback + compose defaults
3. `update ai handoff documentation` — Full project handoff docs

Native Linux test harness at `scripts/phase23-native-linux-matrix.sh`.

Next: run on Linux host to validate Phase 22 fdlb improvements under non-emulated conditions.

### p99 Attribution Results

Ran 4 k6 runs (same-session baseline):

| Run | p99_dur | p99_wait | Notes |
|-----|---------|----------|-------|
| baseline-1 (full LB) | 6.46ms | 6.36ms | Cold Docker networking |
| simd (full LB) | 3.83ms | 3.70ms | SIMD canary, warm networking |
| baseline-2 (full LB) | 2.20ms | 2.13ms | Warmest networking |
| lean-nb (lean LB + non-blocking) | 2.44ms | 2.40ms | LB optimizations applied |

**Key findings:**
- `http_req_waiting` is 97%+ of p99 duration — requests spend almost all time queued
- Classifier effective p99 is 55-62us — only 2.5-2.8% of best-case p99 (2.2ms)
- SIMD classifier improvement (~7us) cannot explain >4ms p99 variance
- Local k6 noise/variance is ~4ms between runs (Docker Desktop + Windows host)
- Docker networking warmup effect is significant (first run 6.46ms → third run 2.20ms)

### LB Improvements Applied

Two targeted changes to `src/fdlb.c`:

1. **Non-blocking sendmsg** (`fdlb_send_one_fd`): Added `MSG_DONTWAIT` to prevent head-of-line blocking when API control socket buffer is full. Previously, the LB would block on sendmsg, queuing all subsequent connections.

2. **Lean mode graceful fallback** (`lean_deliver_fd`): On EAGAIN/EWOULDBLOCK from non-blocking sendmsg, try next upstream instead of closing/reconnecting.

Preview compose updated:
- `RINHA_FDLB_LEAN=true` — removes feedback polling overhead
- `RINHA_FDLB_ACCEPT_BATCH=64` — batch accepts for fewer syscalls

### Working Validation

All unit tests pass. kdclass3 baseline and SIMD both exact (0/0/0 errors, 0 mismatches vs kdprimary2). Release images build for all configurations.

## Phase 24: Official Result (commit edff294)

**First official run with Phase 22 fdlb improvements:**

| Metric | Phase 24 | Previous Checkpoint | Delta |
|--------|----------|---------------------|-------|
| p99 | 1.649ms | ~1.82ms | **-171us (-9.4%)** |
| p99_score | 2782.86 | ~2740 | +42.86 |
| final_score | 5782.86 | ~5740.81 | **+42.05** |
| detection_score | 3000 | 3000 | — |
| FP/FN/Error | 0/0/0 | 0/0/0 | — |
| HTTP errors | 0 | 0 | — |

**What was active:** Non-blocking sendmsg, EAGAIN fallback, lean LB mode, ACCEPT_BATCH=16, kdclass3 baseline (SIMD opt-in only).

**Score formula:** Each -100us p99 improvement yields ~+27-30 points. To reach 6000 would require p99 = 1.0ms. More realistically, each ~50us reduction is worth ~13-15 points.

**Interpretation:** Phase 22 direction confirmed — LB queuing optimization is the right layer. But the gain (-171us) suggests the LB at 0.16 CPU may still be the bottleneck. With 900 rps, the LB performs accept + sendmsg + round-robin selection. Under cgroup CPU throttling, even these lightweight operations can queue when the quota is exhausted, creating backlog queuing that propagates to p99.
