#include "safearea.h"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
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

// --- the aspect probe -------------------------------------------------------

// The nine-byte getter plus the one padding byte behind it. Ten bytes is
// exactly what `mov eax, imm32; movd xmm0, eax; ret` needs; anything less and
// we would be writing into the next function.
constexpr std::size_t kAspectPatchSize = 10;

std::uintptr_t g_aspect_site = 0;
std::uintptr_t g_aspect_global = 0;
std::array<std::uint8_t, kAspectPatchSize> g_aspect_original{};
std::array<std::uint8_t, kAspectPatchSize> g_aspect_expected{};
int g_aspect_candidates = 0;
bool g_aspect_flat = false;

float read_float(std::uintptr_t address) {
    float value = 0.0f;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

bool readable(std::uintptr_t address, std::size_t size) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const auto block_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return address + size <= block_end;
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

bool init_aspect(const std::vector<mem::NamedRegion>& sections, float display_aspect,
                 std::uintptr_t module_base) {
    g_aspect_site = 0;
    g_aspect_candidates = 0;

    const auto pattern = mem::parse_pattern(kFloatGetter);
    if (!pattern) {
        return false;
    }

    std::vector<std::uintptr_t> hits;
    for (const auto& [name, region] : sections) {
        mem::find_all(region, *pattern, hits);
    }

    // Generous: the value only has to look like this display's aspect, because
    // the point is to identify a global, not to measure one.
    constexpr float kTolerance = 1e-3f;
    std::uintptr_t found = 0;

    for (const std::uintptr_t hit : hits) {
        const std::uintptr_t target = mem::resolve_rip_relative(hit, kDispOffset, kMovssLength);
        if (!readable(target, sizeof(float))) {
            continue;
        }
        if (std::fabs(read_float(target) - display_aspect) > kTolerance) {
            continue;
        }
        ++g_aspect_candidates;
        found = hit;
        logger::info("safearea: aspect getter candidate 0x{:016X} -> global 0x{:016X} = {:.6f}",
                     hit, target, read_float(target));
    }

    if (g_aspect_candidates != 1) {
        logger::info("safearea: {} getter(s) read {:.6f} at this moment", g_aspect_candidates,
                     display_aspect);

        // The value-based route needs the global to be populated, and a second
        // after load it is not. Fall back to the address the decompiler gave us
        // -- but verify it is still that getter rather than trusting an offset
        // across a game patch.
        const std::uintptr_t site = module_base + patterns::candidates::kAspectGetter;
        if (!readable(site, kAspectPatchSize)) {
            logger::info("safearea: the known aspect getter address is not readable");
            return false;
        }
        const auto* code = reinterpret_cast<const std::uint8_t*>(site);
        if (code[0] != 0xF3 || code[1] != 0x0F || code[2] != 0x10 || code[3] != 0x05 ||
            code[8] != 0xC3) {
            logger::info("safearea: the known aspect getter address does not hold that getter "
                         "(starts {:02X} {:02X} {:02X} {:02X}) -- stale offset, not patching",
                         code[0], code[1], code[2], code[3]);
            return false;
        }
        const std::uintptr_t target = mem::resolve_rip_relative(site, kDispOffset, kMovssLength);
        logger::info("safearea: falling back to the known address 0x{:016X}, global 0x{:016X} = "
                     "{:.6f}",
                     site, target, readable(target, sizeof(float)) ? read_float(target) : -1.0f);
        found = site;
        g_aspect_candidates = 1;
    }

    // Ten bytes have to be ours. The tenth is padding behind the ret; if it is
    // neither int3 nor nop we are looking at something else and stop.
    const auto tail = *reinterpret_cast<const std::uint8_t*>(found + kAspectPatchSize - 1);
    if (tail != 0x90 && tail != 0xCC) {
        logger::info("safearea: byte behind the aspect getter is 0x{:02X}, not padding -- not patching",
                     tail);
        return false;
    }

    g_aspect_site = found;
    g_aspect_global = mem::resolve_rip_relative(found, kDispOffset, kMovssLength);
    std::memcpy(g_aspect_original.data(), reinterpret_cast<const void*>(found),
                g_aspect_original.size());
    logger::info("safearea: aspect probe ready at 0x{:016X}", found);
    return true;
}

bool set_aspect_pretend_16_9(bool on) {
    if (g_aspect_site == 0) {
        return false;
    }
    if (on == g_aspect_flat) {
        return true;
    }

    // What the getter reads right now, logged at the moment of the test rather
    // than at load. If this is not the display aspect the probe is aimed at the
    // wrong global and the result would mean nothing.
    if (g_aspect_global != 0 && readable(g_aspect_global, sizeof(float))) {
        logger::info("safearea: the aspect global currently reads {:.6f}",
                     read_float(g_aspect_global));
    }

    std::array<std::uint8_t, kAspectPatchSize> bytes = g_aspect_original;
    if (on) {
        const float value = 16.0f / 9.0f;
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));

        bytes[0] = 0xB8;  // mov eax, imm32
        std::memcpy(&bytes[1], &bits, sizeof(bits));
        bytes[5] = 0x66;  // movd xmm0, eax
        bytes[6] = 0x0F;
        bytes[7] = 0x6E;
        bytes[8] = 0xC0;
        bytes[9] = 0xC3;  // ret
    }

    if (!write_bytes(g_aspect_site, bytes.data(), bytes.size())) {
        logger::info("safearea: could not write the aspect patch");
        return false;
    }
    g_aspect_expected = bytes;
    g_aspect_flat = on;
    logger::info("safearea: aspect reported as {}", on ? "16:9" : "the real display aspect");
    return true;
}

void verify_aspect_patch() {
    if (g_aspect_site == 0) {
        return;
    }
    if (!readable(g_aspect_site, kAspectPatchSize)) {
        logger::info("safearea: the patch site is no longer readable");
        return;
    }

    std::array<std::uint8_t, kAspectPatchSize> now{};
    std::memcpy(now.data(), reinterpret_cast<const void*>(g_aspect_site), now.size());

    const bool matches_expected = now == g_aspect_expected;
    const bool matches_original = now == g_aspect_original;

    std::string bytes;
    for (const std::uint8_t byte : now) {
        bytes += std::format("{:02X} ", byte);
    }
    logger::info("safearea: patch site now reads {}", bytes);

    if (matches_expected) {
        logger::info("safearea:   -- that is what we wrote, the patch is holding");
    } else if (matches_original) {
        logger::info("safearea:   -- that is the ORIGINAL: something restored it (Arxan?).");
        logger::info("safearea:      the probe result is void, not negative.");
    } else {
        logger::info("safearea:   -- neither ours nor the original; the code moved underneath us");
    }
}

bool aspect_pretending() { return g_aspect_flat; }

int aspect_count() { return g_aspect_candidates; }

void restore() {
    if (g_flat) {
        set_flat(false);
    }
    if (g_aspect_flat) {
        set_aspect_pretend_16_9(false);
    }
}

}  // namespace safearea
