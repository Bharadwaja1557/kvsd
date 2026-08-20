# kvsd design notes

This document is written for someone who has not read the code. It explains what kvsd
is, how a byte travels from a socket to a reply, and -- more importantly -- why each
piece is built the way it is rather than one of the other ways it could have been.

kvsd is a key-value server that speaks RESP2, the protocol Redis uses, well enough that
the real `redis-cli` and `redis-benchmark` talk to it without knowing the difference.
It is single-threaded, has no dependencies beyond the C++17 standard library and POSIX,
and runs on Linux (epoll) and macOS/BSD (kqueue).

---

## 1. The shape of the program

```
  src/net/     sockets, the readiness poller, the event loop, per-connection buffers
  src/resp/    the incremental RESP2 parser and the reply encoders
  src/db/      the keyspace: a hash map, expiry deadlines, the expiry cycle
  src/cmd/     the command table: name -> handler, with arity checking
  src/util/    logging
  src/main.cpp arguments, signals, startup
```

The dependency arrows all point one way: `net` knows about `resp`, `cmd` and `db`;
`cmd` knows about `db` and `resp`; `resp` and `db` know about nothing above them. In
particular **no command handler can touch a socket**. A handler is handed a parsed
argument vector and an output buffer, and appends bytes to it. That single restriction
is what makes the command layer testable without a network, and it is also what makes
pipelining fall out for free: a hundred commands parsed from one `read()` append their
replies to the same buffer and leave with one `write()`.

The whole server is one thread. There is no mutex anywhere in the codebase.

---

## 2. Why an event loop instead of a thread per connection

The thread-per-connection design is the one that reads most naturally: accept a socket,
hand it to a thread, and let that thread block on `read()` in a simple loop. For a
server like this it is the wrong choice, for four reasons in increasing order of
importance.

**Memory.** A thread needs a stack, and the default is 512 KiB on macOS and 8 MiB of
reserved address space on Linux. Ten thousand idle connections is somewhere between
five and eighty gigabytes of stacks, and a Redis-shaped workload is mostly idle
connections: clients hold a connection open for the life of the process and use it in
short bursts. The event loop holds two buffers and about a hundred bytes of bookkeeping
per connection instead.

**Scheduling cost.** Every command in a thread-per-connection server is a context
switch in and a context switch out. At the ~230,000 requests/second this server
sustains (see [BENCHMARKS.md](BENCHMARKS.md)), the scheduler would be doing more work
than the server. The event loop handles a batch of ready connections between two
syscalls, on a hot cache, with no switch at all.

**Locking.** This is the big one. The keyspace is shared state. With N threads it needs
a lock, and one lock over one hash map serializes exactly the work the threads were
supposed to parallelize -- so you shard the keyspace, and then multi-key operations need
to take locks in a canonical order, and then you are debugging lock-order inversions in
a key-value store. With one thread, `unordered_map<string, Value>` is the whole story
and every command is atomic because nothing can interleave with it. Redis made this
trade for the same reason, and it is the reason Redis's semantics are easy to reason
about.

**Explainability.** A single-threaded server has one place where time passes: the top of
the loop. Reasoning about a bug means reading a sequence, not a schedule.

The cost of this choice is real and worth stating: kvsd uses one core. A CPU-bound
command would stall every other client, and there is no way to spend the other nine
cores of the benchmark machine. The mitigation is that everything a Phase 1 command does
is a hash lookup and a memcpy -- microseconds -- and the design never allows a blocking
call on the loop thread. Every socket is non-blocking, and the only call that ever waits
is the poller's, which has a timeout.

---

## 3. Readiness: kqueue and epoll

The loop needs one primitive: *tell me which of these file descriptors I can act on
without blocking.* Both kernels provide it and neither agrees with the other on the
details.

### What the syscalls actually do

**epoll (Linux)** is three calls around a kernel object.

- `epoll_create1(EPOLL_CLOEXEC)` creates an epoll instance -- itself a file descriptor
  -- holding an *interest set* (which fds, which events) and a *ready list*.
- `epoll_ctl(epfd, op, fd, &event)` mutates the interest set: `EPOLL_CTL_ADD`,
  `EPOLL_CTL_MOD`, `EPOLL_CTL_DEL`. `event.events` is a bitmask for one fd -- `EPOLLIN |
  EPOLLOUT | EPOLLRDHUP` -- so read and write interest live in the same registration
  and changing one means rewriting both. One call per fd per change.
- `epoll_wait(epfd, events, maxevents, timeout_ms)` blocks until something in the
  interest set is ready and fills an array. **One entry per fd**, with a combined mask.

**kqueue (macOS/BSD)** is two calls, and is a more general mechanism that happens to
cover sockets.

- `kqueue()` creates the queue, also a file descriptor.
- `kevent(kq, changelist, nchanges, eventlist, nevents, timeout)` is registration *and*
  waiting in a single call: it applies the changes in `changelist`, then blocks and
  fills `eventlist`. Registrations are built with the `EV_SET` macro and identified by
  the pair *(ident, filter)* -- `EVFILT_READ` and `EVFILT_WRITE` are **separate
  registrations for the same fd**, not bits in one mask. `EV_ADD`, `EV_DELETE`,
  `EV_ENABLE` and `EV_DISABLE` are flags on a change. kqueue also handles timers,
  signals, process exits and file changes through the same interface; none of that is
  used here.
- Because filters are independent, kqueue returns **one entry per (fd, filter)**: a
  socket that is both readable and writable produces two events in the same batch.

### How kvsd registers, on each

| | epoll | kqueue |
|---|---|---|
| create | `epoll_create1(EPOLL_CLOEXEC)` | `kqueue()`, then `fcntl(FD_CLOEXEC)` |
| add an fd | one `epoll_ctl(ADD)` with `EPOLLIN\|EPOLLRDHUP`, plus `EPOLLOUT` if wanted | one `kevent()` with two changes: `EVFILT_READ` `EV_ADD\|EV_ENABLE`, and `EVFILT_WRITE` `EV_ADD` either enabled or disabled |
| arm/disarm writes | `epoll_ctl(MOD)` rewriting the whole mask | `kevent()` with `EV_ENABLE`/`EV_DISABLE` on `EVFILT_WRITE` |
| remove | `epoll_ctl(DEL)` | `kevent()` with `EV_DELETE` on both filters |
| wait | `epoll_wait()` | `kevent()` with an empty changelist |

Two decisions in that table are worth their comments in the code. The write filter is
registered up front and toggled with `EV_ENABLE`/`EV_DISABLE` rather than added and
deleted, because `EV_DELETE` on a filter that is not registered fails with `ENOENT` and
would force the caller to track registration state that epoll does not require it to
track. And the changelist is never batched into the waiting call, even though kqueue
would allow it, because matching epoll's one-call-per-change shape keeps the two
backends behaviourally identical -- and the change rate in steady state is nearly zero
anyway (see §6).

### Level-triggered, on both, deliberately

A **level-triggered** poller reports a descriptor as ready whenever it *is* ready: if
2 KiB arrive and you read 1 KiB, the next wait reports it readable again, because there
is still data. An **edge-triggered** poller reports only the *transition*: the arrival
of those 2 KiB is one notification, and if you do not drain the socket to `EAGAIN` you
will not hear about the remainder until more bytes arrive -- which, for a client waiting
on a reply to bytes it already sent, is never. That is a hung connection, and it is a
hang that only appears under load, when reads start coming back short.

epoll is level-triggered unless you set `EPOLLET`. kqueue is level-triggered unless you
set `EV_CLEAR`. kvsd sets neither, in both backends, on purpose:

- **The correctness burden is on the right side.** Edge triggering makes "loop until
  `EAGAIN`" a requirement, and forgetting it in one place produces a stall that no unit
  test will find.
- **It buys the fairness knob.** Because leftovers are re-reported, kvsd can
  deliberately do one 16 KiB `read()` per readiness event and go serve someone else. The
  rest of that client's data is waiting at the next iteration. Under edge triggering,
  one client with a megabyte in flight must be drained completely before the loop can
  move on.
- **It makes the fd-exhaustion recovery possible at all** (§8). A listener we cannot
  accept from stays readable, which is what turns the problem into a visible spin
  instead of a silently dropped connection -- and it is what lets the loop retry.

What level triggering costs is one wasted wakeup in one specific case: a socket that is
always writable and registered for writes will be reported ready on every single wait.
That is why write interest is armed only while there is a partial write outstanding
(§6). With that one rule observed, level triggering costs nothing here.

### What `poller.h` hides, and what it does not

`Poller` is the answer to "why did you abstract this?", so it is worth being precise
about where the seam is.

It **hides**:
- which syscall family exists on this host, and its spelling;
- that kqueue needs two registrations per fd and epoll needs one;
- that kqueue reports one event per filter while epoll reports one per fd. The kqueue
  backend coalesces per-fd, so the loop always sees at most one `PollEvent` per fd per
  batch. Without that, an fd reported readable *and* writable would be dispatched
  twice, the first dispatch could close the connection, and the second would run against
  a closed fd -- or against a brand-new connection that had already reused the number;
- that "the peer hung up" is spelled `EPOLLRDHUP`/`EPOLLHUP` on one side and `EV_EOF` on
  the other. Both are normalized to *readable, with an error flag*, so that teardown
  happens in exactly one place: the read path, where `read()` returning 0 already means
  the same thing;
- that an interrupted wait is `EINTR`. Both backends report it as zero events, so a
  signal during shutdown is a normal empty iteration rather than an error.

It deliberately **does not hide**:
- **Read interest.** It is always on. Nothing in kvsd wants to stop reading from a live
  connection, and an unused knob is an untested knob.
- **Anything kqueue can do that epoll cannot.** Timers, signal delivery, `EVFILT_VNODE`
  -- all absent. An abstraction over the *union* of two APIs is one that cannot be
  implemented on either.
- **The event loop's policy.** `Poller` reports readiness. It does not own connections,
  buffers, timeouts, or the decision to close anything.
- **The choice itself.** There is no virtual function and no runtime branch: CMake
  compiles exactly one of `poller_epoll.cpp` / `poller_kqueue.cpp` based on the target
  system, and the linker resolves `Poller::wait` to it. Only one of the two syscall
  families exists on any given host, so paying for dynamic dispatch would buy
  portability the build system already provides for free.

The honest reason the abstraction exists is not portability for its own sake. It is that
`tests/test_poller.cpp` runs unmodified on both backends: it encodes the semantics
`poller.h` promises, so the epoll build cannot quietly diverge from the kqueue build
that the benchmarks were taken on. Since I cannot run Linux, that test plus CI is the
only thing standing between "the epoll backend compiles" and "the epoll backend works".

---

## 4. The connection lifecycle

```
                        listener readable
                               |
                               v
                     +---------------------+   accept() == EMFILE/ENFILE
                     |     ACCEPTING       |----------------------------> [ SHED  ]
                     +---------------------+                              (see §8)
                               | accept() >= 0
                               | O_NONBLOCK, TCP_NODELAY, SO_NOSIGPIPE
                               v
              +--------------------------------+
     +------> |             IDLE               |  registered: read interest only
     |        |   (in and out both empty)      |  the steady state of a live client
     |        +--------------------------------+
     |                        | readable
     |                        v
     |        +--------------------------------+
     |        |            READING             |  one read() of at most 16 KiB
     |        +--------------------------------+
     |                        |
     |            +-----------+-----------+---------------------+
     |            | read()==0 | read()>0  | read()<0 (real)     |
     |            v           v           v                     
     |        [ CLOSING ]     |       [ CLOSING ]
     |                        v
     |        +--------------------------------+
     |        |            PARSING             |  parse one command from `in`
     |        +--------------------------------+
     |            |             |            |
     |     Incomplete       Complete       Error
     |            |             |            |
     |            |             v            v
     |            |     +---------------+   queue "-ERR Protocol error: ..."
     |            |     |  DISPATCHING  |   set close_after_write
     |            |     +---------------+            |
     |            |             |                    |
     |            |     append reply to `out`        |
     |            |     QUIT? set close_after_write  |
     |            |     out > cap? --> [ CLOSING ]   |
     |            |             |                    |
     |            |             +--> back to PARSING (pipelined commands)
     |            |                                  |
     |            +----------------+-----------------+
     |                             v
     |        +--------------------------------+
     |        |            WRITING             |  send() until EAGAIN or empty
     |        +--------------------------------+
     |             |                        |
     |     out empty                out non-empty
     |             |                        |
     |     close_after_write?        arm write interest
     |        no /      \ yes               |
     |          /        v                  v
     +---------+     [ CLOSING ]    +--------------------+
                                    |   DRAINING         | registered: read + write
                                    | (partial write)    |
                                    +--------------------+
                                             | writable
                                             v
                                        back to WRITING
                                    (on empty: disarm write interest,
                                     return to IDLE or CLOSING)

  [ CLOSING ]:  unregister from the poller  ->  close(fd)  ->  destroy the Conn
                ->  if the listener was paused, try to resume it
```

Three properties of that diagram are load-bearing:

- **`CLOSING` always unregisters before `close()`.** The kernel hands a closed
  descriptor number straight back to the next `accept()`, and a stale registration would
  route a new client's events into a dead callback.
- **`close_after_write` is never a close.** QUIT, a protocol error, and a soft shutdown
  all set a flag; the socket goes away only once `out` has drained. Closing on the spot
  would discard the `+OK` the client is blocked reading.
- **The loop from `DISPATCHING` back to `PARSING`** is pipelining. N commands in one
  read produce N replies in one buffer and one `WRITING` pass.

---

## 5. Buffers

Each connection owns two `Buffer`s: `in` for bytes the kernel gave us that do not yet
form a whole command, and `out` for replies the kernel would not take yet. Both are a
`vector<char>` with a read cursor and a write cursor.

The read cursor is the interesting part. The obvious implementation of "consume 40
bytes" is to erase them from the front, which is a `memmove` of everything behind them.
Under pipelining -- a hundred commands in one buffer -- that is a `memmove` per command
and quadratic behaviour exactly when throughput matters. Consuming is instead a cursor
bump, and the dead prefix is reclaimed only when the buffer needs room and reclaiming
would provide it: one `memmove` amortized over many commands.

Growth doubles up to 16 MiB and then grows linearly in 16 MiB steps, because doubling a
512 MiB bulk string would ask the allocator for a gigabyte to hold 512 MiB and one byte.

---

## 6. Why the write path is optimistic

When a reply is ready, kvsd calls `send()` immediately rather than waiting to be told
the socket is writable. Almost always the socket buffer has room, the whole reply
leaves, and the connection never touches the poller at all. Write interest is armed
*only* when `send()` came back short or `EAGAIN`, and disarmed the moment the buffer
drains.

This is not a micro-optimization; under level triggering it is a correctness-adjacent
requirement. A registered, idle, writable socket is reported ready on *every* `wait()`
-- a connection with nothing to send would spin the loop at 100% CPU. Arming write
interest only for a partial write means the steady state has zero write registrations,
which is also why not batching kqueue changelists costs nothing (§3): in steady state
there are no changes to batch.

---

## 7. The incremental parser

The parser is the piece most shaped by the fact that TCP is a byte stream and not a
message stream. A single `read()` can return:

- half of one command (`*3\r\n$3\r\nSE`),
- exactly one command,
- seventeen commands and a fragment of the eighteenth,
- one byte.

All four must work, and none of them may cost more than the bytes involved.

### How state is held between reads

`RespParser` is a small state machine, one instance per connection, with four members:

```
  state_          Start | ArgLen | ArgData
  args_remaining_ arguments still expected in the current multibulk
  arg_len_        declared length of the bulk argument being read
  argv_           arguments completed so far for the in-flight command
```

The invariant that makes it incremental: **bytes are consumed from the buffer as soon as
they form a complete token, and everything needed to resume lives in those four
members.** Reading `*3\r\n$3\r\nSE` consumes the `*3\r\n` (setting `args_remaining_ =
3`) and the `$3\r\n` (setting `arg_len_ = 3`, `state_ = ArgData`), then finds only two
of the five bytes it needs and returns `Incomplete`, leaving `SE` in the buffer. When
the next read appends `T\r\n$1\r\nk\r\n...`, parsing resumes in `ArgData` with three
bytes available and never looks at what came before.

The alternative -- keep the whole command in the buffer and re-scan from byte zero each
time more arrives -- is much easier to write and quadratic in the number of reads. A
512 MiB bulk string arriving in 16 KiB chunks would be re-scanned 32,768 times.

The caller's loop is therefore: parse; if `Complete`, dispatch and parse again; if
`Incomplete`, stop and wait for more bytes; if `Error`, reply and close. Pipelining is
just the "parse again" branch, and partial commands are just the "stop" branch. There is
no special case for either.

### Framing errors are fatal to the connection

A protocol error is not like a wrong-arity error. If a client says a bulk argument is 3
bytes and the bytes at offset 3 are not `\r\n`, the framing is wrong and there is no
principled place to resynchronize -- every subsequent byte is of unknown meaning.
kvsd replies `-ERR Protocol error: ...` and closes. Guessing would corrupt the client's
view of which reply belongs to which request, which is worse than a disconnect.

Note the asymmetry: a payload *shorter* than its declared length is not an error at all,
it is a command still arriving. Only a payload that overruns its declared length proves
the framing is broken.

### Inline commands

Anything that does not start with `*` is treated as an inline command: a whitespace-
separated line terminated by `\n`, with quoting rules matching redis-cli's. This is what
makes `nc`, `telnet` and a shell `printf` usable against the server, which in turn makes
debugging the wire format possible without a client library.

### Ceilings

Without limits, an eleven-byte header (`*9999999999`) asks the server to commit
unbounded memory. The parser caps the argument count (1M), a single bulk argument
(512 MiB) and an inline line (64 KiB); the defaults match Redis so a client that works
against redis-server works here. A header line that grows past 64 bytes without a CRLF
is rejected too -- that is not a slow client, that is junk.

---

## 8. Running out of file descriptors

`accept()` can fail with `EMFILE` (this process is at its descriptor limit) or `ENFILE`
(the system is). This is the failure that turns into an outage rather than an error,
because of level triggering: the pending connection stays in the accept queue, the
listener therefore stays readable, `wait()` returns immediately, `accept()` fails again
-- and the server burns a core in a tight loop, serving nobody, until an operator
notices. Ignoring the error is not an option, and neither is exiting.

**kvsd uses the reserved-spare-fd technique.** At startup the server opens `/dev/null`
and holds the descriptor for no reason other than to have one to spend later. On
`EMFILE`/`ENFILE` it closes the spare, uses the descriptor that just freed up to
`accept()` the pending connection, immediately `close()`s it, and reopens the spare.
The client gets a fast, clean disconnect instead of a hang, the accept queue is drained,
the listener stops being readable, and the loop goes back to serving the clients it
already has.

Why this one rather than the alternatives:

- **Versus doing nothing:** doing nothing is the 100% CPU spin described above.
- **Versus switching the listener to edge-triggered:** it would stop the spin, but the
  connection would sit unaccepted in the queue with the client hanging until it times
  out, and the server would need some other event to remember to try again.
- **Versus unregistering the listener until a connection closes:** this is a real
  option, and kvsd uses it as a *fallback* -- if the spare descriptor cannot be
  reacquired, the listener is removed from the poller entirely and re-registered when a
  connection closes and frees a descriptor. It is the fallback rather than the primary
  because on its own it leaves the pending client hanging rather than telling it no, and
  because a server that has stopped listening is a harder state to diagnose than one
  that is logging a rejection per attempt.

Both paths log. The integration test starts a server under `ulimit -n 64`, floods it
with 300 connections, and asserts that it keeps serving its existing clients, accepts
new ones once descriptors free up, logs the rejection, and exits cleanly.

---

## 9. Why an output buffer limit is not optional

`out` grows when the kernel will not take bytes, which happens when the peer is not
reading them. Nothing about the protocol prevents a client from pipelining ten thousand
`GET`s of a one-megabyte value and then never calling `read()` -- it does not even have
to be malicious, a client that is paused in a debugger, swapping, or simply slower than
the server does the same thing. The replies have to go somewhere, and with no limit that
somewhere is the server's heap: one client can convert its own slowness into unbounded
memory growth in a process that serves everybody, and the failure mode is the OOM killer
taking down the server for all its clients. A limit converts an unbounded, shared,
fatal failure into a bounded, local, recoverable one: kvsd checks the buffer after every
command in a pipelined batch, and a connection that pushes past
`--max-output-buffer` (64 MiB by default) is logged and dropped. It is dropped without
flushing, because a client that has accumulated that much unread output is by definition
not reading. Redis has the same mechanism, with per-class limits, for the same reason.

---

## 10. Expiry

A key can carry an absolute deadline in milliseconds since the Unix epoch. Two
mechanisms enforce it, and neither one is a timer.

### Lazy expiry

Every read path in the keyspace goes through `Db::lookup`, which compares the deadline
against the current time and, if it has passed, deletes the entry and reports the key as
absent. `GET`, `TTL`, `EXISTS`, `DEL`, `TYPE`, `PERSIST` and `EXPIRE` all inherit this,
so **no expired key is ever observable**, whatever else is or is not running. Lazy expiry
alone is sufficient for correctness.

### Active expiry

What lazy expiry cannot do is free memory for a key nobody asks about again -- the
common case for a cache. So once per event-loop tick (100 ms by default) the server runs
a sampling cycle: pick keys at random, delete the ones past their deadline, and repeat
while more than 25% of the sample came back dead, up to a bounded number of rounds. A
mass expiry drains in a few ticks; a keyspace with nothing due costs one sample and
stops.

Sampling runs over a **separate index of the keys that have a TTL**, not over the whole
keyspace. The yield of a random sample is the fraction of sampled keys that are
volatile, so a cache of a thousand TTL'd keys sitting in front of a million permanent
ones -- an entirely normal shape -- would sample about 0.1% usefully and effectively
never collect anything. The cost of the index is one extra hash operation on the paths
that add or drop a TTL, and a duplicated key string per volatile key. Redis keeps a
dedicated `expires` dict for the same reason.

### Why not a timer per key

The obvious design is a priority queue of deadlines, or a timer per key, firing exactly
when a key dies. It is precise, and it is the wrong trade here:

- **Cost is paid per key rather than per tick.** Every `SET ... EX` becomes a heap
  insertion, every `PERSIST` and every overwrite becomes a heap removal or a tombstone,
  and the heap is as large as the volatile keyspace. Sampling's cost is a constant the
  operator chooses (`--expire-samples`), independent of how many keys have TTLs.
- **It buys precision nobody can observe.** Lazy expiry already guarantees that a dead
  key is never returned. Deleting it at deadline+0 ms instead of deadline+150 ms changes
  only when memory is released -- and `DBSIZE`, which is the one place the difference is
  visible, has exactly this fuzziness in Redis too, for exactly this reason.
- **A heap makes the wrong operation cheap.** "Which key dies next?" is a question
  nothing needs answered. "Are any of these keys dead?" is the actual question, and
  sampling answers it with bounded, tunable work.
- **Latency.** Ten million keys expiring in the same second is ten million heap pops on
  the thread that serves commands. The sampled cycle spreads the same work over as many
  ticks as it needs, and the cap on rounds bounds any single tick.

The trade accepted in return: memory for expired-but-unsampled keys is released late,
and `DBSIZE` may transiently overcount. Both converge within a few ticks.

---

## 11. The clock: why expiry uses wall time

`now_ms()` reads `std::chrono::system_clock` -- the wall clock -- and not
`steady_clock`. The textbook advice is the opposite, so the reasoning matters.

**An expiry deadline is not a duration.** The textbook rule ("never measure elapsed time
with the wall clock") is about measuring an interval between two points *inside* one
process. A deadline is a point in time that has to mean the same thing to two parties
that never compare clocks: the client that said `EXPIRE k 60`, and -- from Phase 2 on --
a file that outlives the process. `steady_clock`'s zero point is unspecified and
typically boot or process start, so a deadline recorded against it is meaningless after a
restart and *cannot be serialized at all*. Storing "expires 60 seconds after some moment
this process no longer remembers" is not a deadline.

**What breaks if the clock jumps backward.** If the system clock steps back by an hour --
NTP correcting a large drift, a VM restored from a snapshot, an operator setting the
date -- then every key with a deadline in that hour lives an hour longer than it should.
Keys expire *late*, by exactly the size of the jump. Symmetrically, a forward jump
expires them early, possibly all at once. Two things bound the damage. First, nothing is
corrupted: an expired key is a key that is gone, and both halves of the expiry machinery
compare a stored absolute deadline against the current time, so they simply agree with
each other about the new, wrong, time. Second, real clock steps are small, because NTP
*slews* small corrections (adjusting the clock's rate) rather than stepping them; the
scenarios above are unusual events, not steady-state behaviour. The failure mode is a
cache entry that lives too long -- which is what a cache TTL is allowed to do -- rather
than a wrong answer or a crash.

The alternative would be to store deadlines as monotonic timestamps and additionally
record the wall-clock time at process start, converting on the way in and out. That
recovers precision under clock steps and immediately loses it again across a restart,
where the conversion has no valid anchor. Redis makes the same trade, and it is a
deliberate one: correctness of *observation* (§10's lazy check) does not depend on the
clock being well-behaved, only the *timing* of collection does.

**Phase 2, persistence.** This is where the choice pays. A snapshot has to record when
each key dies, in a form the process that loads it can interpret. With absolute wall
deadlines that is one `int64` written verbatim, and loading is one comparison against
the current time: a key whose deadline passed while the server was down is simply not
loaded. With monotonic timestamps there is nothing meaningful to write. Redis's RDB
format stores absolute millisecond timestamps for precisely this reason, and Phase 2
will do the same.

---

## 12. Command dispatch

The command table maps a lowercased name to a handler and an arity, built once at first
use and read-only afterwards. Dispatch is a hash lookup, an arity check, and an indirect
call.

Arity follows Redis's convention: a positive number is the exact argument count
*including* the command name, a negative number is a minimum of its absolute value. One
integer covers `GET` (2) and `DEL` (-2), which lets the check live in the dispatcher
instead of at the top of every handler.

Handlers are grouped by subject -- connection, string, keyspace, server -- with one
registration function per group, so adding a group is a new file and one line rather
than an edit to a growing central list.

Reply encoding lives in `src/resp/writer.h` and appends to the connection's output
buffer. Integer formatting is hand-rolled rather than `std::to_string`, because every
`INCR` and every array header goes through it and `to_string` allocates a string to
throw away.

---

## 13. How this is tested

Three layers, because they catch different things:

- **Unit tests** (`tests/test_*.cpp`, run by CTest) for the buffer, both poller
  backends, the event loop, the parser, the reply encoders, the keyspace, and the
  command table. The parser tests are the most detailed: partial frames at every byte
  offset, pipelined frames, malformed input, huge bulk headers, binary payloads with
  embedded NULs and CRLFs. The harness is forty lines in `tests/test_util.h`; the
  no-third-party-dependency rule is worth more here than a framework would be.
- **Integration tests** (`tests/integration_test.py`) against a real process over real
  sockets, with a hand-written RESP client so the exact bytes -- including malformed
  ones no real client would send -- are under the test's control. Nineteen tests
  covering pipelining, byte-at-a-time delivery, 8 MiB values, fifty concurrent clients,
  protocol errors, the output buffer limit, fd exhaustion under `ulimit -n 64`, and
  clean shutdown on SIGTERM.
- **The real clients.** `redis-cli` connects and every implemented command behaves;
  `redis-benchmark` runs to completion. A protocol implementation that passes its own
  tests and not the reference client has tested its own misunderstanding.

`scripts/verify.sh` does a clean configure, a build with `-Wall -Wextra -Werror`, and
the full suite in one command. CI runs that same script on `ubuntu-latest` and
`macos-latest`, which is the only thing that verifies the epoll backend at all -- see
the note in [BENCHMARKS.md](BENCHMARKS.md) about what is and is not measured.

---

## 14. What Phase 1 deliberately leaves out

Named here so their absence reads as a decision rather than an oversight: persistence
(RDB/AOF), replication, RESP3 and `HELLO`, `CONFIG`, `INFO`, multiple databases and
`SELECT`, transactions, pub/sub, containers (lists, hashes, sets, sorted sets), key
eviction under a memory ceiling, IPv6 and Unix domain sockets, and authentication.

Two of those have hooks already in place, because retrofitting them would have changed
existing structure rather than added to it: `Value` carries a type tag so that `TYPE`
answers from stored state and a future `LPUSH` has an existing place to reject a string,
and deadlines are stored as absolute wall-clock milliseconds so a snapshot can write them
verbatim (§11).
