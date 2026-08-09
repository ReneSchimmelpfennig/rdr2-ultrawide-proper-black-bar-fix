#pragma once

#include <cstddef>
#include <string_view>

// AOB signatures, deliberately not hardcoded offsets: RDR2 still ships updates
// and the module base moves anyway.
//
// Verified against RDR2.exe 1.0.1491.50, measured in the running game -- see
// docs/messungen.md. Anything here should be re-checked after a game patch;
// the plugin logs every hit count on startup for exactly that reason.
namespace patterns {

// mov byte ptr [rip+disp32], 0FFh ; movaps xmm6, [rsp+60h]
//
// Lifted verbatim from the shipped RDR2NoBlackBars.asi. The store is 7 bytes:
// C6 05 <disp32> FF.
//
// The name in that plugin is misleading and we kept it too long: the byte this
// writes is 0FFh during gameplay *and* during cutscenes -- it never toggles, so
// it is useless as a trigger. What makes it valuable is its address. It sits at
// +0x08 of a 32-byte struct that holds the whole letterbox state, and the
// fields around it are exactly what the fix needs.
inline constexpr std::string_view kLetterboxStructAnchor = "C6 05 ? ? ? ? FF 0F 28 74 24 60";
inline constexpr std::size_t kAnchorDispOffset = 2;
inline constexpr std::size_t kAnchorLength = 7;

// Layout of the struct, relative to the anchor byte the store above targets.
// Field names are ours; offsets and meanings are measured, not guessed.
namespace letterbox {

// float, 0.0 during gameplay, 1.0 while fully letterboxed. Eases in over about
// 1.29 s and out over about 1.00 s. This is the blend weight the design wants:
// it carries both the trigger (non-zero = cutscene) and the interpolation.
inline constexpr std::ptrdiff_t kWeight = -8;

// Duplicate of kWeight, byte-identical in all 293 observed frames. Unknown why
// there are two; read kWeight.
inline constexpr std::ptrdiff_t kWeightDuplicate = -4;

// The anchor byte itself. Constant 0xFF, kept only for documentation.
inline constexpr std::ptrdiff_t kConstantFF = 0;

// float = kWeight * 0.121749. That constant is (1 - (16/9)/2.35)/2, i.e. the
// bar height for a 2.35:1 frame in 16:9 space -- the artistic target.
inline constexpr std::ptrdiff_t kBarFraction235 = 4;

// float = kWeight * (1 - k)/2, where k is our own correction factor computed
// from the real backbuffer aspect. In other words the game already derives k
// per frame; see framing::correction_factor_from_bars().
inline constexpr std::ptrdiff_t kBarFractionDisplay = 8;

// The struct repeats every 32 bytes. The second copy lags one frame behind the
// first, so it is double buffered. Read the first.
inline constexpr std::size_t kStride = 32;

}  // namespace letterbox

// A function prologue, also taken from RDR2NoBlackBars.asi. Purpose still
// unconfirmed; resolves to exactly one address on this build.
inline constexpr std::string_view kUnknownPrologue =
    "48 8B C4 48 89 58 08 56 57 41 56 48 81 EC C0 00 00 00 0F 29 70 D8";

}  // namespace patterns
