#pragma once

#include <cstddef>
#include <cstdint>
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

// float GetFov()  ->  movss xmm0, [rip+disp32] ; ret
//
// The camera's vertical field of view in degrees, proven by the binocular test
// (51.282 in gameplay, 8.578 fully zoomed) and confirmed a second time by the
// engine computing 24.0 / (2*tan(fov/2)) from it -- photographic focal length
// with the 24 mm height of a 35 mm frame.
//
// CAVEAT: the getter on its own is nine bytes of the most generic float getter
// imaginable and matches 50+ times. Uniqueness only comes from the sixteen
// bytes in front of it, which are the *tail of a different function*. That is
// more fragile across game patches than a signature contained in one function.
// The plugin therefore verifies the match instead of trusting it: the
// RIP-relative target must land inside .data, and the value there must look
// like a field of view.
inline constexpr std::string_view kFovGetter =
    "C0 74 05 48 8B C3 EB 02 33 C0 48 83 C4 20 5B C3 F3 0F 10 05 ? ? ? ? C3 CC";

// Where the getter itself starts inside the match above.
inline constexpr std::size_t kFovGetterOffset = 16;
// The movss is 8 bytes with its disp32 at +4.
inline constexpr std::size_t kFovMovssDispOffset = 4;
inline constexpr std::size_t kFovMovssLength = 8;

// A field of view outside this range means we hooked the wrong thing.
inline constexpr float kFovSanityMin = 1.0f;
inline constexpr float kFovSanityMax = 170.0f;

// void ApplyCameraState(CameraState* dst, const CameraState* src)
//
// Found with a hardware watchpoint on the FOV master: exactly one instruction
// writes it, `movss [rbx+0x60], xmm0`, and it lives here. Ghidra could not find
// it because the store goes through a register, not a RIP-relative address.
//
// The function copies a camera state field by field, clamping as it goes. FOV
// is at +0x60 of both structures. Hooking it and correcting after the original
// has run puts our value in place exactly when the game commits the camera --
// early enough for the projection, which reads it immediately afterwards.
//
// Unlike kFovGetter this signature is entirely inside the function it names,
// so it does not borrow uniqueness from a neighbour.
inline constexpr std::string_view kCameraApply =
    "48 89 5C 24 08 57 48 83 EC 20 F3 0F 6F 42 30 41";

// Offset of the field of view within the camera state.
inline constexpr std::ptrdiff_t kCameraStateFov = 0x60;

// Candidates from the differential search, see docs/ghidra.md.
//
// WARNING: these are raw module offsets for RDR2.exe 1.0.1491.50, not AOB
// patterns. They will be wrong on any other build, silently. They exist only so
// the observation pass can watch them; nothing in the actual fix may depend on
// them. Once the FOV is confirmed, the value gets reached through a signature
// like everything else.
//
// None of them is confirmed to be a FOV. What is known: the two degree
// candidates hold 45.000 during gameplay, change every frame during cutscenes,
// and are copies rather than sources.
namespace candidates {

inline constexpr std::uintptr_t kDegreeCopyA = 0x39B06E4;   // written from the getter below
inline constexpr std::uintptr_t kDegreeCopyB = 0x3AE24B8;   // broadcast as a shader constant
inline constexpr std::uintptr_t kGetterSource = 0x3EA0BE0;  // what the getter returns
inline constexpr std::uintptr_t kScaleX = 0x3A11250;        // 1.0 in gameplay
inline constexpr std::uintptr_t kScaleY = 0x3A11254;        // 1.0 in gameplay

// What kDegreeCopyA reads as during gameplay. Checked at startup: if it does not
// match, the offsets are stale and the observation is meaningless.
inline constexpr float kExpectedGameplayValue = 45.0f;

}  // namespace candidates

// A function prologue, also taken from RDR2NoBlackBars.asi. Purpose still
// unconfirmed; resolves to exactly one address on this build.
inline constexpr std::string_view kUnknownPrologue =
    "48 8B C4 48 89 58 08 56 57 41 56 48 81 EC C0 00 00 00 0F 29 70 D8";

}  // namespace patterns
