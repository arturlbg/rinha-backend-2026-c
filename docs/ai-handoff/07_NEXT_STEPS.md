# AI Handoff: Recommended Next Steps

## Current Status (after Phase 23 commits)

Worktree is clean — 3 commits ahead of origin/main:
1. `7a88c33` preserve kdclass3 simd canary
2. `a64a7ae` reduce fdlb tail blocking on upstream handoff
3. `c1466a8` update ai handoff documentation

Native Linux test harness: `scripts/phase23-native-linux-matrix.sh`

## Priority 1: Run Native Linux A/B Matrix

Goal: Validate Phase 22 fdlb improvements on non-emulated Linux.

Run `scripts/phase23-native-linux-matrix.sh` on a native Linux host.
Tests 7 variants × 3 repetitions:

| Variant | LEAN | ACCEPT_BATCH | Description |
|---------|------|-------------|-------------|
| A-baseline | false | 1 | Current stable fdlb |
| B-msgdontwait | false | 1 | Non-blocking sendmsg only |
| C-lean | true | 1 | Lean mode only |
| D-lean-batch16 | true | 16 | Lean + moderate batching |
| E-lean-batch32 | true | 32 | Lean + medium batching |
| F-lean-batch64 | true | 64 | Lean + high batching (current default) |
| G-lean-batch128 | true | 128 | Lean + max batching |

Expected duration: ~40-50 minutes (7 × 3 × ~2min k6 + startup/cooldown).

Decision rules:
- If MSG_DONTWAIT improves p99 without failures → keep
- If lean=true improves p99 → consider making release default
- If batch > 16 worsens p99 vs batch=1 → reduce batch
- Use median p99 across 3 runs for decisions

Risk level: Low (script is read-only, only spawns containers).

## Priority 2: Commit Phase 20/22 Changes

Goal: Convert dirty worktree into clean commits.

Modified files:
- `src/fdlb.c` — non-blocking sendmsg + lean EAGAIN fallback
- `docker-compose.preview-kdclass3-fdpass.yml` — lean mode + accept batch

Untracked Phase 20 files to decide on:
- `src/kdclass3_simd.c`, `src/kdclass3_bbox_avx2.c`, `src/kdclass3_opt.c`, etc.

Risk level: Medium. Do not auto-commit without explicit approval.

## Priority 3: Investigate Docker Socket Volume

Goal: Check if tmpfs volume for Unix sockets impacts queueing latency.

Current compose uses default Docker volume (not tmpfs). Many competitors use `tmpfs` with `size=4m`. Our negative experiments said tmpfs didn't help, but combined with lean LB + non-blocking sendmsg, it may matter.

Expected validation:
- Add `driver_opts: type: tmpfs, device: tmpfs` to sockets volume.
- Same-session k6 A/B comparison.

Risk level: Low.

## Priority 4: Evaluate kdclass3 SIMD Promotion

Goal: Revisit whether `RINHA_KDCLASS3_IMPL=simd_full` should be promoted to default.

Offline: 12-19% faster than baseline, exactness preserved (0/0/0).
Local k6: Did not beat baseline in noisy environment.
Decision: Keep opt-in until official Linux validation shows repeatable p99 improvement.

## Priority 5: Align Release Defaults

Goal: Decide whether `Dockerfile.release` should default to kdclass3.

Current: `Dockerfile.release` defaults to kdprimary2, but root compose uses kdclass3.
Risk: Medium.

## Suggested Work Order For Next Agent

1. Read updated `docs/ai-handoff/02_CURRENT_STATE.md` for Phase 22 findings.
2. Run `git status --short --branch` to understand current dirty state.
3. If Linux host available: run k6 comparison of full LB vs lean LB.
4. Decide whether to commit Phase 20+22 changes.
5. Do not randomly delete Phase 20 files without explicit approval.
