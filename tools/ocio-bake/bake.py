"""Bake the ACES 2.0 output transform to a Resolve-dialect .cube for the scene-referred pipeline.

The LUT's input domain is ACEScct in [0, 1] (the analytic shaper the shader applies before the
fetch), the grid is N^3 with the RED index varying fastest, and the output is the encoded display
colour space of the chosen display/view (for `sRGB - Display` that is piecewise-sRGB-encoded
Rec.709 code values in [0, 1]).

Each output file carries a provenance comment block, and each gets a `<name>.report.txt` holding
the round-trip error of the baked grid against the live OCIO processor.

Usage:
    uv run bake.py --all
    uv run bake.py --preset odt_srgb_100nit
    uv run bake.py --all --outdir out --also-copy <dir>
    uv run bake.py --list
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import os
import subprocess
import sys

import numpy as np
import PyOpenColorIO as ocio

import acescct

try:
    import tomllib  # Python >= 3.11
except ModuleNotFoundError:  # pragma: no cover
    import tomli as tomllib  # type: ignore

HERE = os.path.dirname(os.path.abspath(__file__))
PRESETS_PATH = os.path.join(HERE, "presets.toml")
DEFAULT_OUTDIR = os.path.join(HERE, "out")

REQUIRED_ACES_VERSION = "2.0"
RNG_SEED = 20260828  # fixed so the error report is reproducible
REPORT_SAMPLES = 200000


# --------------------------------------------------------------------------------------------
# config handling
# --------------------------------------------------------------------------------------------
def load_config(uri: str) -> ocio.Config:
    if uri.startswith("ocio://"):
        return ocio.Config.CreateFromBuiltinConfig(uri[len("ocio://"):])
    return ocio.Config.CreateFromFile(uri)


def config_aces_version(cfg: ocio.Config) -> str:
    """Extract the ACES version the config implements, from its name or description.

    The built-in configs are named `<flavour>-config-vX.Y.Z_aces-vA.B_ocio-vC.D`, and the
    description repeats it as `[ACES vA.B]`.
    """
    import re

    for text in (cfg.getName() or "", cfg.getDescription() or ""):
        m = re.search(r"aces[-_ ]v?(\d+\.\d+)", text, re.IGNORECASE)
        if m:
            return m.group(1)
        m = re.search(r"\[ACES\s+v?(\d+\.\d+)\]", text, re.IGNORECASE)
        if m:
            return m.group(1)
    return "unknown"


def die(msg: str) -> "NoReturn":  # type: ignore[valid-type]
    print("ERROR: %s" % msg, file=sys.stderr)
    sys.exit(2)


def assert_aces_2(cfg: ocio.Config, uri: str) -> str:
    ver = config_aces_version(cfg)
    if ver != REQUIRED_ACES_VERSION:
        print("ERROR: config %s implements ACES %s, not ACES %s."
              % (uri, ver, REQUIRED_ACES_VERSION), file=sys.stderr)
        print("       Baking an ACES 1.x view would silently reintroduce the 1.x RRT hue skews",
              file=sys.stderr)
        print("       that the scene-referred spec (D10) exists to avoid.", file=sys.stderr)
        print("       Available built-in configs:", file=sys.stderr)
        for entry in ocio.BuiltinConfigRegistry().getBuiltinConfigs():
            name = entry[0] if isinstance(entry, (tuple, list)) else entry
            print("         ocio://%s" % name, file=sys.stderr)
        sys.exit(2)
    return ver


def assert_display_view(cfg: ocio.Config, display: str, view: str) -> None:
    displays = list(cfg.getDisplays())
    if display not in displays:
        print("ERROR: display %r not in this config. Available displays:" % display,
              file=sys.stderr)
        for d in displays:
            print("         %s" % d, file=sys.stderr)
        sys.exit(2)
    views = list(cfg.getViews(display))
    if view not in views:
        print("ERROR: view %r not available on display %r. Available views:" % (view, display),
              file=sys.stderr)
        for v in views:
            print("         %s" % v, file=sys.stderr)
        sys.exit(2)
    if not view.startswith("ACES %s" % REQUIRED_ACES_VERSION):
        print("WARNING: view %r does not name ACES %s; make sure this is intentional."
              % (view, REQUIRED_ACES_VERSION))


def assert_colorspace(cfg: ocio.Config, name: str) -> None:
    if cfg.getColorSpace(name) is None:
        print("ERROR: colour space %r not in this config. Available:" % name, file=sys.stderr)
        for cs in cfg.getColorSpaces():
            print("         %s" % cs.getName(), file=sys.stderr)
        sys.exit(2)


# --------------------------------------------------------------------------------------------
# grid + processor
# --------------------------------------------------------------------------------------------
def make_grid(size: int) -> np.ndarray:
    """N^3 x 3 grid over [0,1]^3 with the RED index varying fastest (Resolve .cube order)."""
    axis = np.linspace(0.0, 1.0, size, dtype=np.float32)
    r = np.tile(axis, size * size)
    g = np.tile(np.repeat(axis, size), size)
    b = np.repeat(axis, size * size)
    return np.stack([r, g, b], axis=1).astype(np.float32)


def check_ordering(lut: np.ndarray, size: int) -> None:
    """Known-value test for the red-fastest ordering.

    Getting red/blue the wrong way round produces a plausible-looking but hue-rotated image, so
    assert it rather than trust the loop: a mid-level saturated red must come out red-dominant,
    a mid-level saturated blue blue-dominant, and a neutral must stay neutral.
    """
    def at(r, g, b):
        return lut[b * size * size + g * size + r]

    mid = int(round(0.62 * (size - 1)))  # ACEScct ~0.62, comfortably inside the log segment
    red, blue, neutral = at(mid, 0, 0), at(0, 0, mid), at(mid, mid, mid)
    if not (red[0] > red[1] and red[0] > red[2]):
        die("ordering check failed: the saturated-red node is not red-dominant (%s)" % red)
    if not (blue[2] > blue[0] and blue[2] > blue[1]):
        die("ordering check failed: the saturated-blue node is not blue-dominant (%s)" % blue)
    if float(np.abs(neutral - neutral.mean()).max()) > 2.0e-4:
        die("ordering check failed: a neutral input did not stay neutral (%s)" % neutral)


def get_cpu(cfg: ocio.Config, input_space: str, display: str, view: str):
    dvt = ocio.DisplayViewTransform(src=input_space, display=display, view=view)
    proc = cfg.getProcessor(dvt, ocio.TRANSFORM_DIR_FORWARD)
    return proc.getOptimizedCPUProcessor(
        ocio.BIT_DEPTH_F32, ocio.BIT_DEPTH_F32, ocio.OPTIMIZATION_LOSSLESS
    )


def apply_cpu(cpu, rgb: np.ndarray) -> np.ndarray:
    """Apply a CPU processor to an (N,3) float32 array, out of place."""
    buf = np.ascontiguousarray(rgb, dtype=np.float32).copy()
    desc = ocio.PackedImageDesc(buf, buf.shape[0], 1, 3)
    cpu.apply(desc)
    return buf


# --------------------------------------------------------------------------------------------
# trilinear resampling of the baked grid (what the shader's sampler will do)
# --------------------------------------------------------------------------------------------
def trilinear(lut: np.ndarray, size: int, rgb: np.ndarray) -> np.ndarray:
    """lut: (size^3, 3) in red-fastest order; rgb: (N,3) in [0,1]. Returns (N,3)."""
    cube = lut.reshape(size, size, size, 3)  # [b, g, r, ch]
    p = np.clip(rgb, 0.0, 1.0).astype(np.float64) * (size - 1)
    i0 = np.floor(p).astype(np.int64)
    i0 = np.minimum(i0, size - 2)
    f = p - i0
    out = np.zeros((rgb.shape[0], 3), dtype=np.float64)
    for db in (0, 1):
        wb = f[:, 2] if db else 1.0 - f[:, 2]
        for dg in (0, 1):
            wg = f[:, 1] if dg else 1.0 - f[:, 1]
            for dr in (0, 1):
                wr = f[:, 0] if dr else 1.0 - f[:, 0]
                w = (wr * wg * wb)[:, None]
                out += w * cube[i0[:, 2] + db, i0[:, 1] + dg, i0[:, 0] + dr]
    return out


# --------------------------------------------------------------------------------------------
# provenance
# --------------------------------------------------------------------------------------------
def git_hash() -> str:
    try:
        out = subprocess.run(
            ["git", "-C", HERE, "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=15,
        )
        if out.returncode != 0:
            return "no-git"
        h = out.stdout.strip()
        dirty = subprocess.run(
            ["git", "-C", HERE, "status", "--porcelain", "--", HERE],
            capture_output=True, text=True, timeout=15,
        )
        if dirty.returncode == 0 and dirty.stdout.strip():
            h += "-dirty"
        return h
    except Exception:
        return "no-git"


def tool_digest() -> str:
    h = hashlib.sha256()
    for name in ("bake.py", "acescct.py", "presets.toml"):
        path = os.path.join(HERE, name)
        if os.path.exists(path):
            with open(path, "rb") as fh:
                h.update(fh.read())
    return h.hexdigest()[:16]


def provenance_lines(meta: dict) -> list[str]:
    lines = [
        "# Generated by tools/ocio-bake/%s -- do not edit by hand." % meta.get("tool", "bake.py"),
        "#",
    ]
    # THE MACHINE-READABLE LINE. Everything else in this block is for a human; this one is parsed
    # by the CinematicCamera plugin, which uses it to decide whether a LUT dropped into the Look
    # slot has to be wrapped before it can be a legal LMT (decisions/s10-grade-component.md 4.1).
    # Exact spelling, one space after the colon, nothing else on the line.
    lines.append("# ReC-LUT-Space: %s" % meta.get("rec_lut_space", "ACEScct"))
    lines.append("#")
    lines.extend([
        "# preset            : %s" % meta["preset"],
        "# config URI        : %s" % meta["config_uri"],
        "# config name       : %s" % meta["config_name"],
        "# ACES version      : %s" % meta["aces_version"],
        "# input space       : %s   (LUT domain is this encoding, [0,1], red fastest)" % meta["input_space"],
        "# display           : %s" % meta["display"],
        "# view              : %s" % meta["view"],
        "# output encoding   : %s" % meta.get("output_encoding", "%s (encoded display code values)" % meta["display"]),
        "# grid size         : %d^3" % meta["size"],
        "# OCIO library      : %s" % meta["ocio_version"],
        "# baked (UTC)       : %s" % meta["utc"],
        "# tool git hash     : %s" % meta["git"],
        "# tool sha256[:16]  : %s" % meta["digest"],
        "#",
    ])
    return lines


# --------------------------------------------------------------------------------------------
# writers
# --------------------------------------------------------------------------------------------
def write_cube(path: str, title: str, size: int, data: np.ndarray, meta: dict) -> None:
    lines = provenance_lines(meta)
    for extra in meta.get("notes", []):
        lines.append("# %s" % extra)
    if meta.get("notes"):
        lines.append("#")
    lines.append('TITLE "%s"' % title)
    lines.append("LUT_3D_SIZE %d" % size)
    lines.append("DOMAIN_MIN 0.0 0.0 0.0")
    lines.append("DOMAIN_MAX 1.0 1.0 1.0")
    body = "\n".join("%.6f %.6f %.6f" % (r, g, b) for r, g, b in data)
    with open(path, "w", encoding="ascii", newline="\n") as fh:
        fh.write("\n".join(lines))
        fh.write("\n")
        fh.write(body)
        fh.write("\n")


def stats(err: np.ndarray) -> dict:
    a = np.abs(err)
    return {
        "max": float(a.max()),
        "mean": float(a.mean()),
        "p99": float(np.percentile(a, 99.0)),
        "max_cv8": float(a.max() * 255.0),
        "mean_cv8": float(a.mean() * 255.0),
        "p99_cv8": float(np.percentile(a, 99.0) * 255.0),
    }


def write_report(path: str, meta: dict, blocks: list[tuple[str, dict]]) -> None:
    out = []
    out.extend(provenance_lines(meta))
    out.append("")
    out.append("Round-trip error of the baked grid against the live OCIO processor.")
    out.append("")
    out.append("Method: %d pseudo-random ACEScct samples (seed %d), uniform in [0,1]^3 -- the")
    out[-1] = out[-1] % (REPORT_SAMPLES, RNG_SEED)
    out.append("LUT's own domain, which is where the shader will fetch. Each sample is put")
    out.append("through (i) the live processor and (ii) trilinear interpolation of the baked")
    out.append("grid, exactly as the GPU sampler will do. Errors are in output code value,")
    out.append("i.e. the display-encoded [0,1] the LUT stores; 'cv8' scales that by 255 so")
    out.append("'below half a code value' means cv8 < 0.5.")
    for extra in meta.get("report_notes", []):
        out.append(extra)
    out.append("")
    for label, s in blocks:
        out.append("-- %s" % label)
        out.append("   max  abs err = %.8f   (%.4f 8-bit code values)" % (s["max"], s["max_cv8"]))
        out.append("   mean abs err = %.8f   (%.4f 8-bit code values)" % (s["mean"], s["mean_cv8"]))
        out.append("   p99  abs err = %.8f   (%.4f 8-bit code values)" % (s["p99"], s["p99_cv8"]))
        out.append("")
    with open(path, "w", encoding="ascii", newline="\n") as fh:
        fh.write("\n".join(out))
        fh.write("\n")


# --------------------------------------------------------------------------------------------
# baking
# --------------------------------------------------------------------------------------------
def bake_identity(name: str, spec: dict, outdir: str) -> list[str]:
    size = int(spec.get("size", 33))
    grid = make_grid(size).astype(np.float64)
    meta = {
        "preset": name,
        "config_uri": "(none -- analytic identity)",
        "config_name": "(none)",
        "aces_version": "(n/a)",
        "input_space": spec.get("input_space", "ACEScct"),
        "display": "(none -- pass-through)",
        "view": "(none -- pass-through)",
        "rec_lut_space": spec.get("rec_lut_space", spec.get("input_space", "ACEScct")),
        "size": size,
        "ocio_version": ocio.__version__,
        "utc": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git": git_hash(),
        "digest": tool_digest(),
        "notes": list(spec.get("notes", [])) or [
            "Identity LUT: output == input everywhere. Used to prove the engine's .cube parser,",
            "3D texture upload, sampler addressing and channel order are correct: with this in",
            "the LMT slot the image must be bit-identical to the LMT slot being disabled.",
        ],
    }
    out_cube = os.path.join(outdir, spec["out"])
    write_cube(out_cube, spec.get("title", name), size, grid, meta)
    report = os.path.join(outdir, os.path.splitext(spec["out"])[0] + ".report.txt")
    rng = np.random.default_rng(RNG_SEED)
    samples = rng.random((REPORT_SAMPLES, 3), dtype=np.float32)
    interp = trilinear(grid, size, samples)
    write_report(report, meta, [("identity, %d^3, trilinear vs exact" % size,
                                stats(interp - samples.astype(np.float64)))])
    print("  wrote %s  (%d^3)" % (out_cube, size))
    print("  wrote %s" % report)
    return [out_cube, report]


def bake_inverse_odt(name: str, spec: dict, outdir: str, sizes: list[int] | None) -> list[str]:
    """The INVERSE of an output transform: display code values in, ACEScct out.

    This file is never uploaded to the GPU and is never chosen by a user. It is the second half of
    the wrap recipe (decisions/s10-grade-component.md 4.2): to apply a Rec.709 display LUT to
    scene data the plugin evaluates forward-ODT -> the foreign LUT -> THIS, on the CPU, at load.

    Two things are deliberately different from bake_odt:
      * the output is NOT clamped to [0,1]. The values are ACEScct code values and they do sit
        inside [0,1] in practice, but clamping them would hide a bake mistake rather than reveal
        one, and nothing downstream needs the clamp (no fp16 texture, no shader saturate).
      * there is no ordering check. A display-code-value grid put through an inverse output
        transform is not red-dominant at the red node in any useful sense - the check that
        matters for this file is the round trip in the report below.
    """
    uri = spec["config"]
    cfg = load_config(uri)
    aces = assert_aces_2(cfg, uri)
    output_space = spec.get("output_space", "ACEScct")
    display = spec["display"]
    view = spec["view"]
    assert_colorspace(cfg, output_space)
    assert_display_view(cfg, display, view)

    print("  config %s  (ACES %s, OCIO config v%d.%d)"
          % (cfg.getName(), aces, cfg.getMajorVersion(), cfg.getMinorVersion()))
    print("  [%s | %s] -> %s   (INVERSE)" % (display, view, output_space))

    dvt = ocio.DisplayViewTransform(src=output_space, display=display, view=view)
    inv = cfg.getProcessor(dvt, ocio.TRANSFORM_DIR_INVERSE).getOptimizedCPUProcessor(
        ocio.BIT_DEPTH_F32, ocio.BIT_DEPTH_F32, ocio.OPTIMIZATION_LOSSLESS)
    fwd = get_cpu(cfg, output_space, display, view)

    ship_sizes = sizes if sizes else [int(s) for s in spec.get("sizes", [spec.get("size", 65)])]

    rng = np.random.default_rng(RNG_SEED)
    samples = rng.random((REPORT_SAMPLES, 3), dtype=np.float32)   # display code values
    reference = apply_cpu(inv, samples).astype(np.float64)

    written = []
    for size in ship_sizes:
        grid = make_grid(size)
        data = apply_cpu(inv, grid).astype(np.float64)

        interp = trilinear(data, size, samples)
        blocks = [("%d^3 grid, trilinear vs the live inverse processor "
                   "(errors in ACEScct code value)" % size, stats(interp - reference))]

        # The round trip that actually matters: forward then inverse, over the ACEScct domain the
        # plugin's wrap walks. Everything above display white is NOT recoverable and the numbers
        # below say by how much - which is why the plugin applies the wrap as a residual.
        acescct = rng.random((REPORT_SAMPLES, 3), dtype=np.float32)
        display_cv = np.clip(apply_cpu(fwd, acescct), 0.0, 1.0)
        back = trilinear(data, size, display_cv)
        err = back - acescct.astype(np.float64)
        blocks.append(("%d^3 grid, inv(fwd(x)) vs x over the whole ACEScct domain "
                       "-- the NON-invertible part, quoted so it is not a surprise" % size,
                       stats(err)))
        # The interesting split is by SATURATION, not by brightness: this transform's invertible
        # range reaches ACEScct 0.954 on the neutral axis (= linear ~180, +10 stops over grey), so
        # almost nothing is lost to the highlight clip. What is lost is colour outside the display
        # gamut, which the ACES 2.0 gamut compressor folds inwards on the way out and cannot be
        # unfolded on the way back.
        neutralish = (acescct.max(axis=1) - acescct.min(axis=1)) <= 0.10
        blocks.append(("%d^3 grid, inv(fwd(x)) vs x, NEAR-NEUTRAL samples "
                       "(ACEScct max-min <= 0.10) [%.1f%% of samples]"
                       % (size, 100.0 * neutralish.mean()), stats(err[neutralish])))
        to709 = cfg.getProcessor(output_space, "Linear Rec.709 (sRGB)").getDefaultCPUProcessor()
        in709 = (apply_cpu(to709, acescct) >= 0.0).all(axis=1)
        blocks.append(("%d^3 grid, inv(fwd(x)) vs x, samples INSIDE the Rec.709 gamut "
                       "[%.1f%% of samples]" % (size, 100.0 * in709.mean()), stats(err[in709])))

        meta = {
            "preset": name,
            "config_uri": uri,
            "config_name": cfg.getName(),
            "aces_version": aces,
            "input_space": display,
            "display": display,
            "view": view,
            "output_encoding": "%s (ACEScct code values, NOT clamped)" % output_space,
            "rec_lut_space": spec.get("rec_lut_space", "Rec709"),
            "size": size,
            "ocio_version": ocio.__version__,
            "utc": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "git": git_hash(),
            "digest": tool_digest(),
            "report_notes": [
                "The second and third blocks are round-trip blocks, not interpolation blocks:",
                "they measure how much of the ACEScct domain this inverse can actually recover.",
            ],
            "notes": list(spec.get("notes", [])),
        }
        out_name = spec["out"].replace("{size}", str(size))
        cube = os.path.join(outdir, out_name)
        write_cube(cube, spec.get("title", name), size, data, meta)
        report = os.path.join(outdir, os.path.splitext(out_name)[0] + ".report.txt")
        write_report(report, meta, blocks)
        print("  wrote %s  (%d^3)" % (cube, size))
        print("  wrote %s" % report)
        for label, s in blocks:
            print("    %-78s max %9.5f  mean %9.6f" % (label, s["max"], s["mean"]))
        written.extend([cube, report])
    return written


def fold_grade(grade: dict):
    """Fold the CineCam Grade's own controls into ONE ASC CDL, exactly as the component does.

    A transcription of decisions/s10-grade-component.md 3a.2 (which is what
    SCineGradeParams::Resolve() implements): the ASC order is one affine step then one power, and
    every wheel and the contrast are affine, so they compose into a single (slope, offset) pair
    with nothing left over.

        a = S_base ; b = O_base
        a = (gain - lift) * a ;  b = (gain - lift) * b + lift
        a = contrast * a      ;  b = contrast * b + pivot * (1 - contrast)
        slope = max(a, 1e-4) ; offset = b ; power = max(P_base / gamma, 1e-4)

    Returns (slope, offset, power, saturation, controls-as-typed).
    """
    def vec3(key, default):
        v = grade.get(key, default)
        if isinstance(v, (int, float)):
            v = [float(v)] * 3
        v = [float(x) for x in v]
        if len(v) != 3:
            die("grade.%s needs one number or three" % key)
        return v

    lift = vec3("lift", 0.0)
    gamma = vec3("gamma", 1.0)
    gain = vec3("gain", 1.0)
    base_slope = vec3("slope", 1.0)
    base_offset = vec3("offset", 0.0)
    base_power = vec3("power", 1.0)
    contrast = float(grade.get("contrast", 1.0))
    pivot = float(grade.get("pivot", acescct.MID_GREY))
    saturation = float(grade.get("saturation", 1.0))

    slope, offset, power = [0.0] * 3, [0.0] * 3, [0.0] * 3
    for c in range(3):
        a = base_slope[c]
        b = base_offset[c]
        w = gain[c] - lift[c]
        a, b = w * a, w * b + lift[c]
        a, b = contrast * a, contrast * b + pivot * (1.0 - contrast)
        slope[c] = max(a, 1e-4)
        offset[c] = b
        power[c] = max(base_power[c] / gamma[c], 1e-4)

    controls = dict(lift=lift, gamma=gamma, gain=gain, contrast=contrast, pivot=pivot,
                    base_slope=base_slope, base_offset=base_offset, base_power=base_power)
    return slope, offset, power, saturation, controls


def bake_look(name: str, spec: dict, outdir: str, sizes):
    """A shipped look: the CineCam Grade's own controls, evaluated in ACEScct onto a grid.

    No OCIO, no output transform, no colour science that is not already on the component's own
    controls. That is the whole design (decisions/s10-grade-component.md 4.4): the recipe is
    printed in the header as the numbers you would TYPE INTO THE COMPONENT, plus the single ASC
    CDL they fold into, so a user can read the file and rebuild the look by hand.

    Evaluation order is the chain's own order: ASC CDL -> saturation -> master curve (3b.1).
    """
    size = int((sizes or [int(spec.get("size", 33))])[0])
    grid = make_grid(size).astype(np.float64)

    grade = dict(spec.get("grade", {}))
    slope, offset, power, saturation, controls = fold_grade(grade)
    stops = [float(v) for v in spec.get("curve", {}).get("master_stops", [0.0] * acescct.KNOT_COUNT)]
    if len(stops) != acescct.KNOT_COUNT:
        die("preset %r: curve.master_stops needs exactly %d values" % (name, acescct.KNOT_COUNT))

    data = acescct.apply_cdl(grid, slope, offset, power, saturation)
    data = acescct.apply_master_curve(data, stops)

    neutral_base = (controls["base_slope"] == [1.0] * 3
                    and controls["base_offset"] == [0.0] * 3
                    and controls["base_power"] == [1.0] * 3)

    recipe = [
        "THE RECIPE, so that nothing here is a mystery. Everything below is a control the CineCam",
        "Grade component itself has: type these numbers in and you get this file's picture, live.",
        "Applied in ACEScct, in the chain's own order: ASC CDL -> saturation -> master curve.",
        "",
        "  Grade group",
        "    Lift (RGB)          %+.6f %+.6f %+.6f" % tuple(controls["lift"]),
        "    Gamma (RGB)          %.6f  %.6f  %.6f" % tuple(controls["gamma"]),
        "    Gain (RGB)           %.6f  %.6f  %.6f" % tuple(controls["gain"]),
        "    Contrast             %.6f" % controls["contrast"],
        "    Contrast Pivot       %.7f   (%s)" % (
            controls["pivot"],
            "= ACEScct(0.18), the default"
            if abs(controls["pivot"] - acescct.MID_GREY) < 1e-6 else "NOT the default pivot"),
        "    Saturation           %.6f" % saturation,
        ("    CDL base (slope/offset/power) left neutral" if neutral_base else
         "    CDL base            slope %s offset %s power %s"
         % (controls["base_slope"], controls["base_offset"], controls["base_power"])),
        "",
        "  Curves group -- master curve, in STOPS (black / shadow / mid / highlight / white;",
        "  knots at ACEScct 0, mid grey -4 stops, mid grey, mid grey +4 stops, 1.0):",
        "    %+.3f  %+.3f  %+.3f  %+.3f  %+.3f" % tuple(stops),
        "",
        "  and the ONE ASC CDL the whole Grade group folds into -- what the camera publishes and",
        "  what a .cdl sidecar carries. The master curve above is NOT in it: a CDL cannot hold a",
        "  curve, which is exactly why the two are separate groups on the component.",
        "    slope      %.6f %.6f %.6f" % tuple(slope),
        "    offset     %.6f %.6f %.6f" % tuple(offset),
        "    power      %.6f %.6f %.6f" % tuple(power),
        "    saturation %.6f" % saturation,
    ]

    meta = {
        "preset": name,
        "config_uri": "(none -- ACEScct arithmetic only, no OCIO)",
        "config_name": "(none)",
        "aces_version": "(n/a -- a look is not an ACES transform)",
        "input_space": "ACEScct",
        "display": "(none -- a look, not an output transform)",
        "view": "(none)",
        "output_encoding": "ACEScct (a look: same encoding in and out)",
        "rec_lut_space": "ACEScct",
        "size": size,
        "ocio_version": ocio.__version__,
        "utc": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git": git_hash(),
        "digest": tool_digest(),
        "notes": list(spec.get("notes", [])) + [""] + recipe,
    }

    out_name = spec["out"].replace("{size}", str(size))
    cube = os.path.join(outdir, out_name)
    write_cube(cube, spec.get("title", name), size, data, meta)

    # The report answers one question: how far does trilinear interpolation of this grid sit from
    # evaluating the recipe exactly? A look built from smooth operators interpolates well; a
    # number here that is not tiny would mean the grid is too coarse for the recipe.
    rng = np.random.default_rng(RNG_SEED)
    samples = rng.random((REPORT_SAMPLES, 3), dtype=np.float32)
    exact = acescct.apply_master_curve(
        acescct.apply_cdl(samples.astype(np.float64), slope, offset, power, saturation), stops)
    interp = trilinear(data, size, samples)
    err = interp - exact
    # Split at the ACEScct toe. Below ACEScct 0.155 the encoding is a straight segment over linear
    # [0, 2^-7] - i.e. everything more than 4.5 stops under mid grey, which the output transform
    # puts at display black - and that is where a tone curve's curvature is steepest and a coarse
    # grid interpolates worst. Quoting one number over the whole domain would attribute an error
    # nobody can see to the part of the picture everybody looks at.
    above_toe = (samples >= acescct.ACESCCT_Y_BRK).all(axis=1)
    blocks = [
        ("%d^3 grid, trilinear vs the recipe evaluated exactly, WHOLE domain "
         "(errors in ACEScct code value; 1.0 / 17.52 = one stop)" % size, stats(err)),
        ("%d^3 grid, above the ACEScct toe (>= %.4f in every channel) [%.1f%% of samples]"
         % (size, acescct.ACESCCT_Y_BRK, 100.0 * above_toe.mean()), stats(err[above_toe])),
    ]
    report = os.path.join(outdir, os.path.splitext(out_name)[0] + ".report.txt")
    write_report(report, meta, blocks)
    print("  wrote %s  (%d^3)" % (cube, size))
    print("  wrote %s" % report)
    for label, s in blocks:
        print("    %-78s max %9.6f  mean %9.7f" % (label, s["max"], s["mean"]))
    return [cube, report]


def bake_odt(name: str, spec: dict, outdir: str, sizes: list[int] | None) -> list[str]:
    uri = spec["config"]
    cfg = load_config(uri)
    aces = assert_aces_2(cfg, uri)
    input_space = spec.get("input_space", "ACEScct")
    display = spec["display"]
    view = spec["view"]
    assert_colorspace(cfg, input_space)
    assert_display_view(cfg, display, view)

    print("  config %s  (ACES %s, OCIO config v%d.%d)"
          % (cfg.getName(), aces, cfg.getMajorVersion(), cfg.getMinorVersion()))
    print("  %s -> [%s | %s]" % (input_space, display, view))

    cpu = get_cpu(cfg, input_space, display, view)

    ship_sizes = sizes if sizes else [int(s) for s in spec.get("sizes", [spec.get("size", 33)])]
    report_sizes = sorted(set(ship_sizes) | {33, 65})

    # error statistics: one random sample set, reused for every size
    rng = np.random.default_rng(RNG_SEED)
    samples = rng.random((REPORT_SAMPLES, 3), dtype=np.float32)
    reference = np.clip(apply_cpu(cpu, samples), 0.0, 1.0).astype(np.float64)

    baked: dict[int, np.ndarray] = {}
    for size in report_sizes:
        grid = make_grid(size)
        baked[size] = np.clip(apply_cpu(cpu, grid), 0.0, 1.0).astype(np.float64)
        check_ordering(baked[size], size)

    # Subsets. The worst trilinear error is not in the normally-exposed range but on the
    # cusp of the ACES 2.0 gamut compressor, at bright colours far outside Rec.709, so report
    # the whole domain and the useful subsets separately rather than quoting one number.
    to709 = cfg.getProcessor(input_space, "Linear Rec.709 (sRGB)").getDefaultCPUProcessor()
    lin709 = apply_cpu(to709, samples)
    in709 = (lin709 >= 0.0).all(axis=1)
    neutralish = (samples.max(axis=1) - samples.min(axis=1)) <= 0.10
    subsets = [
        ("all samples in the LUT domain", np.ones(samples.shape[0], dtype=bool)),
        ("samples inside the Rec.709 gamut", in709),
        ("near-neutral samples (ACEScct max-min <= 0.10)", neutralish),
    ]

    blocks = []
    for size in report_sizes:
        interp = trilinear(baked[size], size, samples)
        err = interp - reference
        for label, mask in subsets:
            blocks.append(("%d^3 grid, trilinear -- %s [%.1f%% of samples]"
                           % (size, label, 100.0 * mask.mean()),
                           stats(err[mask])))

    written = []
    for size in ship_sizes:
        out_name = spec["out"] if len(ship_sizes) == 1 else spec["out"]
        out_name = out_name.replace("{size}", str(size))
        if "{size}" not in spec["out"] and len(ship_sizes) > 1:
            stem, ext = os.path.splitext(spec["out"])
            out_name = "%s_%d%s" % (stem, size, ext)
        meta = {
            "preset": name,
            "config_uri": uri,
            "config_name": cfg.getName(),
            "aces_version": aces,
            "input_space": input_space,
            "display": display,
            "view": view,
            "rec_lut_space": spec.get("rec_lut_space", input_space),
            "size": size,
            "ocio_version": ocio.__version__,
            "utc": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "git": git_hash(),
            "digest": tool_digest(),
            "notes": list(spec.get("notes", [])) + [
                "Output values are clamped to [0,1]; the shaper (linear -> ACEScct) is applied",
                "analytically in the shader before the fetch, so this file carries no 1D LUT.",
            ],
        }
        cube = os.path.join(outdir, out_name)
        write_cube(cube, spec.get("title", name), size, baked[size], meta)
        report = os.path.join(outdir, os.path.splitext(out_name)[0] + ".report.txt")
        write_report(report, meta, blocks)
        print("  wrote %s  (%d^3)" % (cube, size))
        print("  wrote %s" % report)
        for label, s in blocks:
            print("    %-72s max %7.3f cv8  mean %7.4f cv8"
                  % (label, s["max_cv8"], s["mean_cv8"]))
        written.extend([cube, report])
    return written


# --------------------------------------------------------------------------------------------
def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--presets", default=PRESETS_PATH)
    ap.add_argument("--preset", action="append", default=None, help="bake only this preset")
    ap.add_argument("--all", action="store_true", help="bake every preset")
    ap.add_argument("--list", action="store_true", help="list the presets and exit")
    ap.add_argument("--size", type=int, action="append", default=None,
                    help="override the grid size(s) for this run")
    ap.add_argument("--outdir", default=DEFAULT_OUTDIR)
    ap.add_argument("--also-copy", default=None,
                    help="directory to copy the shipped .cube/.report.txt files into")
    args = ap.parse_args(argv)

    with open(args.presets, "rb") as fh:
        presets = tomllib.load(fh)

    if args.list:
        for name, spec in presets.items():
            print("%-28s %s" % (name, spec.get("title", "")))
        return 0

    if args.all:
        chosen = list(presets)
    elif args.preset:
        chosen = args.preset
    else:
        ap.error("give --all, --preset NAME, or --list")

    os.makedirs(args.outdir, exist_ok=True)
    print("PyOpenColorIO %s" % ocio.__version__)
    print("output directory: %s" % os.path.abspath(args.outdir))
    print()

    shipped: list[str] = []
    for name in chosen:
        if name not in presets:
            die("no preset named %r in %s" % (name, args.presets))
        spec = presets[name]
        print("[%s] %s" % (name, spec.get("title", "")))
        kind = spec.get("kind", "odt")
        if kind == "identity":
            files = bake_identity(name, spec, args.outdir)
        elif kind == "inverse_odt":
            files = bake_inverse_odt(name, spec, args.outdir, args.size)
        elif kind == "look":
            files = bake_look(name, spec, args.outdir, args.size)
        elif kind == "odt":
            files = bake_odt(name, spec, args.outdir, args.size)
        else:
            die("preset %r has unknown kind %r (odt, inverse_odt, look, identity)" % (name, kind))
        if spec.get("ship", True):
            shipped.extend(files)
        print()

    if args.also_copy:
        import shutil
        os.makedirs(args.also_copy, exist_ok=True)
        for path in shipped:
            shutil.copy2(path, os.path.join(args.also_copy, os.path.basename(path)))
        print("copied %d shipped files into %s" % (len(shipped), os.path.abspath(args.also_copy)))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
