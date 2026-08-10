#include "hunt.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <windows.h>

#include "log.h"
#include "patterns.h"

namespace hunt {
namespace {

// RAGE takes FOV in degrees at the script level (SET_CAM_FOV), so degrees is
// the likely storage too. The radian window is kept as a fallback signal: if
// the degree pass finds nothing, the log still says whether the radian range
// held anything worth widening to.
constexpr float kDegLo = 10.0f;
constexpr float kDegHi = 90.0f;
constexpr float kRadLo = 0.15f;
constexpr float kRadHi = 1.70f;

// Below this the change is noise, not a camera cut.
constexpr float kMinChange = 0.01f;

constexpr float kGameplay = 0.001f;  // weight at or below this: no letterbox
constexpr float kFullBars = 0.999f;  // weight at or above this: bars fully in

constexpr std::size_t kMaxReported = 40;

bool plausible(float value, float lo, float hi) {
    return std::isfinite(value) && value >= lo && value <= hi;
}

float read_float(std::uintptr_t addr) {
    float value = 0.0f;
    std::memcpy(&value, reinterpret_cast<const void*>(addr), sizeof(value));
    return value;
}

bool readable(std::uintptr_t addr, std::size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const auto block_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return addr + size <= block_end;
}

// Blocks until the weight satisfies `wanted`, or the deadline passes.
bool wait_for(std::uintptr_t weight_addr, bool (*wanted)(float), DWORD deadline) {
    while (GetTickCount() < deadline) {
        if (readable(weight_addr, sizeof(float)) && wanted(read_float(weight_addr))) {
            return true;
        }
        Sleep(16);
    }
    return false;
}

bool is_gameplay(float w) { return w <= kGameplay; }
bool is_full_bars(float w) { return w >= kFullBars; }

void report(const char* title, const std::vector<Candidate>& candidates,
            std::uintptr_t module_base) {
    logger::info("");
    logger::info("{}: {} candidate(s)", title, candidates.size());

    const std::size_t shown = (std::min)(candidates.size(), kMaxReported);
    for (std::size_t i = 0; i < shown; ++i) {
        const Candidate& c = candidates[i];
        logger::info("    module +0x{:<9X}  gameplay {:8.3f}   cutscene {:8.3f}   +1 frame {:8.3f}",
                     c.address - module_base, c.gameplay, c.cutscene, c.later);
    }
    if (candidates.size() > shown) {
        logger::info("    ... and {} more", candidates.size() - shown);
    }
}

}  // namespace

std::vector<Candidate> collect(const mem::Region& region, float lo, float hi) {
    std::vector<Candidate> candidates;

    for (const mem::Region& sub : mem::readable_subranges(region)) {
        // Floats are 4-byte aligned; start at the first aligned address inside.
        std::uintptr_t addr = (sub.base + 3) & ~static_cast<std::uintptr_t>(3);
        for (; addr + sizeof(float) <= sub.end(); addr += 4) {
            const float value = read_float(addr);
            if (plausible(value, lo, hi)) {
                candidates.push_back({addr, value, 0.0f, 0.0f});
            }
        }
    }
    return candidates;
}

void keep_changed(std::vector<Candidate>& candidates, float lo, float hi, float min_change) {
    const auto dead = std::remove_if(candidates.begin(), candidates.end(), [&](Candidate& c) {
        if (!readable(c.address, sizeof(float))) {
            return true;
        }
        c.cutscene = read_float(c.address);
        if (!plausible(c.cutscene, lo, hi)) {
            return true;
        }
        return std::fabs(c.cutscene - c.gameplay) < min_change;
    });
    candidates.erase(dead, candidates.end());
}

void resample(std::vector<Candidate>& candidates) {
    for (Candidate& c : candidates) {
        if (readable(c.address, sizeof(float))) {
            c.later = read_float(c.address);
        }
    }
}

void run(const mem::Region& search_area, std::uintptr_t weight_addr, std::uintptr_t module_base,
         unsigned int timeout_ms) {
    const DWORD deadline = GetTickCount() + timeout_ms;

    logger::info("");
    logger::info("=== FOV hunt ===");
    logger::info("search area 0x{:016X} .. 0x{:016X} ({:.1f} MB)", search_area.base,
                 search_area.end(), static_cast<double>(search_area.size) / (1024.0 * 1024.0));
    logger::info("stay in gameplay for a moment, then go into a cutscene and stay there.");

    if (!wait_for(weight_addr, is_gameplay, deadline)) {
        logger::info("timed out waiting for gameplay -- hunt aborted");
        return;
    }
    // Let the frame settle so we sample a steady state, not a transition.
    Sleep(1000);

    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    std::vector<Candidate> degrees = collect(search_area, kDegLo, kDegHi);
    std::vector<Candidate> radians = collect(search_area, kRadLo, kRadHi);

    QueryPerformanceCounter(&t1);
    logger::info("pass 1 (gameplay): {} float(s) in [{}..{}], {} in [{}..{}] -- {:.1f} s",
                 degrees.size(), kDegLo, kDegHi, radians.size(), kRadLo, kRadHi,
                 static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart));

    logger::info("now go into a cutscene and let the bars come fully in.");
    if (!wait_for(weight_addr, is_full_bars, deadline)) {
        logger::info("timed out waiting for a cutscene -- hunt aborted");
        return;
    }
    // The bars finish before the camera settles; give it a moment.
    Sleep(500);

    keep_changed(degrees, kDegLo, kDegHi, kMinChange);
    keep_changed(radians, kRadLo, kRadHi, kMinChange);
    logger::info("pass 2 (cutscene): {} degree candidate(s), {} radian candidate(s) survived",
                 degrees.size(), radians.size());

    // One frame later. What the camera writes every frame moves again; what is
    // configuration stays put. The design notes say cutscene cameras rewrite
    // FOV every frame, so the per-frame group is the interesting one.
    Sleep(50);
    resample(degrees);
    resample(radians);

    const auto split = [](std::vector<Candidate>& all) {
        std::vector<Candidate> per_frame;
        std::vector<Candidate> stable;
        for (const Candidate& c : all) {
            if (std::fabs(c.later - c.cutscene) > 1e-6f) {
                per_frame.push_back(c);
            } else {
                stable.push_back(c);
            }
        }
        return std::pair{per_frame, stable};
    };

    auto [deg_per_frame, deg_stable] = split(degrees);
    auto [rad_per_frame, rad_stable] = split(radians);

    report("DEGREES, rewritten every frame (most likely the camera)", deg_per_frame, module_base);
    report("DEGREES, stable during the cutscene", deg_stable, module_base);
    report("RADIANS, rewritten every frame", rad_per_frame, module_base);
    logger::info("");
    logger::info("radians stable: {} candidate(s), not listed", rad_stable.size());
    logger::info("=== FOV hunt done ===");
}

void run_hotkey(const mem::Region& search_area, std::uintptr_t module_base,
                unsigned int window_ms) {
    logger::info("");
    logger::info("=== FOV hunt, hotkey triggered ===");
    logger::info("search area 0x{:016X} .. 0x{:016X} ({:.1f} MB)", search_area.base,
                 search_area.end(), static_cast<double>(search_area.size) / (1024.0 * 1024.0));
    logger::info("Get into gameplay and hold still. Baseline is taken in 15 seconds.");
    Sleep(15000);

    std::vector<Candidate> degrees = collect(search_area, kDegLo, kDegHi);
    std::vector<Candidate> radians = collect(search_area, kRadLo, kRadHi);
    logger::info("baseline: {} float(s) in [{}..{}], {} in [{}..{}]", degrees.size(), kDegLo,
                 kDegHi, radians.size(), kRadLo, kRadHi);

    logger::info("");
    logger::info(">>> NOW press the other mod's FOV hotkey, and change it a LOT. <<<");
    logger::info(">>> You have {} seconds. Do not move the camera more than you must. <<<",
                 window_ms / 1000);
    Sleep(window_ms);

    keep_changed(degrees, kDegLo, kDegHi, kMinChange);
    keep_changed(radians, kRadLo, kRadHi, kMinChange);
    logger::info("after the hotkey: {} degree candidate(s), {} radian candidate(s) changed",
                 degrees.size(), radians.size());

    Sleep(50);
    resample(degrees);
    resample(radians);

    report("DEGREES changed by the hotkey", degrees, module_base);
    report("RADIANS changed by the hotkey", radians, module_base);
    logger::info("=== hotkey hunt done ===");
    logger::info("Anything listed here is written by a mod that visibly works,");
    logger::info("so it reaches the projection -- unlike the getter and the master global.");
}

void find_known_values(const mem::Region& search_area, std::uintptr_t module_base,
                       std::uintptr_t weight_addr, unsigned int timeout_ms) {
    struct Needle {
        const char* what;
        float as_float;
        std::int32_t as_int;
        bool search_int;
    };

    // Everything the 2D layer would need to box itself into the cutscene window.
    const Needle needles[] = {
        {"visible width  2560", 2560.0f, 2560, true},
        {"visible height 1090", 1090.0f, 1090, true},
        {"side bar       440", 440.0f, 440, true},
        {"top bar        175", 175.0f, 175, true},
        {"k              0.744186", 0.744186f, 0, false},
        {"bar frac       0.127907", 0.127907f, 0, false},
        {"bar frac       0.121749", 0.121749f, 0, false},
        {"1/k            1.343750", 1.34375f, 0, false},
        {"height ratio   0.756944", 0.756944f, 0, false},
    };

    logger::info("");
    logger::info("=== searching for the known 2D geometry ===");
    logger::info("area 0x{:016X} .. 0x{:016X} ({:.1f} MB)", search_area.base, search_area.end(),
                 static_cast<double>(search_area.size) / (1024.0 * 1024.0));
    logger::info("get into a cutscene and stay there; the scan starts once the bars are fully in.");

    const DWORD deadline = GetTickCount() + timeout_ms;
    if (!wait_for(weight_addr, is_full_bars, deadline)) {
        logger::info("timed out waiting for a cutscene -- aborted");
        return;
    }
    Sleep(500);

    for (const Needle& needle : needles) {
        std::vector<std::uintptr_t> as_float;
        std::vector<std::uintptr_t> as_int;

        for (const mem::Region& sub : mem::readable_subranges(search_area)) {
            std::uintptr_t addr = (sub.base + 3) & ~static_cast<std::uintptr_t>(3);
            for (; addr + 4 <= sub.end(); addr += 4) {
                float f = 0.0f;
                std::memcpy(&f, reinterpret_cast<const void*>(addr), sizeof(f));
                if (f == needle.as_float) {
                    as_float.push_back(addr);
                }
                if (needle.search_int) {
                    std::int32_t i = 0;
                    std::memcpy(&i, reinterpret_cast<const void*>(addr), sizeof(i));
                    if (i == needle.as_int) {
                        as_int.push_back(addr);
                    }
                }
            }
        }

        logger::info("{}: {} as float, {} as int32", needle.what, as_float.size(), as_int.size());

        // Only the rare ones are worth listing, and worth dumping. A value with
        // hundreds of hits is a common constant and says nothing.
        //
        // The neighbourhood is what actually cracks these: the letterbox struct
        // gave up its meaning the moment the fields around the anchor were read
        // side by side. A rect is unlikely to be alone -- expect the other
        // dimension, an origin, maybe a scale, within a few words.
        const auto list = [&](const char* kind, const std::vector<std::uintptr_t>& hits,
                              bool dump_neighbours) {
            if (hits.empty() || hits.size() > 12) {
                return;
            }
            for (const auto hit : hits) {
                logger::info("    {} module +0x{:X}", kind, hit - module_base);
                if (!dump_neighbours) {
                    continue;
                }
                constexpr std::ptrdiff_t kBefore = 8 * 4;
                constexpr std::ptrdiff_t kAfter = 8 * 4;
                const std::uintptr_t from = hit - kBefore;
                if (!readable(from, kBefore + kAfter + 4)) {
                    continue;
                }
                for (std::ptrdiff_t off = -kBefore; off <= kAfter; off += 4) {
                    std::int32_t as_i = 0;
                    float as_f = 0.0f;
                    std::memcpy(&as_i, reinterpret_cast<const void*>(hit + off), sizeof(as_i));
                    std::memcpy(&as_f, reinterpret_cast<const void*>(hit + off), sizeof(as_f));
                    logger::info("        {:+4}  int {:<12}  float {:<14.5f}{}", off, as_i, as_f,
                                 off == 0 ? "   <- the value" : "");
                }
            }
        };
        // Dump around the rare ones only; 1090 had four hits, 2560 had ten.
        list("float", as_float, as_float.size() <= 4);
        list("int32", as_int, as_int.size() <= 4);
    }

    logger::info("=== search done ===");
    logger::info("A value appearing only a handful of times is the interesting one.");
}

void watch(std::uintptr_t module_base, std::uintptr_t weight_addr, unsigned int duration_ms) {
    struct Slot {
        const char* name;
        std::uintptr_t address;
        float value;
    };

    Slot slots[] = {
        {"degA", module_base + patterns::candidates::kDegreeCopyA, 0.0f},
        {"degB", module_base + patterns::candidates::kDegreeCopyB, 0.0f},
        {"getter", module_base + patterns::candidates::kGetterSource, 0.0f},
        {"scaleX", module_base + patterns::candidates::kScaleX, 0.0f},
        {"scaleY", module_base + patterns::candidates::kScaleY, 0.0f},
    };

    logger::info("");
    logger::info("=== watching the FOV candidates ===");
    for (const Slot& s : slots) {
        if (!readable(s.address, sizeof(float))) {
            logger::info("{} at module +0x{:X} is not readable -- aborting", s.name,
                         s.address - module_base);
            return;
        }
        logger::info("    {:<7} module +0x{:<9X} = {:.4f}", s.name, s.address - module_base,
                     read_float(s.address));
    }

    // The offsets are raw, build-specific and will be silently wrong on another
    // version of the game. Say so loudly rather than logging nonsense.
    const float sanity = read_float(slots[0].address);
    if (std::fabs(sanity - patterns::candidates::kExpectedGameplayValue) > 0.5f) {
        logger::info("");
        logger::info("WARNING: degA reads {:.4f}, expected about {:.1f}.", sanity,
                     patterns::candidates::kExpectedGameplayValue);
        logger::info("         Either this is not RDR2 1.0.1491.50, or you are not in gameplay.");
        logger::info("         Watching anyway, but treat the numbers with suspicion.");
    }

    logger::info("");
    logger::info("Open the graphics settings and move the FIELD OF VIEW slider.");
    logger::info("If degA follows the slider, the question is answered.");
    logger::info("");

    constexpr DWORD kPollMs = 16;
    constexpr DWORD kMinIntervalMs = 100;  // keeps a cutscene from flooding the log
    constexpr int kMaxLines = 2000;
    constexpr float kEpsilon = 1e-4f;

    int lines = 0;
    bool first = true;
    DWORD last_line = 0;
    const DWORD started = GetTickCount();

    while (GetTickCount() - started < duration_ms) {
        bool changed = first;
        for (Slot& s : slots) {
            if (!readable(s.address, sizeof(float))) {
                continue;
            }
            const float now = read_float(s.address);
            if (std::fabs(now - s.value) > kEpsilon) {
                changed = true;
            }
            s.value = now;
        }

        const DWORD now_ms = GetTickCount();
        if (changed && (first || now_ms - last_line >= kMinIntervalMs)) {
            if (lines >= kMaxLines) {
                logger::info("line budget exhausted -- stopping the watch");
                return;
            }
            const float weight =
                readable(weight_addr, sizeof(float)) ? read_float(weight_addr) : -1.0f;
            logger::info(
                "weight {:6.4f}   degA {:9.4f}   degB {:9.4f}   getter {:9.4f}   "
                "scaleX {:7.4f}   scaleY {:7.4f}",
                weight, slots[0].value, slots[1].value, slots[2].value, slots[3].value,
                slots[4].value);
            ++lines;
            last_line = now_ms;
            first = false;
        }

        Sleep(kPollMs);
    }

    logger::info("watch finished, {} line(s)", lines);
}

}  // namespace hunt
