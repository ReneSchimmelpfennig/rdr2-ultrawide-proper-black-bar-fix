#pragma once

#include <cstdint>
#include <vector>

#include "mem.h"

// Makes the game report a bar height of zero to whoever asks for it.
//
// The bar fractions in the letterbox struct have exactly one reader each: a
// nine-byte getter `movss xmm0, [rip+bar]; ret`. That getter is called by a
// tiny wrapper that stores the result through a pointer argument, and the
// wrapper itself is only ever referenced as *data* -- it sits in a table of
// several hundred such wrappers. That is what a script native looks like.
//
// So the bar geometry is not consumed by engine code at all; it is handed to
// the script layer. RDR2 lays its cutscene 2D out in script, which is exactly
// the layer that is still sized for the 2560x1090 window. If the scripts ask
// "how tall are the bars" to derive their safe area, answering zero should make
// them lay out across the full screen.
//
// Zeroing the floats in memory was tried and did nothing -- but that write
// happened in the camera hook and the letterbox update recomputes the fields
// afterwards, so it may simply have been undone before anyone read it. Patching
// the getter cannot be overwritten.
namespace safearea {

// Finds the bar-height getters and prepares the patch. `anchor` is the address
// of the constant 0xFF byte, i.e. the same anchor the rest of the fix uses.
//
// Deliberately not matched by a byte signature: `movss xmm0,[rip]; ret` is the
// most generic getter shape there is. Every one of them is scanned and only
// those whose RIP target lands on a bar-height field are kept, which makes the
// match self-verifying and survives a game patch moving the code.
bool init(const std::vector<mem::NamedRegion>& sections, std::uintptr_t anchor);

// Patches the getters to `xorps xmm0, xmm0; ret` (or restores them).
bool set_flat(bool flat);

[[nodiscard]] bool flat();

// How many getters were found. Zero means init() did not identify anything and
// set_flat() will do nothing -- worth logging, so a null result can be told
// apart from a null attempt.
[[nodiscard]] int count();

void restore();

}  // namespace safearea
