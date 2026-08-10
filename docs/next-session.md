# Next session: the 2D layer

Everything else works. This is the one open problem, and seven approaches have
now failed on it. The point of this file is that the eighth should not be the
ninth variation of the same idea.

## What every failed approach had in common

All of them started from a **value** and worked forward, hoping to arrive at the
draw: bar heights, viewport and scissor rects, memory search, constant buffers,
the script native, the aspect getter. Each one only ever proved what the cause
is *not*.

The direction never tried is backwards: **start at the draw and find the code
that issued it.**

## Plan A: RenderDoc capture with callstacks

The one instrument that inverts the direction.

1. In RenderDoc's capture dialog, tick **Capture callstacks**. Optionally also
   "Capture callstacks only for actions" to keep the file smaller.
2. Capture a cutscene frame that has subtitles or the credits visible -- the
   same kind of capture as before.
3. Pick the subtitle or credits draw in the Event Browser and open its callstack.

There are no symbols for RDR2.exe, so the stack is raw addresses. That is
enough: subtract the module base and the result is a Ghidra offset.

```
base = 0x7FF6750A0000   (in the dump; the running process is ASLR'd, take the
                         base from the log line "RDR2.exe base 0x...")
offset = address - base
```

Hand me the frames and I will decompile them. The function that submits the text
draw is at most a couple of frames below whatever computed its coordinates, and
that computation is the thing we have been circling for a week.

**If callstack capture does not work** (it can fail on packed binaries, since it
walks the stack), fall back to Plan B rather than fighting it.

## Plan B: hook the rect drawer and log its callers

Needs no RenderDoc, only a game session.

`FUN_7ff67562ffcc` (module `+0x58FFCC`) is the rectangle drawer -- proven,
because it is what draws the letterbox bars: `FUN_7ff6753b93cc` calls it four
times with `(0 .. bar)` and `(1-bar .. 1)`.

Hook it, and for every call log the four coordinates plus the **return address**,
deduplicated by return address. In a cutscene that yields a map of who draws 2D
and in which coordinate range. Anything whose x stays inside `[0.128, 0.872]` is
laying out in the 16:9 box, and its caller is the lead.

Caveat worth stating up front: text may not go through this path at all. If the
map comes back with nothing but the bars, that is a real answer too, and it
points at the text renderer as a separate subsystem.

## Plan C: find the counterpart of the transform the game already has

`FUN_7ff675604f38` maps a coordinate pair from the 16:9 box onto the full
screen: scale by `aspect * 9/16` (1.34375 here, which is `1/k`) and recentre
around 0.5. It sends 0.127907 to 0.0, 0.5 to 0.5, and 0.872093 to 1.0.

Three callers. Those are the elements that *do* get widened.

Somewhere there must be the opposite -- something multiplying by `k` and
offsetting by `(1-k)/2` to put UI into the box in the first place. Candidates are
among the 18 readers of the 16/9 constant at `+0x3311378`. Four are already
ruled out:

| Function | What it is |
|---|---|
| `FUN_7ff6753c00e8` | the letterbox update itself |
| `FUN_7ff675604f38` | the box-to-screen transform |
| `FUN_7ff6756460c8` | thumbnail size calculator (1920x1080 cap, width rounded to 64) |
| `FUN_7ff67564e438` | two edges that narrow as the aspect widens, i.e. constant physical width |

Fourteen left. Cheap to grind through, no game session needed, but it is still
working forwards, so it ranks below A and B.

## Unrelated leftovers

- Compare vignette and depth of field against a ReShade reference. The focal
  length formula `24/(2*tan(FOV/2))` hangs off our value, so DOF moves with it.
- `fov.txt` is not found inside the game process (`ERROR_FILE_NOT_FOUND`) even
  though the file exists and a directory listing from inside the process hides
  it. The mode is compiled in because of this. Cause unknown, and it is the same
  phenomenon that made marker files unusable.
