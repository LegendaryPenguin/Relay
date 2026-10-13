# Benchmark results

Apple Silicon, `-O2 -DNDEBUG`, Apple clang 21. Produced by:

- `./build/relay-bench` — per-request CPU of the decision path
- `./scripts/stress.sh` — end-to-end throughput through real sockets

**Method, stated so the numbers can be discounted appropriately.** No
framework, matching the rest of the project. Medians over fixed iteration
counts, single run per cell, no outlier rejection, no confidence intervals.
Treat same-order differences as noise; the conclusions below only lean on
differences of 2× or more, and the cells where that is not safe say so.

One caveat that applies to the whole file: these are single-process,
single-machine, loopback numbers. Nothing here measures what a real network
does to tail latency, and the proxy's architecture (a bounded thread pool over
blocking sockets, not an event loop — see `socket.hpp`) means the end-to-end
figure in §6 is a property of that choice rather than of the code quality.

---

## 1. P2C is the whole argument for not using least-request

| endpoints | round_robin | random | **p2c** | least_request | ring_hash |
|---|---|---|---|---|---|
| 10 | 6.2 ns | 3.1 ns | **6.4 ns** | 20.7 ns | 80.7 ns |
| 100 | 33.5 ns | 2.6 ns | **3.8 ns** | 69.0 ns | 78.2 ns |
| 1000 | 291.3 ns | 2.7 ns | **3.5 ns** | 482.2 ns | 83.8 ns |

**P2C is O(1) and least-request is O(n), and at 1000 endpoints that is a 138×
difference.** P2C holds at 3.5 ns across three orders of magnitude of cluster
size; least-request climbs 20.7 → 69 → 482 ns because it scans every endpoint
on every request.

This is the measurement that justifies the implementation. P2C is provably
close to optimal for load distribution (and §2 confirms the distribution is
fine), so paying an O(n) scan for the exact answer buys very little and costs
a lot at scale.

**An unexpected result: weighted round-robin is also O(n)** — 6.2 → 33.5 →
291 ns. I had filed round-robin mentally as the cheap one. It is not, because
`PickRoundRobin` walks the eligible list accumulating weights to find the
target slot. For an unweighted cluster that walk is pure waste. Fixing it
needs either a precomputed weight-expanded list (memory) or smooth weighted
round-robin (more state); neither is implemented, and the honest summary is
that **round-robin is the wrong default and p2c is the right one**, which is
what `DefaultConfig` already says — but for a reason I only learned here.

`ring_hash` is flat at ~80 ns, dominated by hashing the key plus a binary
search over 16k points. More expensive than p2c and constant, which is the
right trade when affinity is what you need.

---

## 2. The distribution is fine, and the ring needed a real fix

Measured by `balancer_test` and `ring_test` rather than by timing: worst-case
deviation from a fair share over 100k selections across 8–10 endpoints.

| balancer | worst deviation |
|---|---|
| round_robin | 0% (exact) |
| random | < 5% |
| p2c | < 6% |
| ring_hash, 160 vnodes | < 25% |

**Consistent hashing is meaningfully less even than the others**, which is the
price of the property it buys: removing one endpoint of ten remaps about a
tenth of keys and leaves the rest where they were, measured at 3–20% moved
against the ~100% a `hash(key) % n` scheme would move.

### The bug this caught

The first version used plain FNV-1a for ring points and the worst-case
deviation was **99.1%** — one endpoint getting essentially everything.

FNV-1a diffuses well into the low bits and poorly into the high ones: the last
byte is XORed and multiplied exactly once, so for short keys sharing a prefix
the top bits stay dominated by that prefix. Ring points are exactly such keys
(`10.0.0.1:8080#0`, `#1`, …) and the ring is sorted by the full 64-bit value —
so all 160 of an endpoint's points landed in one narrow arc, and a 10-endpoint
ring became 10 giant arcs instead of 1600 interleaved ones.

Adding the MurmurHash3 64-bit finalizer fixed it. Both languages pin the same
output vector, because the Go control plane computes the same hash to predict
placement and a divergence would make its answers confidently wrong.

**Worth recording because every unit test passed before the fix.** Lookup was
deterministic, the chain was distinct, removal moved the right *fraction* —
every functional property held. Only a distribution measurement showed it.

### Ring construction cost

| endpoints | build time |
|---|---|
| 10 | 26.7 µs |
| 100 | 537 µs |
| 1000 | **7.39 ms** |

O(n · vnodes · log) and the 1000-endpoint figure is a real operational cost:
every config push rebuilds the ring, so a 1000-endpoint cluster pays 7 ms of
single-threaded work per push. At the poll interval the control plane uses
that is negligible; at a push per second across many clusters it would not be.
Not optimised, and the fix (incremental point insertion) is absent.

---

## 3. Parsing, and a counterintuitive result

| operation | cost | implied rate |
|---|---|---|
| parse request, 5 headers, no body | 310.0 ns | 3.2M /sec |
| parse request, 1 KiB body | **199.7 ns** | 5.0M /sec |
| serialize request | 45.5 ns | 22.0M /sec |

**Parsing a request with a 1 KiB body is faster than parsing one with five
headers and no body.** That inverts the obvious expectation and the
explanation is straightforward once seen: the body path is a bounded `memcpy`
after a single `Content-Length` parse, while each header costs a line scan, a
token validation, a lowercase pass and a vector push. **Headers are the
expensive part of HTTP/1.1, not bodies.**

At 310 ns, parsing is ~3% of the 9 µs end-to-end cost in §6. It is not the
bottleneck and optimising it would be wasted effort.

---

## 4. Policy checks are free

| check | cost |
|---|---|
| load shedder admit | 1.0 ns |
| circuit breaker allow + success | 1.3 ns |
| histogram record | 1.6 ns |
| retry budget on_request | 3.5 ns |
| rate limiter, 1 key | 6.3 ns |
| rate limiter, 10k rotating keys | 22.6 ns |
| histogram percentile | 84.8 ns |

The whole policy layer costs about 13 ns per request — 0.15% of end-to-end
cost. **There is no performance argument for making any of it optional**,
which matters because "we turned off the circuit breaker for throughput" is a
decision someone makes when the cost is unknown.

The rate limiter's 3.6× penalty under key rotation (6.3 → 22.6 ns) is cache
misses across the bucket table, and is the cost of bounding memory. Still
negligible.

`histogram percentile` at 84.8 ns scans all 416 buckets. Fine for a metrics
scrape; it should not be called per request, and nothing does.

---

## 5. Connection pooling works

From `scripts/stress.sh`, 3200 requests across 3 origins:

```
relay_pool_hits_total{cluster="default"} 3198
relay_pool_misses_total{cluster="default"} 3
```

**99.9% reuse** — three connections opened, one per origin, reused for
everything after. Without pooling each request would pay a TCP handshake,
which on a 1 ms-RTT network would roughly double the latency of a 1 ms
upstream.

**This cell reported 0 and 0 until I fixed a wiring bug.** The transport was
bound to a `ClusterStats` object at construction that the router never
rendered, so pool counters accumulated somewhere nothing read. An operator
would have seen a pool that looked entirely ineffective. The transport now
takes the calling cluster's stats per call, which also attributes connection
churn to the right cluster when there is more than one.

---

## 6. End-to-end throughput, and what bounds it

8 client processes × 400 keep-alive requests, 8 proxy workers, 3 origins:

```
3200 requests in 0.36s = 8,788 req/sec (0.114 ms/req mean)
```

**~9k req/sec and 114 µs mean latency.** Sum the decision path from §1–4 and
it comes to roughly 400 ns, which is **0.35% of the measured per-request
time**. The other 99.65% is syscalls, thread scheduling, and loopback TCP.

That ratio is the honest summary of this project's performance story. The
mechanisms are cheap; the architecture is what costs. Specifically:

- **Routing is serialised.** `Server::HandleConnection` takes a mutex around
  `Router::Route` because the router holds per-endpoint breakers and
  per-cluster budgets as plain non-atomic state. I/O is parallel across
  workers, routing is not. This is the throughput ceiling, it is called out in
  `server.cpp`, and the fix — per-worker policy state with periodic merging,
  trading breaker exactness for parallelism — is a real trade I did not make.
- **One thread per connection.** Memory per connection is a thread stack
  rather than a few hundred bytes, and concurrency is bounded by the pool size
  rather than by file descriptors. An event-loop reactor would fix both and
  requires every stage to be resumable, which is several times the code.

So: **this is not a high-throughput proxy, and the number above is the
measurement that says so.** Envoy does six figures per core. What this
codebase has is correct mechanisms with measured costs, and the gap to a
production proxy is architectural rather than a matter of tuning.

---

## 7. Load shedding, measured end to end

From `scripts/demo.sh` step 7 — 40 concurrent requests at a 200 ms origin, 2
workers, shed queue depth 6:

```
 7 × 200
33 × 503
```

Deliberate refusal, not timeouts and not connection resets.

**That distinction took a fix to achieve.** `-queue-depth` originally set both
the shed threshold *and* the accept queue, so every request past the threshold
got a TCP reset from the accept loop before any worker could consult the
shedder — 68 connection failures and zero 503s. A client cannot tell a reset
from the proxy being dead, and the entire point of shedding is to *answer*.
The accept queue now defaults to 4× the shed threshold, and setting it lower
emits a warning.

---

## Summary

| Claim | Status |
|---|---|
| p2c is O(1) where least-request is O(n) | **confirmed** — 138× at 1000 endpoints |
| round-robin is the cheap option | **refuted** — also O(n), 291 ns at 1000 endpoints |
| consistent hashing minimises remapping | **confirmed** — 3–20% moved vs ~100% for modulo |
| plain FNV-1a is adequate for ring points | **refuted** — 99.1% skew; needs a finalizer |
| bodies are the expensive part of parsing | **refuted** — headers cost more than a 1 KiB body |
| the policy layer is cheap enough to always run | **confirmed** — ~13 ns, 0.15% of a request |
| connection pooling reuses connections | **confirmed** — 99.9% hit rate |
| this is a high-performance proxy | **refuted** — ~9k req/sec; routing is serialised |
| load shedding answers rather than dropping | **confirmed**, after decoupling the accept queue |
