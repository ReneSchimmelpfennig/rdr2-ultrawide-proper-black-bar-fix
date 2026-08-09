# What is inside the unpacked image

Analysis of the validated dump (see [packing.md](packing.md)) in Ghidra,
2026-08-09, RDR2.exe 1.0.1491.50.

All addresses are **module offsets**. In the dump `ImageBase` is
`0x7FF6750A0000`; Ghidra address = base + offset.

## Functions and globals found

| Offset | What |
|---|---|
| `+0x3200E8` | `LetterboxUpdate()` -- computes weight and bars, once per frame |
| `+0x320545` | inside it: the store `mov byte [rip+X], 0FFh` (our AOB anchor) |
| `+0x174230` | `GetLetterboxWeight()` -- `return weight` |
| `+0x173964` | `GetAspectRatio()` -- `return g_aspect` |
| `+0x173ED4` | `GetFov()` -- `return g_fovMaster` |
| `+0x170028` | `ApplyCameraState(dst, src)` -- **the hook site** |
| `+0x19FBCC` | `GetViewportAspect()` -- width/height from the viewport, virtual call |
| `+0x1A1358` | `BlendAspectToCutscene(obj, aspect)` -- lerp with the letterbox weight |
| `+0x39751AC` | `g_letterboxWeight` (float) |
| `+0x39751B4` | the anchor byte, constant `0xFF` |
| `+0x395B458` | `g_aspect` (float) = **2.3888888** in the dump = 3440/1440 |
| `+0x3EA0BE0` | `g_fovMaster` (float), inside a camera state at `+0x3EA0B80 +0x60` |

## The letterbox computation, verified

`LetterboxUpdate()` ends with exactly what was measured:

```c
weight    = max(channel_a, channel_b);          // +0x39751AC
bar235    = (1.0 - (16/9)/aspect_235)        * 0.5 * weight;
barScreen = (1.0 - (16/9)/GetAspectRatio())  * 0.5 * weight;
g_anchor  = 0xff;                               // unconditionally, every frame
```

Constants from `.rdata`, read straight out of the dump:

| Address | Value |
|---|---|
| `+0x330C7A8` | `1.0` |
| `+0x3311378` | `1.7777778` = **16/9** |
| `+0x330F3D4` | `0.5` |
| `+0x330F388` | `1e-06` (epsilon) |

With that, `(1 - k)/2 = 0.127907` is no longer an interpretation but something
read off. The anchor byte is set to `0xFF` unconditionally as the last statement
-- which is why it was constant across 293 measured frames. Not a measurement
error; that is how it is built.

## Negative result: the weight does not lead to the FOV

`GetLetterboxWeight()` has exactly **three** callers, and none of them sets a
field of view:

1. `+0x1A1358` -- lerp of an **aspect ratio** against the cutscene value. The
   base of that lerp comes from `GetViewportAspect()`, so it is width/height, not
   a field of view.
2. `+0x6F1910` (`*param_1 = weight`) -- writes the weight into a buffer, looks
   like a shader constant or a script native.
3. a jump with no associated function.

The only caller of (1) compares two blended aspects and derives a bool from them
-- not camera code.

**Consequence:** the letterbox weight feeds the bars and the aspect
interpolation, not the camera. The path "xrefs on the weight → camera" is
exhausted.

## Rejected: searching via the degrees-to-radians constant

`0.017453292` appears nine times in `.rdata` -- five of those as a group of four,
i.e. as an XMM vector constant for SIMD. The most interesting scalar is at
`+0x3311368`, right next to `0.999999` (`+0x331136C`) and `16/9` (`+0x3311378`)
which the letterbox code uses; same constant pool, presumably the same
translation unit.

**Useless anyway:** the xrefs are capped at 40 and clearly far beyond that. An
engine converts degrees to radians in hundreds of places. That is not a filter.
The `rad2deg` constant (`+0x330F200` ff.) behaves the same.

Do not try again.

## Followed: the object trail

| Offset | What |
|---|---|
| `+0x3EA04C8` | `g_cinematicMgr` -- singleton pointer, returned by `GetCinematicMgr()` |
| `+0x44037C` | `GetCutsceneAspect(mgr)` = `*(float*)(*(mgr+0x568)+0x80)` |

`GetCutsceneAspect()` has two callers, `GetAspectRatio()` fifteen. Every path
examined ends at an **aspect ratio**, never at a field of view. The `+0x80` field
of the sub-object at `+0x568` is an aspect, not a focal length.

So this route is exhausted too: the entire letterbox and cinematic branch deals
in aspects and bar heights. The camera FOV is set elsewhere.

## Result of the differential search

`.data` was scanned, 35.6 MB. Pass 1 in gameplay found 3860 floats in the degree
window and 11395 in the radian window. After comparing against a cutscene and the
following frame, **two candidates each** remained that change every frame:

| Offset | Gameplay | Cutscene | +1 frame | Window |
|---|---|---|---|---|
| `+0x39B06E4` | **45.000** | 48.840 | 26.991 | degrees |
| `+0x3AE24B8` | **45.000** | 48.840 | 26.991 | degrees |
| `+0x3A11250` | 1.000 | 0.598 | 0.567 | radians |
| `+0x3A11254` | 1.000 | 0.801 | 0.820 | radians |

The two degree candidates carry identical values, so they are two copies of the
same value. Exactly `45.000` in gameplay and a change in every cutscene frame --
the profile of a camera field of view.

The two radian candidates sit next to each other and are both `1.0` in gameplay.
That looks like a scale pair (x, y), not an angle. Set aside.

### Where the degree candidates come from

`+0x3AE24B8` is filled in a per-frame function like this:

```c
DAT_7ff678b824b8 = *(float *)(index * 0x690 + 0x7ff678f61050);
_DAT_7ff679fdf270 = DAT_7ff678b824b8;   // four times in a row -> XMM broadcast
```

So an **array of view structures with a stride of `0x690`**, based around
`+0x3EC0B00`. The value is stored four times side by side, like a shader
constant. `+0x3AE24B8` is therefore only a copy, not the source.

`+0x39B06E4` is written in a different per-frame function from a getter
(`+0x173ED4`, `return DAT_7ff678f40be0`), immediately followed by a
`fabs(new - old) < eps` comparison. That is change detection; the value itself is
again only a cache.

### Why the source is not in the candidate list

`+0x3EA0BE0` lies inside the scanned range but does not show up. That fits the
finding: the search compares **fixed addresses** over time. A value that travels
through an array or is written indirectly falls through that net -- the fixed
copies do not. Those are exactly what we found.

Xrefs on `+0x3EA0BE0` show only readers, no writer: the value is set through a
computed address that Ghidra does not resolve statically.

## Proven: the FOV is found

There is no FOV slider in the game menu, so the test used the **binoculars** --
continuous zoom, hence a continuous FOV sweep rather than discrete steps. The
result is unambiguous:

| State | Value |
|---|---|
| Normal gameplay | **51.282°** (89 of 225 measured lines) |
| Slightly different (moving, riding) | 52.183° |
| Binoculars raised | 22.620° |
| Binoculars fully zoomed | **8.578°** |
| Maximum across the run | 63.589° |

A value that drops to a sixth when zooming in is a field of view. That settles it
without anything having to be written.

### The order of the three addresses

`+0x3EA0BE0` is the **master**. The two copies follow it; in 10 of 225 lines they
lag exactly one frame behind, otherwise all three are identical. At startup the
master was still `0.0` while the copies already carried `45.0` -- so the 45 was
an initial value from the menu, not the gameplay FOV.

| Offset | Role |
|---|---|
| `+0x3EA0BE0` | **master**, returned by `GetFov()` (`+0x173ED4`) |
| `+0x39B06E4` | copy used for change detection |
| `+0x3AE24B8` | copy from the view array, broadcast as a shader constant |

### It is the vertical FOV

Not measured directly, but the arithmetic allows only one reading:

| Assumption | Consequence at 3440x1440 |
|---|---|
| 51.282° is **vertical** | hFOV = 97.8° -- plausible |
| 51.282° is horizontal | vFOV = 22.7° -- impossibly narrow |

This matches the design note: RDR2 is Hor+, the vFOV stays constant and the hFOV
grows with the aspect ratio. That is exactly the quantity
`framing::corrected_vfov_deg()` needs, and in **degrees**, which is what the
function takes.

### Settled: the radian candidates

`+0x3A11250` and `+0x3A11254` move independently of the FOV. Their magnitude
`sqrt(x²+y²)` varies between 0.35 and 1.0 -- a direction vector is plausible but
not established. Irrelevant to the fix, not pursued.

## Second independent confirmation: the focal length formula

The second reader of the master (`+0xF98FE8`) computes:

```c
t = tanf(master * 0.5 * 0.017453292);   // tan(vFOV/2), constant = deg2rad
f = 24.0 / (t + t);                     // clamped to <= 9999
```

`24 / (2·tan(FOV/2))` is the photographic focal length in millimetres, and
**24 mm is the height** of a 35 mm frame (36×24). Independently of the aspect
arithmetic, this confirms the value is the **vertical** FOV, as a full angle, in
degrees.

It also means a change in FOV drags depth of field along with it -- this focal
length presumably feeds the DOF computation.

## The view structure

| Offset in the structure | Content |
|---|---|
| `+0x000` | view position (x, y, z, w) |
| `+0x548` | unknown |
| `+0x550` | **FOV** -- source of the shader constant |

Base `0x7ff678f60b00` (module offset `+0x3EC0B00`), stride **`0x690`**, index
comes from the TLS block. An array of view constant buffers, presumably one per
eye/cascade/frame-in-flight.

## Result of the getter hook: no effect on the picture

The hook on `GetFov()` (`+0x173ED4`) demonstrably runs -- the log shows
`50.0000 -> 25.0000` -- and the picture stays unchanged. The observation next to
it explains why:

| Value | Behaviour with the hook active |
|---|---|
| `degA` (`+0x39B06E4`) | **halved**, in 279 of 284 lines exactly `getter/2` |
| `degB` (`+0x3AE24B8`) | unchanged, at the full value |
| Master (`+0x3EA0BE0`) | unchanged |

So the getter feeds only `degA`, the copy used for change detection. The view
structure, and with it the shader constant, hang off a different branch.

**Two things follow:**

1. The getter is finished as an intervention point. Its ten callers are
   peripheral (LOD, depth of field), not the projection.
2. **Arxan does not mind a MinHook trampoline** in `.text` -- no crash, nothing
   written back, the plugin kept running. That answers the question left open in
   [packing.md](packing.md): detours are viable in this code region, byte
   patching is not required.

The intervention has to sit **above** the master: at the code that writes it,
before the projection matrix and the view structure are built from it.

## Third-party mods as a source: checked, nothing for the write site

### "RDR2 display mods" / "RDR2 FOV - Widescreen Mod" (PCGamingWiki)

Archive password `pcgw`. The current version 3.5 is blocked by Windows Defender
as a virus or unwanted software and was therefore **not** read -- no exclusion
added, Defender not disabled.

Version 3.4 from 2019 could be analysed:

| Property | Value |
|---|---|
| Architecture | **x86**, GUI |
| Toolchain | Delphi |
| Imports | only SHLWAPI, KERNEL32, USER32, ADVAPI32 |
| Resources | 5.83 MB of 5.89 MB total, compressed |
| Embedded PE module | another **x86** executable, 220 KB |

**A 32-bit process cannot hook into 64-bit RDR2.** The mod therefore does not
change the FOV at runtime through memory access; it patches game files, and the
"button press" writes a configuration that takes effect on the next launch. It
does not know a write site in the code -- it does not need one.

Caveat: the compressed resource block was not unpacked, so an x64 module could in
theory still be in there. Given the overall picture (Delphi installer, sister mod
replaces game files) that is unlikely.

### "Custom First Person FOV + No Black Bars" by vStar925

A Lenny's Mod Loader mod that replaces
`update:/x64/data/metadata/cameras.ymt`. That file is **plain XML**, 46,884
lines, and it independently confirmed our measurements:

- The included readme states explicitly: *"This is VERTICAL FOV, not
  horizontal"* -- a third independent confirmation after the aspect arithmetic
  and the focal length formula.
- `<Fov value="51.30000000"/>` appears dozens of times. That is **exactly our
  measured gameplay value of 51.282**. The value in memory is the one loaded from
  this table.
- Other values: 37.8 / 27.0 / 18.2 for other cameras, `BaseFov` 50.0.

**Decisive for this project:** `cameras.ymt` contains **no cutscene camera FOV**
-- only `CutsceneBlendSpringConstant` and `CutsceneBlendSpringDampingRatio`.
Cutscene focal lengths are set per shot in the cutscene data, not in the camera
table.

That proves what the project assumed from the start: **a static file swap cannot
correct cutscene framing.** The runtime intervention is not convenience, it is
necessary.

## Found: the write site

Static analysis could not deliver it; a hardware watchpoint could.
`watchpoint::find_writers()` set DR0 on 19 threads and logged for 30 seconds:

```
armed on 19 thread(s)
watchpoint: 347 hit(s) recorded
  distinct writer(s): 1
    module +0x17007B     347 hit(s)
```

**Exactly one writer.** The instruction before it is `F3 0F 11 43 60` =
`movss [rbx+0x60], xmm0` -- a store through a *register*. That is precisely why
Ghidra found no reference to `+0x3EA0BE0`: there is no RIP-relative one.

The structure follows from it: `rbx = +0x3EA0B80`, and the FOV sits at **`+0x60`**
inside it. Immediately before, `minss xmm0, xmm2` clamps the value.

### The function

`+0x170028`, which we call `ApplyCameraState(dst, src)`. It copies a camera state
field by field, clamping as it goes:

```c
fVar3 = *(float *)(src + 0x60);          // source FOV
... clamping against 1.0 and an upper limit ...
*(float *)(dst + 0x60) = fVar4;          // our store
```

Dozens of further fields follow -- position, rotation, DOF parameters. It is the
camera's commit function, and it runs exactly when the game fixes the camera
state: early enough for the projection, which reads it immediately afterwards.

### Signature

```
48 89 5C 24 08 57 48 83 EC 20 F3 0F 6F 42 30 41
```

Unique at 16 bytes, and **entirely inside the prologue of the target function** --
unlike `kFovGetter`, which borrows its uniqueness from the tail of a neighbouring
function. Correspondingly more robust against game patches.

### Why the hook runs after the original

The detour calls the original first and then corrects `dst + 0x60`. That way our
value wins against the game's clamp, and it is in place before the projection
reads it.

Which destination gets corrected is decided at runtime -- see
[how-it-works.md](how-it-works.md). Correcting all of them was wrong: during a
transition the rendered camera is the blend of two others, so correcting its
sources and then the blend compounds the correction.
