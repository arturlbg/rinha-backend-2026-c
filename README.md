# Rinha Backend 2026 — C Solution

A fraud-detection backend for [Rinha de Backend 2026](https://github.com/zanfranceschi/rinha-de-backend-2026). Written in C.

## Architecture

```
Client → C fdlb (port 9999) → SCM_RIGHTS fdpass → 2× C API → kdclass3 → response
```

- **fdlb**: custom file-descriptor passing load balancer. Accepts TCP connections on `:9999`, passes accepted sockets to API containers via Unix sockets + `SCM_RIGHTS`. Payload-blind — no HTTP parsing, no business logic.
- **API**: epoll-based HTTP server. Receives fds via fdpass, parses HTTP, vectorizes the transaction JSON via `fastvector`, classifies with kdclass3 exact search, returns prebuilt response.
- **kdclass3**: exact class-separated KD-tree classifier. Splits 3M reference vectors into fraud/legit trees, searches until 3rd-nearest fraud and legit distances can be compared.

## Key Optimizations

- **fdpass topology**: LB moves file descriptors, not bytes. Zero-copy handoff to APIs.
- **lean LB mode** (`RINHA_FDLB_LEAN=true`): removes per-iteration feedback polling.
- **nonblocking sendmsg** (`MSG_DONTWAIT`): prevents LB blocking when API control socket is saturated.
- **MAP_POPULATE + MADVISE_HUGEPAGE** (`RINHA_KDCLASS3_POPULATE=true`, `RINHA_KDCLASS3_MADVISE=hugepage`): pre-faults kdclass3 index pages and enables 2MB transparent huge pages for reduced TLB misses.
- **prebuilt responses**: all 6 fraud-score responses compiled as static byte arrays — no per-request formatting.
- **AVX2 SIMD** (`RINHA_KDCLASS3_IMPL=simd_full`): opt-in SIMD bbox traversal and distance computation.

## Project Structure

```
├── docker-compose.yml       # Official submission compose
├── Dockerfile.release        # API release image
├── Dockerfile.fdlb           # LB release image
├── include/                  # Public headers
├── src/                      # C source
│   ├── fdlb.c / fdlb_main.c # Load balancer
│   ├── main.c                # API entry point
│   ├── raw_http.c            # HTTP parser + epoll reactor
│   ├── fastvector.c          # Transaction vectorization
│   ├── kdclass3.c            # Exact classifier
│   ├── fdpass.c              # SCM_RIGHTS fd receiver
│   └── responses.c           # Prebuilt HTTP responses
├── tests/                    # Unit tests
├── tools/                    # Builders and evaluators
```

## Building

```bash
# API image (kdclass3)
docker build --platform linux/amd64 \
  -f Dockerfile.release \
  --build-arg RINHA_RELEASE_INDEX=kdclass3 \
  -t ghcr.io/arturlbg/rinha-backend-2026-c:kdclass3-l64 .

# LB image
docker build --platform linux/amd64 \
  -f Dockerfile.fdlb \
  -t ghcr.io/arturlbg/rinha-backend-2026-c:fdlb .
```

## Testing

```bash
# Start
docker compose up -d

# Smoke test
curl http://localhost:9999/ready
curl -X POST http://localhost:9999/fraud-score -H "Content-Type: application/json" -d @request.json

# Full test suite
docker build --platform linux/amd64 --target test -t rinha-c-test .
docker run --rm rinha-c-test

# Official benchmark
k6 run test/test.js
```

## Best Official Result

- p99: **1.46ms**
- Score: **5834.49**
- Detection: **3000** (0/0/0 errors)
