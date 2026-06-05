# AI Handoff: Commands And Tests

All commands assume PowerShell from:

```powershell
cd C:\Users\Usuario\Documents\rinha-backend\rinha-backend-2026-c
```

## Required External Paths

Go-generated source/reference index:

```text
C:\Users\Usuario\Documents\rinha-backend\rinha-2026-go\release\index.bin
```

Official-local test data:

```text
C:\Users\Usuario\Documents\rinha-backend\rinha-de-backend-2026\test
```

Local generated index output:

```text
C:\Users\Usuario\Documents\rinha-backend\rinha-backend-2026-c\tmp
```

Release artifacts:

```text
C:\Users\Usuario\Documents\rinha-backend\rinha-backend-2026-c\release
```

## Check Repository State

```powershell
git status --short --branch
git diff --name-status
git diff --check
```

Do this before making changes. The worktree may already be dirty.

## Build Test Image

```powershell
docker build --platform linux/amd64 --target test -t rinha-c-test .
```

## Run Mounted Test Suite

```powershell
docker run --rm `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-2026-go\release:/data:ro `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-de-backend-2026\test:/testdata:ro `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-backend-2026-c\tmp\kdclass3:/out:ro `
  rinha-c-test
```

The test image `CMD` runs `make test` by default.

## Build API Release Image: Default kdprimary2

```powershell
docker build --platform linux/amd64 -f Dockerfile.release -t rinha-c-preview:kdprimary2 .
```

Important:

- This uses `RINHA_RELEASE_INDEX=kdprimary2` by default.
- Requires `release/kdprimary2.bin`.

## Build API Release Image: kdclass3

```powershell
docker build --platform linux/amd64 `
  -f Dockerfile.release `
  --build-arg RINHA_RELEASE_INDEX=kdclass3 `
  -t rinha-c-preview:kdclass3-l64 .
```

Requires:

- `release/kdclass3.bin`

## Build C fdlb Image

```powershell
docker build --platform linux/amd64 -f Dockerfile.fdlb -t rinha-c-fdlb:local .
```

## Validate Compose Files

```powershell
docker compose -f docker-compose.yml config
docker compose -f docker-compose.preview-kdclass3-fdpass.yml config
docker compose -f docker-compose.preview-kdclass3.yml config
```

## Run Root Compose Smoke

```powershell
docker compose -f docker-compose.yml up -d
```

Ready endpoint:

```powershell
1..10 | ForEach-Object {
  curl.exe -fsS http://localhost:9999/ready
}
```

Fraud endpoint with a local request JSON:

```powershell
1..10 | ForEach-Object {
  curl.exe -fsS -X POST http://localhost:9999/fraud-score `
    -H "Content-Type: application/json" `
    --data-binary "@request0.json"
}
```

Cleanup:

```powershell
docker compose -f docker-compose.yml down
docker ps
```

## Build kdclass3 Index

If `tmp/kdclass3/kdclass3-l64.bin` is missing, build it from the Go-generated index:

```powershell
New-Item -ItemType Directory -Force tmp\kdclass3 | Out-Null

docker run --rm `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-2026-go\release:/data:ro `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-backend-2026-c\tmp\kdclass3:/out `
  rinha-c-test `
  ./build/build_kdclass3 --index /data/index.bin --output /out/kdclass3-l64.bin --leaf-size 64
```

Copy to release if building release images:

```powershell
New-Item -ItemType Directory -Force release | Out-Null
Copy-Item tmp\kdclass3\kdclass3-l64.bin release\kdclass3.bin -Force
```

Do not commit `.bin` files.

## Evaluate kdclass3 Official-Local Baseline

```powershell
docker run --rm `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-backend-2026-c\tmp\kdclass3:/out:ro `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-backend-2026-c\release:/release:ro `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-de-backend-2026\test:/testdata:ro `
  rinha-c-test `
  ./build/evaluate_kdclass3 `
    --tree /out/kdclass3-l64.bin `
    --kdprimary2 /release/kdprimary2.bin `
    --test-data /testdata/test-data.json `
    --impl baseline `
    --touch
```

Expected for stable exactness:

- `FP/FN/Error = 0/0/0`
- approved mismatches vs KD-primary2: `0`
- fallback count: `0`

## Evaluate kdclass3 SIMD Canary

```powershell
docker run --rm `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-backend-2026-c\tmp\kdclass3:/out:ro `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-backend-2026-c\release:/release:ro `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-de-backend-2026\test:/testdata:ro `
  rinha-c-test `
  ./build/evaluate_kdclass3 `
    --tree /out/kdclass3-l64.bin `
    --kdprimary2 /release/kdprimary2.bin `
    --test-data /testdata/test-data.json `
    --impl simd_full `
    --touch
```

Expected from Phase 20B:

- Exactness preserved.
- Faster offline than baseline.
- Do not promote to default without k6 repeatability.

## Evaluate Perturb/Reference Dataset

If `tmp/rf/rf-dataset.csv` exists:

```powershell
docker run --rm `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-backend-2026-c\tmp\kdclass3:/out:ro `
  -v C:\Users\Usuario\Documents\rinha-backend\rinha-backend-2026-c\tmp\rf:/rf:ro `
  rinha-c-test `
  ./build/evaluate_kdclass3_opt `
    --tree /out/kdclass3-l64.bin `
    --vectors-csv /rf/rf-dataset.csv `
    --variant simd-full `
    --touch
```

## Run k6

The official local test directory contains k6 scripts. Typical shape:

```powershell
cd C:\Users\Usuario\Documents\rinha-backend\rinha-de-backend-2026\test
k6 run .\test.js
```

Then return to repo:

```powershell
cd C:\Users\Usuario\Documents\rinha-backend\rinha-backend-2026-c
```

Use existing scripts when possible. Check `scripts/` for current helpers.

Known useful script:

- `scripts/collect-k6-details.ps1`

## k6 Output Warning

k6 may write useful info to stderr. In PowerShell, this can interact badly with `$ErrorActionPreference = 'Stop'`.

If a k6 wrapper appears to fail:

- Check process exit code.
- Check generated summary JSON.
- Check final score output.
- Do not assume stderr means benchmark failure.

## Stable Root Compose Environment

Root `docker-compose.yml` API env:

```text
RINHA_ADDR=:8080
RINHA_SEARCH_IMPL=kdclass3
RINHA_KDCLASS3_PATH=/app/resources/kdclass3.bin
RINHA_KDCLASS3_TOUCH=true
RINHA_KDCLASS3_FALLBACK=none
RINHA_LISTEN_MODE=fdpass
RINHA_EXEC_MODE=epoll
RINHA_API_PROCESS_MODE=sync
RINHA_API_PROCESSES=1
RINHA_METRICS_ENABLED=false
```

Root `docker-compose.yml` LB env:

```text
RINHA_LB_ADDR=:9999
RINHA_FDPASS_UPSTREAMS=/sockets/api1.ctrl,/sockets/api2.ctrl
```

## Local Preview Environment For kdclass3

Use `docker-compose.preview-kdclass3-fdpass.yml` with local images.

Expected images:

- `rinha-c-fdlb:local`
- `rinha-c-preview:kdclass3-l64`

## Build Profiles

Makefile supports:

- `CFLAGS_PROFILE=current`
- `CFLAGS_PROFILE=pre10b`
- `CFLAGS_PROFILE=o3`
- `CFLAGS_PROFILE=lto`
- `CFLAGS_PROFILE=v3`
- `CFLAGS_PROFILE=pgo-generate`
- `CFLAGS_PROFILE=pgo-use`

Release Dockerfile currently builds with:

```text
CFLAGS_PROFILE=pre10b
RINHA_ENABLE_METRICS=0
```

Do not switch release profile based only on offline timing.

## Known Commands That Can Fail

Release image build can fail if required binary is missing:

- kdprimary2 build requires `release/kdprimary2.bin`.
- kdclass3 build requires `release/kdclass3.bin`.

Mounted tests can fail if external directories are missing:

- `/data/index.bin`
- `/testdata/test-data.json`

k6 can fail if compose is not fully ready:

- Always smoke `/ready` before running k6.
- Check `docker compose logs` if connection refused or empty reply appears.

## Cleanup After Every Compose Run

```powershell
docker compose -f docker-compose.yml down
docker ps
```

For preview compose:

```powershell
docker compose -f docker-compose.preview-kdclass3-fdpass.yml down
docker ps
```
