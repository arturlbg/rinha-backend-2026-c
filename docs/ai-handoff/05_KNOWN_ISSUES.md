# AI Handoff: Known Issues And Risks

## Dirty Worktree Risk

The repository is not clean at handoff. Active Phase 20A/20B files are modified/untracked.

Risk:

- A new agent may accidentally revert or delete active work.

Mitigation:

- Start with `git status --short --branch`.
- Ask before reverting/deleting anything.
- Separate documentation, cleanup, and performance commits.

## Local k6 Noise

Local k6 runs can be much noisier than official runs.

Examples:

- Official KD-primary2 checkpoint p99: `1.82ms`.
- Local same-session p99s have ranged from about `2ms` to tens of milliseconds.
- Phase 20B k6 was especially noisy, with p99 above `50ms` in some runs.

Risk:

- A good offline improvement may appear bad locally.
- A bad local result may reflect thermal/Docker/client noise.

Mitigation:

- Always run same-session A/B.
- Alternate order.
- Run multiple repetitions.
- Compare `http_req_waiting` p99.
- Treat official-compatible repeatability as more important than single-run best.

## Offline Speed Does Not Guarantee k6 p99

Repeated finding:

- Search microbenchmarks improve.
- App-internal timing improves.
- k6 p99 may not improve.

Likely reasons:

- LB queueing.
- Docker bridge/network scheduling.
- API write path.
- fdpass queueing.
- k6/client scheduling.
- Keep-alive connection distribution.

Mitigation:

- Instrument before optimizing.
- Compare direct TCP epoll vs fdlb.
- Keep `/debug/info` timing optional.

## p99 Constraint Makes Tail Fallbacks Dangerous

If fallback rate is >= 1%, global p99 likely includes fallback latency.

This killed:

- KD repair as tail path.
- Model gates with fallback above 1%.

Mitigation:

- Any gate must have fallback below 1%, ideally below 0.5%.
- Validate zero errors on held-out data, not just full official-local.

## kdclass3 Tie/Fallback Semantics Are Critical

kdclass3 decision depends on comparing third-nearest fraud and third-nearest legit distances.

Risk:

- Mishandling equal distances can change exactness.

Current known behavior:

- Official-local kdclass3-l64 fallback count is `0`.
- `RINHA_KDCLASS3_FALLBACK=none` is safe only if there are no ties/fallbacks in evaluated data.

Mitigation:

- Always run `evaluate_kdclass3` after search changes.
- Track fallback count.
- Compare approved decisions vs KD-primary2.

## SIMD Canaries Need AVX2 Gating

`RINHA_KDCLASS3_IMPL=simd_full` requires AVX2 support.

Risk:

- Running on unsupported CPU must not crash.

Current intended behavior:

- If AVX2 unsupported or impl invalid, warn and use baseline.

Mitigation:

- Keep baseline available.
- Test invalid config fallback.
- Do not compile away scalar/baseline path.

## fdlb fd Lifecycle Is Fragile

LB accepts client fds and sends them to APIs.

Risk points:

- fd leaks on `sendmsg` failure.
- double-close.
- blocking `sendmsg`.
- API socket readiness.
- LB starts before API control sockets exist.
- Uneven connection distribution under keep-alive.

Mitigation:

- Smoke `/ready` repeatedly.
- Check API/LB logs for restarts.
- Keep per-connection logging disabled by default.
- Use counters only when diagnostics are enabled.

## Async Worker Mode Worsened p99

`RINHA_API_PROCESS_MODE=async_worker` exists but is not recommended.

Risk:

- New agent may assume async improves throughput.

Known result:

- Async request workers worsened p99 locally.

Mitigation:

- Keep `RINHA_API_PROCESS_MODE=sync` unless a dedicated experiment proves otherwise.

## fdlb Strategies Did Not Help

`least_active` and `power_of_two` exist.

Known result:

- They did not beat round-robin consistently.

Mitigation:

- Keep `RINHA_FDLB_STRATEGY=round_robin`.
- Do not spend more time here unless new metrics show imbalance.

## Release Default And Root Compose Differ

`Dockerfile.release` default:

- `RINHA_RELEASE_INDEX=kdprimary2`

Root `docker-compose.yml`:

- Uses public API image `ghcr.io/arturlbg/rinha-backend-2026-c:kdclass3-l64`.

Risk:

- A new local build without build args creates kdprimary2, not kdclass3.

Mitigation:

- For kdclass3 local image:

```powershell
docker build --platform linux/amd64 -f Dockerfile.release --build-arg RINHA_RELEASE_INDEX=kdclass3 -t rinha-c-preview:kdclass3-l64 .
```

## Generated Artifacts Are Required But Ignored

Release images need:

- `release/kdprimary2.bin`
- `release/kdclass3.bin`

These are ignored by Git.

Risk:

- Release build fails if artifact is missing.

Mitigation:

- Build/copy artifacts from `tmp/`.
- Never commit `.bin` files.

## CRLF Warnings

`git diff --check` has previously produced CRLF warnings but no blocking whitespace errors.

Risk:

- Line ending churn.

Mitigation:

- Do not mass-format.
- Keep changes minimal.

## PowerShell k6 Redirection Quirk

k6 writes useful info to stderr. With `$ErrorActionPreference = 'Stop'`, PowerShell can treat k6 stderr output as failure even when k6 exits successfully.

Mitigation:

- Use scripts that handle k6 output carefully.
- Or use `Start-Process`/separate stdout-stderr files.
- Do not conclude benchmark failed from stderr alone; check exit code and summary.

## Direct TCP Diagnostic Is Not Submission-Valid

Direct single API can be faster.

Risk:

- Tempting to treat direct p99 as candidate p99.

Mitigation:

- Use direct TCP only to isolate runtime behavior.
- Final topology must be LB + at least two APIs.

## Model Gate Overfitting

Model-gate studies found thresholds/models that looked good on training/full data but failed validation.

Risk:

- Hidden official data may fail.

Mitigation:

- Require validation zero errors.
- Require fallback below 1%.
- No row lookup or request hash lookup.

## Memory Budget Is Tight

Official limit:

- Total memory <= `350MB`.

Stable root split:

- LB `30MB`.
- APIs `160MB` each.
- Total `350MB`.

Risk:

- More processes or extra indexes can OOM.

Mitigation:

- mmap indexes read-only.
- Avoid loading both kdprimary2 and kdclass3 unless explicitly required.
- Check container restarts and memory after k6.

## Unresolved Runtime Attribution

The app compute path is already very fast. Remaining p99 likely comes from:

- LB/network.
- Docker scheduling.
- fdpass queueing.
- response write path.
- kernel socket buffers.

Next instrumentation should attribute these before more search math.

## Phase 22 Findings: p99 Bottleneck Confirmed

p99 attribution data (4 same-session k6 runs):

- **Classifier effective p99:** 55-62us (2.5-2.8% of total p99).
- **http_req_waiting:** 97%+ of p99 duration.
- **Docker Desktop noise:** ~4ms variance between runs (Windows/macOS emulation overhead).
- **LB improvements applied:** Non-blocking sendmsg + lean mode + accept batching should reduce head-of-line blocking.
- **Write path:** Already optimal (static precomputed responses, no dynamic formatting).
- **p99 variance is environmental:** Not algorithmic — the classifier and API runtime are already near-optimal.

**Conclusion:** Further p99 reduction requires either official Linux host testing or deeper Docker/kernel tuning. The local k6 environment is too noisy for sub-ms optimization decisions.
