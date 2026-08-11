#pragma once

#include <cstdint>

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
