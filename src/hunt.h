#pragma once

#include <cstdint>
#include <vector>

#include "mem.h"

// Differential value search -- what Cheat Engine is normally used for, but
// automated and driven by the letterbox weight as an exact clock.
//
// The letterbox and cinematic code turned out to deal only in aspect ratios and
// bar heights (see docs/ghidra.md), so following references from there never
// reaches the camera. This finds the FOV from the other end: by what it *does*
// rather than by what references it.
namespace hunt {

struct Candidate {
    std::uintptr_t address = 0;
    float gameplay = 0.0f;   // value while weight == 0
    float cutscene = 0.0f;   // value while weight == 1
    float later = 0.0f;      // value one frame after that
};

// Every 4-byte-aligned float in `region` whose value lies in [lo, hi].
// Unreadable pages are skipped rather than faulted on.
std::vector<Candidate> collect(const mem::Region& region, float lo, float hi);

// Drops candidates whose value did not move by at least `min_change`, or whose
// new value left [lo, hi]. Fills in `cutscene` on the survivors.
void keep_changed(std::vector<Candidate>& candidates, float lo, float hi, float min_change);

// Fills in `later` from memory. Call a frame after keep_changed.
void resample(std::vector<Candidate>& candidates);

// Runs the whole sequence against the game, logging as it goes. Blocks for as
// long as it takes the player to enter and leave a cutscene, up to `timeout_ms`.
void run(const mem::Region& search_area, std::uintptr_t weight_addr,
         std::uintptr_t module_base, unsigned int timeout_ms);

// Watches the candidates from patterns::candidates side by side with the
// letterbox weight, logging whenever any of them moves. Purely reading.
//
// This is the cheap decisive test: move the FOV slider in the graphics menu. If
// the 45.000 follows it, the value is the field of view and no writing was
// needed to find out.
void watch(std::uintptr_t module_base, std::uintptr_t weight_addr, unsigned int duration_ms);

// Same differential idea as run(), but triggered by the player instead of by a
// cutscene: baseline now, then a window in which an *external* tool changes the
// FOV, then compare.
//
// The point is to watch a mod that demonstrably works. Whatever it writes
// reaches the projection by definition -- which is exactly the knowledge our
// own getter hook and the poke test failed to produce.
void run_hotkey(const mem::Region& search_area, std::uintptr_t module_base,
                unsigned int window_ms);

// Searches for the exact numbers the 2D layer must be built from.
//
// The FOV hunt had to sweep a whole plausible range because the value was
// unknown. Here it is not: the visible cutscene area was measured at 2560x1090
// inside 3440x1440, so a viewport, a scissor rect or a scale factor holding the
// 2D layer must contain 2560, 1090, 440 or 175 -- or the ratios derived from
// them. Looking for known constants is far more selective than a range sweep,
// and a hit is close to proof.
//
// Scans as 32-bit integer and as float, since a viewport may be either.
void find_known_values(const mem::Region& search_area, std::uintptr_t module_base,
                       std::uintptr_t weight_addr, unsigned int timeout_ms);

}  // namespace hunt
