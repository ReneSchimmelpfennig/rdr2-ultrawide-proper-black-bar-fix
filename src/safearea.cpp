#include "safearea.h"

#include <windows.h>

#include <array>
#include <cstring>
#include <string_view>

#include "log.h"
#include "patterns.h"

namespace safearea {
namespace {

// movss xmm0, [rip+disp32] ; ret     -- the shape of every float getter.
constexpr std::string_view kFloatGetter = "F3 0F 10 05 ? ? ? ? C3";
constexpr std::size_t kDispOffset = 4;
constexpr std::size_t kMovssLength = 8;

// xorps xmm0, xmm0 ; ret. Four bytes, so it fits over the head of the movss;
// the remaining four bytes of the displacement become unreachable padding. The
// function is only ever entered at its first byte, so nothing else can land in
// the middle of it.
constexpr std::array<std::uint8_t, 4> kZeroReturn = {0x0F, 0x57, 0xC0, 0xC3};

struct Site {
    std::uintptr_t address = 0;
    std::array<std::uint8_t, 4> original{};
    const char* field = "";
};

std::vector<Site> g_sites;
bool g_flat = false;

bool write_bytes(std::uintptr_t address, const std::uint8_t* data, std::size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(address), size, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(address), data, size);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(address), size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), size);
    return true;
}

}  // namespace

bool init(const std::vector<mem::NamedRegion>& sections, std::uintptr_t anchor) {
    g_sites.clear();
    if (anchor == 0) {
        return false;
    }

    const auto pattern = mem::parse_pattern(kFloatGetter);
    if (!pattern) {
        logger::info("safearea: malformed getter signature");
        return false;
    }

    // The two fields that describe how tall the bars are. The weight at -8 is
    // deliberately not in here: it is the cutscene trigger and the blend factor
    // for the field-of-view correction, and neutering it would take the fix
    // down with it.
    const struct {
        std::uintptr_t address;
        const char* name;
    } wanted[] = {
        {anchor + patterns::letterbox::kBarFractionDisplay, "bar height (display aspect)"},
        {anchor + patterns::letterbox::kBarFraction235, "bar height (2.35:1)"},
    };

    std::vector<std::uintptr_t> hits;
    for (const auto& [name, region] : sections) {
        mem::find_all(region, *pattern, hits);
    }
    logger::info("safearea: {} float getter(s) in the image, checking what they read", hits.size());

    for (const std::uintptr_t hit : hits) {
        const std::uintptr_t target = mem::resolve_rip_relative(hit, kDispOffset, kMovssLength);
        for (const auto& want : wanted) {
            if (target != want.address) {
                continue;
            }
            Site site;
            site.address = hit;
            site.field = want.name;
            std::memcpy(site.original.data(), reinterpret_cast<const void*>(hit),
                        site.original.size());
            g_sites.push_back(site);
            logger::info("safearea:   0x{:016X} reads {}", hit, want.name);
        }
    }

    if (g_sites.empty()) {
        logger::info("safearea: no getter reads the bar heights -- nothing to patch");
        return false;
    }
    return true;
}

bool set_flat(bool flat) {
    if (g_sites.empty()) {
        return false;
    }
    if (flat == g_flat) {
        return true;
    }

    int done = 0;
    for (const Site& site : g_sites) {
        const std::uint8_t* bytes = flat ? kZeroReturn.data() : site.original.data();
        if (write_bytes(site.address, bytes, kZeroReturn.size())) {
            ++done;
        } else {
            logger::info("safearea: could not patch 0x{:016X}", site.address);
        }
    }

    g_flat = flat;
    logger::info("safearea: bar height reported as {} ({}/{} getter(s) patched)",
                 flat ? "zero" : "the real value", done, static_cast<int>(g_sites.size()));
    return done == static_cast<int>(g_sites.size());
}

bool flat() { return g_flat; }

int count() { return static_cast<int>(g_sites.size()); }

void restore() {
    if (g_flat) {
        set_flat(false);
    }
}

}  // namespace safearea
