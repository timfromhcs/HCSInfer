"""Self-tests for tools/imgqa (stdlib unittest + numpy). Run: python -m unittest tools/imgqa/test_imgqa.py"""
import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare  # noqa: E402
import report  # noqa: E402


def write_png(path: Path, arr: np.ndarray, filt: int = 0) -> None:
    h, w, c = arr.shape
    rows = []
    prev = np.zeros(w * c, dtype=np.int32)
    for y in range(h):
        line = arr[y].reshape(-1).astype(np.int32)
        if filt == 0:
            enc = line
        elif filt == 1:
            left = np.concatenate([np.zeros(c, dtype=np.int32), line[:-c]])
            enc = (line - left) & 255
        elif filt == 2:
            enc = (line - prev) & 255
        elif filt == 4:  # Paeth
            enc = np.zeros_like(line)
            for i in range(len(line)):
                a = line[i - c] if i >= c else 0
                b = prev[i]
                cc = prev[i - c] if i >= c else 0
                p = a + b - cc
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - cc)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else cc)
                enc[i] = (line[i] - pred) & 255
        rows.append(bytes([filt]) + bytes(enc.astype(np.uint8)))
        prev = line

    def chunk(t: bytes, d: bytes) -> bytes:
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    ct = {1: 0, 3: 2, 4: 6}[c]
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


class MetricTests(unittest.TestCase):
    def setUp(self):
        rng = np.random.default_rng(1)
        self.a = rng.integers(0, 256, (32, 40, 3), dtype=np.uint8)

    def test_identical(self):
        r = compare.compare_arrays(self.a, self.a.copy())
        self.assertTrue(r["pixel_identical"])
        self.assertEqual(r["psnr_db"], float("inf"))
        self.assertAlmostEqual(r["ssim_luma"], 1.0, places=9)

    def test_known_psnr(self):
        b = np.clip(self.a.astype(int) + 1, 0, 255).astype(np.uint8)
        b[self.a == 255] = 255
        d = (b.astype(float) - self.a.astype(float)) ** 2
        want = 10 * np.log10(255.0**2 / d.mean())
        self.assertAlmostEqual(compare.psnr(self.a, b), want, places=9)

    def test_ssim_low_for_unrelated_image(self):
        other = np.random.default_rng(2).integers(0, 256, self.a.shape, dtype=np.uint8)
        self.assertLess(compare.ssim(self.a, other), 0.2)

    def test_ssim_high_for_small_perturbation(self):
        ramp = np.tile(np.linspace(0, 255, 64).astype(np.uint8)[None, :, None], (48, 1, 3))
        pert = np.clip(ramp.astype(int) + 2, 0, 255).astype(np.uint8)
        self.assertGreater(compare.ssim(ramp, pert), 0.99)

    def test_shape_mismatch(self):
        with self.assertRaises(ValueError):
            compare.compare_arrays(self.a, self.a[:, :-1])


class DecodeTests(unittest.TestCase):
    def test_all_filters_roundtrip(self):
        rng = np.random.default_rng(3)
        arr = rng.integers(0, 256, (12, 15, 3), dtype=np.uint8)
        with tempfile.TemporaryDirectory(dir="D:/HCSScratch/tmp") as d:
            for f in (0, 1, 2, 4):
                p = Path(d) / f"f{f}.png"
                write_png(p, arr, f)
                np.testing.assert_array_equal(compare.decode_png_numpy(p.read_bytes()), arr)


class ReportTests(unittest.TestCase):
    def test_report_counts_hashes_and_failures(self):
        rng = np.random.default_rng(4)
        base = rng.integers(0, 256, (32, 32, 3), dtype=np.uint8)
        other = base.copy()
        other[0, 0, 0] ^= 1
        with tempfile.TemporaryDirectory(dir="D:/HCSScratch/tmp") as d:
            d = Path(d)
            runs = []
            for name, be, rep, arr in [("a1", "cpu", 1, base), ("a2", "cpu", 2, base), ("b1", "vk", 1, other),
                                       ("b2", "vk", 2, base)]:
                write_png(d / f"{name}.png", arr)
                runs.append({"name": name, "backend": be, "params": "cpu", "rep": rep, "rc": 0, "wall_s": 1,
                             "sha256": compare.sha256_file(d / f"{name}.png")})
            runs.append({"name": "bad", "backend": "vk", "params": "disk", "rep": 1, "rc": 3, "wall_s": 1, "sha256": ""})
            (d / "runs.jsonl").write_text("\n".join(json.dumps(r) for r in runs), encoding="utf-8")
            rep = report.build(d, "cpu:cpu", False)
            self.assertEqual(rep["repeat"]["cpu:cpu"]["distinct_hashes"], 1)
            self.assertEqual(rep["repeat"]["vk:cpu"]["distinct_hashes"], 2)
            self.assertEqual(rep["repeat"]["vk:cpu"]["worst_pair"]["max_abs_diff"], 1)
            self.assertEqual([f["name"] for f in rep["failed_runs"]], ["bad"])
            self.assertFalse(rep["pairs"][0]["pixel_identical"])


if __name__ == "__main__":
    unittest.main()
