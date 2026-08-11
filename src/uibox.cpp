#include "uibox.h"

#include <windows.h>

#include <MinHook.h>
#include <intrin.h>

#include <atomic>
#include <cstring>

#include "log.h"
#include "patterns.h"

namespace uibox {
namespace {

// void ApplyUiBox(float* pos, float* size, bool useWindow)
using Fn = void(__fastcall*)(float*, float*, unsigned char);

Fn g_original = nullptr;
std::uintptr_t g_site = 0;
std::atomic<bool> g_disabled{false};
std::atomic<unsigned> g_calls{0};

// Who calls it, and with what. A byte patch could only ever answer "did the
// picture move"; three attempts have now shown that a picture which does not
// move is the least informative outcome there is. This answers the prior
// question -- whether the code runs at all -- and hands over the callers, which
// is what Plan B was going to cost a whole session to obtain.
constexpr int kMaxCallers = 24;

struct Caller {
    std::uintptr_t ret = 0;
    unsigned count = 0;
    float pos_in = 0.0f;
    float size_in = 0.0f;
    float pos_out = 0.0f;
    float size_out = 0.0f;
    bool had_pos = false;
    bool had_size = false;
};

Caller g_callers[kMaxCallers]{};
std::atomic<int> g_caller_count{0};
CRITICAL_SECTION g_lock{};
bool g_lock_ready = false;

void record(std::uintptr_t ret, const float* pos_in, const float* size_in, const float* pos_out,
            const float* size_out) {
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
        Caller& slot = g_callers[used];
        slot.ret = ret;
        slot.count = 1;
        slot.had_pos = pos_in != nullptr;
        slot.had_size = size_in != nullptr;
        if (pos_in != nullptr) {
            slot.pos_in = *pos_in;
            slot.pos_out = *pos_out;
        }
        if (size_in != nullptr) {
            slot.size_in = *size_in;
            slot.size_out = *size_out;
        }
        g_caller_count.store(used + 1, std::memory_order_relaxed);
    }
    LeaveCriticalSection(&g_lock);
}

void __fastcall detour(float* pos, float* size, unsigned char flag) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());

    const float pos_in = pos != nullptr ? *pos : 0.0f;
    const float size_in = size != nullptr ? *size : 0.0f;

    if (!g_disabled.load(std::memory_order_relaxed)) {
        g_original(pos, size, flag);
    }

    const float pos_out = pos != nullptr ? *pos : 0.0f;
    const float size_out = size != nullptr ? *size : 0.0f;
    record(ret, pos != nullptr ? &pos_in : nullptr, size != nullptr ? &size_in : nullptr, &pos_out,
           &size_out);
}

// The sibling. Counted only -- never disabled -- because the question it
// answers is whether the family runs at all, and a second behavioural change
// would only muddy that.
using AlignFn = std::uint64_t(__fastcall*)(int, float*, float*, float*);

AlignFn g_align_original = nullptr;
std::uintptr_t g_align_site = 0;
std::atomic<unsigned> g_align_calls{0};

std::uint64_t __fastcall align_detour(int mode, float* pos, float* size, float* rect) {
    g_align_calls.fetch_add(1, std::memory_order_relaxed);
    return g_align_original(mode, pos, size, rect);
}

// Hooks `signature`, storing the trampoline in `original`. Returns the address
// or 0, and says in the log which of the two failure modes happened -- a
// signature that did not resolve is a different problem from a hook that did
// not take.
std::uintptr_t hook_one(const std::vector<mem::NamedRegion>& sections, std::string_view signature,
                        const char* label, void* detour_fn, void** original) {
    const auto pattern = mem::parse_pattern(signature);
    if (!pattern) {
        logger::info("uibox: malformed signature for {}", label);
        return 0;
    }

    std::vector<std::uintptr_t> hits;
    for (const auto& [name, region] : sections) {
        mem::find_all(region, *pattern, hits);
    }
    if (hits.size() != 1) {
        logger::info("uibox: {} matched {} time(s), need exactly one -- not hooking", label,
                     hits.size());
        return 0;
    }

    const std::uintptr_t site = hits.front();
    if (const MH_STATUS status =
            MH_CreateHook(reinterpret_cast<LPVOID>(site), detour_fn, original);
        status != MH_OK) {
        logger::info("uibox: MH_CreateHook({}) failed: {}", label, MH_StatusToString(status));
        return 0;
    }
    if (const MH_STATUS status = MH_EnableHook(reinterpret_cast<LPVOID>(site)); status != MH_OK) {
        logger::info("uibox: MH_EnableHook({}) failed: {}", label, MH_StatusToString(status));
        return 0;
    }
    logger::info("uibox: {} hooked at 0x{:016X}", label, site);
    return site;
}

}  // namespace

bool init(const std::vector<mem::NamedRegion>& sections) {
    g_site = 0;

    const auto pattern = mem::parse_pattern(patterns::kUiBoxTransform);
    if (!pattern) {
        logger::info("uibox: malformed signature");
        return false;
    }

    std::vector<std::uintptr_t> hits;
    for (const auto& [name, region] : sections) {
        mem::find_all(region, *pattern, hits);
    }

    if (hits.size() != 1) {
        logger::info("uibox: signature matched {} time(s), need exactly one -- not hooking",
                     hits.size());
        for (const auto hit : hits) {
            logger::info("uibox:   candidate 0x{:016X}", hit);
        }
        return false;
    }

    g_site = hits.front();

    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }

    void* trampoline = nullptr;
    if (const MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(g_site),
                                              reinterpret_cast<LPVOID>(&detour), &trampoline);
        status != MH_OK) {
        logger::info("uibox: MH_CreateHook failed: {}", MH_StatusToString(status));
        g_site = 0;
        return false;
    }
    if (const MH_STATUS status = MH_EnableHook(reinterpret_cast<LPVOID>(g_site)); status != MH_OK) {
        logger::info("uibox: MH_EnableHook failed: {}", MH_StatusToString(status));
        g_site = 0;
        return false;
    }
    g_original = reinterpret_cast<Fn>(trampoline);
    logger::info("uibox: 16:9 box transform hooked at 0x{:016X}", g_site);

    void* align_trampoline = nullptr;
    g_align_site = hook_one(sections, patterns::kUiAlignVariant, "align variant",
                            reinterpret_cast<LPVOID>(&align_detour), &align_trampoline);
    g_align_original = reinterpret_cast<AlignFn>(align_trampoline);

    return true;
}

bool set_disabled(bool disable) {
    if (g_site == 0) {
        return false;
    }
    g_disabled.store(disable, std::memory_order_relaxed);
    logger::info("uibox: 16:9 boxing of the UI is now {}", disable ? "OFF" : "ON");
    return true;
}

bool disabled() { return g_disabled.load(std::memory_order_relaxed); }

bool found() { return g_site != 0; }

void report(std::uintptr_t module_base) {
    if (g_site == 0) {
        logger::info("uibox: never hooked -- nothing to report");
        return;
    }

    const unsigned calls = g_calls.load(std::memory_order_relaxed);
    const unsigned align = g_align_calls.load(std::memory_order_relaxed);
    logger::info("uibox: box transform {} call(s), {} distinct caller(s); align variant {} call(s)",
                 calls, g_caller_count.load(std::memory_order_relaxed), align);

    if (calls == 0 && align == 0) {
        logger::info("uibox:   ZERO calls on both. Neither ever executes, so patching them could");
        logger::info("uibox:   not have changed anything. A dead path, not a failed idea -- and");
        logger::info("uibox:   the whole ultrawide-UI family goes with it.");
        return;
    }
    if (calls == 0) {
        logger::info("uibox:   the box transform itself never runs; only the align variant does");
        return;
    }

    if (!g_lock_ready) {
        return;
    }
    EnterCriticalSection(&g_lock);
    const int used = g_caller_count.load(std::memory_order_relaxed);
    for (int i = 0; i < used; ++i) {
        const Caller& c = g_callers[i];
        logger::info("uibox:   from 0x{:016X} (module +0x{:X})  x{}", c.ret, c.ret - module_base,
                     c.count);
        if (c.had_pos) {
            logger::info("uibox:       pos  {:.6f} -> {:.6f}", c.pos_in, c.pos_out);
        }
        if (c.had_size) {
            logger::info("uibox:       size {:.6f} -> {:.6f}", c.size_in, c.size_out);
        }
        if (!c.had_pos && !c.had_size) {
            logger::info("uibox:       both pointers null -- this call does nothing either way");
        }
    }
    LeaveCriticalSection(&g_lock);
}

void restore() {
    if (g_site != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_site));
        g_site = 0;
    }
}

}  // namespace uibox
