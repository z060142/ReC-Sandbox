"""ACEScct arithmetic shared by bake.py and wrap.py.

Everything here is a transcription of code that already runs in the engine, and the transcription
is the point: a shipped look whose recipe the user cannot reproduce on the CineCam Grade's own
wheels and curve would be a mystery, which is exactly what the shipped looks exist not to be.

Sources, verbatim:
  * the shaper                 Engine/Shaders/HWScripts/CryFX/CommonMath.cfi:271-300
  * the ASC CDL + saturation   Engine/Shaders/HWScripts/CryFX/HDRPostProcess.cfx:1270-1274
  * the master tone curve      Code/CryEngine/CryCommon/CryRenderer/SceneReferredCurves.h:89-197
"""

from __future__ import annotations

import numpy as np

# --------------------------------------------------------------------------------------------
# the shaper (CommonMath.cfi:271-300)
# --------------------------------------------------------------------------------------------
ACESCCT_X_BRK = 0.0078125000
ACESCCT_Y_BRK = 0.1552511416
ACESCCT_A = 10.5402377417
ACESCCT_B = 0.0729055342
ACESCCT_LOG_OFFSET = 9.7200000000
ACESCCT_LOG_SCALE = 17.5200000000

#! ACEScct(0.18). The one anchor the pre-exposure guarantees; the default contrast pivot and the
#  master curve's middle knot.
MID_GREY = 0.4135878


def linear_to_acescct(x: np.ndarray) -> np.ndarray:
    x = np.asarray(x, dtype=np.float64)
    with np.errstate(divide="ignore", invalid="ignore"):
        log_part = (np.log2(np.maximum(x, 1e-30)) + ACESCCT_LOG_OFFSET) / ACESCCT_LOG_SCALE
    return np.where(x < ACESCCT_X_BRK, ACESCCT_A * x + ACESCCT_B, log_part)


def acescct_to_linear(y: np.ndarray) -> np.ndarray:
    y = np.asarray(y, dtype=np.float64)
    return np.where(y < ACESCCT_Y_BRK,
                    (y - ACESCCT_B) / ACESCCT_A,
                    np.exp2(y * ACESCCT_LOG_SCALE - ACESCCT_LOG_OFFSET))


# --------------------------------------------------------------------------------------------
# ASC CDL + saturation (HDRPostProcess.cfx:1270-1274)
# --------------------------------------------------------------------------------------------
#! The ASC's own weights, NOT AP1's - see the long comment at HDRPostProcess.cfx:1257.
ASC_CDL_LUMA_WEIGHTS = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)


def apply_cdl(rgb: np.ndarray, slope, offset, power, saturation: float) -> np.ndarray:
    """(in * slope + offset) ^ power, then saturation about the graded luma. (N,3) in, (N,3) out."""
    s = np.asarray(slope, dtype=np.float64).reshape(1, 3)
    o = np.asarray(offset, dtype=np.float64).reshape(1, 3)
    p = np.asarray(power, dtype=np.float64).reshape(1, 3)
    out = np.asarray(rgb, dtype=np.float64) * s + o
    # The ASC's rule, and the shader's max(): negative values are clamped to zero before the power.
    out = np.power(np.maximum(out, 0.0), p)
    luma = out @ ASC_CDL_LUMA_WEIGHTS
    return luma[:, None] + float(saturation) * (out - luma[:, None])


# --------------------------------------------------------------------------------------------
# the master tone curve (SceneReferredCurves.h)
# --------------------------------------------------------------------------------------------
KNOT_COUNT = 5


def master_knot_x() -> np.ndarray:
    """SceneReferredCurves::MasterKnotX() - the bottom of the encoding, +-4 stops around mid grey,
    and the top (= +10.3 stops over grey)."""
    return np.array([
        0.0,
        MID_GREY - 4.0 / ACESCCT_LOG_SCALE,
        MID_GREY,
        MID_GREY + 4.0 / ACESCCT_LOG_SCALE,
        1.0,
    ], dtype=np.float64)


def master_knot_y(stops) -> np.ndarray:
    """SceneReferredCurves::MasterKnotY() - heights from five offsets in stops. All zero = y == x."""
    stops = np.asarray(stops, dtype=np.float64)
    if stops.shape != (KNOT_COUNT,):
        raise ValueError("the master curve takes exactly %d offsets in stops" % KNOT_COUNT)
    return master_knot_x() + stops / ACESCCT_LOG_SCALE


def evaluate_hermite(x: np.ndarray, y: np.ndarray, t: np.ndarray) -> np.ndarray:
    """SceneReferredCurves::Evaluate() - monotone cubic Hermite (Fritsch & Carlson 1980) with the
    Fritsch-Butland weighted harmonic mean for the interior tangents.

    Transcribed rather than replaced by scipy's PCHIP: the engine and the shipped looks have to be
    the SAME curve, and "the same algorithm" is not the same thing as "the same code".
    """
    x = np.asarray(x, dtype=np.float64)
    y = np.asarray(y, dtype=np.float64)
    n = len(x)

    h = np.maximum(x[1:] - x[:-1], 1e-6)
    d = (y[1:] - y[:-1]) / h

    m = np.empty(n, dtype=np.float64)
    m[0] = d[0]
    m[n - 1] = d[n - 2]
    for i in range(1, n - 1):
        if d[i - 1] * d[i] <= 0.0:
            m[i] = 0.0
        else:
            w1 = 2.0 * h[i] + h[i - 1]
            w2 = h[i] + 2.0 * h[i - 1]
            m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i])

    t = np.asarray(t, dtype=np.float64)
    out = np.empty_like(t)

    below = t <= x[0]
    above = t >= x[n - 1]
    out[below] = y[0]
    out[above] = y[n - 1]

    inside = ~(below | above)
    if np.any(inside):
        ti = t[inside]
        # The same forward walk the C++ does: k is the last knot whose x is not past t.
        k = np.zeros(ti.shape, dtype=np.int64)
        for j in range(n - 2):
            k = np.where(ti > x[j + 1], j + 1, k)
        s = (ti - x[k]) / h[k]
        s2 = s * s
        s3 = s2 * s
        h00 = 2.0 * s3 - 3.0 * s2 + 1.0
        h10 = s3 - 2.0 * s2 + s
        h01 = -2.0 * s3 + 3.0 * s2
        h11 = s3 - s2
        out[inside] = h00 * y[k] + h10 * h[k] * m[k] + h01 * y[k + 1] + h11 * h[k] * m[k + 1]

    return out


def apply_master_curve(rgb: np.ndarray, stops) -> np.ndarray:
    """The master curve as the shader applies it, including the out-of-domain rule at
    HDRPostProcess.cfx:1300: curve(saturate(x)) + (x - saturate(x)), i.e. the curve extended with
    slope 1 outside [0,1]. Identity for all-zero offsets, exactly."""
    stops = np.asarray(stops, dtype=np.float64)
    if not np.any(stops):
        return np.asarray(rgb, dtype=np.float64)

    x = master_knot_x()
    y = master_knot_y(stops)
    flat = np.asarray(rgb, dtype=np.float64).reshape(-1)
    inside = np.clip(flat, 0.0, 1.0)
    outside = flat - inside
    curved = np.clip(evaluate_hermite(x, y, inside), -1.0, 2.0)
    return (curved + outside).reshape(np.shape(rgb))


# --------------------------------------------------------------------------------------------
# trilinear sampling of a .cube grid (what the GPU sampler and the plugin's CPU wrap both do)
# --------------------------------------------------------------------------------------------
def trilinear(lut: np.ndarray, size: int, rgb: np.ndarray) -> np.ndarray:
    """lut: (size^3, 3) in red-fastest order; rgb: (N,3). Input is clamped to [0,1] first, exactly
    as the shader's saturate() and the plugin's CPU wrap do."""
    cube = lut.reshape(size, size, size, 3)  # [b, g, r, ch]
    p = np.clip(np.asarray(rgb, dtype=np.float64), 0.0, 1.0) * (size - 1)
    i0 = np.minimum(np.floor(p).astype(np.int64), size - 2)
    f = p - i0
    out = np.zeros((p.shape[0], 3), dtype=np.float64)
    for db in (0, 1):
        wb = f[:, 2] if db else 1.0 - f[:, 2]
        for dg in (0, 1):
            wg = f[:, 1] if dg else 1.0 - f[:, 1]
            for dr in (0, 1):
                wr = f[:, 0] if dr else 1.0 - f[:, 0]
                out += (wr * wg * wb)[:, None] * cube[i0[:, 2] + db, i0[:, 1] + dg, i0[:, 0] + dr]
    return out
