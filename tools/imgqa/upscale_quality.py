"""Upscaler quality tools (numpy only; no Pillow/scikit-image/LPIPS needed).

Images are exchanged as ``.raw`` files (``"<w> <h> <stride>\\n"`` + BGR rows, as written by a tiny System.Drawing
helper) or as PNGs small enough for the pure-numpy decoder in compare.py. Subcommands:

  down      area-downscale an image by an integer factor (the "low-res input" of a super-resolution test)
  resize    baseline up/down-scaling (lanczos3 | bicubic | area), same filters as the C++ pipeline
  metrics   PSNR / SSIM / sharpness of a candidate against a reference of the same size
  seams     tiled-vs-whole-image error profile around tile boundaries (seam artefact metric)
  crop      zoomed crop to a PNG for looking at detail regions
  consist   downscale a big result back to the source size and compare with the source (fidelity)

LPIPS is NOT RUN: its weights are not installed locally and nothing is downloaded.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import load_rgb, psnr, ssim  # noqa: E402


def load(path: str) -> np.ndarray:
    """HxWx3 uint8 RGB from .raw (BGR rows with stride) or .png."""
    p = Path(path)
    if p.suffix.lower() == ".raw":
        data = p.read_bytes()
        nl = data.index(b"\n")
        w, h, stride = (int(x) for x in data[:nl].split())
        body = np.frombuffer(data, dtype=np.uint8, offset=nl + 1)
        rows = body[: stride * h].reshape(h, stride)[:, : w * 3].reshape(h, w, 3)
        return rows[:, :, ::-1].copy()  # BGR -> RGB
    return load_rgb(p)


def save_png(path: str, rgb: np.ndarray) -> None:
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(tag: bytes, body: bytes) -> bytes:
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    Path(path).write_bytes(png)


# ----------------------------------------------------------------------------------------------- resampling
def _lanczos3(x: np.ndarray) -> np.ndarray:
    x = np.abs(x)
    out = np.where(x < 1e-12, 1.0, 3.0 * np.sin(np.pi * x) * np.sin(np.pi * x / 3.0) / (np.pi * x) ** 2)
    return np.where(x >= 3.0, 0.0, out)


def _bicubic(x: np.ndarray, a: float = -0.5) -> np.ndarray:  # Keys / Catmull-Rom
    x = np.abs(x)
    return np.where(
        x <= 1, (a + 2) * x**3 - (a + 3) * x**2 + 1, np.where(x < 2, a * x**3 - 5 * a * x**2 + 8 * a * x - 4 * a, 0.0)
    )


def _area(x: np.ndarray) -> np.ndarray:
    return np.where(np.abs(x) < 0.5, 1.0, np.where(np.abs(x) == 0.5, 0.5, 0.0))


def _matrix(n_in: int, n_out: int, kind: str) -> np.ndarray:
    """Resampling matrix (n_out x n_in); filter support scales with the downsampling ratio."""
    scale = n_in / n_out
    fscale = max(1.0, scale)
    kern, support = {"lanczos3": (_lanczos3, 3.0), "bicubic": (_bicubic, 2.0), "area": (_area, 0.5)}[kind]
    centres = (np.arange(n_out) + 0.5) * scale - 0.5
    idx = np.arange(n_in)[None, :]
    w = kern((idx - centres[:, None]) / fscale)
    w[np.abs(idx - centres[:, None]) > support * fscale] = 0.0
    s = w.sum(axis=1, keepdims=True)
    return (w / np.where(s == 0, 1, s)).astype(np.float32)


def resize(rgb: np.ndarray, out_w: int, out_h: int, kind: str) -> np.ndarray:
    h, w, _ = rgb.shape
    f = rgb.astype(np.float32)
    f = np.einsum("oh,hwc->owc", _matrix(h, out_h, kind), f, optimize=True)
    f = np.einsum("pw,owc->opc", _matrix(w, out_w, kind), f, optimize=True)
    return np.clip(f + 0.5, 0, 255).astype(np.uint8)


# --------------------------------------------------------------------------------------------------- metrics
def sharpness(rgb: np.ndarray) -> float:
    """Variance of the Laplacian of luma (higher = more high-frequency energy)."""
    y = 0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] + 0.114 * rgb[..., 2]
    lap = -4 * y[1:-1, 1:-1] + y[:-2, 1:-1] + y[2:, 1:-1] + y[1:-1, :-2] + y[1:-1, 2:]
    return float(lap.var())


def overshoot(rgb: np.ndarray) -> float:
    """Fraction of pixels at 0 or 255 in any channel (clipping from over-sharpening/ringing)."""
    return float(np.mean(((rgb == 0) | (rgb == 255)).any(axis=2)))


def metrics(ref: np.ndarray, cand: np.ndarray) -> dict:
    return {
        "psnr_db": round(psnr(ref, cand), 3),
        "ssim_luma": round(ssim(ref, cand), 5),
        "sharpness_ref": round(sharpness(ref), 2),
        "sharpness_cand": round(sharpness(cand), 2),
        "clipped_frac_ref": round(overshoot(ref), 5),
        "clipped_frac_cand": round(overshoot(cand), 5),
    }


def seam_profile(tiled: np.ndarray, whole: np.ndarray, tile: int, overlap: int, scale: int) -> dict:
    """Absolute error of a tiled result against the single-pass result, bucketed by distance to the nearest
    interior tile boundary (in output pixels). A seam shows up as an error peak near distance 0."""
    h, w, _ = whole.shape
    err = np.abs(tiled.astype(np.int16) - whole.astype(np.int16)).max(axis=2).astype(np.float32)
    stride = (tile - overlap) * scale
    out: dict = {"tile": tile, "overlap": overlap, "scale": scale}
    cols = np.arange(w)
    # interior boundaries = where a new tile starts (x = k * stride) except 0; the overlap band is
    # [k*stride, k*stride + overlap*scale) and its centre is where the two tiles carry equal weight.
    starts = [k * stride for k in range(1, (w - 1) // stride + 1)]
    if not starts:
        out["note"] = "single tile in x: no seams"
        return out
    dist = np.min(np.abs(cols[:, None] - np.array(starts)[None, :] - (overlap * scale) // 2), axis=1)
    near = dist <= (overlap * scale) // 2 + 2
    out["mean_abs_err_near_seam"] = round(float(err[:, near].mean()), 4)
    out["mean_abs_err_elsewhere"] = round(float(err[:, ~near].mean()), 4)
    out["max_abs_err"] = int(err.max())
    out["psnr_vs_whole_db"] = round(psnr(tiled, whole), 3)
    out["frac_pixels_err_gt4"] = round(float((err > 4).mean()), 5)
    out["frac_pixels_err_gt16"] = round(float((err > 16).mean()), 5)
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("down")
    d.add_argument("--in", dest="inp", required=True)
    d.add_argument("--factor", type=int, default=4)
    d.add_argument("--out", required=True)
    r = sub.add_parser("resize")
    r.add_argument("--in", dest="inp", required=True)
    r.add_argument("--long-edge", type=int)
    r.add_argument("--size", type=int, nargs=2, metavar=("W", "H"))
    r.add_argument("--method", choices=["lanczos3", "bicubic", "area"], default="lanczos3")
    r.add_argument("--out", required=True)
    m = sub.add_parser("metrics")
    m.add_argument("--ref", required=True)
    m.add_argument("--cand", required=True)
    s = sub.add_parser("seams")
    s.add_argument("--tiled", required=True)
    s.add_argument("--whole", required=True)
    s.add_argument("--tile", type=int, required=True)
    s.add_argument("--overlap", type=int, required=True)
    s.add_argument("--scale", type=int, default=4)
    c = sub.add_parser("crop")
    c.add_argument("--in", dest="inp", required=True)
    c.add_argument("--x", type=int, required=True)
    c.add_argument("--y", type=int, required=True)
    c.add_argument("--w", type=int, required=True)
    c.add_argument("--h", type=int, required=True)
    c.add_argument("--zoom", type=int, default=1)
    c.add_argument("--out", required=True)
    k = sub.add_parser("consist")
    k.add_argument("--src", required=True)
    k.add_argument("--big", required=True)
    a = ap.parse_args(argv)

    if a.cmd == "down":
        img = load(a.inp)
        h, w, _ = img.shape
        save_png(a.out, resize(img, w // a.factor, h // a.factor, "area"))
    elif a.cmd == "resize":
        img = load(a.inp)
        h, w, _ = img.shape
        if a.size:
            ow, oh = a.size
        else:
            f = a.long_edge / max(w, h)
            ow, oh = (a.long_edge, round(h * f)) if w >= h else (round(w * f), a.long_edge)
        save_png(a.out, resize(img, ow, oh, a.method))
    elif a.cmd == "metrics":
        print(json.dumps(metrics(load(a.ref), load(a.cand)), indent=2))
    elif a.cmd == "seams":
        print(json.dumps(seam_profile(load(a.tiled), load(a.whole), a.tile, a.overlap, a.scale), indent=2))
    elif a.cmd == "crop":
        img = load(a.inp)[a.y : a.y + a.h, a.x : a.x + a.w]
        if a.zoom > 1:
            img = np.repeat(np.repeat(img, a.zoom, axis=0), a.zoom, axis=1)
        save_png(a.out, img)
    elif a.cmd == "consist":
        src, big = load(a.src), load(a.big)
        h, w, _ = src.shape
        res = metrics(src, resize(big, w, h, "area"))
        res["note"] = "area-downscale of the big result vs the source"
        print(json.dumps(res, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
