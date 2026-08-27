# `tools/ocio-bake` — baking the ACES 2.0 output transform

Offline tool that produces the `.cube` display-transform files the engine loads. No engine code
here. Design note: `scene-notes/research/s4-ocio-bake.md`; decision: `scene-notes/decisions/
s4-display-transform.md` §4.

We do not invent a display transform. OCIO's built-in ACES configs *are* the transform; this tool
samples one and writes it out as data, with enough provenance in the file that any shipped LUT can
be traced back to the exact config, view and tool revision that produced it.

## Requirements

Python is managed by **uv** only — no global installs.

```
uv sync          # creates .venv and installs the pinned dependencies
```

`.python-version` pins **CPython 3.12**. The `opencolorio` wheel had no CPython 3.14 build at the
time of writing and the machine's default interpreter is 3.14, so the pin is load-bearing; uv
downloads 3.12 by itself. Resolved versions: `opencolorio 2.5.2`, `numpy 2.5.2`.

## Usage

```
bake.bat                            # uv sync + bake every preset into out\
uv run bake.py --all
uv run bake.py --preset odt_srgb_100nit
uv run bake.py --all --size 65      # override the grid size for one run
uv run bake.py --list
uv run bake.py --all --also-copy <dir>      # also copy the shipped files somewhere

uv run list_config.py                       # ocio://studio-config-latest
uv run list_config.py --builtins            # every registered built-in config
uv run list_config.py "ocio://cg-config-latest" /path/to/config.ocio

uv run verify_matrices.py                   # matrices, luminance weights, ACEScct constants
```

`out/` is generated and git-ignored. The shipped files are committed in the engine plugin's asset
tree (see "Shipped files").

## What the config actually ships (run of 2026-08-28, OCIO 2.5.2)

`list_config.py --builtins` on this machine lists:

```
ocio://cg-config-v1.0.0_aces-v1.3_ocio-v2.1
ocio://cg-config-v2.1.0_aces-v1.3_ocio-v2.3
ocio://cg-config-v2.2.0_aces-v1.3_ocio-v2.4
ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5        [recommended, default]
ocio://studio-config-v1.0.0_aces-v1.3_ocio-v2.1
ocio://studio-config-v2.1.0_aces-v1.3_ocio-v2.3
ocio://studio-config-v2.2.0_aces-v1.3_ocio-v2.4
ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5    [recommended]
```

`ocio://studio-config-latest` resolves to **`studio-config-v4.0.0_aces-v2.0_ocio-v2.5`**. The
research note guessed `studio-config-v3.0.0_aces-v2.0_ocio-v2.4`; the real ACES 2.0 configs are
`v4.0.0` and require OCIO **2.5**, not 2.4. `pyproject.toml` still asks for `opencolorio>=2.4`
because that is the floor stated in the spec; what is actually installed and required for the ACES
2.0 built-in configs is 2.5.

### The view names (this run settles the research note's `[VERIFY]`)

The SDR view name the note guessed turned out to be exactly right:

| display | view (exact string) |
|---|---|
| `sRGB - Display` | `ACES 2.0 - SDR 100 nits (Rec.709)` |
| `Rec.1886 Rec.709 - Display` | `ACES 2.0 - SDR 100 nits (Rec.709)` |
| `Gamma 2.2 Rec.709 - Display` | `ACES 2.0 - SDR 100 nits (Rec.709)` |
| `Display P3 - Display` | `ACES 2.0 - SDR 100 nits (P3 D65)` |
| `Rec.2100-PQ - Display` | `ACES 2.0 - HDR {500,1000,2000,4000} nits ({P3 D65,Rec.2020})` |
| `ST2084-P3-D65 - Display` | `ACES 2.0 - HDR {108,500,1000,2000,4000} nits (P3 D65)` |

Every display additionally carries `Un-tone-mapped`, `Video (colorimetric)` and `Raw`. The full
display list is `sRGB - Display`, `Gamma 2.2 Rec.709 - Display`, `Display P3 - Display`,
`Display P3 HDR - Display`, `P3-D65 - Display`, `Rec.1886 Rec.709 - Display`,
`Rec.2100-HLG - Display`, `Rec.2100-PQ - Display`, `ST2084-P3-D65 - Display`.

Two corrections to the research note's guesses: there is no `Display P3 - Display` HDR view (HDR on
P3 lives on `Display P3 HDR - Display`), and the config gained a `Video (colorimetric)` view that
did not exist in the ACES 1.3 configs.

The scene-linear Rec.709 space is named **`Linear Rec.709 (sRGB)`**; roles are `scene_linear ->
ACEScg`, `color_timing -> ACEScct`, `compositing_log -> ACEScct`, `aces_interchange ->
ACES2065-1`, exactly as the note assumed.

## Verified constants

Produced by `uv run verify_matrices.py` against `ocio://studio-config-latest`. These are *exact*
double-precision values read out of OCIO's own operator parameters (`MatrixTransform.getMatrix()`,
`LogCameraTransform.get*Value()`), not sampled through a float32 processor — a float32 sample of
the same transform agrees to 2.7e-8, and OCIO's own inverse agrees with the numeric inverse to
2.2e-16.

### Rec.709 linear (sRGB primaries, D65) → ACEScg (AP1, D60), Bradford

```
 0.61309740   0.33952315   0.04737945
 0.07019372   0.91635388   0.01345240
 0.02061559   0.10956977   0.86981463
```

Row sums are 1.00000000 to 8 decimals; `(1,1,1) -> (1,1,1)`; every entry non-negative.

The chromatic adaptation is **Bradford**, confirmed numerically: rebuilding the matrix from the
published primaries with a Bradford CAT reproduces OCIO's to 2.4e-15, while omitting the
adaptation entirely is off by 7.1e-2.

### ACEScg → Rec.709 linear (the inverse)

```
 1.70505099  -0.62179212  -0.08325887
-0.13025642   1.14080474  -0.01054832
-0.02400336  -0.12896898   1.15297233
```

### AP1 → CIE XYZ (D60)

```
 0.66245418   0.13400421   0.15618769
 0.27222872   0.67408177   0.05368952
-0.00557465   0.00406073   1.01033910
```

Derived from the ACES primaries and cross-checked against OCIO's exact AP0↔AP1 matrix — a
comparison that involves no chromatic adaptation at all, since both are ACES white — to 6.7e-16.
(No OCIO colour space exposes an unadapted D60 XYZ directly; the config's only XYZ interchange
space is D65.)

### AP1 luminance weights (the Y row)

```
0.27222872   0.67408177   0.05368952        (sum = 1.0000000000)
```

### ACEScct encode / decode

```
X_BRK      =  0.0078125000        (2^-7)
Y_BRK      =  0.1552511416
A          = 10.5402377417        (linear-segment slope)
B          =  0.0729055342        (linear-segment offset)
LOG_OFFSET =  9.7200000000
LOG_SCALE  = 17.5200000000
```

```
x <  X_BRK :  y = A*x + B
x >= X_BRK :  y = (log2(x) + 9.72) / 17.52
```

Evaluated properties:

```
encode(0.18)  = 0.41358781      mid grey
-B/A          = -0.00691688     the most negative linear value the encode still represents
decode(1.0)   = 222.86108398    linear at the top of the LUT domain
decode(1.468) = 65506.82        the top of the ACEScct codomain
```

**`decode(1.0) = 222.86`, not ~65504.** The research note's §3 claim that ACEScct 0..1 spans the
whole fp16 range is wrong; the tool's clamp of the shaper output to `[0,1]` therefore clips at
about **223× reference white (≈ +10.3 stops over mid grey)**, not at the fp16 ceiling. That is
still far above anything the ODT resolves, but it is a real ceiling and the note has been
corrected.

### Agreement with `scene-notes/research/s3-matrices-and-white-point.md`

| value | verdict |
|---|---|
| AP1 → XYZ (D60) | matches to 4.9e-8 — within the note's 7 printed decimals |
| AP1 luminance weights | match exactly |
| all six ACEScct constants | match to 2.7e-14 or better |
| Rec.709 → ACEScg | **corrected**: 4 entries were wrong in the 7th decimal (max 4.7e-7) |
| ACEScg → Rec.709 | **corrected**: 7 entries wrong in the 7th decimal (max 1.4e-6) |

A dated correction was appended to that note. The errors are below any visible threshold, but the
note's own uncertainty ledger asked for them to be regenerated rather than trusted, and the
verified digits are what the shader constant and the CPU helper must both carry.

## Presets and shipped files

`presets.toml`, one table per output file. Each preset names the config URI, the input space, the
display, the view and the grid size(s). The tool refuses to bake if:

* the config does not implement ACES 2.0 (it prints every registered built-in config and exits);
* the display or the view does not exist (it prints what *is* there and exits);
* the red-fastest ordering known-value test fails (a saturated red node that is not red-dominant,
  a saturated blue node that is not blue-dominant, or a neutral input that does not stay neutral).

| file | display | view | size |
|---|---|---|---|
| `odt_srgb_100nit_aces2_33.cube` | `sRGB - Display` | `ACES 2.0 - SDR 100 nits (Rec.709)` | 33³ |
| `odt_srgb_100nit_aces2_65.cube` | `sRGB - Display` | `ACES 2.0 - SDR 100 nits (Rec.709)` | 65³ |
| `odt_rec1886_100nit_aces2_33.cube` | `Rec.1886 Rec.709 - Display` | `ACES 2.0 - SDR 100 nits (Rec.709)` | 33³ |
| `lmt_identity_33.cube` | — | — (analytic identity) | 33³ |

The default is the sRGB one: the engine tail writes to a non-sRGB-typed 8-bit swapchain
(`decisions/s4-display-transform.md` §3A), so the LUT's own encode is what reaches the panel. The
Rec.1886 bake is the reference for the Resolve round-trip (D12), whose default viewer path is
Rec.1886. `lmt_identity_33.cube` is a pass-through for the LMT slot: with it loaded the image must
be bit-identical to the LMT slot being off, which proves the parser, the 3D texture upload, the
sampler addressing and the channel order in one test.

### File format

Resolve-dialect `.cube`: an ASCII comment block, `TITLE`, `LUT_3D_SIZE`, `DOMAIN_MIN`/`DOMAIN_MAX`,
then N³ triplets with the **red index varying fastest**. No embedded 1D shaper — the
linear→ACEScct encode is applied analytically in the shader (research §7 q3), so the file's domain
is plain `[0,1]` and the engine parser stays trivial.

The comment block records the config URI and resolved name, the ACES version, the input space, the
display, the view, the grid size, the OCIO library version, the UTC bake time, the tool's git hash
(with a `-dirty` suffix when the tree is not clean) and a SHA-256 prefix of `bake.py` +
`presets.toml`. That block is the provenance: without it the D12 acceptance test cannot say which
transform produced a given file.

## Precision — what the error reports actually say

Each `.cube` gets a `<name>.report.txt`: 200 000 pseudo-random ACEScct samples (fixed seed, so the
report is reproducible), each pushed through the live OCIO processor and, separately, through
trilinear interpolation of the baked grid — exactly what the GPU sampler will do. Errors are in
output code value; `cv8` is that scaled by 255.

`odt_srgb_100nit`, max / mean absolute error in 8-bit code values:

| sample set | 33³ max | 33³ mean | 65³ max | 65³ mean |
|---|---|---|---|---|
| all samples in the LUT domain | 80.43 | 0.476 | 53.58 | 0.149 |
| inside the Rec.709 gamut (7.4%) | 21.82 | 0.424 | 11.96 | 0.128 |
| near-neutral (2.7%) | 6.32 | 0.337 | 3.13 | 0.105 |

`odt_rec1886_100nit` at 33³: 85.28 max / 0.515 mean over the whole domain, 32.55 / 0.420 inside
Rec.709, 5.99 / 0.334 near-neutral. `lmt_identity_33`: exactly zero, as it must be.

**The mean matches the research note's prediction (under half a code value at 33³, a third of that
at 65³); the maximum does not, and the note has been corrected.** The worst cases are not in the
normally-exposed range — they sit on the cusp of the ACES 2.0 gamut compressor, at bright colours
far outside Rec.709, where the transform has a genuine V-shaped kink. Sweeping green through the
worst sample's neighbourhood, the red output goes 0.42 → 0.12 → 0.66 across two grid cells; no
33³ or 65³ trilinear grid resolves that, and going from 33³ to 65³ only halves it. This is the
real cost of the LUT approach (research §3, "the one place where a 3D LUT is genuinely lossier
than a live transform") and it is measured here rather than assumed.

Practical reading: **ship 33³**. Doubling to 65³ costs 8× the data for a third of a code value on
content anyone will actually shoot, and does not fix the cusp. The 65³ file exists for the A/B.

## Shipped files

Committed here: the tool. The `.cube`s themselves are placed in the engine plugin's asset tree:

```
CRYENGINE_Scene/Code/CryPlugins/CinematicCamera/Assets/ODT/
```

so that a build never needs OCIO. `out/` is the working directory and is not committed.

## Licence / attribution

The ACES configs and the ACES output transforms are Apache-2.0 / ACES-licensed; a baked LUT is a
derived work. The attribution the shipped `.cube`s need before the public sync is still open
(research §7 q6) — the provenance comment block already names the config, which is the minimum.
