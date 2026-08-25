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

## Layout

| Path | What it is |
| --- | --- |
| `engine/` | The modified and added CRYENGINE files, in engine tree layout: the two plugins under `Code/CryPlugins/`, the renderer changes under `Code/CryEngine/RenderDll/`, and the shaders under `Engine/Shaders/`. `DELETED_FILES.txt` lists files removed from the stock tree. |
| `patches/` | The same changes as a numbered patch series (one patch per step) and `full.patch`, one cumulative diff against pristine 5.7.1. |
| `docs/` | User documentation: the two plugin READMEs (parameters, workflow) and the console reference (commands and cvars). |
| `tools/` | RenderDoc Python scripts used to measure the GPU cost of the added passes. |
| `MANIFEST.md` | Which revision this snapshot corresponds to and the list of changed files. |
| `LICENSE.md` | License terms (see below). |

## Building it into CRYENGINE 5.7.1

Start from a pristine CRYENGINE 5.7.1 source checkout, then either apply the patches:

```
git am --keep-cr --3way path/to/ReC-Sandbox/patches/0*.patch
```

or copy `engine/` over the checkout and delete what `engine/DELETED_FILES.txt` lists. Configure
with the engine's bundled CMake (`Tools/CMake/Win32/bin/cmake.exe`, Visual Studio 17 2022, x64)
and build `CryRenderD3D11`, `CinematicCamera` and `CryPhoneTracker` in the Profile
configuration. The third-party SDK drop (`Code/SDKs`) is not part of this repository.

To use it in a project, copy the built `CryRenderD3D11.dll` and the two plugin DLLs into the
engine's `bin/win_x64/`, the modified `.cfx` / `.cfi` shader files into
`engine/shaders/HWScripts/CryFX/` (the engine compiles loose shader files at startup), and add
the plugins to the project's `.cryproject`. See `docs/` for the parameters.

## Related

- The Android tracker app and a physical lens-control module (3D-printed body with magnetic
  encoders for focus, zoom and iris) are separate projects and will be published on their own.

## License

Two-part, see [LICENSE.md](LICENSE.md): the modifications to CRYENGINE files remain Crytek's
copyright and are provided only for users who hold their own CRYENGINE license; the project's
original work (the plugins, docs and tooling) is released under the MIT License.
