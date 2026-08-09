# Measurements taken in the running game

All addresses are **module offsets**, not absolute -- RDR2.exe is ASLR-relocated,
so the base differs on every launch.

## Run 1 -- 2026-08-09, RDR2.exe 1.0.1491.50

First run of the skeleton. Full log:
`%LOCALAPPDATA%\RDR2UltrawideCutsceneFix\plugin.log`

| Item | Value |
|---|---|
| Module base (this run) | `0x00007FF6750A0000` |
| Image size in memory | 121,206,272 bytes (115.6 MB) |
| Executable sections | 2 |
| `.text` #1 | `+0x1000`, 50.8 MB |
| `.text` #2 | `+0x6033000`, 19.4 MB |
| Letterbox store found at | **`+0x320545`** (in `.text` #1) |
| Address of the letterbox flag | **`+0x39751B4`** (in `.data`) |
| Unknown prologue found at | **`+0x57A458`** (in `.text` #1) |
| Scan time | 9.4 ms and 68.9 ms across 70 MB |

### What follows from it

**Both signatures are unambiguous on this build** -- exactly one hit each. They
are usable as anchors without any further tightening.

**The code is already unpacked when the plugin loads.** Hits on scan attempt 1,
roughly 80 ms after `DLL_PROCESS_ATTACH`. Arxan therefore decrypts before the
entry point, not lazily. The retry loop in `scan_until_found()` stays anyway: it
costs nothing when the first attempt succeeds and covers the case after a game
patch.

**The flag lives in `.data`,** `0x5E1B4` past the start of the section (`.data`
begins at RVA `0x3917000`). A global byte -- consistent with an enable flag.

## Run 2 -- 2026-08-09, two cutscenes in and out

293 logged frames. Result: **the byte our signature points at is a constant
`0xFF`** -- in gameplay and in cutscenes alike, across all 293 frames. As a
trigger it is worthless. What is valuable is its *address*: it sits at `+0x08` of
a 32-byte structure holding the entire letterbox state.

### The structure, relative to the anchor byte

| Offset | Type | Content |
|---|---|---|
| `-0x08` | float | **weight**, 0.0 in gameplay → 1.0 fully letterboxed |
| `-0x04` | float | duplicate of the weight, byte-identical in all 293 frames |
| `+0x00` | byte | constant `0xFF` -- the anchor byte |
| `+0x04` | float | weight × **0.121749** |
| `+0x08` | float | weight × **0.127907** |
| `+0x20` | — | the whole structure repeats, one frame behind (double buffered) |

### The two constants

The ratios `+0x04 / weight` and `+0x08 / weight` are **exactly constant** across
all frames (minimum = maximum to six decimal places):

```
0.121749 = (1 - (16/9) / 2.35   ) / 2      bar height for 2.35:1
0.127907 = (1 - (16/9) / 2.3889 ) / 2      bar height for 3440x1440
```

Inverted, the first constant yields a target aspect of 2.3500 and the second
2.3889 -- the latter being our display aspect to four digits. Put differently:

```
0.127907 = (1 - k) / 2     with k = 0.744186
```

**The game already computes our `k` itself, every frame, from the real
resolution.** `framing::correction_factor_from_bars()` inverts that and recovers
`k` from it. This beats the resolution from `GetSystemMetrics`, because it is
correct in windowed mode, at non-native resolutions and with render scaling
without any extra work.

Later confirmed by a user measurement of the visible area, 2560x1090 inside
3440x1440: the two constants are not competing letterbox targets but the
**horizontal and vertical bars**.

```
3440 - 2 · (3440 · 0.127907) = 2560     side bars
1440 - 2 · (1440 · 0.121749) = 1089     top and bottom bars
```

The game places the cutscene in a 16:9 window (2560 = 1440 · 16/9) and crops that
to 2.35:1.

### Timing

Both cutscenes behave identically:

| Phase | Duration | Frames |
|---|---|---|
| Fade in | ~1.29 s | 71 and 70 |
| Hold at 1.0 | variable | — |
| Fade out | ~1.00 s | 55 and 55 |

The ramp is **not** linear: the per-frame delta falls from 0.0222 to 0.0217 -- a
slight ease-out. That is precisely why the value is read rather than
reconstructed; a hand-built curve would fight the bar animation.

## Settled: trigger and interpolation come from the same source

The original worry was that `mov byte ptr [rip+disp32], 0FFh` writes only a
boolean and therefore cannot carry the smooth blending of `k` that the design
assumes. It resolved differently than expected: the byte really is useless
(always `0xFF`), but eight bytes in front of it sits exactly the 0..1 float the
design needs.

**Trigger and interpolation weight are the same value** -- `weight > 0` means
cutscene, and the magnitude is the blend weight. The design note was right; only
the assumed source was not.
