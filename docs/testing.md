# Where to look when testing

Everything here comes from what *other* bar-removal mods are reported to break.
None of it has been reproduced with this plugin — the point is to know which
scenes are load-bearing, so that testing goes where the risk is instead of
wherever the save happens to be.

Community reports, not verified findings. Sources at the bottom.

## Softlocks in scripted scenes

The `NoBlackBars` mod ships a hotkey whose only purpose is to put the bars *back*
when the game locks up, and its page names the scenes where players needed it:

| Scene | Chapter | Reported trigger |
|---|---|---|
| Exit Pursued by a Bruised Ego | 2 | selling your horse |
| Robbing a homestead with Javier | 2 | — |
| Horse Flesh for Dinner | 3 | leading the white horse |

What these have in common is a scripted sequence that hands control back and
forth rather than a plain cutscene. A mod that removes the bars by force can
leave such a sequence waiting for a state that never arrives.

**Why this plugin might behave differently:** it never removes the letterbox
*weight*, which is the game's own progress value for the transition, and it uses
that weight as its trigger. The bars go; the timing information the scripts read
stays. That is a reason to expect fewer problems, not a guarantee, and these
three scenes are the cheapest way to find out.

## Videos played over gameplay

The high-honour deer and low-honour coyote scenes are videos laid over the live
game world. With the bars gone, players report seeing the world behind them
instead of black.

This is the same shape as the problem this plugin already hit in the scene after
the intro video, and it is the strongest reason to test with
`RemoveAllBlackBars = true` specifically — that setting reaches exactly the kind
of bars these scenes rely on.

## Screen effects behind the bars

Pause menu, shops and photo mode apply effects that are scissored to a 16:9
window. The bars normally cover the difference; without them an unprocessed strip
can appear at the edges. The author of `No Letter Box Black Bars` says plainly
that his mod does not fix this.

Applies here only with `RemoveAllBlackBars = true`.

## What cannot affect this plugin

- **`cameras.ymt` conflicts.** Several mods change a value in that file, which
  collides with anything else editing it and produces a black screen at unusual
  resolutions. This plugin does not touch the file.
- **An external tool per launch.** Some ultrawide fixes need one running
  alongside. This plugin is a plain ASI.

## Sources

- [NoBlackBars (349)](https://www.nexusmods.com/reddeadredemption2/mods/349) —
  the hotkey to un-softlock, and the scenes where it was needed
- [No Letter Box Black Bars (5340)](https://www.nexusmods.com/reddeadredemption2/mods/5340)
  — no un-softlock key needed, and the note about screen effects
- [Remove Black Bars in Cutscenes (1389)](https://www.nexusmods.com/reddeadredemption2/mods/1389)
  — the `cameras.ymt` line edit
- [Help with Remove Black Bars mod?](https://www.rdr2mods.com/forums/topic/2015-help-with-remove-black-bars-mod/)
  — "problems with certain quests", cutscenes failing to load
