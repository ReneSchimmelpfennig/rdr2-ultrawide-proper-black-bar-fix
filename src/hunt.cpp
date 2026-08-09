#include "hunt.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <windows.h>

#include "log.h"

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

}  // namespace hunt
