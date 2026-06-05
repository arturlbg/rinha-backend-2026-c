# AI Handoff: Work Completed

## Chronological Phase Summary

### Phase 1: C Runtime Bootstrap

- Created the initial C API runtime.
- Established Docker build/test flow.
- Added basic `/ready` and `/fraud-score` service structure.

### Phase 2: Raw HTTP Parser

- Implemented robust raw HTTP parser.
- Added parser tests for fragmented and pipelined requests.
- Avoided framework overhead.

Important files:

- `include/raw_http.h`
- `src/raw_http.c`
- `tests/test_raw_http.c`

### Phase 3: fastvector C

- Ported transaction vectorization to C.
- Matched Go vectorizer behavior.

Important files:

- `src/fastvector.c`
- `include/fastvector.h`
- `tests/test_fastvector.c`

Validation:

- Fastvector parity vs Go on 1000 official-local requests: `0` mismatches.

### Phase 4: mmap IVF8 Index Loader

- Implemented mmap loader for Go-generated `index.bin`.
- Confirmed reference vector layout and label access.

Important files:

- `include/ivf8_index.h`
- `src/ivf8_index.c`

### Phase 5: Scalar IVF8 Search

- Implemented scalar IVF8 search.
- Validated against Go IVF8 behavior.

Validation:

- 54,100 official-local queries vs Go IVF8 K4096 cap4096 probes8.
- `0` fraud_count mismatches.

### Phase 6: Runtime IVF8 Search

- Wired `/fraud-score` to real vectorization/search.
- Added scalar search runtime path.

### Phase 7: fdpass With Go fdlb

- Added fdpass API mode compatible with Go fdlb.
- API receives accepted client fds via Unix control sockets.

Important files:

- `src/fdpass.c`

### Phase 8A: Metrics And Diagnostics

- Added latency metrics and execution diagnostics.
- Added `/debug/info` support gated by environment flags.

### Phase 8B: AVX2 IVF8 Search

- Implemented AVX2 IVF8 search.
- AVX2 was faster but detection remained imperfect.

Known result:

- AVX2 IVF8: `FP/FN/Error = 13/14/0`.

### Phase 8C: epoll fdpass Execution

- Added epoll execution mode for fdpass.
- Improved connection handling vs older modes.

### Phase 8E: KD-tree Feasibility And Hybrid Repair

- Built exact KD-tree feasibility lab from Go-generated index vectors.
- Implemented offline hybrid IVF8 + KD exact repair evaluator.

Important results:

- KD-tree exact on full 54,100:
  - `FP/FN/Error = 0/0/0`.
  - p99 search about `2477us`.
- Hybrid `boundary23_far45`:
  - repair rate about `3.4067%`.
  - `FP/FN/Error = 0/0/0`.
  - p99 included slow KD repairs, not final-p99 friendly.

### Phase 8F: Runtime KD Repair

- Added opt-in runtime KD exact repair for IVF8 AVX2.
- Repair disabled by default.

Known result:

- Perfect detection with repair.
- p99 worsened due repair tail.

### Phase 8G: Repair Policy Mining

- Mined narrower repair policies.

Known results:

- `minimal_v1`: repair rate `0.887%`, but `FP/FN/Error = 3/3/0`.
- `perfect_v1`: repair rate `3.015%`, perfect detection.
- Conclusion: could not get perfect detection below 1% repair.

### Phase 8H/8I: KD-primary Offline And Runtime

- Built self-contained KD-primary indexes.
- Integrated `RINHA_SEARCH_IMPL=kdprimary`.
- Found good offline timing but poor k6 runtime.

Known full offline kdprimary leaf32 result:

- `FP/FN/Error = 0/0/0`.
- avg about `243us`.
- p99 about `1018us`.
- memory about `87.97 MiB`.

### Phase 9A: KD-primary2 Exact Search

- Implemented KD-primary2 v2 format and search.
- Added AVX2-friendly SoA leaf block layout and bbox pruning.

Known offline leaf64 result:

- `FP/FN/Error = 0/0/0`.
- avg about `122us`.
- p95 about `240us`.
- p99 about `411us`.
- memory about `97.50 MiB`.

### Phase 9B/9C: Runtime Isolation And Topology Diagnostics

- Added runtime metrics/debug info.
- Added direct API diagnostic compose.
- Ran repeated matrix for direct vs fdpass.

Conclusion:

- KD-primary2 search itself was not the main p99 bottleneck.
- k6 p99 was mostly `http_req_waiting`.
- CPU throttling was tiny.
- Queueing/runtime/topology dominated.

### Phase 9D: Direct TCP epoll

- Added direct TCP epoll accept mode using same raw HTTP state machine.
- Direct diagnostic achieved much lower p99 than older direct modes.

Known diagnostic:

- Direct TCP epoll + KD-primary2 p99 best: about `2.33ms`.
- Not valid final topology because official requires LB + at least two APIs.

### Phase 9E/9F: Release Packaging And C fdlb

- Created API release image with embedded index.
- Implemented minimal C fdlb.
- Added official-compatible preview compose using C fdlb.
- Published checkpoint images when explicitly requested earlier.

Known local C fdlb preview result:

- p99 best about `3.22ms`.
- final_score about `5492.73`.
- `FP/FN/Error = 0/0/0`.

Known official checkpoint:

- p99 `1.82ms`.
- final_score `5740.81`.
- `FP/FN/Error = 0/0/0`.

### Phase 10A/10B: Resource Splits And Compiler Profiles

- Swept resource splits.
- Audited build flags.
- Tested `O3`, LTO, x86-64-v3/AVX2-tuned, and PGO-style ideas.

Conclusion:

- Offline search improved.
- k6 did not improve enough.
- Release profile reverted to stable behavior.

### Phase 10C: fdlb Scheduling

- Added/tested `round_robin`, `least_active`, and `power_of_two`.

Conclusion:

- Connection-level scheduling did not unlock p99.
- `round_robin` remains stable.

### Phase 10D: Async Request Workers

- Implemented request-level async worker path.
- Reactor owns sockets; workers own only KD search computation.
- Used eventfd for completion notification.

Conclusion:

- Async worsened p99.
- Keep `sync` default.

### Phase 10E/10F/10G: Model Gate Studies

- Built datasets and model-gate studies.
- Tried hand rules, custom forests/GBDT-like approaches, calibration ideas.

Known results:

- Best vector/poly models could reach zero validation errors only with fallback several percent.
- Strict target was fallback below 1% with zero validation errors.
- No runtime integration.

### Phase 11/12: kdclass3 And Runtime Diagnostics

- Implemented kdclass3 exact classifier.
- Built kdclass3-l64 index.
- Added kdclass3 canary compose and diagnostics.

Known Phase 12C diagnostic:

- kdclass3 avg search about `49us`.
- handler avg about `56us`.
- parser avg about `1.8-2.4us`.
- write path avg about `43-58us`.
- fallback count `0`.
- HTTP errors/parser/write errors `0`.

Known k6 diagnostic:

- kdprimary2 same-session p99 `4.16ms`.
- kdclass3 touch=true p99 `2.17ms`.
- kdclass3 touch=false p99 `2.30ms`.

### Cleanup Phase

- Removed obsolete compose and phase scripts.
- Kept root `docker-compose.yml`.
- Replaced root compose with stable official topology:
  - C fdlb.
  - kdclass3-l64 public image.
  - LB `0.16 CPU / 30MB`.
  - APIs `0.42 CPU / 160MB`.

Important commit:

- `90f2a51 Use stable kdclass3 topology as default compose`

### Phase 20A/20B: kdclass3 SIMD Optimization Lab

- Phase 20A found full 14-dim SIMD bbox traversal useful.
- Leaf checkpoint pruning worsened latency.
- Phase 20B added opt-in runtime canary:
  - `RINHA_KDCLASS3_IMPL=simd_full`

Recent validation:

- Test image build: passed.
- Mounted test suite: passed.
- Release builds: passed.
- Official-local kdclass3 baseline vs simd_full:
  - baseline avg effective `44.637us`, p99 `187.751us`.
  - simd_full avg effective `36.790us`, p99 `149.990us`.
  - `FP/FN/Error = 0/0/0`.
  - mismatches `0`.
  - fallback `0`.
- Perturb/reference 144,906 rows:
  - simd_full avg improvement about `18.9%`.
  - p99 improvement about `12.0%`.
  - exactness preserved.
- Same-session local k6:
  - very noisy.
  - simd_full did not beat baseline.
  - keep opt-in only.

## Recent Git History

Recent commits observed:

```text
90f2a51 Use stable kdclass3 topology as default compose
4923a55 delete obsolet files
bb40681 Add fdlb backlog and accept batch lab
b28e527 Add kdclass3 and RF lab experiments
d07e4ce Add offline kdclass3 exact search lab
a05b080 Add safe lean runtime experiments
469b628 Add experimental multiprocess API mode
17dc1aa model gate
d801db8 Add offline model gate feasibility tooling
9781251 Add optional async worker processing mode
19be037 Add fdlb scheduling experiments
48fd0f2 Add p99 sweep tooling and fdlb cleanup
d2d40a5 Add C fdlb KD-primary2 checkpoint candidate
c179b11 Add local KD-primary2 preview packaging
b6a4bb9 Add direct TCP epoll diagnostics
3b8d356 Add KD-primary2 runtime diagnostics
c855798 Add opt-in KD-primary search mode
a13d750 Add KD-primary feasibility lab
71b82e2 Add opt-in KD-tree repair
1404d9c Add KD-tree hybrid evaluation
```

## Validation Principle Established

Every new search/runtime candidate must pass in this order:

1. Unit tests.
2. Full official-local offline exactness.
3. Perturb/reference exactness if the path changes search math.
4. Release image build.
5. Compose config.
6. Smoke.
7. Same-session A/B k6 with repeated runs.
8. No HTTP errors, OOMs, or restarts.

### Phase 22: p99 Attribution and LB Optimization (2026-06-04)

- Preserved dirty Phase 20 worktree with safety snapshot under `docs/ai-handoff/snapshots/`.
- Built test image, ran full test suite — all unit tests passed.
- Validated kdclass3 baseline exactness (0/0/0 errors, 0 mismatches vs kdprimary2, 0 fallback).
- Validated kdclass3 SIMD exactness (0/0/0 errors, 0 mismatches, 0 fallback).
- Built all release images (kdprimary2, kdclass3-l64, fdlb).
- Ran 4 same-session k6 attribution runs:
  - Confirmed `http_req_waiting` is 97%+ of p99 (classifier is 55-62us vs 2.2-6.5ms total).
  - Confirmed local k6 noise/variance is ~4ms between runs (Docker Desktop on Windows).
- Implemented LB improvements:
  - `fdlb_send_one_fd`: Changed from blocking sendmsg to `MSG_NOSIGNAL | MSG_DONTWAIT`.
  - `lean_deliver_fd`: On EAGAIN, fall through to next upstream instead of closing/reconnecting.
  - Preview compose: set `RINHA_FDLB_LEAN=true` and `RINHA_FDLB_ACCEPT_BATCH=64`.
- Rebuilt C fdlb image and validated compose smoke test.
- Confirmed API response path is already optimal (precomputed static responses with partial-write tracking).

### Phase 23: Worktree Cleanup and Native Linux Prep (2026-06-04)

- Committed Phase 20 SIMD as `7a88c33 preserve kdclass3 simd canary` (20 files, 3430+ insertions).
- Committed Phase 22 fdlb as `a64a7ae reduce fdlb tail blocking on upstream handoff` (2 files, 7+ insertions).
- Committed handoff docs as `c1466a8 update ai handoff documentation` (11 files, 2227+ insertions).
- Worktree is clean — 3 commits ahead of origin/main, zero dirty/untracked files.
- Created `scripts/phase23-native-linux-matrix.sh` — automated 7-variant × 3-repetition k6 test harness for native Linux.

### Phase 24: Official Submission Candidate (commit edff294)

- Submitted root compose with GHCR images, kdclass3 baseline, SIMD opt-in.
- Active: MSG_DONTWAIT, EAGAIN fallback, lean LB mode, ACCEPT_BATCH=16.
- Resource split: LB 0.16 CPU / 30MB, APIs 0.42 CPU / 160MB each.
- **Official result:** p99 1.649ms (-171us, -9.4%), score 5782.86 (+42), 0/0/0 errors, 0 HTTP errors.
- Direction confirmed: LB queuing optimization is the right layer.

### Phase 25: LB CPU Reallocation Candidate (in progress)

- Hypothesis: LB at 0.16 CPU may be throttled under 900 rps burst.
- Increased LB CPU from 0.16 to 0.25 (+56%), reduced APIs from 0.42 to 0.375 each (-10.7%).
- Total: 1.00 CPU / 350MB preserved.
- kdclass3 baseline, SIMD opt-in, Phase 22 fdlb improvements retained.
