#!/usr/bin/env bash
#
# End-to-end: C++ data plane, Go control plane, three origins, then failures.
#
# The last three steps are the ones worth watching. Step 5 kills an origin and
# shows the control plane evict it and the proxy stop sending to it. Step 6
# makes an origin slow and shows the circuit breaker open. Step 7 floods the
# proxy and shows it shed load deliberately rather than letting latency grow
# without bound.

set -euo pipefail
set +m   # no job-control chatter when a background process is killed

cd "$(dirname "$0")/.."
WORK=$(mktemp -d)
PIDS=()
# Named PIDs as well as the list. Indexing into a flat array is how the first
# version of this script killed origin 9002 while announcing 9001, and macOS
# ships bash 3.2 which has no negative subscripts either.
ORIGIN_9001=""; ORIGIN_9002=""; ORIGIN_9003=""
PROXY_MAIN=""; PROXY_SHED=""; ORIGIN_SLOW=""
cleanup() {
    # No bare `wait` here: it would block on the origins and control plane,
    # which are long-running by design and never exit on their own.
    for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT

export PATH="/opt/homebrew/bin:$PATH"

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
jqp() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }

say "1. Build both halves"
cmake -S . -B "$WORK/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build "$WORK/build" -j8 --target relay-proxy relay-origin >/dev/null
(cd go-control-plane && go build -o "$WORK/control-plane" ./cmd/relay-control-plane)
echo "   data plane:    $(du -h "$WORK/build/relay-proxy" | cut -f1) (C++)"
echo "   control plane: $(du -h "$WORK/control-plane" | cut -f1) (Go)"

say "2. Start three origins on 9001-9003"
for port in 9001 9002 9003; do
    "$WORK/build/relay-origin" -port "$port" -body "origin-$port" >"$WORK/origin-$port.log" 2>&1 &
    pid=$!
    PIDS+=("$pid")
    eval "ORIGIN_$port=$pid"
done
sleep 1

say "3. Start the control plane; it resolves and health-checks the origins"
"$WORK/control-plane" -addr 127.0.0.1:9901 \
    -cluster "api=127.0.0.1:9001,127.0.0.1:9002,127.0.0.1:9003" \
    -probe-interval 500ms -probe-timeout 300ms \
    -unhealthy-after 2 -healthy-after 1 \
    >"$WORK/cp.log" 2>&1 &
PIDS+=($!)
for _ in $(seq 50); do
    curl -sf http://127.0.0.1:9901/status >/dev/null 2>&1 && break
    sleep 0.1
done
sleep 1
curl -s http://127.0.0.1:9901/status | jqp '
c = d["clusters"][0]
print("   cluster %s: %d endpoints, %d eligible (version %d)" % (c["name"], c["endpoints"], c["eligible"], d["version"]))'

say "4. Start the proxy and send traffic across all three origins"
"$WORK/build/relay-proxy" -port 9100 -workers 8 \
    -upstreams "127.0.0.1:9001,127.0.0.1:9002,127.0.0.1:9003" \
    -balancer round_robin -attempt-timeout-ms 500 -deadline-ms 1500 \
    >"$WORK/proxy.log" 2>&1 &
PROXY_MAIN=$!
PIDS+=("$PROXY_MAIN")
for _ in $(seq 50); do
    curl -sf http://127.0.0.1:9100/ >/dev/null 2>&1 && break
    sleep 0.1
done
echo "   30 requests, round robin:"
for i in $(seq 1 30); do curl -s http://127.0.0.1:9100/ ; done | sort | uniq -c | sed 's/^/     /'

say "5. The control plane notices a dead origin"
kill "$ORIGIN_9001" 2>/dev/null || true
echo "   killed the origin on 9001; waiting for two failed probes..."
sleep 2.5
curl -s http://127.0.0.1:9901/health | jqp '
for e in d["endpoints"]:
    print("     %-20s %-10s probes=%d failures=%d" % (e["address"], e["health"], e["total_probes"], e["total_failures"]))'
curl -s http://127.0.0.1:9901/status | jqp '
c = d["clusters"][0]
print("   cluster now: %d endpoints, %d eligible (version %d — the snapshot changed)" % (c["endpoints"], c["eligible"], d["version"]))'

say "6. The circuit breaker opens on a dead upstream"
# The proxy was started with a static upstream list, so it still tries 9001.
# That is deliberate here: it shows the data plane protecting itself without
# waiting for the control plane.
echo "   20 requests through the proxy, which still has 9001 in its list:"
ok=0; fail=0
for i in $(seq 1 20); do
    body=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:9100/ || true)
    if [ "$body" = "200" ]; then ok=$((ok+1)); else fail=$((fail+1)); fi
done
echo "     $ok succeeded, $fail failed"
echo "     (retries route around the dead host; the breaker stops attempting it)"
grep -o 'relay_breaker_rejections_total{[^}]*} [0-9]*' "$WORK/proxy.log" 2>/dev/null | tail -1 | sed 's/^/     /' || true

say "7. Load shedding under a flood"
# A queue-depth-based shedder with a small queue and few workers: the flood
# produces 503s rather than unbounded latency.
kill "$PROXY_MAIN" 2>/dev/null || true
sleep 0.3
"$WORK/build/relay-origin" -port 9004 -latency-ms 200 >"$WORK/origin-slow.log" 2>&1 &
ORIGIN_SLOW=$!
PIDS+=("$ORIGIN_SLOW")
sleep 0.5
"$WORK/build/relay-proxy" -port 9101 -workers 2 \
    -upstreams "127.0.0.1:9004" -balancer round_robin \
    -queue-depth 6 -accept-queue 64 -max-concurrent 4 \
    -attempt-timeout-ms 2000 -deadline-ms 3000 \
    >"$WORK/proxy-shed.log" 2>&1 &
PROXY_SHED=$!
PIDS+=("$PROXY_SHED")
for _ in $(seq 50); do
    curl -sf --max-time 3 http://127.0.0.1:9101/ >/dev/null 2>&1 && break
    sleep 0.1
done

echo "   40 concurrent requests at a 200ms origin, 2 workers, shed queue depth 6:"
: > "$WORK/codes.txt"
CURLS=()
for i in $(seq 1 40); do
    # Each writes its own line; a shared append from 40 processes is fine for
    # three-byte writes but a separate file per request removes the question.
    # curl -w always prints a code, including 000 on a connection failure, so
    # no fallback echo — adding one double-wrote and produced 74 lines from 40
    # requests.
    ( curl -s -o /dev/null -w '%{http_code}\n' --max-time 5 \
        http://127.0.0.1:9101/ >>"$WORK/codes.txt" 2>/dev/null ) &
    CURLS+=($!)
done
# Wait only on the curl PIDs. A bare `wait` here blocks on the origins and
# control plane too, which never exit — that hung the whole script.
for pid in "${CURLS[@]}"; do wait "$pid" 2>/dev/null || true; done
sort "$WORK/codes.txt" | uniq -c | sed 's/^/     /'
echo "   503s are the proxy refusing work deliberately, not timing out."

say "Done"
echo "A proxy's job under overload is to fail fast and on purpose."
