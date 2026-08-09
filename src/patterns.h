#pragma once

#include <cstddef>
#include <string_view>

// AOB signatures, deliberately not hardcoded offsets: RDR2 still ships updates
// and the module base moves anyway.
//
// Verified against RDR2.exe 1.0.1491.50. Anything found here should be
// re-checked after a game patch -- the plugin logs every hit count on startup
// for exactly that reason.
namespace patterns {

// mov byte ptr [rip+disp32], 0FFh ; movaps xmm6, [rsp+60h]
//
// Lifted verbatim from the shipped RDR2NoBlackBars.asi, which carries its
// signatures as plaintext strings. Strong candidate for the write to the
// letterbox enable flag; the RIP target is what we want as our trigger.
// The store is 7 bytes: C6 05 <disp32> FF.
inline constexpr std::string_view kLetterboxFlagStore = "C6 05 ? ? ? ? FF 0F 28 74 24 60";
inline constexpr std::size_t kLetterboxFlagStoreDispOffset = 2;
inline constexpr std::size_t kLetterboxFlagStoreLength = 7;

// A function prologue, also taken from RDR2NoBlackBars.asi. Purpose not yet
// confirmed -- included so we can see whether it still resolves on this build.
inline constexpr std::string_view kUnknownPrologue =
    "48 8B C4 48 89 58 08 56 57 41 56 48 81 EC C0 00 00 00 0F 29 70 D8";

}  // namespace patterns
