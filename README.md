# 🛰️ Relay

*An L7 traffic platform with a C++ data plane and a Go control plane, where the interesting part turned out not to be forwarding requests but controlling how failure spreads.*

A relay in a circuit is the thing that decides whether current gets through. The name is the point: a proxy's value under normal conditions is routing, and its value under failure is refusing — bounding retries, cutting off dead upstreams, and shedding work it cannot do rather than queueing it until everything is slow.

Relay is **4,499 lines of C++** and **1,554 lines of Go**, backed by **3,980 lines of tests** — **199 tests** across both languages, clean under AddressSanitizer and UndefinedBehaviorSanitizer and `-race`. It implements an HTTP/1.1 parser with request-smuggling defences, five load-balancing policies including consistent hashing, connection pooling, per-endpoint circuit breakers, a retry *budget*, token-bucket rate limiting, queue-depth load shedding, and a control plane that resolves and health-checks endpoints and publishes versioned configuration.

The thing I would point at: **three of my own assumptions are marked "refuted" in [`bench/results.md`](bench/results.md).** Round-robin is not the cheap balancer. Plain FNV-1a is not adequate for ring points — it produced 99% load skew while passing every functional test. And headers, not bodies, are the expensive part of HTTP parsing.

```
$ ./scripts/demo.sh
== 5. The control plane notices a dead origin
     127.0.0.1:9001       unhealthy  probes=10 failures=6
     127.0.0.1:9002       healthy    probes=10 failures=0
   cluster now: 3 endpoints, 2 eligible (version 2 — the snapshot changed)

== 7. Load shedding under a flood
   40 concurrent requests at a 200ms origin, 2 workers, shed queue depth 6:
        7 200
       33 503
   503s are the proxy refusing work deliberately, not timing out.
```

## ✨ Why I Built It

I wanted to know what is actually underneath "put it behind a load balancer". The forwarding part I could have guessed at. What I could not have told you beforehand is why a retry count is the wrong way to bound retries, or why a circuit breaker that counts a shed request as a failure converts self-protection into an outage.

Both of those are in here as bugs I wrote and then fixed.

## 🔭 What It Does

**Parses HTTP/1.1 defensively.** Incremental at any byte boundary, with every limit enforced during parsing. Ambiguous framing — `Content-Length` with `Transfer-Encoding`, duplicate `Content-Length`, obfuscated `chunked` — is *rejected* rather than resolved.

**Balances five ways.** Round-robin, random, least-request, power-of-two-choices, and consistent hashing with virtual nodes. Selection is separated from proxying, which is what made the distribution properties testable over a million selections.

**Pools upstream connections**, with three separate defences against the spurious 502 that naive pools cause.

**Breaks circuits per endpoint**, not per cluster. A per-cluster breaker takes healthy hosts out along with the sick one.

**Bounds retries by budget**, not by count — retries may be at most a fraction of *successful* traffic.

**Rate limits per key** with bounded memory.

**Sheds load on queue depth**, probabilistically, before latency becomes unbounded.

**Discovers and health-checks** in Go, publishing whole versioned snapshots that the data plane polls and applies — with a dependency-free JSON reader on the C++ side, a bootstrap config so the proxy can serve before the control plane answers, and a refusal path that keeps the previous table when a snapshot does not validate.

## 🧱 How I Built It

**Week 1 — the proxy, and one decision I should flag immediately.** Listener, parser, upstream connection, forward. The architecture is a bounded thread pool over blocking sockets with poll-based timeouts, *not* a single-threaded event-loop reactor. That bounds throughput at about 9k req/sec, measured, and the reasoning plus the cost is in `socket.hpp` and §6 of the results rather than buried.

**Week 2 — pooling and balancing, kept apart.** The balancer takes a snapshot and a hash and returns an endpoint; it opens no sockets and knows nothing about HTTP. That split is why `balancer_test.cpp` can run a million selections.

**Week 3 — the control plane, and why it is a second language.** The request path wants predictable microseconds and no locks. Configuration wants retries, partial failure, and DNS — work measured in seconds that changes shape as requirements do. What crosses between them is a whole versioned snapshot, never a delta: a delta needs both sides to agree about the state it applies to, and they stop agreeing the first time a message is lost. The data plane polls it, parses it with a hand-written JSON reader, and swaps the route table atomically — `Router::SetTable` keeps per-endpoint breaker state across the swap, because a control plane polling every second would otherwise reset every circuit breaker every second and they would never open.

**Week 4 — consistent hashing and health.** Virtual nodes, and the three-state health model (unknown / healthy / unhealthy) that stops a freshly deployed endpoint from receiving traffic on faith.

**Week 5 — retries and breakers, designed together.** Covered below.

**Week 6 — rate limiting and shedding.** Queue depth as the signal, not latency.

**Week 7 — instrumentation and stress testing.** A log-linear latency histogram, per-cluster counters, and a deliberately misbehaving origin server so failure tests do not need a real upstream to break on command.

### Failure protection only works as a set

This is the thing I would most want to have understood at the start. Each of these four, alone, makes an outage *worse*:

| mechanism | alone, it… |
|---|---|
| retries | multiply load exactly when the upstream can least take it |
| circuit breakers | convert a partial failure into a total one, and synchronise everyone's recovery probe |
| rate limits | reject traffic the system could have served |
| load shedding | needs a signal, and the obvious one (latency) lags the overload |

Together they work because each bounds a different failure mode. And the coupling that is easiest to get wrong — **shed load must not charge the circuit breaker.** Shedding is the proxy protecting the upstream; if it trips the breaker, self-protection becomes a self-inflicted outage. That lives in `Status::charges_breaker()` and has its own test.

### A retry budget, not a retry count

My first version allowed three attempts per request. That bounds amplification *per request* at 3× and bounds it in aggregate at nothing: when every request starts failing, total upstream load triples at precisely the worst moment.

A budget expressed as "retries may be at most 20% of successful requests" bounds the aggregate instead. Budget is earned from successes only — so during a total outage there are no successes, the bucket drains, and retries *stop*. `retry_test.cpp` asserts this as a comparison: a total outage produces strictly fewer retries than a partial one.

## 🧪 How I Tried to Break It

**Smuggling has its own test file.** Thirteen cases, every one *rejected* rather than resolved, because any precedence rule the proxy applies is one the upstream may apply differently. Plus a positive case, because a file of all-negatives passes trivially if the parser rejects everything.

**The parser is fed at every byte boundary.** A request arrives in whatever chunks the network decides, and a parser that works on whole buffers and fails on a split at byte 23 passes every naive test.

**Distribution is measured, not asserted.** The ring bug — 99% skew — passed every functional property: deterministic lookup, distinct failover chain, correct *fraction* of keys remapped on removal. Only counting where 100,000 keys actually landed showed it.

**Policy is tested on a virtual clock.** Nothing in `policy.hpp` reads the wall clock, and the load shedder takes its random roll as a parameter, so the shedding curve is checked exactly rather than statistically.

**A deliberately misbehaving origin.** `relay-origin` takes its failures as flags: fixed latency, error rate, fail-after-N, hang, drop. Breaking a real server on request is not a thing you can do.

**ASAN and UBSAN as a separate build**, because clang refuses to combine them with TSAN and keeping them apart keeps what each proves apart.

### Bugs worth recording

**FNV-1a is unsuitable for ring points.** It diffuses into the low bits well and the high bits poorly, so short keys sharing a prefix cluster at the top — and the ring is sorted by the full word. All 160 points per endpoint landed in one arc. A MurmurHash3 finalizer fixed it; both languages now pin the same vector.

**A nested `Config` cannot be a default argument.** `explicit CircuitBreaker(Config config = {})` fails to compile across six classes: a default argument cannot aggregate-initialise a nested struct whose members have default initialisers, because the enclosing class is not complete at that point in its own definition. A delegating constructor's mem-initialiser list *is* a complete-class context.

**The load shedder admitted everything at exactly its limit.** `ShedProbability` tested the soft threshold before the hard one, and with `soft_threshold = 1.0` the two coincide — so the proxy admitted freely at precisely the depth it was configured to stop at.

**A percentile that truncated where its own comment promised a ceiling.** `p99` of two samples read as the median. A latency metric lying in the flattering direction is the worst way for one to be wrong.

**The rate limiter stopped limiting the client it most needed to.** Eviction replaced an arbitrary slot, so under key pressure a hot key was constantly thrown out and re-admitted with a fresh allowance. It now evicts the *most idle* bucket — a hot key has few tokens and survives.

**Pool metrics reported zero on a pool with 99.9% reuse.** The transport was bound to a `ClusterStats` at construction that the router never rendered. Found by reading the stress-test output rather than by any test.

**A test macro that printed freed memory.** `CHECK_EQ` captured its operands with `auto&&`, which lifetime-extends a temporary *value* but not a reference *into* one — so `CHECK_EQ(Parse(json)->AsString(), "x")` bound a reference to a string whose owning `shared_ptr` died at the end of that statement, and the failure output printed whatever was in the freed bytes. The operands are now copied by value.

**The accept queue made load shedding unreachable.** One flag set both the shed threshold and the accept queue, so excess load got TCP resets from the accept loop before any worker consulted the shedder. A client cannot tell a reset from a dead proxy.

## 📊 What the Measurements Say

Full detail in [`bench/results.md`](bench/results.md). The headlines:

**P2C is O(1); least-request is O(n).** At 1000 endpoints, 3.5 ns against 482 ns — **138×**. P2C holds flat across three orders of magnitude of cluster size.

**Round-robin is not the cheap one** — also O(n) (6.2 → 33.5 → 291 ns), because weighted selection walks the list accumulating weights. I had assumed otherwise.

**Headers cost more than bodies.** Parsing five headers with no body is 310 ns; parsing a 1 KiB body is 199 ns. Each header is a line scan plus token validation plus a lowercase pass; a body is a bounded `memcpy`.

**The entire policy layer costs ~13 ns per request** — 0.15% of end-to-end cost. There is no performance argument for turning any of it off.

**Connection pooling achieves 99.9% reuse** in the stress test: three connections opened, 3,198 hits.

**End to end: ~9,000 req/sec, 114 µs mean.** The decision path is ~400 ns of that — 0.35%. The rest is syscalls and scheduling, and routing is serialised behind a mutex because the router holds non-atomic policy state. That is the ceiling, and it is architectural.

## 📚 What I Learned

**A retry count bounds the wrong thing.** Per-request amplification is not the quantity that matters during an outage; aggregate load is. Earning retry budget from successes inverts the failure mode so that total failure produces fewer retries rather than more.

**Self-protection must not look like upstream failure.** The proxy refusing work and the upstream failing are different events with the same shape — a request that did not succeed. Collapsing them means your load shedder trips your circuit breakers.

**Functional correctness and distribution correctness are different properties.** The ring hash passed every behavioural test with a 99% skew. If a structure's value is statistical, the tests have to be statistical, and I would now reach for a distribution measurement on anything whose job is to spread things out.

**"Reject rather than resolve" is the only safe answer to ambiguity in a proxy.** Any rule for resolving conflicting framing is a rule the upstream might apply differently, and the gap between the two interpretations is the vulnerability. This generalises past HTTP: a component in the middle should refuse what it cannot interpret unambiguously.

**Two languages is a boundary, not a cost.** The request path and the configuration path have genuinely opposite requirements, and splitting them forced the interface between them to be a real protocol — a whole versioned snapshot — rather than a function call that would have quietly coupled them.

**The cheap parts are the ones I expected to be expensive, and the architecture is the expensive part.** 0.35% of a request is my code; 99.65% is the thread-per-connection model. Measuring that stopped me optimising a parser that was never the problem.

## ⚖️ What I Do Not Claim

- **This is not a high-performance proxy, and ~9,000 req/sec is the measurement that says so.** Envoy does six figures per core. The architecture is a bounded thread pool over blocking sockets, not an event-loop reactor, and **routing is serialised behind a single mutex** because the router holds per-endpoint breakers and per-cluster budgets as plain non-atomic state. The fix — per-worker policy state with periodic merging — is a real trade I did not make.
- **No TLS, in either direction.** No HTTPS listener, no upstream TLS, no SNI, no certificate verification. For an L7 proxy in 2026 this is the largest functional gap.
- **HTTP/1.1 only.** No HTTP/2, no h2c, no gRPC, no WebSocket upgrade — `Upgrade` is stripped as hop-by-hop. HTTP/2 would mean multiplexing, flow control and HPACK, which is a different project.
- **Configuration propagation is bounded by the poll interval, not instant.** The data plane polls (`-control-plane host:port`), so a change takes up to one interval to land. That is the deliberate trade for not having the control plane track delivery; it does mean a health change and the traffic shift are not simultaneous.
- **There is no config rollback.** A snapshot that fails validation is refused and the previous table keeps serving, which is the right default — but there is no way to tell the data plane to go back to version N-1 once a *valid* but wrong config has been applied. Rolling forward is the only option.
- **Bodies are fully buffered**, bounded at 8 MiB. No streaming, so a large upload occupies memory for its duration and a streaming response is not possible.
- **The retry-safety signal is coarse.** `request_was_sent` distinguishes "never written" from "possibly processed", which makes retrying a POST safe in the first case. It cannot distinguish "written but not processed", so a timeout on a non-idempotent request is never retried — correct, and more conservative than a real proxy with idempotency keys would need to be.
- **5xx responses do not charge the circuit breaker.** An upstream returning 500 for one bad route would otherwise open the breaker for all its traffic. Whether 5xx *should* count is a genuine policy question whose answer depends on the upstream; this picks the conservative side and does not make it configurable.
- **The rate limiter is approximate under key pressure.** A fixed table with a 4-slot probe window means a client can evict another's bucket and briefly get a fresh allowance. The alternative, unbounded tracking, lets a rotating identifier exhaust memory — which turns a rate limiter into an amplification vector.
- **`max_idle` must be shorter than the upstream's own idle timeout, and the proxy cannot discover that value.** It is a configuration the operator has to get right; getting it wrong produces intermittent spurious 502s, which is exactly the failure mode pooling is supposed to prevent.
- **No TSAN run is reported.** The build option exists (`RELAY_TSAN=ON`) and the concurrency surface is small — the server's queue and the router mutex — but the test suite is single-threaded apart from `pool_test`, so a TSAN run would mostly prove that single-threaded code has no races. The honest statement is that the data plane's thread-safety rests on the mutex in `server.cpp` and on review, not on a sanitizer.
- **Chunked trailers are parsed and dropped**, because the proxy re-frames every body with a `Content-Length` it computed itself. Deliberate, and it does mean discarding data a client sent.
- **The balancer assumes the control plane appends rather than reorders endpoints.** Live load counts are preserved by index across a snapshot swap, so a reordering push would misattribute in-flight counts. The control plane sorts endpoints by address, which makes this true in practice and is not enforced.
- **No access logging.** Counters and latency histograms only, so there is no per-request record to debug a specific client's problem with.
- **The histogram's percentiles are not exact.** Bucket-bounded error, about 6% relative. Bucket counts merge exactly, so a merged histogram is as good as a single one — neither is exact, and a histogram presented as exact is worse than one presented honestly.

## 🛠️ Tech Stack

- **C++20**, Apple clang / libstdc++, standard library only. No Boost, no Abseil, no HTTP library, no test framework.
- **Go 1.24**, standard library only — `go.mod` has no `require` block.
- CMake, with `RELAY_ASAN` and `RELAY_TSAN` as separate build directories because clang refuses to combine them.
- `overflow-checks` are not relevant in C++, but `-Wall -Wextra -Wpedantic` is clean and the build is warning-free.

## 🚀 Setup

```bash
# Data plane
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j8
(cd build && ctest --output-on-failure)

# Sanitizers, as a separate build directory
cmake -S . -B build-asan -DRELAY_ASAN=ON && cmake --build build-asan -j8
(cd build-asan && ctest)

# Control plane
cd go-control-plane && go test -race ./... && go build ./... && cd ..

# Everything: proxy + control plane + 3 origins, a dead origin, and a flood
./scripts/demo.sh

# Numbers
./build/relay-bench      # decision-path CPU
./scripts/stress.sh      # end-to-end throughput
```

Running it by hand:

```bash
./build/relay-origin -port 9001 -body hello &
./build/relay-origin -port 9002 -latency-ms 50 &

# Static configuration
./build/relay-proxy -port 8080 -workers 8 \
    -upstreams 127.0.0.1:9001,127.0.0.1:9002 \
    -balancer p2c -max-attempts 3 -retry-ratio 0.2 \
    -queue-depth 64 -accept-queue 256

# Or driven by the control plane, with -upstreams as the bootstrap only
./build/relay-proxy -port 8080 -workers 8 \
    -upstreams 127.0.0.1:9001 \
    -control-plane 127.0.0.1:9901 -control-poll-ms 1000

go run ./go-control-plane/cmd/relay-control-plane \
    -addr 127.0.0.1:9901 -cluster "api=127.0.0.1:9001,127.0.0.1:9002"

curl -s localhost:9901/status   | python3 -m json.tool
curl -s localhost:9901/health   | python3 -m json.tool
curl -s "localhost:9901/placement?cluster=api&key=/user/42"
```

## 🗺️ Repository Map

```
data-plane/
  include/relay/
    status.hpp        the error taxonomy, and why kOverloaded is not a breaker failure
    http.hpp          HTTP/1.1, with the smuggling defences stated up front
    json.hpp          a bounded JSON reader, for configuration only
    configclient.hpp  polls the control plane and swaps the route table
    upstream.hpp      endpoints, five balancers, the consistent hash ring
    policy.hpp        breakers, retry budgets, rate limits, shedding — as one set
    pool.hpp          connection reuse, and the three spurious-502 defences
    metrics.hpp       log-linear latency histogram, per-cluster counters
    socket.hpp        sockets, and an honest note about the threading model
    proxy.hpp         the router: the order of operations IS the design
  src/                implementations
  tests/              13 binaries, 199 cases
  tools/              relay-proxy, relay-origin (misbehaves on request), relay-bench

go-control-plane/
  config/             the snapshot model, validation, and the shared hash
  discovery/          resolution vs health, and why they fail differently
  api/                pull-based config serving, ETags, placement prediction
  cmd/                relay-control-plane

bench/results.md      nine claims, three of them refuted
scripts/demo.sh       end to end, including a dead origin and a flood
scripts/stress.sh     end-to-end throughput
```

## 📖 References

- [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112) — HTTP/1.1 message syntax. §6 is the framing rules the smuggling defences implement.
- [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110) — HTTP semantics; method idempotency, which gates retry safety.
- Mitzenmacher, [*The Power of Two Choices in Randomized Load Balancing*](https://www.eecs.harvard.edu/~michaelm/postscripts/handbook2001.pdf) — why two random samples get most of the benefit of a full scan.
- Karger et al., [*Consistent Hashing and Random Trees*](https://www.cs.princeton.edu/courses/archive/fall09/cos518/papers/chash.pdf) (STOC 1997).
- Nygard, *Release It!* — circuit breakers and bulkheads as a design vocabulary.
- [Envoy's architecture docs](https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/arch_overview) — the retry budget idea, and a standing reminder of the gap.

## 📄 License

MIT. See [LICENSE](LICENSE).
