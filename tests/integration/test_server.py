#!/usr/bin/env python3
"""Black-box tests for the hws binary over real sockets.

Run: python3 -I tests/integration/test_server.py --binary build/hws [-v] [-k pattern]
Only the standard library is used.
"""
import argparse
import hashlib
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

BINARY = None
SANITIZER_RE = re.compile(r"ERROR: (Address|Thread|Leak|Memory)Sanitizer|WARNING: ThreadSanitizer|runtime error:")


class Response:
    def __init__(self, status, reason, headers, body):
        self.status = status
        self.reason = reason
        self.headers = headers  # lower-case name -> value (last wins)
        self.body = body

    def header(self, name):
        return self.headers.get(name.lower())


class Conn:
    """A raw HTTP/1.1 client connection with a buffered response reader."""

    def __init__(self, port, timeout=5.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.buf = b""

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.sock.sendall(data)

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError("connection closed")
        self.buf += chunk

    def read_response(self, head=False):
        while b"\r\n\r\n" not in self.buf:
            self._fill()
        head_bytes, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head_bytes.decode("latin-1").split("\r\n")
        m = re.match(r"HTTP/1\.1 (\d{3}) ?(.*)", lines[0])
        assert m, "bad status line: %r" % lines[0]
        status = int(m.group(1))
        headers = {}
        for line in lines[1:]:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
        body = b""
        if not head and status >= 200 and status not in (204, 304):
            n = int(headers.get("content-length", "0"))
            while len(self.buf) < n:
                self._fill()
            body, self.buf = self.buf[:n], self.buf[n:]
        return Response(status, m.group(2), headers, body)

    def closed_by_peer(self, wait=3.0):
        """True if the server closes the connection (EOF or RST) within `wait` seconds."""
        self.sock.settimeout(wait)
        try:
            while True:
                data = self.sock.recv(65536)
                if not data:
                    self.sock.close()
                    return True
                self.buf += data
        except (ConnectionResetError, BrokenPipeError):
            self.sock.close()
            return True
        except socket.timeout:
            return False

    def close(self):
        self.sock.close()


class Server:
    def __init__(self, *args, root=None):
        self.root = root
        cmd = [BINARY, "--listen", "127.0.0.1:0", "--workers", "2", "--log-level", "info"]
        if root:
            cmd += ["--root", root]
        cmd += list(args)
        self.proc = subprocess.Popen(cmd, stderr=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.log = []
        self.port = None
        deadline = time.time() + 10
        while time.time() < deadline:
            line = self.proc.stderr.readline()
            if not line:
                break
            self.log.append(line)
            m = re.search(r"listening on 127\.0\.0\.1:(\d+)", line)
            if m:
                self.port = int(m.group(1))
                break
        if self.port is None:
            self.proc.kill()
            raise RuntimeError("server did not start:\n" + "".join(self.log))
        self._drain = threading.Thread(target=self._pump, daemon=True)
        self._drain.start()

    def _pump(self):
        for line in self.proc.stderr:
            self.log.append(line)

    def conn(self, timeout=5.0):
        return Conn(self.port, timeout)

    def get(self, path, headers=None, method="GET"):
        c = self.conn()
        try:
            extra = "".join("%s: %s\r\n" % kv for kv in (headers or {}).items())
            c.send("%s %s HTTP/1.1\r\nHost: test\r\n%s\r\n" % (method, path, extra))
            return c.read_response(head=(method == "HEAD"))
        finally:
            c.close()

    def metric(self, name, labels=""):
        body = self.get("/metrics").body.decode()
        key = name + ("{%s}" % labels if labels else "")
        for line in body.splitlines():
            if line.startswith(key + " "):
                return float(line.split()[-1])
        raise KeyError(key)

    def stop(self, sig=signal.SIGTERM, timeout=15):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
            try:
                self.proc.wait(timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.close_pipes()
        return self.proc.returncode

    def close_pipes(self):
        self._drain.join(2)
        for p in (self.proc.stdout, self.proc.stderr):
            if p and not p.closed:
                p.close()
        # A sanitizer report from the server fails the test even if every response looked right.
        report = [l for l in self.log if SANITIZER_RE.search(l)]
        if report:
            raise AssertionError("sanitizer report from server:\n" + "".join(self.log[-60:]))


def make_root():
    root = tempfile.mkdtemp(prefix="hws-it-")
    with open(os.path.join(root, "index.html"), "w") as f:
        f.write("<h1>home</h1>\n")
    with open(os.path.join(root, "hello.txt"), "w") as f:
        f.write("hello world\n")
    os.makedirs(os.path.join(root, "docs"))
    with open(os.path.join(root, "docs", "index.html"), "w") as f:
        f.write("docs index\n")
    os.makedirs(os.path.join(root, "empty"))
    with open(os.path.join(root, ".env"), "w") as f:
        f.write("SECRET=1\n")
    with open(os.path.join(root, "big.bin"), "wb") as f:
        f.write(os.urandom(8 * 1024 * 1024))  # larger than the 4 MiB max socket send buffer
    os.symlink("/etc/passwd", os.path.join(root, "escape"))
    os.symlink("hello.txt", os.path.join(root, "alias.txt"))
    return root


class StaticFiles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = make_root()
        cls.srv = Server("--hello-path", "/hello", root=cls.root)

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        shutil.rmtree(cls.root)

    def test_file_and_headers(self):
        r = self.srv.get("/hello.txt")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.body, b"hello world\n")
        self.assertEqual(r.header("content-type"), "text/plain; charset=utf-8")
        self.assertEqual(r.header("accept-ranges"), "bytes")
        self.assertRegex(r.header("etag"), r'^"[0-9a-f]+-[0-9a-f]+"$')
        self.assertTrue(r.header("last-modified").endswith(" GMT"))
        self.assertEqual(r.header("server"), "hws")
        self.assertTrue(r.header("date"))

    def test_head(self):
        r = self.srv.get("/hello.txt", method="HEAD")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.header("content-length"), "12")
        self.assertEqual(r.body, b"")

    def test_root_index(self):
        r = self.srv.get("/")
        self.assertEqual((r.status, r.body), (200, b"<h1>home</h1>\n"))
        self.assertEqual(r.header("content-type"), "text/html; charset=utf-8")

    def test_directory_redirect_and_index(self):
        r = self.srv.get("/docs?x=1")
        self.assertEqual(r.status, 301)
        self.assertEqual(r.header("location"), "/docs/?x=1")
        self.assertEqual(self.srv.get("/docs/").body, b"docs index\n")
        self.assertEqual(self.srv.get("/empty/").status, 403)

    def test_not_found_and_method(self):
        self.assertEqual(self.srv.get("/nope.txt").status, 404)
        self.assertEqual(self.srv.get("/hello.txt/").status, 404)
        r = self.srv.get("/hello.txt", method="DELETE")
        self.assertEqual(r.status, 405)
        self.assertEqual(r.header("allow"), "GET, HEAD")

    def test_conditional(self):
        etag = self.srv.get("/hello.txt").header("etag")
        lm = self.srv.get("/hello.txt").header("last-modified")
        r = self.srv.get("/hello.txt", {"If-None-Match": etag})
        self.assertEqual(r.status, 304)
        self.assertEqual(r.body, b"")
        self.assertEqual(r.header("etag"), etag)
        self.assertEqual(self.srv.get("/hello.txt", {"If-None-Match": 'W/' + etag}).status, 304)
        self.assertEqual(self.srv.get("/hello.txt", {"If-None-Match": '"other"'}).status, 200)
        self.assertEqual(self.srv.get("/hello.txt", {"If-Modified-Since": lm}).status, 304)
        self.assertEqual(
            self.srv.get("/hello.txt", {"If-Modified-Since": "Thu, 01 Jan 1970 00:00:00 GMT"}).status, 200)
        # If-None-Match wins over If-Modified-Since
        self.assertEqual(self.srv.get("/hello.txt", {"If-None-Match": '"x"', "If-Modified-Since": lm}).status, 200)

    def test_ranges(self):
        r = self.srv.get("/hello.txt", {"Range": "bytes=0-4"})
        self.assertEqual((r.status, r.body), (206, b"hello"))
        self.assertEqual(r.header("content-range"), "bytes 0-4/12")
        r = self.srv.get("/hello.txt", {"Range": "bytes=-6"})
        self.assertEqual(r.body, b"world\n")
        r = self.srv.get("/hello.txt", {"Range": "bytes=100-"})
        self.assertEqual(r.status, 416)
        self.assertEqual(r.header("content-range"), "bytes */12")
        self.assertEqual(self.srv.get("/hello.txt", {"Range": "bytes=0-1,3-4"}).status, 200)
        etag = self.srv.get("/hello.txt").header("etag")
        self.assertEqual(self.srv.get("/hello.txt", {"Range": "bytes=0-1", "If-Range": etag}).status, 206)
        self.assertEqual(self.srv.get("/hello.txt", {"Range": "bytes=0-1", "If-Range": '"stale"'}).status, 200)
        # sendfile path with a range in the middle of a large file
        with open(os.path.join(self.root, "big.bin"), "rb") as f:
            data = f.read()
        r = self.srv.get("/big.bin", {"Range": "bytes=1000000-1999999"})
        self.assertEqual(r.status, 206)
        self.assertEqual(r.body, data[1000000:2000000])

    def test_large_file_sendfile(self):
        with open(os.path.join(self.root, "big.bin"), "rb") as f:
            want = hashlib.sha256(f.read()).hexdigest()
        r = self.srv.get("/big.bin")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.header("content-type"), "application/octet-stream")
        self.assertEqual(hashlib.sha256(r.body).hexdigest(), want)

    def test_traversal_and_hidden_files(self):
        for path in ["/../etc/passwd", "/%2e%2e/%2e%2e/etc/passwd", "/docs/../../etc/passwd",
                     "/..%2f..%2fetc%2fpasswd", "/a%00b"]:
            self.assertEqual(self.srv.get(path).status, 400, path)
        self.assertEqual(self.srv.get("/.env").status, 404)
        self.assertEqual(self.srv.get("/docs/../.env").status, 404)
        self.assertEqual(self.srv.get("/escape").status, 404)       # symlink out of root: refused by openat2
        self.assertEqual(self.srv.get("/alias.txt").body, b"hello world\n")  # symlink inside root is fine

    def test_file_change_is_picked_up(self):
        path = os.path.join(self.root, "changing.txt")
        with open(path, "w") as f:
            f.write("v1")
        for _ in range(4):  # warm both workers' caches
            self.assertEqual(self.srv.get("/changing.txt").body, b"v1")
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            f.write("version2")
        os.rename(tmp, path)  # atomic deploy
        # Each worker revalidates its own cache entry once --file-cache-ttl (2s) has passed.
        time.sleep(2.5)
        for _ in range(10):
            self.assertEqual(self.srv.get("/changing.txt").body, b"version2")
        os.unlink(path)
        time.sleep(2.5)
        for _ in range(6):
            self.assertEqual(self.srv.get("/changing.txt").status, 404)


class Protocol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = make_root()
        cls.srv = Server("--hello-path", "/hello", "--echo-path", "/echo", "--max-body-bytes", "64k",
                         "--max-header-bytes", "4k", "--max-uri-bytes", "1k", root=cls.root)

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        shutil.rmtree(cls.root)

    def test_keepalive_reuses_connection(self):
        c = self.srv.conn()
        for _ in range(50):
            c.send("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n")
            r = c.read_response()
            self.assertEqual((r.status, r.body), (200, b"Hello, World!"))
            self.assertIsNone(r.header("connection"))
        c.close()

    def test_pipelining_preserves_order(self):
        c = self.srv.conn()
        paths = ["/hello", "/hello.txt", "/nope", "/hello", "/docs/"] * 20
        c.send("".join("GET %s HTTP/1.1\r\nHost: t\r\n\r\n" % p for p in paths))
        statuses = [c.read_response().status for _ in paths]
        self.assertEqual(statuses, [200, 200, 404, 200, 200] * 20)
        c.close()

    def test_pipelined_byte_by_byte(self):
        c = self.srv.conn()
        data = b"GET /hello HTTP/1.1\r\nHost: t\r\n\r\n" * 3
        for i in range(len(data)):
            c.send(data[i:i + 1])
        for _ in range(3):
            self.assertEqual(c.read_response().status, 200)
        c.close()

    def test_connection_close(self):
        c = self.srv.conn()
        c.send("GET /hello HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.header("connection"), "close")
        self.assertTrue(c.closed_by_peer())

    def test_http10(self):
        c = self.srv.conn()
        c.send("GET /hello HTTP/1.0\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.status, 200)
        self.assertEqual(r.header("connection"), "close")
        self.assertTrue(c.closed_by_peer())
        c = self.srv.conn()
        c.send("GET /hello HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
        self.assertEqual(c.read_response().header("connection"), "keep-alive")
        c.send("GET /hello HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")
        self.assertEqual(c.read_response().status, 200)
        c.close()

    def test_post_content_length(self):
        c = self.srv.conn()
        body = os.urandom(40000)
        c.send(b"POST /echo HTTP/1.1\r\nHost: t\r\nContent-Type: application/x-test\r\nContent-Length: %d\r\n\r\n"
               % len(body))
        c.send(body[:1000])
        time.sleep(0.05)
        c.send(body[1000:])
        r = c.read_response()
        self.assertEqual((r.status, r.body), (200, body))
        self.assertEqual(r.header("content-type"), "application/x-test")
        # connection stays usable after a body that spanned reads
        c.send("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n")
        self.assertEqual(c.read_response().status, 200)
        c.close()

    def test_post_chunked(self):
        c = self.srv.conn()
        payload = b"".join(b"%x;ext=1\r\n%s\r\n" % (len(p), p) for p in [b"hello", b" ", b"chunked" * 100])
        msg = b"POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n" + payload + b"0\r\nX-T: 1\r\n\r\n"
        for i in range(0, len(msg), 7):  # dribble it in
            c.send(msg[i:i + 7])
        r = c.read_response()
        self.assertEqual(r.body, b"hello chunked" + b"chunked" * 99)
        c.close()

    def test_expect_continue(self):
        c = self.srv.conn()
        c.send("POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n")
        interim = c.read_response()
        self.assertEqual(interim.status, 100)
        c.send("abcd")
        r = c.read_response()
        self.assertEqual((r.status, r.body), (200, b"abcd"))
        c.close()

    def test_limits(self):
        c = self.srv.conn()
        c.send("POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 70000\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.status, 413)
        self.assertEqual(r.header("connection"), "close")
        self.assertTrue(c.closed_by_peer())
        self.assertEqual(self.srv.get("/" + "a" * 2000).status, 414)
        self.assertEqual(self.srv.get("/hello", {"X-Big": "b" * 5000}).status, 431)
        c = self.srv.conn()
        c.send(b"POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n" +
               (b"8000\r\n" + b"x" * 0x8000 + b"\r\n") * 3)
        self.assertEqual(c.read_response().status, 413)
        c.close()

    def test_errors(self):
        self.assertEqual(self.srv.get("/hello", method="PROPFIND").status, 501)
        self.assertEqual(self.srv.get("/hello", method="TRACE").status, 501)
        r = self.srv.get("/echo")
        self.assertEqual(r.status, 405)
        self.assertEqual(r.header("allow"), "POST, PUT")
        for raw in ["GET / HTTP/2.0\r\nHost: t\r\n\r\n", "GET / HTTP/1.1\r\n\r\n",
                    "GET / HTTP/1.1\r\nHost: t\r\nBad Header: x\r\n\r\n", "GARBAGE\r\n\r\n"]:
            c = self.srv.conn()
            c.send(raw)
            r = c.read_response()
            self.assertIn(r.status, (400, 505), raw)
            self.assertEqual(r.header("connection"), "close")
            self.assertTrue(c.closed_by_peer(), raw)

    def test_smuggling_attempts_close_the_connection(self):
        attacks = [
            "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 6\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\nG",
            "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\nContent-Length: 40\r\n\r\nabcd",
            "POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked, identity\r\n\r\n0\r\n\r\n",
            "POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding : chunked\r\n\r\n0\r\n\r\n",
            "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n Transfer-Encoding: chunked\r\n\r\nabcd",
        ]
        for raw in attacks:
            c = self.srv.conn()
            c.send(raw + "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n")
            r = c.read_response()
            self.assertEqual(r.status, 400, raw)
            # The smuggled second request must never be answered.
            self.assertTrue(c.closed_by_peer(), raw)
            self.assertNotIn(b"HTTP/1.1 200", c.buf, raw)

    def test_options_and_absolute_form(self):
        c = self.srv.conn()
        c.send("OPTIONS * HTTP/1.1\r\nHost: t\r\n\r\n")
        r = c.read_response()
        self.assertEqual(r.status, 204)
        self.assertIn("GET", r.header("allow"))
        c.send("GET http://example.com/hello HTTP/1.1\r\nHost: other\r\n\r\n")
        self.assertEqual(c.read_response().body, b"Hello, World!")
        c.close()

    def test_health_and_metrics(self):
        r = self.srv.get("/healthz")
        self.assertEqual((r.status, r.body), (200, b"ok\n"))
        body = self.srv.get("/metrics").body.decode()
        for name in ["hws_connections_active", "hws_responses_total{code=\"2xx\"}",
                     "hws_request_duration_seconds_bucket{le=\"+Inf\"}", "hws_worker_connections_active{worker=\"1\"}",
                     "hws_process_resident_memory_bytes", "hws_parse_errors_total"]:
            self.assertIn(name, body)
        self.assertGreater(self.srv.metric("hws_responses_total", 'code="2xx"'), 0)

    def test_many_concurrent_connections(self):
        conns = [self.srv.conn() for _ in range(500)]
        for c in conns:
            c.send("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n")
        for c in conns:
            self.assertEqual(c.read_response().status, 200)
            c.close()


class Timeouts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = make_root()
        cls.srv = Server("--hello-path", "/hello", "--echo-path", "/echo", "--header-timeout", "1s",
                         "--keepalive-timeout", "1s", "--body-timeout", "1s", "--write-timeout", "1s",
                         root=cls.root)

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        shutil.rmtree(cls.root)

    def test_slowloris_header_timeout(self):
        c = self.srv.conn()
        c.send("GET /hello HTTP/1.1\r\nHost: t\r\n")
        start = time.time()
        for _ in range(10):  # keep dribbling bytes: the deadline is absolute, not per byte
            time.sleep(0.2)
            try:
                c.send("X-Slow: 1\r\n")
            except OSError:
                break
        r = c.read_response()
        self.assertEqual(r.status, 408)
        self.assertLess(time.time() - start, 2.5)
        self.assertTrue(c.closed_by_peer())

    def test_silent_connection_closed(self):
        c = self.srv.conn()
        self.assertTrue(c.closed_by_peer(wait=3))
        self.assertEqual(c.buf, b"")  # closed without a response

    def test_idle_keepalive_timeout(self):
        c = self.srv.conn()
        c.send("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n")
        self.assertEqual(c.read_response().status, 200)
        self.assertTrue(c.closed_by_peer(wait=3))
        c.close()

    def test_body_timeout(self):
        c = self.srv.conn()
        c.send("POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 100\r\n\r\nonly-a-little")
        r = c.read_response()
        self.assertEqual(r.status, 408)
        c.close()

    def test_write_timeout_for_client_that_never_reads(self):
        before = self.srv.metric("hws_timeouts_total", 'kind="write"')
        s = socket.create_connection(("127.0.0.1", self.srv.port))
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: t\r\n\r\n")
        deadline = time.time() + 6
        while time.time() < deadline:
            if self.srv.metric("hws_timeouts_total", 'kind="write"') > before:
                break
            time.sleep(0.2)
        self.assertGreater(self.srv.metric("hws_timeouts_total", 'kind="write"'), before)
        s.close()


class Lifecycle(unittest.TestCase):
    def setUp(self):
        self.root = make_root()

    def tearDown(self):
        shutil.rmtree(self.root)

    def test_graceful_drain_finishes_inflight_download(self):
        srv = Server("--drain-timeout", "20s", root=self.root)
        with open(os.path.join(self.root, "big.bin"), "rb") as f:
            want = f.read()
        s = socket.create_connection(("127.0.0.1", srv.port))
        # Above the ~64 KiB loopback MSS (smaller windows stall on TCP persist probes), and small
        # enough that 8 MiB cannot fit in kernel buffers: the response is still in flight below.
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 262144)
        s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: t\r\n\r\n")
        got = s.recv(1024)  # response started; stop reading so it stays in flight
        idle = srv.conn()   # idle keep-alive connection: closed immediately by the drain
        time.sleep(0.2)
        srv.proc.send_signal(signal.SIGTERM)
        self.assertTrue(idle.closed_by_peer(wait=2))
        time.sleep(0.3)
        with self.assertRaises(OSError):  # listeners are closed
            probe = socket.create_connection(("127.0.0.1", srv.port), timeout=1)
            probe.close()
        self.assertIsNone(srv.proc.poll())  # still running: the download is not finished
        s.settimeout(10)
        while True:  # now drain the response as fast as possible
            chunk = s.recv(1 << 20)
            if not chunk:
                break
            got += chunk
        s.close()
        head, body = got.split(b"\r\n\r\n", 1)
        self.assertIn(b"Content-Length: %d" % len(want), head)
        self.assertEqual(len(body), len(want))
        self.assertEqual(body, want)
        # The connection closes once the response completes, long before the 20s deadline.
        self.assertEqual(srv.proc.wait(5), 0)
        srv.close_pipes()
        self.assertTrue(any("shutdown complete" in l for l in srv.log), "".join(srv.log))

    def test_drain_deadline_forces_exit(self):
        srv = Server("--drain-timeout", "500ms", root=self.root)
        s = socket.create_connection(("127.0.0.1", srv.port))
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: t\r\n\r\n")
        time.sleep(0.2)
        start = time.time()
        srv.proc.send_signal(signal.SIGTERM)
        self.assertEqual(srv.proc.wait(10), 0)
        self.assertLess(time.time() - start, 3)
        s.close()
        srv.close_pipes()

    def test_second_signal_stops_immediately(self):
        srv = Server("--drain-timeout", "60s", root=self.root)
        s = socket.create_connection(("127.0.0.1", srv.port))
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        s.sendall(b"GET /big.bin HTTP/1.1\r\nHost: t\r\n\r\n")
        time.sleep(0.2)
        srv.proc.send_signal(signal.SIGTERM)
        time.sleep(0.3)
        srv.proc.send_signal(signal.SIGINT)
        self.assertEqual(srv.proc.wait(5), 0)
        s.close()
        srv.close_pipes()

    def test_max_connections_answers_503(self):
        srv = Server("--workers", "1", "--max-connections", "2", "--hello-path", "/hello", root=self.root)
        try:
            a, b = srv.conn(), srv.conn()
            time.sleep(0.2)
            c = srv.conn()
            r = c.read_response()
            self.assertEqual(r.status, 503)
            self.assertEqual(r.header("retry-after"), "1")
            a.send("GET /hello HTTP/1.1\r\nHost: t\r\n\r\n")
            self.assertEqual(a.read_response().status, 200)
            for x in (b, c):
                x.close()
            time.sleep(0.2)  # free a slot so the metrics request itself is admitted
            self.assertGreaterEqual(srv.metric("hws_connections_rejected_total", 'reason="max_connections"'), 1)
            a.close()
        finally:
            srv.stop()

    def test_access_log_and_reopen(self):
        log = os.path.join(self.root, "access.log")
        srv = Server("--access-log", log, "--hello-path", "/hello", root=self.root)
        try:
            c = srv.conn()
            c.send('GET /hello?q="x" HTTP/1.1\r\nHost: t\r\nUser-Agent: it/1.0\r\nReferer: http://r/\r\n\r\n')
            c.read_response()
            c.close()
            time.sleep(1.5)  # buffered: flushed at least once per second
            with open(log) as f:
                line = f.read().strip().splitlines()[-1]
            self.assertRegex(line, r'^127\.0\.0\.1 - - \[\d\d/\w{3}/\d{4}:\d\d:\d\d:\d\d \+0000\] '
                                   r'"GET /hello\?q=\\x22x\\x22 HTTP/1\.1" 200 13 "http://r/" "it/1\.0" \d+\.\d{6}$')
            os.rename(log, log + ".1")
            srv.proc.send_signal(signal.SIGHUP)
            time.sleep(0.3)
            srv.get("/hello")
            time.sleep(1.5)
            with open(log) as f:
                self.assertIn('"GET /hello HTTP/1.1" 200', f.read())
        finally:
            srv.stop()

    def test_bad_config_exits_nonzero(self):
        r = subprocess.run([BINARY, "--workers", "x"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)
        self.assertIn("invalid value", r.stderr)
        r = subprocess.run([BINARY, "--listen", "127.0.0.1:0", "--root", "/nonexistent-dir"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)


def main():
    global BINARY
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    args, rest = ap.parse_known_args()
    BINARY = os.path.abspath(args.binary)
    unittest.main(argv=[sys.argv[0]] + rest)


if __name__ == "__main__":
    main()
