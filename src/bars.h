#pragma once

#include <cstdint>
#include <vector>

#include "mem.h"

// Removes the cutscene letterbox by patching the instruction that enables it.
//
// The anchor we have been using all along is `mov byte ptr [rip+disp32], 0FFh`
// -- the store whose *address* gave us the letterbox struct. Its immediate is
// what switches the bars on. Patching that one byte to zero turns them off.
//
// Deliberately not touching the weight: it is computed earlier in the same
// function and is our cutscene trigger and blend factor. The bars go, the
// timing information stays.
namespace bars {

// `anchor_store` is the address of the C6 05 ... FF instruction itself, not of
// the byte it writes.
bool init(std::uintptr_t anchor_store);

// Side bars for displays wider than the film frame.
//
// On anything wider than 2.35:1 the corrected picture keeps its full height and
// no longer fills the width -- see framing::clamped_for_wide_display. This puts
// the difference back under black, so the composition is framed rather than
// extended.
//
// It works by hooking the drawing itself and replacing the two bar heights just
// before they are read: the vertical ones go to zero, the horizontal ones become
// (1 - 2.35/aspect)/2, scaled by the letterbox weight so the bars still animate
// with the game's own easing. Everything else about the bars stays the game's.
//
// `anchor` is the address of the constant 0xFF byte. Returns false if the
// drawing function could not be found, in which case the caller should keep
// hiding the bars as before.
bool init_side_bars(const std::vector<mem::NamedRegion>& sections, std::uintptr_t anchor);

// Turns the side bars on. Until this is called the hook only passes through.
void set_side_bars(bool on);

[[nodiscard]] bool side_bars();

// Samples the second letterbox's rectangle and logs it whenever it is not empty.
//
// Called from the worker loop rather than from our own drawing hook: the first
// sampling attempt sat inside that hook and never saw anything, because the two
// letterboxes are not on at the same time. The second one is most likely busy
// exactly when ours is idle -- over the intro video, for one.
void poll_second_letterbox();

// Turns the bars off (patched) or back on (original). Safe to call repeatedly.
bool set_hidden(bool hidden);

[[nodiscard]] bool hidden();

// Reads the patched byte back and logs it if it is no longer what we wrote.
//
// After the pre-rendered intro, the side bars stay visible for a few seconds in
// the in-game cutscene before fading. Only one instruction in the whole game
// writes that alpha byte -- the one we patch -- so either something restores our
// patch for a while, or those bars come from a different drawing path
// altogether. Watching the byte tells the two apart without guessing.
void verify();

// Restores the original byte. Called on unload.
void restore();

}  // namespace bars
