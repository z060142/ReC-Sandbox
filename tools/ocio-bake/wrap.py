"""Wrap a foreign .cube into a legal LMT for the scene-referred camera.

The problem: a look LUT you were handed is almost always made for a **Rec.709 display image** -
it expects sRGB-encoded display code values in and gives them out. The camera's LMT slot sits in
the middle of a scene-referred chain and hands the LUT **ACEScct**. Both are three floats in
[0,1], so the wrong one loads without complaint and produces a wrong picture silently.

The fix is the recipe everybody uses (ACES's own guidance for reusing a legacy show LUT;
Baselight's and Resolve's inverse-ODT node sandwiches; Nuke's OCIODisplay invert; OCIO's
LookTransform with a display process_space):

    forward output transform  ->  the foreign LUT  ->  inverse output transform

with one modification of ours - it is applied as a RESIDUAL:

    W(x) = x + [ inv(LUT(fwd(x))) - inv(fwd(x)) ]

because inv(fwd(x)) is not the identity (an SDR output transform folds the out-of-gamut colours
inwards and clips above display white, and neither can be undone), and this project's rule is
that a neutral is an EXACT identity. With the residual form the two inv() evaluations coincide
for an identity LUT and cancel bit for bit. See decisions/s10-grade-component.md 4.2.

The CinematicCamera plugin does exactly this at load time, on the CPU, with a sampled inverse
cube. This tool does the same thing with OCIO's exact inverse processor at a bigger grid, so a
shipping look need not be wrapped again at every load.

Usage:
    uv run wrap.py --lut mylook.cube                       # tag decides, or --space
    uv run wrap.py --lut mylook.cube --space rec709 --size 65
    uv run wrap.py --lut mylook.cube --method sandwich      # the plain recipe, for comparison
    uv run wrap.py --verify-identity                        # the acceptance instrument
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import os
import sys

import numpy as np
import PyOpenColorIO as ocio

import acescct
from bake import (DEFAULT_OUTDIR, HERE, RNG_SEED, REPORT_SAMPLES, apply_cpu, die, get_cpu,
                  git_hash, load_config, make_grid, stats, tool_digest, trilinear, write_cube,
                  write_report, assert_aces_2)

# The output transform a Rec.709 look LUT was authored against. Fixed, not configurable: it is a
# property of the LUT (the monitor it was made on), not of the camera that will play it.
WRAP_CONFIG = "ocio://studio-config-latest"
WRAP_DISPLAY = "sRGB - Display"
WRAP_VIEW = "ACES 2.0 - SDR 100 nits (Rec.709)"
WRAP_SPACE = "ACEScct"

TAG_KEY = "rec-lut-space:"

SPACE_ALIASES = {
    "acescct": "ACEScct",
    "acescg": "ACEScg", "linear": "ACEScg",
    "rec709": "Rec709", "rec.709": "Rec709", "rec709display": "Rec709",
    "srgb": "Rec709", "srgbdisplay": "Rec709", "display": "Rec709",
}


# --------------------------------------------------------------------------------------------
def read_cube(path: str):
    """Minimal Resolve-dialect .cube reader, with the same refusals the plugin's parser has.

    Returns (size, data (N^3,3) red-fastest, tag or None, title).
    """
    size = 0
    tag = None
    title = ""
    dmin = [0.0, 0.0, 0.0]
    dmax = [1.0, 1.0, 1.0]
    vals = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("#"):
                low = line[1:].strip().lower()
                if low.startswith(TAG_KEY):
                    tag = line[1:].strip()[len(TAG_KEY):].strip()
                continue
            up = line.upper()
            if up.startswith("TITLE"):
                title = line[5:].strip().strip('"')
                continue
            if up.startswith("LUT_1D_SIZE"):
                die("%s is a 1D LUT; the LMT slot wants a 3D cube" % path)
            if up.startswith("LUT_3D_SIZE"):
                size = int(line.split()[1])
                if size < 2 or size > 129:
                    die("%s: LUT_3D_SIZE %d out of range (2..129)" % (path, size))
                continue
            if up.startswith("DOMAIN_MIN"):
                dmin = [float(x) for x in line.split()[1:4]]
                continue
            if up.startswith("DOMAIN_MAX"):
                dmax = [float(x) for x in line.split()[1:4]]
                continue
            parts = line.split()
            if len(parts) != 3:
                die("%s: unparsable line %r" % (path, line[:32]))
            vals.append([float(parts[0]), float(parts[1]), float(parts[2])])

    if size == 0:
        die("%s: no LUT_3D_SIZE" % path)
    if len(vals) != size ** 3:
        die("%s: %d samples for a %d^3 grid" % (path, len(vals), size))
    if any(abs(v) > 1e-6 for v in dmin) or any(abs(v - 1.0) > 1e-6 for v in dmax):
        die("%s: DOMAIN %s..%s -- this chain feeds a LUT [0,1]; re-bake on a unit domain"
            % (path, dmin, dmax))
    return size, np.asarray(vals, dtype=np.float64), tag, title


def resolve_space(tag: str | None, override: str | None, path: str) -> str:
    """The tag wins when both are present; a disagreement is said out loud. Same rule the plugin
    applies (decisions/s10-grade-component.md 4.1)."""
    tag_space = SPACE_ALIASES.get((tag or "").strip().lower()) if tag else None
    if tag and tag_space is None:
        print("WARNING: %s carries an unrecognised ReC-LUT-Space %r; ignoring it." % (path, tag))
    ov = SPACE_ALIASES.get((override or "").strip().lower()) if override else None
    if override and ov is None:
        die("--space %r is not one of acescct / acescg / rec709" % override)

    if tag_space and ov and tag_space != ov:
        print("WARNING: %s says ReC-LUT-Space: %s but --space says %s. The TAG wins."
              % (path, tag_space, ov))
    if tag_space:
        return tag_space
    if ov:
        return ov
    print("NOTE: %s carries no ReC-LUT-Space tag and no --space was given; assuming ACEScct "
          "(= no wrapping, the file is already an LMT)." % path)
    return "ACEScct"


# --------------------------------------------------------------------------------------------
def wrap_rec709(lut: np.ndarray, size: int, grid: np.ndarray, fwd, inv, method: str) -> np.ndarray:
    """forward ODT -> the LUT -> inverse ODT, over an ACEScct grid. `fwd`/`inv` are OCIO CPU
    processors (this tool) or callables wrapping a sampled cube (what the plugin does)."""
    display = np.clip(fwd(grid), 0.0, 1.0)
    looked = trilinear(lut, size, display)
    sandwich = inv(looked)
    if method == "sandwich":
        return sandwich
    return grid + (sandwich - inv(display))


def wrap_acescg(lut: np.ndarray, size: int, grid: np.ndarray, method: str) -> np.ndarray:
    """The same construction with the analytic shaper instead of an output transform. A unit-domain
    LINEAR cube covers scene linear [0,1] and nothing above, while ACEScctToLinear(1.0) = 222.86;
    the residual form is what keeps everything above 1.0 moving instead of clamping."""
    lin = np.clip(acescct.acescct_to_linear(grid), 0.0, 1.0)
    looked = trilinear(lut, size, lin)
    sandwich = acescct.linear_to_acescct(looked)
    if method == "sandwich":
        return sandwich
    return grid + (sandwich - acescct.linear_to_acescct(lin))


# --------------------------------------------------------------------------------------------
def make_processors():
    cfg = load_config(WRAP_CONFIG)
    assert_aces_2(cfg, WRAP_CONFIG)
    fwd_cpu = get_cpu(cfg, WRAP_SPACE, WRAP_DISPLAY, WRAP_VIEW)
    dvt = ocio.DisplayViewTransform(src=WRAP_SPACE, display=WRAP_DISPLAY, view=WRAP_VIEW)
    inv_cpu = cfg.getProcessor(dvt, ocio.TRANSFORM_DIR_INVERSE).getOptimizedCPUProcessor(
        ocio.BIT_DEPTH_F32, ocio.BIT_DEPTH_F32, ocio.OPTIMIZATION_LOSSLESS)
    return cfg, (lambda a: apply_cpu(fwd_cpu, a).astype(np.float64)), \
        (lambda a: apply_cpu(inv_cpu, a).astype(np.float64))


def sampled_processors(fwd_cube: str, inv_cube: str):
    """The plugin's own pair: two sampled cubes and trilinear interpolation, so `--verify-identity`
    measures what the ENGINE will do and not only what OCIO would do."""
    fsize, fdata, _, _ = read_cube(fwd_cube)
    isize, idata, _, _ = read_cube(inv_cube)
    return (lambda a: trilinear(fdata, fsize, a)), (lambda a: trilinear(idata, isize, a))


def file_sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# --------------------------------------------------------------------------------------------
def cmd_wrap(args) -> int:
    size_in, lut, tag, title = read_cube(args.lut)
    space = resolve_space(tag, args.space, args.lut)
    # 65 by default whatever the source grid was, and for the same reason the plugin uses 65: the
    # wrapped LUT carries the OUTPUT TRANSFORM's curvature as well as the look's, and the ODT is
    # much the steeper of the two. Measured, 33^3 vs 65^3 on a third-party-style 709 look LUT:
    # 0.87 vs 0.34 8-bit code values of mean error against applying the LUT to the ODT output.
    out_size = args.size or 65
    grid = make_grid(out_size).astype(np.float64)

    if space == "ACEScct":
        print("%s is already ACEScct; there is nothing to wrap." % args.lut)
        return 0

    if space == "Rec709":
        cfg, fwd, inv = make_processors()
        data = wrap_rec709(lut, size_in, grid, fwd, inv, args.method)
        how = ("forward %s / %s  ->  the LUT  ->  the exact OCIO inverse of the same"
               % (WRAP_DISPLAY, WRAP_VIEW))
        cfg_name = cfg.getName()
    else:
        data = wrap_acescg(lut, size_in, grid, args.method)
        how = "ACEScct -> linear (analytic shaper)  ->  the LUT  ->  linear -> ACEScct"
        cfg_name = "(none -- analytic shaper)"

    meta = {
        "tool": "wrap.py",
        "preset": "wrap of %s" % os.path.basename(args.lut),
        "config_uri": WRAP_CONFIG if space == "Rec709" else "(none)",
        "config_name": cfg_name,
        "aces_version": "2.0" if space == "Rec709" else "(n/a)",
        "input_space": "ACEScct",
        "display": WRAP_DISPLAY if space == "Rec709" else "(none)",
        "view": WRAP_VIEW if space == "Rec709" else "(none)",
        "output_encoding": "ACEScct (a look: same encoding in and out)",
        "rec_lut_space": "ACEScct",
        "size": out_size,
        "ocio_version": ocio.__version__,
        "utc": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git": git_hash(),
        "digest": tool_digest(),
        "notes": [
            "WRAPPED LOOK -- this file is a derivative, not an original.",
            "",
            "  source file     : %s" % os.path.basename(args.lut),
            "  source sha256   : %s" % file_sha256(args.lut),
            "  source grid     : %d^3" % size_in,
            "  source space    : %s%s" % (space, "  (from the file's own tag)" if tag else
                                          "  (declared with --space; the file carried no tag)"),
            "  method          : %s" % ("residual: W(x) = x + [inv(LUT(fwd(x))) - inv(fwd(x))]"
                                        if args.method == "residual" else
                                        "plain sandwich: W(x) = inv(LUT(fwd(x)))"),
            "  transform chain : %s" % how,
            "",
            "The inverse of an SDR output transform is not exact: colour outside the display",
            "gamut is folded inwards on the way out and cannot be unfolded on the way back, and",
            "everything above display white shares one code value. The residual method makes an",
            "IDENTITY source an exact identity anyway, and keeps highlight separation above",
            "display white (which matters because the EXR export tap is taken after the LMT).",
        ],
    }

    outdir = args.outdir
    os.makedirs(outdir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(args.lut))[0]
    out_name = args.out or ("%s_lmt_%d.cube" % (stem, out_size))
    cube = os.path.join(outdir, out_name)
    write_cube(cube, "%s (wrapped to ACEScct)" % (title or stem), out_size, data, meta)
    print("  wrote %s  (%d^3, %s -> ACEScct, %s)" % (cube, out_size, space, args.method))

    # How far is the wrapped grid from wrapping evaluated exactly at each sample?
    rng = np.random.default_rng(RNG_SEED)
    samples = rng.random((REPORT_SAMPLES, 3), dtype=np.float32).astype(np.float64)
    if space == "Rec709":
        exact = wrap_rec709(lut, size_in, samples, fwd, inv, args.method)
    else:
        exact = wrap_acescg(lut, size_in, samples, args.method)
    blocks = [("%d^3 wrapped grid, trilinear vs the wrap evaluated exactly "
               "(errors in ACEScct code value; 1.0 / 17.52 = one stop)" % out_size,
               stats(trilinear(data, out_size, samples) - exact))]
    report = os.path.join(outdir, os.path.splitext(out_name)[0] + ".report.txt")
    write_report(report, meta, blocks)
    print("  wrote %s" % report)
    for label, s in blocks:
        print("    %-78s max %9.6f  mean %9.7f" % (label, s["max"], s["mean"]))
    return 0


def cmd_verify_identity(args) -> int:
    """THE ACCEPTANCE INSTRUMENT (the brief's own words): wrapping an IDENTITY Rec.709 LUT must
    give a near-identity LMT. Run for both methods and for both evaluators - OCIO's exact
    processors (what wrap.py uses) and the two sampled cubes (what the plugin uses at load).
    """
    fwd_cube = args.forward or os.path.join(HERE, "out", "odt_srgb_100nit_aces2_65.cube")
    inv_cube = args.inverse or os.path.join(HERE, "out", "odt_srgb_100nit_aces2_inv_65.cube")
    for p in (fwd_cube, inv_cube):
        if not os.path.exists(p):
            die("missing %s -- run `uv run bake.py --preset odt_srgb_100nit "
                "--preset odt_srgb_100nit_inv` first" % p)

    print("identity-wrap check")
    print("  forward cube : %s" % fwd_cube)
    print("  inverse cube : %s" % inv_cube)
    print()

    for size in (33, 65):
        grid = make_grid(size).astype(np.float64)
        ident = grid.copy()   # an identity Rec.709 LUT on the same grid

        for evaluator in ("plugin (two sampled cubes, trilinear)", "wrap.py (exact OCIO)"):
            if evaluator.startswith("plugin"):
                fwd, inv = sampled_processors(fwd_cube, inv_cube)
            else:
                _, fwd, inv = make_processors()
            for method in ("residual", "sandwich"):
                out = wrap_rec709(ident, size, grid, fwd, inv, method)
                err = np.abs(out - grid)
                print("  %d^3  %-38s  %-9s  max %.9f  mean %.9f"
                      % (size, evaluator, method, err.max(), err.mean()))

        # the ACEScg path, whose shaper is analytic on both sides
        for method in ("residual", "sandwich"):
            out = wrap_acescg(ident, size, grid, method)
            err = np.abs(out - grid)
            print("  %d^3  %-38s  %-9s  max %.9f  mean %.9f"
                  % (size, "ACEScg (analytic shaper)", method, err.max(), err.mean()))
        print()

    print("Reading: the residual rows must be EXACTLY zero - the two inv() evaluations land on")
    print("the same coordinate for an identity source and cancel bit for bit. The sandwich rows")
    print("are what the plain, documented recipe costs, and are the reason for the residual.")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lut", help="the foreign .cube to wrap")
    ap.add_argument("--space", default=None,
                    help="its space, when the file carries no ReC-LUT-Space tag: "
                         "acescct / acescg / rec709")
    ap.add_argument("--size", type=int, default=None, help="grid size of the wrapped result")
    ap.add_argument("--method", choices=("residual", "sandwich"), default="residual")
    ap.add_argument("--out", default=None, help="output file name (default: <stem>_lmt_<N>.cube)")
    ap.add_argument("--outdir", default=DEFAULT_OUTDIR)
    ap.add_argument("--verify-identity", action="store_true",
                    help="measure the identity-wrap error for every method and evaluator")
    ap.add_argument("--forward", default=None, help="forward ODT cube for --verify-identity")
    ap.add_argument("--inverse", default=None, help="inverse ODT cube for --verify-identity")
    args = ap.parse_args(argv)

    print("PyOpenColorIO %s" % ocio.__version__)
    if args.verify_identity:
        return cmd_verify_identity(args)
    if not args.lut:
        ap.error("give --lut FILE or --verify-identity")
    return cmd_wrap(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
