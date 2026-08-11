# Next session: the 2D layer

Everything else works. This is the one open problem.

## Correction of aim: the symptom is the overlays, not the subtitles

The subtitles were a probe I chose because they were convenient, not the thing
that actually bothers anyone. The symptoms that matter are the **full-screen
overlays in the intro** and the **credit inserts**. Whether they share a cause
with the too-small subtitles was never more than an assumption of mine.

## Settled: it is not the geometry

`tools/dump_overlay_geometry.py` classified all 870 draws of the artefact frame
by their clip-space extent. 437 write the final image, 251 had readable meshes.

```
event 7875  verts 4  x [-1.00000 .. +1.00000]  y [+0.75650 .. +1.00000]  FULL SCREEN
event 7876  verts 4  x [-1.00000 .. +1.00000]  y [-1.00000 .. -0.75650]  FULL SCREEN
event 7877  verts 4  x [-1.00000 .. -0.74419]  y [-1.00000 .. +1.00000]
event 7878  verts 4  x [+0.74419 .. +1.00000]  y [-1.00000 .. +1.00000]
```

Those four are the bars: 0.756502 is `1 - 2*0.121749`, and 0.744186 is `k`.
**Nothing else in the frame is confined to the box.** The overlays are drawn
across the whole screen; they only look wrong outside the middle.

So the cause is not the shape of the geometry, and this also confirms the pixel
history from earlier, which was recorded and then not followed up: a broken
pixel and a good pixel take the same passes in the same order, and the
difference is already present in what feeds them. Something an overlay *reads*
is only correct inside the box.

The next step is `tools/dump_render_targets.py`, which asks the two questions
nobody has asked yet: does any render target in the frame have the size of the
framed window, and what do the final full-screen draws actually sample. Constant
buffers were searched for 2560 and 1090 and came back clean, but the textures
themselves never were.

## Settled: the whole ultrawide-UI family is dead code

The lead described below was the best one the investigation had, and it is
finished. Both functions were hooked and counted through a complete cutscene:

```
--- uibox: the cutscene just ended (the count that matters) ---
uibox: box transform 0 call(s), 0 distinct caller(s); align variant 0 call(s)
```

Zero, on both, in gameplay and in the cutscene alike. In the same run the
field-of-view detour fired ten times and corrected normally, which proves the
hooking machinery worked -- so this is a measurement, not a failure to measure.

That retires `FUN_7ff67526bd30`, `FUN_7ff67526bda8` and the
`SET_SCRIPT_GFX_ALIGN` family with them, and it explains the earlier byte patch
that changed nothing: the code never ran.

It also closes the pattern that has been forming. The bar-height native, the
aspect getter and now the alignment family are all reachable from script and all
irrelevant to the cutscene 2D layer. **RDR2's cutscene 2D does not appear to go
through the script-gfx path at all.** Every remaining forward approach is
looking in that direction, which is why the only sensible next step is the one
that works backwards.

Go to **Plan A**.

The census is still in `src/uibox.cpp` but is no longer installed at startup;
`Ctrl+Alt+U` installs it for anyone wanting to repeat the measurement.

## Retired: the boxing transform

Plan C below was meant to be the slow, low-ranked option. It produced the
answer on the fourth function.

`FUN_7ff67526bd30` (module `+0x1CBD30`) is the transform that puts the UI into a
16:9 box:

```c
if (aspect > 16/9) {
    k     = (16/9) / aspect;            // 0.744186 at 3440x1440
    *pos  = 0.5 - (0.5 - *pos) * k;     // == *pos * k + (1-k)/2
    *size = *size * k;
}
```

It sends 0 to 0.127907, 0.5 to 0.5 and 1 to 0.872093 -- the exact inverse of
`FUN_7ff675604f38`, the box-to-screen mapping found earlier. Between them the
two halves of the game's ultrawide UI convention are now accounted for.

Three things make this the strongest lead the investigation has had:

1. **It is the transform itself**, not a value suspected of feeding one. Every
   earlier attempt started from a number and hoped it led to the layout.
2. **It reaches the script layer.** Its only caller, `FUN_7ff67526bf94`, is
   referenced as *data* at `0x7ff67ad71d28`, and `bd30` itself at
   `0x7ff67ad71d10` -- adjacent entries in a native table. This is the
   `SET_SCRIPT_GFX_ALIGN` family, and cutscene subtitles and credits are drawn
   by script.
3. **It explains the aspect probe's negative result.** The aspect here comes
   from `FUN_7ff6752741b8`, which computes width/height itself from the
   backbuffer globals or from a viewport object. It never touches the global
   that `GetAspectRatio()` reads, which is the one we patched. That test was
   aimed at the wrong source, and now we know why rather than assuming it.

Note that `FUN_7ff67526bf94`'s own alignment arithmetic collapses to identity on
a display wider than 16:9 (it clamps the aspect to 16/9 first, making its factor
1.0 and its offset 0.0). All of the squeezing happens in `bd30`.

### What is already built

`src/uibox.cpp` patches the first byte of `bd30` to `ret`. The function returns
void and has touched nothing at that point, so both output pointers keep the
coordinates the caller passed in. Found by a 24-byte signature that stops before
the call displacement, so it carries no build-specific bytes; one hit in the
image.

**On from startup** in the current build, so the layout is built without the
boxing rather than having it removed underneath a layout that already happened.
`Ctrl+Alt+U` toggles, and the patch site is read back and logged either way.

### What to look for

- do subtitles and credits move outward and grow?
- what happens to the **gameplay HUD**? It goes through the same path, so it
  will move too. That is the thing to judge: if the HUD ends up wrong, the fix
  cannot be this blunt and has to distinguish callers -- which is what the
  return-address logging in Plan B was for.
- the artefacts in the former bar area: same or gone?

A positive result identifies the location. It is not automatically the shipped
fix.

## Plan B: hook the rect drawer and log its callers

Still the fallback if the patch turns out to be too broad and the cutscene path
has to be told apart from the HUD.

`FUN_7ff67562ffcc` (module `+0x58FFCC`) is the rectangle drawer, proven because
it draws the letterbox bars: `FUN_7ff6753b93cc` calls it four times with
`(0 .. bar)` and `(1-bar .. 1)`. Hook it and log the four coordinates plus the
**return address**, deduplicated. Anything whose x stays inside
`[0.128, 0.872]` is laying out in the box, and its caller is the lead.

## Plan A: RenderDoc capture with callstacks

Kept for completeness; less necessary now, but it is the only way to get from a
specific draw back to the code that issued it, which is what would be needed to
gate the patch to cutscenes only.

Tick **Capture callstacks** in the capture dialog, capture a cutscene frame with
subtitles, and open the callstack for that draw. There are no symbols, so the
frames are raw addresses -- subtract the module base (the log prints it as
`RDR2.exe base 0x...`) and they become Ghidra offsets.

## Plan C: the rest of the 16/9 readers

Eighteen functions read the 16/9 constant at `+0x3311378`. Worked through so far:

| Function | What it is |
|---|---|
| `FUN_7ff67526bd30` | **the boxing transform** -- the lead above |
| `FUN_7ff67526bf94` | its only caller; identity on ultrawide, then calls `bd30` |
| `FUN_7ff67526bda8` | the alignment variant, same family, several align modes |
| `FUN_7ff6752753c0` | derives an offset from the two aspect getters |
| `FUN_7ff67527a35c` | width, scaled by `(16/9)/aspect` when the display is wider |
| `FUN_7ff67527f548` | the predicate "is this display wider than 16:9" |
| `FUN_7ff6752741b8` | aspect from the backbuffer or a viewport, the source used here |
| `FUN_7ff6753c00e8` | the letterbox update |
| `FUN_7ff675604f38` | box-to-screen, the inverse |
| `FUN_7ff6756460c8` | thumbnail size calculator (1920x1080 cap, width rounded to 64) |
| `FUN_7ff67564e438` | two edges that narrow as the aspect widens |

Remaining and unexamined: `FUN_7ff675278564`, `FUN_7ff67604476c`,
`FUN_7ff67524196c`, `FUN_7ff675c67fc0`, `FUN_7ff67561a458`, `FUN_7ff675484130`,
`FUN_7ff67567ebcc`, `FUN_7ff6772f6e54`.

## Unrelated leftovers

- Compare vignette and depth of field against a ReShade reference. The focal
  length formula `24/(2*tan(FOV/2))` hangs off our value, so DOF moves with it.
- `fov.txt` is not found inside the game process (`ERROR_FILE_NOT_FOUND`) even
  though the file exists and a directory listing from inside the process hides
  it. The mode is compiled in because of this. Cause unknown, and it is the same
  phenomenon that made marker files unusable.
