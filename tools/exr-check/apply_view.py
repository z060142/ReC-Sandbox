"""Render a scene-referred EXR through the same ACES display/view the engine baked its ODT from.

This is procedure A of the S6 acceptance test: it removes DaVinci Resolve from the loop entirely,
so that the only differences left between the engine's TGA and this PNG are LUT interpolation, the
shaper clamp, 10 -> 8 bit truncation and the engine's dither. If A fails, procedure B (Resolve)
cannot succeed and Resolve is not the suspect.

The reference is computed by SOMEBODY ELSE'S implementation of the transform, from a file that
left the process. That is the whole point - an in-engine "ODT diff" mode would compare the engine
against itself.

    uv run apply_view.py capture/exr --out capture/ocio_png
    uv run apply_view.py capture/exr --out capture/ocio_png --compare capture/tga

What it refuses to do, and why each refusal matters:

  * run on a file whose `chromaticities` attribute disagrees with the colour space it is about to
    assume. Getting that wrong is exactly the error class this test exists to catch, and it fails
    silently on any frame without saturated colour.
  * run on a NEGATIVE tap (`rec/exportTap` == "HDRTarget"). That file has no white balance, no CDL,
    no LMT, no bloom composite and no vignette, so no output transform applied to it can match the
    engine view. Comparing one is not a failed test, it is a meaningless one.
  * silently accept a different OCIO config from the one the .cube was baked with. A quietly
    upgraded wheel moving the reference is a genuinely nasty half-day.
"""

from __future__ import annotations

import argparse
import os
import sys

import numpy as np

try:
    import PyOpenColorIO as ocio
except ImportError:  # pragma: no cover
    sys.exit("PyOpenColorIO is not installed - run this through `uv run`.")

try:
    import OpenEXR
except ImportError:  # pragma: no cover
    sys.exit("The OpenEXR python bindings are not installed - run this through `uv run`.")

from PIL import Image

# AP1 (ACEScg) and AP0 (ACES2065-1), CIE 1931 xy, in the order the EXR attribute stores them.
CHROMATICITIES = {
    "ACEScg": (0.713, 0.293, 0.165, 0.830, 0.128, 0.044, 0.32168, 0.33767),
    "ACES2065-1": (0.7347, 0.2653, 0.0, 1.0, 0.0001, -0.0770, 0.32168, 0.33767),
}

DEFAULT_CONFIG = "ocio://studio-config-latest"
DEFAULT_DISPLAY = "sRGB - Display"
DEFAULT_VIEW = "ACES 2.0 - SDR 100 nits (Rec.709)"

# The tolerance the acceptance note budgets for procedure A, in 8-bit code values.
PASS_MEAN = 0.15
PASS_MAX = 2.0


def load_config(uri: str) -> "ocio.Config":
    if uri.startswith("ocio://"):
        return ocio.Config.CreateFromBuiltinConfig(uri[len("ocio://"):])
    return ocio.Config.CreateFromFile(uri)


def read_exr(path: str):
    """Return (rgb float32 HxWx3, header dict)."""
    with OpenEXR.File(path) as f:
        header = dict(f.header())
        channels = f.channels()
        planes = [np.asarray(channels[c].pixels, dtype=np.float32) for c in ("R", "G", "B")]
    return np.ascontiguousarray(np.stack(planes, axis=-1)), header


def source_space(header: dict, path: str) -> str:
    """Decide what the file holds, from the file itself - never from a command-line flag."""
    tap = str(header.get("rec/exportTap", "")).strip()
    if tap and tap != "pre-ODT":
        raise SystemExit(
            f"{path}: rec/exportTap is '{tap}'. That is the camera negative - it has no white "
            "balance, no CDL, no LMT and no composites, so no output transform applied to it can "
            "match the engine view. Capture with r_SceneReferredExportTap 0."
        )

    chroma = header.get("chromaticities")
    if chroma is None:
        raise SystemExit(f"{path}: no `chromaticities` attribute - refusing to guess the primaries.")

    values = tuple(float(v) for v in np.asarray(chroma).reshape(-1))
    for name, reference in CHROMATICITIES.items():
        if np.allclose(values, reference, atol=1e-4):
            declared = str(header.get("rec/workingSpace", name)).strip() or name
            if declared != name:
                raise SystemExit(
                    f"{path}: chromaticities say {name} but rec/workingSpace says '{declared}'. "
                    "One of the two is a lie and the picture cannot tell you which."
                )
            return name

    raise SystemExit(f"{path}: chromaticities {values} match neither AP1 nor AP0.")


def render(path: str, cfg: "ocio.Config", display: str, view: str) -> np.ndarray:
    rgb, header = read_exr(path)
    src = source_space(header, path)

    dvt = ocio.DisplayViewTransform(src=src, display=display, view=view)
    proc = cfg.getProcessor(dvt).getDefaultCPUProcessor()
    proc.applyRGB(rgb)

    return np.clip(np.rint(rgb * 255.0), 0, 255).astype(np.uint8)


def compare(png_dir: str, tga_dir: str) -> int:
    """Report per-file statistics against the engine's own capture of the same frames."""
    from PIL import Image as _Image

    worst_mean = 0.0
    worst_max = 0.0
    pairs = 0

    for name in sorted(os.listdir(png_dir)):
        stem = os.path.splitext(name)[0]
        candidates = [f for f in os.listdir(tga_dir) if os.path.splitext(f)[0] == stem]
        if not candidates:
            print(f"  {stem}: no engine capture to compare against")
            continue

        a = np.asarray(_Image.open(os.path.join(png_dir, name)).convert("RGB"), dtype=np.int16)
        b = np.asarray(_Image.open(os.path.join(tga_dir, candidates[0])).convert("RGB"), dtype=np.int16)
        if a.shape != b.shape:
            print(f"  {stem}: size mismatch {a.shape} vs {b.shape}")
            continue

        d = np.abs(a - b)
        mean, mx = float(d.mean()), float(d.max())
        worst_mean, worst_max = max(worst_mean, mean), max(worst_max, mx)
        pairs += 1
        print(f"  {stem}: mean |d| {mean:.3f}  max |d| {mx:.0f}  differing {int((d > 0).any(axis=-1).sum())} px")

    if not pairs:
        return 2

    verdict = "PASS" if (worst_mean < PASS_MEAN and worst_max <= PASS_MAX) else "INVESTIGATE"
    print(f"\n{verdict}: worst mean |d| {worst_mean:.3f} (budget {PASS_MEAN}), "
          f"worst max |d| {worst_max:.0f} (budget {PASS_MAX})")
    print("The SHAPE of a failure names its cause - see scene-notes/research/s6-acceptance-roundtrip.md "
          "section 5. A uniform offset is data levels or exposure; error growing with luminance is a "
          "wrong input transform; error only in saturated colour is the shaper's negative clamp.")
    return 0 if verdict == "PASS" else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exr_dir", help="directory of EXRs written by rec_CaptureEXR")
    ap.add_argument("--out", required=True, help="directory to write the reference PNGs into")
    ap.add_argument("--compare", help="directory of engine TGA captures of the same frames")
    ap.add_argument("--config", default=DEFAULT_CONFIG)
    ap.add_argument("--display", default=DEFAULT_DISPLAY)
    ap.add_argument("--view", default=DEFAULT_VIEW)
    args = ap.parse_args()

    cfg = load_config(args.config)
    if args.display not in cfg.getDisplays():
        return int(sys.exit(f"display '{args.display}' is not in this config: {list(cfg.getDisplays())}") or 2)
    if args.view not in cfg.getViews(args.display):
        return int(sys.exit(f"view '{args.view}' is not on that display: {list(cfg.getViews(args.display))}") or 2)

    os.makedirs(args.out, exist_ok=True)
    count = 0
    for name in sorted(os.listdir(args.exr_dir)):
        if not name.lower().endswith(".exr"):
            continue
        png = render(os.path.join(args.exr_dir, name), cfg, args.display, args.view)
        Image.fromarray(png).save(os.path.join(args.out, os.path.splitext(name)[0] + ".png"))
        count += 1

    print(f"rendered {count} frame(s) through {args.display} / {args.view}")

    if args.compare:
        print("\ncomparing against the engine's own capture:")
        return compare(args.out, args.compare)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
