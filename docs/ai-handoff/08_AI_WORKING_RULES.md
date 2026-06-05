# AI Handoff: Working Rules For The Next Agent

## First Rule: Preserve User Work

Before doing anything:

```powershell
git status --short --branch
git diff --name-status
```

The worktree may already contain intentional uncommitted experiments.

Do not:

- Revert modified files without explicit approval.
- Delete untracked files without a deletion plan and approval.
- Commit or push unless explicitly asked.
- Touch the Go repo.
- Touch the official Rinha repo.

## Repository-Specific Safety Rules

Do not change casually:

- `fastvector` semantics.
- Raw HTTP parser behavior.
- KD-primary2 exactness.
- kdclass3 `F3` vs `L3` decision semantics.
- fdpass fd ownership rules.
- Root `docker-compose.yml` official-compatible topology.
- `Dockerfile.release` default index.

Never add:

- Payload lookup.
- Request memorization.
- Query hash lookup.
- Row-index lookup.
- Test-data lookup at runtime.
- Fraud/business logic in the LB.

## Coding Conventions

General C style:

- Keep code simple and explicit.
- Avoid dynamic allocation in hot request paths.
- Prefer stack-local fixed-size buffers when safe.
- Use mmap read-only for large indexes.
- Keep optional diagnostics behind env flags or compile-time flags.
- Do not log per request in benchmark paths.
- Keep exact search fallback paths available when introducing SIMD.

Hot-path conventions:

- No malloc per request.
- No printf/logging per request.
- No heavy metrics when disabled.
- No mode dispatch loops that load unused indexes.
- Keep response mapping as direct/prebuilt as possible.

Tests:

- Add focused unit tests for parser/config/search changes.
- Add offline full evaluation for any search semantic change.
- Add perturb/reference evaluation for classifier changes when available.

## Performance Rules

Detection is solved. p99 is the target.

Before optimizing:

- Identify whether bottleneck is classifier, parser, write path, LB, fdpass, Docker, or k6/client.
- Use same-session A/B comparisons.
- Repeat runs.
- Check `http_req_waiting` p99.
- Check HTTP errors and container restarts.

Do not promote a candidate based only on:

- Offline microbenchmark.
- One local k6 run.
- Avg latency improvement.
- p50 improvement.

Promotion requires:

- Full exactness.
- No HTTP errors.
- No OOM/restarts.
- p99 improvement in repeated official-compatible runs.

## Testing Rules

Minimum before claiming runtime code is safe:

```powershell
docker build --platform linux/amd64 --target test -t rinha-c-test .
docker run --rm `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-2026-go\release:/data:ro `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-de-backend-2026\test:/testdata:ro `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-backend-2026-c\tmp\kdclass3:/out:ro `
  rinha-c-test
```

Before release-image claims:

```powershell
docker build --platform linux/amd64 -f Dockerfile.release --build-arg RINHA_RELEASE_INDEX=kdclass3 -t rinha-c-preview:kdclass3-l64 .
docker build --platform linux/amd64 -f Dockerfile.fdlb -t rinha-c-fdlb:local .
```

Before compose claims:

```powershell
docker compose -f docker-compose.yml config
docker compose -f docker-compose.preview-kdclass3-fdpass.yml config
```

After every compose run:

```powershell
docker compose -f docker-compose.yml down
docker ps
```

## Benchmark Rules

Use official-compatible topology for candidate decisions:

- LB + at least two APIs.
- Bridge network.
- No host networking.
- No privileged mode.
- CPU <= 1.
- Memory <= 350MB.

Use direct TCP only for diagnostics.

For k6 comparisons:

- Run baseline and candidate in the same session.
- Alternate order when possible.
- Use cooldowns on noisy/mobile CPUs.
- Collect p99, final score, detection score, FP/FN/Error, failure rate, HTTP errors, and `http_req_waiting` p99.
- Check memory and restarts.

## What To Avoid

Avoid:

- Reintroducing nginx or haproxy.
- Making async workers default.
- Making fdlb least-active default.
- Making kdclass3 SIMD default without repeatable k6.
- Tracking generated `.bin` files.
- Adding broad refactors during benchmark phases.
- Changing several variables at once.
- Submitting based on local direct-TCP diagnostics.

## When To Ask The User First

Ask before:

- Deleting files.
- Reverting dirty worktree changes.
- Committing or pushing.
- Changing default release image/index.
- Updating submission branch.
- Publishing GHCR images.
- Modifying official repo or Go repo.
- Making an experimental implementation default.

## How To Update This Handoff

After each major phase, update at least:

- `02_CURRENT_STATE.md`
- `04_TASKS_DONE.md`
- `05_KNOWN_ISSUES.md`
- `07_NEXT_STEPS.md`

For every benchmark update, record:

- Exact command.
- Image tag.
- Compose file.
- Env vars.
- Resource split.
- p99.
- final_score.
- detection_score.
- `FP/FN/Error`.
- failure rate.
- HTTP errors.
- memory/restarts.
- interpretation.

For every search/classifier change, record:

- Official-local exactness.
- Fallback count.
- Mismatches vs baseline.
- Offline avg/p95/p99.
- Runtime k6 outcome.

## Handoff Reading Order

The next agent should read:

1. `00_PROJECT_BRIEF.md`
2. `02_CURRENT_STATE.md`
3. `05_KNOWN_ISSUES.md`
4. `06_COMMANDS_AND_TESTS.md`
5. `07_NEXT_STEPS.md`
6. `01_ARCHITECTURE.md`
7. `03_DECISIONS.md`
8. `04_TASKS_DONE.md`
9. `08_AI_WORKING_RULES.md`
