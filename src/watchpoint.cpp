#include "watchpoint.h"

#include <windows.h>

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "log.h"

namespace watchpoint {
namespace {

// DR7: enable DR0 locally, break on writes only, four byte length.
//   bit 0        L0
//   bits 16..17  RW0 = 01 (write)
//   bits 18..19  LEN0 = 11 (4 bytes)
constexpr DWORD64 kDr7Enable = 0x1 | (0x1ull << 16) | (0x3ull << 18);

// Hits are recorded without allocating or locking: the handler runs on the
// game's own threads, inside an exception, and must not take the logger mutex
// or touch the CRT heap. Everything is reported afterwards.
constexpr std::size_t kMaxHits = 4096;
std::atomic<std::size_t> g_hit_count{0};
std::uintptr_t g_hits[kMaxHits]{};

std::uintptr_t g_watched = 0;
void* g_handler = nullptr;

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Bit 0 of DR6 means our DR0 watchpoint tripped, as opposed to an ordinary
    // single step from some other debugger or from the game itself.
    if ((info->ContextRecord->Dr6 & 0x1) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const std::size_t slot = g_hit_count.fetch_add(1, std::memory_order_relaxed);
    if (slot < kMaxHits) {
        g_hits[slot] = static_cast<std::uintptr_t>(info->ContextRecord->Rip);
    }

    info->ContextRecord->Dr6 = 0;
    // Resume flag: do not trap again on the instruction we are returning to.
    info->ContextRecord->EFlags |= 0x10000;
    return EXCEPTION_CONTINUE_EXECUTION;
}

std::vector<DWORD> other_thread_ids() {
    std::vector<DWORD> ids;

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return ids;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self_process = GetCurrentProcessId();
    const DWORD self_thread = GetCurrentThreadId();

    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == self_process && entry.th32ThreadID != self_thread) {
                ids.push_back(entry.th32ThreadID);
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return ids;
}

// Returns how many threads accepted the change.
int apply_to_all_threads(std::uintptr_t address, bool enable) {
    int applied = 0;

    for (const DWORD id : other_thread_ids()) {
        const HANDLE thread =
            OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, id);
        if (thread == nullptr) {
            continue;
        }

        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                if (enable) {
                    context.Dr0 = address;
                    context.Dr7 = (context.Dr7 & ~0xFull) | kDr7Enable;
                } else {
                    context.Dr0 = 0;
                    context.Dr7 &= ~kDr7Enable;
                }
                context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (SetThreadContext(thread, &context)) {
                    ++applied;
                }
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    return applied;
}

void report(std::uintptr_t module_base) {
    const std::size_t total = (std::min)(g_hit_count.load(), kMaxHits);
    logger::info("watchpoint: {} hit(s) recorded", g_hit_count.load());
    if (total == 0) {
        logger::info("  nothing tripped it. Either the address is written from a thread we could");
        logger::info("  not touch, or the debug registers were cleared -- see Arxan.");
        return;
    }

    // Group identical instruction pointers and sort by frequency.
    std::vector<std::uintptr_t> sorted(g_hits, g_hits + total);
    std::sort(sorted.begin(), sorted.end());

    struct Entry {
        std::uintptr_t rip;
        std::size_t count;
    };
    std::vector<Entry> grouped;
    for (std::size_t i = 0; i < sorted.size();) {
        std::size_t j = i;
        while (j < sorted.size() && sorted[j] == sorted[i]) {
            ++j;
        }
        grouped.push_back({sorted[i], j - i});
        i = j;
    }
    std::sort(grouped.begin(), grouped.end(),
              [](const Entry& a, const Entry& b) { return a.count > b.count; });

    logger::info("  distinct writer(s): {}", grouped.size());
    for (std::size_t i = 0; i < grouped.size() && i < 12; ++i) {
        const Entry& e = grouped[i];
        // Our own detour writes the same address, from our .asi rather than
        // from RDR2.exe. Label it so it is not mistaken for a second game path.
        const std::uintptr_t offset = e.rip - module_base;
        const bool in_module = offset < 0x8000000;
        if (in_module) {
            logger::info("    module +0x{:<9X}  {} hit(s)   <- store ends here", offset, e.count);
        } else {
            logger::info("    0x{:016X}      {} hit(s)   <- outside RDR2.exe (our own hook?)",
                         e.rip, e.count);
        }
    }
}

}  // namespace

void find_writers(std::uintptr_t address, std::uintptr_t module_base, unsigned int duration_ms) {
    g_watched = address;
    g_hit_count.store(0);

    logger::info("");
    logger::info("=== watchpoint on module +0x{:X} for {} s ===", address - module_base,
                 duration_ms / 1000);

    g_handler = AddVectoredExceptionHandler(1, on_exception);
    if (g_handler == nullptr) {
        logger::info("AddVectoredExceptionHandler failed -- aborting");
        return;
    }

    const int armed = apply_to_all_threads(address, true);
    logger::info("armed on {} thread(s)", armed);
    if (armed == 0) {
        RemoveVectoredExceptionHandler(g_handler);
        g_handler = nullptr;
        logger::info("no thread accepted the watchpoint -- aborting");
        return;
    }

    // Threads created after arming do not inherit the debug registers, so
    // re-arm periodically. Cheap enough at this interval.
    const DWORD started = GetTickCount();
    while (GetTickCount() - started < duration_ms) {
        Sleep(2000);
        apply_to_all_threads(address, true);
    }

    apply_to_all_threads(address, false);
    RemoveVectoredExceptionHandler(g_handler);
    g_handler = nullptr;

    report(module_base);
    logger::info("=== watchpoint done ===");
}

}  // namespace watchpoint
