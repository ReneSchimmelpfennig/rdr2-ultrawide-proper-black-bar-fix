# RDR2 Ultrawide Cutscene Fix

Removes the cutscene letterbox in Red Dead Redemption 2 **and keeps the original
framing**. Existing fixes only remove the bars, which leaves you seeing more of
the scene than the shot was composed for.

## The problem

On a 16:9 display, RDR2 renders cutscenes into a 16:9 window and crops it to
2.35:1 with black bars. On a 3440x1440 display the game keeps that 16:9 window
and pillarboxes it, so the visible area is **2560x1090** inside a 3440x1440
screen — bars on all four sides.

Removing those bars reveals the rest of the frame, which the game renders but
never intended you to see. The horizontal field of view jumps from 72.7° to
89.4°: more image, wrong composition.

This plugin narrows the vertical field of view by exactly the amount that makes
the full ultrawide screen show what the 2560-wide window used to show:

```
k        = (16/9) / (width/height)          // 0.744186 at 3440x1440
vFOV_new = 2 * atan(k * tan(vFOV_old / 2))  // note: k scales the tangent
```

At 3440x1440, `k` works out to exactly `2560/3440` — the pillarbox ratio the
game itself computes. The correction is blended in and out using the game's own
letterbox animation, so the transition matches the bars rather than snapping.

## Status

| | |
|---|---|
| Field of view in cutscenes | works, verified frame by frame against a walkthrough recording |
| Fade in / fade out | smooth, follows the game's own easing |
| Black bars | removed |
| **2D elements without bars** | **not fixed yet — see below** |

### Known issue: the 2D layer

With the bars removed, 2D elements during cutscenes are still laid out for the
old 2560x1090 window, so they appear too small, and the area the bars used to
cover can show artefacts.

Press **F8** to bring the bars back. The field-of-view correction stays active,
so you get correct framing with letterboxing — not the goal of this project, but
a usable state until the 2D layer is sorted out.

Zeroing the game's computed bar heights was tried and has no effect; the 2D
layer takes its safe area from somewhere else. Tracking that down is a separate
piece of reverse engineering of roughly the same size as the field-of-view work.
Contributions welcome.

## Requirements

- Red Dead Redemption 2, tested on **1.0.1491.50**
- An ASI loader, e.g. [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
- An ultrawide display. On 16:9 the plugin computes `k = 1` and does nothing.

## Installation

Copy `RDR2UltrawideCutsceneFix.asi` into the game folder, next to `RDR2.exe`.
That folder is usually writable only by administrators, so the copy needs an
elevated shell.

## Hotkeys

| Key | Effect |
|---|---|
| `F7` | correction on / off |
| `F8` | black bars on / off |
| `F9` / `F10` | correction strength -0.05 / +0.05 |
| `F11` | apply the correction during gameplay too (for still comparisons) |
| `F12` | reset strength to 1.00 |

Strength `1.00` is the geometrically correct value. It is adjustable because the
visible window (2.349:1) and an ultrawide screen (2.389:1) do not match exactly:
matching the width crops 1.7% vertically, matching the height shows 1.7% extra
horizontally. `0.95` matches the height instead of the width.

## Log

The plugin writes to `%LOCALAPPDATA%\RDR2UltrawideCutsceneFix\plugin.log`, not
to the game folder — the game process cannot create files there.

## Building

MSVC, x64. MinHook is fetched automatically.

```powershell
cmake --preset vs2026
cmake --build --preset release
.\build\Release\scanner_test.exe
```

`scanner_test.exe` covers the pattern scanner, the framing maths, the image
dumper and the log fallback, without a test framework.

## How it works

The short version: the plugin hooks the function that commits a camera state,
corrects the field of view of the camera that is actually being rendered, once
per frame, using a factor derived from the game's own letterbox geometry.

Every part of that sentence is load-bearing and each was arrived at by
measurement:

- [`docs/how-it-works.md`](docs/how-it-works.md) — the runtime chain, the three
  rules in the detour, and what did not work
- [`docs/ghidra.md`](docs/ghidra.md) — what is in the unpacked image, and how the
  write site was found
- [`docs/measurements.md`](docs/measurements.md) — the numbers taken from the
  running game
- [`docs/packing.md`](docs/packing.md) — RDR2.exe is Arxan-packed, so nothing can
  be found in the file on disk; the plugin dumps the decrypted image out of the
  running process for analysis

The "what did not work" list is the part worth reading before touching this:
hooking the FOV getter, overwriting the master global, and searching for the
write site statically all look reasonable and all fail.

## Credits

The two AOB signatures used to locate the letterbox state were lifted from
`RDR2NoBlackBars.asi`, which carries them as plaintext strings in its binary.

## License

MIT, see [LICENSE](LICENSE).
