# How the fix actually works

The route to it is in [ghidra.md](ghidra.md) and [measurements.md](measurements.md).
This is only the result, in the order it happens at runtime.

## The chain

| Step | Where | What |
|---|---|---|
| 1 | AOB `kLetterboxStructAnchor` | locates the letterbox state struct |
| 2 | `anchor −0x08` | **weight** 0..1 — trigger *and* blend factor |
| 3 | `anchor +0x08` | bar height for the real display aspect → `k` |
| 4 | AOB `kFovGetter` | yields the address of the FOV master, used as a sanity check |
| 5 | AOB `kCameraApply` | `ApplyCameraState(dst, src)` — the hook site |
| 6 | `dst +0x60` | the vertical field of view, in degrees |
| 7 | `bars::set_hidden` | patches the anchor instruction's immediate from `FF` to `00` |

`k` comes from step 3, not from `GetSystemMetrics`: the game derives it itself
from the actual resolution, so it stays correct in windowed mode and with render
scaling.

## The correction

```
k        = 1 − 2 · (bar height / weight)
factor   = 1 + (k − 1) · weight            // smooth transition
vFOV_new = 2 · atan(factor · tan(vFOV_old / 2))
```

At 3440x1440, `k = 0.744186`, which is exactly `2560/3440` — the width of the
pillarbox window the game opens for cutscenes. The correction therefore maps the
original framing onto the full screen.

## The three rules in the detour

All three come from measurements, not from caution. Without them the result was
demonstrably wrong.

**1. Only the rendered camera.** `ApplyCameraState` runs for more than two dozen
camera states. Correcting all of them means correcting the sources of a blend --
and then its result as well. Which state gets rendered is given away by the
shader constant at `+0x3AE24B8`: it carries that state's value, one frame later.
The match is unambiguous, measured at 99% against 0%.

The detection is **sticky**: at a camera cut the shader constant jumps and for
one frame nothing matches. Without holding on, 4 frames out of 489 were skipped,
each visible as a step of twice the usual size.

**2. At most one correction per frame.** During a ramp a corrected value reaches
another state through the blend spring -- via arithmetic, which rounds away any
marker. Without this rule the correction ran two or three times per frame, giving
a result around 25% too narrow. The weight serves as the frame marker because
the game computes it exactly once per frame.

**3. A tag against exact copies.** The low eight mantissa bits carry a fixed
pattern (deviation < 0.0003°). That catches a corrected value passed on
unchanged -- the normal case at a settled weight.

## What did not work

So that nobody repeats it:

- **Hooking the getter.** It is read 235 times per second and still feeds only
  peripheral consumers (LOD, depth of field). No effect on the picture.
- **Overwriting the master global.** A read watchpoint shows that exactly one
  instruction in the entire game reads it: the getter. Dead end.
- **Finding the write site statically.** The store goes through a register
  (`movss [rbx+0x60], xmm0`), so Ghidra sees no reference. Only a hardware
  watchpoint found it.
- **A tolerance against recently written values.** Works in principle, but its
  correctness depends on ring size and threshold. At 512 entries it became a
  sieve that discarded 94% of all corrections.
- **Zeroing the bar heights so the 2D layer would lay out full screen.** No
  effect whatsoever, cleanly verified. The 2D layer takes its safe area from
  somewhere else.
- **Telling our own output from an authored value by asking whether it changed.**
  See below — the most instructive failure of the lot.

### Why "did the value change" cannot identify our own output

The single-frame flash comes from the ring of recently written values matching an
authored value by coincidence: our 39.3139 against another camera's authored
39.3141, 5e-6 apart, inside a tolerance that has to be loose because the value
comes back rounded.

The ring cannot simply be replaced by a per-camera test. It matches across
structures on purpose — it catches our own output when the game hands it to a
*different* camera state as input, and without that the correction compounds into
a threefold zoom.

So the idea was to keep the ring and add a discriminator: a copied value has
*changed*, an authored value has been sitting there all along. Nothing copies a
value that is already there.

Except that it does. The game copies one camera state into dozens of structures
and repeats it on every call — the log shows slots 3, 50, 25 and 27 all carrying
the same 39.3248 in the same frame. From its second call onwards a propagated
value is unchanged, and therefore indistinguishable from an authored one by
exactly this test. Measured result: 200 double corrections in one session,

```
MISS  w 1.0000  slot 3  we wrote 22.3802, the picture shows 39.3141
```

where 22.3802 is 39.3141 corrected twice, and the picture jitters frame by frame.

The premise is wrong, not the threshold, so no amount of tuning saves it. What a
future attempt needs is provenance that survives propagation — something in the
value itself, or a signal from outside the value. The tag in the low mantissa
bits was that, and rounding destroys it.

### What worked instead: separating the values

Detection failed twice, so the third attempt stopped detecting. If a correction
would land close enough to an authored value to be confused with it, it is moved
0.004° clear — ten times the ring's tolerance, with the round trip's own rounding
of about 1e-4 still comfortably inside. On a 39° field that is 1e-4 relative, a
fraction of a pixel at the frame edge.

The property that makes it safe is that it changes no decision at all. No branch
is taken differently; only the value written changes, by an amount nobody can
see. Both earlier attempts failed by creating a new way for the correction to
compound, and this one cannot: at worst it does nothing.

Measured over one session with four cutscenes: 13 separations, and the three
remaining MISS events all sat at a letterbox weight of 0.0003, 0.0000 and 0.0123
— the edge of the ramp, where the correction is a hundredth of a degree. The
session before, on the same scenes, the misses sat at full weight and were 11 to
17° wrong. That is the difference between a visible flash and bookkeeping.

### What the separation was tested against, and why it is off

Four builds, four measurements, no improvement — so it ships switched off, along
with two smaller rules tried in the same run. What the four runs established is
worth more than the code:

1. **The ring holds authored values.** At the start of a ramp the blend factor is
   barely below 1, so a "correction" returns its own input, and that goes into
   the ring as ours. Skipping corrections below 0.05° removes those entries.
   Measured: 68 skipped in one session, and **no change on screen**.
2. **The set of known authored values held 20 ms.** It was appended on every
   non-matching call — 1700 a second into 32 seats. Storing each distinct value
   once instead makes it last a whole cutscene. Measured: nudges rose from 13 to
   1629, and **no change on screen**.
3. **Our output for one camera can be another camera's authored value.** The
   screen trace of the affected cutscene flips between 39.3141 and 29.7728 five
   times in a tenth of a second, and 29.7728 is exactly what 39.3141 corrects to.
   The ring hit names the other side: 39.3141 had been written 15 ms earlier, as
   the correction of the *gameplay* camera, into a different slot. Two cameras
   whose fields of view sit a factor of k apart, which is why it is one cutscene
   and not all of them.
4. **And separating them does not fix it.** Offsetting every correction by
   0.004° so the two can never coincide changed nothing either.

Point 4 is the one that matters. If the coincidence caused the flip-flop, moving
the values apart would have stopped it. It did not, so something else decides
frame by frame whether that camera is corrected, and the value collision is only
how the decision becomes visible. Every value-based idea is looking in the wrong
place.

The switches are `kSeparateFromAuthored`, `kAlwaysOffset`, `kSkipTinyCorrections`
and `kIdentityTest` in `src/fov.cpp`, all false, all with their reasoning next to
them.

### What actually found it: tracing the decision, not the value

Logging every decision around the flicker — armed by the symptom itself, so the
sample budget is spent where it matters — settled in one run what four value
experiments could not.

The same camera across 155 frames, perfectly regular:

```
FLIP skip-own slot 25  in 39.3141 -> out 39.3141    every frame
FLIP CORRECT  slot 25  in 39.3131 -> out 29.7723    every frame
```

The structure is written **twice per frame**. The first value is a constant
39.3141 and is skipped as ours every time; the second drifts and is corrected.
Usually the second write saves the frame. Where it does not come, nothing is
corrected at all — and those frames are exactly the visible faults:

```
23:02:56.801  skip-own(39.3141)                      one frame  -> the flash
23:03:04.102  skip-own(39.3141), skip-not(27.9647)   dozens     -> the held jump
```

Both symptoms, one cause. And the ring-hit provenance names the writer: **the
focal clamp puts 39.3141 into the ring every frame**, as its correction of a
different camera, and that value happens to equal the cutscene camera's authored
field of view.

Two ways out, and the log chose between them:

- **Hide the clamp's writes from ApplyCameraState.** Tried; the picture came out
  too close. So the clamp's corrected value flows back into camera states, and
  without seeing it in the ring the other site corrects it twice. The ring has to
  stay shared. (`kSeparateRings`, false.)
- **Stop producing the value.** Switching the clamp's correction off removed the
  held jump and kept the width right — but the manual cinematic camera in free
  roam then took a visible moment to reach our value, because reaching a camera a
  frame earlier is what that site is for.

The resolution is that those two live at different weights. The poisoning was
logged at w = 1.0000, long settled; the cinematic camera needs the early
correction *during* a ramp. So the clamp corrects while the weight is moving and
stops once the scene has settled, where ApplyCameraState alone has always been
enough (`kClampCorrectsOnlyWhileRamping`). Its defence against the game's own
focal limiting runs in both cases.

Result: the held jump is gone, the cinematic camera is prompt again, and one jump
at the start of that cutscene's transition remains — inside the ramp, which is
where the clamp is still active. That is where to look next.

### Still open: the sustained jump at a cut inside the ramp

One cutscene shows two hard jumps rather than a flash, and the log says they are
a different animal:

```
CUT  w 0.9598  slot 29..44  in 37.7233 -> 20.5340  (delta -17.1893)
                            shader 37.7233   decision skip-own
```

A value arrives in a dozen structures at once, every one of them is waved through
as ours, the shader still carries the old camera, and the new value then holds
for 0.7 seconds — a whole shot, not a frame. It happens near the end of the ramp,
at weight 0.96.

Whether 20.5340 was ever ours is not answerable from that log: the CORRECT lines
run on a budget and none survives in that window. Settling it needs the ring
match itself logged — which entry matched, and when it was written.

## Open: the 2D layer

Without bars, 2D elements are too small -- they still lay out for the 2560x1090
window -- and the area the bars used to cover shows artefacts.

This is a separate piece of work of the same shape as the field-of-view hunt:
find the value, find the write site, find the intervention point. The cheap
route (zeroing the known bar heights) has been tried and does nothing.

Fallback at no cost: **F8** brings the bars back -- pillarbox and letterbox both,
since one patched byte controls the whole thing. Layout and artefacts are then as
the game intended and the FOV correction stays active. Not the goal of the
project, but a usable state.

## The 2D layer: what was ruled out

The cutscene 2D layer -- subtitles, credits, the intro's photographic filter --
is composed for the 2560x1090 window and placed into the 3440x1440 frame
unscaled. It therefore looks too small, and the strip the bars used to cover
shows horizontal smearing where the effect samples past the edge of its valid
region. One cause, both symptoms.

Four approaches were tried and none of them reached it. Recorded here so nobody
repeats them.

**Zeroing the bar heights.** The two floats in the letterbox struct are set to
zero every frame. No effect on the 2D layer whatsoever, verified with the toggle
logging what it overwrote.

**Viewport and scissor rects.** A RenderDoc capture of every draw call in a
cutscene frame: no viewport and no scissor anywhere carries 2560 or 1090. The 2D
layer is not boxed in by a rectangle -- its vertices arrive already scaled.

**Pixel history on an artefact pixel.** A broken pixel and a good pixel go
through the *same* passes in the same order. No pass is missing. The difference
is already present in the composite that feeds them.

**Searching memory for the geometry.** `hunt::find_known_values` looked for
2560, 1090, 440 and 175 as int32 and float across the whole of `.data`, plus
every derived ratio. The ratios do not occur at all -- so the layer works in
pixels, not in normalised factors. The pixel values that do occur are
coincidence: three of the four hits for 1090 sit inside an ascending index table
(1088, 1089, **1090**, 1091, ...), the fourth in a stride-16 enumeration, and the
440s sit among 680, 582.5, 497.5 in what looks like a distance table. The hit
list also changed between two runs.

**The constant buffers.** Every constant buffer bound to every draw that writes
the final image, scanned word by word as float and as int32, including the
reciprocals a shader would use for a screen size. 91 matches, so the scan
demonstrably worked -- and **2560 and 1090 do not occur once**. What the shaders
are told is the *full* screen: 3440 and 1440 appear 30 times between them, their
reciprocals 38 times.

Getting there took three attempts, and the first two reported "nothing found"
while reading zero bytes: the accessor is named differently in this version of
RenderDoc, and the errors were being swallowed. A tool that cannot say how much
it examined cannot produce a negative result. It counts now.

**Where that leaves it.** The shaders know the real screen size, so nothing is
telling the 2D layer to occupy only part of it -- the vertices must already
arrive scaled down. Vertex positions for 2D elements are computed on the CPU,
which puts the cause back in game code rather than in the renderer, and makes it
reachable by the same means as the field of view.

## The letterbox maths, decompiled

The function that writes the letterbox struct ends like this, with every
constant read out of the image and checked against the measured values:

```
aspect = GetAspectRatio()                       // 2.388889 at 3440x1440
bar(display) = (1 - (16/9) / aspect)     * 0.5 * weight   // 0.127907
bar(2.35)    = (1 - (16/9) / 2.35)       * 0.5 * weight   // 0.121749
```

Both reproduce exactly. That settles what the framing actually is: **the game
letterboxes a 16:9 window, not the display.** The pillarbox term is whatever it
takes to fit 16:9 into the real aspect, and the 2.35 term crops that window to
scope.

Which reinterprets the measurement this whole investigation started from.
`1440 x 16/9 = 2560`, exactly. The 2560x1090 the 2D layer lays out for is not a
cutscene rectangle at all -- it is the game's 16:9 box, cropped to 2.35:1. There
is no separate "cutscene 2D viewport" to find.

## Displays wider than the film frame

On 21:9 the corrected picture happens to fill the screen, because 2.3889 and the
2.35 the shots are composed for are the same thing to within a percent. Wider
than that they are not, and three parts of the plugin have to agree about what
to do.

**The correction stops at the film frame.** `framing::clamped_for_wide_display`
replaces `k = (16/9)/aspect` with `(16/9)/2.35` above
`framing::kUltrawideThreshold`, so the vertical field of view lands on the height
of the intended composition instead of continuing to shrink. This is not
optional and does not depend on the setting: 32:9 taken all the way would be a
strip of a picture.

Note what that leaves. A vertical field of view fixed at the composition's height
on a 3.556-wide screen produces `3.556/2.35 = 1.51` times the intended width --
the extra scene is *already there*, rendered, and the only question is whether to
show it.

**The side bars answer that question.** `ExpandCutscenesSideways = false`, the
default, writes `(1 - 2.35/aspect)/2` into the drawn bar width every frame, so
the composition is framed rather than extended. `true` leaves it alone and the
extra 51% is simply visible.

**The full-screen overlays follow the bars, not the aspect ratio.** The 16:9
overlay assets are mapped into whatever the picture around them occupies: the
2.35 window when the side bars are on, the full width when they are not. The
condition in `overlay.cpp` is `bars::side_bars()` for exactly this reason -- with
the sides expanded, an overlay stopping at the film frame would sit in the middle
with scene showing past its edges.

Everything above is gated on the display aspect, so a 21:9 or 16:9 machine never
enters any of it, and the setting cannot change what those displays already get.

## The 2D layer: two more dead ends, and what they narrowed down

**The bar heights have no consumer other than the bars.** Traced exhaustively
through xrefs rather than guessed. Each fraction is written by the letterbox
update, copied once into the second buffer (the struct is double buffered), and
read from that copy by exactly one function -- which calls a rectangle drawer
four times, twice with `(0 .. bar)` and twice with `(1-bar .. 1)`. That is the
bars being drawn and nothing else.

So the 2D layer never reads the bar geometry, and zeroing it could never have
worked. The earlier "no effect whatsoever" was the correct result for the wrong
reason.

**The script native.** One further reader exists for the horizontal fraction: a
nine-byte getter behind a wrapper that is only ever referenced as *data*, from a
table of 32-bit RVAs full of near-identical neighbours -- a native registration
table. The bar height is therefore exposed to the script layer, and RDR2 lays
its cutscene 2D out in script, which made this the best lead so far.

Patched to return zero (`src/safearea.cpp`, still available on Ctrl+Alt+S) and
tested through a full cutscene: **no change at all.** The patch demonstrably
applied -- 714 float getters scanned, exactly one identified by its RIP target,
logged -- so this is a real negative, not a missed attempt.

**What the game already has.** `FUN_7ff675604f38` maps a coordinate pair from
the 16:9 box onto the full screen, using `aspect * 9/16` as the scale and
recentring around 0.5. At 3440x1440 that scale is 1.34375, which is `1/k`, and
it sends 0.127907 to 0.0, 0.5 to 0.5, and 0.872093 to 1.0. This is precisely the
transform the 2D layer is missing -- and it exists, with three callers, none of
them the cutscene path.

Also found along the way: `+0xF0DB40` and `+0xF0DB44` hold the real backbuffer
size as two ints (3440 and 1440 in the dump), which is a better source than
`GetSystemMetrics` for anything that still needs one.

**The current lead.** If the 2D box comes from the aspect ratio, a getter that
reports 16:9 makes the box the whole screen. `GetAspectRatio()` is a nine-byte
`movss xmm0,[rip]; ret` with one padding byte behind it -- ten bytes, exactly
enough for `mov eax, imm32; movd xmm0, eax; ret`. `safearea::init_aspect()`
finds it by the value it reads rather than by a byte signature and refuses to
patch unless exactly one getter matches. Ctrl+Alt+A toggles it.

It is a probe, not a candidate fix: it feeds fifteen callers and disturbs the
bars as a side effect.

**What it measured.** The patch reaches the letterbox maths exactly as the
decompilation predicted -- confirmed end to end, from the struct rather than
from the picture:

```
before   bar(display) 0.127907   k  0.74419
after    bar(display) 1.000000   k -1.00000
```

Not zero, though. At a true 16:9 the pillarbox term computes to zero and the
game substitutes 1.0 for it:

```c
if (weight > 1e-06) {
    if (bar(2.35)    == 0.0) bar(2.35)    = 1.0;
    if (bar(display) == 0.0) bar(display) = 1.0;
}
```

Nothing of that was visible, for two good reasons: the bars are patched off
anyway, and `k = -1` fails the plausibility check in `fov.cpp`, which falls back
to computing `k` from the resolution. The correction therefore carried on
unchanged.

So the aspect getter demonstrably drives the letterbox. Whether it also drives
the 2D layout was still open at that point, because every test had toggled it
*during* a cutscene -- which only answers whether the aspect is read per frame.
If the 2D layer sizes itself once, when the cutscene is built, no later toggle
can reach it.

Repeated with the probe on from startup, so the layout was built under the faked
aspect in the first place: **no change.** Confirmed from the log rather than the
picture -- probe armed, patch holding, 120 frames at full letterbox weight.

`GetAspectRatio()` is therefore not the lever for the 2D layer either. That
retires the last lead this document carried.

## Where the 2D problem stands

Six approaches have now failed, all of them instrumented well enough that the
negatives are real: bar-height zeroing, viewport and scissor rects, pixel
history, memory search, constant buffers, the script native, and the aspect
getter.

What is known for certain:

- the 2D layer lays out in a **16:9 box** (2560x1440 here), cropped to 2.35:1
  for cutscenes -- there is no cutscene-specific rectangle
- it does not derive that box from the bar heights, nor from the aspect getter
- the vertices arrive already scaled, so the cause is CPU-side
- the game *has* the inverse transform (`FUN_7ff675604f38`, three callers), so
  whatever boxes the 2D layer must be findable as its counterpart

The untried instrument, and the one most likely to end this: **a RenderDoc
capture with callstack capture enabled.** That gives the CPU call stack of the
subtitle or credits draw directly. There are no symbols, so it will be raw
addresses -- which is all that is needed, since subtracting the module base
turns them into Ghidra offsets. Every approach so far has worked forward from a
value hoping to reach the draw; this works backward from the draw, which is the
one direction not yet tried.

Press **F8** to bring the bars back. The field-of-view correction stays active.

## Known limits

- At the camera cut ending a cutscene the field of view jumps, because the game
  cuts there itself (48.84° → 51.28°). The correction shrinks that jump; it does
  not create it.
- The offsets in `patterns::candidates` are raw addresses for 1.0.1491.50 and
  are wrong for any other build. Only the shader constant among them is used in
  normal operation; the rest exist for diagnostics.
