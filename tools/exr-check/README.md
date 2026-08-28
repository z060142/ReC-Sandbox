# exr-check — procedure A of the S6 acceptance test

Renders an EXR written by `rec_CaptureEXR` through the **same** ACES 2.0 display/view the shipped
ODT `.cube` was baked from, using PyOpenColorIO, and compares the result with the engine's own
capture of the same frames.

This is the tight, daily check. It removes DaVinci Resolve from the loop, so the only differences
left are LUT interpolation, the ACEScct shaper's clamp, 10→8-bit truncation and the engine's
dither. If it fails, procedure B (Resolve) cannot succeed and Resolve is not the suspect.

The reference is computed by **someone else's** implementation of the transform, from a file that
left the process — that is the entire point. An in-engine "ODT diff" mode would compare the engine
against itself.

## Running

```
uv run apply_view.py capture/exr --out capture/ocio_png
uv run apply_view.py capture/exr --out capture/ocio_png --compare capture/tga
```

`--compare` prints per-file mean and max absolute difference in 8-bit code values, and a verdict
against the budget in `scene-notes/research/s6-acceptance-roundtrip.md` §5 (mean < 0.15,
max ≤ 2). For a byte-level diff and an amplified difference image, run `../bitcompare.py` on the
same two directories — it is the more informative tool when something is actually wrong, and its
"pass" state is byte-identity, which this comparison can never reach.

## Capture conditions

The two files must be the **same rendered frame**. Before capturing:

```
r_displayInfo 0
r_AntialiasingMode 0
r_HDRGrainAmount 0
r_Sharpening 0
r_HDRDithering 0
r_HDRDebug 0
```

and lock the exposure (ccam Manual, or AUTO locked). An unlocked AUTO exposure produces a mismatch
that looks exactly like a transform bug.

## What it refuses to do

* Run on a file whose `chromaticities` disagree with `rec/workingSpace`. One of the two is wrong
  and the picture cannot tell you which.
* Run on a **negative** tap (`rec/exportTap` = `HDRTarget`). That file has no white balance, no
  CDL, no LMT and no composites, so no output transform applied to it can match the engine view.
  Comparing one is not a failed test, it is a meaningless one.
* Guess the primaries when the file carries no `chromaticities` at all.
