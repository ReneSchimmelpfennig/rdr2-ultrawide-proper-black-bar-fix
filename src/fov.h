#pragma once

#include <cstdint>
#include <vector>

#include "mem.h"

// The actual fix: hooks the camera's field-of-view getter and returns a value
// corrected for the display aspect, blended by the letterbox weight.
namespace fov {

enum class Mode {
    Off,        // hook installed, value passed through unchanged
    Test,       // unconditional fixed factor, so the effect is unmissable
    Corrected,  // the real thing: k in tangent space, weighted by the letterbox
    Poke        // no hook: overwrite the master global directly, see run_poke()
};

// Reading fov.txt from inside the game process fails with ERROR_FILE_NOT_FOUND
// for a file that provably exists, while creating the log in the same directory
// works. Until that is understood, the mode a build ships with is compiled in
// here, so a run does not depend on a file we cannot read.
inline constexpr Mode kCompiledDefaultMode = Mode::Poke;
inline constexpr float kCompiledPokeValue = 25.0f;

struct Config {
    Mode mode = kCompiledDefaultMode;
    float test_factor = 0.5f;
    float poke_value = kCompiledPokeValue;
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

// Hammers the master FOV global with the configured value, racing the game's
// own per-frame write.
//
// This answers one question the getter hook could not: does the value the game
// stores actually reach the projection? The hook proved that the *getter* does
// not feed it. If overwriting the global does not move the picture either, the
// projection is fed further upstream and a breakpoint on this address would be
// wasted effort.
//
// Deliberately crude. Racing a per-frame writer means we win some frames and
// lose others, which shows up as flicker -- and flicker is a perfectly good
// answer to "does this address matter at all".
void run_poke(unsigned int duration_ms);

}  // namespace fov
