# Making a look for the scene-referred camera

What the camera does to colour, in what order, and how to put your own look into it.

Design: [../SceneReferredSpec.md](../SceneReferredSpec.md) (stages S4 and S5, decisions D8, D9,
D10). Property reference: [../README.md](../README.md) - groups **White Balance** and **Output**
on the Cinematic Camera, groups **Grade** and **Look** on the **CineCam Grade** component you add
to the same entity.

---

## 1. The chain

With **Scene Referred** on, the frame leaves the lights in scene units and stays there until the
very last step:

```
  lights, sun, sky, emissive                 real luminance
    -> the camera's exposure (T-stop, shutter, ISO, ND)      18% grey lands on 0.18
    -> Rec.709 -> ACEScg                     the working space, wide gamut, fp16, no clamps
    -> the optics: DOF, bloom, glare, halation, ghosts, the front filter
    -> vignette
    ---------- everything above knows nothing about a display ----------
    -> WHITE BALANCE   Bradford adaptation from the camera's Kelvin/tint
    -> ACEScct         the grading log space
    -> ASC CDL         (in * slope + offset) ^ power, then saturation
    -> MASTER CURVE    one tone curve, all three channels       [optional]
    -> SAT vs SAT      saturation multiplier vs saturation      [optional]
    -> LOOK (LMT)      your .cube, ACEScct in and out          [optional]
    -> OUTPUT (ODT)    ACES 2.0, ACEScct in, display code values out
    -> dither -> the screen
```

Which control is where: the camera owns **White Balance** and the **Output** transform - what the
camera is and what its monitor is - while the **ASC CDL**, the two **CURVES** and the **LOOK** slot
live on a separate **CineCam Grade** component you add to the same entity.

The order of those three is not ours: it is the fixed processing order every on-set grading desk
enforces (Pomfort Livegrade: input transform, then primary adjustments = CDL and saturation, then
secondary adjustments = curves, then output transform). Livegrade's reason for fixing it is ours
too - it is what makes a CDL + LUT export possible at all. With nothing in front of the CDL, the
`.cdl` a capture writes describes the **front** of the chain exactly, and the curves become the
node after it. A look is then a thing you add, copy to
another camera and delete. Nothing about the chain above changes with it: the camera is still the
only thing that talks to the renderer, and it reads the grade component when it publishes. With no
CineCam Grade on the entity the CDL is neutral and the look slot is empty, which is the same
picture as an untouched component - pure ACES 2.0.

Two properties of that picture are worth stating plainly, because they are what the whole
pipeline is for.

**Only the ODT knows a display exists.** Everything before it is "how much light is in the
scene". That is why opening the aperture recovers detail in the clouds instead of moving a
white blob around, and why a saturation push has somewhere to go.

**With nothing set, you get ACES 2.0 and nothing else.** No house look, no hidden correction, no
"ReC flavour" baked into the renderer (design decision D10). If you want a look, it is a file you
can load, inspect and delete.

---

## 2. The output transform (ODT)

The ODT is the one file that decides what the monitor shows. It ships with the plugin:

```
Code/CryPlugins/CinematicCamera/Assets/cinecam/
    luts/
        odt_srgb_100nit_aces2_65.cube      <- the default: ACES 2.0 SDR 100 nit, sRGB display, 65^3
        odt_srgb_100nit_aces2_33.cube         the same view at 33^3 (8x smaller, ~1/3 code value worse)
        odt_rec1886_100nit_aces2_33.cube      Rec.1886 - the reference for a Resolve round-trip
        lmt_identity_33.cube                  a pass-through look, for proving the plumbing
        look_film_contrast_33.cube            shipped look: soft S-curve
        look_warm_print_33.cube               shipped look: warm shadows, cooler highlights
        test_identity_rec709_33.cube          test fixture: an identity TAGGED as a 709 display LUT
        system/
            odt_srgb_100nit_aces2_inv_65.cube  the INVERSE of the default - machinery, see section 3
    cdl/                                       ASC CDLs (see SceneReferredExport.md)
    grades/                                    CineCam Grade presets (section 5)
```

Every one of those files is a **CryEngine asset**: it has a `.cryasset` beside it, it appears in
the Asset Browser under **Colour LUT (.cube)**, and it can be picked. The **ODT File** field opens
the Asset Browser filtered to LUTs, in both the project and the engine root - so a cube installed
only into the engine tree can be picked as well as loaded.

**Drop a file in and it registers itself.** Copy a `.cube`, a `.cdl` or a `.cinegrade` into
`Assets/cinecam/` (any depth) while the editor is running and, within a second or two, its
`.cryasset` appears beside it and the file is in the Asset Browser and in the pickers. Dragging a
file from Explorer onto the Asset Browser does the same and copies it in for you, as does
**File → Import**. Files copied in while the editor was closed are picked up the next time it
starts. Three things this never does: it never rewrites a `.cryasset` that already exists (that
file carries the asset's identity), it never touches anything outside `Assets/cinecam/`, and it
never runs the Resource Compiler - so **you never need "Generate/Repair Metadata"** for a grade
file. If a file ever does not show up, `cinecam_RegisterAssets` on the console registers everything
under `Assets/cinecam/` that is not registered yet, one sidecar per file, and leaves the rest alone.

Vendor LUT names are taken as they are, dots and all. The Asset Browser's *Name* column cuts a name
at its first dot (`XT3_FLog_FGamut_to_ETERNA_BT.709_33grid_V.1.01.cube` is listed as
`XT3_FLog_FGamut_to_ETERNA_BT`), but the file name on disk, the picker and the loader all use the
whole thing - the label is the only thing that is short.

`luts/system/` holds files that are machinery, not choices: the inverse output transform exists so
that a Rec.709 look LUT can be wrapped (section 3) and nobody should ever select it. It is a
separate folder because a per-type picker cannot hide one file.

Levels saved before the folder move keep working: an `Assets/ODT/...` path that no longer resolves
is retried under `Assets/cinecam/luts/` and the console says so. Re-pick the file to store the new
path.

### The LUT's domain is the real upper clip

Everything the ODT sees is ACEScct, and `ACEScctToLinear(1.0) = 222.86` - **+10.3 stops over mid
grey**. The 3D LUT is sampled with a `saturate()` on its input, so every scene value above that
lands on the same corner texel: one flat display white, per channel. That is a property of the
shaper, not of the tonescale, and it sets a hard ceiling on what any control upstream can express
on screen. In particular **Sensor Clip** above ~10 stops is invisible in the picture (it still
shapes the EXR tap, which is taken before the LUT). If highlight separation above +10.3 stops is
ever wanted on the display, the lever is the **shaper range** of the baked cube, not the sensor.

### Installing them

The plugin resolves a LUT path through the pak system: **first as given**, relative to the
project's asset directory, **then under `%ENGINE%`**. Copy the whole `Assets/cinecam` folder into
either tree and the default path `Assets/cinecam/luts/odt_srgb_100nit_aces2_65.cube` resolves in both the
editor and the launcher.

* per project: `<project>/<asset dir>/Assets/cinecam/luts/`
* engine-wide: `<engine>/engine/Assets/cinecam/luts/`

A **successful** load logs the grid size and the absolute path it came from:

```
[SceneReferred] ODT LUT 'Assets/cinecam/luts/odt_srgb_100nit_aces2_65.cube' loaded: 65^3, texture id 42,
                from 'S:/.../gamesdk/Assets/cinecam/luts/odt_srgb_100nit_aces2_65.cube'
```

The absolute path is there because "not found" and "found in the other tree" are indistinguishable
in a relative path, and those two trees are exactly the pair that gets confused.

If the file cannot be found or parsed, the reason is a **warning** (`[SceneReferred] ODT LUT '...'
NOT loaded: ...`), listing both absolute paths that were tried, and the picture goes deliberately,
visibly wrong - a flat, milky, clamped log image. That is not a bug to chase; it is the
missing-ODT state, made impossible to mistake for a correct picture.

An **empty** ODT field is not that state. It is a field nobody has filled in, so the shipped
default (`Assets/cinecam/luts/odt_srgb_100nit_aces2_65.cube`) is loaded instead: ticking Scene Referred on
a camera that has never been touched gives a correct picture, not a diagnosis.

### Baking another one

`tools/ocio-bake` in the ReC-Sandbox repository does the baking, from the OCIO built-in ACES
studio config. It is a uv project; it needs no engine and the engine needs no OCIO.

```
uv run list_config.py            # what displays and views the config actually has
uv run bake.py --list
uv run bake.py --preset odt_srgb_100nit --size 65
```

Every baked file carries a provenance comment block - config URI and resolved name, ACES version,
input space, display, view, grid size, OCIO version, bake time, the tool's git hash - and a
`.report.txt` measuring the trilinear interpolation error against the live transform. Read the
tool's README before adding a preset; the display and view strings are exact and the tool refuses
to bake against one it cannot find.

**A different ODT changes only what the monitor shows.** It never touches scene data, so it never
changes what an EXR export contains.

---

## 3. Making a look (LMT)

A Look Modification Transform is a creative grade that lives *before* the output transform. It
sees scene data in ACEScct, so it can move exposure and saturation around without fighting a tone
curve that has already been applied.

### The three looks that ship

Put one of these in **Look -> LMT File** on the entity's **CineCam Grade** component:

| file | what it is |
| --- | --- |
| `Assets/cinecam/luts/lmt_identity_33.cube` | **neutral** - the identity. Also the plumbing test: the picture must not change at all. |
| `Assets/cinecam/luts/look_film_contrast_33.cube` | a soft S-curve about mid grey, a little contrast and a little more colour |
| `Assets/cinecam/luts/look_warm_print_33.cube` | warm shadows, cooler highlights, mild contrast, slightly softer colour |

**None of them is a mystery.** Open one in a text editor: the comment header at the top lists the
exact CineCam Grade settings that produce it - Lift, Gamma, Gain, Contrast, Saturation, the five
master-curve offsets in stops, and the single ASC CDL they all fold into. Type those numbers into
the component and you get the same picture, live. They are built out of nothing but the controls
the component already has, deliberately: they exist to be readable and to prove the slot works. A
look worth shipping on a show is authored in Resolve and comes back as its own cube.

They are baked by `tools/ocio-bake` from a recipe in `presets.toml`, so changing one is editing a
few numbers and re-running the tool - not editing 35 937 lines of grid.

### In Resolve

1. Set the project colour management so the timeline is **ACEScct**. (Colour Science: ACEScct, or
   a colour-managed timeline with ACEScct as the working space.) Getting this wrong is the one
   mistake that matters - see the next section, which is about what to do when you get it wrong
   or when somebody hands you a LUT that was made that way.
2. Grade a still or a clip until you like it. Use nodes freely; the export flattens them.
3. Right-click the clip -> **Generate 3D LUT (Cube)**, 33-point.
4. Save the `.cube` next to the ODTs, e.g. `Assets/cinecam/luts/lmt_myshow_33.cube`.
5. Put that path in **Look -> LMT File** on the entity's **CineCam Grade** component (add one if
   the camera has none). The "..." button opens a `*.cube` file dialog.

The LUT is loaded and uploaded on the next frame after the path changes. A parse error is logged
once, with the reason, and leaves the slot empty (identity) rather than stale.

### Dropping in an ordinary Rec.709 look LUT

Most LUTs in the world - everything you can buy, everything that came with a camera, everything
somebody sends you - are made for a **finished Rec.709 display picture**. They expect display code
values in and give display code values out. This chain hands the LMT slot **ACEScct** instead.

Both are three numbers in [0,1] on a unit domain, so a 709 LUT loads without complaining and
produces a wrong picture *silently*. There is nothing in the `.cube` format to tell them apart -
the format has a title, a grid size, a domain and comments, and no metadata at all.

So there are two ways for a file to say what it is, in this order:

1. **A line in the file**: `# ReC-LUT-Space: Rec709`. Our bake tool writes one into everything it
   makes, and `wrap.py` writes one into everything it converts. If a file has one, it wins -
   whoever made the LUT knew better than whoever loaded it. Accepted values, case-insensitive:
   `ACEScct`, `ACEScg` (or `Linear`), `Rec709` (or `Rec.709`, `sRGB`, `Display`). You can add the
   line to a third-party LUT by hand; it is just a comment and no other tool will mind.
2. **The `LMT Input Space` property**, right above the file field. This is for the ordinary case:
   a LUT with no tag and no prospect of getting one. Set it to **Rec.709 display** and the LUT
   simply works.

If the file says one thing and the property says another, the file wins and the console says so,
naming the file. If the file carries a space name we do not know, you get a warning and the
property is used.

**What "simply works" means.** The plugin wraps the LUT in the output transform and its inverse -
the standard recipe (it is what ACES's own guidance says to do with a legacy show LUT, and what an
inverse-ODT node in front of a LUT node does in Resolve or Baselight):

```
   ACEScct  ->  the ACES 2.0 SDR 100 nit sRGB output transform
            ->  your Rec.709 LUT
            ->  the inverse of that same output transform
            ->  ACEScct
```

evaluated once on the CPU when the file loads, onto a 65-point grid. Nothing is added to the
per-pixel cost, and nothing about the rest of the chain changes.

The output transform used for the wrap is **fixed** at the sRGB SDR one, deliberately. Which
monitor a look LUT was made on is a property of the look, so pointing your camera at a different
ODT must not change what that look means.

**A LUT tagged `ACEScg`** (linear in and out) is wrapped the same way with the analytic
`linear <-> ACEScct` shaper instead of the output transform. Note the honest limit: a unit-domain
*linear* cube covers scene linear 0 to 1 and nothing above, while the top of ACEScct is 222.86 -
so a linear look LUT really wanted a shaper, which this chain does not accept. Everything above
linear 1.0 gets the LUT's effect *at* 1.0, carried on as a constant gain.

### How exact is the wrap?

The inverse of an SDR output transform is not a true inverse. Colour outside the display gamut is
folded inwards on the way out and cannot be unfolded coming back, and everything above display
white shares one code value. Two consequences worth knowing:

* **An identity 709 LUT gives an exact identity LMT.** That is not luck - the wrap is applied as a
  *difference* rather than as a straight substitution, so the two inverse lookups cancel exactly
  when the LUT does nothing. Measured over the whole grid, at 33 and 65 points, with both the
  engine's own evaluator and OCIO's exact one: **maximum error 0.000000000**. `test_identity_rec709_33.cube`
  ships so you can see this in the picture: load it with the space set to Rec.709 and nothing may
  change.
* **On real content the wrap is worth under half a code value.** Against a third-party-style 709
  look LUT, comparing what the engine shows with the wrap against applying the LUT to the ODT
  output directly, over colour inside the Rec.709 gamut and inside the display's range: mean 0.44
  8-bit code values, 99th percentile 3.5. (Wrapping offline with `wrap.py` and OCIO's exact
  inverse instead: 0.34 and 3.0 - i.e. almost all of what is left is the grid, not the method.)
* **Above display white the wrap does not reproduce what the LUT does to white.** Instead of
  flattening every super-white value onto `LUT(white)`, it carries the LUT's white-point shift up
  as a constant gain and lets the output transform clip. That keeps a highlight separating on
  screen instead of going flat, and - because the EXR export tap is taken *after* the LMT - keeps
  the highlight data in an export. A print look that pulls white down still rolls towards its
  lower white.

**For a look you are going to ship, wrap it once offline instead**, and load the result as an
ordinary ACEScct LMT:

```
cd tools/ocio-bake
uv run wrap.py --lut theirlook.cube --space rec709 --size 65
```

The result carries the source file's sha256, the transform chain and the method in its header, and
a `# ReC-LUT-Space: ACEScct` line, so it never gets wrapped twice. The engine says so in the log
whenever it wraps something at load time.

### Editing a look while the engine runs

`.cube` files hot reload. Save over the file the LMT (or ODT) slot is using and the picture follows
within about half a second - no property to touch, no level reload. Author on one monitor, watch on
the other.

* On by default in the editor, off in the launcher. `cinecam_LutHotReload <frames>` changes the
  poll interval; `0` turns it off.
* A change has to be seen twice before it is acted on, so a half-written file is never loaded.
* If the new file does not parse, **the previous good look stays in the picture** and one warning
  says why. A path you *type* that does not parse still clears the slot - that one you asked for
  and need to see.
* Every reload logs one line.

### Proving the plumbing first

Load `Assets/cinecam/luts/lmt_identity_33.cube` into the CineCam Grade component's Look slot. The picture must not change at all -
that one test exercises the parser, the fp16 conversion, the 3D texture upload, the sampler
addressing and the channel order together. If it *does* change, the look you author next will be
wrong in the same way and much harder to see.

Then load `Assets/cinecam/luts/test_identity_rec709_33.cube` with **LMT Input Space** set to *Rec.709
display*. The picture must not change either - and that one test exercises the whole wrap on top.

### What the parser accepts

Resolve-dialect `.cube`: comments, `TITLE`, `LUT_3D_SIZE`, optional `DOMAIN_MIN` / `DOMAIN_MAX`,
then N^3 triplets with the **red index varying fastest**. Grid sizes 2..129. One comment is read
rather than skipped: `# ReC-LUT-Space: <value>`.

It refuses, with a message saying why:

* `LUT_1D_SIZE` - a 1D curve is not a display transform.
* a domain other than `[0,1]` - the shaper is analytic in the shader and hands the LUT ACEScct in
  `[0,1]` by construction, so a cube on another domain is not a transform for this chain.
* a sample count that does not match the declared grid.

There is no embedded 1D shaper in any of our files and none is needed: `linear -> ACEScct` happens
in the shader, exactly, rather than through a sampled curve.

---

## 4. Grading in the engine instead

For anything short of a full look, the **CineCam Grade** component is faster than a round trip
through Resolve, and it is live - the picture moves as you drag. Add it to the same entity as the
Cinematic Camera; without one the camera renders the neutral grade, which is exactly ACES 2.0.

White Balance is the exception in the table below: it stays on the camera, because it says what
the camera thinks the light is rather than what the colourist wants.

| want | reach for |
| --- | --- |
| the shot is too warm / too cool | **White Balance -> Color Temperature** (and **Tint** for green/magenta). Camera semantics: the number is what the camera is told the illuminant IS, so a shot reads *cooler* as you dial the Kelvin DOWN and warmer as you dial it up - the opposite of a "colour temperature" slider on a finished image. What the transform guarantees is that clipped highlights stay neutral either way. |
| lift the shadows without touching the highlights | **Lift** up (or **Lift Master** for all three channels at once) |
| brighter / darker highlights without touching black | **Gain** (or **Gain Master**) |
| brighter or darker mids only | **Gamma** (or **Gamma Master**) - above 1 opens the mids |
| more contrast with mid grey left where it is | **Contrast** above 1. It turns about **Contrast Pivot**, whose default 0.4136 is ACEScct(0.18), so a grey card does not move |
| more contrast in the shadows / the highlights instead | the same, with **Contrast Pivot** lowered or raised |
| a colour cast to remove | **Lift** per channel in the shadows, **Gain** per channel in the highlights |
| more or less colour | **Saturation** |
| a film S-curve - crushed toe, rolled shoulder, mid grey untouched | **Curves -> Master Shadow** down and **Master Highlight** up (start at ±0.5 stops). Unlike Contrast this does not have to be symmetric, and it cannot move black or white unless you tell it to |
| a film shoulder only - highlights roll off, nothing else moves | **Master Highlight** down, everything else 0 |
| the mid tone brighter without moving black or white | **Master Mid** - a thing the aperture cannot do |
| tame ONE screaming colour and leave the rest | **Sat @ 100 %** down. A global Saturation cannot do this: it takes the pastels with it |
| lift the colour out of an ordinary shot without the neons going plastic | **Sat @ 50 %** up and **Sat @ 100 %** down |
| clean a faint cast out of the near-neutrals | **Sat @ 0** down |
| reproduce a `.cdl` somebody sent you | type it into **CDL Slope / Offset / Power (base)** with the wheels and contrast left neutral - those three fields are then exactly what the camera publishes |
| see it ungraded | **Bypass Grade** |
| the wheels, as wheels | **Tools -> Cinematic Camera -> CineCam Grade** |

**The grading panel.** Everything in that table is an inspector property and can be typed. It is
also all in one window: **Tools -> Cinematic Camera -> CineCam Grade** shows the three Primaries
wheels as hue/saturation discs - offset direction is the hue, distance is the strength, the centre
is exactly neutral, the master sits beside each disc, and a double-click or a right-click puts a
wheel back to neutral - with contrast and pivot, saturation, both curves, the ASC CDL base and the
look slot below them. The panel grades **the single selected entity's** CineCam Grade and greys
itself out when the selection is anything else; it is a view of the component, so a value changed
in the inspector (or by an undo, or by a preset) appears in it by itself, and one drag of a wheel is
one Ctrl+Z. Export Preset and Re-apply Preset stay on the component, in the inspector's Preset
group.

All of it folds into a **single** ASC CDL in the ASC's own order - `(in * slope + offset) ^ power`,
then saturation with the ASC's Rec.709 weights - so it means the same thing here as it does in
Resolve, Baselight or Nuke, and so a capture's `.cdl` sidecar reproduces the engine picture exactly
(see [SceneReferredExport.md](SceneReferredExport.md)). The wheels are those same ASC numbers
written the way a colourist reads them: `slope = gain - lift`, `offset = lift`, `power = 1 / gamma`,
which is exactly why lift holds white put and gain holds black put. Contrast about a pivot is
affine, so it folds into the same slope and offset with nothing left over and needs no extra step in
the shader.

The curves are a separate group because they are the part that a CDL cannot carry: everything in
**Grade** folds into one ASC CDL, and everything in **Curves** does not. They are applied after the
CDL and before the look, they cost nothing at all while neutral (nothing is uploaded and no fetch
is issued), and their neutral is an exact identity - a master offset of 0 stops and a saturation
multiplier of 1. A capture writes them beside the `.cdl` as their own files; see
[SceneReferredExport.md](SceneReferredExport.md).

Every property here is **animatable in TrackView**: right-click the entity node in a sequence ->
*Add Track* -> *Components* -> *CineCam Grade* -> *Grade* or *Curves* -> the property. The RGB triples arrive as
three-channel vector tracks and the scalars as float tracks. One CDL cannot describe an animated
grade, so an EXR capture's `.cdl` sidecar carries the grade of the take's first frame - each frame's
own numbers are still in its own EXR header.

Every neutral value is an **exact** identity, not a near one. That matters: it means you can
always get back to "pure ACES" by typing the defaults, and that the switch itself can be proved to
change nothing when nothing is set.

---

## 5. Saving and reusing a look (grade presets)

A **grade preset** is the whole CineCam Grade in one file: the three wheels and their masters,
Contrast and its pivot, Saturation, the ASC CDL base, both curves, the LMT reference with its input
space, and the bypass flag. It is an asset like anything else - `Assets/cinecam/grades/*.cinegrade`,
XML, readable and diffable in a text editor.

**To save one:** in the CineCam Grade component, open the **Preset** group and press
**Export Preset**. It writes `Assets/cinecam/grades/<entity name>.cinegrade`, registers it as an
asset, and points the Grade Preset field at it. The console names the file.

**To reuse one:** on any other CineCam Grade, in any level, pick it in **Preset -> Grade Preset**.
Every value is copied into the component at once and the picture follows immediately.

**It is a copy, not a link.** Once a preset is applied the component owns the numbers: trimming a
wheel afterwards changes the shot and never the file, and re-opening the level does not re-read the
preset. The path stays in the field as a note of where the look came from. If you edited the file
on disk and want the new version, press **Re-apply Preset**.

Two consequences worth knowing:

* the LMT path travels with the preset, so a preset that references a look LUT only works where
  that LUT is installed - both are assets, so ship them together;
* a preset written by a newer build than the one reading it is refused with a message rather than
  half-applied.

`.cdl` files and the 1D `.cube` a capture writes are assets too, but they are **exports**, not
presets: a `.cdl` is the folded result and cannot be unfolded back into wheels. To bring a take's
files into the asset tree, run `cinecam_ImportGradeAssets` (with no argument it looks in the
CaptureEXR folder) - it copies every `.cdl` and `.cube` it finds into `Assets/cinecam/cdl/` and
`Assets/cinecam/luts/` and writes their metadata.

---

## 6. What is not available while this is on

The stock engine's LDR grading is skipped whole (design decision D9), and one line in the log says
so when the switch flips. Inert: the colour chart, the saturation / contrast / brightness sliders,
the stock vignette, sharpening, and the engine film grain. They graded a picture that had already
been squeezed onto a monitor - which is the thing this pipeline exists to get away from.

Still running: PostAA, the HUD, the final blit, dither, and every one of the camera's own optical
effects.

---

## 7. When the picture looks wrong

| symptom | look at |
| --- | --- |
| flat, milky, washed out, low contrast everywhere | no ODT loaded - check the log for `ODT LUT ... NOT loaded` and check the path |
| a global colour cast that will not go away | **Tint** or **Offset** left off neutral; or a look LUT still in the slot |
| the whole picture is too bright or too dark | that is exposure, not grading - the **Exposure** group, and `r_HDRDebug 1` for the applied and metered EV |
| a stock slider does nothing | expected; see section 6 |
| **Sensor Clip** changes nothing at high values | expected above ~10 stops - the ODT LUT's domain ends at +10.3 and clips there first. Work in 4-8, where a real sensor lives |
| it looks different from before the switch | expected; the level is lit for one path or the other. See [SceneReferredContent.md](SceneReferredContent.md) |
| a look LUT makes the picture violently wrong - crushed, posterised, wrong colour | it is almost certainly a **Rec.709 display** LUT loaded as if it were ACEScct. Set **LMT Input Space** to *Rec.709 display* |
| the console says a LUT is being *loaded RAW* | the wrap needs `Assets/cinecam/luts/system/odt_srgb_100nit_aces2_inv_65.cube` and cannot find it. Install the whole `Assets/cinecam` folder, or re-bake it with `uv run bake.py --preset odt_srgb_100nit_inv` |
| the console says *THE FILE WINS* | the cube carries its own `# ReC-LUT-Space:` line and it disagrees with the property. Either fix the property or edit the line out of the file |
| a cube copied into `Assets/cinecam/luts/` does not appear in the browser or the picker | run `cinecam_RegisterAssets` on the console; it says how many files it registered. If it says 0, the file is not under `Assets/cinecam/` in the **project's** asset tree (the engine tree is deliberately not watched - see section 2) |
| editing a cube on disk changes nothing | `cinecam_LutHotReload` is 0 (the launcher default). Set it to 30 |
| a cube edit made the picture go away for a moment and come back | it did not - a reload that fails keeps the previous LUT. Look for the warning; the file was probably half written and the next poll picked it up |

`r_SceneReferredDebug 1` skips the working-space conversion so you can A/B the primaries change on
its own; **Bypass Grade** A/Bs the grade without touching the exposure or the optics.
