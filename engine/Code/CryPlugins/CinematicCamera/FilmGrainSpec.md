# Film Grain — specification

Status: **G0 + G1 shipped 2026-09-11** (branch `grain`, worktree `CRYENGINE_Grain`) — the film grain the
camera ships is the §3.5 white-Gaussian cell generator for the Film family plus the §3.4 sensor model for
CMOS / CCD / Phone. **G2 (grain plates) PARKED** — the §3.5 generator already gives film grain the user
accepted, so scanned plates buy detail nobody has asked for yet. **G3 (EXR metadata + measurement tools)
PARKED** — nothing downstream consumes the metadata until an offline re-synthesis workflow exists.
Both unpark when a concrete camera-emulation need arrives.
**Base since 2026-09-10: `dev@a4fed24b` — the scene-referred pipeline IS the main line now** (`scene` was
fast-forwarded into `dev`, S9 editor CineCam preview and S10 CineCam Grade included). G0 and G1 were
written against the stock `dev@28a492c1` and rebased onto the merged main line; the only behavioural
consequence is in §2 below (the scene-referred gate is back, and it zeroes the stock grain independently
of us).
Author: Fable. Implementer: Opus, one stage per agent. Working notes (outside git):
`scene-notes/grain/` — `01-requirements.md` (R-numbers), `PROPOSALS.md` (design space, §0 shared
skeleton), `research/01-ce-native-grain.md` (stock implementation, every claim with file:line),
`decisions/01-generator-and-placement.md`.

## 1. Goal

Replace CE's stock film grain for the cinematic camera with a capture-side noise model that behaves like
a real camera's: grain has a physical size on the negative / sensor, an amplitude that follows tone and
ISO by the right law, colour structure per layer, a fresh pattern per capture frame, deterministic seeds,
and a family slot (Film / CMOS / CCD / Phone) that the future composable body preset fills. Stock behaviour
is byte-identical whenever the block is inactive.

Two generator families, one skeleton:

- **Film** — grain plates (offline-synthesised from the Boolean grain model), sampled in film-mm space.
- **Digital** — shot + read + fixed-pattern sensor model; CMOS / CCD / Phone are parameter variants.

## 2. Placement and pass order (facts, research §2/§6/§7)

- `CPostAAStage::Execute` (`PostAA.cpp:545-586`): SMAA → TAA → `DoFinalComposition`. The composition
  pass (`PostAAComposites_PS`, `PostAA.cfx:959-1194`) is the last full-screen pass and the only path to
  the back buffer. Inside it, in order: squeeze / letterbox remap → distortion → base fetch → lateral CA →
  gate mask → sharpen (`SAMPLE2`) → lens-optics composite (`SAMPLE1`) → **grain** (`:1060-1061`) →
  `saturate` (`:1063`) → viewfinder overlays → Rec.709 range compression.
- TAA runs **after** the tonemap / ODT in both stock and scene trees. Grain injected earlier is averaged
  out by the history clamp (`PostAA.cfx:904` says so). Therefore **v1 replaces the grain block in place**:
  same slot, same order relative to sharpen / overlays. Pre-ODT placement is the future upgrade once TAA
  moves in front of the tonemap; the interface in §4 does not change then (R1.6).
- The stock `ApplyFilmGrain` (`PostAA.cfx:905-924`) and its amount path (`PostAA.cpp:408-412`,
  `HDRParams.w`) stay exactly as they are for the inactive case. **On the merged main line the
  scene-referred gate of D9 applies again**: `DoFinalComposition` already forces the stock amount to 0
  while `IsSceneReferredStage(5)` is true, because the stock overlay blend pivots on 0.5 and is only
  defined on [0,1]. The two gates are independent and the resolved line is
  `(bSceneReferred || bCineGrain) ? 0.0f : max(paramGrainAmount, environmentGrainAmount)`. Consequences:
  under Scene Referred the stock grain is already gone whether or not a cinecam is driving, and our
  block is what takes the vacated slot — which is exactly the "comes back before the ODT as a sensor
  effect in S7" that the D9 comment promised, landing in the composition pass for now (the pre-ODT move
  still waits for TAA to go in front of the tone map, R1.6). Off the scene-referred path the block's own
  `Grain_User_Active` is still the only thing that zeroes the stock amount.
- Source / destination at that point: `$DisplayTargetDst` R10G10B10A2 UNORM → swap-chain R8G8B8A8 UNORM
  (research §2.3). The write is 8-bit: grain σ must stay above ~1 LSB where it is meant to be visible (R8.4).

## 3. Model (all families)

Per pixel, per channel c, on the display-encoded value d (what the pass has in `OUT.Color` at the grain
slot). The first three lines are shared; **the blend is not** — Film is multiplicative, Digital is additive
(changed 2026-09-10, see §3.4 and the fix note below):

    x   = DecodeDisplay(d)                     // inverse sRGB (both trees end in sRGB display encoding)
    t   = log2(max(x, 2^-14))                  // display-log axis, −14 … 0
    n   = generator(p_mm, c, seed)             // zero-mean, unit-variance, already footprint-integrated

    FILM     g   = amount_c · R_c(t)            // R_c: 1D response LUT over t, 0 above clip
             x'  = x · exp2(g · n − bias(g))     // bias(g) = (g/2.4)² · ln2 / 2 : keeps the DISPLAY-ENCODED mean (R2.4)

    DIGITAL  Δ   = amount_c · sensorField(x, c) // Δ and its σ in DISPLAY-LINEAR units, not in stops
             x'  = clamp(x + Δ, 0, 1)          // no log, no exp, no bias term, no σ cap

    d'  = EncodeDisplay(x')

**Why the two blends differ.** Film grain modulates a DENSITY that is proportional to the exposure: nothing
develops where nothing was exposed, so the perturbation is relative and belongs in the log domain. Sensor
noise is ADDED by the readout: read noise, DSNU and the row / column terms are fixed electron counts that do
not know the pixel's value, and shot noise is √e, which likewise does not vanish with the signal the way a
relative term would. Expressing that in stops means dividing by x, and the quotient diverges as x → 0.
That is the bug fixed on 2026-09-10: the σ cap (1.5 stops) did not save it, because the unit-variance field
n = Δ/σ is itself unbounded in the deep shadows (the Poisson branch draws integer electron counts against a
sub-electron σ), so g·n reached ten stops and more on a near-black pixel and `exp2` turned it into a bright
dot, coloured by the per-channel raw WB gains — the sparse saturated magenta / cyan / white sparkles seen in
a dark CMOS frame. A real sensor's black is a lifted, slightly grey floor with a faint chroma blotch, which
is what the additive path gives.

This is a **calibrated display-domain model** (as AV1 film grain is), not a density simulation; the film
response LUT R_c carries the toe / shoulder shaping the ODT would otherwise provide (PROPOSALS §0.1, §8.5).
The digital families have no R_c at all: their response is emergent from the electron count.
Above the clip point (x ≥ 1 − ε) the amplitude is 0 for both families (R2.5). A pixel whose display value is
identically 0 in all three channels is left untouched on the digital path too — that is the anamorphic gate
mask's letterbox bar, and additive read noise would otherwise put grain on it. Then the stock `saturate`
runs as before.

**Mean.** Film: preservation is defined in the display-encoded domain (what the eye and an 8-bit capture
average), not in linear — a linear-mean bias darkens the encoded median by ~g², and with per-layer
amplitudes that is a colour cast (found in G0 testing: Film turned the picture yellow). Digital: additive
zero-mean noise is mean-preserving in linear by construction, so **no bias term is applied**; the encoded
mean shifts by a second-order concave term far under one code value, which is ignored on purpose, and the
clamp at 0 lifts the deepest black, which is exactly what a real sensor's black-level pedestal exists to
absorb. No pedestal is modelled (the block runs on an already-encoded display value), so the lifted black
stays and is documented rather than cancelled.

Channel correlation ρ (R4.1, film only): n_c = √ρ · n_mono + √(1−ρ) · n_c,indep, where n_mono is a fourth
independent field. ρ = 1 → monochrome grain; ρ = 0 → independent layers. The digital families ignore ρ: their
colour structure comes from the raw WB gains and the chroma NR instead.

### 3.1 Film space (R3.1, R3.4)

    fp_um   = sensorWidth_mm · 1000 / outputWidth_px            // output-pixel footprint on the negative
    p_mm    = (pixel + 0.5 − centre) · (sensorWidth_mm / outputWidth_px), then p_mm.x /= squeeze

`outputWidth_px` is the render output width (`PS_ScreenSize`), `sensorWidth_mm` and `squeeze` come from
cinecam (already published as `Dof_User_SensorWidth`, `Dof_User_AnamorphicSqueeze`). Grain sizes are µm on the
negative; the generator receives `fp_um` and integrates sub-pixel grain (R3.2): plates through mip LOD =
log2(fp_um / plateTexel_um), procedural generators through analytic variance division by the number of
grains / photosites per output pixel.

### 3.2 Seeding (R5, §8.6 of PROPOSALS)

    seed        = Hash(shotSeed, captureFrameIndex, viewIndex)
    per-cell    = Hash(seed, cellX, cellY, layer)                  // integer hash, pcg3d class

No RNG state, no `Time`. `captureFrameIndex` is a cinecam-owned counter (§5.3); `r_FilmGrainFreeze 1` pins it.
Digital fixed-pattern terms hash the sensor-pixel index only (no seed). All hashing and coordinate
arithmetic is integer so results are bit-identical across runs on the same GPU; floats enter only at
the final blend.

### 3.3 Film generator — plates (P1) — **PARKED, not implemented**

> Stage G2. Nothing below exists in the shipped build. The Film family currently uses the §3.5
> white-Gaussian cell generator, which the user accepted on 2026-09-11, so the plate pipeline
> (offline tool, DDS array, per-tile variant sampling, uploaded response LUT) is kept as a
> design only. The offline synthesis tool exists untracked at `ReC-Sandbox/tools/grain-plates/`.
> Unpark when a specific stock has to be matched.

- Asset: `Assets/Grain/<sizeClass>.dds`, Texture2DArray, 8 slices, 512², R8G8B8A8 (or BC-free RGB8),
  full mip chain generated from the **periodic** plate, zero-mean (127.5 = 0), unit variance per slice
  after normalisation. Size classes: `fine` (r̄ 3 µm), `medium` (6), `coarse` (12), `xcoarse` (24). Plate
  texel = r̄ / 2 µm (a 512² plate covers 0.77–6.1 mm of negative).
- Synthesis (offline, `ReC-Sandbox/tools/grain-plates/`, uv + PEP 723, numpy): Boolean model on a torus
  (periodic Poisson process, log-normal radii, three independent layers with per-layer r̄ ratios
  B 1.25 / G 0.8 / R 1.0 of the class r̄), coverage at 0.5, mean removed, filtered so the spectrum follows
  √S(f) of the target (Zhang et al. 2023 statistics; PROPOSALS §8.3). Emits DDS plus a JSON sidecar with
  the measured variance / autocorrelation used for calibration.
- Sampling: per-tile variant. Tile = the plate's mm extent; `variant = Hash(seed, tileX, tileY)` selects
  slice (8), rotation (4) and flip (2) = 64 variants; plus a per-frame sub-tile offset from `seed`. Sampled
  with trilinear filtering at LOD = log2(fp_um / plateTexel_um); the mip average is the footprint
  integration. Anamorphic: sample at `p_mm` (already squeezed).
- Per-layer amplitude `amount_c`, response LUT `R_c` and ρ come from the preset (§5.2).

### 3.4 Digital generator — sensor model (P4)

Runs at the virtual sensor pitch, in linear "sensor" units, x from §3 standing in for exposure:

    e       = x · fullWell · (isoBase / iso)                 // electrons
    shot    = N(0, e)            (Poisson draw below 20 e⁻)
    read    = N(0, σr(iso)²)                                  // per-ISO table, dual-gain = two tables
    prnu    = 1 + k_prnu · H(pixelIdx)      dsnu = k_dsnu · H'(pixelIdx)    // static, no seed
    row     = k_row · H''(row, seed)        col  = smearE · H'''(column, seed)   // per-frame
    dE      = shot · prnu + e · (prnu − 1) + read + dsnu + row + col           // electrons
    Δ_raw   = dE / (fullWell · isoBase / iso) · wb_c                            // display-linear, per channel

**Output units: DISPLAY-LINEAR and ADDITIVE** (changed 2026-09-10; previously stops). The generator returns
Δ and its analytic σ in the same units, both already attenuated by the integration below, and the caller
applies `x' = clamp(x + amount·Δ, 0, 1)`. There is no σ cap and no division by x anywhere — those were the
sparkle bug of §3. σ is still computed analytically from the same coefficients the draw uses (every draw has
unit variance, so it is the root of the sum of their squares) and is used for the chroma split, for the
"is there anything to apply" test and for the debug report; it is no longer part of the blend.

    σ_lin²  = (k · wb_c)² · ( e + (e·k_prnu)² + σr² + k_dsnu² + k_row² + smearE² ),   k = 1/(fullWell·isoBase/iso)

Per-channel raw WB gain (blue / red amplified, green the reference) is where the colour of digital noise
comes from. The chroma component of Δ is then low-passed by chromaNR — redrawn on a 1–4 px hashed cell and
attenuated ×0.6, the luma component kept per pixel. The split is a linear recombination and is done in the
same linear units as Δ.

Sensor → output integration: nPix = (sensorWidth_px / outputWidth_px)²; σ ÷ √(nPix^E · stackN), where **E is
the integration exponent** — 1.0 is the textbook variance ÷ N, but demosaicing correlates neighbouring
photosites so a real downscaled raw frame measures noisier than that, and 0.7 was the documented
compromise. E = 0 turns the integration off altogether: one photosite's noise on one output pixel whatever
the downscale factor — the "pixel peeping at 1:1" look. E is a PRESET VALUE on the bus
(`Grain_User_Digital3.x`, §4) rather than a `#define`, so a body can carry its own; a negative value
selects the shader's compile-time fallback, which stays 0.7 for reference. **Since 2026-09-10 every preset
carries E = 0** — a judgement, not a measurement: against real footage the CMOS family read too clean at
any downscale, and 0 is the honest 1:1 statement. Frame stacking (`stackN`) is a separate divisor and is
unaffected.

**Format → photosite (2026-09-10).** With E = 0 the downscale ratio no longer affects noise at all, so the
FORMAT has to carry its own. The preset therefore fixes a photosite **count** (its resolution class) and a
**reference body width**, and derives the pitch:

    count   = photositeCount / sizeScale            (clamped to [320, maxSensorPx])
    pitch   = refWidth_mm · 1000 / photositeCount · min(bodyWidth_mm / refWidth_mm, 1) · sizeScale
    fullWell(body) = fullWell(ref) · (pitch / refPitch)²

Full-well capacity is proportional to photosite AREA, so a smaller body divides the same count into a
smaller width, gets a smaller photosite, collects fewer electrons and is noisier — M43 is 2.08× a full
frame at the same ISO, which is the two stops of real-world "equivalence". Read noise, DSNU and the row
term are amplifier properties in electrons and do not shrink, so their relative weight grows too. A body
WIDER than the reference gets no bonus (the `min(…, 1)`): a family's electronics are designed for a format,
and a 2/3-inch CCD's readout on a full-frame photosite is not a body anyone has built.

**The structured terms (2026-09-10 fix).** `row` and `col` come from ONE amplifier / ONE readout register
serving the whole line or column, so the offset they add is a single number the three channels share; the
Bayer filter only makes them respond to it slightly differently. Both are therefore drawn as
√0.9 · (one shared draw) + √0.1 · (per-channel draw) — unit variance per channel, 10 % of the energy as
colour. Drawing three independent numbers (which is what G1 did) turned a luma band into a rainbow band.
`smearE` is a fraction of the **saturating column's charge at this ISO**, `fullWell · isoBase/iso`, NOT of
the bare full well: with the bare well the term kept its absolute electron count while the signal shrank
with the ISO gain, so it grew against the picture by iso/isoBase (×64 at ISO 12800) — that, plus a gate
with a 0.3 FLOOR that put a third of it on every pixel of the frame, is how a sub-code-value term became
tens of percent of a daylight frame. The gate now starts at zero: `smoothstep(0.75, 1.0, luma)`, so there
is nothing at all in a normally exposed scene and a faint streak only under a clipping source. The
datasheet figure for smear is 0.01–0.1 % of the saturating charge **per line transferred**; the whole-column
accumulation is one to two orders larger, and the 1 % the preset uses is calibrated to put the streak at
about 1 code value at base ISO (ceiling ≈ 2 CV).

Gaussian draws from two hashes via Box–Muller. CCD variant: no dual gain, higher σr, a vertical smear term
(column-correlated noise gated by local brightness, static per frame), coarser chroma blotch, 8–12-bit ADC
quantisation (not modelled — the 8-bit swap-chain write at the end of the pass quantises harder).
Phone variant: pitch 0.7–1.4 µm, stacking N frames → variance ÷ N, strong chromaNR.

### 3.5 Stage G0 generator — white Gaussian — **SHIPPED; this is the Film generator**

Before plates exist: n = Gaussian(Hash(seed, pixel, c)), footprint-integrated by variance ÷ max(1,
(fp_um / sizeUm)²). Zero assets. It is also the shot-noise core of §3.4, so it is not throw-away.

## 4. Interface cinecam → renderer (R6)

Post-effect params, all prefixed `Grain_User_` (registered like the `Dof_User_*` family, manager level).
Written every frame from `FinalizeGameCamera` under `CanDriveRenderState()`, saved once / restored on
release exactly like `ApplyFilmGrain` / `RestoreFilmGrain` today (`CinematicCameraComponent.cpp:935-956`,
`:1404-1411`).

| Param | Type | Meaning |
|---|---|---|
| `Grain_User_Active` | float 0/1 | block active → renderer ignores `FilterGrain_Amount` (forced 0) and runs §3 |
| `Grain_User_Family` | float enum | 0 Film, 1 CMOS, 2 CCD, 3 Phone |
| `Grain_User_Amount` | Vec4 | amount_r, amount_g, amount_b, ρ |
| `Grain_User_Size` | Vec4 | sizeUm_r, sizeUm_g, sizeUm_b, plateTexel_um (0 = procedural) |
| `Grain_User_Sensor` | Vec4 | sensorWidth_mm, squeeze, sensorWidth_px, fp override (0 = auto) |
| `Grain_User_Seed` | Vec4 | shotSeed, captureFrameIndex, freeze, algorithmVersion |
| `Grain_User_Digital0` | Vec4 | fullWell, isoBase, iso, σr(iso) |
| `Grain_User_Digital1` | Vec4 | k_prnu, k_dsnu, k_row, chromaNR |
| `Grain_User_Digital2` | Vec4 | raw WB gain red, raw WB gain blue (green = 1), stacked frames, CCD smear |
| `Grain_User_Digital3` | Vec4 | x = sensor→output integration exponent E (**0 = default since 2026-09-10**, 0.7 = shader fallback, 1 = textbook 1/N); y = effective photosite pitch µm (report only); z, w reserved |
| `Grain_User_Response` | float texId | 1D response LUT (RGB, 64 texels) uploaded by cinecam, 0 = built-in |
| `Grain_User_Plates` | float texId | Texture2DArray uploaded / loaded by cinecam, 0 = none (G0 fallback) |

Textures are plugin-owned, published as ids and resolved with `CTexture::GetByID`, folded into the pass
`IsDirty` tuple, as the ODT / LMT LUTs are (`CinematicCameraComponent.cpp:1436-1487`, `ToneMapping.cpp:362`).
The renderer holds no grain logic beyond §3.

## 5. cinecam component

### 5.1 Properties (group "Film Grain", replaces `SCineGrainParams`; `AddMember` names `[A-Za-z0-9_]+`)

| Property | Range / default | Note |
|---|---|---|
| Enable Film Grain | bool, true | as today |
| Family | enum Film / CMOS / CCD / Phone, Film | body family; the future preset fills it |
| Grain Strength | 0–4 (slider 0–1), 1.0 | multiplies `ComputeFilmGrain01()` (kept as the ISO / light-starvation law for now, R6.2) |
| Size | 0.25–4, 1.0 | multiplies the preset's size class / pitch |
| Colour | 0–1, preset | 1 − ρ |
| Shot Seed | int 0–65535, 0 | a new number = a new roll of film |
| Advanced (collapsed) | per-family numbers of §3.3 / §3.4 | preset-driven defaults |

Defaults: Film = 500T-style, Super 35, `medium` class, layer amplitudes B 1.3 / G 0.9 / R 1.0, ρ 0.35,
response peak at t = −2.5 (mid-tones), width 3 stops, fading to 0 at t = 0 and below −9.
CMOS = 6000 photosites across a full-frame reference (36 mm → 6.0 µm pitch), full well 60 000 e⁻ at that
pitch, base ISO 800, σr 3 e⁻ (dual gain at ISO 3200: 1.5 e⁻), k_prnu 0.005, k_dsnu 1.0 e⁻, k_row 0.2 e⁻,
chromaNR 0.6, **E = 0**.
CCD = 4300 across 36 mm (8.4 µm), 40 000 e⁻, base ISO 200, σr 12 e⁻ (no dual gain), k_prnu 0.008,
k_dsnu 3.0 e⁻, k_row 1.2 e⁻, chromaNR 0.45, smear 0.6, **E = 0**.
Phone = 8000 across a 7.6 mm reference (0.95 µm), 6 000 e⁻, base ISO 50, σr 1.2 e⁻, k_prnu 0.010,
k_dsnu 1.0 e⁻, k_row 0.3 e⁻, chromaNR 0.9, 6 frames stacked, **E = 0**.
The structured terms are quoted at the quiet end of the measured ranges (row/column 1–2 e⁻ rms before the
optical-black reference lines cancel most of it, DSNU 1–3 e⁻, PRNU 0.5–1 %) because every real body
corrects them on the way out.

### 5.2 Presets

For v1 a C++ table keyed by family (the composable body preset replaces it later without touching the
renderer). Numbers-only; plates are shared by size class.

### 5.3 Capture-frame counter (R5.2)

cinecam owns `m_captureFrameIndex`: +1 per frame from `ApplyGameOnlyEffects()`, which since S9 is the one
activation path for every effect — game mode reaches it from `FinalizeGameCamera`, the editor from the
plugin's per-frame resolver when the viewport looks through a cinecam entity, so the grain obeys the same
editor policy as DOF and halation. Reset to 0 when the component
activates as the authority, when Shot Seed changes, and when a TrackView sequence starts; while a sequence
is playing / scrubbing it is `floor(sequenceTime · captureFps)` (captureFps = 24 default, property).
`r_FilmGrainFreeze` pins it. Not advanced while `gEnv->pSystem->IsPaused()` unless "Camera Rolling While
Paused" (bool, default true).

## 6. Cvars (renderer, `RendererCVars`, house style of `r_DepthOfFieldLensModel`)

| Cvar | Default | Meaning |
|---|---|---|
| `r_FilmGrain` | 1 | master switch for the cinecam block (0 = stock path even if cinecam publishes it) |
| `r_FilmGrainDebug` | 0 | 1 = grain only on mid grey, 2 = response LUT overlay, 3 = per-second plain-language report (family, amount, size in px, fp_um, LOD, seed, cost estimate) |
| `r_FilmGrainFreeze` | 0 | pin the capture-frame index |

## 7. Export (R8.6) — **PARKED, not implemented**

> Stage G3. No `rec.grain.*` metadata is written and no `grain-apply` tool exists. Nothing
> downstream consumes it yet; unpark when an offline re-synthesis workflow is actually needed.

The EXR tap is pre-ODT, so the master is grain-free. `rec_CaptureEXR` writes a metadata block
`rec.grain.*`: family, the §4 vectors, shotSeed, captureFrameIndex, algorithmVersion. An offline uv tool
(`tools/grain-apply/`, later stage) re-synthesises identical grain from it.

## 8. Byte identity and gating (project rule)

- cinecam off, or `Grain_User_Active` 0, or `r_FilmGrain` 0: `DoFinalComposition` and the shader are
  unchanged in behaviour; verify with `bitcompare.py` on the VisRegTest captures.
- The new HLSL lives in a new `FilmGrain.cfi` included by `PostAA.cfx`; the stock `ApplyFilmGrain` stays.
  A new static flag (pick a free `%_RT_*` bit; document it) selects the block so the stock permutation
  is untouched.

## 9. Stages (one agent each, each a reviewed commit on `grain`)

- **G0 skeleton** — **SHIPPED 2026-09-11**: params + registration, `FilmGrain.cfi` (hash, §3 blend, §3.1 film space, §3.5 white
  Gaussian, ρ mixing, clip), gating + flag, cvars, cinecam properties / counter / apply-restore, debug 1 and 3,
  docs. Test: grey card, ISO sweep, freeze, byte identity.
- **G1 digital** — **SHIPPED 2026-09-11**: §3.4 full (CMOS / CCD / Phone variants), presets table, debug 2.
- **G2 plates** — **PARKED** (§3.3; the §3.5 generator is good enough, offline tool untracked at `ReC-Sandbox/tools/grain-plates/`): offline tool, four size classes, loader / upload, §3.3 sampling, film presets.
- **G3 export + measurement** — **PARKED** (§7; nothing consumes the metadata yet): EXR metadata, `grain-apply` tool, statistics script (mean / variance /
  correlation / spectrum / autocorrelation over sequences).

## 10. Acceptance

`scene-notes/grain/01-requirements.md` R11 (1–11), one topic per round, results written by the user into
their own checklist file (agents never write it).

## 11. Cost

Target ≤ 0.10 ms at 1080p / ≤ 0.30 ms at 4K measured with `rdc_perf.py` on the composition pass
(stock grain vs block vs none). No new full-screen pass, no intermediate target.
