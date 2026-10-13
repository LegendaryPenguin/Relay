#!/usr/bin/env bash
#
# End-to-end throughput, measured rather than claimed.
#
# The microbenchmarks in bench/results.md measure the decision path — parsing,
# balancing, policy. This measures the whole thing through real sockets, which
# is dominated by syscalls and thread scheduling instead, and is the number
# that bounds what the proxy can actually do.
#
# Uses a fixed number of client processes each issuing keep-alive requests, so
# connection setup is amortised the way a real client's would be.

set -euo pipefail
set +m

cd "$(dirname "$0")/.."
WORK=$(mktemp -d)
PIDS=()
cleanup() {
    for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT
export PATH="/opt/homebrew/bin:$PATH"

CLIENTS=${CLIENTS:-8}
REQUESTS=${REQUESTS:-400}
WORKERS=${WORKERS:-8}

echo "building..."
cmake -S . -B "$WORK/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "$WORK/build" -j8 --target relay-proxy relay-origin >/dev/null

echo "starting 3 origins and the proxy ($WORKERS workers)..."
for port in 9201 9202 9203; do
    "$WORK/build/relay-origin" -port "$port" -body ok >/dev/null 2>&1 &
    PIDS+=($!)
done
sleep 1
"$WORK/build/relay-proxy" -port 9200 -workers "$WORKERS" \
    -upstreams "127.0.0.1:9201,127.0.0.1:9202,127.0.0.1:9203" \
    -balancer p2c -attempt-timeout-ms 1000 -deadline-ms 3000 \
    -queue-depth 256 -accept-queue 1024 -max-concurrent 1024 \
    >"$WORK/proxy.log" 2>&1 &
PIDS+=($!)
for _ in $(seq 50); do
    curl -sf http://127.0.0.1:9200/ >/dev/null 2>&1 && break
    sleep 0.1
done

total=$((CLIENTS * REQUESTS))
echo "$CLIENTS clients x $REQUESTS keep-alive requests = $total total"

start=$(python3 -c 'import time; print(time.time())')
CLIENT_PIDS=()
for c in $(seq 1 "$CLIENTS"); do
    # One curl process per client, reusing a single connection across all its
    # requests via -K with repeated urls. Keeps connection setup out of the
    # measurement the way a real client would.
    ( for i in $(seq 1 "$REQUESTS"); do echo "url = http://127.0.0.1:9200/"; done \
        | curl -s -o /dev/null -K - ) &
    CLIENT_PIDS+=($!)
done
for pid in "${CLIENT_PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
end=$(python3 -c 'import time; print(time.time())')

python3 - "$start" "$end" "$total" <<'PY'
import sys
start, end, total = float(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3])
elapsed = end - start
print("   %d requests in %.2fs = %.0f req/sec (%.3f ms/req mean)" % (
    total, elapsed, total / elapsed, elapsed * 1000 / total))
PY

echo
echo "proxy counters:"
kill -TERM "${PIDS[${#PIDS[@]}-1]}" 2>/dev/null || true
sleep 0.5
grep -E "relay_(requests|responses_2xx|retries|pool_hits|pool_misses)_total" "$WORK/proxy.log" \
    | sed 's/^/   /' || echo "   (none captured)"
