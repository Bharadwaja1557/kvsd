# Benchmarks

All numbers here were measured on one machine, on macOS, against the kqueue backend.
**The Linux/epoll build is not benchmarked anywhere in this document.** It is verified
for correctness by CI (see `.github/workflows/ci.yml`, which runs the full test suite
on `ubuntu-latest`), and that is the only claim made about it. I do not have a Linux
machine, and a number I cannot reproduce is a number I cannot defend.

## Test machine

| | |
|---|---|
| Machine | Apple M4, 10 cores, 16 GB |
| OS | macOS 26.5.2 (build 25F84), arm64 |
| Compiler | Apple clang 21.0.0 |
| Build | `RelWithDebInfo` (`-O2 -g`), the configuration `scripts/verify.sh` produces |
| Poller backend | kqueue |
| Comparison server | `redis-server` 8.10.0 (Homebrew), started with `--save '' --appendonly no` |
| Benchmark client | `redis-benchmark` 8.10.0, on the same host over loopback |

Both servers were run one after the other in the same session by `scripts/bench.sh`,
which starts a server, benchmarks it, stops it, and only then starts the other. Nothing
but the server under test changes between the two runs.

Persistence is disabled on the Redis side because kvsd has none yet (that is Phase 2).
Leaving RDB snapshotting on would have measured a feature only one of the two has.

## Results: the required workload

`redis-benchmark -p <port> -t set,get -n 100000 -c 50`, three rounds each, median
reported. Figures are requests per second, higher is better.

| Workload | kvsd | redis-server 8.10.0 | ratio (kvsd / redis) |
|---|---:|---:|---:|
| SET | 230,947 | 244,499 | 0.95x |
| GET | 234,192 | 245,700 | 0.95x |

Per-round figures, requests/sec:

| Round | kvsd SET | kvsd GET | redis SET | redis GET |
|---|---:|---:|---:|---:|
| 1 | 226,757 | 233,100 | 242,718 | 241,546 |
| 2 | 230,947 | 234,192 | 244,499 | 245,700 |
| 3 | 234,192 | 235,294 | 249,377 | 248,139 |

Latency for the kvsd GET round, as `redis-benchmark` reports it (milliseconds):

| avg | min | p50 | p95 | p99 | max |
|---:|---:|---:|---:|---:|---:|
| 0.110 | 0.040 | 0.111 | 0.119 | 0.135 | 0.367 |

At 50 concurrent clients on loopback, both servers are close to the point where the
cost is dominated by the syscalls and the round trip rather than by anything either
server does with the data: a read, a parse, a hash lookup, and a write, with p50 latency
of about 111 microseconds on both. Being within 5% of Redis here mostly says that the
event loop and the parser are not doing anything stupid, not that the two are equivalent
pieces of software.

## Results: pipelined

`redis-benchmark -t set,get -n 1000000 -c 50 -P 16`, three rounds each, median reported.

| Workload | kvsd | redis-server 8.10.0 | ratio (kvsd / redis) |
|---|---:|---:|---:|
| SET, `-P 16` | 2,994,012 | 1,996,008 | 1.50x |
| GET, `-P 16` | 3,048,780 | 2,617,801 | 1.16x |

**This is not evidence that kvsd is a better server than Redis.** Pipelining removes the
syscall and round-trip cost that dominated the first table and exposes the per-command
work, and kvsd is faster there for an unflattering reason: it does far less per command.
Redis 8 checks ACLs, fires keyspace notifications, maintains a dirty counter and
replication backlog, tracks client output-buffer classes, handles RESP3 client-side
caching invalidation, and dispatches through a much larger command table. kvsd looks up
a name in a hash map, checks an arity, and touches an `unordered_map`. The gap is the
price of the features it does not have.

The first version of this measurement used `-n 100000`, which at these rates completes
in about 30 milliseconds -- short enough that start-up effects dominate. The run was
repeated with `-n 1000000` for the numbers above; the per-round spread is under 2%.

## Caveats worth stating out loud

- **Client and server share a machine.** `redis-benchmark` is itself a busy process, and
  on 10 cores it competes with the server for CPU and for the loopback path. This
  compresses the difference between any two servers. It is the same distortion for both
  sides, which is what makes the comparison fair, but the absolute numbers are not what
  either server would do with a dedicated load generator.
- **One machine, one OS, one compiler.** No claim is made about Linux, about x86-64,
  about GCC, or about a machine with a different core count.
- **Three rounds is enough to see the spread, not to do statistics.** The spread across
  rounds (about 3% unpipelined) is reported so it can be judged directly; no confidence
  intervals are claimed.
- **The workload is 3-byte values, all hits, no expiry churn.** It says nothing about
  memory footprint, about behaviour at thousands of connections, about large values, or
  about a keyspace that is actively expiring while it is read. Those are the workloads
  where the active expiry cycle and the output buffer limit would start to matter, and
  they are not measured here.
- `redis-benchmark` prints `WARNING: Could not fetch server CONFIG` when pointed at
  kvsd. That is cosmetic: the benchmark asks for `CONFIG GET save|appendonly` to report
  the server's persistence settings, kvsd has no `CONFIG` command, and the benchmark
  proceeds normally.

## Reproducing

```sh
./scripts/verify.sh                                  # build the RelWithDebInfo binary
./scripts/bench.sh                                   # the table above
PIPELINE=16 REQUESTS=1000000 ./scripts/bench.sh      # the pipelined table
```

`scripts/bench.sh` needs `redis-server`, `redis-cli` and `redis-benchmark` on `PATH`
(`brew install redis`). `REQUESTS`, `CLIENTS`, `ROUNDS`, `PIPELINE`, `KVSD_PORT` and
`REDIS_PORT` can all be overridden from the environment.
