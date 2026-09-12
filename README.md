# ReC Sandbox

A learning project that turns CRYENGINE 5.7.1 into a small virtual film studio: a camera that
behaves like a real one, a rendering path that keeps real light levels all the way to the
screen, and a colour-grading desk that works on that light instead of on a finished picture.

Everything is built as an extension of the engine's own systems (entity components,
post-effect parameters, graphics-pipeline stages, shaders, editor plugins). Nothing here is a
separate renderer or tool. With the camera's switches off, the engine renders exactly as stock.

## What you get

### A physical camera

The `CinematicCamera` component takes the numbers a cinematographer thinks in and derives the
picture from them.

- **Body and lens**: focal length, f-number and blade count, T-stop, focus distance, ISO,
  shutter, ND filter. They drive field of view, depth of field and bokeh, exposure, motion blur
  and grain.
- **Lens character**: shaped bokeh masks, iris diffraction (wave-optics streaks from the real
  pupil shape), anamorphic squeeze and flare, halation, vignetting, distortion, cat-eye bokeh,
  coma and astigmatism, axial and lateral colour fringing, lens ghosts, front filters such as
  star filters.
- **Viewfinder aids**: frame guides with readouts, focus peaking.

### A simple phone controller

The `CryPhoneTracker` plugin plus an Android app. The phone's ARCore pose moves the camera as a
handheld 6DoF rig, and on-screen faders drive focus, zoom and aperture. A pose filter and a
virtual tripod keep a hand-held shot steady, and the phone follows whichever camera is active
when you cut between camera slots.

### Real light on the way to the screen

With the camera's **Scene Referred** switch on, the engine stops squeezing the picture into a
display range early:

- Every light source, the sky, fog, clouds, emissive materials and global illumination share one
  physical unit, so opening the aperture brightens a wall, the sky and the fog by the same amount.
- Exposure comes from the camera (T-stop, shutter, ISO, ND) or from a camera-style meter with
  Average, Centre-weighted and Spot modes, not from the engine's eye adaptation.
- The post chain works in ACEScg with no clamps, and the picture reaches the monitor through an
  ACES 2.0 output transform baked with OpenColorIO. Nothing before that transform knows a screen
  exists.
- The pre-display frame can be written to OpenEXR (ACES2065-1) and opened in DaVinci Resolve or
  Nuke with the same output transform, so what you saw in the engine is what you get in post.

### A grading desk

The `CineCam Grade` component, added to the same entity as a camera, grades what that camera
renders. It is deliberately the toolset of an on-set colourist rather than a full grading suite:

- **Primaries**: Lift, Gamma and Gain colour wheels with masters, contrast about a pivot,
  saturation, and the raw ASC CDL numbers underneath. What you set here is exactly what a
  `.cdl` file says, so it round-trips to Resolve.
- **Curves**: a five-point master tone curve and a saturation-versus-saturation curve.
- **Looks**: a LUT slot that knows which colour space a LUT was made for. A LUT built for
  Rec.709 monitors can be dropped in and is wrapped into the pipeline automatically; LUT files
  reload when they change on disk. Three example looks ship with the project, each with its
  recipe written in the file header.
- **Everything is a file**: LUTs, CDLs and whole grade presets are CryEngine assets under
  `Assets/cinecam/`, browsable in the Asset Browser, exportable from one camera and re-applied
  to another. A capture writes its grade beside the EXR frames.
- **A grade panel** in the Sandbox editor (`Tools -> Cinematic Camera -> CineCam Grade`): the
  wheels and every grade value in one dockable window, bound to the selected camera, with fine
  control while dragging and one undo step per gesture. Every grade value can be animated in
  TrackView.

### Film grain

Capture-side film grain for the cinematic camera, replacing the engine's own overlay grain
whenever a cinematic camera drives the frame. It is a **Film** family with grain size given in
micrometres on the negative and a tone response that fades out near display white and nine stops
under it, plus **CMOS**, **CCD** and **Phone** families built on a real sensor-noise model (shot
noise, read noise, fixed-pattern noise) instead of a procedural texture. Grain is deterministic
per capture frame: a stateless hash of the shot seed, capture-frame index, view and cell, no wall
clock, so a rendered sequence grains the same way on replay. With the block off, engine grain is
untouched and the stock path is byte-identical. Deploy note: the repository's
`engine/Engine/Shaders/RunTime.ext` must be deployed as a loose file into the engine's `engine/shaders/`,
or the grain technique's runtime flag stays masked off and the grain block never compiles. Full spec:
`engine/Code/CryPlugins/CinematicCamera/FilmGrainSpec.md`.

### Area components (branch `area-components`)

Shapes and functions as entity components, next to the untouched legacy "Area" objects. One
shape per entity (Box, Sphere, Polygon, Spline) drawn and edited in the viewport from an
**Edit Shape** button; function components on the same entity use it: Area (the classic area
events, priorities and fade, linked through the stock Link tool), Trigger Bounds, Gravity Volume
and a Distributor that places meshes along a spline (chord alignment, stretch to fit, smoothing,
bake to brushes). The editor core gained a generic "component owns editable geometry" hook that
any component can use. Vis areas, portals and occluders stay as they are. See
`docs/AreaComponents-README.md`.

## Status

- Verified on a physically lit interior and on the sample airfield in daylight.
- Direct3D 11 only.
- Stock CRYENGINE content is lit in arbitrary units and looks wrong on the scene-referred path
  until it is re-lit in physical units. `docs/SceneReferredContent.md` explains how.
- The previous, display-referred pipeline is kept on the `stock-pipeline` branch and the
  `stock-pipeline-base` tag. The `film-grain` branch is now merged into `main`; it is kept as a
  historical branch.

## Documentation

| Document | What it covers |
| --- | --- |
| `docs/CinematicCamera-README.md` | Every camera and grade parameter, group by group. |
| `docs/ConsoleReference.md` | Console commands and cvars. |
| `docs/SceneReferredContent.md` | Lighting a scene in physical units, the physical environment preset, probes, HDRI import. |
| `docs/SceneReferredLook.md` | The colour chain, LUTs, looks, grading, the Resolve workflow. |
| `docs/SceneReferredExport.md` | EXR capture, the sidecar files, the Resolve round trip. |
| `docs/SceneReferredCalibration.md` | Checking the sky, sun and fog against measured values. |
| `docs/CryPhoneTracker-README.md` | The phone rig. |
| `docs/AreaComponents-README.md` | Shape and function components: creating, editing, every property, limits. |

## Installing it into CRYENGINE 5.7.1

You need your own CRYENGINE 5.7.1 source checkout and its third-party SDK drop; neither is part
of this repository.

1. Apply the changes to a pristine checkout, either as a patch series

   ```
   git am --keep-cr --3way path/to/ReC-Sandbox/patches/0*.patch
   ```

   or by copying `engine/` over the checkout and deleting what `engine/DELETED_FILES.txt` lists.
2. Configure with the engine's bundled CMake (`Tools/CMake/Win32/bin/cmake.exe`, Visual Studio
   17 2022, x64) and build, in the Profile configuration: `CryRenderD3D11`, `Cry3DEngine`,
   `CryEntitySystem`, `CryDefaultEntities`, `CinematicCamera`, `CryPhoneTracker`, and
   `CinematicCameraEditor` (the Sandbox side).
3. Copy the built DLLs into the engine's `bin/win_x64/` (`CinematicCameraEditor.dll` goes into
   `bin/win_x64/EditorPlugins/`), the changed `.cfx` / `.cfi` shaders into
   `engine/shaders/HWScripts/CryFX/` (the engine compiles loose shader files at startup),
   `Engine/Shaders/RunTime.ext` into `engine/shaders/` as a loose file (it lists the runtime flag the
   film grain needs; without it the grain never compiles in), and the `Assets/cinecam/` tree next to
   your project's assets.
4. Add the `CinematicCamera` and `CryPhoneTracker` plugins to your project's `.cryproject`.

## Repository layout

| Path | Contents |
| --- | --- |
| `engine/` | The modified and added engine files, in engine tree layout. |
| `patches/` | The same changes as a numbered patch series plus one cumulative `full.patch`. |
| `docs/` | The documentation listed above. |
| `tools/` | `ocio-bake/` (bakes the ACES LUTs and looks), `exr-check/` (EXR vs screenshot comparison), RenderDoc timing scripts, `bitcompare.py`. |
| `MANIFEST.md` | The engine revision this snapshot corresponds to and the list of changed files. |

## Related

The Android tracker app and a physical lens-control module (a 3D-printed body with magnetic
encoders for focus, zoom and iris) are separate projects and will be published on their own.

## License

Two-part, see [LICENSE.md](LICENSE.md): the modifications to CRYENGINE files remain Crytek's
copyright and are provided only for users who hold their own CRYENGINE license; the project's
original work (the plugins, documentation and tooling) is released under the MIT License.
