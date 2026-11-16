#!/usr/bin/env python3
"""[Phase 3] API 서버 기능 테스트

실제 서버 프로세스를 임시 데이터 디렉터리로 띄우고, HTTP 로 요청을 보내 검증한다.
    python3 tests/api_test.py      (먼저 make 로 build/mdb_server 를 빌드)
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER_BIN = os.path.join(ROOT, "build", "mdb_server")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, data_dir, threads=8, queue=64):
        self.port = free_port()
        self.log = open(os.path.join(data_dir, "server.log"), "w")
        self.proc = subprocess.Popen(
            [SERVER_BIN, "-d", data_dir, "-p", str(self.port), "-t", str(threads), "-q", str(queue)],
            stdout=self.log, stderr=self.log)
        deadline = time.time() + 5
        while True:
            try:
                if request(self.port, "GET", "/health")[0] == 200:
                    return
            except OSError:
                pass
            if time.time() > deadline or self.proc.poll() is not None:
                raise RuntimeError("server did not start")
            time.sleep(0.05)

    def stop(self):
        self.proc.send_signal(signal.SIGINT)
        rc = self.proc.wait(timeout=10)
        self.log.close()
        return rc


def request(port, method, path, body=None):
    data = body.encode() if isinstance(body, str) else body
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", "text/plain; charset=utf-8")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, json.loads(resp.read()), resp.headers
    except urllib.error.HTTPError as e:
        raw = e.read()
        return e.code, json.loads(raw) if raw else {}, e.headers


def query(port, sql):
    return request(port, "POST", "/query", sql)


def raw_http(port, payload, timeout=5):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall(payload)
        chunks = []
        while True:
            data = s.recv(4096)
            if not data:
                break
            chunks.append(data)
    return b"".join(chunks)


# ---------------------------------------------------------------- tests

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


@test
def health_and_stats(s):
    status, body, _ = request(s.port, "GET", "/health")
    assert status == 200 and body["ok"] is True
    status, body, _ = request(s.port, "GET", "/stats")
    assert status == 200 and body["threads"] == 8
    assert {t["name"] for t in body["tables"]} == {"users"}


@test
def insert_then_select(s):
    status, body, _ = query(s.port, "INSERT INTO users VALUES ('kim', 20), ('lee', 25);")
    assert status == 200, body
    r = body["results"][0]
    assert r["type"] == "insert" and r["inserted"] == 2 and r["first_id"] == 1 and r["last_id"] == 2

    status, body, _ = query(s.port, "SELECT * FROM users WHERE id = 2;")
    assert status == 200
    r = body["results"][0]
    assert r["columns"] == ["id", "name", "age"]
    assert r["rows"] == [[2, "lee", 25]]
    assert r["index_used"] is True and r["rows_examined"] == 1


@test
def multiple_statements_in_one_request(s):
    status, body, _ = query(s.port, "INSERT INTO users VALUES ('park', 30); SELECT name FROM users WHERE name = 'park';")
    assert status == 200 and body["statements"] == 2
    assert body["results"][1]["rows"] == [["park"]]
    assert body["results"][1]["index_used"] is False


@test
def unicode_and_special_characters(s):
    status, body, _ = query(s.port, "INSERT INTO users VALUES ('민수 \"MS\", O''Neil', 41);")
    assert status == 200
    new_id = body["results"][0]["first_id"]
    status, body, _ = query(s.port, f"SELECT name FROM users WHERE id = {new_id}")
    assert body["results"][0]["rows"] == [['민수 "MS", O\'Neil']]


@test
def sql_errors_return_400(s):
    status, body, _ = query(s.port, "SELECT * FROM nope;")
    assert status == 400 and body["ok"] is False
    assert "does not exist" in body["results"][0]["error"]

    status, body, _ = query(s.port, "SELEC * FROM users;")
    assert status == 400 and "expected INSERT or SELECT" in body["results"][0]["error"]

    # 일부만 실패: 성공한 문장의 결과도 함께 돌려준다
    status, body, _ = query(s.port, "SELECT id FROM users LIMIT 1; INSERT INTO users VALUES (1, 2);")
    assert status == 400 and body["errors"] == 1
    assert body["results"][0]["ok"] is True and body["results"][1]["ok"] is False


@test
def http_errors(s):
    assert request(s.port, "GET", "/nope")[0] == 404
    assert request(s.port, "GET", "/query")[0] == 405
    assert query(s.port, "")[0] == 400
    assert query(s.port, "  -- just a comment\n")[0] == 400
    assert raw_http(s.port, b"POST /query HTTP/1.1\r\nHost: x\r\n\r\n").startswith(b"HTTP/1.1 411")
    assert raw_http(s.port, b"garbage\r\n\r\n").startswith(b"HTTP/1.1 400")
    big = b"POST /query HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n"
    assert raw_http(s.port, big).startswith(b"HTTP/1.1 413")


@test
def concurrent_inserts_are_consistent(s):
    before = len(query(s.port, "SELECT id FROM users")[1]["results"][0]["rows"])
    n = 300

    def one(i):
        return query(s.port, f"INSERT INTO users (name, age) VALUES ('c_{i}', {i % 50});")

    with ThreadPoolExecutor(max_workers=32) as ex:
        results = list(ex.map(one, range(n)))
    assert all(st == 200 for st, _, _ in results), [b for st, b, _ in results if st != 200][:3]
    workers = {h.get("X-Worker") for _, _, h in results}
    assert len(workers) > 1, f"only one worker handled requests: {workers}"

    rows = query(s.port, "SELECT id, name FROM users")[1]["results"][0]["rows"]
    ids = [r[0] for r in rows]
    assert len(rows) == before + n
    assert ids == list(range(1, len(rows) + 1)), "ids must be unique and contiguous"
    assert {r[1] for r in rows if r[1].startswith("c_")} == {f"c_{i}" for i in range(n)}
    print(f"      {n} parallel inserts handled by {len(workers)} different workers")


@test
def concurrent_mixed_reads_and_writes(s):
    def one(i):
        if i % 3 == 0:
            return query(s.port, f"INSERT INTO users VALUES ('m_{i}', 1), ('m_{i}b', 2);")
        if i % 3 == 1:
            return query(s.port, f"SELECT * FROM users WHERE id = {i % 100 + 1};")
        return query(s.port, "SELECT id FROM users WHERE age = 7;")

    with ThreadPoolExecutor(max_workers=24) as ex:
        results = list(ex.map(one, range(240)))
    assert all(st == 200 for st, _, _ in results)
    rows = query(s.port, "SELECT id FROM users")[1]["results"][0]["rows"]
    assert [r[0] for r in rows] == list(range(1, len(rows) + 1))


def backpressure_returns_503(tmp):
    """워커 1개, 큐 1칸: 느린 연결 2개가 자리를 차지하면 세 번째는 즉시 503"""
    s = Server(tmp, threads=1, queue=1)
    try:
        a = socket.create_connection(("127.0.0.1", s.port))   # 워커가 이 연결을 붙잡고 기다림
        time.sleep(0.3)
        b = socket.create_connection(("127.0.0.1", s.port))   # 큐의 한 칸을 차지
        time.sleep(0.3)
        resp = raw_http(s.port, b"GET /health HTTP/1.1\r\n\r\n")
        assert resp.startswith(b"HTTP/1.1 503"), resp[:40]
        a.close()
        b.close()
        time.sleep(0.2)
        assert request(s.port, "GET", "/health")[0] == 200   # 자리가 나면 다시 정상
        stats = request(s.port, "GET", "/stats")[1]
        assert stats["rejected"] >= 1
    finally:
        assert s.stop() == 0


def main():
    if not os.path.exists(SERVER_BIN):
        print("build/mdb_server not found. run `make` first.")
        return 1
    tmp = tempfile.mkdtemp(prefix="mdb_api_")
    with open(os.path.join(tmp, "users.schema"), "w") as f:
        f.write("id INT\nname TEXT\nage INT\n")

    failures = 0

    def run(name, fn, *args):
        nonlocal failures
        try:
            fn(*args)
            print(f"  PASS {name}")
        except Exception as e:  # noqa: BLE001
            failures += 1
            print(f"  FAIL {name}: {e!r}")

    print("[api]")
    s = Server(tmp)
    try:
        for fn in TESTS:
            run(fn.__name__, fn, s)
        rows_before = len(query(s.port, "SELECT id FROM users")[1]["results"][0]["rows"])
    finally:
        rc = s.stop()

    run("graceful_shutdown_exit_code", lambda: (_ for _ in ()).throw(AssertionError(rc)) if rc != 0 else None)

    def persistence():
        s2 = Server(tmp)
        try:
            rows = query(s2.port, "SELECT id FROM users")[1]["results"][0]["rows"]
            assert len(rows) == rows_before, (len(rows), rows_before)
        finally:
            s2.stop()

    run("data_survives_restart", persistence)
    run("backpressure_returns_503", backpressure_returns_503, tmp)

    shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{len(TESTS) + 3 - failures}/{len(TESTS) + 3} API tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
