# Scene-Referred Export — writing the frame to OpenEXR

Stage S6 of `SceneReferredSpec.md`. Companion documents:
[SceneReferredLook.md](SceneReferredLook.md) (the display chain the export tap sits inside),
[SceneReferredCalibration.md](SceneReferredCalibration.md),
[ConsoleReference.md](ConsoleReference.md).

The scene-referred pipeline keeps everything between the lights and the output transform in scene
units. This is how that data leaves the engine as a file, rather than as an 8-bit picture of
itself.

---

## 1. Two taps, two different artefacts

| `r_SceneReferredExportTap` | What the file holds | What it is for |
| --- | --- | --- |
| **0 — graded** (default) | The value `HDRFinalPass` has immediately before the ODT fetch: white balance, ASC CDL, the look LUT, the bloom composite, the sun shafts and the vignette all applied — handed back to **linear ACEScg**. | The **acceptance instrument**. It is the only tap for which "apply the same ACES 2.0 output transform in Resolve and compare against the engine view" tests the output transform, rather than testing the output transform plus every operator in front of it. |
| **1 — negative** | `$HDRTarget` as it enters the tone map: exposed, converted to ACEScg, ungraded, unvignetted, un-composited. | The **deliverable**. What a colourist takes into a DI suite when the grade is going to happen there. |

They are not two settings of one thing. `rec/exportTap` in every file's header says which it is,
so nothing downstream has to ask a human.

The ACEScct encode is *not* in the file. It belongs to the ODT LUT's input domain — its shaper —
not to the grade, and an EXR of the log value would be a file no ACES tool could read without
being told about our private shaper.

---

## 2. Commands

```
rec_CaptureEXR [folder] [prefix]     start a sequence
rec_CaptureEXRStop                   stop it
rec_CaptureEXRFrame [name] [folder]   write exactly one frame and disarm
```

* `folder` — relative paths land under the **user folder**
  (`%USERPROFILE%\Documents\CRYENGINE\...` on a default install); absolute paths are taken as
  given. Created if it does not exist. Default `CaptureEXR`.
* `prefix` — default `frame`. Files are `<prefix>.%06d.exr`, dot-separated, which is what Nuke and
  Resolve auto-detect as a sequence rather than as a folder of stills.
* The frame counter belongs to the capture and starts at zero on every arm. It is **not**
  `capture_frames`, which `ManualFrameStep` and CryMovie also write.
* **`rec_CaptureEXRFrame` takes its arguments the other way round: name first, folder second.**
  Taking single frames is something you do repeatedly while looking at something, and what you
  want to say about one is what it is (`rec_CaptureEXRFrame dusk_wide`), not where it goes - the
  folder is the default nearly always. Making the name reachable without also typing the folder is
  the point of the asymmetry.
* **A single frame's numbering continues for the whole session**, per destination: the command
  uses the first free `<name>.%06d.exr`, so repeated captures come out `frame.000000.exr`,
  `frame.000001.exr`, ... and nothing already on disk - from this session or a previous one - is
  overwritten. Delete a file and its number is reused. A *sequence* still starts at zero, because
  a sequence is one artefact and its first frame is frame zero by definition.

Nothing is written unless the Cinematic Camera's **Scene Referred** path is actually running —
off it there is no pre-ODT buffer to tap.

### What a sequence does to the session

`rec_CaptureEXR` sets `t_FixedStep` (from `r_SceneReferredExportFixedStep`, default 1/30) and
restores it on stop. Outside the editor nothing else locks frame time — `capture_frames` does not,
whatever its name suggests — so without this the sequence is one file per rendered frame at
whatever rate the machine managed, and N frames is not N/fps seconds. An EXR sequence is a timed
artefact or it cannot be conformed.

It also pins `r_DepthOfFieldLensModelJitter` to `2` (frozen phase) for the duration and puts it
back afterwards. A sequence in which every frame carries a different sub-pixel dither is noise with
a picture behind it, and it is indistinguishable from TAA ghosting when someone later tries to work
out what went wrong.

### When it stops on its own

If the readback or the writer falls behind, the capture **stops and logs why**. It never drops a
frame: a hole in a sequence is invisible until someone tries to conform it, which is exactly when
it costs the most. At 1080p a frame is 15.8 MiB of raw half data and at 4K it is 63.3 MiB, so at 30 fps
this is a disk-throughput question, not a GPU one.

---

## 3. Encodings

| `r_SceneReferredExportEncoding` | Primaries | `chromaticities` | `acesImageContainerFlag` | In Resolve |
| --- | --- | --- | --- | --- |
| **0 — ACES2065-1** (default) | AP0 (converted from AP1 on the CPU during the write) | AP0 | **set** | Imports correctly with no per-clip input transform — an untagged EXR in an ACES project is *assumed* to be ACES2065-1 |
| 1 — ACEScg | AP1 — the buffer byte for byte | AP1 | **never** | Needs the clip's ACES Input Transform set to `ACEScg` by hand |

The container flag asserts SMPTE ST 2065-4 conformance, which is a claim about AP0 data. It goes on
the AP0 file and never on the AP1 one; putting it on an AP1 file would be exactly the "private
variant that makes exported EXRs non-standard" the spec's §3 table says to avoid.

Both are half float. The tap *is* an fp16 render target, so half is what the data is: float32 would
store fabricated precision at twice the size and force a conversion pass. RGB only — the tap's
alpha carries nothing.

Compression is `r_SceneReferredExportCompression` (default 3 = ZIP).

---

## 4. Metadata

Every frame carries its own header. The three that are load-bearing rather than decorative:

| Attribute | Why it matters |
| --- | --- |
| `rec/exposureScale` | The linear multiplier the pre-exposure applied on the light side. The whole pipeline rests on `1.0 = 10 000 cd/m²`, and the pre-exposure destroys that relation unless the scale travels with the file. Without it the EXR's absolute scale is **unrecoverable**. |
| `rec/exportTap` | `pre-ODT` or `HDRTarget`. The two look similar and mean completely different things. |
| `rec/workingSpace` | `ACES2065-1` or `ACEScg`, alongside the standard `chromaticities`. Two independent statements of the same fact, so a mis-set import is diagnosable from the file rather than from the picture. |

The rest: `owner`, `rec/specVersion`, `rec/schemaVersion`, `rec/frameIndex`, `rec/renderFrameId`,
`rec/engineTime`, `rec/fps`, `rec/cameraName`, `rec/focalLength_mm`, `rec/tStop`, `rec/iso`,
`rec/shutterTime_s` (also as the standard `expTime`), `rec/ndStops`, `rec/ev100`,
`rec/whiteBalance_K`, `rec/whiteBalance_tint`, `rec/cdl` (see below), `rec/curves` (see 4.2),
`rec/odt`, `rec/lmt`.

`rec/cdl` is the **effective** ASC CDL of that frame, in the ASC's own order and number format
(six fixed decimals): what the tone map really evaluated, not what the camera was asked for. The
colour wheels, contrast and the CDL base on the *CineCam Grade* component have already been folded
into one slope/offset/power triple by the time it reaches the bus, `Bypass Grade` folds in as the
identity — a bypassed frame carries `1 1 1 / 0 0 0 / 1 1 1 / 1 (bypassed)`, because that is what is
in the picture — and the renderer's own `1e-4` floors on slope and power are applied, so the header
describes the frame that was rendered rather than the numbers that were typed.

`rec/curves` is the same idea for the *CineCam Grade*'s two 1D curves — the master curve's five
knot offsets in stops and the five Sat vs Sat multipliers. It is written **only when a curve is
doing something**, which is honest here in a way it would not be for the CDL: a CDL is always
evaluated, while a neutral curve is genuinely not sampled at all. `Bypass Grade` takes the curves
out with the CDL, so a bypassed frame carries no `rec/curves` either.

---

## 4.1 The ASC `.cdl` sidecar

The header tells a *reader* what the grade was. The sidecar lets an *application* apply it.

Every capture also writes one ASC ColorDecisionList beside the frames:

| | |
| --- | --- |
| a sequence | `<folder>/<prefix>.cdl`, one per take, `id` = the prefix |
| a single frame | `<folder>/<prefix>.%06d.cdl`, matching the frame's own name, `id` = `<prefix>.<number>` |

It carries the first captured frame's effective CDL as `SOPNode` (Slope/Offset/Power) + `SatNode`
(Saturation), in `urn:ASC:CDL:v1.01`, with a `Description` naming the working space — a CDL carries
no colour space of its own, which is the format's oldest trap — and a `ViewingDescription` naming
the ODT the engine was showing it through.

```xml
<?xml version="1.0" encoding="UTF-8"?>
<ColorDecisionList xmlns="urn:ASC:CDL:v1.01">
  <Description>ReC Sandbox scene-referred capture - ASC CDL applied in ACEScct (AP1 primaries) before the output transform</Description>
  <ViewingDescription>Assets/cinecam/luts/odt_srgb_100nit_aces2_65.cube</ViewingDescription>
  <ColorDecision>
    <ColorCorrection id="take01">
      <SOPNode>
        <Slope>1.200000 1.000000 0.900000</Slope>
        <Offset>0.020000 0.020000 0.020000</Offset>
        <Power>1.000000 1.000000 1.000000</Power>
      </SOPNode>
      <SatNode>
        <Saturation>1.000000</Saturation>
      </SatNode>
    </ColorCorrection>
  </ColorDecision>
</ColorDecisionList>
```

**Getting it into Resolve.** `.cdl` and `.ccc` XML are two of the three formats Resolve's ColorTrace
accepts (the third is a CMX EDL carrying SOP in its comments). Individual files are imported on the
**Gallery** page — right-click in the *Stills* tab → **Import** — and then applied to a clip by
hand. Automatic per-shot linking needs an EDL whose metadata matches the timeline, which a
single-camera engine take does not have and does not need.

**Details worth knowing**

* **A neutral grade is still written.** A missing file cannot be told apart from a writer that
  failed; an identity CDL is a positive statement. `r_SceneReferredExportCDL 0` turns it off.
* **One CDL cannot describe an animated grade.** Every grade property is animatable in TrackView;
  if the grade moves during a sequence the sidecar keeps the **first** frame's numbers and the log
  says at which frame it stopped being the whole truth. Each frame's own `rec/cdl` is still correct
  for that frame.
* **Which tap you are holding decides what the sidecar is FOR.** With `r_SceneReferredExportTap 0`
  (graded) the CDL is already baked into the pixels and the file is provenance — what was applied,
  so it can be read, matched or undone. With `r_SceneReferredExportTap 1` (negative) the pixels are
  ungraded and the sidecar is the **starting point**: apply it in Resolve and the grade the camera
  operator dialled on the day is back, exactly.
* Only the numbers travel. The **look LUT** does not: `rec/lmt` names the `.cube` and the file
  itself has to go with the media.

`rec/tStop` and not `aperture`: the T-stop is what the exposure is actually computed from here, and
`aperture` is the classic attribute name to get wrong — a photographic reading (f-number) and a
projection reading (aperture size) both plausibly claim it.

`rec/lensName` is published **empty** until lens presets exist (a later item). An invented name
stamped on every frame would be worse than no name.

---

## 4.2 The curve sidecars

The ASC CDL cannot carry a curve, so the two curves travel as their own files — written once per
take, beside the frames, and **only when a curve is non-neutral** (`r_SceneReferredExportCurves 1`,
the default; `0` opts out). Unlike the `.cdl`, an absent file here is not ambiguous: the `.cdl` is
always written and always dates the take.

| file | what it is |
| --- | --- |
| `<prefix>.curve_master.cube` | the **master (luma) curve** as a Resolve 1D LUT (`LUT_1D_SIZE 1024`, three identical columns). A per-channel tone curve *is* a 1D LUT, so this one transfers mechanically. |
| `<prefix>.curves.txt` | both curves' control points in plain text, plus the chain order and the exact definition of the Sat vs Sat axis. |

Naming follows the frames, exactly as the `.cdl` does: `<prefix>.…` beside a sequence,
`<prefix>.%06d.…` beside a single frame.

**In Resolve**, on the negative:

```
node 1  ASC CDL          <- imported from <prefix>.cdl
node 2  1D LUT           <- <prefix>.curve_master.cube
node 3  Curves -> Sat vs Sat, dialled from <prefix>.curves.txt
```

**Why the Sat vs Sat curve is text and not a `.cube`.** It is a *multiplier* indexed by a per-pixel
saturation measure, not a per-channel value mapping. Written as a 1D LUT it would load, apply, and
be silently nonsense — the worst possible failure for an interchange file. Resolve has the control
natively, so what is useful is the numbers plus the definition of the axis they were dialled
against, which is what the `.txt` carries:

```
luma = dot(c, (0.2126, 0.7152, 0.0722))     the ASC CDL's own Rec.709 weights
sat  = saturate(length(c - luma) * 2.0)     c in ACEScct
out  = luma + (c - luma) * multiplier
```

Transferring that curve is the one part of the round trip that has to be matched **by eye**: no
interchange format carries a saturation-vs-saturation relationship, and Resolve's own measure is
not documented well enough to assume it is identical.

**One known limitation of the `.cube`.** A 1D LUT's domain is `[0, 1]`, and the engine extends the
master curve with slope 1 above ACEScct 1.0 (= +10.3 stops over mid grey) so that a capture keeps
highlight separation the sensor recorded. The `.cube` cannot carry that extension. It matters only
above +10.3 stops — nothing reaches the display up there anyway, since the ODT LUT's own domain
ends at the same place — so it is visible only in the EXR, and only with **Sensor Clip** off or
above ~10.

**An animated curve** has the same limit the `.cdl` does: one file describes one curve, so the
sidecars carry the take's first frame. Each frame's own `rec/curves` is still correct for that
frame.

---

## 4.3 Keeping a take's grade files as assets

The sidecars are written **beside the frames**, under `%USER%`, and deliberately not into the asset
tree: that is where a take belongs, and an EXR sequence is not an asset. When one of them is worth
keeping, adopt it explicitly:

```
cinecam_ImportGradeAssets              # from the CaptureEXR folder under %USER%
cinecam_ImportGradeAssets MyShow/take3 # or from any other folder
```

Every `.cdl` goes to `Assets/cinecam/cdl/` and every `.cube` (i.e. the `.curve_master.cube`) to
`Assets/cinecam/luts/`, each with the `.cryasset` metadata that makes it a registered asset - so
the CDL shows up in the Asset Browser under **ASC CDL**, and the master curve becomes a LUT you can
pick in an **LMT File** field. Any `.curves.txt` beside them travels along as documentation.
Frames are never copied.

Note what these files are: **exports**, in interchange formats. A `.cdl` is the folded result of
the wheels, contrast and the CDL base, and it cannot be unfolded back into wheels; the
`.curve_master.cube` is a sampled 1D curve, not the five knots. To carry a whole grade from one
camera to another - wheels, curves, LMT reference and all - use a **grade preset** instead
(`Export Preset` on the CineCam Grade component; see
[SceneReferredLook.md](SceneReferredLook.md) section 5).

---

## 5. Checking the result

### 5.1 The writer itself

```
r_SceneReferredExrSelfTest 1
```

Writes a 4×4 half EXR into the user folder, reads it back with tinyexr's own loader, and logs
whether the pixels round-tripped **exactly** (half in, half out — there is no conversion on this
path, so anything short of exact is a bug) and whether the header attributes survived. Run it once
after any change to the writer or its build wiring.

### 5.2 The automatable round trip — engine vs OCIO

`tools/exr-check/` in the ReC-Sandbox repository. It renders the EXR through the **same** ACES 2.0
display/view the shipped ODT `.cube` was baked from, using PyOpenColorIO, and compares against the
engine's own capture of the same frames.

```
uv run apply_view.py capture\exr --out capture\ocio_png --compare capture\tga
```

Budget: mean |d| < 0.15 code values, max |d| ≤ 2. What remains is LUT interpolation, the ACEScct
shaper's clamp on negatives, 10→8-bit truncation and dither. `tools/bitcompare.py` on the same two
directories gives the amplified difference image, which is what tells "one pixel off by one" apart
from "the whole frame moved".

The reference is computed by **someone else's** implementation of the transform, from a file that
left the process. An in-engine "ODT diff" mode would only compare the engine against itself.

Before capturing, kill everything temporal and lock the exposure — an unlocked AUTO produces a
mismatch that looks exactly like a transform bug:

```
r_displayInfo 0
r_AntialiasingMode 0
r_HDRGrainAmount 0
r_Sharpening 0
r_HDRDithering 0
r_HDRDebug 0
```

### 5.3 The deliverable round trip — engine vs DaVinci Resolve

This is the spec's own acceptance wording (D12) and the one test that exercises tagging, container
conventions and a third party's idea of what our file means.

1. **Project Settings → Color Management**
   * Color science: `DaVinci YRGB Color Managed` → ACES, or the direct `ACEScct` option — choose
     **ACEScct**, the grading space this pipeline picked.
   * **ACES version: 2.0.** If the installed Resolve offers only 1.x, **stop**: 1.x has exactly the
     hue skews the spec set out to avoid, and the comparison would be meaningless. Bake a 1.3
     `.cube` as a second preset and run the round trip on that matched pair instead, saying clearly
     that this is what was tested.
   * ACES Output Transform: `sRGB` (100 nit) — the display/view the shipped `.cube` was baked from.
   * Timeline resolution = capture resolution. No scaling anywhere.
2. **Import** the EXR sequence. With the default ACES2065-1 encoding the clip needs **no** input
   transform. With the ACEScg encoding, set the clip's ACES Input Transform to `ACEScg` by hand —
   and record that the file needed manual tagging.
3. **No grade.** No nodes, no LUT, no CDL, nothing at clip or timeline level.
4. **Deliver** → PNG, **8-bit**, Data Levels **Full**. A 16–235 "Video" render is the classic
   silent failure and shows up as a uniform offset across the whole frame.
5. Compare with `tools/bitcompare.py` against the engine TGAs. Budget: mean |d| < 0.5,
   max |d| ≤ 4 in smooth regions.
6. **Record the Resolve version and its ACES version** next to the numbers. The reference moves
   when Resolve updates.

### 5.5 The grade round trip — the `.cdl` sidecar

5.3 proves the *output transform* travels. This proves the *grade* travels, and it is the acceptance
for the CineCam Grade component's wheels and contrast.

The engine's scene-referred chain is
`negative -> vignette x bloom -> white balance -> sensor clip -> ACEScct -> CDL -> curves -> LMT -> ODT`.
Resolve, handed the negative and the sidecar, can do `ACEScct -> CDL -> ODT`. So the two agree
**exactly** once the operators Resolve was not given are neutral — which is the point of the test,
not a limitation of it:

1. Neutralise the camera composites: bloom off (`r_HDRBloom 0`), the camera's **Vignette Amount**
   at 0, `r_sunshafts 0` (the default), **White Balance** unticked, **LMT File** empty.
2. Dial a grade on the *CineCam Grade* component that uses the wheels — e.g. Gain `1.2 / 1.0 / 0.9`,
   Lift Master `+0.02`, Contrast `1.3`. Note the `[CinematicCamera] grade published:` line.
3. `r_SceneReferredExportTap 1` (the negative), then `rec_CaptureEXRFrame` and a matching engine
   screenshot. You get `frame.000000.exr` and `frame.000000.cdl`; the log line
   `[EXR] ASC CDL written:` repeats the same numbers as step 2 — that alone catches a fold that has
   gone wrong before Resolve is even opened.
4. In Resolve, set up as in 5.3, import the EXR, then **Gallery → Stills → right-click → Import**
   the `.cdl` and apply it to the clip. One node, nothing else. (Leave the **Curves** group neutral
   for this test — with a curve dialled the test needs the extra nodes of 4.2 and stops being a
   test of the fold.)
5. Deliver and compare as in 5.3. The budget is the same. A *uniform* difference means an operator
   that was not neutralised in step 1; a difference only in the saturated areas means the saturation
   coefficients disagree; a difference only in the deep shadows is the ACEScct toe and is expected
   to be small.

### 5.4 Reading a failure

The *shape* of the difference names the cause more usefully than its size does:

| What the diff looks like | What it is |
| --- | --- |
| uniform offset over the whole frame | data levels (Video vs Full), or an exposure mismatch |
| error growing with luminance | wrong input transform, or a gamma applied twice |
| error concentrated in saturated colour | the ACEScct shaper's negative clamp, or AP0/AP1 confusion |
| error in one channel only | chromaticities / white point mismatch |
| salt-and-pepper ±1 everywhere | dither on one side only |
| error localised at edges | an antialiasing or resize path is still on |
| error only where the vignette is dark | the vignette is inside the tap on one side and not the other |
| structured, obvious, moving content | the two files are not the same rendered frame |

---

## 6. What this does *not* touch

`capture_frames`, `ScreenShot`, `RT_ReadTexture`, `SCaptureFormatInfo` and
`ICaptureFrameListener` are exactly as the engine ships them. That is deliberate: they are 8-bit by
contract, they serve JPEG screenshots and TrackView batch renders, and the byte-identity harness
reads TGA written by precisely that code. The EXR capture is a parallel path, so
"ccam off → the engine is byte-identical to stock" stays trivially true.

---

## 7. Driving a capture from TrackView

CryMovie never captures anything itself: `CMovieSystem::ControlCapture()` only drives the
renderer's `capture_frames` family, and that path is 8-bit by contract. To get an **EXR sequence
out of a TrackView shot**, drive `rec_CaptureEXR*` from the sequence instead. No new code is
needed for this — the mechanism has been in CryMovie all along.

### 7.1 The recipe — a Console track

1. Select the scene node of the sequence and add a **Console** track.
2. Put a key at (or just before) the first frame you want, with the command:

   ```
   rec_CaptureEXR MyShot beauty
   ```

3. Put a key one frame after the last frame you want, with:

   ```
   rec_CaptureEXRStop
   ```

4. Play the sequence in the editor. `CAnimSceneNode::ApplyConsoleKey()` is a plain
   `ExecuteString`, so the arm and the stop happen exactly on their keys.

The exporter pins `t_FixedStep` to `r_SceneReferredExportFixedStep` (default 1/30) for the duration,
so the sequence renders at a fixed cadence whatever the frame rate is.

**An 8-bit TrackView capture and an EXR capture can run at the same time.** They number
independently — the EXR path deliberately does not reuse `capture_frames`' global counter — so one
playthrough gives you the review JPEGs and the conform EXRs together.

### 7.2 The one interaction that produces a *wrong* file

A TrackView sequence that does **not** carry the capture flag, but does carry a TimeWarp or
FixedTimeStep track, **zeroes `t_FixedStep` every frame** (`CAnimSceneNode`). Playing such a
sequence while an EXR capture is armed fights the exporter frame by frame, and the export silently
loses its cadence — the frames are all there, but they are not evenly spaced in scene time.

This is **not** guarded against today. Do not play an unrelated sequence with a TimeWarp or
FixedTimeStep track while a capture is armed; if a captured shot's timing looks wrong and nothing
else explains it, that is the interaction to suspect.

### 7.3 The batch-render dialog: use a Console key, **not** the item's cvar list

TrackView's batch-render dialog lets each render item carry a list of console commands, executed at
the start of the item. **`rec_CaptureEXR` typed there will arm and die on the same frame.**

`BegCaptureItem()` runs the item's cvar list and *then* calls
`gEnv->pRenderer->ResizeContext(...)` to set the item's output resolution. The exporter answers a
changed resolution with

```
[SceneReferred] capture stopped: the resolution changed under the capture
```

because its readback ring is allocated at one size and re-allocating mid-sequence can fail
mid-sequence. That message is correct behaviour, not a bug.

**Put the commands on a Console key inside the sequence instead.** The keys fire after the resize,
at a resolution that then stays put for the whole item.

Two smaller notes about the batch renderer:

* it enters game mode for the run, so the camera's editor↔game handover must complete before the
  first captured frame. The dialog's own resolution warm-up happens to cover this.
* there is **no video encoder in this tree** — the dialog's mp4 option needs an FFMPEG plugin that
  does not exist here, so every moving-image output from this editor is an image sequence. Which is
  what a scene-referred pipeline wants anyway: EXR → Resolve.

### 7.4 The same limit applies to `e_ScreenShot 1`

A high-res screenshot resizes the pipeline twice — up and back — and therefore **ends an armed
capture**, once each way, for the same reason. Take your screenshots outside the sequence. A 4096²
screenshot also allocates the whole scene-referred working set at 16.8 Mpx, roughly **+340 MiB over
stock** for the duration of one nested frame; that is where a scene-referred session runs out of
VRAM and a stock one does not.

`e_ScreenShot 3` / `4` (the terrain minimap) is a different case entirely: it is refused the
scene-referred path altogether, because a minimap is data and must not carry a camera's exposure,
white balance, grade and output transform. See `SceneReferredContent.md`.
