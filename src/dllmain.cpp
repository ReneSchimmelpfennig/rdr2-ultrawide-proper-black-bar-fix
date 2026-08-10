#include <windows.h>

#include <MinHook.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bars.h"
#include "dump.h"
#include "fov.h"
#include "framing.h"
#include "hunt.h"
#include "log.h"
#include "mem.h"
#include "patterns.h"
#include "watchpoint.h"

namespace {

HMODULE g_self = nullptr;

// Address of the `C6 05 ... FF` instruction itself. The letterbox patch needs
// the instruction, the struct lookup needs what it points at.
std::uintptr_t g_anchor_store = 0;

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
    logger::info("RDR2 Ultrawide Cutscene Fix {}", PLUGIN_VERSION);
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

    g_anchor_store = letterbox.front();
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
// Returns true once a complete cutscene has been observed (weight up and back
// down), false on timeout. The caller uses that to dump the image only after
// the cutscene code paths have actually executed.
bool monitor_letterbox_state(std::uintptr_t anchor, DWORD duration_ms) {
    constexpr DWORD kPollMs = 16;    // ~ once per frame
    constexpr int kMaxLines = 1500;  // keep the log finite
    constexpr float kEpsilon = 1e-4f;
    constexpr float kRisen = 0.5f;
    constexpr float kSettled = 0.01f;

    const std::uintptr_t weight_addr = anchor + patterns::letterbox::kWeight;
    const std::uintptr_t bar235_addr = anchor + patterns::letterbox::kBarFraction235;
    const std::uintptr_t bar_display_addr = anchor + patterns::letterbox::kBarFractionDisplay;

    if (!is_readable(weight_addr, patterns::letterbox::kStride)) {
        logger::info("letterbox struct is not readable -- giving up on monitoring");
        return false;
    }

    logger::info("");
    logger::info("watching the letterbox struct for up to {} minutes.", duration_ms / 60000);
    logger::info("go into a cutscene and back out -- the image is dumped once that happened,");
    logger::info("so that the cutscene code is decrypted and resident when it is.");

    float previous = -1.0f;
    int lines = 0;
    bool risen = false;

    const DWORD started = GetTickCount();
    while (GetTickCount() - started < duration_ms) {
        if (!is_readable(weight_addr, patterns::letterbox::kStride)) {
            Sleep(kPollMs);
            continue;
        }

        const float weight = read_float(weight_addr);

        if (weight > kRisen) {
            risen = true;
        } else if (risen && weight < kSettled) {
            logger::info("a full cutscene was observed -- ending the watch");
            return true;
        }

        if (std::fabs(weight - previous) > kEpsilon) {
            if (lines >= kMaxLines) {
                logger::info("line budget exhausted -- stopping the watch");
                return risen;
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

    logger::info("watch timed out after {} minutes, {} change(s) logged", duration_ms / 60000,
                 lines);
    return risen;
}

// Writes the decrypted image next to the log, once. Skipped if it is already
// there -- it is 115 MB and there is no reason to rewrite it every launch.
// Delete the file to force a fresh dump.
void dump_image_once(const mem::Region& module, bool after_cutscene) {
    std::filesystem::path out = logger::path();
    if (out.empty()) {
        logger::info("no log location -- skipping the image dump");
        return;
    }
    out.replace_filename(L"RDR2.dump.exe");

    std::error_code ec;
    if (std::filesystem::exists(out, ec)) {
        logger::info("image dump already exists, skipping: {}", out.string());
        logger::info("  delete it to force a fresh dump on the next launch");
        return;
    }

    if (!after_cutscene) {
        logger::info("NOTE: no cutscene was observed before dumping. Cutscene code may still");
        logger::info("      be encrypted in this dump. Delete it and replay to get a better one.");
    }

    logger::info("dumping the decrypted image to {} ...", out.string());
    const dump::Result result = dump::write_image(module, out);

    if (!result.ok) {
        logger::info("image dump FAILED after {} bytes", result.bytes_written);
        return;
    }
    logger::info("image dump done: {} bytes in {:.1f} s, {} byte(s) were unreadable and zeroed",
                 result.bytes_written, result.seconds, result.unreadable_bytes);
    logger::info("  file offsets equal RVAs -- '+0x320545' in this log is 0x320545 in the file");
}

// Live controls, so the framing can be judged and dialled in during a single
// cutscene instead of one game launch per guess.
void run_hotkeys(unsigned int duration_ms) {
    logger::info("");
    logger::info("=== hotkeys ===");
    logger::info("  F7   correction on / off");
    logger::info("  F8   letterbox bars on / off");
    logger::info("  F9   strength -0.05      F10  strength +0.05");
    logger::info("  Ctrl+Alt+B   zero the bar heights (test: do the artefacts go away?)");
    logger::info("  F11  force the correction in gameplay (for still comparisons)");
    logger::info("  F12  set strength to exactly 1.00");
    logger::info("current: strength {:.2f}, bars {}", fov::strength(),
                 bars::hidden() ? "hidden" : "visible");

    const auto pressed = [](int vk, bool& was_down) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool edge = down && !was_down;
        was_down = down;
        return edge;
    };
    bool bars_key = false;  // Ctrl+Alt+B; not F6, that is RDR2's photo mode
    bool f7 = false, f8 = false, f9 = false, f10 = false, f11 = false, f12 = false;

    const auto combo_pressed = [](int vk, bool& was_down) {
        const bool held = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
                          (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const bool down = held && (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool edge = down && !was_down;
        was_down = down;
        return edge;
    };
    float saved_strength = fov::strength();

    const DWORD started = GetTickCount();
    while (GetTickCount() - started < duration_ms) {
        if (combo_pressed('B', bars_key)) {
            fov::set_flatten_bars(!fov::flattening_bars());
            logger::info("Ctrl+Alt+B: bar heights zeroed {}",
                         fov::flattening_bars() ? "ON" : "OFF");
        }
        if (pressed(VK_F7, f7)) {
            if (fov::strength() > 0.0f) {
                saved_strength = fov::strength();
                fov::set_strength(0.0f);
            } else {
                fov::set_strength(saved_strength > 0.0f ? saved_strength : 1.0f);
            }
            logger::info("F7: correction {} (strength {:.2f})",
                         fov::strength() > 0.0f ? "ON" : "OFF", fov::strength());
        }
        if (pressed(VK_F8, f8)) {
            bars::set_hidden(!bars::hidden());
        }
        if (pressed(VK_F9, f9)) {
            fov::set_strength(fov::strength() - 0.05f);
            logger::info("F9: strength {:.2f}", fov::strength());
        }
        if (pressed(VK_F10, f10)) {
            fov::set_strength(fov::strength() + 0.05f);
            logger::info("F10: strength {:.2f}", fov::strength());
        }
        if (pressed(VK_F11, f11)) {
            fov::set_force(!fov::forced());
            logger::info("F11: gameplay force {}", fov::forced() ? "ON" : "OFF");
        }
        if (pressed(VK_F12, f12)) {
            fov::set_strength(1.0f);
            logger::info("F12: strength reset to 1.00");
        }
        Sleep(30);
    }
    logger::info("hotkey window closed");
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
        const std::uintptr_t weight = anchor + patterns::letterbox::kWeight;

        // The dump is written once; on later launches this returns immediately
        // and we go straight to hunting.
        std::filesystem::path existing = logger::path();
        existing.replace_filename(L"RDR2.dump.exe");
        std::error_code ec;
        if (!std::filesystem::exists(existing, ec)) {
            constexpr DWORD kWatchMs = 10 * 60 * 1000;
            const bool saw_cutscene = monitor_letterbox_state(anchor, kWatchMs);
            dump_image_once(module, saw_cutscene);
        } else {
            logger::info("image dump already present, skipping straight to the hunt");
        }

        // The actual fix. Installed before the watch so the watch shows the
        // hooked values -- degA is written from the getter's return value, so
        // it doubles as an in-log confirmation that the detour is live.
        const fov::Config fov_config = fov::read_config();
        const bool fov_ready =
            fov::install(mem::executable_sections(module), module, anchor, fov_config);

        if (fov_ready && fov_config.mode == fov::Mode::Poke) {
            constexpr unsigned int kPokeMs = 60 * 1000;
            fov::run_poke(kPokeMs);
        } else if (fov_ready && fov_config.mode == fov::Mode::Corrected) {
            bars::init(g_anchor_store);
            bars::set_hidden(true);  // the framing cannot be judged with them on
            constexpr unsigned int kHotkeyMs = 60 * 60 * 1000;
            run_hotkeys(kHotkeyMs);
        } else if (fov_ready && (fov_config.mode == fov::Mode::Watch ||
                                 fov_config.mode == fov::Mode::TestWatch)) {
            // 90 s, because this pass needs the player to actively make the
            // game change the FOV -- aim, use the binoculars, ride. The first
            // watchpoint run only saw a steady camera and therefore only found
            // the steady-state writer.
            constexpr unsigned int kWatchMs = 90 * 1000;
            // Reads as well as writes this time. Our correction lands after
            // every single write and the picture still reverts, so the question
            // is no longer who overwrites us -- it is who actually consumes
            // this value, and whether the projection is among them at all.
            watchpoint::find_writers(fov::master_address(), module.base, kWatchMs,
                                     watchpoint::Trap::ReadsAndWrites);
            fov::report_destinations(module.base);
        }

        // The reverse-engineering tools below are how the correction was found:
        // a differential value search, a watchpoint, and a live watch of the
        // candidates. They are not part of the fix and stay dormant unless
        // someone drops a marker file next to the log.
        std::filesystem::path rerun = logger::path();
        rerun.replace_filename(L"rerun-hunt");
        std::filesystem::path hotkey_hunt = logger::path();
        hotkey_hunt.replace_filename(L"hunt-hotkey");
        std::filesystem::path watch_marker = logger::path();
        watch_marker.replace_filename(L"watch-candidates");
        std::filesystem::path geometry_marker = logger::path();
        geometry_marker.replace_filename(L"find-2d");

        constexpr unsigned int kTimeoutMs = 15 * 60 * 1000;
        const mem::Region data = mem::section(module, ".data");

        if (std::filesystem::exists(hotkey_hunt, ec) && data) {
            logger::info("'hunt-hotkey' found -- differential search around an external FOV change");
            hunt::run_hotkey(data, module.base, 60 * 1000);
        } else if (std::filesystem::exists(rerun, ec) && data) {
            logger::info("'rerun-hunt' found -- running the differential search again");
            hunt::run(data, weight, module.base, kTimeoutMs);
        } else if (std::filesystem::exists(geometry_marker, ec) && data) {
            logger::info("'find-2d' found -- searching for the known 2D geometry");
            hunt::find_known_values(data, module.base, weight, kTimeoutMs);
        } else if (std::filesystem::exists(watch_marker, ec)) {
            logger::info("'watch-candidates' found -- watching the known addresses");
            hunt::watch(module.base, weight, kTimeoutMs);
        }
    }

    logger::info("running");
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
        fov::uninstall();
        MH_Uninitialize();
        logger::close();
    }
    return TRUE;
}
