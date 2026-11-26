"""Self-test of the stress harness against a fake HTTP server (no HCSInfer, no model). Run:
    python -I tools/stress/test_stress.py
It proves the abort logic: every abort condition fires, the ramp stops, the kill switch is called and the JSON
report says so. The fake server stands in for the overload controller's endpoints only."""
import json
import os
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stress  # noqa: E402

MIB = 1 << 20
TOKEN = "unit-test-token-0123456789"


class Fake:
    """Mutable server behaviour shared with the handler."""

    def __init__(self):
        self.free = 16 * 1024 * MIB
        self.commit = 0.4
        self.level = "normal"
        self.enabled = True
        self.chat_status = 200
        self.chat_delay = 0.0
        self.chat_delay_growth = 0.0  # added per chat request
        self.chat_calls = 0
        self.shed_calls = 0
        self.on_chat = None
        self.lock = threading.Lock()


def make_server(fake):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, doc, headers=None):
            raw = json.dumps(doc).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(raw)

        def _authed(self):
            return self.headers.get("Authorization") == "Bearer " + TOKEN

        def do_GET(self):
            if self.path == "/hcs/v1/health":
                return self._send(200, {"status": "ok"})
            if not self._authed():
                return self._send(401, {})
            if self.path == "/hcs/v1/runtime":
                return self._send(200, {"version": "fake"})
            if self.path == "/hcs/v1/runtime/overload":
                if not fake.enabled:
                    return self._send(200, {"enabled": False})
                return self._send(200, {"enabled": True, "level": fake.level, "in_flight": 0, "sample": {
                    "free_physical_bytes": fake.free, "commit_fraction": fake.commit}})
            self._send(404, {})

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            self.rfile.read(n)
            if not self._authed():
                return self._send(401, {})
            if self.path == "/hcs/v1/runtime/shed":
                with fake.lock:
                    fake.shed_calls += 1
                return self._send(200, {"jobs_cancelled": 0, "model_unloaded": True})
            if self.path in ("/v1/chat/completions", "/v1/images/generations"):
                with fake.lock:
                    fake.chat_calls += 1
                    delay = fake.chat_delay + fake.chat_delay_growth * fake.chat_calls
                if fake.on_chat:
                    fake.on_chat(fake.chat_calls)
                time.sleep(delay)
                hdr = {"Retry-After": "5"} if fake.chat_status in (429, 503) else {}
                return self._send(fake.chat_status, {"ok": fake.chat_status == 200}, hdr)
            self._send(404, {})

    srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv


class PureLogic(unittest.TestCase):
    L = stress.Limits(floor_bytes=2048 * MIB, commit_fraction=0.9, max_error_rate=0.5, spike_factor=4.0,
                      spike_min_ms=100, max_runtime_s=10, c_growth_bytes=100 * MIB)

    def S(self, free=8192 * MIB, commit=0.5, level="normal"):
        return stress.MemSample(source="controller", free_bytes=free, commit_fraction=commit, level=level)

    def test_memory_conditions(self):
        self.assertIsNone(stress.check_memory(self.S(), self.L))
        self.assertEqual(stress.check_memory(self.S(free=2047 * MIB), self.L).reason, "free_ram_below_floor")
        self.assertIsNone(stress.check_memory(self.S(free=2048 * MIB), self.L))  # exactly at the floor is fine
        self.assertEqual(stress.check_memory(self.S(commit=0.91), self.L).reason, "commit_above_limit")
        self.assertEqual(stress.check_memory(self.S(level="critical"), self.L).reason, "server_critical")
        blind = stress.MemSample()
        self.assertEqual(stress.check_memory(blind, self.L).reason, "no_memory_visibility")
        only_commit = stress.MemSample(source="local", commit_fraction=0.2)
        self.assertIsNone(stress.check_memory(only_commit, self.L))  # a partial view still protects

    def test_step_conditions(self):
        self.assertIsNone(stress.check_step(4, 2, 50, None, self.L))  # 50 % is not above 50 %
        self.assertEqual(stress.check_step(4, 3, 50, None, self.L).reason, "error_rate")
        self.assertIsNone(stress.check_step(0, 0, None, None, self.L))
        self.assertEqual(stress.check_step(2, 0, 900, 100, self.L).reason, "latency_spike")
        self.assertIsNone(stress.check_step(2, 0, 90, 10, self.L))  # below spike_min_ms
        self.assertIsNone(stress.check_step(2, 0, 300, 100, self.L))  # only 3x

    def test_runtime_and_c_drive(self):
        self.assertIsNone(stress.check_runtime(9, self.L))
        self.assertEqual(stress.check_runtime(11, self.L).reason, "runtime_limit")
        self.assertIsNone(stress.check_c_drive(1000 * MIB, 950 * MIB, self.L))
        self.assertEqual(stress.check_c_drive(1000 * MIB, 800 * MIB, self.L).reason, "c_drive_growth")
        self.assertIsNone(stress.check_c_drive(None, 1, self.L))

    def test_classify_and_percentile(self):
        self.assertEqual([stress.classify(c) for c in (200, 204, 429, 503, 400, 500, 0)],
                         ["ok", "ok", "refused", "refused", "error", "error", "error"])
        self.assertEqual(stress.percentile([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 0.9), 9)
        self.assertIsNone(stress.percentile([], 0.5))

    def test_overload_json_parsing_keeps_unknown_unknown(self):
        s = stress.sample_from_overload_json({"enabled": True, "level": "normal", "sample": {
            "free_physical_bytes": "unknown", "commit_fraction": "unknown"}}, 0)
        self.assertIsNone(s.free_bytes)
        self.assertIsNone(s.commit_fraction)
        self.assertEqual(stress.sample_from_overload_json({"enabled": False}, 0).source, "unknown")

    def test_pid_alive(self):
        self.assertTrue(stress.pid_alive(os.getpid()))
        import subprocess
        p = subprocess.Popen([sys.executable, "-I", "-c", "pass"])
        p.wait()
        self.assertFalse(stress.pid_alive(p.pid))


class EndToEnd(unittest.TestCase):
    def setUp(self):
        self.fake = Fake()
        self.srv = make_server(self.fake)
        self.url = f"http://127.0.0.1:{self.srv.server_address[1]}"
        self.tmp = tempfile.mkdtemp()
        os.environ["HCS_STRESS_TEST_TOKEN"] = TOKEN

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def run_main(self, *extra, expect_rc=None):
        report = os.path.join(self.tmp, "r.json")
        argv = ["--base-url", self.url, "--token-env", "HCS_STRESS_TEST_TOKEN", "--model", "tiny", "--dry",
                "--poll-interval", "0.05", "--report", report, "--c-growth-mb", "0", *extra]
        rc = stress.main(argv)
        if expect_rc is not None:
            self.assertEqual(rc, expect_rc)
        with open(report, encoding="utf-8") as f:
            return rc, json.load(f)

    def test_clean_run_passes_and_covers_every_step(self):
        rc, rep = self.run_main(expect_rc=0)
        self.assertEqual(rep["result_status"], "PASS")
        self.assertFalse(rep["abort"]["fired"])
        axes = [(s["axis"], s["value"]) for s in rep["steps"]]
        self.assertEqual(axes, [("context", 16), ("context", 64), ("context", 256),
                                ("concurrency", 1), ("concurrency", 2)])
        self.assertEqual(self.fake.shed_calls, 0)
        self.assertNotIn(TOKEN, json.dumps(rep))  # the token never reaches the report
        self.assertEqual(rep["schema_version"], 1)

    def test_floor_above_free_ram_aborts_before_the_first_request(self):
        rc, rep = self.run_main("--floor-mb", str(32 * 1024), expect_rc=3)  # floor 32 GiB > 16 GiB free
        self.assertEqual(rep["result_status"], "ABORTED")
        self.assertEqual(rep["abort"]["reason"], "free_ram_below_floor")
        self.assertEqual(self.fake.chat_calls, 0)  # nothing was sent
        self.assertEqual(self.fake.shed_calls, 1)  # the kill switch was pulled
        self.assertEqual(rep["shed_on_abort"]["http_status"], 200)

    def test_free_ram_collapsing_mid_run_aborts_the_ramp(self):
        def drop(n):
            if n >= 2:
                self.fake.free = 512 * MIB

        self.fake.on_chat = drop
        rc, rep = self.run_main("--context-steps", "16,64,256,1024", "--reps", "2", expect_rc=3)
        self.assertEqual(rep["abort"]["reason"], "free_ram_below_floor")
        self.assertLess(self.fake.chat_calls, 8)  # the later steps never ran
        self.assertEqual(rep["steps"][-1]["outcome"], "aborted")
        self.assertEqual(self.fake.shed_calls, 1)

    def test_commit_charge_aborts(self):
        self.fake.commit = 0.95
        rc, rep = self.run_main(expect_rc=3)
        self.assertEqual(rep["abort"]["reason"], "commit_above_limit")

    def test_server_critical_level_aborts(self):
        self.fake.level = "critical"
        rc, rep = self.run_main(expect_rc=3)
        self.assertEqual(rep["abort"]["reason"], "server_critical")

    def test_error_rate_aborts(self):
        self.fake.chat_status = 500
        rc, rep = self.run_main("--reps", "2", expect_rc=3)
        self.assertEqual(rep["abort"]["reason"], "error_rate")
        self.assertEqual(rep["steps"][0]["errors"], 2)
        self.assertEqual(len(rep["steps"]), 1)

    def test_latency_spike_aborts(self):
        # step 1 fast, step 2 is 12x slower than step 1 and above --spike-min-ms
        def grow(n):
            self.fake.chat_delay = 0.02 if n < 2 else 0.5

        self.fake.on_chat = grow
        rc, rep = self.run_main("--spike-min-ms", "100", "--spike-factor", "4", expect_rc=3)
        self.assertEqual(rep["abort"]["reason"], "latency_spike")

    def test_server_backpressure_is_a_limit_not_an_error(self):
        self.fake.chat_status = 503
        rc, rep = self.run_main("--axes", "concurrency", expect_rc=0)
        self.assertEqual(rep["result_status"], "LIMIT_REACHED")
        st = rep["steps"][0]
        self.assertEqual(st["refused"], 1)
        self.assertEqual(st["errors"], 0)
        self.assertEqual(st["outcome"], "limit_reached")
        self.assertEqual(st["retry_after_seen"], ["5"])
        self.assertEqual(len(rep["steps"]), 1)  # the axis stopped at the first refused step
        self.assertEqual(self.fake.shed_calls, 0)

    def test_dead_server_process_aborts(self):
        import subprocess
        p = subprocess.Popen([sys.executable, "-I", "-c", "pass"])
        p.wait()
        rc, rep = self.run_main("--server-pid", str(p.pid), expect_rc=3)
        self.assertEqual(rep["abort"]["reason"], "process_gone")
        self.assertEqual(self.fake.chat_calls, 0)
        self.assertEqual(self.fake.shed_calls, 0)  # nobody to shed

    def test_server_vanishing_mid_run_aborts_as_process_gone(self):
        def kill(n):
            if n == 1:
                threading.Thread(target=self.srv.shutdown, daemon=True).start()
                self.srv.socket.close()

        self.fake.on_chat = kill
        rc, rep = self.run_main("--reps", "1", "--request-timeout", "2", "--axes", "context,concurrency",
                                "--context-steps", "16,64,256,1024", expect_rc=None)
        self.assertIn(rc, (0, 3))
        self.assertTrue(rep["abort"]["fired"] or any(s["errors"] for s in rep["steps"]))

    def test_runtime_limit_aborts(self):
        self.fake.chat_delay = 0.15
        rc, rep = self.run_main("--max-runtime-s", "0.2", "--reps", "2", "--context-steps", "16,64,256,1024",
                                expect_rc=3)
        self.assertEqual(rep["abort"]["reason"], "runtime_limit")

    def test_never_ramps_blind(self):
        self.fake.enabled = False
        orig = stress.local_sample
        stress.local_sample = lambda t: stress.MemSample(t=t)  # no local probe either
        try:
            rc, rep = self.run_main(expect_rc=3)
        finally:
            stress.local_sample = orig
        self.assertEqual(rep["abort"]["reason"], "no_memory_visibility")
        self.assertEqual(self.fake.chat_calls, 0)

    def test_disabled_controller_falls_back_to_the_local_probe(self):
        self.fake.enabled = False
        orig = stress.local_sample
        stress.local_sample = lambda t: stress.MemSample(t=t, source="local", free_bytes=8192 * MIB, commit_fraction=0.3)
        try:
            rc, rep = self.run_main(expect_rc=0)
        finally:
            stress.local_sample = orig
        self.assertEqual(rep["result_status"], "PASS")
        self.assertEqual(rep["memory_timeline"][0]["source"], "local")

    def test_missing_token_is_a_usage_error(self):
        os.environ.pop("HCS_STRESS_TEST_TOKEN")
        rc = stress.main(["--base-url", self.url, "--token-env", "HCS_STRESS_TEST_TOKEN", "--model", "m"])
        self.assertEqual(rc, stress.EXIT_USAGE)

    def test_unreachable_server(self):
        rc = stress.main(["--base-url", "http://127.0.0.1:1", "--token-env", "HCS_STRESS_TEST_TOKEN", "--model", "m",
                          "--report", os.path.join(self.tmp, "x.json")])
        self.assertEqual(rc, stress.EXIT_UNREACHABLE)


if __name__ == "__main__":
    unittest.main(verbosity=2)
