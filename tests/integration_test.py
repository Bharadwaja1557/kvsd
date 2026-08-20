#!/usr/bin/env python3
"""End-to-end tests: a real kvsd process, real sockets, raw RESP bytes.

The C++ unit tests check each layer in isolation with the network mocked out. This
script checks the thing an operator actually runs -- including the behaviours that only
exist once a kernel is in the loop: partial reads, partial writes, pipelining, orderly
shutdown, and what happens when a client misbehaves.

usage: integration_test.py /path/to/kvsd
"""

import os
import re
import shlex
import socket
import subprocess
import sys
import threading
import time

BINARY = None
TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


class Error(str):
    """A RESP error reply. Distinct from a status reply so a test cannot confuse them."""


class Client:
    """A minimal RESP2 client. Deliberately not redis-py: the point is to control the
    exact bytes on the wire, including malformed ones a real client would never send."""

    def __init__(self, port, timeout=10.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    @staticmethod
    def encode(*args):
        out = b"*%d\r\n" % len(args)
        for a in args:
            if isinstance(a, str):
                a = a.encode()
            out += b"$%d\r\n%s\r\n" % (len(a), a)
        return out

    def send_raw(self, data):
        self.sock.sendall(data)

    def send(self, *args):
        self.send_raw(self.encode(*args))

    def cmd(self, *args):
        self.send(*args)
        return self.read_reply()

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError("server closed the connection")
        self.buf += chunk

    def _line(self):
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def _exact(self, n):
        while len(self.buf) < n:
            self._fill()
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def read_reply(self):
        line = self._line()
        kind, rest = line[:1], line[1:]
        if kind == b"+":
            return rest.decode()
        if kind == b"-":
            return Error(rest.decode())
        if kind == b":":
            return int(rest)
        if kind == b"$":
            n = int(rest)
            if n == -1:
                return None
            data = self._exact(n + 2)
            return data[:-2]
        if kind == b"*":
            n = int(rest)
            if n == -1:
                return None
            return [self.read_reply() for _ in range(n)]
        raise AssertionError("unknown reply type %r in %r" % (kind, line))

    def at_eof(self, timeout=5.0):
        """True if the server closed the connection (after draining whatever it sent)."""
        self.sock.settimeout(timeout)
        try:
            while True:
                if not self.sock.recv(65536):
                    return True
        except socket.timeout:
            return False
        except OSError:
            return True


class Server:
    """A kvsd child process bound to an ephemeral port.

    Port 0 plus the port scraped from the startup log, rather than a fixed port, so
    that a leftover process from an earlier run cannot make this run fail -- and so the
    tests can run in parallel with a real redis-server on the same machine.
    """

    def __init__(self, args=(), fd_limit=None):
        argv = [BINARY, "--port", "0", "--bind", "127.0.0.1", *args]
        if fd_limit is None:
            cmd = argv
        else:
            # ulimit in a shell rather than preexec_fn: same effect, no fork-safety
            # caveat in a script that runs reader threads.
            cmd = ["/bin/sh", "-c",
                   "ulimit -n %d; exec %s" % (fd_limit, " ".join(shlex.quote(a) for a in argv))]

        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.PIPE, text=True)
        self.log = []
        self._port = None
        self._ready = threading.Event()
        # Drained on a thread so a chatty server can never block on a full stderr pipe.
        self._reader = threading.Thread(target=self._drain, daemon=True)
        self._reader.start()
        if not self._ready.wait(10.0):
            self.stop()
            raise AssertionError("server did not report a listening port:\n" + "".join(self.log))

    def _drain(self):
        for line in self.proc.stderr:
            self.log.append(line)
            if self._port is None:
                m = re.search(r"listening on 127\.0\.0\.1:(\d+)", line)
                if m:
                    self._port = int(m.group(1))
                    self._ready.set()

    @property
    def port(self):
        return self._port

    def client(self, **kw):
        return Client(self.port, **kw)

    def alive(self):
        return self.proc.poll() is None

    def stop(self, expect_clean=True):
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            rc = self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise AssertionError("server ignored SIGTERM:\n" + "".join(self.log))
        if expect_clean:
            assert rc == 0, "server exited with %d:\n%s" % (rc, "".join(self.log))
        return rc

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


def eq(got, want, what=""):
    assert got == want, "%s got %r, want %r" % (what or "value", got, want)


# --------------------------------------------------------------------------------


@test
def test_basic_commands():
    with Server() as s, s.client() as c:
        eq(c.cmd("PING"), "PONG")
        eq(c.cmd("PING", "hello"), b"hello")
        eq(c.cmd("ECHO", "hi there"), b"hi there")
        eq(c.cmd("SET", "k", "v"), "OK")
        eq(c.cmd("GET", "k"), b"v")
        eq(c.cmd("TYPE", "k"), "string")
        eq(c.cmd("TYPE", "absent"), "none")
        eq(c.cmd("EXISTS", "k"), 1)
        eq(c.cmd("EXISTS", "k", "absent", "k"), 2)
        eq(c.cmd("DBSIZE"), 1)
        eq(c.cmd("DEL", "k", "absent"), 1)
        eq(c.cmd("GET", "k"), None)
        eq(c.cmd("DBSIZE"), 0)

        # Command names are case-insensitive; arguments are not.
        eq(c.cmd("sEt", "Case", "Value"), "OK")
        eq(c.cmd("get", "Case"), b"Value")
        eq(c.cmd("get", "case"), None)


@test
def test_counters():
    with Server() as s, s.client() as c:
        eq(c.cmd("INCR", "n"), 1)
        eq(c.cmd("INCR", "n"), 2)
        eq(c.cmd("DECR", "n"), 1)
        eq(c.cmd("GET", "n"), b"1")
        eq(c.cmd("SET", "n", "notanumber"), "OK")
        assert isinstance(c.cmd("INCR", "n"), Error)


@test
def test_expiry_options_and_ttl():
    with Server() as s, s.client() as c:
        eq(c.cmd("SET", "k", "v", "EX", "100"), "OK")
        eq(c.cmd("TTL", "k"), 100)
        eq(c.cmd("PERSIST", "k"), 1)
        eq(c.cmd("TTL", "k"), -1)
        eq(c.cmd("EXPIRE", "k", "50"), 1)
        eq(c.cmd("TTL", "k"), 50)
        eq(c.cmd("TTL", "absent"), -2)
        eq(c.cmd("EXPIRE", "absent", "50"), 0)

        eq(c.cmd("SET", "k", "v2", "NX"), None)
        eq(c.cmd("SET", "brand-new", "v", "XX"), None)
        eq(c.cmd("SET", "brand-new", "v", "NX"), "OK")
        assert isinstance(c.cmd("SET", "k", "v", "NX", "XX"), Error)
        assert isinstance(c.cmd("SET", "k", "v", "EX", "0"), Error)


@test
def test_lazy_expiry():
    with Server() as s, s.client() as c:
        eq(c.cmd("SET", "quick", "v", "PX", "60"), "OK")
        eq(c.cmd("GET", "quick"), b"v")
        time.sleep(0.25)
        # Nothing has swept it; the read path itself must refuse to return a dead key.
        eq(c.cmd("GET", "quick"), None)
        eq(c.cmd("TTL", "quick"), -2)


@test
def test_active_expiry_reclaims_untouched_keys():
    # The whole point of the active cycle: keys nobody ever reads again still go away.
    with Server(["--tick-interval", "20", "--expire-samples", "50"]) as s, s.client() as c:
        pipeline = b"".join(Client.encode("SET", "k%d" % i, "v", "PX", "50")
                            for i in range(500))
        c.send_raw(pipeline)
        for _ in range(500):
            eq(c.read_reply(), "OK")
        eq(c.cmd("SET", "permanent", "v"), "OK")

        deadline = time.time() + 10.0
        size = None
        while time.time() < deadline:
            size = c.cmd("DBSIZE")
            if size == 1:
                break
            time.sleep(0.05)
        eq(size, 1, "DBSIZE after active expiry")
        eq(c.cmd("GET", "permanent"), b"v")


@test
def test_pipelining():
    with Server() as s, s.client() as c:
        n = 1000
        pipeline = b"".join(Client.encode("SET", "key%d" % i, "val%d" % i) for i in range(n))
        pipeline += b"".join(Client.encode("GET", "key%d" % i) for i in range(n))
        c.send_raw(pipeline)
        for i in range(n):
            eq(c.read_reply(), "OK", "SET %d" % i)
        for i in range(n):
            eq(c.read_reply(), b"val%d" % i, "GET %d" % i)


@test
def test_command_split_across_packets():
    # One command dribbled a byte at a time: the parser must hold its state between
    # reads and produce exactly one reply, at the end.
    with Server() as s, s.client() as c:
        payload = Client.encode("SET", "split", "value")
        for i, byte in enumerate(payload):
            c.send_raw(bytes([byte]))
            if i % 3 == 0:
                time.sleep(0.002)
        eq(c.read_reply(), "OK")
        eq(c.cmd("GET", "split"), b"value")

        # A split that lands in the middle of a bulk payload, plus a second command in
        # the same final write -- framing must not bleed from one into the other.
        blob = b"x" * 5000
        payload = Client.encode("SET", "blob", blob)
        c.send_raw(payload[:37])
        time.sleep(0.02)
        c.send_raw(payload[37:] + Client.encode("PING"))
        eq(c.read_reply(), "OK")
        eq(c.read_reply(), "PONG")
        eq(c.cmd("GET", "blob"), blob)


@test
def test_inline_commands():
    with Server() as s, s.client() as c:
        c.send_raw(b"PING\r\n")
        eq(c.read_reply(), "PONG")
        # A bare LF, as telnet-style clients and nc send.
        c.send_raw(b"ECHO hello\n")
        eq(c.read_reply(), b"hello")
        c.send_raw(b'SET inline "a b c"\r\n')
        eq(c.read_reply(), "OK")
        eq(c.cmd("GET", "inline"), b"a b c")
        # A blank line is not a command and must not produce a reply.
        c.send_raw(b"\r\nPING\r\n")
        eq(c.read_reply(), "PONG")


@test
def test_binary_safety():
    with Server() as s, s.client() as c:
        value = bytes(range(256)) + b"\r\n*1\r\n$4\r\nPING\r\n"
        eq(c.cmd("SET", "bin", value), "OK")
        eq(c.cmd("GET", "bin"), value)
        # A key may be binary too, including an embedded NUL.
        key = b"key\x00with\x00nuls"
        eq(c.cmd("SET", key, "v"), "OK")
        eq(c.cmd("GET", key), b"v")
        eq(c.cmd("DBSIZE"), 2)


@test
def test_large_value_round_trip():
    # 8 MiB: larger than any socket buffer, so this exercises both a read that arrives
    # in many chunks and a reply that cannot be written in one send().
    with Server() as s, s.client() as c:
        blob = os.urandom(8 * 1024 * 1024)
        eq(c.cmd("SET", "huge", blob), "OK")
        got = c.cmd("GET", "huge")
        eq(len(got), len(blob), "huge value length")
        assert got == blob, "huge value round trip corrupted the payload"


@test
def test_protocol_errors_close_the_connection():
    cases = [
        (b"*1\r\n%bad\r\n", "expected '$'"),
        # A payload longer than its declared length: the bytes where the CRLF should
        # be are not a CRLF, so the framing is provably wrong. (A payload *shorter*
        # than its header is not an error -- it is a command still arriving.)
        (b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\nabcdef\r\n", "bulk length"),
        (b"*notanumber\r\n", "multibulk length"),
        (b'PING "unbalanced\r\n', "quotes"),
    ]
    with Server() as s:
        for payload, expected in cases:
            with s.client() as c:
                c.send_raw(payload)
                reply = c.read_reply()
                assert isinstance(reply, Error), "%r produced %r" % (payload, reply)
                assert "Protocol error" in reply, reply
                assert expected in reply, "%r: %r" % (payload, reply)
                # Framing is unrecoverable, so the server must hang up rather than
                # resynchronise at some guessed offset.
                assert c.at_eof(), "server left the connection open after %r" % payload
        # None of that may harm the server.
        with s.client() as c:
            eq(c.cmd("PING"), "PONG")


@test
def test_oversized_bulk_is_refused():
    # A four-byte header that asks for a gigabyte. The parser must refuse on the header
    # rather than start buffering.
    with Server() as s, s.client() as c:
        c.send_raw(b"*2\r\n$3\r\nGET\r\n$999999999999\r\n")
        reply = c.read_reply()
        assert isinstance(reply, Error) and "invalid bulk length" in reply, reply
        assert c.at_eof()


@test
def test_unknown_command_and_arity():
    with Server() as s, s.client() as c:
        reply = c.cmd("NOSUCHCOMMAND", "a")
        assert isinstance(reply, Error) and "unknown command" in reply, reply
        reply = c.cmd("GET")
        assert isinstance(reply, Error) and "wrong number of arguments" in reply, reply
        # Both are ordinary errors: the connection stays usable.
        eq(c.cmd("PING"), "PONG")


@test
def test_quit_and_flushall():
    with Server() as s:
        with s.client() as c:
            eq(c.cmd("SET", "a", "1"), "OK")
            eq(c.cmd("FLUSHALL"), "OK")
            eq(c.cmd("DBSIZE"), 0)
            # QUIT must deliver its +OK before the socket goes away.
            eq(c.cmd("QUIT"), "OK")
            assert c.at_eof(), "QUIT did not close the connection"


@test
def test_redis_cli_handshake():
    # redis-cli sends COMMAND DOCS before its first prompt and must not choke on the
    # stub reply.
    with Server() as s, s.client() as c:
        eq(c.cmd("COMMAND", "DOCS"), [])
        assert isinstance(c.cmd("COMMAND", "COUNT"), int)


@test
def test_many_concurrent_clients():
    with Server() as s:
        clients = [s.client() for _ in range(50)]
        try:
            # Interleaved, not one client at a time: every connection has state in
            # flight at the same moment.
            for i, c in enumerate(clients):
                c.send("SET", "client%d" % i, "value%d" % i)
            for i, c in enumerate(clients):
                eq(c.read_reply(), "OK", "client %d SET" % i)
            for i, c in enumerate(clients):
                c.send("GET", "client%d" % i)
            for i, c in enumerate(clients):
                eq(c.read_reply(), b"value%d" % i, "client %d GET" % i)
            eq(clients[0].cmd("DBSIZE"), 50)
        finally:
            for c in clients:
                c.close()


@test
def test_output_buffer_limit_disconnects_a_client_that_never_reads():
    limit = 256 * 1024
    with Server(["--max-output-buffer", str(limit)]) as s:
        with s.client() as c:
            eq(c.cmd("SET", "payload", b"x" * 100_000), "OK")
            # 500 replies of 100 KB, requested at once and never read. The socket
            # absorbs a little; the rest would be ~50 MB of server memory.
            c.send_raw(Client.encode("GET", "payload") * 500)
            assert c.at_eof(timeout=15), "server tolerated an unbounded output buffer"

        assert s.alive(), "server died instead of dropping the offending client"
        with s.client() as c:
            eq(c.cmd("PING"), "PONG")
        assert any("output buffer" in line for line in s.log), \
            "the disconnect was not logged:\n" + "".join(s.log)


@test
def test_survives_file_descriptor_exhaustion():
    # A low fd ceiling, then far more connections than it allows. The failure this
    # guards against is not a rejected client -- that is expected -- but the server
    # spinning on a listener it cannot drain, or dying.
    with Server(fd_limit=64) as s:
        early = s.client()
        eq(early.cmd("SET", "before", "value"), "OK")

        floods = []
        try:
            for _ in range(300):
                try:
                    floods.append(s.client(timeout=2.0))
                except OSError:
                    break  # the client side ran out of descriptors first; fine

            # The server is still there and still serving a connection it accepted
            # before the flood.
            eq(early.cmd("GET", "before"), b"value")
        finally:
            for c in floods:
                c.close()
            early.close()

        assert s.alive(), "server died under fd exhaustion:\n" + "".join(s.log)

        # Descriptors are free again, so the server must accept new work.
        deadline = time.time() + 10.0
        while True:
            try:
                with s.client(timeout=2.0) as c:
                    eq(c.cmd("PING"), "PONG")
                break
            except (OSError, EOFError):
                if time.time() > deadline:
                    raise AssertionError("server never recovered:\n" + "".join(s.log))
                time.sleep(0.1)

        assert any("file descriptors" in line for line in s.log), \
            "fd exhaustion was not logged:\n" + "".join(s.log)


@test
def test_sigterm_shuts_down_cleanly():
    s = Server()
    c = s.client()
    eq(c.cmd("PING"), "PONG")
    # Server.stop asserts the exit status is 0: an orderly shutdown, not a crash and
    # not a timeout waiting for the loop to notice.
    s.stop()
    c.close()


def main():
    global BINARY
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    BINARY = os.path.abspath(sys.argv[1])
    if not os.access(BINARY, os.X_OK):
        print("not executable: %s" % BINARY, file=sys.stderr)
        return 2

    failures = 0
    for fn in TESTS:
        name = fn.__name__
        started = time.time()
        try:
            fn()
        except Exception as exc:  # noqa: BLE001 - a test failure is any exception
            failures += 1
            print("FAIL %s: %s: %s" % (name, type(exc).__name__, exc))
        else:
            print("ok   %s (%.2fs)" % (name, time.time() - started))

    print("\n%d/%d integration tests passed" % (len(TESTS) - failures, len(TESTS)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
