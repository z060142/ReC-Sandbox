# Front Filter PSF (star filter and friends) — Specification

Status: approved for implementation, 2026-08-26. Owner of semantics: the camera manager.
Lives in the bloom domain next to the diffraction streaks (`CBloomStage::ExecuteStreaks`);
no dependency on the depth of field.

## 1. What this adds

A cross-screen / star filter is a plate with etched grooves in front of the lens. Every bright
point is diffracted into lines perpendicular to the grooves: 4, 6 or 8 points depending on the
plate, at whatever angle the plate is turned to, at every f-number (unlike the iris sunstars,
which need a stopped-down aperture), with rainbow dispersion toward the line ends. Brighter
sources show longer lines — not because the pattern changes, but because more of its faint
tail rises above the scene.

That last sentence is the model: **a front filter is a fixed point-spread function (PSF)**, and
the picture is the scene convolved with it. The PSF is authored as an RGB texture (the way the
bokeh shape mask is authored), and the convolution is done sparsely: one textured sprite per
bright quarter-resolution texel, added into the bloom target. The same mechanism therefore
also covers any other front-element PSF a preset may want (smeared/dirty filter, custom flare
plates); the star filter is the first user.

No texture bound: a procedural N-point star with rainbow tips, so the effect is testable and
has a sane default.

## 2. Model

* Source: the quarter-resolution HDR frame thresholded (same `HDRStreakThreshold` technique
  and scratch target the streaks use), threshold `T_f` with soft knee `T_f / 4`.
* For every source texel with excess `E` (rgb), a sprite of half-size `R = size * H_q` pixels
  (quarter-res pixels, `H_q` = quarter-res frame height) is drawn centred on the texel,
  rotated by `rotation`, sampling the PSF texture `psf(uv)` (rgb, black outside the pattern,
  bright centre), depositing `E * amount * psf(uv) * kNorm / R²` additively. `kNorm = 400`
  puts amount 1 at "clearly visible spikes on a lamp" for a PSF whose bright core is a few
  texels wide; `amount` absorbs authoring differences (documented, like the splat's gain).
* Composited exactly like the streaks: additive into `m_pTexHDRFinalBloom` with the deposit
  divided by `fBloomCompositeWeight`, so on-screen energy does not depend on the scene's
  bloom amount.
* Anamorphic squeeze: the filter sits in front of the anamorphic element, so its PSF is
  squeezed like everything else in the frame — the sprite's x half-size is divided by S
  (read the squeeze from the DOF user param `Dof_User_AnamorphicSqueeze`, or, simpler and
  already in the bloom stage's reach, from the streaks' anamorphic branch if it exposes it;
  otherwise `PostEffectMgr()->GetByName("Dof_User_AnamorphicSqueeze")`).
* Not rotated by the iris phase, not scaled by the aperture: it is a plate, not the iris.

### Procedural fallback (no texture)

`psf(uv)` for `N` points (`N` even, 4/6/8): for each of the `N/2` line directions `θ_j =
j * π / (N/2)` (rotation applied outside): `along = dot(d, dir_j)`, `perp = dot(d, perp_j)`,
`line_j = exp(-|perp| * 40) * pow(saturate(1 - |along|), 2)` with `d` in [-1, 1] sprite space;
rainbow: tint each line by a hue that runs red → blue with `|along|` (mix with white by
`(1 - |along|)`), sum over lines, plus a small central core `exp(-|d|² * 60)`.

## 3. Renderer (`Bloom.cpp` / `HDRPostProcess.cfx`)

`CBloomStage::ExecuteFilterPSF()` after `ExecuteStreaks()` (the scratch targets are free
again, same as halation):

1. Threshold pass into `m_pTexHDRTargetScaled[1][1]` (`techStreakThreshold`,
   `StreakParams1 = (T_f, T_f * 0.25, 0, 0)`), skipped entirely when `amount < 0.001`.
2. Sprite pass: technique `HDRFilterPSF` (new VS `FilterPSFVS` + PS `FilterPSFPS`), one
   `CRenderPrimitive` with `SetCustomVertexStream(~0u, Empty, 0)` and
   `width_q * height_q * 6` vertices, per-view CB generated for the quarter-res grid (copy the
   DOF ghost's pattern: `GeneratePerViewConstantBuffer` with a custom viewport), render target
   `m_pTexHDRFinalBloom`, viewport = its size, additive blend, no depth. VS: cell → texel
   centre, one point read of the thresholded source, cull on zero, expand the quad to
   `R * (1/S, 1)` in NDC, pass `Color = E * amount * kNorm / (R² * fBloomCompositeWeight)`
   and the quad-local uv. PS: rotate uv by `rotation` around 0.5, sample `psf` (or the
   procedural star), multiply by `Color`, output rgb (alpha 0).
   Constants: `FilterPSFParams0 = (R_x_ndc, R_y_ndc, cos rot, sin rot)`,
   `FilterPSFParams1 = (amount-scaled energy, points N, texBound 0/1, 0)`.
3. Log once per change: `[BLOOM] filter psf amount=%.2f size=%.2f rot=%.0f thresh=%.1f
   tex=%s`.

`r_HDRStreaks` stays the master switch for the whole streak/flare/PSF block (0 disables this
too); `r_HDRStreaks 2` does not force the PSF.

## 4. Parameters

Post-effect params on `CHDRStreaks` (they are all "what the front of the lens does to a
point"): `HDR_Filter_Amount` (0), `HDR_Filter_Size` (0.25), `HDR_Filter_Rotation` (0, radians),
`HDR_Filter_Threshold` (6), `HDR_Filter_Points` (4), `HDR_Filter_PSFTex` (`AddParamTex`, empty).
`SHDRStreaksParams` gains the matching fields (texture as `CTexture*` like the DOF's
`pBokehShapeTex`).

New component group `SCineFilterParams` "Front Filter" (own GUID, new 4CCs):

| Member | Label | Range | Default | Tooltip gist |
|---|---|---|---|---|
| `starAmount` ('flAm') | Star Amount | `Range<0, 2, 0, 1>` | 0 | Strength of the star / PSF filter. 0 = off (no pass). |
| `starPoints` ('flPt') | Star Points | `Range<4, 8, 4, 8, int>` | 4 | Points of the procedural star when no PSF texture is set: 4, 6 or 8 (odd values round down). |
| `starSize` ('flSz') | Star Size | `Range<0, 1, 0, 1>` | 0.25 | Reach of the pattern, as a fraction of frame height. Brighter sources show more of it. |
| `starRotation` ('flRt') | Star Rotation (deg) | `Range<0, 360, 0, 360>` | 0 | Turning the filter turns the star. |
| `starThreshold` ('flTh') | Star Threshold | `Range<1, 64, 2, 16>` | 6 | Scene HDR luminance a source needs to spawn the pattern. |
| `psfTexture` ('flTx') | PSF Texture | `Schematyc::TextureFileName` (keep LAST, `operator==` memcmp prefix like `SCineLensParams`) | empty | RGB point-spread image of the filter, centred, black outside the pattern. Asset type = the bokeh shape masks' (uncompressed 32-bit DDS, in `textures/lights/lens_flares/` with the projector-light textures; no resource-compiler round trip). Examples `filter_star4/6/8.dds`, generated by `tools/make_star_psf.py`. Empty = procedural star. |

Component: apply / save / restore in the streaks apply function (`ApplyStreaks` or wherever
`HDR_Streaks_AnamorphicAmount` is pushed), texture through `SetPostEffectParamString` like
`Dof_User_BokehShapeTex`. Game-mode + editor-live like the anamorphic flare.

## 5. Acceptance

1. Amount 0: no pass, frame identical.
2. Amount 1, 4 points, a lamp: a four-pointed star centred on the lamp, lines fading out with
   rainbow tips; Rotation 45° turns it; 6 / 8 points; brighter lamps show longer lines; f/1.4
   and f/8 look the same (it is a plate).
3. A PSF texture (make a 256×256 test image with a 6-point star in the scratchpad or
   `Assets/`; note the path in the report): the pattern is that image.
4. Anamorphic 2×: the star is squeezed horizontally like the picture.
5. Bloom amount in the TOD changed: the star's on-screen brightness does not change.
6. RenderDoc: < 0.15 ms at 1080p with a few sources.

## 6. Docs

README: "Front Filter" group table + paragraph; spec listed. ConsoleReference unchanged.
