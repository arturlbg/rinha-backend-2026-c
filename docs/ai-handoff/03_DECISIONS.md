# AI Handoff: Technical Decisions

## Decision: Use A Custom C Runtime

Decision:

- Implement raw HTTP parsing, vectorization, search, and fdpass runtime in C.

Why:

- Rinha scoring is latency dominated after detection is solved.
- Avoid framework overhead.
- Control memory layout, mmap, cache behavior, and epoll.

Rejected alternatives:

- Go API runtime for final C path.
- nginx/haproxy fronting APIs.
- Higher-level HTTP frameworks.

Trade-off:

- More fragile code.
- Requires strict tests for parser, fd lifecycle, and memory-mapped indexes.

Do not change casually:

- Raw HTTP parser behavior.
- Prebuilt response mapping.
- fd ownership rules.

## Decision: Use fdpass LB/API Topology

Decision:

- LB accepts TCP clients and passes accepted fds to APIs through Unix control sockets with `SCM_RIGHTS`.

Why:

- Preserves one public LB service.
- Lets API containers handle original client sockets.
- Avoids HTTP parsing/proxying in LB.

Rejected alternatives:

- Go fdlb as permanent dependency.
- nginx/haproxy.
- Direct single API on port `9999` as final architecture.
- Host networking.

Trade-off:

- fd lifecycle is delicate.
- Control sockets and API readiness must be correct.
- Debugging queueing latency is harder.

Do not change casually:

- LB must not inspect payloads.
- LB must not contain fraud logic.
- LB must close its local accepted fd after successful `sendmsg`.
- API reactor owns client socket reads/writes.

## Decision: Keep Root Compose Official-Compatible

Decision:

- Root `docker-compose.yml` is retained as a default runnable official-compatible topology.

Why:

- New agents/humans expect root compose to work.
- It is the safest default entry point.

Current root topology:

- C fdlb.
- Two APIs.
- kdclass3-l64 public API image.
- fdpass + epoll + sync.

Rejected alternatives:

- Deleting root compose during cleanup.
- Replacing root compose with diagnostics.

Trade-off:

- Root compose uses public images, while local preview composes may use local tags.
- `Dockerfile.release` still defaults to kdprimary2.

Do not change casually:

- Do not reintroduce nginx/proxy/multiprocess/async/backlog experiments into root compose.

## Decision: Keep kdprimary2 As Conservative Release Default

Decision:

- `Dockerfile.release` default remains `RINHA_RELEASE_INDEX=kdprimary2`.
- kdclass3 release image is built with `--build-arg RINHA_RELEASE_INDEX=kdclass3`.

Why:

- KD-primary2 has a known official checkpoint at p99 `1.82ms`.
- kdclass3 is promising and used by stable compose, but default release behavior was intentionally conservative.

Rejected alternatives:

- Making kdclass3 the Dockerfile default without explicit promotion.

Trade-off:

- A new agent must be careful to pass the build arg for kdclass3.

Do not change casually:

- Default release index.
- Embedded resource paths.

## Decision: Use kdclass3 For Current Stable Candidate

Decision:

- kdclass3 is the current highest-potential exact classifier path.

Why:

- It matches KD-primary2 on official-local data.
- It has zero fallback in known official-local evaluation.
- It is faster inside the app than KD-primary2.

Rejected alternatives:

- IVF8 as final detector: imperfect.
- KD repair as final path: p99 tail too high.
- KD-primary2 as the only future path: exact but more expensive.

Trade-off:

- kdclass3 correctness depends on class-tree distance logic and tie handling.
- Runtime k6 gains have been environment-sensitive.

Do not change casually:

- `F3` vs `L3` exact decision semantics.
- Fallback behavior.
- Tie handling.

## Decision: Keep kdclass3 SIMD As Opt-In

Decision:

- Phase 20B SIMD implementation is controlled by `RINHA_KDCLASS3_IMPL=simd_full`.
- Default remains `baseline`.

Why:

- Offline evaluation improved.
- Local k6 did not improve in noisy same-session runs.
- Runtime promotion needs repeatability.

Rejected alternatives:

- Making `simd_full` default after only offline wins.
- Promoting leaf checkpoint pruning.

Trade-off:

- More code paths to maintain.
- Requires AVX2 support checks.

Do not change casually:

- Fallback-to-baseline behavior when AVX2 is unavailable.
- Baseline implementation.

## Decision: Keep sync epoll As Stable API Mode

Decision:

- `RINHA_API_PROCESS_MODE=sync` remains the stable mode.

Why:

- Async request-worker mode was implemented and validated but worsened p99.
- Current workload and CPU quotas likely make extra handoff/context switching unhelpful.

Rejected alternatives:

- Worker threads owning whole keep-alive sockets.
- Async worker as default.

Trade-off:

- The reactor can still block on search.
- Simpler and more stable than worker handoff.

Do not change casually:

- Socket ownership: reactor should own reads/writes.

## Decision: Keep fdlb round_robin Strategy

Decision:

- `RINHA_FDLB_STRATEGY=round_robin` remains stable.

Why:

- `least_active` and `power_of_two` were tested and did not beat round-robin consistently.

Rejected alternatives:

- Connection-level least-active as default.
- Power-of-two as default.

Trade-off:

- Round-robin by connection can still imbalance request load under keep-alive.
- Simplicity and stability win until better evidence exists.

## Decision: Generated Binary Indexes Stay Out Of Git

Decision:

- `.bin` files are ignored.
- `tmp/` is ignored.

Why:

- Index files are large generated artifacts.
- Release images embed them at build time from local `release/`.

Do not change casually:

- Do not track `release/*.bin`.
- Do not track `tmp/` artifacts.

## Decision: No Runtime Lookup Tricks

Decision:

- No payload lookup, row-index lookup, query-hash lookup, or memorized official test cases.

Why:

- Violates the spirit and likely rules of the benchmark.
- Makes hidden/offical evaluation invalid.

Do not change casually:

- Any feature that keys on request identity rather than generic transaction/vector features must be rejected.

## Decision: Treat Offline Speed As Necessary But Not Sufficient

Decision:

- Do not promote a search implementation based only on offline evaluator timing.

Why:

- Multiple phases showed offline wins do not always translate to k6 p99.
- Local k6 can be dominated by queueing, waiting, write path, Docker runtime, and LB topology.

Validation required:

- Full official-local accuracy.
- Same-session k6 A/B.
- Repeated runs.
- No HTTP errors.
- No OOM/restarts.
