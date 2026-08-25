# ReC-Sandbox

Virtual cinematography for CRYENGINE 5.7.1: a physically based cinematic camera
(`CinematicCamera` plugin), a phone-driven 6DoF camera tracker (`CryPhoneTracker` plugin,
paired with the `androidtracker` app) and the renderer work they need (hybrid sprite bokeh with
shape masks, iris diffraction streaks, anamorphic lens model, lens character, T-stop exposure).

This repository is the **publishable snapshot** of that work. Development happens in the local
engine tree next to it (`../CRYENGINE_Source`, branch `dev`, baseline `main` = pristine
CRYENGINE 5.7.1); after every reviewed stage `sync.ps1` regenerates everything here from that
repository, and the result is committed.

## Layout

| Path | Content |
| --- | --- |
| `engine/` | Every engine file added or modified, in CRYENGINE tree layout. Copy over a pristine 5.7.1 checkout (`Code/`, `Engine/`, `Tools/`) to get the full change set. `DELETED_FILES.txt` lists files removed from the tree. |
| `patches/` | The same changes as a `git format-patch` series (one file per commit, apply in order with `git am --keep-cr --3way`) plus `full.patch`, one cumulative diff against pristine 5.7.1. |
| `docs/` | Plugin READMEs, the console/cvar reference and the design specs, copied from the engine tree. |
| `MANIFEST.md` | Which dev commit this snapshot was taken from, its commit log and the changed-file list. |
| `sync.ps1` | Regenerates the three trees and the manifest from the dev repository. |

## Applying to a pristine CRYENGINE 5.7.1

```
git checkout <pristine 5.7.1>
git am --keep-cr --3way path/to/ReC-Sandbox/patches/0*.patch
```

or copy `engine/` over the checkout and delete what `engine/DELETED_FILES.txt` lists. Then
configure with the bundled CMake (`Tools/CMake/Win32/bin/cmake.exe`, Visual Studio 17 2022, x64)
and build `CryRenderD3D11`, `CinematicCamera` and `CryPhoneTracker` in the Profile configuration.
The third-party SDK drop (`Code/SDKs`) is not part of this repository.

## Related repositories

- `../androidtracker` - the Android ARCore tracker / lens fader app that feeds `CryPhoneTracker`.
- `../Virtual-Lens-Module` - the physical camera-control module (hardware, firmware) that will
  integrate with the tracker as its controller.

## Syncing a new stage

```
powershell -ExecutionPolicy Bypass -File sync.ps1
git add -A
git commit -m "sync: <stage name> (dev@<hash>)"
```
