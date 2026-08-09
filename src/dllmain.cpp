#include <windows.h>

#include <MinHook.h>

#include <cstdint>
#include <filesystem>
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
// Returns true once the letterbox signature resolved to exactly one address.
bool scan_patterns(const mem::Region& module,
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

    const auto letterbox = scan_one("letterbox flag store", patterns::kLetterboxFlagStore);
    scan_one("unknown prologue", patterns::kUnknownPrologue);

    if (letterbox.size() != 1) {
        if (letterbox.size() > 1) {
            logger::info("    ambiguous -- signature needs tightening before it can be trusted");
        }
        return false;
    }

    const std::uintptr_t flag =
        mem::resolve_rip_relative(letterbox.front(), patterns::kLetterboxFlagStoreDispOffset,
                                  patterns::kLetterboxFlagStoreLength);
    logger::info("    -> RIP target 0x{:016X} (module +0x{:X}){}", flag, flag - module.base,
                 module.contains(flag) ? "" : "  [OUTSIDE THE MODULE -- suspicious]");
    return true;
}

// RDR2.exe is packed: on disk the signatures do not exist at all, they only
// appear once Arxan has decrypted the code. So a single scan at load time is
// not enough -- retry until the code shows up.
void scan_until_found(const mem::Region& module) {
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
        if (scan_patterns(module, sections, verbose)) {
            logger::info("signatures resolved on attempt {} ({} s after load)", attempt,
                         attempt * kDelayMs / 1000);
            return;
        }
        Sleep(kDelayMs);
    }

    logger::info("giving up after {} attempts -- signatures never appeared", kMaxAttempts);
}

DWORD WINAPI worker(LPVOID) {
    logger::open(g_self);

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

    scan_until_found(module);

    logger::info("skeleton done -- no hooks installed yet");
    logger::close();
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
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
