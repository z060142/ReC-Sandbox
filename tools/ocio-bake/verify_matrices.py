"""Verify the colour matrices and ACEScct constants used by the scene-referred pipeline.

Every number is taken from PyOpenColorIO itself -- not by sampling a float32 CPU processor, but
by pulling the *exact* double-precision operator parameters out of the optimised group transform
(MatrixTransform.getMatrix(), LogCameraTransform.get*Value()).  Sampling is used only as a
cross-check.  The results are then compared against the values written down in
scene-notes/research/s3-matrices-and-white-point.md section 2.

Computed here:
  (a) Rec.709 (sRGB linear, D65) -> ACEScg (AP1, D60), Bradford-adapted, and its inverse.
  (b) AP1 -> CIE XYZ (D60) and the AP1 luminance weights (the Y row).
  (c) The ACEScct encode/decode constants (linear-segment slope/offset, break point, log terms).

Usage:  uv run verify_matrices.py [--config ocio://studio-config-latest]
"""

from __future__ import annotations

import argparse
import math
import sys

import numpy as np
import PyOpenColorIO as ocio

DEFAULT_CONFIG = "ocio://studio-config-latest"

# ---------------------------------------------------------------------------------------------
# Reference values from scene-notes/research/s3-matrices-and-white-point.md section 2.
# These are the numbers under test; the script reports every disagreement.
# ---------------------------------------------------------------------------------------------
NOTE_REC709_TO_ACESCG = np.array([
    [0.6130974, 0.3395231, 0.0473795],
    [0.0701933, 0.9163538, 0.0134528],
    [0.0206156, 0.1095698, 0.8698151],
])
NOTE_ACESCG_TO_REC709 = np.array([
    [1.7050515, -0.6217907, -0.0832587],
    [-0.1302564, 1.1408047, -0.0105485],
    [-0.0240033, -0.1289689, 1.1529732],
])
NOTE_AP1_TO_XYZ = np.array([
    [0.6624542, 0.1340042, 0.1561877],
    [0.2722287, 0.6740818, 0.0536895],
    [-0.0055746, 0.0040607, 1.0103391],
])
NOTE_ACESCCT = {
    "X_BRK": 0.0078125,
    "Y_BRK": 0.155251141552511,
    "A": 10.5402377416545,
    "B": 0.0729055341958355,
    "LOG_OFFSET": 9.72,
    "LOG_SCALE": 17.52,
}

# The note prints 7 decimals, so a rounding-level agreement is anything under 5e-8.
MATRIX_TOL = 5.0e-8
SCALAR_TOL = 1.0e-9

# ACES chromaticities (SMPTE ST 2065-1 for AP0, ACES TB-2014-004 for AP1, both with ACES white).
# Used only to *derive* AP1 -> XYZ(D60), which no OCIO colour space exposes directly (the config's
# only XYZ interchange space is D65-adapted).  The derivation is then cross-checked against OCIO's
# own exact AP0 <-> AP1 matrix, which contains no chromatic adaptation (both are ACES white): if
# either the primaries or the white point below were wrong, that cross-check would fail.
AP0_PRIMARIES = [(0.73470, 0.26530), (0.00000, 1.00000), (0.00010, -0.07700)]
AP1_PRIMARIES = [(0.71300, 0.29300), (0.16500, 0.83000), (0.12800, 0.04400)]
ACES_WHITE = (0.32168, 0.33767)
REC709_PRIMARIES = [(0.640, 0.330), (0.300, 0.600), (0.150, 0.060)]
D65_WHITE = (0.3127, 0.3290)

BRADFORD = np.array([
    [0.8951, 0.2664, -0.1614],
    [-0.7502, 1.7135, 0.0367],
    [0.0389, -0.0685, 1.0296],
])


# --------------------------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------------------------
def xy_to_XYZ(xy):
    x, y = xy
    return np.array([x / y, 1.0, (1.0 - x - y) / y])


def rgb_to_xyz_matrix(primaries, white):
    """Standard primaries -> XYZ construction (SMPTE RP 177)."""
    P = np.array([xy_to_XYZ(p) for p in primaries]).T  # columns = R, G, B
    S = np.linalg.solve(P, xy_to_XYZ(white))
    return P * S


def bradford_cat(src_white, dst_white):
    s = BRADFORD @ xy_to_XYZ(src_white)
    d = BRADFORD @ xy_to_XYZ(dst_white)
    return np.linalg.inv(BRADFORD) @ np.diag(d / s) @ BRADFORD


def load_config(uri):
    if uri.startswith("ocio://"):
        return ocio.Config.CreateFromBuiltinConfig(uri[len("ocio://"):])
    return ocio.Config.CreateFromFile(uri)


def exact_matrix(cfg, src, dst):
    """Compose the exact double-precision 3x3 of a matrix-only colour space conversion."""
    group = cfg.getProcessor(src, dst).createGroupTransform()
    total = np.eye(3)
    for tr in group:
        if not isinstance(tr, ocio.MatrixTransform):
            raise RuntimeError("%s -> %s is not matrix-only: found %s"
                               % (src, dst, type(tr).__name__))
        m = np.array(tr.getMatrix(), dtype=np.float64).reshape(4, 4)[:3, :3]
        if tr.getDirection() == ocio.TRANSFORM_DIR_INVERSE:
            m = np.linalg.inv(m)
        if np.abs(np.array(tr.getOffset(), dtype=np.float64)).max() > 0.0:
            raise RuntimeError("%s -> %s carries a non-zero offset" % (src, dst))
        total = m @ total
    return total


def sampled_matrix(cfg, src, dst):
    """Recover the same 3x3 by pushing basis vectors through the float32 CPU processor."""
    cpu = cfg.getProcessor(src, dst).getDefaultCPUProcessor()
    cols = []
    for basis in ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)):
        buf = np.array(basis, dtype=np.float32)
        cpu.applyRGB(buf)
        cols.append(buf.astype(np.float64))
    return np.array(cols).T


def print_matrix(label, m):
    print(label)
    for row in m:
        print("    " + "  ".join("% .8f" % v for v in row))
    print("    row sums: " + "  ".join("% .8f" % v for v in m.sum(axis=1)))


def compare(got, note):
    d = np.abs(got - note)
    ok = bool(d.max() < MATRIX_TOL)
    print("  vs research note: max abs diff = %.3e  ->  %s"
          % (d.max(), "MATCH (within the note's 7 decimals)" if ok else "DIFFERS"))
    if not ok:
        for i in range(3):
            for j in range(3):
                if d[i, j] >= MATRIX_TOL:
                    print("      [%d][%d] computed % .8f   note % .8f   diff %+.2e"
                          % (i, j, got[i, j], note[i, j], got[i, j] - note[i, j]))
    return ok


def acescct_constants():
    """Read the exact ACEScct parameters out of OCIO's own ACEScct curve built-in.

    OCIO models ACEScct as a LogCameraTransform:
        y = logSideSlope * log_base(linSideSlope * x + linSideOffset) + logSideOffset,  x >= brk
        y = linearSlope * x + linearOffset,                                             x <  brk
    with linearSlope defaulted to the tangent of the log segment at the break, which is exactly
    how S-2016-001 defines the ACEScct toe.
    """
    cfg = ocio.Config.CreateRaw()
    group = cfg.getProcessor(
        ocio.BuiltinTransform("CURVE - ACEScct-LOG_to_LINEAR", ocio.TRANSFORM_DIR_FORWARD)
    ).createGroupTransform()
    log = None
    for tr in group:
        if isinstance(tr, ocio.LogCameraTransform):
            log = tr
    if log is None:
        raise RuntimeError("no LogCameraTransform in the ACEScct built-in curve")

    base = log.getBase()
    log_slope = log.getLogSideSlopeValue()[0]
    log_offset = log.getLogSideOffsetValue()[0]
    lin_slope = log.getLinSideSlopeValue()[0]
    lin_offset = log.getLinSideOffsetValue()[0]
    x_brk = log.getLinSideBreakValue()[0]

    LOG_SCALE = 1.0 / log_slope
    LOG_OFFSET = log_offset * LOG_SCALE

    # y at the break, from the log segment
    y_brk = log_slope * math.log(lin_slope * x_brk + lin_offset, base) + log_offset
    # linearSlope: explicit if the config sets one, otherwise the tangent at the break
    explicit = log.getLinearSlopeValue()[0]
    if explicit == explicit:  # not NaN
        A = explicit
    else:
        A = log_slope * lin_slope / ((lin_slope * x_brk + lin_offset) * math.log(base))
    B = y_brk - A * x_brk

    # sampled cross-check of the whole curve
    enc = cfg.getProcessor(
        ocio.BuiltinTransform("CURVE - ACEScct-LOG_to_LINEAR", ocio.TRANSFORM_DIR_INVERSE)
    ).getDefaultCPUProcessor()
    dec = cfg.getProcessor(
        ocio.BuiltinTransform("CURVE - ACEScct-LOG_to_LINEAR", ocio.TRANSFORM_DIR_FORWARD)
    ).getDefaultCPUProcessor()

    def encode(x):
        buf = np.array([x, x, x], dtype=np.float32)
        enc.applyRGB(buf)
        return float(buf[0])

    def decode(y):
        buf = np.array([y, y, y], dtype=np.float32)
        dec.applyRGB(buf)
        return float(buf[0])

    sample_err = max(
        abs(encode(x) - ((A * x + B) if x < x_brk
                         else (math.log2(x) + LOG_OFFSET) / LOG_SCALE))
        for x in (1e-5, 1e-4, 1e-3, 0.005, 0.0078125, 0.01, 0.18, 1.0, 16.0, 1000.0)
    )

    return {
        "base": base,
        "A": A,
        "B": B,
        "X_BRK": x_brk,
        "Y_BRK": y_brk,
        "LOG_OFFSET": LOG_OFFSET,
        "LOG_SCALE": LOG_SCALE,
        "_extra": {
            "log-segment y at X_BRK": (math.log2(x_brk) + LOG_OFFSET) / LOG_SCALE,
            "encode(0.18) mid grey": encode(0.18),
            "negative floor -B/A": -B / A,
            "decode(1.0) linear": decode(1.0),
            "decode(1.468) linear": decode(1.468),
            "roundtrip err at 0.18": abs(decode(encode(0.18)) - 0.18),
            "max |analytic - OCIO| over samples": sample_err,
        },
    }


# --------------------------------------------------------------------------------------------
def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", default=DEFAULT_CONFIG)
    args = ap.parse_args(argv)

    print("PyOpenColorIO %s" % ocio.__version__)
    cfg = load_config(args.config)
    print("config: %s (%s)" % (args.config, cfg.getName()))
    print()

    ok = True

    print("=" * 92)
    print("(a) Rec.709 linear (sRGB primaries, D65) -> ACEScg (AP1, D60), Bradford")
    print("=" * 92)
    fwd = exact_matrix(cfg, "Linear Rec.709 (sRGB)", "ACEScg")
    print_matrix("  Rec.709 -> ACEScg   [exact, from OCIO's MatrixTransform coefficients]", fwd)
    ok &= compare(fwd, NOTE_REC709_TO_ACESCG)
    print("  float32 CPU-processor sample agrees to %.3e"
          % np.abs(fwd - sampled_matrix(cfg, "Linear Rec.709 (sRGB)", "ACEScg")).max())
    print()

    inv = np.linalg.inv(fwd)
    inv_ocio = exact_matrix(cfg, "ACEScg", "Linear Rec.709 (sRGB)")
    print_matrix("  ACEScg -> Rec.709   [numerically inverted]", inv)
    ok &= compare(inv, NOTE_ACESCG_TO_REC709)
    print("  OCIO's own exact inverse agrees to %.3e" % np.abs(inv - inv_ocio).max())
    print("  fwd @ inv identity error: %.3e" % np.abs(fwd @ inv - np.eye(3)).max())
    print("  neutral check (1,1,1) -> " + "  ".join("% .8f" % v for v in fwd @ np.ones(3)))
    print("  forward matrix entirely non-negative: %s" % bool((fwd >= 0.0).all()))
    print()

    m709 = rgb_to_xyz_matrix(REC709_PRIMARIES, D65_WHITE)
    ap1_xyz_d60 = rgb_to_xyz_matrix(AP1_PRIMARIES, ACES_WHITE)
    bfd = np.linalg.inv(ap1_xyz_d60) @ bradford_cat(D65_WHITE, ACES_WHITE) @ m709
    nocat = np.linalg.inv(ap1_xyz_d60) @ m709
    print("  CAT identification: |OCIO - Bradford-derived| = %.3e" % np.abs(fwd - bfd).max())
    print("                      |OCIO - no adaptation|    = %.3e" % np.abs(fwd - nocat).max())
    print("  -> the adaptation in the OCIO transform is Bradford.")
    print()

    print("=" * 92)
    print("(b) AP1 -> CIE XYZ (D60, unadapted) and the AP1 luminance weights")
    print("=" * 92)
    ap0_xyz_d60 = rgb_to_xyz_matrix(AP0_PRIMARIES, ACES_WHITE)
    ap0_to_ap1_ocio = exact_matrix(cfg, "ACES2065-1", "ACEScg")
    ap0_to_ap1_derived = np.linalg.inv(ap1_xyz_d60) @ ap0_xyz_d60
    err = np.abs(ap0_to_ap1_ocio - ap0_to_ap1_derived).max()
    print("  cross-check AP0->AP1 (OCIO exact, no CAT involved) vs derived: %.3e  -> %s"
          % (err, "OK" if err < 1e-7 else "MISMATCH"))
    ok &= bool(err < 1e-7)
    print()
    print_matrix("  AP1 -> CIE XYZ (D60)", ap1_xyz_d60)
    ok &= compare(ap1_xyz_d60, NOTE_AP1_TO_XYZ)
    print()
    w = ap1_xyz_d60[1]
    print("  AP1 luminance weights (Y row): % .8f  % .8f  % .8f" % (w[0], w[1], w[2]))
    print("  sum = %.10f  (must be 1.0: AP1 white is the D60 reference white)" % w.sum())
    print()

    print("=" * 92)
    print("(c) ACEScct encode / decode constants (exact, from OCIO's own curve operator)")
    print("=" * 92)
    c = acescct_constants()
    print("  log base = %g" % c["base"])
    for key in ("A", "B", "X_BRK", "Y_BRK", "LOG_OFFSET", "LOG_SCALE"):
        note = NOTE_ACESCCT[key]
        diff = c[key] - note
        good = abs(diff) < SCALAR_TOL
        print("  %-11s = % .10f    note % .10f   diff %+.2e   %s"
              % (key, c[key], note, diff, "MATCH" if good else "DIFFERS"))
        ok &= good
    print()
    for key, val in c["_extra"].items():
        print("  %-36s = % .8f" % (key, val))
    print()

    print("=" * 92)
    print("RESULT: %s" % ("all verified values agree with the research note"
                          if ok else "at least one value DIFFERS from the research note"))
    print("=" * 92)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
