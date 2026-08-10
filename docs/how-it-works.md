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
