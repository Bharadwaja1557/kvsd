# kvsd

A Redis-compatible key-value server in C++17: single-threaded event loop, incremental
RESP2 parser, no dependencies beyond the standard library and POSIX.

It speaks the real protocol, so the real tools work against it:

```sh
$ ./build/kvsd --port 6380 &
$ redis-cli -p 6380
127.0.0.1:6380> set greeting "hello"
OK
127.0.0.1:6380> get greeting
"hello"
127.0.0.1:6380> ttl greeting
(integer) -1
```

Unpipelined, it sustains about 231,000 SET/s and 234,000 GET/s on an M4 -- roughly 0.95x
a stock `redis-server` on the same machine. The measurements, the method, and the
caveats are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

**Why it is built the way it is** -- event loop versus threads, level- versus
edge-triggered readiness, how the parser resumes mid-command, why expiry is lazy plus
sampled rather than a timer per key -- is in [docs/DESIGN.md](docs/DESIGN.md). That
document is the point of this project; the code is the evidence for it.

## Requirements

- CMake >= 3.16 and a C++17 compiler
- Linux (epoll) or macOS/BSD (kqueue)
- Python 3 for the integration test (optional; CTest skips it if absent)

## Build

```sh
cmake -B build
cmake --build build -j
```

Or, to do exactly what CI does -- clean configure, `-Wall -Wextra -Werror`, full test
suite -- in one command:

```sh
./scripts/verify.sh
```

## Run

```sh
./build/kvsd                          # 127.0.0.1:6380
./build/kvsd --port 7000 --verbose    # another port, with per-connection logging
```

| Option | Default | Meaning |
|---|---|---|
| `--port <n>` | `6380` | Port to listen on; `0` lets the kernel choose |
| `--bind <addr>` | `127.0.0.1` | IPv4 address to bind |
| `--max-output-buffer <n>` | `67108864` | Pending reply bytes per client before it is disconnected |
| `--expire-samples <n>` | `20` | Keys sampled per active expiry round |
| `--tick-interval <ms>` | `100` | Event loop tick period |
| `--verbose` | off | Log accepts, closes, and expiry detail |
| `--help` | | Usage |

`SIGINT` and `SIGTERM` shut the server down cleanly.

## Supported commands

RESP2, single database, string values.

| Command | Form | Notes |
|---|---|---|
| `PING` | `PING [message]` | `+PONG`, or the message echoed as a bulk string |
| `ECHO` | `ECHO message` | |
| `SET` | `SET key value [EX s\|PX ms] [NX\|XX]` | Options may appear in either order. A refused `NX`/`XX` replies with a null bulk. Replaces any existing TTL |
| `GET` | `GET key` | Null bulk if absent or expired |
| `DEL` | `DEL key [key ...]` | Number actually deleted |
| `EXISTS` | `EXISTS key [key ...]` | Counts occurrences, so `EXISTS k k` is 2 |
| `EXPIRE` | `EXPIRE key seconds` | A non-positive TTL deletes the key and still replies 1 |
| `TTL` | `TTL key` | Seconds, `-1` if no TTL, `-2` if no key |
| `PERSIST` | `PERSIST key` | 1 if a TTL was removed |
| `INCR` / `DECR` | `INCR key` | Creates the key at 0 first; preserves an existing TTL; refuses on overflow |
| `TYPE` | `TYPE key` | `string` or `none` |
| `DBSIZE` | `DBSIZE` | Entry count; may transiently include expired-but-unsampled keys, as in Redis |
| `FLUSHALL` | `FLUSHALL [ASYNC\|SYNC]` | The modifier is accepted and ignored -- there is no second thread |
| `COMMAND` | `COMMAND [DOCS\|COUNT]` | A stub, enough for `redis-cli` to connect cleanly |
| `QUIT` | `QUIT` | Replies `+OK`, then closes once the reply is on the wire |

Values and keys are binary safe. Inline commands (`PING\r\n` over `nc` or telnet) work
alongside the multibulk protocol.

Not implemented in Phase 1, deliberately: persistence, replication, RESP3, `CONFIG`,
`INFO`, `SELECT`, transactions, pub/sub, and the container types.
[docs/DESIGN.md §14](docs/DESIGN.md) lists them and says which ones already have hooks.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

- `tests/test_*.cpp` -- unit tests for the buffer, both poller backends, the event loop,
  the RESP parser and writer, the keyspace, and the command table. The harness is 40
  lines of `tests/test_util.h`, for the same no-dependencies reason as the server.
- `tests/integration_test.py` -- nineteen tests against a real server process over real
  sockets: pipelining, byte-at-a-time delivery, 8 MiB values, fifty concurrent clients,
  protocol errors, the output buffer limit, file-descriptor exhaustion under
  `ulimit -n 64`, and clean shutdown on `SIGTERM`.

CI runs the whole thing on `ubuntu-latest` and `macos-latest` with warnings as errors.
The Linux/epoll backend is verified there and nowhere else -- it is not benchmarked, and
[docs/BENCHMARKS.md](docs/BENCHMARKS.md) says so rather than quoting numbers nobody
measured.

## Benchmarks

```sh
./scripts/bench.sh                                 # kvsd vs redis-server, same host
PIPELINE=16 REQUESTS=1000000 ./scripts/bench.sh    # pipelined
```

Needs `redis-server`, `redis-cli` and `redis-benchmark` on `PATH`.

## Layout

```
src/net/      sockets, readiness poller (epoll | kqueue), event loop, buffers, server
src/resp/     incremental RESP2 parser, reply encoders
src/db/       keyspace, values, expiry
src/cmd/      command table and handlers
src/util/     logging
tests/        unit tests and the Python integration suite
docs/         DESIGN.md, BENCHMARKS.md
scripts/      verify.sh, bench.sh
```
