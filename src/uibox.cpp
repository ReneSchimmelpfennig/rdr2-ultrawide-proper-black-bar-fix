#include "uibox.h"

#include <windows.h>

#include <cstring>

#include "log.h"
#include "patterns.h"

namespace uibox {
namespace {

constexpr std::uint8_t kRet = 0xC3;

std::uintptr_t g_site = 0;
std::uint8_t g_original = 0;
bool g_disabled = false;

bool write_byte(std::uintptr_t address, std::uint8_t value) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(address), 1, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    *reinterpret_cast<volatile std::uint8_t*>(address) = value;
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(address), 1, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), 1);
    return true;
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
        logger::info("uibox: signature matched {} time(s), need exactly one -- not patching",
                     hits.size());
        for (const auto hit : hits) {
            logger::info("uibox:   candidate 0x{:016X}", hit);
        }
        return false;
    }

    g_site = hits.front();
    g_original = *reinterpret_cast<const std::uint8_t*>(g_site);
    logger::info("uibox: 16:9 box transform found at 0x{:016X}", g_site);
    return true;
}

bool set_disabled(bool disable) {
    if (g_site == 0) {
        return false;
    }
    if (disable == g_disabled) {
        return true;
    }
    if (!write_byte(g_site, disable ? kRet : g_original)) {
        logger::info("uibox: could not write the patch");
        return false;
    }
    g_disabled = disable;
    logger::info("uibox: 16:9 boxing of the UI is now {}", disable ? "OFF" : "ON");
    return true;
}

bool disabled() { return g_disabled; }

bool found() { return g_site != 0; }

void verify() {
    if (g_site == 0) {
        return;
    }
    const auto now = *reinterpret_cast<const volatile std::uint8_t*>(g_site);
    const std::uint8_t expected = g_disabled ? kRet : g_original;
    if (now == expected) {
        logger::info("uibox: patch site reads 0x{:02X} as written -- holding", now);
    } else {
        logger::info("uibox: patch site reads 0x{:02X}, expected 0x{:02X} -- something restored it,"
                     " the result is void rather than negative",
                     now, expected);
    }
}

void restore() {
    if (g_disabled) {
        set_disabled(false);
    }
}

}  // namespace uibox
