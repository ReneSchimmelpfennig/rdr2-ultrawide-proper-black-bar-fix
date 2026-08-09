#include "fov.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "framing.h"
#include "log.h"
#include "patterns.h"

namespace fov {
namespace {

using GetFovFn = float (*)();

GetFovFn g_original = nullptr;
void* g_target = nullptr;

std::uintptr_t g_weight_addr = 0;
std::uintptr_t g_bar_addr = 0;
Config g_config;

// The detour runs several times per frame from ten call sites, so it must stay
// cheap and must not allocate or take locks. Logging is limited to the first
// few calls, purely as proof that it is being reached at all.
std::atomic<int> g_calls{0};
constexpr int kLoggedCalls = 8;

float read_float(std::uintptr_t addr) {
    float value = 0.0f;
    std::memcpy(&value, reinterpret_cast<const void*>(addr), sizeof(value));
    return value;
}

float detour() {
    const float original = g_original();

    float result = original;
    float weight = 0.0f;

    switch (g_config.mode) {
        case Mode::Off:
            break;

        case Mode::Test:
            result = original * g_config.test_factor;
            break;

        case Mode::Corrected: {
            weight = read_float(g_weight_addr);
            if (weight > 0.0f) {
                // k straight from the game's own bar height: it already divides
                // by the true backbuffer aspect, so this is correct in windowed
                // mode and at non-native resolutions without asking Windows.
                const float bar = read_float(g_bar_addr);
                double k = framing::correction_factor_from_bars(bar, weight);
                if (!(k > 0.0 && k < 1.0)) {
                    // Bars not usable this frame -- fall back to the display.
                    k = framing::correction_factor(GetSystemMetrics(SM_CXSCREEN),
                                                   GetSystemMetrics(SM_CYSCREEN));
                }
                const double factor = framing::blended_factor(k, weight);
                result = static_cast<float>(framing::corrected_vfov_deg(original, factor));
            }
            break;
        }
    }

    const int call = g_calls.fetch_add(1);
    if (call < kLoggedCalls) {
        logger::info("  GetFov call {}: {:.4f} -> {:.4f}   (weight {:.4f})", call + 1, original,
                     result, weight);
    }
    return result;
}

}  // namespace

Config read_config() {
    Config config;

    std::filesystem::path path = logger::path();
    if (path.empty()) {
        return config;
    }
    path.replace_filename(L"fov.txt");

    std::ifstream file(path);
    if (!file) {
        logger::info("no fov.txt -- defaulting to TEST mode, factor {:.3f}", config.test_factor);
        logger::info("  write 'real' into {} to switch to the actual correction",
                     path.string());
        return config;
    }

    std::string word;
    file >> word;
    for (char& c : word) {
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }

    if (word == "real") {
        config.mode = Mode::Corrected;
    } else if (word == "off") {
        config.mode = Mode::Off;
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

    const float current = read_float(target);
    logger::info("FOV getter at module +0x{:X}, reads module +0x{:X} = {:.4f}",
                 getter - module.base, target - module.base, current);

    if (!(current >= patterns::kFovSanityMin && current <= patterns::kFovSanityMax)) {
        logger::info("  that is not a plausible field of view -- not installing");
        logger::info("  (expected {:.0f}..{:.0f} degrees)", patterns::kFovSanityMin,
                     patterns::kFovSanityMax);
        return false;
    }

    g_target = reinterpret_cast<void*>(getter);
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

void uninstall() {
    if (g_target == nullptr) {
        return;
    }
    MH_DisableHook(g_target);
    MH_RemoveHook(g_target);
    g_target = nullptr;
}

}  // namespace fov
