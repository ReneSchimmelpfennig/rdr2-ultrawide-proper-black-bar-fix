#include <windows.h>

#include <MinHook.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "framing.h"
#include "log.h"
#include "mem.h"
#include "patterns.h"

namespace {

HMODULE g_self = nullptr;

// Refuse to do anything if we somehow got loaded into a different process --
// every signature in patterns.h is specific to RDR2.exe.
bool host_is_rdr2() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return false;
    }
    const auto name = std::filesystem::path(path).filename().wstring();
    return _wcsicmp(name.c_str(), L"RDR2.exe") == 0;
}

void report_environment() {
    logger::info("RDR2 Ultrawide Cutscene Fix {} -- skeleton build", PLUGIN_VERSION);
    logger::info("host RDR2.exe version {}",
              mem::main_module_version().empty() ? "<unknown>" : mem::main_module_version());

    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    const double k = framing::correction_factor(width, height);

    // Primary monitor only for now. The value that actually matters is the
    // backbuffer size, which we can only read once the swapchain exists.
    logger::info("primary display {}x{}, k = {:.5f} (16:9 reference)", width, height, k);
    logger::info("  60.0 deg vFOV -> {:.3f} deg corrected", framing::corrected_vfov_deg(60.0, k));
}

// Scans every signature across every executable section and logs what it found.
// Nothing is hooked or patched yet -- this is the evidence we need before
// touching the FOV.
//
// Returns the address of the letterbox flag, or 0 if the signature did not
// resolve to exactly one hit.
std::uintptr_t scan_patterns(const mem::Region& module,
                             const std::vector<mem::NamedRegion>& sections,
                             bool verbose) {
    const auto scan_one = [&](std::string_view label, std::string_view signature) {
        std::vector<std::uintptr_t> hits;

        const auto pattern = mem::parse_pattern(signature);
        if (!pattern) {
            logger::info("{}: MALFORMED SIGNATURE \"{}\"", label, signature);
            return hits;
        }

        LARGE_INTEGER start{}, stop{}, freq{};
        QueryPerformanceCounter(&start);
        for (const auto& [name, region] : sections) {
            mem::find_all(region, *pattern, hits);
        }
        QueryPerformanceCounter(&stop);
        QueryPerformanceFrequency(&freq);
        const double ms = 1000.0 * static_cast<double>(stop.QuadPart - start.QuadPart) /
                          static_cast<double>(freq.QuadPart);

        if (verbose || !hits.empty()) {
            logger::info("{}: {} hit(s) in {:.1f} ms", label, hits.size(), ms);
            for (const auto hit : hits) {
                logger::info("    0x{:016X}  (module +0x{:X})", hit, hit - module.base);
            }
        }
        return hits;
    };

    const auto letterbox = scan_one("letterbox struct anchor", patterns::kLetterboxStructAnchor);
    scan_one("unknown prologue", patterns::kUnknownPrologue);

    if (letterbox.size() != 1) {
        if (letterbox.size() > 1) {
            logger::info("    ambiguous -- signature needs tightening before it can be trusted");
        }
        return 0;
    }

    const std::uintptr_t flag = mem::resolve_rip_relative(
        letterbox.front(), patterns::kAnchorDispOffset, patterns::kAnchorLength);
    logger::info("    -> RIP target 0x{:016X} (module +0x{:X}){}", flag, flag - module.base,
                 module.contains(flag) ? "" : "  [OUTSIDE THE MODULE -- suspicious]");
    return module.contains(flag) ? flag : 0;
}

// RDR2.exe is packed: on disk the signatures do not exist at all, they only
// appear once Arxan has decrypted the code. So a single scan at load time is
// not enough -- retry until the code shows up.
std::uintptr_t scan_until_found(const mem::Region& module) {
    constexpr int kMaxAttempts = 60;
    constexpr DWORD kDelayMs = 2000;

    const auto sections = mem::executable_sections(module);
    logger::info("{} executable section(s):", sections.size());
    for (const auto& [name, region] : sections) {
        logger::info("    {:<12} 0x{:016X} .. 0x{:016X}  ({:.1f} MB)", name, region.base,
                     region.end(), static_cast<double>(region.size) / (1024.0 * 1024.0));
    }

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        const bool verbose = attempt == 1;
        if (verbose) {
            logger::info("--- scan attempt {} ---", attempt);
        }
        if (const std::uintptr_t flag = scan_patterns(module, sections, verbose); flag != 0) {
            // attempt 1 happens immediately -- the delay is between attempts.
            logger::info("signatures resolved on attempt {} ({:.1f} s after load)", attempt,
                         static_cast<double>((attempt - 1) * kDelayMs) / 1000.0);
            return flag;
        }
        Sleep(kDelayMs);
    }

    logger::info("giving up after {} attempts -- signatures never appeared", kMaxAttempts);
    return 0;
}

bool is_readable(std::uintptr_t addr, std::size_t size) {
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

float read_float(std::uintptr_t addr) {
    float value = 0.0f;
    std::memcpy(&value, reinterpret_cast<const void*>(addr), sizeof(value));
    return value;
}

// Watches the decoded letterbox state, purely by reading -- nothing is hooked
// or written. The layout it assumes is in patterns::letterbox and was measured,
// not guessed; logging it in decoded form means a game patch that moves a field
// shows up as nonsense here instead of silently feeding a wrong FOV.
void monitor_letterbox_state(std::uintptr_t anchor) {
    constexpr DWORD kPollMs = 16;                  // ~ once per frame
    constexpr DWORD kDurationMs = 15 * 60 * 1000;  // stop after 15 minutes
    constexpr int kMaxLines = 1500;                // keep the log finite
    constexpr float kEpsilon = 1e-4f;

    const std::uintptr_t weight_addr = anchor + patterns::letterbox::kWeight;
    const std::uintptr_t bar235_addr = anchor + patterns::letterbox::kBarFraction235;
    const std::uintptr_t bar_display_addr = anchor + patterns::letterbox::kBarFractionDisplay;

    if (!is_readable(weight_addr, patterns::letterbox::kStride)) {
        logger::info("letterbox struct is not readable -- giving up on monitoring");
        return;
    }

    logger::info("");
    logger::info("watching the letterbox struct for {} minutes, logging every change.",
                 kDurationMs / 60000);
    logger::info("go into a cutscene and back out.");

    float previous = -1.0f;
    int lines = 0;

    const DWORD started = GetTickCount();
    while (GetTickCount() - started < kDurationMs) {
        if (!is_readable(weight_addr, patterns::letterbox::kStride)) {
            Sleep(kPollMs);
            continue;
        }

        const float weight = read_float(weight_addr);
        if (std::fabs(weight - previous) > kEpsilon) {
            if (lines >= kMaxLines) {
                logger::info("line budget exhausted -- stopping the watch");
                return;
            }

            const float bar235 = read_float(bar235_addr);
            const float bar_display = read_float(bar_display_addr);
            const double k_from_game = framing::correction_factor_from_bars(bar_display, weight);

            logger::info("weight {:.4f}   bar(2.35) {:.5f}   bar(display) {:.5f}   k {:.5f}",
                         weight, bar235, bar_display, k_from_game);
            ++lines;
            previous = weight;
        }

        Sleep(kPollMs);
    }

    logger::info("watch finished after {} minutes, {} change(s) logged", kDurationMs / 60000,
                 lines);
}

DWORD WINAPI worker(LPVOID) {
    logger::open(g_self);
    logger::info("log file: {}", logger::path().empty()
                                     ? std::string("<none writable, debugger output only>")
                                     : std::filesystem::path(logger::path()).string());

    if (!host_is_rdr2()) {
        logger::info("host process is not RDR2.exe -- doing nothing");
        logger::close();
        FreeLibraryAndExitThread(g_self, 0);
    }

    report_environment();

    if (const MH_STATUS status = MH_Initialize(); status != MH_OK) {
        logger::info("ERROR: MH_Initialize failed: {}", MH_StatusToString(status));
    } else {
        logger::info("MinHook initialised");
    }

    const mem::Region module = mem::main_module();
    if (!module) {
        logger::info("ERROR: could not read the main module headers");
        logger::close();
        return 0;
    }
    logger::info("RDR2.exe base 0x{:016X}, image size {} bytes ({:.1f} MB)", module.base,
                 module.size, static_cast<double>(module.size) / (1024.0 * 1024.0));

    if (const std::uintptr_t anchor = scan_until_found(module); anchor != 0) {
        monitor_letterbox_state(anchor);
    }

    logger::info("skeleton done -- no hooks installed yet");
    logger::close();
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // First thing, before anything can fail: proof of load that survives an
        // unwritable game folder, a missing thread, or a crash further down.
        OutputDebugStringA("[RDR2UltrawideCutsceneFix] DLL_PROCESS_ATTACH\n");

        g_self = module;
        DisableThreadLibraryCalls(module);
        // DllMain runs under the loader lock; everything real happens on our
        // own thread.
        if (HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        MH_Uninitialize();
        logger::close();
    }
    return TRUE;
}
