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

Fallback at no cost: **F8** brings the bars back. Layout and artefacts are then
as the game intended and the FOV correction stays active -- correct framing with
letterboxing. Not the goal of the project, but a usable state.

## Known limits

- At the camera cut ending a cutscene the field of view jumps, because the game
  cuts there itself (48.84° → 51.28°). The correction shrinks that jump; it does
  not create it.
- The offsets in `patterns::candidates` are raw addresses for 1.0.1491.50 and
  are wrong for any other build. Only the shader constant among them is used in
  normal operation; the rest exist for diagnostics.
