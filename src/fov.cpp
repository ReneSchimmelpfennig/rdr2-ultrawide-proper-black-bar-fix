#include "fov.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>

#include "framing.h"
#include "log.h"
#include "patterns.h"

namespace fov {
namespace {

// void ApplyCameraState(CameraState* dst, const CameraState* src)
using ApplyFn = void (*)(std::uintptr_t, std::uintptr_t);

ApplyFn g_original = nullptr;
void* g_target = nullptr;

std::uintptr_t g_weight_addr = 0;
std::uintptr_t g_bar_addr = 0;
std::uintptr_t g_master_addr = 0;
std::uintptr_t g_module_base = 0;
std::uintptr_t g_shader_fov_addr = 0;  // patterns::candidates::kDegreeCopyB
Config g_config;

// The detour runs several times per frame from ten call sites, so it must stay
// cheap and must not allocate or take locks. Logging is limited to the first
// few calls, purely as proof that it is being reached at all.
std::atomic<int> g_calls{0};
constexpr int kLoggedCalls = 8;

// DIAGNOSTIC BUILD -- set back to 1.0f before shipping anything.
//
// The transition judder needs one question answered that nothing measured so far
// can answer: is it the field-of-view correction at all? The values we write are
// smooth, complete, and demonstrably reach the screen -- shader[n] equals
// out[n-1] to four decimals -- so whatever is visible is not in those numbers.
//
// F7 was supposed to settle it in game, and left no trace in the log at all, so
// that attempt proved nothing. Compiling the answer in removes the dependency on
// a keypress: bars still removed, overlays still fixed, correction dormant.
//
//   still juddering -> not the correction. It comes from removing the bars or
//                      from the 2D layer, and a week has been spent in the wrong
//                      place.
//   gone            -> it is the correction, and the suspect is the shape of the
//                      ramp rather than its values: `in` sits at a constant
//                      51.2820 for the whole transition, so every degree of the
//                      visible movement is ours -- 14 of them in 1.3 seconds,
//                      which the unmodded game never does. That would also
//                      explain why a longer transition looks worse.
constexpr float kStartingStrength = 1.0f;
std::atomic<float> g_strength{kStartingStrength};

// Correct only while the letterbox is fully in, not while it slides.
//
// Ramping the correction with the bar weight was chosen early on so the
// correction would arrive smoothly rather than snap. It was the wrong thing to
// attach it to, and the measurement finally says why: during the whole
// transition the game's own camera does not move. 322 of 436 corrections during
// ramps arrive with in = 51.2820 exactly, the rest within 0.02 of it. The game
// slides the bars over an unchanged frame and *then* cuts to the cutscene
// camera.
//
// So the ramp is a mask, not a reframing -- and by following it we invent a
// twelve-degree zoom that the unmodded game never performs, which then snaps
// when the real cutscene camera arrives. In and back out, and the longer the
// transition the further the excursion. That matches every report.
//
// Correcting only at a settled weight puts our change at the same moment as the
// game's own cut, where it is hidden. The cost is that the correction can no
// longer ease in -- but there was nothing to ease into.
constexpr bool kCorrectOnlySettled = true;
constexpr float kSettledWeight = 0.999f;

// Has the letterbox been fully in during this cutscene? Reset when the weight
// returns to zero, i.e. when gameplay resumes.
std::atomic<bool> g_reached_settled{false};

// TRIED AND REVERTED. Correcting every camera state in an established cutscene
// compounds, exactly as the original design did, and the ring did not prevent
// it.
//
// Measured on the shader constant, so on the picture rather than on our own
// bookkeeping:
//
//   w 0.9999  ->  29.7821     correct: 39.3141 corrected once
//   w 1.0000  ->  22.3802     2*atan(k*tan(29.78/2)) -- corrected twice
//
// 12.2887 appeared as well, a value that never occurs when only the rendered
// camera is corrected. rschi saw it before the log did: "the picture is a bit
// too close".
//
// Why the ring did not catch it: a corrected value reaching another state as
// input does not arrive verbatim. It goes through the camera code, which is
// arithmetic, and arithmetic destroys both the low mantissa bits carrying the
// tag and any hope of exact equality. The ring can only catch verbatim copies --
// which is precisely what the note above the tag says, and I built on it anyway.
//
// The one-frame gap after a cut therefore cannot be closed this way. Closing it
// needs a signal that a cut has happened *before* the shader constant confirms
// it, not a wider net.
constexpr bool kCorrectAllWhenSettled = false;
std::atomic<bool> g_force{false};
std::atomic<bool> g_flatten{false};
std::atomic<int> g_flatten_logged{0};

// Per-call decision logging. Invaluable while the correction was being worked
// out -- it is what finally showed the double corrections during a transition --
// but it writes thousands of lines per cutscene, so it is off unless someone is
// debugging.
constexpr bool kLogEveryDecision = false;

std::atomic<DWORD> g_last_sample{0};
std::atomic<int> g_samples{0};
constexpr int kMaxSamples = 3000;

float read_float(std::uintptr_t addr) {
    float value = 0.0f;
    std::memcpy(&value, reinterpret_cast<const void*>(addr), sizeof(value));
    return value;
}

// Which camera-state structures does the game apply to? Recorded from inside
// the detour, so no allocation and no logger mutex: a small table plus counts,
// reported afterwards.
constexpr std::size_t kMaxDestinations = 64;
constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

std::atomic<std::size_t> g_dst_known{0};
std::uintptr_t g_dst[kMaxDestinations]{};
std::atomic<unsigned long long> g_dst_count[kMaxDestinations]{};

// Identifying the camera that is actually rendered.
//
// The shader constant carries the field of view of whatever camera made it into
// the picture, one frame late. So remembering the value each destination ended
// up with, and comparing it against the shader constant on its next call, says
// plainly which destination feeds the image: measured at 99% for two of them
// and 0% for the other twenty-odd.
//
// This matters because during a transition the rendered camera is the *blend*
// of two others. Correcting its sources and then correcting the blend again is
// what made the picture jitter. Correcting only the rendered camera leaves the
// blend to work on authored values and applies the correction exactly once.
std::atomic<float> g_dst_last_final[kMaxDestinations]{};
std::atomic<unsigned> g_dst_shader_hits[kMaxDestinations]{};
std::atomic<unsigned> g_dst_shader_samples[kMaxDestinations]{};

// Enough samples to be sure, and a clear majority. The two real ones score ~99%,
// everything else scores zero, so the threshold is not delicate.
constexpr unsigned kMinSamples = 20;

// How closely a destination's last value must track the shader constant to count
// as the rendered camera.
//
// This was 5e-3 relative, which at 45 degrees is a window of +-0.22 degrees, and
// that window is the whole transition judder. The full ramp trace makes it plain
// -- these are consecutive frames of one fade-out:
//
//   CORRECT slot 25  in 51.2820  shader 44.6403  prev 44.6403   <- exact
//   CORRECT slot 27  in 45.0000  shader 44.8434  prev 45.0000   <- off by 0.157
//   CORRECT slot  6  in 51.2820  shader 51.2820  prev 51.2820   <- exact
//
// The camera that really is rendered matches *exactly*, because the shader
// constant is literally the value we wrote into it one frame earlier. Slot 27 is
// an idle state parked on 45.0000 that drifts into the tolerance whenever the
// rendered field of view passes 45 -- which it does in the middle of every ramp.
// It then wins the one correction this frame is allowed, and the real camera
// goes uncorrected: two structures corrected in alternate frames, 5.5 degrees
// apart.
//
// 1e-4 relative is 0.0045 degrees at that value: roomy against float rounding,
// and still a factor of twenty-four clear of the nearest false positive
// measured. If a true match ever does fall outside it, the sticky render slot
// carries the correction across that frame, which is exactly what it is for.
constexpr float kShaderMatch = 1e-4f;  // relative

// The correction must be applied once per authored value, never to its own
// output. The camera states feed each other, so a value corrected in one of
// them arrives as another one's source. Left unchecked that multiplies every
// frame until the game's own clamp stops it -- measured as roughly a threefold
// zoom where 1.34 was intended.
//
// The first attempt remembered the last 32 values written. That was too short
// by an order of magnitude: two dozen destinations each write once per frame,
// so the memory covered barely a single frame. A value making its way onwards
// two frames later had already been forgotten and got corrected a second time,
// on some frames but not others -- visible as the picture jittering between
// two fields of view during a transition.
//
// So instead of remembering the values, mark them. The low eight mantissa bits
// carry a constant pattern, which for a value near 34 degrees moves it by about
// a thousandth of a degree -- far below anything visible, and unmistakable on
// the way back in.
//
// An authored value can carry the pattern by chance, once in 256. The cost is a
// single uncorrected frame, which nobody can see.
// The compact ramp trace. Big enough for several transitions in and out, small
// enough that the log stays readable.
constexpr int kMaxRampLines = 600;
std::atomic<int> g_ramp_lines{0};
std::atomic<DWORD> g_ramp_tick{0};
float g_ramp_last_out = 0.0f;
std::atomic<std::size_t> g_ramp_last_slot{static_cast<std::size_t>(-1)};
// 0 = nothing logged yet, 1 = correcting, 2 = sliding.
std::atomic<int> g_ramp_state{0};

// Finding the camera cut.
//
// Both attempts at the transition were keyed on the letterbox weight, and both
// were wrong in the same way: the bars are a mask, the game does not reframe
// while they slide. It reframes at the cut, and there a change of focal length
// is invisible because the whole picture changes anyway.
//
// So the cut is what the correction should hang on, and this finds it: the
// authored input of the rendered camera sits at a constant 51.2820 through the
// whole ramp, so the moment it jumps is the cut. Half a degree is well above
// the drift of a moving camera and far below the several degrees a cut brings.
constexpr float kCutThreshold = 0.5f;
constexpr unsigned kMaxCutLines = 200;
std::atomic<unsigned> g_cut_lines{0};
std::atomic<float> g_dst_last_in[kMaxDestinations]{};

// One degree is far more than the rounding between our write and the shader
// constant (measured at 0.0000 through whole ramps) and far less than the
// several degrees a mistaken camera would be off by.
constexpr float kRenderedMiss = 1.0f;
constexpr unsigned kMaxMissLines = 200;
std::atomic<unsigned> g_miss_lines{0};
std::atomic<float> g_last_our_output{0.0f};

constexpr unsigned kMaxScreenLines = 4000;
std::atomic<unsigned> g_screen_lines{0};
std::atomic<float> g_last_screen{0.0f};
std::atomic<float> g_prev_weight{0.0f};

constexpr unsigned kMaxNocorrLines = 300;
std::atomic<unsigned> g_nocorr_lines{0};

constexpr std::uint32_t kTagMask = 0x000000FFu;
constexpr std::uint32_t kTagValue = 0x000000A5u;

float tag(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bits = (bits & ~kTagMask) | kTagValue;
    float tagged = 0.0f;
    std::memcpy(&tagged, &bits, sizeof(tagged));
    return tagged;
}

bool carries_tag(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & kTagMask) == kTagValue;
}

// There used to be a second test here: treat a value as ours if it came within
// 5e-4 relative of any of the last 512 values written. It was a patch for the
// old design, which corrected every camera state, and it became actively
// harmful once only the rendered camera is corrected.
//
// The reason is arithmetic. During a ramp consecutive outputs differ by about
// 0.03, so 512 of them cover the whole range without gaps, and a tolerance of
// 0.02 then matches *everything*. The guard had turned into a sieve: measured
// over 3000 calls it discarded 1606 legitimate corrections and let 110 through.
//
// The tag stays. It is exact, cannot drift into a false positive beyond one in
// 256, and still catches the one case that matters -- the rendered camera being
// handed back its own previous value as a source.
// The tag alone was not enough, and the comment above says why without drawing
// the conclusion: an authored value carries the pattern by chance once in 256,
// and "the cost is a single uncorrected frame, which nobody can see" is simply
// wrong. The correction is nine and a half degrees, and a single frame of it
// missing is a visible hop.
//
// Measured on the shader constant -- the value that reached the picture, not one
// of ours: 14 jumps larger than a degree in 716 frames of one cutscene, every
// one of them the screen flipping between 39.3141 and 29.7733, which are the
// authored and the corrected form of the same camera. That is the judder, and
// the rate matches a one-in-256 coincidence across several calls per frame.
//
// So the tag stays as a cheap pre-filter, and a hit is then verified against the
// values we actually left behind. A genuine copy of our output is bit-identical
// to one of them; a coincidence is not. Sixty-four comparisons, only on the one
// call in 256 that carries the pattern.
// CORRECTION: the first version of this compared against g_dst_last_final, and
// that verified almost nothing.
//
// finish() stores a value there on *every* path, including skip-not and
// gameplay, so the table holds every authored value the game has recently used
// as well as our own. An authored 39.2995 that hits the tag by chance then finds
// itself in the table -- put there by another structure a frame earlier -- and
// is waved through as "ours". The measurement said so plainly: 300 of 300
// uncorrected frames in a settled cutscene left through skip-own.
//
// A dedicated ring, written only where a correction actually happens, is the
// thing the tag needed to be checked against.
// Sized for the settled-cutscene mode below, which corrects every camera state
// rather than one: that is roughly 28 writes per frame, so 32 entries would hold
// barely a single frame and a value copied onwards two frames later would look
// foreign and be corrected a second time. 256 covers about nine frames.
//
// Enlarging it is only safe because the comparison is exact. The ring that
// failed before compared with a tolerance, where more entries meant a wider net
// and eventually matched everything.
constexpr std::size_t kOutputRing = 256;
std::atomic<std::uint32_t> g_our_outputs[kOutputRing]{};
std::atomic<std::size_t> g_output_next{0};

void remember_our_output(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::size_t index = g_output_next.fetch_add(1, std::memory_order_relaxed) % kOutputRing;
    g_our_outputs[index].store(bits, std::memory_order_relaxed);
}

bool matches_something_we_wrote(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (std::size_t i = 0; i < kOutputRing; ++i) {
        if (g_our_outputs[i].load(std::memory_order_relaxed) == bits) {
            return true;
        }
    }
    return false;
}

bool is_our_own_output(float value) {
    return carries_tag(value) && matches_something_we_wrote(value);
}

// One correction per frame, during a ramp.
//
// The tag catches a corrected value that is copied onwards untouched. It cannot
// catch one that has been through the blend spring, because arithmetic rounds
// the low mantissa bits away -- and that is exactly what happens while the bars
// move. The result was two or three corrections in the same frame, compounding
// into a value 25 percent too narrow.
//
// The earlier answer to this was a tolerance against recently written values.
// It works, but its correctness depends on picking a ring size and a threshold
// that separate "the same value after arithmetic" from "the next frame's
// value", and I got that badly wrong once already: 512 entries turned the guard
// into a sieve that discarded 94 percent of all corrections.
//
// The weight is a better frame marker than any tolerance. The game recomputes
// it once per frame, so during a ramp an identical weight means we are still in
// the same frame and have already done our work. No tuning, no thresholds.
//
// At a settled weight this does not apply -- there the value arrives as a plain
// copy and the tag is enough, which the logs confirm.
std::atomic<float> g_corrected_at_weight{-1.0f};

// The rule is now "at most one *structure* per frame", not "at most one
// correction per frame". Both extremes were measured and both are wrong:
//
//   one correction per frame  -- the rendered structure is called twice per
//                                frame during a fade, and the game's second,
//                                later write was waved through and overwrote
//                                the correction. The shader constant read 51.2
//                                while we were writing 40.x, so the correction
//                                never reached the screen, and whether it did
//                                varied frame to frame. That was the judder.
//
//   correct everything        -- fixes that (the shader constant now follows our
//                                output), but lets the correction compound: a
//                                second structure receives our own output as its
//                                input and gets corrected again. Measured as a
//                                step of -0.295 where every other frame stepped
//                                -0.155, i.e. exactly twice. In one scene that
//                                happened often enough to look abrupt.
//
// Correcting the same structure repeatedly is safe, because each call brings a
// fresh authored value; correcting a *different* one in the same frame is what
// compounds. So the guard keys on the slot rather than on the correction count.
std::atomic<bool> g_once_per_frame{true};

// Which structure took this frame's correction.
std::atomic<std::size_t> g_frame_slot{static_cast<std::size_t>(-1)};

// Which structure was last seen carrying the value that got rendered.
std::atomic<std::size_t> g_render_slot{static_cast<std::size_t>(-1)};

// Some destinations are temporaries on the stack -- the log shows addresses in
// the thread stack range receiving two different camera values in the same
// frame. Stack addresses get reused, so without eviction the table fills up with
// addresses that no longer mean anything, and once it is full a genuine camera
// gets no slot at all. No slot means "not the rendered camera", which means the
// correction silently stops. Least recently used wins, so transients age out.
std::atomic<unsigned long long> g_dst_clock{0};
std::atomic<unsigned long long> g_dst_used[kMaxDestinations]{};
std::atomic<unsigned long long> g_dst_evictions{0};
constexpr unsigned long long kMaxEvictionLines = 60;
std::atomic<unsigned long long> g_evictions_logged{0};

bool is_stack_address(std::uintptr_t addr) {
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    return addr >= low && addr < high;
}

std::size_t record_destination(std::uintptr_t dst) {
    const unsigned long long now = g_dst_clock.fetch_add(1, std::memory_order_relaxed);
    const std::size_t known = g_dst_known.load(std::memory_order_acquire);

    for (std::size_t i = 0; i < known; ++i) {
        if (g_dst[i] == dst) {
            g_dst_count[i].fetch_add(1, std::memory_order_relaxed);
            g_dst_used[i].store(now, std::memory_order_relaxed);
            return i;
        }
    }

    std::size_t slot = known;
    if (known >= kMaxDestinations) {
        // Evict the least recently used entry.
        slot = 0;
        unsigned long long oldest = g_dst_used[0].load(std::memory_order_relaxed);
        for (std::size_t i = 1; i < kMaxDestinations; ++i) {
            const unsigned long long used = g_dst_used[i].load(std::memory_order_relaxed);
            if (used < oldest) {
                oldest = used;
                slot = i;
            }
        }
        g_dst_evictions.fetch_add(1, std::memory_order_relaxed);

        // Log it, because this is the leading suspect for the frames where no
        // correction happens at all.
        //
        // Measured: every missed frame sits on slot 25, whose addresses are in
        // the thread stack range, and the frame time at those points is a normal
        // 15 ms -- so the game did not hitch, we simply did not correct. An
        // eviction would explain it exactly: the history is cleared below, the
        // rendered-camera test needs a non-zero history, and so the frame goes
        // uncorrected and the next step is twice the size. The two ramps with no
        // outliers at all used slots 41 and 24 and never touched 25.
        //
        // That is a good story and it is still only a story until this line
        // shows up next to a missed frame.
        if (g_evictions_logged.fetch_add(1, std::memory_order_relaxed) < kMaxEvictionLines) {
            logger::info("EVICT slot {:<3} was 0x{:012X} (stack {})  now 0x{:012X} (stack {})",
                         slot, g_dst[slot], is_stack_address(g_dst[slot]) ? "yes" : "no ", dst,
                         is_stack_address(dst) ? "yes" : "no ");
        }
        // A reused slot must not inherit the previous occupant's history, or the
        // rendered-camera test compares against a value from a different camera.
        g_dst_shader_hits[slot].store(0, std::memory_order_relaxed);
        g_dst_shader_samples[slot].store(0, std::memory_order_relaxed);
        g_dst_last_final[slot].store(0.0f, std::memory_order_relaxed);
        if (g_render_slot.load(std::memory_order_relaxed) == slot) {
            g_render_slot.store(static_cast<std::size_t>(-1), std::memory_order_relaxed);
        }
    }

    g_dst[slot] = dst;
    g_dst_count[slot].store(1, std::memory_order_relaxed);
    g_dst_used[slot].store(now, std::memory_order_relaxed);
    if (slot == known) {
        g_dst_known.store(known + 1, std::memory_order_release);
    }
    return slot;
}

void detour(std::uintptr_t dst, std::uintptr_t src) {
    g_original(dst, src);

    const std::uintptr_t fov_addr =
        dst + static_cast<std::uintptr_t>(patterns::kCameraStateFov);

    // Restricting the correction to the master was wrong: a read+write
    // watchpoint showed the master is read by exactly one instruction in the
    // whole game, the getter -- and hooking that getter changed nothing on
    // screen. So the master feeds peripheral consumers only, and the projection
    // takes its FOV from a different camera state.
    //
    // ApplyCameraState is generic and runs for two dozen camera states. Only the
    // one that ends up rendered may be corrected -- see the shader scoring below.
    const std::size_t slot = record_destination(dst);
    const float original = read_float(fov_addr);

    // Is *this* destination the one being rendered right now?
    //
    // Asked per call, not accumulated over the session. Which camera gets
    // rendered changes -- gameplay uses one, a cutscene another, a transition
    // the blend of two -- so a hit rate over the whole run averages away
    // precisely the thing it is supposed to detect. The instantaneous test also
    // repairs itself: a camera that stops being rendered simply stops being
    // corrected, and the game overwrites it from its authored source anyway.
    bool is_rendered = false;
    if (slot != kNoSlot) {
        const float shader = read_float(g_shader_fov_addr);
        const float previous = g_dst_last_final[slot].load(std::memory_order_relaxed);
        const bool matches = previous != 0.0f &&
                             std::fabs(previous - shader) <= std::fabs(shader) * kShaderMatch;

        // Sticky, because the game cuts between cameras. At a cut the shader
        // constant jumps and for one frame nothing matches it -- measured as
        // four frames out of 489, each landing on a cut, and each visible as a
        // step of twice the usual size. Remembering which structure was the
        // rendered one carries the correction across that gap; it is handed on
        // as soon as another structure genuinely matches.
        // Back to the form that demonstrably produced a correct picture.
        //
        // Restricting this to the single remembered slot was meant to stop the
        // correction chaining from one camera state into the next. It stopped
        // the correction altogether: the slot is claimed once and then never
        // matches again, so nothing gets corrected and the picture flickers
        // between corrected and untouched. A fix that silences the feature is
        // worse than the fault it was aimed at, so it goes back until the
        // decisions have actually been measured rather than guessed at.
        if (matches) {
            g_render_slot.store(slot, std::memory_order_relaxed);
        }
        is_rendered = matches || g_render_slot.load(std::memory_order_relaxed) == slot;

        // Bookkeeping for the report only.
        g_dst_shader_samples[slot].fetch_add(1, std::memory_order_relaxed);
        if (is_rendered) {
            g_dst_shader_hits[slot].fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Read the weight up front so the decision log can carry it. Cheap, and it
    // keeps every exit path uniform.
    float weight = (g_config.mode == Mode::Corrected) ? read_float(g_weight_addr) : 0.0f;
    if (g_config.mode == Mode::Corrected && g_force.load() && weight <= 0.0f) {
        weight = 1.0f;
    }
    const float shader_now = (slot != kNoSlot) ? read_float(g_shader_fov_addr) : 0.0f;
    const float previous_now =
        (slot != kNoSlot) ? g_dst_last_final[slot].load(std::memory_order_relaxed) : 0.0f;

    // Every path from here on must leave the current value behind, or the test
    // above goes stale. The previous version returned early in gameplay without
    // storing, which broke the detection for exactly the cameras it needed.
    //
    // It also logs. Four attempts at this jitter went wrong partly because the
    // log only ever showed the calls that *were* corrected -- never the ones
    // that were skipped, nor why. During a transition every decision is now
    // recorded, whatever it was.
    const auto finish = [&](float final_value, const char* decision) {
        if (slot != kNoSlot) {
            g_dst_last_final[slot].store(final_value, std::memory_order_relaxed);
        }

        // Each cutscene gets a fresh budget.
        //
        // Reaching the scene that actually misbehaves takes several minutes of
        // loading and riding, and every cutscene on the way would otherwise
        // spend the budget before the interesting one starts. Resetting at each
        // cutscene boundary means the last one in the log is always complete,
        // whatever happened before it.
        {
            const float previous_weight = g_prev_weight.exchange(weight, std::memory_order_relaxed);
            if (weight > 0.0f && previous_weight <= 0.0f) {
                g_screen_lines.store(0, std::memory_order_relaxed);
                g_last_screen.store(0.0f, std::memory_order_relaxed);
                logger::info("--- cutscene begins, screen trace reset ---");
            }
        }

        // What is actually on screen, frame by frame.
        //
        // Every measurement so far has been of one of our own internal
        // quantities, and two of them turned out to measure my own mistakes
        // rather than the game. The shader constant is not ours: it carries the
        // field of view that reached the picture. Whatever hop is visible has to
        // appear in this column, or it is not a field-of-view hop at all.
        //
        // It changes once per frame, so logging only its changes gives one line
        // per frame and no more. A hop reads as A -> B -> A within a few lines.
        if (weight > 0.0f && shader_now != 0.0f &&
            g_screen_lines.load(std::memory_order_relaxed) < kMaxScreenLines) {
            const float last = g_last_screen.load(std::memory_order_relaxed);
            if (std::fabs(shader_now - last) > 1e-4f) {
                g_last_screen.store(shader_now, std::memory_order_relaxed);
                g_screen_lines.fetch_add(1, std::memory_order_relaxed);
                logger::info("SCREEN w {:.4f}  {:8.4f} -> {:8.4f}  ({:+7.4f})", weight, last,
                             shader_now, shader_now - last);
            }
        }

        // Which exit did an uncorrected frame take?
        //
        // The screen trace proves single frames still go uncorrected in the
        // settled part of a cutscene -- 39.3082 -> 29.7674 -> 39.2995 on three
        // consecutive frames, corrected, then not, then corrected again. Nine
        // and a half degrees each way.
        //
        // There are several ways out of this function that leave the value
        // alone, and none of them says so at full weight, so the culprit is
        // still a guess. This names it: the rendered camera, at settled weight,
        // not corrected.
        if (weight >= kSettledWeight && is_rendered &&
            std::strcmp(decision, "CORRECT") != 0 &&
            g_nocorr_lines.fetch_add(1, std::memory_order_relaxed) < kMaxNocorrLines) {
            logger::info("NOCORR w {:.4f}  slot {:<3} decision {:<8} in {:8.4f}  prev {:8.4f}"
                         "  shader {:8.4f}",
                         weight, slot == kNoSlot ? -1 : static_cast<int>(slot), decision, original,
                         previous_now, shader_now);
        }

        // Did the game render what we wrote?
        //
        // The shader constant carries the field of view that reached the picture,
        // one frame late. So comparing it against our own last correction is a
        // direct test of whether the correction landed in the camera that was
        // actually used. During the ramps this was measured at a difference of
        // 0.0000 -- our value was rendered exactly.
        //
        // The open question is the settled part of a cutscene, where the game
        // cuts between shots at 13.6, 18.2, 20.3, 27.0 and 39.3 degrees, and the
        // gameplay camera's 51.2820 is among the structures in play. If the
        // rendered-camera test picks the wrong one for a frame at such a cut, we
        // write a correction twenty degrees off and the picture hops. Each line
        // here is one such frame.
        if (std::strcmp(decision, "CORRECT") == 0) {
            const float ours = g_last_our_output.exchange(final_value, std::memory_order_relaxed);
            if (ours != 0.0f && std::fabs(shader_now - ours) > kRenderedMiss &&
                g_miss_lines.fetch_add(1, std::memory_order_relaxed) < kMaxMissLines) {
                logger::info("MISS  w {:.4f}  slot {:<3} we wrote {:8.4f}, the picture shows"
                             " {:8.4f}  (off by {:+.4f})  now correcting {:8.4f} -> {:8.4f}",
                             weight, slot == kNoSlot ? -1 : static_cast<int>(slot), ours,
                             shader_now, shader_now - ours, original, final_value);
            }
        }

        // Where is the cut, relative to the bar weight? See kCutThreshold.
        // Only during cutscenes: the first run spent 136 of its 200 lines on
        // gameplay cuts, which are not the question.
        if (is_rendered && slot != kNoSlot && weight > 0.0f) {
            const float last_in =
                g_dst_last_in[slot].exchange(original, std::memory_order_relaxed);
            if (last_in != 0.0f && std::fabs(original - last_in) > kCutThreshold &&
                g_cut_lines.fetch_add(1, std::memory_order_relaxed) < kMaxCutLines) {
                logger::info("CUT   w {:.4f}  slot {:<3} in {:8.4f} -> {:8.4f}  (delta {:+.4f})"
                             "  shader {:8.4f}  decision {}",
                             weight, slot == kNoSlot ? -1 : static_cast<int>(slot), last_in,
                             original, original - last_in, shader_now, decision);
            }
        }
        // Only the rendered camera is interesting, and there are two dozen
        // states per frame -- logging all of them filled the budget inside a
        // single transition last time, so the second one went unobserved.
        // Log the first stretch of decisions in every cutscene, not only during
        // a ramp and not only the corrections. Two regressions in a row were
        // diagnosed from logs that only recorded what I had already decided to
        // keep -- the rejected calls are where the answer was both times.
        const bool in_transition = weight > 0.0f && weight < 0.999f;
        const bool worth_logging = weight > 0.0f && g_samples.load() < kMaxSamples;
        if ((kLogEveryDecision || g_samples.load() < 120) && worth_logging) {
            g_samples.fetch_add(1);
            logger::info(
                "  {:<8} slot {:<3} dst 0x{:012X}  w {:.4f}  in {:9.4f}  prev {:9.4f}"
                "  shader {:9.4f}  -> {:9.4f}{}",
                decision, slot == kNoSlot ? -1 : static_cast<int>(slot), dst, weight, original,
                previous_now, shader_now, final_value, in_transition ? "  [ramp]" : "");
        }

        // A second, compact trace: one line per corrected frame, for the whole
        // ramp.
        //
        // The log above records every decision, which is two dozen states per
        // frame, so its 120-line budget is spent after four frames of a ramp
        // that lasts about eighty. Every attempt at the transition judder so far
        // was therefore argued from six percent of the evidence, mine included.
        // This costs one line per frame and covers the whole thing.
        //
        // The interval is in here because the weight advances with frame time:
        // measured steps varied from 0.0086 to 0.0160, and whether that is the
        // game's pacing or something of ours cannot be told without knowing how
        // long the frame took.
        // Log the correction, and also whatever happened to the structure that
        // was corrected last time. On the three bad frames the usual camera was
        // passed over, and the reason it was passed over is the whole question;
        // the previous trace recorded only the winner and so could not answer
        // it.
        const bool is_correction = std::strcmp(decision, "CORRECT") == 0;
        const bool is_sliding = std::strcmp(decision, "sliding") == 0;

        // Log every *change* of state for the rendered camera, over the whole
        // cutscene rather than only its ramps.
        //
        // The previous condition hung on "a correction during a ramp", and with
        // the correction now suppressed while the bars slide, that is never
        // true -- so the trace went silent exactly where the remaining problem
        // is. And the per-decision log is spent by weight 0.0665, which is the
        // first few frames.
        //
        // One line per transition between correcting and not correcting counts
        // flapping directly: if the weight hovers around the threshold, the
        // correction switches on and off, and each switch is a twelve-degree
        // jump. That would look exactly like hopping back and forth.
        bool changed = false;
        if (is_correction || is_sliding) {
            const int state = is_correction ? 1 : 2;
            changed = g_ramp_state.exchange(state, std::memory_order_relaxed) != state;
        }

        if (weight > 0.0f && changed &&
            g_ramp_lines.load(std::memory_order_relaxed) < kMaxRampLines) {
            g_ramp_lines.fetch_add(1, std::memory_order_relaxed);
            const DWORD now = GetTickCount();
            const DWORD previous_tick = g_ramp_tick.exchange(now, std::memory_order_relaxed);
            const long interval =
                previous_tick == 0 ? 0 : static_cast<long>(now) - static_cast<long>(previous_tick);
            // Slot, destination and shader constant are in here because the
            // first version of this trace proved that three frames per ramp
            // arrive with 45.0000 instead of the cutscene camera's 48.8400 --
            // and then could not say *which* structure those three were, nor
            // what the shader constant was at that moment. Without those two
            // columns the only way to choose a fix is to guess, and the guess I
            // made from the incomplete trace broke the picture outright.
            logger::info(
                "RAMP  {:<8} w {:.4f}  slot {:<3} dst 0x{:012X}  in {:8.4f}  out {:8.4f}"
                "  shader {:8.4f}  prev {:8.4f}  dFOV {:+7.4f}  {:3d} ms",
                decision, weight, slot == kNoSlot ? -1 : static_cast<int>(slot), dst, original,
                final_value, shader_now, previous_now, final_value - g_ramp_last_out, interval);
            if (is_correction) {
                g_ramp_last_out = final_value;
                g_ramp_last_slot.store(slot, std::memory_order_relaxed);
            }
        }
    };

    // Everything that is not the rendered camera stays exactly as authored. It
    // may well be a source for the blend, and the blend has to work on authored
    // values if the correction is to remain a single step.
    //
    // is_our_own_output stays as a safety net for the case of two rendered
    // cameras feeding each other.
    // In an established cutscene, correct every camera state rather than only
    // the one identified as rendered.
    //
    // The identification costs a frame at every cut: the shader constant carries
    // what was rendered one frame late, so when the game switches a shot onto a
    // different structure, that structure is not yet confirmed and goes
    // uncorrected for one frame. Measured as exactly the three remaining jumps,
    // each nine and a half degrees, each immediately after a cut.
    //
    // Correcting everything removes the need to predict the cut. This is how the
    // plugin worked originally, and it failed then by compounding into a
    // threefold zoom -- because a corrected value arriving as another state's
    // input was corrected again. What was missing then is now in place and
    // measured: is_our_own_output checks the tag *and* exact membership in the
    // ring of values we actually wrote.
    //
    // Restricted to full weight in an established cutscene on purpose. During a
    // ramp the blend spring mixes states arithmetically, which destroys both the
    // tag and the exact match, and that is the ground the old design foundered
    // on. At a settled weight there is no such blend.
    const bool settled_cutscene = kCorrectAllWhenSettled && weight >= kSettledWeight &&
                                  g_reached_settled.load(std::memory_order_relaxed);
    if (!is_rendered && !settled_cutscene) {
        finish(original, "skip-not");
        return;
    }
    if (is_our_own_output(original)) {
        finish(original, "skip-own");
        return;
    }

    // Already corrected in this frame -- see g_corrected_at_weight.
    // Toggleable, because the ramp trace suggests this rule is what is left of
    // the transition judder and I am not willing to guess at it again.
    //
    // The evidence: during a fade-out the rendered camera's structure is called
    // *twice* per frame. First with the authored 51.2820, which we correct to
    // about 40. Then again with the game's own blend value -- 51.2419, 51.2114,
    // 51.1766 -- which this rule waves through, overwriting our correction. The
    // shader constant proves it goes on to be rendered: it reads 51.2x while we
    // are writing 40.x.
    //
    // Whether the correction survives a frame therefore depends on whether the
    // game happens to write again after us, which varies frame to frame. That is
    // a judder by construction.
    //
    // Turning the rule off corrects every authored value arriving at the
    // rendered camera, including the late one. The risk is the compounding this
    // rule was built to stop -- but the tag already catches our own output, and
    // the second write measurably is not our output. Ctrl+Alt+1 switches it, so
    // the question gets answered in one cutscene rather than in another build.
    const bool ramping = weight > 0.0f && weight < 0.999f;
    if (g_once_per_frame.load(std::memory_order_relaxed) && ramping &&
        weight == g_corrected_at_weight.load(std::memory_order_relaxed) &&
        slot != g_frame_slot.load(std::memory_order_relaxed)) {
        finish(original, "skip-oth");
        return;
    }

    float result = original;

    switch (g_config.mode) {
        case Mode::Off:
        case Mode::Poke:
        case Mode::Watch:
            finish(original, "skip-off");
            return;

        case Mode::Test:
        case Mode::TestWatch:
            result = original * g_config.test_factor;
            break;

        case Mode::Corrected: {
            if (weight <= 0.0f) {
                g_reached_settled.store(false, std::memory_order_relaxed);
                finish(original, "gameplay");  // leave the camera exactly as authored
                return;
            }
            // The two ends of a cutscene are not symmetric, which the first
            // version of this rule got wrong.
            //
            // Fading in, the game still renders the gameplay camera while the
            // bars slide over it -- measured as a constant authored 51.2820
            // through the whole ramp. Correcting there invents a zoom the game
            // never performs.
            //
            // Fading out it keeps the cutscene shot: at weight 0.9686 the screen
            // still showed 39.3141, the cutscene camera, and only later cut to
            // gameplay. Stopping the correction when the weight starts to fall
            // therefore produced a nine-and-a-half degree jump of our own making,
            // one frame long, right at the end of every cutscene.
            //
            // So the test is not "are the bars moving" but "has this cutscene
            // already been established". Once the weight has reached full in this
            // cutscene, it stays corrected until gameplay resumes.
            if (weight >= kSettledWeight) {
                g_reached_settled.store(true, std::memory_order_relaxed);
            }
            if (kCorrectOnlySettled && weight < kSettledWeight &&
                !g_reached_settled.load(std::memory_order_relaxed)) {
                finish(original, "sliding");
                return;
            }
            // k straight from the game's own bar height: it already divides by
            // the true backbuffer aspect, so this is correct in windowed mode
            // and at non-native resolutions without asking Windows.
            const float bar = read_float(g_bar_addr);
            double k = framing::correction_factor_from_bars(bar, weight);
            if (!(k > 0.0 && k < 1.0)) {
                k = framing::correction_factor(GetSystemMetrics(SM_CXSCREEN),
                                               GetSystemMetrics(SM_CYSCREEN));
            }
            // Strength scales how far k moves away from 1, still in tangent
            // space, so the blend with the letterbox weight stays intact.
            const double scaled = 1.0 + (k - 1.0) * static_cast<double>(g_strength.load());
            const double factor = framing::blended_factor(scaled, weight);
            result = static_cast<float>(framing::corrected_vfov_deg(original, factor));

            // Order matters: k came from the bar height just above, so the
            // heights may only be cleared afterwards. The camera update runs
            // early in the frame, well before the scissor is set.
            if (g_flatten.load(std::memory_order_relaxed)) {
                const std::uintptr_t bar235 = g_weight_addr +
                                              patterns::letterbox::kBarFraction235 -
                                              patterns::letterbox::kWeight;
                if (g_flatten_logged.fetch_add(1) == 0) {
                    logger::info("flatten: bar heights were {:.5f} / {:.5f}, now zero",
                                 read_float(bar235), read_float(g_bar_addr));
                }
                const float zero = 0.0f;
                std::memcpy(reinterpret_cast<void*>(bar235), &zero, sizeof(zero));
                std::memcpy(reinterpret_cast<void*>(g_bar_addr), &zero, sizeof(zero));
            }


            break;
        }
    }

    result = tag(result);
    // Only corrections go into the ring. That is the whole point of it: it has
    // to hold what we wrote, not what the game wrote.
    remember_our_output(result);
    std::memcpy(reinterpret_cast<void*>(fov_addr), &result, sizeof(result));
    if (ramping) {
        g_corrected_at_weight.store(weight, std::memory_order_relaxed);
        g_frame_slot.store(slot, std::memory_order_relaxed);
    }
    finish(result, "CORRECT");

    const int call = g_calls.fetch_add(1);
    if (call < kLoggedCalls) {
        logger::info("  ApplyCameraState {}: dst 0x{:016X}  {:.4f} -> {:.4f}   (weight {:.4f})",
                     call + 1, dst, original, result, weight);
    }
}

}  // namespace

Config read_config() {
    Config config;

    const char* compiled = "?";
    switch (kCompiledDefaultMode) {
        case Mode::Off: compiled = "OFF"; break;
        case Mode::Test: compiled = "TEST"; break;
        case Mode::Corrected: compiled = "CORRECTED"; break;
        case Mode::Poke: compiled = "POKE"; break;
    }
    logger::info("compiled-in default mode: {}", compiled);

    std::filesystem::path path = logger::path();
    if (path.empty()) {
        return config;
    }
    path.replace_filename(L"fov.txt");

    // CreateFileW rather than ifstream: the first attempt at this silently
    // reported "no fov.txt" for a file that demonstrably existed and was
    // readable, and an ifstream failure carries no reason. Going through Win32
    // directly gives us GetLastError to log.
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        logger::info("fov.txt not readable (error {}) -- using the compiled-in mode", error);
        logger::info("  tried: {}", path.string());

        // Error 2 on a file that demonstrably exists, in a directory this same
        // process just created the log in. Enumerate the directory to see what
        // the game process actually thinks is there -- that distinguishes "the
        // file is filtered" from "this is a different directory entirely".
        std::filesystem::path pattern = path;
        pattern.replace_filename(L"*");

        WIN32_FIND_DATAW found{};
        const HANDLE search = FindFirstFileW(pattern.c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) {
            logger::info("  directory enumeration failed too (error {})", GetLastError());
        } else {
            logger::info("  what this process sees in that directory:");
            int entries = 0;
            do {
                const std::wstring name = found.cFileName;
                if (name != L"." && name != L"..") {
                    logger::info("    {}", std::filesystem::path(name).string());
                    ++entries;
                }
            } while (FindNextFileW(search, &found) && entries < 20);
            FindClose(search);
            if (entries == 0) {
                logger::info("    (nothing)");
            }
        }
        return config;
    }

    char buffer[128]{};
    DWORD read = 0;
    const bool ok = ReadFile(handle, buffer, sizeof(buffer) - 1, &read, nullptr) != 0;
    CloseHandle(handle);
    if (!ok || read == 0) {
        logger::info("fov.txt is empty or unreadable -- defaulting to TEST mode");
        return config;
    }

    std::istringstream file(std::string(buffer, read));
    logger::info("fov.txt: '{}'", std::string(buffer, read).substr(0, read));

    std::string word;
    file >> word;
    for (char& c : word) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }

    if (word == "real") {
        config.mode = Mode::Corrected;
    } else if (word == "off") {
        config.mode = Mode::Off;
    } else if (word == "poke") {
        config.mode = Mode::Poke;
        float value = 0.0f;
        if (file >> value && value > 1.0f && value < 170.0f) {
            config.poke_value = value;
        }
    } else if (word == "test") {
        config.mode = Mode::Test;
        float factor = 0.0f;
        if (file >> factor && factor > 0.05f && factor < 4.0f) {
            config.test_factor = factor;
        }
    } else {
        logger::info("fov.txt: did not understand '{}' -- using TEST mode", word);
    }
    return config;
}

bool install(const std::vector<mem::NamedRegion>& sections, const mem::Region& module,
             std::uintptr_t anchor, const Config& config) {
    g_config = config;
    g_module_base = module.base;
    g_shader_fov_addr = module.base + patterns::candidates::kDegreeCopyB;
    g_weight_addr = anchor + patterns::letterbox::kWeight;
    g_bar_addr = anchor + patterns::letterbox::kBarFractionDisplay;

    const auto pattern = mem::parse_pattern(patterns::kFovGetter);
    if (!pattern) {
        logger::info("FOV getter signature is malformed -- not installing");
        return false;
    }

    std::vector<std::uintptr_t> hits;
    for (const auto& [name, region] : sections) {
        mem::find_all(region, *pattern, hits);
    }
    if (hits.size() != 1) {
        logger::info("FOV getter signature matched {} time(s), need exactly 1 -- not installing",
                     hits.size());
        return false;
    }

    const std::uintptr_t getter = hits.front() + patterns::kFovGetterOffset;

    // The signature borrows its uniqueness from a neighbouring function, so
    // verify rather than trust: the movss must point somewhere sane, and what
    // it points at must look like a field of view.
    const std::uintptr_t target = mem::resolve_rip_relative(
        getter, patterns::kFovMovssDispOffset, patterns::kFovMovssLength);
    if (!module.contains(target)) {
        logger::info("FOV getter reads 0x{:016X}, outside the module -- not installing", target);
        return false;
    }

    logger::info("FOV getter at module +0x{:X}, reads module +0x{:X}", getter - module.base,
                 target - module.base);

    // At plugin load the game has not rendered yet and the value is still 0.0,
    // so checking it immediately rejects a perfectly good hook. Wait for the
    // camera to come up first -- the observation run showed it takes about a
    // second, but loading a save takes considerably longer.
    constexpr DWORD kStepMs = 500;
    constexpr DWORD kMaxWaitMs = 5 * 60 * 1000;

    float current = read_float(target);
    DWORD waited = 0;
    bool announced = false;
    while (!(current >= patterns::kFovSanityMin && current <= patterns::kFovSanityMax)) {
        if (waited >= kMaxWaitMs) {
            logger::info("  value never became plausible (last: {:.4f}) -- not installing",
                         current);
            logger::info("  (expected {:.0f}..{:.0f} degrees)", patterns::kFovSanityMin,
                         patterns::kFovSanityMax);
            return false;
        }
        if (!announced) {
            logger::info("  value is {:.4f} -- waiting for the camera to come up", current);
            announced = true;
        }
        Sleep(kStepMs);
        waited += kStepMs;
        current = read_float(target);
    }

    logger::info("  reads {:.4f} degrees after {:.1f} s -- plausible", current,
                 static_cast<double>(waited) / 1000.0);
    g_master_addr = target;

    if (g_config.mode == Mode::Poke) {
        logger::info("mode POKE -- not hooking; the global is overwritten directly");
        return true;
    }
    if (g_config.mode == Mode::Watch) {
        logger::info("mode WATCH -- not hooking; looking for whoever writes the global");
        return true;
    }

    // The getter only told us where the master lives. What actually has to be
    // hooked is the function that writes it, found with the watchpoint.
    const auto apply_pattern = mem::parse_pattern(patterns::kCameraApply);
    if (!apply_pattern) {
        logger::info("ApplyCameraState signature is malformed -- not installing");
        return false;
    }
    std::vector<std::uintptr_t> apply_hits;
    for (const auto& [name, region] : sections) {
        mem::find_all(region, *apply_pattern, apply_hits);
    }
    if (apply_hits.size() != 1) {
        logger::info("ApplyCameraState matched {} time(s), need exactly 1 -- not installing",
                     apply_hits.size());
        return false;
    }
    logger::info("ApplyCameraState at module +0x{:X}", apply_hits.front() - module.base);

    g_target = reinterpret_cast<void*>(apply_hits.front());
    if (const MH_STATUS status =
            MH_CreateHook(g_target, reinterpret_cast<void*>(&detour),
                          reinterpret_cast<void**>(&g_original));
        status != MH_OK) {
        logger::info("MH_CreateHook failed: {}", MH_StatusToString(status));
        g_target = nullptr;
        return false;
    }
    if (const MH_STATUS status = MH_EnableHook(g_target); status != MH_OK) {
        logger::info("MH_EnableHook failed: {}", MH_StatusToString(status));
        MH_RemoveHook(g_target);
        g_target = nullptr;
        return false;
    }

    switch (g_config.mode) {
        case Mode::Off:
            logger::info("hook installed, mode OFF -- value passed through unchanged");
            break;
        case Mode::TestWatch:
            logger::info("hook installed, mode TEST+WATCH -- factor {:.3f}, watchpoint follows",
                         g_config.test_factor);
            break;
        case Mode::Test:
            logger::info("hook installed, mode TEST -- every FOV multiplied by {:.3f}",
                         g_config.test_factor);
            logger::info("  this applies in gameplay too, on purpose: the effect must be obvious");
            break;
        case Mode::Corrected:
            logger::info("hook installed, mode CORRECTED -- cutscenes only");
            break;
    }
    return true;
}

void run_poke(unsigned int duration_ms) {
    if (g_master_addr == 0 || g_module_base == 0) {
        logger::info("poke: addresses unknown -- nothing to do");
        return;
    }

    // Every launch costs the player a load, so hammer all three known copies at
    // once. If the picture stays still, all three are excluded in one run. If it
    // moves, a second run narrows it down -- still cheaper than three runs.
    struct Slot {
        const char* name;
        std::uintptr_t address;
        bool writable;
    };
    Slot slots[] = {
        {"master", g_master_addr, false},
        {"degA", g_module_base + patterns::candidates::kDegreeCopyA, false},
        {"degB", g_module_base + patterns::candidates::kDegreeCopyB, false},
    };

    int usable = 0;
    for (Slot& s : slots) {
        MEMORY_BASIC_INFORMATION mbi{};
        const bool ok =
            VirtualQuery(reinterpret_cast<LPCVOID>(s.address), &mbi, sizeof(mbi)) != 0 &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY)) != 0;
        s.writable = ok;
        if (ok) {
            ++usable;
        } else {
            logger::info("poke: {} at module +0x{:X} is not writable (protect 0x{:X})", s.name,
                         s.address - g_module_base, mbi.Protect);
        }
    }
    if (usable == 0) {
        logger::info("poke: nothing writable -- aborting");
        return;
    }

    logger::info("");
    logger::info("=== POKE: writing {:.3f} over {} address(es) for {} s ===", g_config.poke_value,
                 usable, duration_ms / 1000);
    logger::info("Watch the picture. Expect flicker rather than a clean change --");
    logger::info("we are racing the game's own per-frame write, and that is fine:");
    logger::info("flicker answers 'do any of these reach the projection at all'.");

    const DWORD started = GetTickCount();
    DWORD last_report = started;
    unsigned long long writes = 0;
    float seen[3]{};

    while (GetTickCount() - started < duration_ms) {
        for (int i = 0; i < 3; ++i) {
            if (!slots[i].writable) {
                continue;
            }
            seen[i] = read_float(slots[i].address);
            std::memcpy(reinterpret_cast<void*>(slots[i].address), &g_config.poke_value,
                        sizeof(float));
        }
        ++writes;

        const DWORD now = GetTickCount();
        if (now - last_report >= 5000) {
            // What we read back before writing is whatever won the last race.
            // Values close to ours mean the game is not rewriting that slot.
            logger::info("  {} rounds; read back before writing: master {:.4f}, degA {:.4f}, degB {:.4f}",
                         writes, seen[0], seen[1], seen[2]);
            last_report = now;
        }
        Sleep(1);
    }

    logger::info("=== POKE done, {} rounds ===", writes);
}

std::uintptr_t master_address() { return g_master_addr; }

void set_strength(float value) {
    g_strength.store((std::max)(0.0f, (std::min)(1.5f, value)));
}

float strength() { return g_strength.load(); }

void set_force(bool on) { g_force.store(on); }

bool forced() { return g_force.load(); }

void set_flatten_bars(bool on) {
    g_flatten.store(on);
    g_flatten_logged.store(0);
}

bool flattening_bars() { return g_flatten.load(); }

void set_once_per_frame(bool on) { g_once_per_frame.store(on); }

bool once_per_frame() { return g_once_per_frame.load(); }

std::uintptr_t rendered_fov_address() {
    const std::size_t slot = g_render_slot.load(std::memory_order_relaxed);
    if (slot == static_cast<std::size_t>(-1) || slot >= kMaxDestinations) {
        return 0;
    }
    const std::uintptr_t dst = g_dst[slot];
    return dst == 0 ? 0 : dst + patterns::kCameraStateFov;
}


void report_destinations(std::uintptr_t module_base) {
    const std::size_t known = g_dst_known.load();
    logger::info("");
    logger::info("camera-state destinations seen: {} (slots evicted: {})", known,
                 g_dst_evictions.load());
    for (std::size_t i = 0; i < known; ++i) {
        const std::uintptr_t fov_addr = g_dst[i] + patterns::kCameraStateFov;
        const bool is_master = fov_addr == g_master_addr;
        const unsigned samples = g_dst_shader_samples[i].load();
        const unsigned hits = g_dst_shader_hits[i].load();
        const int percent = samples > 0 ? static_cast<int>(hits * 100 / samples) : 0;
        logger::info("    [{:>3}% rendered]", percent);
        // Globals sit inside the module; heap-allocated camera objects do not.
        const bool in_module = g_dst[i] > module_base && g_dst[i] - module_base < 0x8000000;
        if (in_module) {
            logger::info("    module +0x{:<9X}  {} call(s){}", g_dst[i] - module_base,
                         g_dst_count[i].load(), is_master ? "   <- the master" : "");
        } else {
            logger::info("    0x{:016X}  {} call(s)   <- {}", g_dst[i], g_dst_count[i].load(),
                         is_stack_address(g_dst[i]) ? "on the stack (temporary)" : "heap object");
        }
    }
    if (known == kMaxDestinations) {
        logger::info("    (table full -- there may be more)");
    }
}

void uninstall() {
    if (g_target == nullptr) {
        return;
    }
    MH_DisableHook(g_target);
    MH_RemoveHook(g_target);
    g_target = nullptr;
}

}  // namespace fov
