#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bars.h"
#include "config.h"
#include "dump.h"
#include "fov.h"
#include "framing.h"
#include "hunt.h"
#include "log.h"
#include "mem.h"
#include "overlay.h"
#include "patterns.h"
#include "safearea.h"
#include "uibox.h"
#include "watchpoint.h"

namespace {

HMODULE g_self = nullptr;

// Address of the `C6 05 ... FF` instruction itself. The letterbox patch needs
// the instruction, the struct lookup needs what it points at.
std::uintptr_t g_anchor_store = 0;

// Diagnostics are requested by key, not by file.
//
// The game process cannot see files other processes create in its own log
// directory -- the log itself lists the directory contents and my marker files
// are simply absent from that listing, twice now. Whatever causes that, a
// marker file is not a usable switch, and two sessions were spent waiting for
// searches that could never start.
std::atomic<bool> g_request_2d_search{false};

// The letterbox struct, so the hotkey thread can measure what a patch did
// instead of relying on the picture.
std::uintptr_t g_letterbox_anchor = 0;

// So the hotkey thread can turn return addresses into module offsets, and can
// install the uibox census on demand.
std::uintptr_t g_module_base = 0;
std::vector<mem::NamedRegion> g_sections;

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
    // The build stamp is here because it has already cost us twice: a
    // conclusion drawn from a run that turned out to be using an older .asi.
    // Copy-Item carries the source timestamp over, so the file date in the game
    // folder is the build date and says nothing about which build is deployed.
    // This line does.
    logger::info("RDR2 Ultrawide Cutscene Fix {}  (built {} {})", PLUGIN_VERSION, __DATE__,
                 __TIME__);
    if (fov::strength() == 0.0f) {
        logger::info("*** DIAGNOSTIC BUILD: the field-of-view correction starts DISABLED. ***");
        logger::info("*** Bars are still removed. F10 or F12 brings the correction back.  ***");
    }
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

// What the letterbox struct says right now, sampled over a few frames.
//
// Exists because a probe that changes nothing on screen is ambiguous: it can
// mean the value does not drive the picture, or that the value never changed at
// all. Twice now that ambiguity has cost a game session. The struct is the
// nearest measurable point downstream of the aspect getter, so it settles which
// of the two happened without anyone judging a picture by eye.
void sample_letterbox(const char* label) {
    if (g_letterbox_anchor == 0) {
        logger::info("  {}: no letterbox anchor -- cannot measure", label);
        return;
    }
    const std::uintptr_t weight_addr = g_letterbox_anchor + patterns::letterbox::kWeight;
    const std::uintptr_t bar235_addr = g_letterbox_anchor + patterns::letterbox::kBarFraction235;
    const std::uintptr_t display_addr =
        g_letterbox_anchor + patterns::letterbox::kBarFractionDisplay;

    if (!is_readable(weight_addr, patterns::letterbox::kStride)) {
        logger::info("  {}: letterbox struct unreadable", label);
        return;
    }

    for (int i = 0; i < 4; ++i) {
        const float weight = read_float(weight_addr);
        const float bar235 = read_float(bar235_addr);
        const float display = read_float(display_addr);
        logger::info("  {} [{}]  weight {:.4f}  bar(2.35) {:.6f}  bar(display) {:.6f}  k {:.5f}",
                     label, i, weight, bar235, display,
                     framing::correction_factor_from_bars(display, weight));
        Sleep(80);
    }
}

// Live controls, so the framing can be judged and dialled in during a single
// cutscene instead of one game launch per guess.
void run_hotkeys(unsigned int duration_ms) {
    logger::info("");
    logger::info("=== hotkeys ===");
    logger::info("  F7   correction on / off");
    logger::info("  F8   letterbox bars on / off");
    logger::info("  F9   strength -0.05      F10  strength +0.05");
    logger::info("  Ctrl+Alt+1   one correction per frame on / off  [transition test]");
    logger::info("  Ctrl+Alt+O   full-screen overlays: stretched / fitted to 16:9  [2D test]");
    logger::info("  Ctrl+Alt+U   16:9 boxing of the UI on / off  [2D test]");
    logger::info("  Ctrl+Alt+A   report the display as 16:9  [2D test -- also flattens the bars]");
    logger::info("  Ctrl+Alt+S   report the bar height as zero to the script layer  [2D test]");
    logger::info("  Ctrl+Alt+B   zero the bar heights (test: do the artefacts go away?)");
    logger::info("  Ctrl+Alt+F   search memory for the 2D geometry (do this in a cutscene)");
    logger::info("  F11  force the correction in gameplay (for still comparisons)");
    logger::info("  F12  set strength to exactly 1.00");
    logger::info("current: strength {:.2f}, bars {}, safe area {} ({} getter(s))", fov::strength(),
                 bars::hidden() ? "hidden" : "visible", safearea::flat() ? "flattened" : "original",
                 safearea::count());

    const auto pressed = [](int vk, bool& was_down) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool edge = down && !was_down;
        was_down = down;
        return edge;
    };
    bool bars_key = false;    // Ctrl+Alt+B; not F6, that is RDR2's photo mode
    bool search_key = false;  // Ctrl+Alt+F
    bool safe_key = false;    // Ctrl+Alt+S
    bool aspect_key = false;  // Ctrl+Alt+A
    bool uibox_key = false;   // Ctrl+Alt+U
    bool overlay_key = false; // Ctrl+Alt+O
    bool once_key = false;    // Ctrl+Alt+1
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
        if (combo_pressed('F', search_key)) {
            g_request_2d_search.store(true);
            logger::info("Ctrl+Alt+F: 2D geometry search requested");
        }
        if (combo_pressed('1', once_key)) {
            fov::set_once_per_frame(!fov::once_per_frame());
            logger::info("Ctrl+Alt+1: one correction per frame {}",
                         fov::once_per_frame() ? "ON (shipping behaviour)" : "OFF (correct every "
                                                                            "authored value)");
        }
        if (combo_pressed('O', overlay_key)) {
            logger::info("Ctrl+Alt+O pressed");
            overlay::report(g_module_base);
            switch (overlay::mode()) {
                case overlay::Mode::Cover:
                    overlay::set_mode(overlay::Mode::Fitted);
                    break;
                case overlay::Mode::Fitted:
                    overlay::set_mode(overlay::Mode::Stretched);
                    break;
                case overlay::Mode::Stretched:
                    overlay::set_mode(overlay::Mode::Cover);
                    break;
            }
        }
        if (combo_pressed('U', uibox_key)) {
            logger::info("Ctrl+Alt+U pressed");
            if (!uibox::found()) {
                logger::info("  installing the 16:9-boxing census (measured dead once already)");
                uibox::init(g_sections);
            } else {
                uibox::report(g_module_base);
            }
        }
        if (combo_pressed('A', aspect_key)) {
            logger::info("Ctrl+Alt+A pressed");
            if (safearea::aspect_count() == 0) {
                logger::info("  ... but the aspect probe never armed -- nothing was changed, so a");
                logger::info("      picture that does not move says nothing here");
            }
            sample_letterbox("before");
            safearea::set_aspect_pretend_16_9(!safearea::aspect_pretending());
            sample_letterbox("after ");
            // Does the patch survive? Arxan restoring it would look exactly like
            // a value that does not matter, and the two need telling apart.
            safearea::verify_aspect_patch();
        }
        if (combo_pressed('S', safe_key)) {
            safearea::set_flat(!safearea::flat());
        }
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

DWORD WINAPI hotkey_thread(LPVOID) {
    constexpr unsigned int kHotkeyMs = 60 * 60 * 1000;
    run_hotkeys(kHotkeyMs);
    return 0;
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

    // Read now, act later. Nothing consults these yet -- this run only proves
    // the file can be found and parsed at all, which after the fov.txt episode
    // is not something to take on trust.
    config::load(g_self);

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
        g_letterbox_anchor = anchor;

        std::error_code ec;

        // The fix goes in first, always. It used to sit behind a check for the
        // image dump: no dump meant ten minutes of watching a letterbox and no
        // hook at all. That made a diagnostic step a precondition for the actual
        // feature, and when the log moved the dump was suddenly "missing" and
        // the whole fix quietly stopped working.
        //
        // The dump is a reverse-engineering aid. It runs when asked, never
        // otherwise.
        const fov::Config fov_config = fov::read_config();
        const bool fov_ready =
            fov::install(mem::executable_sections(module), module, anchor, fov_config);

        if (fov_ready && fov_config.mode == fov::Mode::Poke) {
            constexpr unsigned int kPokeMs = 60 * 1000;
            fov::run_poke(kPokeMs);
        } else if (fov_ready && fov_config.mode == fov::Mode::Corrected) {
            bars::init(g_anchor_store);
            bars::set_hidden(true);  // the framing cannot be judged with them on

            // Prepared, not switched on: the display aspect is not known until
            // the first correction has read the game's bar height, so the
            // decision is made in the loop below.
            bars::init_side_bars(mem::executable_sections(module), anchor);

            // Prepared but not applied: measured to have no effect on the 2D
            // layer (see docs/how-it-works.md). The site stays reachable by
            // Ctrl+Alt+S so the measurement can be repeated cheaply, but it is
            // not part of the fix.
            safearea::init(mem::executable_sections(module), anchor);

            // The aspect probe. Identified by the value the getter reads, so
            // the primary display's aspect is what it is looked up by -- good
            // enough to find a global, and it is logged either way.
            const float display_aspect = static_cast<float>(GetSystemMetrics(SM_CXSCREEN)) /
                                         static_cast<float>(GetSystemMetrics(SM_CYSCREEN));
            if (safearea::init_aspect(mem::executable_sections(module), display_aspect,
                                      module.base)) {
                // On from the start, not from a key. Toggling mid-cutscene only
                // answers whether the aspect is read *per frame*; if the 2D
                // layer sizes itself once when the cutscene is built, a later
                // toggle can never reach it. This way the layout is built under
                // the faked aspect in the first place.
                //
                // The field of view survives it: the faked aspect drives the bar
                // height to a degenerate 1.0, k comes out negative, and the
                // implausible-k fallback above computes k from the resolution
                // instead. So the picture stays as it always was and any change
                // to the 2D layer is attributable.
                // Measured with this on from startup: no effect on the 2D
                // layer. Off again, so the shipped behaviour is the plain fix.
                // Ctrl+Alt+A still arms it for anyone wanting to repeat the
                // measurement -- but note it drives the bar height to a
                // degenerate 1.0, which makes F8 cover the screen.
                logger::info("safearea: aspect probe armed but inactive (Ctrl+Alt+A)");
            }

            // uibox is deliberately NOT installed here any more. Both functions
            // in that family were hooked and counted through a full cutscene and
            // came back at zero calls -- measured, with the field-of-view detour
            // firing in the same run to prove the hooks worked at all. They are
            // dead code, so there is nothing to switch off. Ctrl+Alt+U still
            // installs the census for anyone wanting to repeat it.
            g_module_base = module.base;
            g_sections = mem::executable_sections(module);

            // The overlay fit. Unlike everything before it, this one arrived
            // with numbers: its two outputs are the constants RenderDoc found in
            // the intro filter's uniforms. On from startup so a single cutscene
            // answers it; Ctrl+Alt+O switches back for the comparison.
            if (overlay::init(g_sections)) {
                logger::info("");
                // Cover by default: it fills the width without distorting, which
                // is the same trade the field-of-view correction makes and the
                // reason this project calls itself proper.
                logger::info("Ctrl+Alt+O cycles how full-screen overlays are mapped:");
                logger::info("  COVER -> FITTED -> STRETCHED -> ...");
                logger::info("  Cover keeps the proportions and crops 25%% of the height.");
                logger::info("  Fitted is the game's own behaviour, with the smearing.");
                logger::info("  Stretched fills the width and is 34%% wider than authored.");
                overlay::set_mode(overlay::Mode::Cover);
            }

            // On its own thread. It used to run here and block for an hour,
            // which meant everything below -- every diagnostic tool -- was
            // unreachable in the one mode people actually run. A marker file
            // sat there being ignored and the search it asked for never
            // happened.
            // Hotkeys are off in shipped builds.
            //
            // They were how the correction was dialled in, and by the end there
            // were nine of them, several driving experiments that have since
            // been decided one way or the other. For anyone installing the mod
            // that is noise, and a key that changes the picture without
            // explanation is worse than no key at all.
            //
            // The code stays. Flip this to get the whole set back for a
            // debugging session.
            constexpr bool kEnableHotkeys = false;
            if (kEnableHotkeys) {
                if (HANDLE keys = CreateThread(nullptr, 0, hotkey_thread, nullptr, 0, nullptr)) {
                    CloseHandle(keys);
                }
            }
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
        std::filesystem::path dump_marker = logger::path();
        dump_marker.replace_filename(L"dump-image");

        if (std::filesystem::exists(dump_marker, ec)) {
            logger::info("'dump-image' found -- waiting for a cutscene, then dumping");
            constexpr DWORD kWatchMs = 10 * 60 * 1000;
            const bool saw_cutscene = monitor_letterbox_state(anchor, kWatchMs);
            dump_image_once(module, saw_cutscene);
        }

        constexpr unsigned int kTimeoutMs = 15 * 60 * 1000;
        const mem::Region data = mem::section(module, ".data");

        if (std::filesystem::exists(hotkey_hunt, ec) && data) {
            logger::info("'hunt-hotkey' found -- differential search around an external FOV change");
            hunt::run_hotkey(data, module.base, 60 * 1000);
        } else if (std::filesystem::exists(rerun, ec) && data) {
            logger::info("'rerun-hunt' found -- running the differential search again");
            hunt::run(data, weight, module.base, kTimeoutMs);
        } else if (std::filesystem::exists(watch_marker, ec)) {
            logger::info("'watch-candidates' found -- watching the known addresses");
            hunt::watch(module.base, weight, kTimeoutMs);
        }

        logger::info("running -- Ctrl+Alt+F in a cutscene starts the 2D geometry search");

        // Serve key-triggered diagnostics for as long as the game runs. The
        // marker files above stay for the tools nobody needs mid-session, but
        // anything that has to be triggered while playing goes by key: the game
        // process cannot see files created by other processes here.
        // Report the uibox measurement on a timer as well as on the key. Two
        // sessions have already been spent on a hotkey that was mistyped or
        // forgotten; the answer to "does this code run at all" should not
        // depend on anyone remembering to ask.
        // Report when it matters rather than on a clock. The last run produced
        // "0 calls after 45 s", which was true and nearly worthless: the game
        // was still in the menu, where nothing script-driven draws anyway. The
        // meaningful moments are the two edges of a cutscene, so the letterbox
        // weight triggers the report.
        bool in_cutscene = false;
        DWORD last_periodic = GetTickCount();

        // Once per session, inside an established cutscene: who reads the field
        // of view of the camera that is being rendered?
        //
        // The remaining judder is a one-frame gap after each camera cut. We
        // identify the rendered camera from the shader constant, which carries
        // last frame's value, so at a cut the new structure is not confirmed yet
        // and goes uncorrected for one frame. No wider net fixes that -- the
        // attempt to correct every state instead compounded and had to be
        // reverted.
        //
        // Correcting where the projection *reads* the value would have neither
        // problem. Whether that is one place or five decides whether it is a
        // small change or the rewrite that was rejected at the start of the
        // project, and this answers it before anything is rewritten.
        bool wide_display_applied = false;

        while (data) {
            if (g_request_2d_search.exchange(false)) {
                hunt::find_known_values(data, module.base, weight, 60 * 1000);
            }

            // Only while a census is installed, i.e. after Ctrl+Alt+U. The
            // cutscene edges are the moments worth reporting: the first run of
            // this reported "0 calls" from the main menu, which was true and
            // nearly worthless.
            // Wait a moment after the cutscene settles, so a correction has
            // actually happened and there is an address worth watching.
            // The reader probe used to run here and it has been removed.
            //
            // It did its job -- it is how the focal-length clamp was found, and
            // that fixed the transition judder. But it blocked this thread for
            // twelve seconds, and everything else the loop does waited with it.
            // On a wide display that delayed switching the side bars on by
            // exactly that long, so the first quarter of a cutscene ran without
            // them: 17:13:34 the aspect was known, 17:13:47 the bars came on.
            //
            // A diagnostic that stalls the feature it is meant to inform has
            // outstayed its welcome. `watchpoint` remains available for the next
            // question that needs it.

            // Cheap, and it settles the question about the intro's side bars.
            bars::verify();
            bars::poll_second_letterbox();
            if (bars::side_bars()) {
                bars::set_target_aspect(true);
            }

            // Once, as soon as the aspect is known. On 21:9 the condition is
            // false and nothing here ever runs.
            if (!wide_display_applied) {
                const double aspect = fov::display_aspect();
                if (aspect > 0.0) {
                    wide_display_applied = true;
                    if (aspect > framing::kUltrawideThreshold &&
                        !config::current().expand_cutscenes_sideways) {
                        logger::info("");
                        logger::info("wider than 21:9 and ExpandCutscenesSideways = false:");
                        logger::info("  framing the picture with side bars instead of extending"
                                     " it");
                        bars::set_hidden(false);
                        bars::set_side_bars(true);
                    }
                }
            }

            if ((uibox::found() || overlay::found()) && is_readable(weight, sizeof(float))) {
                const float w = read_float(weight);
                if (!in_cutscene && w > 0.5f) {
                    in_cutscene = true;
                    logger::info("");
                    logger::info("--- a cutscene just started ---");
                    uibox::report(module.base);
                    overlay::report(module.base);
                } else if (in_cutscene && w < 0.01f) {
                    in_cutscene = false;
                    logger::info("");
                    logger::info("--- the cutscene just ended (the counts that matter) ---");
                    uibox::report(module.base);
                    overlay::report(module.base);
                }
            }
            if ((uibox::found() || overlay::found()) &&
                GetTickCount() - last_periodic > 60 * 1000) {
                last_periodic = GetTickCount();
                uibox::report(module.base);
                overlay::report(module.base);
            }
            Sleep(200);
        }
    }
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
        overlay::restore();
        uibox::restore();
        safearea::restore();
        bars::restore();
        MH_Uninitialize();
        logger::close();
    }
    return TRUE;
}
