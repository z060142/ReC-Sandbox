# /// script
# requires-python = ">=3.9"
# dependencies = ["pillow", "numpy"]
# ///
"""Byte-identity comparison of two directories of engine captures.

Part of the scene-referred byte-identity harness (SceneReferredSpec.md stage S0,
scene-notes/decisions/s0-byte-identity-harness.md). The procedure that produces the two
directories is in README-bitcompare.md.

    uv run bitcompare.py <dir_A> <dir_B> [--out <dir>] [--amplify N] [--quiet]

What it does, per file, in this order:

 1. sha256 of the raw bytes. Equal -> identical, and nothing is decoded. This is the whole
    acceptance criterion for "the switch is off and the picture is unchanged".
 2. Only if the hashes differ: decode both, report max and mean absolute difference per
    channel and the number of differing pixels, and write an amplified difference image as a
    PNG so that "one pixel off by one" is distinguishable from "the whole frame moved".

Files present on one side only, and files that differ in size, are reported and counted.

VisRegTest writes its captures as "<map>_<n>.bmp" but the contents are TGA: the engine looks
the extension up in SCaptureFormatInfo::GetCaptureFormatByExtension(), "bmp" is not one of
tga/jpg/png, and the fallback is TGA. So the extension is not to be trusted and every file is
decoded by content, with the TGA decoder as the explicit fallback.

Exit code 0 = every file byte-identical, 1 = anything else, 2 = usage/IO error.
"""

import argparse
import hashlib
import sys
from pathlib import Path

import numpy as np
from PIL import Image, TgaImagePlugin

IMAGE_SUFFIXES = {".tga", ".bmp", ".png", ".jpg", ".jpeg", ""}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_image(path: Path) -> np.ndarray:
    """Decode by content, not by name. Returns HxWx3 uint8 (alpha dropped: the engine's
    capture path writes three eight-bit channels)."""
    try:
        img = Image.open(path)
        img.load()
    except Exception:
        # The .bmp-named TGA case, and anything else Pillow will not sniff.
        fh = path.open("rb")
        img = TgaImagePlugin.TgaImageFile(fh)
        img.load()
    return np.asarray(img.convert("RGB"), dtype=np.uint8)


def collect(root: Path) -> dict:
    files = {}
    for p in sorted(root.rglob("*")):
        if p.is_file() and p.suffix.lower() in IMAGE_SUFFIXES:
            files[p.relative_to(root).as_posix()] = p
    return files


def compare_pair(a: Path, b: Path, rel: str, out_dir: Path, amplify: int, quiet: bool) -> bool:
    """True when the two files are byte-identical."""
    ha, hb = sha256(a), sha256(b)
    if ha == hb:
        if not quiet:
            print("  IDENTICAL  %s  (%s)" % (rel, ha[:16]))
        return True

    print("  DIFFERS    %s" % rel)
    print("             A sha256 %s  (%d bytes)" % (ha[:32], a.stat().st_size))
    print("             B sha256 %s  (%d bytes)" % (hb[:32], b.stat().st_size))

    try:
        ia, ib = load_image(a), load_image(b)
    except Exception as exc:
        print("             could not decode for a pixel diff: %s" % exc)
        return False

    if ia.shape != ib.shape:
        print("             DIFFERENT SIZE: %s vs %s - the two runs are not comparable "
              "(resolution, sys_spec or supersampling differ)." % (ia.shape, ib.shape))
        return False

    diff = np.abs(ia.astype(np.int16) - ib.astype(np.int16))
    differing = int(np.count_nonzero(diff.any(axis=2)))
    total = diff.shape[0] * diff.shape[1]
    print("             pixels differing: %d / %d (%.4f%%)" % (differing, total, 100.0 * differing / total))
    for i, ch in enumerate("RGB"):
        print("             %s  max |d| %3d   mean |d| %.5f" % (ch, int(diff[..., i].max()), float(diff[..., i].mean())))

    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / (rel.replace("/", "_") + ".diff.png")
    Image.fromarray(np.clip(diff * amplify, 0, 255).astype(np.uint8)).save(out_path)
    print("             diff image (x%d): %s" % (amplify, out_path))
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description="Byte-compare two directories of engine captures.")
    ap.add_argument("dir_a", type=Path)
    ap.add_argument("dir_b", type=Path)
    ap.add_argument("--out", type=Path, default=None,
                    help="where difference PNGs go (default: <dir_b>/_bitcompare_diff)")
    ap.add_argument("--amplify", type=int, default=16,
                    help="multiplier applied to the difference image so small errors are visible (default 16)")
    ap.add_argument("--quiet", action="store_true", help="only print files that differ")
    args = ap.parse_args()

    for d in (args.dir_a, args.dir_b):
        if not d.is_dir():
            print("not a directory: %s" % d, file=sys.stderr)
            return 2

    out_dir = args.out if args.out is not None else args.dir_b / "_bitcompare_diff"

    fa, fb = collect(args.dir_a), collect(args.dir_b)
    only_a = sorted(set(fa) - set(fb))
    only_b = sorted(set(fb) - set(fa))
    common = sorted(set(fa) & set(fb))

    print("A = %s   (%d images)" % (args.dir_a, len(fa)))
    print("B = %s   (%d images)" % (args.dir_b, len(fb)))
    print("")

    identical = 0
    for rel in common:
        if compare_pair(fa[rel], fb[rel], rel, out_dir, args.amplify, args.quiet):
            identical += 1

    if only_a or only_b:
        print("")
        for rel in only_a:
            print("  ONLY IN A  %s" % rel)
        for rel in only_b:
            print("  ONLY IN B  %s" % rel)

    print("")
    print("%d/%d compared images byte-identical; %d only in A, %d only in B."
          % (identical, len(common), len(only_a), len(only_b)))

    ok = identical == len(common) and not only_a and not only_b and common
    print("RESULT: %s" % ("BYTE-IDENTICAL" if ok else "DIFFERENT"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
