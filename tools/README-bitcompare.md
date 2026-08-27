# Byte-identity harness

The scene-referred pipeline has one hard rule: with the switch off, the engine renders exactly
what a stock build renders. This is how that is checked, at every review.

Reference: `SceneReferredSpec.md` section 6 (stage S0),
`scene-notes/decisions/s0-byte-identity-harness.md`.

There are two different claims, and they need two different instruments:

| Claim | Instrument | Answers |
| --- | --- | --- |
| The presented image is unchanged | `VisRegTest` captures + `bitcompare.py` | "does the user see the same picture" |
| The pipeline is unchanged | RenderDoc + `rdc_diff.py` | "same pass list, same render-target formats, same pixels at every stage" |

Neither substitutes for the other. Identical back buffers with a changed render-target format
means we got lucky on this scene; an identical pass list with different back buffers means a
shader or a constant moved.

---

## Pieces

| File | Where it must live | What it is |
| --- | --- | --- |
| `visregtest.xml` | `S:\Crytek\crytek\cryengine-gamesdk-sample-project2\5.7.1\gamesdk\visregtest.xml` | The capture ceremony. `CVisRegTest::LoadConfig()` loads `PathUtil::GetGameFolder() + "/" + <file>`, and the game folder for this project is `gamesdk`. The copy in this directory is the master; copy it over after editing. |
| `bitcompare.py` | here | Hashes and pixel-diffs two directories of captures. `uv run bitcompare.py A B`. |
| `rdc_diff.py` | here | Compares two `.rdc` captures: marker regions, draw counts, render-target formats and sizes, and optionally a per-pass hash of every bound colour target. |

---

## Comparison 1 - stock build vs `scene` build, switch off

This is the acceptance criterion.

Both sides must match in everything that is not the thing under test: same machine, same GPU
driver, same level and level revision, same window resolution, same `sys_spec`, same
`r_AntialiasingMode`, and the same shader cache state (rebuild or clear both).

Run in the **launcher**, not the editor. The editor forces `eAT_SMAA_1X` and skips the
game-mode passes, so an editor comparison does not exercise what a player sees.

1. Deploy the **stock** `CryRenderD3D11.dll` (or launch a build made from `dev`), start the
   sample project, open the console and run

       VisRegTest byteid_stock

   Captures land in `%USER%/TestResults/VisReg/byteid_stock/singleplayer/woodland_<n>.bmp`
   (`%USER%` is the project's `user/` directory). The files are named `.bmp` and contain TGA:
   the engine looks the extension up in `GetCaptureFormatByExtension()`, `bmp` is not one of
   `tga`/`jpg`/`png`, and the fallback is TGA. Lossless, which is all that matters -
   **never compare a JPEG**, and every convenient screenshot path in the engine defaults to
   JPEG.

2. Deploy the `scene` build's `CryRenderD3D11.dll`, make sure `r_SceneReferred 0` (or simply
   have no cinematic camera in the level - the request is what drives the path, the cvar only
   refuses it), and run

       VisRegTest byteid_scene_off

3. Compare:

       cd ReC-Sandbox\tools
       uv run bitcompare.py ^
         "S:\Crytek\crytek\cryengine-gamesdk-sample-project2\5.7.1\user\TestResults\VisReg\byteid_stock" ^
         "S:\Crytek\crytek\cryengine-gamesdk-sample-project2\5.7.1\user\TestResults\VisReg\byteid_scene_off"

   `RESULT: BYTE-IDENTICAL` (exit code 0) is the pass. Anything else prints, per image, the
   number of differing pixels and the max and mean absolute difference per channel, and writes
   an amplified difference PNG.

4. Capture the same frame in RenderDoc on both sides, then

       set RDC_HASH_PASSES=1
       "C:\Program Files\RenderDoc\qrenderdoc.exe" --python rdc_diff.py <capture_dir> stock.rdc scene_off.rdc "stock" "scene, switch off"

   The report goes to `<capture_dir>\rdc_diff_report.txt`. Required results:
   **no marker region only in A or only in B, equal draw counts, no texture whose
   `name WxHxD mips arr FORMAT` key differs**, and - with `RDC_HASH_PASSES=1` - "every hashed
   region produced identical target contents". If not, the report names the first region whose
   colour targets diverge, which is the pass that broke it.

---

## Comparison 2 - `scene` build, switch off vs switch on

This proves the switch is wired, and that it changes only what it is supposed to change. Same
build, ideally the same session.

    VisRegTest byteid_scene_on          (with a cinematic camera active and Scene Referred on)

Then the same two tools. This pair is **expected to differ**, and the differences must be
exactly the intended ones. After S0 that means: the seven low-quality HDR satellite targets
(`$HDRTargetPrev[0..1]`, `$SceneTargetR11G11B10F[0..1]`, `$HDRFinalBloom`,
`$WaterVolumeRefl[0..1]`) change from `R11G11B10F` to `R16G16B16A16F` and nothing else does.
`rdc_diff.py`'s "Textures ONLY in A / ONLY in B" sections read this out directly, because the
format is part of the key. The picture itself should be very close but not bit-equal: fp16
removes banding in the darks.

---

## Things that will waste a day if forgotten

- **Measure the noise floor first.** Run comparison 1 with build A against *itself*, twice, in
  two separate process launches. If that is not byte-identical, GPU scheduling on this driver
  is not deterministic and the criterion has to relax to a measured tolerance. Do this before
  trusting any other result.
- **Fixed timestep, seeded RNG, streaming settled, adaptation settled** - all four, every time.
  `visregtest.xml` gets all four for free from the engine: `eCMDOnMapLoaded` sets
  `t_FixedStep 0.033333` and seeds both RNGs, `eCMDWaitStreaming` waits for streaming and then
  64 more frames "for tone-mapper to adapt".
- **Nothing drawn over the picture.** `gamesdk.cryproject` sets `r_displayInfo 1`; the config
  block in `visregtest.xml` turns it off along with the profiler and the viewfinder overlays.
- **No temporal accumulation and no time-driven grain** - also in that config block
  (`r_AntialiasingMode 0`, `r_HDRGrainAmount 0`).
- **Never compare across resolutions or `sys_spec`.** Render-target sizes, the pass list and
  (because format is decided at first allocation) possibly the formats all move.
- **Comparing the `scene` build against itself with the switch toggled is not comparison 1.**
  It proves the switch is wired, not that the stock path is untouched. Both are needed.

## Viewpoints

The six `Sample` lines in `visregtest.xml` are the woodland level's own SpawnPoints raised to
eye height, each looking a different way. To use a viewpoint of your own: stand there in game,
type `goto` in the console, and paste the numbers into a `Sample location` attribute -
`playerGoto x y z wx wy wz`, degrees. Every screenshot the engine writes logs its own `goto`
line, so a viewpoint can always be recovered from the log of the run that produced it.
