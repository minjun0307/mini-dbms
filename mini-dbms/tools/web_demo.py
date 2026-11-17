#!/usr/bin/env python3
"""mini-dbms 웹 데모 서버

    python3 tools/web_demo.py [--port 8080] [--backend-port 9090] [--data data] [--threads 8]

1) build/mdb_server (Phase 3 API 서버)를 --backend-port 로 띄운다.
2) web/ 폴더의 정적 페이지를 --port 로 제공한다.
3) 페이지가 보내는 /api/* 요청을 mdb_server 로 그대로 전달한다(프록시).
   C 서버는 JSON API 만 담당하고, 화면은 이 스크립트가 담당한다.
"""
import argparse
import http.server
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_DIR = os.path.join(ROOT, "web")
SERVER_BIN = os.path.join(ROOT, "build", "mdb_server")
BACKEND = "http://127.0.0.1:9090"


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB_DIR, **kwargs)

    def do_GET(self):
        if self.path in ("/api/health", "/api/stats"):
            return self.proxy("GET", self.path[len("/api"):], None)
        return super().do_GET()

    def do_POST(self):
        if self.path != "/api/query":
            return self.send_error(404)
        length = int(self.headers.get("Content-Length") or 0)
        return self.proxy("POST", "/query", self.rfile.read(length))

    def proxy(self, method, path, body):
        req = urllib.request.Request(BACKEND + path, data=body, method=method)
        if body is not None:
            req.add_header("Content-Type", "text/plain; charset=utf-8")
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                status, data, worker = r.status, r.read(), r.headers.get("X-Worker", "")
        except urllib.error.HTTPError as e:
            status, data, worker = e.code, e.read(), e.headers.get("X-Worker", "")
        except OSError as e:
            status, worker = 502, ""
            data = json.dumps({"ok": False, "error": f"mdb_server unavailable: {e}"}).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-Worker", worker)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):  # 정적 파일 로그는 생략 (C 서버 로그만 보이게)
        pass


def wait_for_backend(proc, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            with urllib.request.urlopen(BACKEND + "/health", timeout=1) as r:
                if r.status == 200:
                    return True
        except OSError:
            time.sleep(0.1)
    return False


def main():
    global BACKEND
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--backend-port", type=int, default=9090)
    ap.add_argument("--data", default=os.path.join(ROOT, "data"))
    ap.add_argument("--threads", type=int, default=8)
    args = ap.parse_args()
    BACKEND = f"http://127.0.0.1:{args.backend_port}"

    if not os.path.exists(SERVER_BIN):
        print("build/mdb_server not found. run `make build/mdb_server` first.", file=sys.stderr)
        return 1

    backend = subprocess.Popen([SERVER_BIN, "-d", args.data, "-p", str(args.backend_port),
                                "-t", str(args.threads)])
    if not wait_for_backend(backend):
        print("mdb_server did not start", file=sys.stderr)
        backend.kill()
        return 1

    def stop(*_):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"web demo: http://localhost:{args.port}  (backend {BACKEND})", file=sys.stderr, flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
        backend.send_signal(signal.SIGINT)  # graceful shutdown
        backend.wait(timeout=10)
    return 0


if __name__ == "__main__":
    sys.exit(main())
