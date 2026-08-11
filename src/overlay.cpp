#include "overlay.h"

#include <windows.h>

#include <MinHook.h>
#include <intrin.h>

#include <atomic>
#include <cstring>

#include "log.h"
#include "patterns.h"

namespace overlay {
namespace {

// void FitTo16by9(float* size, float* pos) -- both two-component.
using Fn = void(__fastcall*)(float*, float*);

Fn g_original = nullptr;
std::uintptr_t g_site = 0;
std::atomic<bool> g_stretched{false};
std::atomic<unsigned> g_calls{0};

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
    const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());

    const float size_in = size != nullptr ? *size : 0.0f;
    const float pos_in = pos != nullptr ? *pos : 0.0f;

    if (!g_stretched.load(std::memory_order_relaxed)) {
        g_original(size, pos);
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

bool set_stretched(bool value) {
    if (g_site == 0) {
        return false;
    }
    g_stretched.store(value, std::memory_order_relaxed);
    logger::info("overlay: full-screen overlays are now {}",
                 value ? "STRETCHED to the whole screen" : "fitted to 16:9 (original)");
    return true;
}

bool stretched() { return g_stretched.load(std::memory_order_relaxed); }

bool found() { return g_site != 0; }

void report(std::uintptr_t module_base) {
    if (g_site == 0) {
        return;
    }
    const unsigned calls = g_calls.load(std::memory_order_relaxed);
    logger::info("overlay: {} call(s), {} distinct caller(s), currently {}", calls,
                 g_caller_count.load(std::memory_order_relaxed),
                 g_stretched.load(std::memory_order_relaxed) ? "stretched" : "fitted");

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
