#include "fov.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>

#include "framing.h"
#include "log.h"
#include "patterns.h"

namespace fov {
namespace {

// void ApplyCameraState(CameraState* dst, const CameraState* src)
using ApplyFn = void (*)(std::uintptr_t, std::uintptr_t);

ApplyFn g_original = nullptr;
void* g_target = nullptr;

std::uintptr_t g_weight_addr = 0;
std::uintptr_t g_bar_addr = 0;
std::uintptr_t g_master_addr = 0;
std::uintptr_t g_module_base = 0;
Config g_config;

// The detour runs several times per frame from ten call sites, so it must stay
// cheap and must not allocate or take locks. Logging is limited to the first
// few calls, purely as proof that it is being reached at all.
std::atomic<int> g_calls{0};
constexpr int kLoggedCalls = 8;

// Periodic sampling while a cutscene is on screen, to make compounding visible.
std::atomic<DWORD> g_last_sample{0};
std::atomic<int> g_samples{0};
constexpr int kMaxSamples = 200;

float read_float(std::uintptr_t addr) {
    float value = 0.0f;
    std::memcpy(&value, reinterpret_cast<const void*>(addr), sizeof(value));
    return value;
}

// Which camera-state structures does the game apply to? Recorded from inside
// the detour, so no allocation and no logger mutex: a small table plus counts,
// reported afterwards.
constexpr std::size_t kMaxDestinations = 24;
std::atomic<std::size_t> g_dst_known{0};
std::uintptr_t g_dst[kMaxDestinations]{};
std::atomic<unsigned long long> g_dst_count[kMaxDestinations]{};

void record_destination(std::uintptr_t dst) {
    const std::size_t known = g_dst_known.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < known; ++i) {
        if (g_dst[i] == dst) {
            g_dst_count[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (known < kMaxDestinations) {
        g_dst[known] = dst;
        g_dst_count[known].store(1, std::memory_order_relaxed);
        g_dst_known.store(known + 1, std::memory_order_release);
    }
}

void detour(std::uintptr_t dst, std::uintptr_t src) {
    g_original(dst, src);

    const std::uintptr_t fov_addr =
        dst + static_cast<std::uintptr_t>(patterns::kCameraStateFov);

    // Restricting the correction to the master was wrong: a read+write
    // watchpoint showed the master is read by exactly one instruction in the
    // whole game, the getter -- and hooking that getter changed nothing on
    // screen. So the master feeds peripheral consumers only, and the projection
    // takes its FOV from a different camera state.
    //
    // ApplyCameraState is generic, so correct every destination and record
    // which ones exist. Whichever one moves the picture is the one we want.
    record_destination(dst);

    const float original = read_float(fov_addr);
    float result = original;
    float weight = 0.0f;

    switch (g_config.mode) {
        case Mode::Off:
        case Mode::Poke:
        case Mode::Watch:
            return;

        case Mode::Test:
        case Mode::TestWatch:
            result = original * g_config.test_factor;
            break;

        case Mode::Corrected: {
            weight = read_float(g_weight_addr);
            if (weight <= 0.0f) {
                return;  // gameplay: leave the camera exactly as authored
            }
            // k straight from the game's own bar height: it already divides by
            // the true backbuffer aspect, so this is correct in windowed mode
            // and at non-native resolutions without asking Windows.
            const float bar = read_float(g_bar_addr);
            double k = framing::correction_factor_from_bars(bar, weight);
            if (!(k > 0.0 && k < 1.0)) {
                k = framing::correction_factor(GetSystemMetrics(SM_CXSCREEN),
                                               GetSystemMetrics(SM_CYSCREEN));
            }
            const double factor = framing::blended_factor(k, weight);
            result = static_cast<float>(framing::corrected_vfov_deg(original, factor));

            // ApplyCameraState is called for two dozen camera states, and some
            // of them may well feed each other. If a corrected value ever ends
            // up as somebody's source, the correction would compound frame over
            // frame and the picture would collapse to a telephoto view. Sample
            // the values into the log so that shows up as evidence rather than
            // as a vague complaint about the zoom.
            const DWORD now = GetTickCount();
            const DWORD last = g_last_sample.load(std::memory_order_relaxed);
            if (now - last >= 500 && g_samples.load() < kMaxSamples) {
                g_last_sample.store(now, std::memory_order_relaxed);
                g_samples.fetch_add(1);
                logger::info("  cutscene: weight {:.3f}  k {:.5f}  blend {:.5f}  {:.3f} -> {:.3f}",
                             weight, k, factor, original, result);
            }
            break;
        }
    }

    std::memcpy(reinterpret_cast<void*>(fov_addr), &result, sizeof(result));

    const int call = g_calls.fetch_add(1);
    if (call < kLoggedCalls) {
        logger::info("  ApplyCameraState {}: dst 0x{:016X}  {:.4f} -> {:.4f}   (weight {:.4f})",
                     call + 1, dst, original, result, weight);
    }
}

}  // namespace

Config read_config() {
    Config config;

    const char* compiled = "?";
    switch (kCompiledDefaultMode) {
        case Mode::Off: compiled = "OFF"; break;
        case Mode::Test: compiled = "TEST"; break;
        case Mode::Corrected: compiled = "CORRECTED"; break;
        case Mode::Poke: compiled = "POKE"; break;
    }
    logger::info("compiled-in default mode: {}", compiled);

    std::filesystem::path path = logger::path();
    if (path.empty()) {
        return config;
    }
    path.replace_filename(L"fov.txt");

    // CreateFileW rather than ifstream: the first attempt at this silently
    // reported "no fov.txt" for a file that demonstrably existed and was
    // readable, and an ifstream failure carries no reason. Going through Win32
    // directly gives us GetLastError to log.
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        logger::info("fov.txt not readable (error {}) -- using the compiled-in mode", error);
        logger::info("  tried: {}", path.string());

        // Error 2 on a file that demonstrably exists, in a directory this same
        // process just created the log in. Enumerate the directory to see what
        // the game process actually thinks is there -- that distinguishes "the
        // file is filtered" from "this is a different directory entirely".
        std::filesystem::path pattern = path;
        pattern.replace_filename(L"*");

        WIN32_FIND_DATAW found{};
        const HANDLE search = FindFirstFileW(pattern.c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) {
            logger::info("  directory enumeration failed too (error {})", GetLastError());
        } else {
            logger::info("  what this process sees in that directory:");
            int entries = 0;
            do {
                const std::wstring name = found.cFileName;
                if (name != L"." && name != L"..") {
                    logger::info("    {}", std::filesystem::path(name).string());
                    ++entries;
                }
            } while (FindNextFileW(search, &found) && entries < 20);
            FindClose(search);
            if (entries == 0) {
                logger::info("    (nothing)");
            }
        }
        return config;
    }

    char buffer[128]{};
    DWORD read = 0;
    const bool ok = ReadFile(handle, buffer, sizeof(buffer) - 1, &read, nullptr) != 0;
    CloseHandle(handle);
    if (!ok || read == 0) {
        logger::info("fov.txt is empty or unreadable -- defaulting to TEST mode");
        return config;
    }

    std::istringstream file(std::string(buffer, read));
    logger::info("fov.txt: '{}'", std::string(buffer, read).substr(0, read));

    std::string word;
    file >> word;
    for (char& c : word) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }

    if (word == "real") {
        config.mode = Mode::Corrected;
    } else if (word == "off") {
        config.mode = Mode::Off;
    } else if (word == "poke") {
        config.mode = Mode::Poke;
        float value = 0.0f;
        if (file >> value && value > 1.0f && value < 170.0f) {
            config.poke_value = value;
        }
    } else if (word == "test") {
        config.mode = Mode::Test;
        float factor = 0.0f;
        if (file >> factor && factor > 0.05f && factor < 4.0f) {
            config.test_factor = factor;
        }
    } else {
        logger::info("fov.txt: did not understand '{}' -- using TEST mode", word);
    }
    return config;
}

bool install(const std::vector<mem::NamedRegion>& sections, const mem::Region& module,
             std::uintptr_t anchor, const Config& config) {
    g_config = config;
    g_module_base = module.base;
    g_weight_addr = anchor + patterns::letterbox::kWeight;
    g_bar_addr = anchor + patterns::letterbox::kBarFractionDisplay;

    const auto pattern = mem::parse_pattern(patterns::kFovGetter);
    if (!pattern) {
        logger::info("FOV getter signature is malformed -- not installing");
        return false;
    }

    std::vector<std::uintptr_t> hits;
    for (const auto& [name, region] : sections) {
        mem::find_all(region, *pattern, hits);
    }
    if (hits.size() != 1) {
        logger::info("FOV getter signature matched {} time(s), need exactly 1 -- not installing",
                     hits.size());
        return false;
    }

    const std::uintptr_t getter = hits.front() + patterns::kFovGetterOffset;

    // The signature borrows its uniqueness from a neighbouring function, so
    // verify rather than trust: the movss must point somewhere sane, and what
    // it points at must look like a field of view.
    const std::uintptr_t target = mem::resolve_rip_relative(
        getter, patterns::kFovMovssDispOffset, patterns::kFovMovssLength);
    if (!module.contains(target)) {
        logger::info("FOV getter reads 0x{:016X}, outside the module -- not installing", target);
        return false;
    }

    logger::info("FOV getter at module +0x{:X}, reads module +0x{:X}", getter - module.base,
                 target - module.base);

    // At plugin load the game has not rendered yet and the value is still 0.0,
    // so checking it immediately rejects a perfectly good hook. Wait for the
    // camera to come up first -- the observation run showed it takes about a
    // second, but loading a save takes considerably longer.
    constexpr DWORD kStepMs = 500;
    constexpr DWORD kMaxWaitMs = 5 * 60 * 1000;

    float current = read_float(target);
    DWORD waited = 0;
    bool announced = false;
    while (!(current >= patterns::kFovSanityMin && current <= patterns::kFovSanityMax)) {
        if (waited >= kMaxWaitMs) {
            logger::info("  value never became plausible (last: {:.4f}) -- not installing",
                         current);
            logger::info("  (expected {:.0f}..{:.0f} degrees)", patterns::kFovSanityMin,
                         patterns::kFovSanityMax);
            return false;
        }
        if (!announced) {
            logger::info("  value is {:.4f} -- waiting for the camera to come up", current);
            announced = true;
        }
        Sleep(kStepMs);
        waited += kStepMs;
        current = read_float(target);
    }

    logger::info("  reads {:.4f} degrees after {:.1f} s -- plausible", current,
                 static_cast<double>(waited) / 1000.0);
    g_master_addr = target;

    if (g_config.mode == Mode::Poke) {
        logger::info("mode POKE -- not hooking; the global is overwritten directly");
        return true;
    }
    if (g_config.mode == Mode::Watch) {
        logger::info("mode WATCH -- not hooking; looking for whoever writes the global");
        return true;
    }

    // The getter only told us where the master lives. What actually has to be
    // hooked is the function that writes it, found with the watchpoint.
    const auto apply_pattern = mem::parse_pattern(patterns::kCameraApply);
    if (!apply_pattern) {
        logger::info("ApplyCameraState signature is malformed -- not installing");
        return false;
    }
    std::vector<std::uintptr_t> apply_hits;
    for (const auto& [name, region] : sections) {
        mem::find_all(region, *apply_pattern, apply_hits);
    }
    if (apply_hits.size() != 1) {
        logger::info("ApplyCameraState matched {} time(s), need exactly 1 -- not installing",
                     apply_hits.size());
        return false;
    }
    logger::info("ApplyCameraState at module +0x{:X}", apply_hits.front() - module.base);

    g_target = reinterpret_cast<void*>(apply_hits.front());
    if (const MH_STATUS status =
            MH_CreateHook(g_target, reinterpret_cast<void*>(&detour),
                          reinterpret_cast<void**>(&g_original));
        status != MH_OK) {
        logger::info("MH_CreateHook failed: {}", MH_StatusToString(status));
        g_target = nullptr;
        return false;
    }
    if (const MH_STATUS status = MH_EnableHook(g_target); status != MH_OK) {
        logger::info("MH_EnableHook failed: {}", MH_StatusToString(status));
        MH_RemoveHook(g_target);
        g_target = nullptr;
        return false;
    }

    switch (g_config.mode) {
        case Mode::Off:
            logger::info("hook installed, mode OFF -- value passed through unchanged");
            break;
        case Mode::TestWatch:
            logger::info("hook installed, mode TEST+WATCH -- factor {:.3f}, watchpoint follows",
                         g_config.test_factor);
            break;
        case Mode::Test:
            logger::info("hook installed, mode TEST -- every FOV multiplied by {:.3f}",
                         g_config.test_factor);
            logger::info("  this applies in gameplay too, on purpose: the effect must be obvious");
            break;
        case Mode::Corrected:
            logger::info("hook installed, mode CORRECTED -- cutscenes only");
            break;
    }
    return true;
}

void run_poke(unsigned int duration_ms) {
    if (g_master_addr == 0 || g_module_base == 0) {
        logger::info("poke: addresses unknown -- nothing to do");
        return;
    }

    // Every launch costs the player a load, so hammer all three known copies at
    // once. If the picture stays still, all three are excluded in one run. If it
    // moves, a second run narrows it down -- still cheaper than three runs.
    struct Slot {
        const char* name;
        std::uintptr_t address;
        bool writable;
    };
    Slot slots[] = {
        {"master", g_master_addr, false},
        {"degA", g_module_base + patterns::candidates::kDegreeCopyA, false},
        {"degB", g_module_base + patterns::candidates::kDegreeCopyB, false},
    };

    int usable = 0;
    for (Slot& s : slots) {
        MEMORY_BASIC_INFORMATION mbi{};
        const bool ok =
            VirtualQuery(reinterpret_cast<LPCVOID>(s.address), &mbi, sizeof(mbi)) != 0 &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY)) != 0;
        s.writable = ok;
        if (ok) {
            ++usable;
        } else {
            logger::info("poke: {} at module +0x{:X} is not writable (protect 0x{:X})", s.name,
                         s.address - g_module_base, mbi.Protect);
        }
    }
    if (usable == 0) {
        logger::info("poke: nothing writable -- aborting");
        return;
    }

    logger::info("");
    logger::info("=== POKE: writing {:.3f} over {} address(es) for {} s ===", g_config.poke_value,
                 usable, duration_ms / 1000);
    logger::info("Watch the picture. Expect flicker rather than a clean change --");
    logger::info("we are racing the game's own per-frame write, and that is fine:");
    logger::info("flicker answers 'do any of these reach the projection at all'.");

    const DWORD started = GetTickCount();
    DWORD last_report = started;
    unsigned long long writes = 0;
    float seen[3]{};

    while (GetTickCount() - started < duration_ms) {
        for (int i = 0; i < 3; ++i) {
            if (!slots[i].writable) {
                continue;
            }
            seen[i] = read_float(slots[i].address);
            std::memcpy(reinterpret_cast<void*>(slots[i].address), &g_config.poke_value,
                        sizeof(float));
        }
        ++writes;

        const DWORD now = GetTickCount();
        if (now - last_report >= 5000) {
            // What we read back before writing is whatever won the last race.
            // Values close to ours mean the game is not rewriting that slot.
            logger::info("  {} rounds; read back before writing: master {:.4f}, degA {:.4f}, degB {:.4f}",
                         writes, seen[0], seen[1], seen[2]);
            last_report = now;
        }
        Sleep(1);
    }

    logger::info("=== POKE done, {} rounds ===", writes);
}

std::uintptr_t master_address() { return g_master_addr; }

void report_destinations(std::uintptr_t module_base) {
    const std::size_t known = g_dst_known.load();
    logger::info("");
    logger::info("camera-state destinations seen: {}", known);
    for (std::size_t i = 0; i < known; ++i) {
        const std::uintptr_t fov_addr = g_dst[i] + patterns::kCameraStateFov;
        const bool is_master = fov_addr == g_master_addr;
        // Globals sit inside the module; heap-allocated camera objects do not.
        const bool in_module = g_dst[i] > module_base && g_dst[i] - module_base < 0x8000000;
        if (in_module) {
            logger::info("    module +0x{:<9X}  {} call(s){}", g_dst[i] - module_base,
                         g_dst_count[i].load(), is_master ? "   <- the master" : "");
        } else {
            logger::info("    0x{:016X}  {} call(s)   <- not in the module (heap object)",
                         g_dst[i], g_dst_count[i].load());
        }
    }
    if (known == kMaxDestinations) {
        logger::info("    (table full -- there may be more)");
    }
}

void uninstall() {
    if (g_target == nullptr) {
        return;
    }
    MH_DisableHook(g_target);
    MH_RemoveHook(g_target);
    g_target = nullptr;
}

}  // namespace fov
