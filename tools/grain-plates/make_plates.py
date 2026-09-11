# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
"""Offline synthesis of the film-grain plates (FilmGrainSpec.md 3.3, stage G2 offline half).

Produces `<class>.dds` -- a DX10 Texture2DArray, DXGI_FORMAT_R8G8B8A8_UNORM, 8 slices,
512x512, full mip chain -- plus a `<class>.json` sidecar with everything the shader and the
verifier need. No engine code, no engine dependency: this writes assets, nothing else.

Model: the Boolean grain model of Newson, Delon & Galerne, "A Stochastic Film Grain Model for
Resolution-Independent Rendering" (CGF 2017) / IPOL "Realistic Film Grain Rendering" (2017),
run offline at true micrometre scale (PROPOSALS.md P1 source 1, P3 option (iii)).

Per layer: grain centres are a periodic Poisson process on the torus with intensity

    lambda = -ln(1 - u) / (pi * E[r^2])          u = coverage = 0.5

so the expected coverage is exactly u; radii are log-normal with mean r_bar and standard
deviation sigma_r = 0.25 * r_bar. The plate value is the area fraction of the texel covered by
the union of discs, estimated by 4x4 supersampling. Everything is periodic in both axes, so the
plate tiles seamlessly with no edge blending (blending would change the spectrum).

Usage
-----
    uv run make_plates.py --out out
    uv run --with matplotlib make_plates.py --out out --classes medium --preview
    uv run make_plates.py --verify out/medium.dds

See README.md.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
import time
import zlib
from pathlib import Path

import numpy as np

TOOL_VERSION = "1.0.0"

# ---------------------------------------------------------------------------
# Model constants (FilmGrainSpec.md 3.3)
# ---------------------------------------------------------------------------

# Size classes: mean developed-grain radius on the negative, in micrometres.
SIZE_CLASSES: dict[str, float] = {
    "fine": 3.0,
    "medium": 6.0,
    "coarse": 12.0,
    "xcoarse": 24.0,
}

# Per-layer mean radius as a ratio of the class r_bar, and the channel each layer lands in.
# Channel order is the DDS byte order R, G, B, A.
LAYERS: list[tuple[str, float]] = [
    ("R", 1.00),
    ("G", 0.80),
    ("B", 1.25),
    ("A", 1.00),   # fourth, independent monochrome field -- see README "Decisions"
]

COVERAGE = 0.5           # u, expected area fraction covered by grains
SIGMA_R_FRAC = 0.25      # sigma_r = SIGMA_R_FRAC * r_bar
TEXEL_PER_RBAR = 0.5     # plate texel size = r_bar / 2
PLATE_PX = 512
N_SLICES = 8
SUPERSAMPLE = 8         # 8x8 per texel; see README "Decisions" for the convergence measurement

# byte = 127.5 + QUANT_SCALE * n, with n zero-mean unit-variance.
# 127.5 / 32 = 3.98 sigma of headroom; the Boolean field never gets near that (see README).
QUANT_SCALE = 32.0

# ---------------------------------------------------------------------------
# DDS (DX10 header) writer / reader -- implemented here, no external DDS library
# ---------------------------------------------------------------------------

DDS_MAGIC = 0x20534444                     # the four bytes D D S space
DDSD_CAPS = 0x1
DDSD_HEIGHT = 0x2
DDSD_WIDTH = 0x4
DDSD_PITCH = 0x8
DDSD_PIXELFORMAT = 0x1000
DDSD_MIPMAPCOUNT = 0x20000
DDPF_FOURCC = 0x4
DDSCAPS_COMPLEX = 0x8
DDSCAPS_MIPMAP = 0x400000
DDSCAPS_TEXTURE = 0x1000
FOURCC_DX10 = 0x30315844                   # the four bytes D X 1 0
DXGI_FORMAT_R8G8B8A8_UNORM = 28
D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3


def dds_bytes(mip_chains: list[list[np.ndarray]]) -> bytes:
    """Serialise a Texture2DArray. mip_chains[slice][level] is (h, w, 4) uint8.

    DDS array layout is slice-major: every mip of slice 0, then every mip of slice 1, ...
    """
    n_slices = len(mip_chains)
    n_mips = len(mip_chains[0])
    h, w = mip_chains[0][0].shape[:2]

    flags = (DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH
             | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT)
    caps = DDSCAPS_TEXTURE | DDSCAPS_COMPLEX | DDSCAPS_MIPMAP

    out = bytearray()
    out += struct.pack("<I", DDS_MAGIC)
    out += struct.pack(
        "<7I",
        124,            # dwSize
        flags,
        h,
        w,
        w * 4,          # pitch of the top level
        0,              # depth
        n_mips,
    )
    out += b"\0" * (11 * 4)                                                  # dwReserved1[11]
    out += struct.pack("<8I", 32, DDPF_FOURCC, FOURCC_DX10, 0, 0, 0, 0, 0)   # DDS_PIXELFORMAT
    out += struct.pack("<5I", caps, 0, 0, 0, 0)                              # caps1..4 + reserved2
    out += struct.pack(
        "<5I",
        DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D10_RESOURCE_DIMENSION_TEXTURE2D,
        0,              # miscFlag (not a cube map)
        n_slices,       # arraySize
        0,              # miscFlags2 (DDS_ALPHA_MODE_UNKNOWN)
    )
    for chain in mip_chains:
        for level in chain:
            out += level.tobytes()
    return bytes(out)


def dds_read(path: Path) -> tuple[dict, list[list[np.ndarray]]]:
    """Inverse of dds_bytes. Returns (header dict, mip_chains)."""
    raw = path.read_bytes()
    if len(raw) < 148:
        raise ValueError("file too short to be a DX10 DDS")
    (magic,) = struct.unpack_from("<I", raw, 0)
    if magic != DDS_MAGIC:
        raise ValueError("not a DDS file (bad magic)")
    size, flags, height, width, pitch, depth, n_mips = struct.unpack_from("<7I", raw, 4)
    if size != 124:
        raise ValueError("bad DDS_HEADER size " + str(size))
    pf = struct.unpack_from("<8I", raw, 4 + 7 * 4 + 11 * 4)
    if pf[0] != 32 or not (pf[1] & DDPF_FOURCC) or pf[2] != FOURCC_DX10:
        raise ValueError("not a DX10-header DDS")
    dxgi, dim, misc, array_size, misc2 = struct.unpack_from("<5I", raw, 128)
    if dxgi != DXGI_FORMAT_R8G8B8A8_UNORM:
        raise ValueError("unexpected DXGI format " + str(dxgi) + ", want 28 (R8G8B8A8_UNORM)")
    if dim != D3D10_RESOURCE_DIMENSION_TEXTURE2D:
        raise ValueError("unexpected resource dimension " + str(dim))

    off = 148
    chains: list[list[np.ndarray]] = []
    for _ in range(array_size):
        chain = []
        for lvl in range(n_mips):
            lw = max(1, width >> lvl)
            lh = max(1, height >> lvl)
            n = lw * lh * 4
            if off + n > len(raw):
                raise ValueError("file truncated before the end of the mip chain")
            chain.append(np.frombuffer(raw, np.uint8, n, off).reshape(lh, lw, 4))
            off += n
        chains.append(chain)
    header = dict(width=width, height=height, mip_count=n_mips, array_size=array_size,
                  dxgi_format=dxgi, pitch=pitch, depth=depth, misc_flag=misc,
                  misc_flags2=misc2, trailing_bytes=len(raw) - off)
    return header, chains


# ---------------------------------------------------------------------------
# Minimal PNG writer (stdlib zlib only -- Pillow is not a dependency)
# ---------------------------------------------------------------------------

def write_png(path: Path, rgb: np.ndarray) -> None:
    """rgb: (h, w, 3) uint8."""
    h, w = rgb.shape[:2]
    raw = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


# ---------------------------------------------------------------------------
# Boolean grain model
# ---------------------------------------------------------------------------

def layer_stats(mean_r_tex: float) -> dict:
    """Poisson intensity and log-normal parameters for one layer, in texel units."""
    s = SIGMA_R_FRAC * mean_r_tex
    e_r2 = mean_r_tex * mean_r_tex + s * s              # E[r^2] = mean^2 + var
    lam = -math.log(1.0 - COVERAGE) / (math.pi * e_r2)  # grains per texel^2
    sig2 = math.log(1.0 + (s / mean_r_tex) ** 2)
    mu = math.log(mean_r_tex) - 0.5 * sig2
    return dict(mean_r_texels=mean_r_tex, sigma_r_texels=s, e_r2=e_r2,
                lambda_per_texel2=lam, lognormal_mu=mu, lognormal_sigma=math.sqrt(sig2))


def rasterise_layer(rng: np.random.Generator, size: int, ss: int,
                    mean_r_tex: float) -> tuple[np.ndarray, int]:
    """One Boolean layer on the torus.

    Returns (coverage in [0,1] as a (size, size) float64 array, grain count).
    Each disc is rasterised inside its own bounding box only; the box indices are taken
    modulo the supersampled plate size, which is what makes the plate periodic.
    """
    st = layer_stats(mean_r_tex)
    n_grains = int(rng.poisson(st["lambda_per_texel2"] * size * size))
    r = rng.lognormal(st["lognormal_mu"], st["lognormal_sigma"], n_grains) * ss
    S = size * ss
    cx = rng.random(n_grains) * S
    cy = rng.random(n_grains) * S

    cov = np.zeros((S, S), dtype=bool)
    for k in range(n_grains):
        rk = float(r[k])
        x = float(cx[k])
        y = float(cy[k])
        yy = np.arange(math.floor(y - rk), math.ceil(y + rk) + 1)
        xx = np.arange(math.floor(x - rk), math.ceil(x + rk) + 1)
        dy = (yy + 0.5) - y
        dx = (xx + 0.5) - x
        mask = (dy[:, None] ** 2 + dx[None, :] ** 2) <= rk * rk
        sel = np.ix_(yy % S, xx % S)          # periodic wrap in both axes
        cov[sel] = cov[sel] | mask

    # Area coverage per texel = mean of the ss x ss block of binary samples.
    return cov.reshape(size, ss, size, ss).mean(axis=(1, 3)), n_grains


# ---------------------------------------------------------------------------
# Spectrum
# ---------------------------------------------------------------------------

def radial_spectrum(field: np.ndarray) -> np.ndarray:
    """Radially averaged power spectrum. Index = spatial frequency in cycles per plate."""
    n = field.shape[0]
    f = np.fft.fft2(field - field.mean())
    p = (np.abs(f) ** 2) / (n * n)
    fy = np.fft.fftfreq(n)[:, None]
    fx = np.fft.fftfreq(n)[None, :]
    rad = np.sqrt(fx * fx + fy * fy) * n
    nb = n // 2
    b = np.clip(np.rint(rad).astype(np.int64), 0, nb)
    acc = np.bincount(b.ravel(), p.ravel(), minlength=nb + 1)
    cnt = np.bincount(b.ravel(), minlength=nb + 1)
    return acc / np.maximum(cnt, 1)


def shape_white_by_sqrt_s(rng: np.random.Generator, s_radial: np.ndarray, n: int) -> np.ndarray:
    """Reshape a white Gaussian field so its power spectrum matches s_radial.

    Filtering multiplies the POWER spectrum by |H(f)|^2, so to obtain the power S(f) from a
    white field of unit power the filter MAGNITUDE must be sqrt(S(f)), not S(f).
    (PROPOSALS.md section 8, item 3.)
    """
    fy = np.fft.fftfreq(n)[:, None]
    fx = np.fft.fftfreq(n)[None, :]
    rad = np.sqrt(fx * fx + fy * fy) * n
    b = np.clip(np.rint(rad).astype(np.int64), 0, len(s_radial) - 1)
    h = np.sqrt(np.maximum(s_radial[b], 0.0))
    h[0, 0] = 0.0                              # zero mean by construction
    white = rng.standard_normal((n, n))
    return np.fft.ifft2(np.fft.fft2(white) * h).real


# ---------------------------------------------------------------------------
# Plate assembly
# ---------------------------------------------------------------------------

def mip_chain_float(top: np.ndarray) -> list[np.ndarray]:
    """Full mip chain of a periodic field by 2x2 box averaging.

    Every level is a power of two, so the 2x2 boxes tile the plate exactly and the periodic
    wrap needs no special case: the reshape below IS the wrapped box filter, and no texel of
    any level ever reads outside the plate. This averaging is the footprint integration the
    shader relies on (spec 3.1 / R3.2): the variance of level k is the variance of the
    coverage of a texel 2^k times larger.
    """
    levels = [top]
    cur = top
    while cur.shape[0] > 1:
        h, w = cur.shape[:2]
        c = cur.shape[2]
        cur = cur.reshape(h // 2, 2, w // 2, 2, c).mean(axis=(1, 3))
        levels.append(cur)
    return levels


def quantise(field: np.ndarray, scale: float) -> np.ndarray:
    return np.clip(np.rint(127.5 + scale * field), 0, 255).astype(np.uint8)


def build_class(name: str, seed: int, plate_px: int, n_slices: int, ss: int,
                mode: str, log=print) -> tuple[bytes, dict, dict]:
    r_bar_um = SIZE_CLASSES[name]
    texel_um = TEXEL_PER_RBAR * r_bar_um
    class_idx = list(SIZE_CLASSES).index(name)

    layer_meta = []
    for li, (ch, ratio) in enumerate(LAYERS):
        mean_r_tex = ratio * r_bar_um / texel_um
        st = layer_stats(mean_r_tex)
        layer_meta.append(dict(
            index=li, channel=ch, r_bar_ratio=ratio,
            r_bar_um=ratio * r_bar_um, sigma_r_um=SIGMA_R_FRAC * ratio * r_bar_um,
            **{k: float(v) for k, v in st.items()},
            expected_grains_per_slice=float(st["lambda_per_texel2"] * plate_px * plate_px),
        ))

    # --- raw Boolean plates -------------------------------------------------
    raw = np.empty((n_slices, plate_px, plate_px, len(LAYERS)), dtype=np.float64)
    grain_counts = []
    for s in range(n_slices):
        counts = []
        for li, meta in enumerate(layer_meta):
            rng = np.random.default_rng([seed, class_idx, s, li])
            cov, n_g = rasterise_layer(rng, plate_px, ss, meta["mean_r_texels"])
            raw[s, :, :, li] = cov
            counts.append(n_g)
        grain_counts.append(counts)
        cov_meas = [round(float(raw[s, :, :, i].mean()), 4) for i in range(len(LAYERS))]
        log("    slice %d: grains per layer %s, coverage %s" % (s, counts, cov_meas))

    # Radial spectrum of the raw Boolean plate, measured before normalisation
    # (normalisation is a scalar, so it only scales S(f)).
    spectra = np.stack([radial_spectrum(raw[0, :, :, li]) for li in range(len(LAYERS))])

    # --- optional target-spectrum reshaping ---------------------------------
    if mode == "target-spectrum":
        for s in range(n_slices):
            for li in range(len(LAYERS)):
                sr = radial_spectrum(raw[s, :, :, li])
                rng = np.random.default_rng([seed, class_idx, s, li, 0xF17E])
                raw[s, :, :, li] = shape_white_by_sqrt_s(rng, sr, plate_px)

    # --- normalise: zero mean, unit variance, per layer per slice -----------
    raw_mean = raw.mean(axis=(1, 2), keepdims=True)
    raw_std = raw.std(axis=(1, 2), keepdims=True)
    norm = (raw - raw_mean) / raw_std

    peak = float(np.abs(norm).max())
    clipped = int((np.abs(norm) * QUANT_SCALE > 127.5).sum())

    # --- mips (from the float, periodic field) and quantisation -------------
    mip_chains: list[list[np.ndarray]] = []
    slice_meta = []
    for s in range(n_slices):
        levels = mip_chain_float(norm[s])
        chain = [quantise(lv, QUANT_SCALE) for lv in levels]
        mip_chains.append(chain)
        mips = []
        for lvl, q in enumerate(chain):
            deq = (q.astype(np.float64) - 127.5) / QUANT_SCALE
            mips.append(dict(
                level=lvl, size=int(q.shape[0]),
                mean=[round(float(x), 6) for x in q.reshape(-1, len(LAYERS)).mean(axis=0)],
                variance=[round(float(x), 8) for x in deq.reshape(-1, len(LAYERS)).var(axis=0)],
            ))
        slice_meta.append(dict(index=s, grain_counts=grain_counts[s], mips=mips))

    blob = dds_bytes(mip_chains)
    sidecar = dict(
        tool="make_plates.py", tool_version=TOOL_VERSION, mode=mode,
        size_class=name, r_bar_um=r_bar_um, texel_um=texel_um,
        plate_px=plate_px, plate_extent_mm=plate_px * texel_um / 1000.0,
        slices=n_slices, supersample=ss, coverage=COVERAGE, sigma_r_frac=SIGMA_R_FRAC,
        seed=seed,
        format=dict(dxgi_format="DXGI_FORMAT_R8G8B8A8_UNORM", dxgi_format_id=28,
                    resource="Texture2DArray", mip_count=len(mip_chains[0])),
        dds_sha256=hashlib.sha256(blob).hexdigest(), dds_bytes=len(blob),
        normalisation=dict(
            encoding="byte = 127.5 + scale * n", scale=QUANT_SCALE,
            decode="n = (byte - 127.5) / scale",
            peak_abs_sigma=round(peak, 4), clipped_texels=clipped),
        layers=layer_meta,
        slice_stats=slice_meta,
    )
    spec_meta = dict(radial_frequency_cycles_per_plate=list(range(spectra.shape[1])),
                     power=[[float(v) for v in row] for row in spectra])
    return blob, sidecar, spec_meta


# ---------------------------------------------------------------------------
# Verify
# ---------------------------------------------------------------------------

def verify(dds_path: Path) -> int:
    side = dds_path.with_suffix(".json")
    problems: list[str] = []
    if not side.exists():
        print("FAIL %s: sidecar %s missing" % (dds_path.name, side.name))
        return 1
    meta = json.loads(side.read_text(encoding="utf-8"))
    header, chains = dds_read(dds_path)

    # Bit-exact integrity first: the statistical checks below are deliberately tolerant and
    # would not notice a handful of flipped bytes.
    digest = hashlib.sha256(dds_path.read_bytes()).hexdigest()
    if digest != meta["dds_sha256"]:
        problems.append("sha256 %s... != sidecar %s..." % (digest[:16], meta["dds_sha256"][:16]))
    if dds_path.stat().st_size != meta["dds_bytes"]:
        problems.append("size %d != sidecar %d" % (dds_path.stat().st_size, meta["dds_bytes"]))

    exp_mips = meta["format"]["mip_count"]
    checks = [
        ("width", header["width"], meta["plate_px"]),
        ("height", header["height"], meta["plate_px"]),
        ("array_size", header["array_size"], meta["slices"]),
        ("mip_count", header["mip_count"], exp_mips),
        ("dxgi_format", header["dxgi_format"], meta["format"]["dxgi_format_id"]),
        ("trailing_bytes", header["trailing_bytes"], 0),
    ]
    for label, got, want in checks:
        if got != want:
            problems.append("%s: got %s, want %s" % (label, got, want))

    scale = meta["normalisation"]["scale"]
    n_ch = len(meta["layers"])
    for s, chain in enumerate(chains):
        smeta = meta["slice_stats"][s]
        if len(chain) != exp_mips:
            problems.append("slice %d: %d mips, want %d" % (s, len(chain), exp_mips))
            continue
        for lvl, q in enumerate(chain):
            want_size = max(1, meta["plate_px"] >> lvl)
            if q.shape[0] != want_size or q.shape[1] != want_size:
                problems.append("slice %d mip %d: size %s, want %d^2"
                                % (s, lvl, q.shape[:2], want_size))
                continue
            m = q.reshape(-1, n_ch).mean(axis=0)
            deq = (q.astype(np.float64) - 127.5) / scale
            v = deq.reshape(-1, n_ch).var(axis=0)
            wm = np.array(smeta["mips"][lvl]["mean"])
            wv = np.array(smeta["mips"][lvl]["variance"])
            if not np.allclose(m, wm, atol=1e-4):
                problems.append("slice %d mip %d: mean %s != sidecar %s" % (s, lvl, m, wm))
            if not np.allclose(v, wv, atol=1e-6, rtol=1e-5):
                problems.append("slice %d mip %d: variance %s != sidecar %s" % (s, lvl, v, wv))
            if lvl == 0 and np.abs(m - 127.5).max() > 0.5:
                problems.append("slice %d mip 0: mean %s not within 0.5 of 127.5" % (s, m))

    top_mean = np.array([meta["slice_stats"][s]["mips"][0]["mean"] for s in range(len(chains))])
    top_var = np.array([meta["slice_stats"][s]["mips"][0]["variance"] for s in range(len(chains))])
    print("%s: %dx%d x%d slices x%d mips, DXGI %d (R8G8B8A8_UNORM), %d bytes"
          % (dds_path.name, header["width"], header["height"], header["array_size"],
             header["mip_count"], header["dxgi_format"], dds_path.stat().st_size))
    print("  mip 0 mean per channel (want 127.5): %s" % (top_mean.mean(axis=0).round(4),))
    print("  mip 0 variance (unit-variance target 1.0): min %.5f max %.5f"
          % (top_var.min(), top_var.max()))
    # Tolerance 0.05: mip 0 is unit variance by construction before quantisation; rounding to
    # 8 bits moves it by up to ~1.5% because the coverage field is nearly binary at texel scale
    # and so takes few distinct values (see README "Decisions").
    if abs(top_var.mean() - 1.0) > 0.05:
        problems.append("mip 0 mean variance %.5f is not ~1.0" % top_var.mean())

    if problems:
        print("  FAIL (%d problems):" % len(problems))
        for p in problems[:20]:
            print("    - " + p)
        return 1
    print("  OK")
    return 0


# ---------------------------------------------------------------------------
# Preview
# ---------------------------------------------------------------------------

def preview(out_dir: Path, name: str, top_slice: np.ndarray, spec_meta: dict,
            texel_um: float, plate_px: int) -> None:
    write_png(out_dir / (name + "_slice0.png"), np.ascontiguousarray(top_slice[:, :, :3]))
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("  (matplotlib missing: wrote %s_slice0.png, skipped the spectrum plot; rerun "
              "with `uv run --with matplotlib make_plates.py ... --preview`)" % name)
        return
    freq = np.array(spec_meta["radial_frequency_cycles_per_plate"][1:], dtype=float)
    cyc_per_mm = freq / (plate_px * texel_um / 1000.0)
    fig, ax = plt.subplots(figsize=(6, 4), dpi=120)
    for li, (ch, _) in enumerate(LAYERS):
        ax.loglog(cyc_per_mm, np.array(spec_meta["power"][li][1:]), label="layer " + ch)
    ax.set_xlabel("spatial frequency (cycles / mm on the negative)")
    ax.set_ylabel("radial power S(f)")
    ax.set_title(name + ": Boolean plate radial power spectrum")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_dir / (name + "_spectrum.png"))
    plt.close(fig)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=Path("out"), help="output directory")
    ap.add_argument("--classes", default=",".join(SIZE_CLASSES),
                    help="comma-separated size classes (default: all four)")
    ap.add_argument("--seed", type=int, default=20260905,
                    help="master seed; the whole run is deterministic from it")
    ap.add_argument("--slices", type=int, default=N_SLICES)
    ap.add_argument("--plate-px", type=int, default=PLATE_PX)
    ap.add_argument("--supersample", type=int, default=SUPERSAMPLE)
    ap.add_argument("--target-spectrum", action="store_true",
                    help="emit a white field reshaped by sqrt(S(f)) fitted from the Boolean "
                         "plate, instead of the Boolean plate itself (default is the Boolean "
                         "plate: its spectrum is already the physical one)")
    ap.add_argument("--preview", action="store_true",
                    help="also write <class>_slice0.png and <class>_spectrum.png")
    ap.add_argument("--verify", type=Path, metavar="DDS",
                    help="verify an existing plate against its sidecar and exit")
    args = ap.parse_args(argv)

    if args.verify:
        return verify(args.verify)

    if args.plate_px & (args.plate_px - 1):
        print("--plate-px must be a power of two (the mip chain is exact 2x2 box averaging)")
        return 2

    names = [c.strip() for c in args.classes.split(",") if c.strip()]
    for n in names:
        if n not in SIZE_CLASSES:
            print("unknown size class %r; known: %s" % (n, ", ".join(SIZE_CLASSES)))
            return 2

    args.out.mkdir(parents=True, exist_ok=True)
    mode = "target-spectrum" if args.target_spectrum else "boolean"
    t_all = time.perf_counter()
    for name in names:
        t0 = time.perf_counter()
        texel_um = TEXEL_PER_RBAR * SIZE_CLASSES[name]
        print("[%s] r_bar %g um, texel %g um, extent %.3f mm, mode %s"
              % (name, SIZE_CLASSES[name], texel_um,
                 args.plate_px * texel_um / 1000.0, mode))
        blob, sidecar, spec_meta = build_class(
            name, args.seed, args.plate_px, args.slices, args.supersample, mode)
        dt = time.perf_counter() - t0
        sidecar["build_seconds"] = round(dt, 2)
        (args.out / (name + ".dds")).write_bytes(blob)
        (args.out / (name + ".json")).write_text(
            json.dumps(sidecar, indent=2) + "\n", encoding="utf-8")
        (args.out / (name + ".spectrum.json")).write_text(
            json.dumps(spec_meta) + "\n", encoding="utf-8")
        print("  wrote %s.dds (%d bytes) + %s.json in %.1f s" % (name, len(blob), name, dt))
        if args.preview:
            _, chains = dds_read(args.out / (name + ".dds"))
            preview(args.out, name, chains[0][0], spec_meta, sidecar["texel_um"], args.plate_px)
    print("total %.1f s" % (time.perf_counter() - t_all))
    return 0


if __name__ == "__main__":
    sys.exit(main())
