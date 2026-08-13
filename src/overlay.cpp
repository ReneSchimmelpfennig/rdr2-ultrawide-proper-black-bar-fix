#include "overlay.h"

#include <windows.h>

#include <MinHook.h>
#include <intrin.h>

#include <atomic>
#include <cstring>

#include "bars.h"
#include "fov.h"
#include "framing.h"
#include "log.h"
#include "patterns.h"

namespace overlay {
namespace {

// void FitTo16by9(float* size, float* pos) -- both two-component.
using Fn = void(__fastcall*)(float*, float*);

Fn g_original = nullptr;
std::uintptr_t g_site = 0;
std::atomic<Mode> g_mode{Mode::Fitted};
std::atomic<unsigned> g_calls{0};

// The scale the game itself would apply, learned by running the original on a
// scratch pair rather than recomputing it from the resolution. That keeps this
// correct in windowed mode and at non-native resolutions, for the same reason
// the field-of-view correction reads k out of the game's own bar height.
float learn_scale() {
    float size[2] = {1.0f, 1.0f};
    float pos[2] = {0.0f, 0.0f};
    g_original(size, pos);
    return size[0];  // 1/k on a display wider than 16:9
}

constexpr int kMaxCallers = 16;

struct Caller {
    std::uintptr_t ret = 0;
    unsigned count = 0;
    float size_in = 0.0f;
    float pos_in = 0.0f;
    float size_out = 0.0f;
    float pos_out = 0.0f;
};

Caller g_callers[kMaxCallers]{};
std::atomic<int> g_caller_count{0};
CRITICAL_SECTION g_lock{};
bool g_lock_ready = false;

void record(std::uintptr_t ret, float size_in, float pos_in, float size_out, float pos_out) {
    if (!g_lock_ready) {
        return;
    }
    EnterCriticalSection(&g_lock);
    const int used = g_caller_count.load(std::memory_order_relaxed);
    for (int i = 0; i < used; ++i) {
        if (g_callers[i].ret == ret) {
            ++g_callers[i].count;
            LeaveCriticalSection(&g_lock);
            return;
        }
    }
    if (used < kMaxCallers) {
        g_callers[used] = Caller{ret, 1, size_in, pos_in, size_out, pos_out};
        g_caller_count.store(used + 1, std::memory_order_relaxed);
    }
    LeaveCriticalSection(&g_lock);
}

void __fastcall detour(float* size, float* pos) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    // The one hook that runs while the full-screen overlay is up, which is the
    // only stretch where top and bottom bars are still showing.
    bars::probe_during_overlay();
    const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());

    const float size_in = size != nullptr ? *size : 0.0f;
    const float pos_in = pos != nullptr ? *pos : 0.0f;

    switch (g_mode.load(std::memory_order_relaxed)) {
        case Mode::Fitted:
            g_original(size, pos);
            break;

        case Mode::Stretched:
            // Nothing at all: the caller's 1.0 and 0.0 reach the shader and the
            // asset is sampled across the full width.
            break;

        case Mode::Cover: {
            // Wider than the film frame: the overlay belongs in the 2.35 window,
            // not on the whole screen.
            //
            // Covering the full width of a 32:9 screen means scaling a 16:9
            // asset by 2.0 and cropping half its height -- and the edges of the
            // asset then sit inside the picture as ragged bands. That is what
            // came back from the first 32:9 test, reported as frayed bars
            // appearing from the overlay sequence onwards, which is exactly when
            // this runs.
            //
            // The picture itself is framed at 2.35 by then, so the overlay has
            // to be framed the same way or the two disagree.
            // Note the direction: in this mapping a *larger* size makes the
            // asset appear *smaller*, because it scales the texture coordinate
            // rather than the picture. The game's own value proves it -- it uses
            // size.x = aspect * 9/16, which is 2.0 on 32:9, to squeeze the asset
            // into the 16:9 window.
            //
            // The 2.35 window therefore wants aspect / 2.35 = 1.513, not its
            // reciprocal. I wrote the reciprocal first and the overlay came back
            // stretched across the width, which is exactly what that mistake
            // looks like.
            // Tied to the side bars, not to the aspect ratio.
            //
            // Framing the overlay at 2.35 is right only because the picture
            // around it is framed at 2.35. With ExpandCutscenesSideways = true
            // there are no side bars, the picture runs to both edges, and an
            // overlay stopping at the film frame would sit in the middle with
            // scene showing past it. Then the full-width path below is the
            // matching one.
            const double aspect = fov::display_aspect();
            if (bars::side_bars() && aspect > framing::kUltrawideThreshold && size != nullptr &&
                pos != nullptr) {
                const double horizontal = aspect / framing::kContentAspect;
                // Cover the frame: match its width and let the height overflow,
                // 16:9 into 2.35 being 1.32 times too tall.
                const double vertical = framing::kContentAspect / framing::kReferenceAspect;

                size[0] = static_cast<float>(size[0] * horizontal);
                pos[0] = static_cast<float>(pos[0] * horizontal + (1.0 - horizontal) * 0.5);
                size[1] = static_cast<float>(size[1] / vertical);
                pos[1] = static_cast<float>(pos[1] / vertical + (1.0 - 1.0 / vertical) * 0.5);
                break;
            }
            // Full width as above, then the vertical axis scaled by k and
            // recentred -- the asset keeps its proportions and loses its top and
            // bottom instead. This is the same formula the game applies to the
            // vertical axis when a display is *narrower* than 16:9, so the shape
            // of it is the game's own, only the condition differs.
            const float s = learn_scale();
            if (s > 1.0f && size != nullptr && pos != nullptr) {
                const float k = 1.0f / s;
                size[1] *= k;
                pos[1] = pos[1] * k + (1.0f - k) * 0.5f;
            }
            break;
        }
    }

    record(ret, size_in, pos_in, size != nullptr ? *size : 0.0f, pos != nullptr ? *pos : 0.0f);
}

}  // namespace

bool init(const std::vector<mem::NamedRegion>& sections) {
    g_site = 0;

    const auto pattern = mem::parse_pattern(patterns::kOverlayFit);
    if (!pattern) {
        logger::info("overlay: malformed signature");
        return false;
    }

    std::vector<std::uintptr_t> hits;
    for (const auto& [name, region] : sections) {
        mem::find_all(region, *pattern, hits);
    }
    if (hits.size() != 1) {
        logger::info("overlay: signature matched {} time(s), need exactly one -- not hooking",
                     hits.size());
        for (const auto hit : hits) {
            logger::info("overlay:   candidate 0x{:016X}", hit);
        }
        return false;
    }

    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }

    const std::uintptr_t site = hits.front();
    void* trampoline = nullptr;
    if (const MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(site),
                                              reinterpret_cast<LPVOID>(&detour), &trampoline);
        status != MH_OK) {
        logger::info("overlay: MH_CreateHook failed: {}", MH_StatusToString(status));
        return false;
    }
    if (const MH_STATUS status = MH_EnableHook(reinterpret_cast<LPVOID>(site)); status != MH_OK) {
        logger::info("overlay: MH_EnableHook failed: {}", MH_StatusToString(status));
        return false;
    }

    g_site = site;
    g_original = reinterpret_cast<Fn>(trampoline);
    logger::info("overlay: 16:9 fit hooked at 0x{:016X}", site);
    return true;
}

bool set_mode(Mode value) {
    if (g_site == 0) {
        return false;
    }
    g_mode.store(value, std::memory_order_relaxed);
    logger::info("overlay: mode is now {}", mode_name());
    return true;
}

Mode mode() { return g_mode.load(std::memory_order_relaxed); }

const char* mode_name() {
    switch (g_mode.load(std::memory_order_relaxed)) {
        case Mode::Fitted:
            return "FITTED (the game's own: 16:9 in the middle, smearing at the sides)";
        case Mode::Stretched:
            return "STRETCHED (full width, 34% wider than authored)";
        case Mode::Cover:
            return "COVER (proportions kept, 25% of the height cropped)";
    }
    return "?";
}

bool found() { return g_site != 0; }

void report(std::uintptr_t module_base) {
    if (g_site == 0) {
        return;
    }
    const unsigned calls = g_calls.load(std::memory_order_relaxed);
    logger::info("overlay: {} call(s), {} distinct caller(s), mode {}", calls,
                 g_caller_count.load(std::memory_order_relaxed), mode_name());

    if (calls == 0) {
        logger::info("overlay:   zero calls -- the hook is on the wrong function, and the");
        logger::info("overlay:   picture could never have told us that.");
        return;
    }
    if (!g_lock_ready) {
        return;
    }
    EnterCriticalSection(&g_lock);
    const int used = g_caller_count.load(std::memory_order_relaxed);
    for (int i = 0; i < used; ++i) {
        const Caller& c = g_callers[i];
        logger::info("overlay:   from 0x{:016X} (module +0x{:X})  x{}", c.ret, c.ret - module_base,
                     c.count);
        logger::info("overlay:       size {:.6f} -> {:.6f}   pos {:.6f} -> {:.6f}", c.size_in,
                     c.size_out, c.pos_in, c.pos_out);
    }
    LeaveCriticalSection(&g_lock);
}

void restore() {
    if (g_site != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_site));
        g_site = 0;
    }
}

}  // namespace overlay
