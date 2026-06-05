#!/usr/bin/env bash
# Phase 23 — Native Linux fdlb p99 A/B Test Matrix
# Run on a native Linux host (not Docker Desktop).
# Requires: docker, k6, jq, curl

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
RESULTS_DIR="${PROJECT_DIR}/tmp/results/phase23-linux"
TIMESTAMP=$(date +%Y%m%d-%H%M%S)

mkdir -p "${RESULTS_DIR}"

K6_TEST="${PROJECT_DIR}/../rinha-de-backend-2026/test/test.js"
K6_TEST_DIR="$(dirname "${K6_TEST}")"

if [ ! -f "${K6_TEST}" ]; then
    echo "ERROR: k6 test file not found at ${K6_TEST}"
    echo "Set K6_TEST_DIR env var or place test.js at expected path."
    exit 1
fi

# ---------------------------------------------------------------------------
# Variant list
# ---------------------------------------------------------------------------
# Each variant: name, LEAN, ACCEPT_BATCH, extra env
VARIANTS=(
    "A-baseline|false|1|"
    "B-msgdontwait|false|1|"
    "C-lean|true|1|"
    "D-lean-batch16|true|16|"
    "E-lean-batch32|true|32|"
    "F-lean-batch64|true|64|"
    "G-lean-batch128|true|128|"
)

REPETITIONS=3

# ---------------------------------------------------------------------------
# Build images once
# ---------------------------------------------------------------------------
echo "=== Building release images ==="
cd "${PROJECT_DIR}"

docker build --platform linux/amd64 -f Dockerfile.release \
    --build-arg RINHA_RELEASE_INDEX=kdclass3 \
    -t rinha-c-preview:kdclass3-l64 .

docker build --platform linux/amd64 -f Dockerfile.fdlb \
    -t rinha-c-fdlb:local .

# ---------------------------------------------------------------------------
# Run each variant
# ---------------------------------------------------------------------------
for variant_def in "${VARIANTS[@]}"; do
    IFS='|' read -r NAME LEAN BATCH EXTRA <<< "${variant_def}"

    VARIANT_DIR="${RESULTS_DIR}/${TIMESTAMP}-${NAME}"
    mkdir -p "${VARIANT_DIR}"

    echo ""
    echo "============================================"
    echo "  Variant: ${NAME}  (lean=${LEAN}, batch=${BATCH})"
    echo "============================================"

    # Write variant compose override
    cat > "${PROJECT_DIR}/docker-compose.phase23-override.yml" <<OVERRIDE
services:
  lb:
    environment:
      RINHA_FDLB_LEAN: "${LEAN}"
      RINHA_FDLB_ACCEPT_BATCH: "${BATCH}"
OVERRIDE

    for rep in $(seq 1 ${REPETITIONS}); do
        REP_DIR="${VARIANT_DIR}/run${rep}"
        mkdir -p "${REP_DIR}"

        echo "  Run ${rep}/${REPETITIONS}..."

        # Start compose
        docker compose \
            -f docker-compose.preview-kdclass3-fdpass.yml \
            -f docker-compose.phase23-override.yml \
            up -d --wait --wait-timeout 120 2>&1 | tee "${REP_DIR}/compose-up.log"

        # Smoke check
        for i in $(seq 1 10); do
            if curl -fsS -m 3 http://localhost:9999/ready > /dev/null 2>&1; then
                break
            fi
            sleep 1
        done

        # Collect container resource baseline
        docker stats --no-stream --format json \
            $(docker compose -f docker-compose.preview-kdclass3-fdpass.yml ps -q) \
            > "${REP_DIR}/container-stats-before.json" 2>/dev/null || true

        # Run k6
        cd "${K6_TEST_DIR}"
        k6 run test.js \
            --summary-export "${REP_DIR}/k6-summary-full.json" \
            --out json="${REP_DIR}/k6-ndjson.log" \
            2>&1 | tee "${REP_DIR}/k6-output.txt"
        cd "${PROJECT_DIR}"

        # Collect container resource after
        docker stats --no-stream --format json \
            $(docker compose -f docker-compose.preview-kdclass3-fdpass.yml ps -q) \
            > "${REP_DIR}/container-stats-after.json" 2>/dev/null || true

        # Collect container logs
        docker compose -f docker-compose.preview-kdclass3-fdpass.yml logs --no-log-prefix \
            > "${REP_DIR}/container-logs.txt" 2>&1 || true

        # Extract key metrics
        if [ -f "${REP_DIR}/k6-summary-full.json" ]; then
            jq -r '
            {
                variant: "'"${NAME}"'",
                run: '"${rep}"',
                lean: "'"${LEAN}"'",
                batch: '"${BATCH}"',
                p99_duration_ms: (.metrics."http_req_duration"."p(99)" // "N/A"),
                p99_waiting_ms: (.metrics."http_req_waiting"."p(99)" // "N/A"),
                p95_duration_ms: (.metrics."http_req_duration"."p(95)" // "N/A"),
                p90_duration_ms: (.metrics."http_req_duration"."p(90)" // "N/A"),
                p50_duration_ms: (.metrics."http_req_duration"."avg" // "N/A"),
                max_duration_ms: (.metrics."http_req_duration"."max" // "N/A"),
                iterations: (.metrics."iterations"."count" // 0),
                iteration_rate: (.metrics."iterations"."rate" // 0),
                tp_count: (.metrics."tp_count"."count" // 0),
                tn_count: (.metrics."tn_count"."count" // 0),
                http_req_failed: (.metrics."http_req_failed"."value" // "N/A"),
                http_req_blocked_p99: (.metrics."http_req_blocked"."p(99)" // 0),
                http_req_receiving_p99: (.metrics."http_req_receiving"."p(99)" // 0),
                http_req_sending_p99: (.metrics."http_req_sending"."p(99)" // 0),
            }' "${REP_DIR}/k6-summary-full.json" >> "${VARIANT_DIR}/metrics.jsonl"
        fi

        # Check for container restarts
        RESTARTS=$(docker compose -f docker-compose.preview-kdclass3-fdpass.yml ps -a --format json 2>/dev/null | jq -r 'select(.ExitCode != null and .ExitCode != "0") | "\(.Service): exit=\(.ExitCode)"' 2>/dev/null || echo "none")
        echo "container_restarts: ${RESTARTS}" >> "${REP_DIR}/health.txt"

        # Stop compose
        docker compose \
            -f docker-compose.preview-kdclass3-fdpass.yml \
            -f docker-compose.phase23-override.yml \
            down -v 2>&1 || true

        # Cooldown between repetitions
        sleep 10
    done

    # Compute median p99 across repetitions for this variant
    if [ -f "${VARIANT_DIR}/metrics.jsonl" ]; then
        echo ""
        echo "  --- ${NAME} summary across ${REPETITIONS} runs ---"
        jq -s '
            sort_by(.p99_duration_ms) |
            {
                variant: .[0].variant,
                runs: length,
                min_p99: .[0].p99_duration_ms,
                median_p99: (if length % 2 == 0 then ((.[length/2-1].p99_duration_ms + .[length/2].p99_duration_ms) / 2) else .[length/2].p99_duration_ms end),
                max_p99: .[-1].p99_duration_ms,
                p99_waiting_median: (if length % 2 == 0 then ((.[length/2-1].p99_waiting_ms + .[length/2].p99_waiting_ms) / 2) else .[length/2].p99_waiting_ms end),
                total_iterations: (map(.iterations) | add),
                failures: [.[].http_req_failed],
            }' "${VARIANT_DIR}/metrics.jsonl" | tee "${VARIANT_DIR}/summary.json"
    fi
done

# ---------------------------------------------------------------------------
# Final comparison table
# ---------------------------------------------------------------------------
echo ""
echo "============================================"
echo "  FINAL COMPARISON TABLE"
echo "============================================"

echo "variant | median_p99_ms | median_waiting_ms | total_iters | failures"
echo "--------|---------------|-------------------|-------------|----------"

for variant_def in "${VARIANTS[@]}"; do
    IFS='|' read -r NAME LEAN BATCH EXTRA <<< "${variant_def}"
    VARIANT_DIR="${RESULTS_DIR}/${TIMESTAMP}-${NAME}"
    if [ -f "${VARIANT_DIR}/summary.json" ]; then
        jq -r '"'"${NAME}"' | \(.median_p99) | \(.p99_waiting_median) | \(.total_iterations) | \(.failures)"' "${VARIANT_DIR}/summary.json"
    fi
done

# Cleanup override file
rm -f "${PROJECT_DIR}/docker-compose.phase23-override.yml"

echo ""
echo "Results directory: ${RESULTS_DIR}/${TIMESTAMP}-*"
echo "Phase 23 native Linux matrix complete."
