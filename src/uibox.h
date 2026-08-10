#pragma once

#include <cstdint>
#include <vector>

#include "mem.h"

// Stops the game squeezing its UI into a 16:9 box.
//
// The 2D layer lays out in a 16:9 window (2560x1440 at this resolution) which
// cutscenes then crop to 2.35:1. Seven attempts to find where that window comes
// from failed; this is the eighth, and the first with the transform itself in
// hand rather than a value suspected of feeding it:
//
//     k     = (16/9) / aspect
//     *pos  = *pos * k + (1-k)/2
//     *size = *size * k
//
// Disabling it is one byte -- a `ret` at the top of the function, which returns
// void and has touched nothing by then, so both output pointers keep the
// coordinates the caller passed in.
//
// Whether that is the *fix* is a different question from whether it is the
// cause: the same path serves the gameplay HUD, so elements anchored near the
// edges may end up somewhere unintended. Treat a positive result as a location,
// not as a finished feature.
namespace uibox {

bool init(const std::vector<mem::NamedRegion>& sections);

bool set_disabled(bool disabled);

[[nodiscard]] bool disabled();

// Whether init() found the site at all. Zero means set_disabled() does nothing,
// which must not be mistaken for "the transform does not matter".
[[nodiscard]] bool found();

// Reads the patch site back and logs whether it still holds what we wrote.
// RDR2.exe is Arxan-protected and a restored patch looks exactly like a
// transform that turned out to be irrelevant.
void verify();

void restore();

}  // namespace uibox
