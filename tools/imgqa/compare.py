#!/usr/bin/env python3
"""Image equivalence metrics for HCSInfer image-generation QA.

Computes, for two images of equal size: byte identity (sha256), pixel identity,
max/mean absolute difference, fraction of differing pixels, PSNR (8-bit) and SSIM
(Gaussian 11x11, sigma 1.5, on BT.601 luma; Wang et al. 2004 constants).

Dependencies: numpy. Pillow is used for decoding when importable; otherwise a small
built-in decoder handles the non-interlaced 8-bit PNGs that stable-diffusion.cpp writes.
LPIPS is intentionally NOT implemented here (needs torch + weights); callers must record
it as NOT RUN rather than inventing a value.

CLI:  compare.py A.png B.png [--json]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

import numpy as np


# --------------------------------------------------------------------------- decoding
def decode_png_numpy(data: bytes) -> np.ndarray:
    """Decode an 8-bit, non-interlaced grey/RGB/RGBA PNG to HxWxC uint8."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, ihdr = 8, [], None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        ctype = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif ctype == b"IDAT":
            idat.append(body)
        elif ctype == b"IEND":
            break
    if ihdr is None:
        raise ValueError("missing IHDR")
    w, h, depth, ctype, _, _, interlace = ihdr
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(ctype)
    if depth != 8 or interlace != 0 or channels is None:
        raise ValueError("unsupported PNG variant (need 8-bit, non-interlaced, non-palette); install Pillow")
    raw = np.frombuffer(zlib.decompress(b"".join(idat)), dtype=np.uint8)
    stride = w * channels
    raw = raw.reshape(h, stride + 1)
    out = np.zeros((h, stride), dtype=np.uint8)
    prev = np.zeros(stride, dtype=np.uint8)
    for y in range(h):
        f, line = raw[y, 0], raw[y, 1:].copy()
        if f == 0:
            cur = line
        elif f == 2:
            cur = (line + prev).astype(np.uint8)
        elif f in (1, 3, 4):
            cur = line
            for i in range(stride):  # sequential dependency; fine at QA sizes
                left = cur[i - channels] if i >= channels else 0
                up = prev[i]
                ul = prev[i - channels] if i >= channels else 0
                if f == 1:
                    pred = left
                elif f == 3:
                    pred = (int(left) + int(up)) // 2
                else:
                    pa = abs(int(up) - int(ul))
                    pb = abs(int(left) - int(ul))
                    pc = abs(int(left) + int(up) - 2 * int(ul))
                    pred = left if (pa <= pb and pa <= pc) else (up if pb <= pc else ul)
                cur[i] = (int(cur[i]) + int(pred)) & 0xFF
        else:
            raise ValueError(f"bad PNG filter {f}")
        out[y] = cur
        prev = cur
    return out.reshape(h, w, channels)


def load_rgb(path: str | Path) -> np.ndarray:
    """Load an image as HxWx3 uint8."""
    p = Path(path)
    try:
        from PIL import Image  # type: ignore

        with Image.open(p) as im:
            return np.asarray(im.convert("RGB"), dtype=np.uint8)
    except ImportError:
        arr = decode_png_numpy(p.read_bytes())
        if arr.shape[2] == 1:
            arr = np.repeat(arr, 3, axis=2)
        elif arr.shape[2] == 2:
            arr = np.repeat(arr[:, :, :1], 3, axis=2)
        return arr[:, :, :3]


def sha256_file(path: str | Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# --------------------------------------------------------------------------- metrics
def psnr(a: np.ndarray, b: np.ndarray) -> float:
    """PSNR in dB for 8-bit data; +inf when identical."""
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    if mse == 0.0:
        return float("inf")
    return 10.0 * float(np.log10(255.0 * 255.0 / mse))


def _gauss_kernel(size: int = 11, sigma: float = 1.5) -> np.ndarray:
    x = np.arange(size, dtype=np.float64) - (size - 1) / 2.0
    k = np.exp(-(x**2) / (2 * sigma * sigma))
    return k / k.sum()


def _filter_valid(img: np.ndarray, k: np.ndarray) -> np.ndarray:
    """Separable 'valid' correlation of a 2-D array with 1-D kernel k on both axes."""
    n = len(k)
    out = sum(k[i] * img[:, i : img.shape[1] - n + 1 + i] for i in range(n))
    return sum(k[i] * out[i : out.shape[0] - n + 1 + i, :] for i in range(n))


def luma(rgb: np.ndarray) -> np.ndarray:
    f = rgb.astype(np.float64)
    return 0.299 * f[..., 0] + 0.587 * f[..., 1] + 0.114 * f[..., 2]


def ssim(a: np.ndarray, b: np.ndarray) -> float:
    """Mean SSIM over BT.601 luma, Gaussian window; identical images give exactly 1.0."""
    x, y = luma(a), luma(b)
    if min(x.shape) < 11:
        raise ValueError("image too small for SSIM (need >= 11 px per side)")
    k = _gauss_kernel()
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mx, my = _filter_valid(x, k), _filter_valid(y, k)
    sxx = _filter_valid(x * x, k) - mx * mx
    syy = _filter_valid(y * y, k) - my * my
    sxy = _filter_valid(x * y, k) - mx * my
    num = (2 * mx * my + c1) * (2 * sxy + c2)
    den = (mx * mx + my * my + c1) * (sxx + syy + c2)
    return float(np.mean(num / den))


def compare_arrays(a: np.ndarray, b: np.ndarray) -> dict:
    if a.shape != b.shape:
        raise ValueError(f"shape mismatch {a.shape} vs {b.shape}")
    d = np.abs(a.astype(np.int16) - b.astype(np.int16))
    return {
        "pixel_identical": bool(not d.any()),
        "max_abs_diff": int(d.max()),
        "mean_abs_diff": float(d.mean()),
        "frac_pixels_differing": float(np.mean(d.max(axis=2) > 0)),
        "frac_pixels_diff_gt8": float(np.mean(d.max(axis=2) > 8)),
        "psnr_db": psnr(a, b),
        "ssim_luma": ssim(a, b),
    }


def compare_files(pa: str | Path, pb: str | Path) -> dict:
    res = compare_arrays(load_rgb(pa), load_rgb(pb))
    res["byte_identical"] = sha256_file(pa) == sha256_file(pb)
    res["sha256_a"], res["sha256_b"] = sha256_file(pa), sha256_file(pb)
    return res


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)
    r = compare_files(args.a, args.b)
    if args.json:
        r = {k: (None if isinstance(v, float) and v == float("inf") else v) for k, v in r.items()}
        print(json.dumps(r, indent=2))
    else:
        for k, v in r.items():
            print(f"{k}: {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
