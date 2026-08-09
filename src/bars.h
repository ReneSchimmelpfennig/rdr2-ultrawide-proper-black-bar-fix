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

// Restores the original byte. Called on unload.
void restore();

}  // namespace bars
