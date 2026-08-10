# Next session: the 2D layer

Everything else works. This is the one open problem.

## Start here: the boxing transform, found

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
