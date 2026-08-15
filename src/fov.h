#pragma once

#include <cstdint>
#include <vector>

#include "mem.h"

// The actual fix: hooks ApplyCameraState and corrects the field of view of the
// camera that is actually rendered, blended by the letterbox weight.
// See docs/how-it-works.md.
namespace fov {

enum class Mode {
    Off,        // hook installed, value passed through unchanged
    Test,       // unconditional fixed factor, so the effect is unmissable
    Corrected,  // the real thing: k in tangent space, weighted by the letterbox
    Poke,       // no hook: overwrite the master global directly, see run_poke()
    Watch,      // no hook: find who writes the master, see watchpoint.h
    TestWatch   // Test hook AND a watchpoint: who undoes our correction?
};

// Reading fov.txt from inside the game process fails with ERROR_FILE_NOT_FOUND
// for a file that provably exists, while creating the log in the same directory
// works. Until that is understood, the mode a build ships with is compiled in
// here, so a run does not depend on a file we cannot read.
inline constexpr Mode kCompiledDefaultMode = Mode::Corrected;
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

// The resolved address of the FOV master global, or 0 before install().
[[nodiscard]] std::uintptr_t master_address();

// How much of the computed correction to apply, in tangent space:
//   1.0  the full k from the design notes (Hor+ assumption)
//   0.0  no correction at all
// Exists because it is not settled whether RDR2 widens horizontally or crops
// vertically for cutscenes. The two models call for different strengths, and
// the eye decides that faster than algebra. Clamped to [0, 1.5].
void set_strength(float value);
[[nodiscard]] float strength();

// Applies the correction in gameplay too, as if the letterbox were fully in.
//
// Cutscene cameras move, which makes an exact before/after comparison awkward.
// Standing still in gameplay gives a frozen scene where two screenshots differ
// in exactly one thing: the correction. Not for normal use.
void set_force(bool on);
[[nodiscard]] bool forced();

// Zeroes the two bar-height floats every frame.
//
// Tried once before and dismissed too quickly, because the only thing looked at
// was the size of the 2D elements -- which it does not change. A RenderDoc
// capture has since shown a second, separate problem: one fullscreen effect is
// scissored to the letterbox band, so the strips the bars normally cover stay
// unfiltered. That scissor has to be derived from the bar geometry, which is
// exactly what these two floats carry.
//
// Same switch, different question: not "do the elements resize" but "do the
// artefacts disappear".
void set_flatten_bars(bool on);
[[nodiscard]] bool flattening_bars();

// The "one correction per frame during a ramp" rule. On is the behaviour that
// has been shipping; the ramp trace suggests it is what lets the game overwrite
// the correction late in a frame. Toggleable so the two can be compared in one
// cutscene instead of one build per guess.
void set_once_per_frame(bool on);
[[nodiscard]] bool once_per_frame();

// Address of the field of view inside the camera state currently identified as
// the rendered one, or 0 if there is none yet.
//
// For arming a read watchpoint on it: the remaining judder is a one-frame gap
// after a camera cut, caused by identifying the rendered camera *after* the
// fact. If the projection reads this field from one identifiable place, the
// correction belongs there instead and the identification problem disappears.
// If it reads from five, that is worth knowing before rewriting anything.
[[nodiscard]] std::uintptr_t rendered_fov_address();

// The display aspect, or 0 until the first correction has run.
//
// Derived from the game's own bar height rather than from Windows, so it is
// right in windowed mode -- which is the only way a 21:9 machine can test the
// behaviour for wider displays at all.
[[nodiscard]] double display_aspect();

// Logs every camera-state destination the detour has seen, with hit counts and
// whether it is the master. Call after a run to find out which structures exist
// -- only one of them can be the one the projection reads.
void report_destinations(std::uintptr_t module_base);

// How many times the identity test overruled the ring.
//
// Each one is a frame that would have gone out uncorrected: the ring recognised
// an authored value as ours by coincidence, and knowing which camera was asking
// caught it. Reported next to the MISS count, because together they say whether
// the flash is gone or merely moved -- a number here that rises while MISS stays
// put would mean the test is firing on the wrong cases.
[[nodiscard]] unsigned long long identity_saves();

// How many times a correction was nudged clear of an authored value.
//
// Zero means the collision the flash comes from never arose in that session, and
// the separation changed nothing -- which is the expected reading most of the
// time, since the flash itself is about three per half hour. A number here
// against a MISS count of zero is the result worth having.
[[nodiscard]] unsigned long long separations();

// How many corrections were skipped for being too small to see. Zero would mean
// the no-op writes that poison the ring never happen, which the last log says
// they do -- so zero here is a sign the guard is not reached, not that it is
// unnecessary.
[[nodiscard]] unsigned long long tiny_skips();

// How many second corrections of the same structure in one frame were skipped.
// Zero means the double write never happened; a number means it did and was
// caught, which is what the ramp trace predicted.
[[nodiscard]] unsigned long long second_writes();

// How many values were recognised as ours by where they came from rather than by
// what they were. Each one is a float comparison that did not have to be made.
[[nodiscard]] unsigned long long src_skips();

}  // namespace fov
