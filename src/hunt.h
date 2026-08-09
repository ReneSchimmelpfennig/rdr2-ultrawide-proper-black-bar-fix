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

}  // namespace hunt
