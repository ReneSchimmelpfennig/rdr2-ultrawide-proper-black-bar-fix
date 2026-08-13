#include "bars.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>

#include "framing.h"
#include "log.h"
#include "patterns.h"

namespace bars {
namespace {

// Offset of the immediate inside `C6 05 <disp32> <imm8>`.
constexpr std::size_t kImmediateOffset = 6;

std::uintptr_t g_immediate = 0;
std::uint8_t g_original = 0xFF;
bool g_hidden = false;

bool write_byte(std::uintptr_t address, std::uint8_t value) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(address), 1, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    *reinterpret_cast<volatile std::uint8_t*>(address) = value;
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(address), 1, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), 1);
    return true;
}

}  // namespace

bool init(std::uintptr_t anchor_store) {
    if (anchor_store == 0) {
        return false;
    }
    g_immediate = anchor_store + kImmediateOffset;
    g_original = *reinterpret_cast<const std::uint8_t*>(g_immediate);

    if (g_original != 0xFF) {
        logger::info("bars: immediate at the anchor store is 0x{:02X}, expected 0xFF -- not touching it",
                     g_original);
        g_immediate = 0;
        return false;
    }
    logger::info("bars: patch site ready (immediate at the anchor store)");
    return true;
}

bool set_hidden(bool hidden) {
    if (g_immediate == 0) {
        return false;
    }
    if (hidden == g_hidden) {
        return true;
    }
    if (!write_byte(g_immediate, hidden ? 0x00 : g_original)) {
        logger::info("bars: could not write the patch");
        return false;
    }
    g_hidden = hidden;
    logger::info("bars: {}", hidden ? "hidden" : "restored");
    return true;
}

bool hidden() { return g_hidden; }

void verify() {
    if (g_immediate == 0) {
        return;
    }
    static std::uint8_t last_reported = 0xEE;
    const auto now = *reinterpret_cast<const volatile std::uint8_t*>(g_immediate);
    const std::uint8_t expected = g_hidden ? 0x00 : g_original;
    if (now == expected || now == last_reported) {
        return;
    }
    last_reported = now;
    logger::info("bars: the patched byte reads 0x{:02X}, expected 0x{:02X} -- something rewrote it",
                 now, expected);
}

namespace {

using DrawFn = void (*)();
DrawFn g_draw_original = nullptr;
void* g_draw_target = nullptr;
std::uintptr_t g_anchor = 0;
std::atomic<bool> g_side_bars{false};
std::atomic<int> g_side_logged{0};

float read_float_at(std::uintptr_t address) {
    float value = 0.0f;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

void write_float_at(std::uintptr_t address, float value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}

// Replace the bar heights immediately before they are used.
//
// Writing them anywhere else does not survive: the game recomputes them every
// frame and copies them into this second buffer, so the only moment at which the
// values are both final and still ours is the entry to the drawing itself.
void draw_detour() {
    if (g_side_bars.load(std::memory_order_relaxed) && g_anchor != 0) {
        const std::uintptr_t weight_addr = g_anchor + patterns::letterbox::kWeight;
        const std::uintptr_t bar235 = g_anchor + patterns::kDrawnBar235;
        const std::uintptr_t bar_display = g_anchor + patterns::kDrawnBarDisplay;

        const float weight = read_float_at(weight_addr);
        if (weight > 0.0f) {
            // The game's own display bar is (1 - (16/9)/aspect)/2 * weight, so
            // the aspect it computed comes straight back out of it -- no second
            // measurement, and correct in windowed mode.
            const float theirs = read_float_at(bar_display);
            const double k = framing::correction_factor_from_bars(theirs, weight);
            const double aspect = framing::aspect_from_correction(k);

            const double side = (1.0 - framing::kContentAspect / aspect) * 0.5;
            const float ours = static_cast<float>(side * weight);

            // Vertical bars off: at this point the picture is already 2.35:1 and
            // fills the height. Horizontal bars carry the difference.
            write_float_at(bar235, 0.0f);
            write_float_at(bar_display, ours);

            if (g_side_logged.fetch_add(1, std::memory_order_relaxed) < 3) {
                logger::info("side bars: aspect {:.4f}, bar {:.6f} -> {:.6f} (weight {:.4f})",
                             aspect, theirs, ours, weight);
            }
        }
    }
    g_draw_original();
}

}  // namespace

bool init_side_bars(const std::vector<mem::NamedRegion>& sections, std::uintptr_t anchor) {
    if (anchor == 0) {
        return false;
    }
    const auto pattern = mem::parse_pattern(patterns::kDrawLetterbox);
    if (!pattern) {
        logger::info("side bars: malformed signature");
        return false;
    }

    std::vector<std::uintptr_t> hits;
    for (const auto& [name, region] : sections) {
        mem::find_all(region, *pattern, hits);
    }
    if (hits.size() != 1) {
        logger::info("side bars: the drawing function matched {} time(s), need 1", hits.size());
        return false;
    }

    g_anchor = anchor;
    g_draw_target = reinterpret_cast<void*>(hits.front());
    if (MH_CreateHook(g_draw_target, reinterpret_cast<void*>(&draw_detour),
                      reinterpret_cast<void**>(&g_draw_original)) != MH_OK ||
        MH_EnableHook(g_draw_target) != MH_OK) {
        logger::info("side bars: hooking the drawing failed");
        g_draw_target = nullptr;
        return false;
    }
    logger::info("side bars: drawing hooked at 0x{:016X}", hits.front());
    return true;
}

void set_side_bars(bool on) {
    g_side_bars.store(on, std::memory_order_relaxed);
    logger::info("side bars: {}", on ? "on -- the picture is framed, not extended" : "off");
}

bool side_bars() { return g_side_bars.load(std::memory_order_relaxed); }

void restore() {
    if (g_immediate != 0 && g_hidden) {
        write_byte(g_immediate, g_original);
        g_hidden = false;
    }
}

}  // namespace bars
