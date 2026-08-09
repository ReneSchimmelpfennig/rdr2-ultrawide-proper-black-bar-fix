#pragma once

#include <cstdint>
#include <vector>

#include "mem.h"

// The actual fix: hooks the camera's field-of-view getter and returns a value
// corrected for the display aspect, blended by the letterbox weight.
namespace fov {

enum class Mode {
    Off,       // hook installed, value passed through unchanged
    Test,      // unconditional fixed factor, so the effect is unmissable
    Corrected  // the real thing: k in tangent space, weighted by the letterbox
};

struct Config {
    Mode mode = Mode::Test;
    float test_factor = 0.5f;  // only used in Test mode
};

// Reads "fov.txt" next to the log. Missing file means Test mode -- the first
// run is meant to answer "does hooking this change the picture at all".
//
//   test          unconditional factor 0.5
//   test 0.75     unconditional factor 0.75
//   real          the actual correction, cutscenes only
//   off           hook installed but inert
Config read_config();

// Finds the getter, verifies it really is one, and installs the detour.
// `anchor` is the letterbox struct anchor; the weight and bar fraction are read
// relative to it. Returns false and logs why on any failure.
bool install(const std::vector<mem::NamedRegion>& sections, const mem::Region& module,
             std::uintptr_t anchor, const Config& config);

void uninstall();

}  // namespace fov
