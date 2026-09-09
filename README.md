# ReC Sandbox

A learning project: extending CRYENGINE 5.7.1 into a virtual film studio. Everything is built
as an extension of the engine's own systems - entity components, post-effect parameters,
graphics-pipeline stages and shaders - rather than a separate renderer or tool.

What exists today, both still being refined:

- **A physically based virtual camera** (`CinematicCamera` plugin). Real lens and body
  parameters - focal length, f-number and blade count, T-stop, focus distance, ISO, shutter,
  ND filter - drive field of view, depth of field and bokeh, exposure, motion blur and grain.
  On top of that sits a growing set of lens character effects: shaped bokeh masks, iris
  diffraction streaks, anamorphic squeeze and flare, halation, vignetting and distortion,
  field-dependent pupil (cat-eye bokeh, coma, astigmatism), and viewfinder aids (frame
  guides, focus peaking).
- **A simple phone controller** (`CryPhoneTracker` plugin plus an Android app). The phone's
  ARCore pose moves the camera as a handheld 6DoF rig, and on-screen faders drive focus, zoom
  and aperture.

## Branch `scene-referred` (experimental preview)

This branch carries the scene-referred rendering path: with the camera's **Scene Referred**
switch on, lighting is pre-exposed from the camera's own exposure (T-stop, shutter, ISO, ND,
or the camera's AUTO metering), the post chain works in ACEScg (AP1) fp16 without clamps, white
balance / ASC CDL / a Look LUT are applied in scene units, and the picture reaches the screen
through an ACES 2.0 output transform baked by OpenColorIO. The same pre-display buffer can be
written to OpenEXR (ACES2065-1 container) for grading in DaVinci Resolve or Nuke. With the
switch off the engine renders exactly as stock.

What the path covers today:

- **One light-unit convention.** Sun, local lights, area lights (LTC, inverse-square), emissive
  materials, probes, the procedural (Nishita) sky, volumetric fog and clouds, SVOGI, particles
  and snow all feed the same exposure, so the aperture darkens the sky and the fog the same way
  it darkens a wall. Ambient lights become real additive fill lights on this path.
- **A camera meter.** A 64-bin log-luminance histogram with Average / Centre-weighted / Spot
  metering, a percentile band instead of a mean, asymmetric adaptation, a sky weight so an
  outdoor shot is not exposed for the sky, and a per-channel **Sensor Clip** (full-well) above
  mid-grey.
- **Display chain.** White balance, Sensor Clip, ACEScct, ASC CDL, a Look LUT (LMT) and the ODT
  LUT, then dither; the stock LDR stage is inert on this path. Screen-space sun shafts retire by
  default (`r_SunShafts 0`, and the camera keeps them off even when a spec `.cfg` turns them
  back on); volumetric fog is on by default (`e_VolumetricFog 1`).
- **Export.** `rec_CaptureEXR` writes the pre-ODT buffer as OpenEXR; `rec_ImportHDRI` calibrates
  a measured HDRI into a probe.

Status: verified on a physically lit interior and on the sample airfield in daylight; D3D11
only; stock content needs re-lighting in physical units (see `docs/SceneReferredContent.md`).
Two stock crashes found on the way are fixed here too (a lens flare on the wrong kind of light,
an ambient light asked to cast shadows). Besides the plugin and the renderer the branch touches
`Cry3DEngine` (time-of-day light units, procedural sky, fog and cloud exposure, cvar defaults),
`CryEntitySystem` and `CryDefaultEntities` (environment-probe bake convention), so build and
deploy those too. User docs: `docs/SceneReferredContent.md` (light units, physical preset,
probes, HDRI import), `SceneReferredCalibration.md`, `SceneReferredLook.md` (LUTs, grading),
`SceneReferredExport.md` (EXR capture and the Resolve round trip). The baked ACES 2.0 LUTs live
under `engine/Code/CryPlugins/CinematicCamera/Assets/ODT/`; `tools/ocio-bake/` regenerates them.

## Layout

| Path | What it is |
| --- | --- |
| `engine/` | The modified and added CRYENGINE files, in engine tree layout: the two plugins under `Code/CryPlugins/`, the renderer changes under `Code/CryEngine/RenderDll/`, the 3D-engine / entity changes under `Code/CryEngine/Cry3DEngine/` and `CryEntitySystem/`, the EXR writer (`Code/Libs/tinyexr`), and the shaders under `Engine/Shaders/`. `DELETED_FILES.txt` lists files removed from the stock tree. |
| `patches/` | The same changes as a numbered patch series (one patch per step) and `full.patch`, one cumulative diff against pristine 5.7.1. |
| `docs/` | User documentation: the two plugin READMEs (parameters, workflow) and the console reference (commands and cvars). |
| `tools/` | RenderDoc Python scripts used to measure the GPU cost of the added passes; `ocio-bake/` (ACES 2.0 LUT baking), `exr-check/` (EXR vs screenshot comparison), `bitcompare.py` (byte-identity check). |
| `MANIFEST.md` | Which revision this snapshot corresponds to and the list of changed files. |
| `LICENSE.md` | License terms (see below). |

## Building it into CRYENGINE 5.7.1

Start from a pristine CRYENGINE 5.7.1 source checkout, then either apply the patches:

```
git am --keep-cr --3way path/to/ReC-Sandbox/patches/0*.patch
```

or copy `engine/` over the checkout and delete what `engine/DELETED_FILES.txt` lists. Configure
with the engine's bundled CMake (`Tools/CMake/Win32/bin/cmake.exe`, Visual Studio 17 2022, x64)
and build `CryRenderD3D11`, `Cry3DEngine`, `CryEntitySystem`, `CryDefaultEntities`,
`CinematicCamera` and `CryPhoneTracker` in the Profile configuration. The third-party SDK drop (`Code/SDKs`) is not part of this repository.

To use it in a project, copy the built engine DLLs and the two plugin DLLs into the engine's
`bin/win_x64/`, the modified `.cfx` / `.cfi` shader files into
`engine/shaders/HWScripts/CryFX/` (the engine compiles loose shader files at startup), the
`Assets/ODT/*.cube` LUTs next to the project's assets, and add the plugins to the project's
`.cryproject`. See `docs/` for the parameters.

## Related

- The Android tracker app and a physical lens-control module (3D-printed body with magnetic
  encoders for focus, zoom and iris) are separate projects and will be published on their own.

## License

Two-part, see [LICENSE.md](LICENSE.md): the modifications to CRYENGINE files remain Crytek's
copyright and are provided only for users who hold their own CRYENGINE license; the project's
original work (the plugins, docs and tooling) is released under the MIT License.
