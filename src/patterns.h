#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// AOB signatures, deliberately not hardcoded offsets: RDR2 still ships updates
// and the module base moves anyway.
//
// Verified against RDR2.exe 1.0.1491.50, measured in the running game -- see
// docs/measurements.md. Anything here should be re-checked after a game patch;
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

// float GetAspectRatio() -> movss xmm0, [rip+disp32] ; ret
//
// The display aspect the letterbox maths divides 16/9 by. Used only by the
// aspect probe, and only as a fallback: the probe first tries to identify the
// getter by the value it reads, which needs no offset at all. That failed once
// because the global is not populated a second after load, hence this.
//
// Verified before use: the bytes must still be that exact getter shape and the
// target must land inside the module.
inline constexpr std::uintptr_t kAspectGetter = 0x173964;

// What kDegreeCopyA reads as during gameplay. Checked at startup: if it does not
// match, the offsets are stale and the observation is meaningless.
inline constexpr float kExpectedGameplayValue = 45.0f;

}  // namespace candidates

// void ApplyUiBox(float* pos, float* size, bool useWindow)
//
// The transform that squeezes the UI into a 16:9 box on a wider display:
//
//     if (aspect > 16/9) {
//         k     = (16/9) / aspect;             // 0.744186 at 3440x1440
//         *pos  = 0.5 - (0.5 - *pos) * k;      // == *pos * k + (1-k)/2
//         *size = *size * k;
//     }
//
// It sends 0 to 0.127907, 0.5 to 0.5 and 1 to 0.872093 -- the exact inverse of
// `FUN_7ff675604f38`, which maps the box back onto the screen. The aspect comes
// from its own computation over the backbuffer size, which is why patching the
// aspect *getter* never reached this.
//
// Called from one place, which is in turn reachable as a script native, so this
// is the path script-drawn 2D takes. That makes it the first real candidate for
// the cutscene 2D layout since the investigation began.
//
// The signature stops before the call displacement, so it contains no
// build-specific bytes. One hit in the image.
inline constexpr std::string_view kUiBoxTransform =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 41 8A F0 41 8A C8";

// undefined8 ApplyUiAlign(int mode, float* pos, float* size, float* rect)
//
// The sibling of kUiBoxTransform, sitting immediately after it and doing the
// same job for several alignment modes at once (left, centre, right, and a
// rectangle variant). Hooked purely to count calls: if neither this nor the
// transform above ever runs during a cutscene, the whole ultrawide-UI family is
// dead code for the 2D layer and the search moves elsewhere.
//
// 28 bytes, no call displacement inside them, one hit in the image.
inline constexpr std::string_view kUiAlignVariant =
    "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 83 EC 30 0F 29 78 E8 49 8B D9 49";

// void FitTo16by9(float* size, float* pos)
//
//     s      = aspect * 9/16          // 1.34375 at 3440x1440, i.e. 1/k
//     size.x = size.x * s
//     pos.x  = pos.x * s + (1 - s) * 0.5
//
// This is the producer of the two constants the intro's full-screen filter
// samples with. RenderDoc found them in its pixel-shader uniforms:
//
//     +0x50   1.343750     = 1/k
//     +0x54   1.000000     (vertical, untouched -- hence no artefacts top or bottom)
//     +0x58  -0.171875     = (1 - 1/k)/2
//
// Feed this function (1.0, 0.0) and it returns exactly those. The shader
// therefore samples a 1920x1080 -- 16:9 -- texture with
// `uv.x * 1.34375 - 0.171875`, which hits the texture's left edge at
// x = 0.127907 and runs negative beyond it. Outside, the sampler repeats the
// edge column, which is the horizontal smearing in the former bar area.
//
// Neutralised, the caller's values survive as 1.0 and 0.0 and the overlay is
// stretched across the whole screen instead.
//
// 24 bytes with the call displacement wildcarded, one hit in the image.
inline constexpr std::string_view kOverlayFit =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ? ? ? ? 66 0F 6E";

// void ClampFocalLength(CameraObject* cam)
//
// Runs after ApplyCameraState and rewrites the field of view in place:
//
//     focal = 24 / (2 * tan(fov * pi/360))     // fov degrees -> focal length
//     focal = clamp(focal, camMin, camMax)     // the camera's lens limits
//     fov   = atan(12 / focal) * 114.59155     // and back, 114.59155 = 2*180/pi
//
// All four constants read out of the image: 24, 12, 114.59155, and the ceilings
// 130 and 9999. It is the very formula this project already documented as the
// game's focal-length relation, found from the other end.
//
// This is what undoes the correction on some frames. Our 39.3141 degrees is a
// focal length of 33.594 mm where the authored 51.2802 is 25.001 mm, so a
// camera whose lens tops out at its authored value clamps straight back -- and
// the field of view returns to 51.28 exactly, which is the alternation seen on
// screen.
//
// Found with a read watchpoint on an address a correction had actually landed
// in: two accesses per frame at +0x46A5AF and +0x46A684, both inside this
// function.
//
// 24 bytes, the one call displacement wildcarded, one hit in the image.
inline constexpr std::string_view kFocalClamp =
    "40 53 48 83 EC 60 48 8B D9 48 8D 4C 24 20 E8 ? ? ? ? 48 8B 03 48 8D";

// Byte offset of the field of view inside the object this one takes. Not the
// same structure as kCameraStateFov -- the decompiler shows param_1[0x2a] on a
// longlong pointer, and the watchpoint confirms [rbx+0x150].
inline constexpr std::ptrdiff_t kFocalClampFov = 0x150;

// void DrawLetterbox()
//
// The only consumer of the two bar heights. It calls a rectangle drawer four
// times -- twice with (0 .. bar) and twice with (1-bar .. 1) -- reading from the
// *second* copy of the letterbox struct, the one the double buffering produces.
//
// Hooked so the bar heights can be replaced immediately before they are used,
// which is the only moment at which nothing else can overwrite them.
//
// 24 bytes with the one RIP displacement wildcarded, one hit in the image.
inline constexpr std::string_view kDrawLetterbox =
    "48 8B C4 48 89 58 10 48 89 70 18 57 48 83 EC 50 80 3D ? ? ? ? 00 0F";

// The bar heights the drawing reads live in the second copy of the struct, one
// stride on from the anchor.
inline constexpr std::ptrdiff_t kDrawnBar235 = letterbox::kStride + letterbox::kBarFraction235;
inline constexpr std::ptrdiff_t kDrawnBarDisplay =
    letterbox::kStride + letterbox::kBarFractionDisplay;

// The second letterbox.
//
// Established by elimination: our zero survives the drawing we hook -- read back
// after the call, still zero -- and that function runs once per frame, so the
// bars that remain at the top and bottom of a 32:9 screen are drawn by somebody
// else. Searching the image for calls to the rectangle drawer found ten, four of
// them ours, and a cluster of three in a function at +0xEE3C2F that takes its
// geometry from an entirely different place.
//
// It reads four consecutive floats:
//
//     movss xmm0, [rip+..]   -> +0x4A5CEA8
//     movss xmm3, [rip+..]   -> +0x4A5CEB4
//     movss xmm2, [rip+..]   -> +0x4A5CEB0
//     movss xmm1, [rip+..]   -> +0x4A5CEAC
//
// which is a rectangle, four floats from the same base. Nothing to do with the
// letterbox struct at +0x39751B4 that the rest of this plugin works with. Very
// likely also what puts bars over the intro video -- the ones that stayed on
// after it and then vanished in a single frame.
//
// The signature is anchored on the test that precedes the loads, because the
// four loads on their own match four times. 36 bytes, displacements wildcarded.
inline constexpr std::string_view kSecondLetterbox =
    "F6 43 39 01 F3 0F 10 05 ? ? ? ? F3 0F 10 1D ? ? ? ? F3 0F 10 15 ? ? ? ? F3 0F 10 0D ? ? ? ?";

// Within the match: the first movss starts at +4 and is 8 bytes long, so its
// target is match + 12 + disp32. The other three floats follow it.
inline constexpr std::size_t kSecondLetterboxDispOffset = 8;
inline constexpr std::size_t kSecondLetterboxInsnEnd = 12;

// A function prologue, also taken from RDR2NoBlackBars.asi. Purpose still
// unconfirmed; resolves to exactly one address on this build.
inline constexpr std::string_view kUnknownPrologue =
    "48 8B C4 48 89 58 08 56 57 41 56 48 81 EC C0 00 00 00 0F 29 70 D8";

}  // namespace patterns
