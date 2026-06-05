# AI Handoff: Project Brief

## What This Project Is

This repository is the C implementation of a Rinha de Backend 2026 fraud-score service.

The service receives HTTP requests for `/fraud-score`, vectorizes the transaction JSON into a fixed `int16_t[14]` feature vector, and returns an approval decision based on a low-latency nearest-neighbor/classifier index.

The current official-compatible architecture is:

- Custom C file-descriptor-passing load balancer (`fdlb`).
- Two C API containers.
- Unix domain control sockets for passing accepted TCP client file descriptors from the LB to APIs via `SCM_RIGHTS`.
- API-side raw HTTP parser and epoll reactor.
- `kdclass3` exact classifier index as the current stable compose topology.
- `kdprimary2` remains the default release build index unless explicitly overridden.

The repository also contains many offline labs and canaries from previous phases: IVF8, KD-tree repair, KD-primary v1/v2, model gates, RF gates, async workers, and runtime diagnostics.

## Main Goal

Maximize Rinha score while preserving exact fraud classification.

Target state:

- `FP/FN/Error = 0/0/0`.
- HTTP errors = 0.
- Official-compatible topology.
- p99 below 1ms if possible.
- Final score close to 6000.

The detection problem is effectively solved. The remaining work is almost entirely p99/runtime/topology reduction.

## Performance And Business Objective

Rinha score combines detection quality and latency.

Known important scores/results:

- Official checkpoint, C fdlb + 2 APIs + KD-primary2 leaf64:
  - Official p99: `1.82ms`.
  - Final score: `5740.81`.
  - Detection score: `3000.00`.
  - `FP/FN/Error = 0/0/0`.
  - HTTP errors: `0`.
- Stable root compose now points at public kdclass3-l64 images:
  - LB image: `ghcr.io/arturlbg/rinha-backend-2026-c:fdlb`.
  - API image: `ghcr.io/arturlbg/rinha-backend-2026-c:kdclass3-l64`.
  - LB resources: `0.16 CPU / 30MB`.
  - API resources: `0.42 CPU / 160MB` each.
- Phase 12C diagnostic showed kdclass3 is faster inside the app:
  - kdprimary2 avg search: about `129-133us`.
  - kdclass3 avg search: about `49us`.
  - kdclass3 fallback count: `0`.
- Phase 20B opt-in `kdclass3` SIMD canary improved offline timing but did not win noisy local k6.

## Current Constraints

Official-compatible constraints:

- Architecture must expose public port `9999` through one LB service.
- Must have LB plus at least two API services.
- Must use Docker bridge networking.
- No host networking.
- No privileged mode.
- Total CPU must be <= `1.0`.
- Total memory must be <= `350MB`.
- Linux amd64 images.
- API contract must not change.
- No payload lookup, request memorization, row-index lookup, or query-hash lookup.
- Generated `.bin` index files must not be tracked in Git.

Repository constraints:

- Do not modify the Go repo or official Rinha repo unless explicitly requested.
- Do not make experimental canaries default without repeatable validation.
- Keep `kdclass3` opt-in by `RINHA_SEARCH_IMPL=kdclass3`.
- Keep `kdclass3` SIMD variant opt-in by `RINHA_KDCLASS3_IMPL=simd_full`.
- Keep `kdprimary2` as `Dockerfile.release` default unless explicitly changed.
- Root `docker-compose.yml` is a stable runnable compose and should not be casually deleted.

## What Success Means

Short-term success:

- A clean, reproducible official-compatible candidate.
- `FP/FN/Error = 0/0/0`.
- HTTP errors = 0.
- Stable p99 improvement over the `1.82ms` official checkpoint.
- No memory/OOM/restart risk.

Long-term success:

- p99 below `1ms`.
- Final score close to `6000`.
- Exactness preserved without runtime lookup tricks.
- Source, Dockerfiles, and compose files remain understandable enough for another agent or human to maintain.

## Current Handoff Warning

At handoff time, the worktree is dirty with active Phase 20A/20B changes. These changes are not all committed and should not be reverted blindly.

Known dirty files include:

- `Makefile`
- `include/config.h`
- `include/kdclass3.h`
- `include/raw_http.h`
- `src/fdpass.c`
- `src/kdclass3.c`
- `src/main.c`
- `src/raw_http.c`
- `tests/test_config.c`
- `tests/test_kdclass3.c`
- `tools/evaluate_kdclass3.c`
- New untracked Phase 20 files such as `src/kdclass3_simd.c`, `src/kdclass3_bbox_avx2.c`, `src/kdclass3_opt.c`, and related tests/tools.

Treat the current worktree as an active lab state, not as a clean release baseline.
