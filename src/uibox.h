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

// Logs how often the transform ran, from where, and what it did to the values.
//
// This exists because the byte patch could only ever answer "did the picture
// move", and a picture that does not move has now been the least informative
// outcome three times running. Call count answers the prior question -- whether
// the code executes at all -- and the return addresses name the callers, which
// is the data the fallback plan was going to cost a whole session to collect.
void report(std::uintptr_t module_base);

void restore();

}  // namespace uibox
