// Self-test for the pattern scanner and the framing maths.
//
// The scanner runs inside a packed process where a wrong result is either an
// access violation or a silently bogus address, so it gets checked here against
// buffers we control.

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numbers>
#include <string>
#include <system_error>
#include <vector>

#include "../src/framing.h"
#include "../src/log.h"
#include "../src/mem.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    } else {
        std::printf("  ok    %s\n", what);
    }
}

void check_near(double actual, double expected, double tolerance, const char* what) {
    const bool ok = std::fabs(actual - expected) <= tolerance;
    if (!ok) {
        std::printf("  FAIL  %s (got %.6f, expected %.6f)\n", what, actual, expected);
        ++g_failures;
    } else {
        std::printf("  ok    %s (%.6f)\n", what, actual);
    }
}

mem::Region region_of(const std::vector<std::uint8_t>& buffer) {
    return {reinterpret_cast<std::uintptr_t>(buffer.data()), buffer.size()};
}

void test_pattern_parsing() {
    std::puts("pattern parsing");

    const auto simple = mem::parse_pattern("C6 05 ? ? ? ? FF");
    check(simple.has_value(), "parses a signature with wildcards");
    check(simple && simple->bytes.size() == 7, "token count");
    check(simple && simple->mask[0] && !simple->mask[2] && simple->mask[6], "wildcard mask");

    check(mem::parse_pattern("  48   8B\tC4 ").has_value(), "tolerates extra whitespace");
    check(mem::parse_pattern("??").has_value(), "'??' is a wildcard too");
    check(!mem::parse_pattern("").has_value(), "rejects an empty signature");
    check(!mem::parse_pattern("ZZ").has_value(), "rejects non-hex");
    check(!mem::parse_pattern("C6 5").has_value(), "rejects a half byte");
}

void test_scanning() {
    std::puts("scanning");

    // Two matches of "C6 05 ?? ?? ?? ?? FF", at offsets 3 and 16.
    std::vector<std::uint8_t> buffer = {
        0x90, 0x90, 0x90,                                      // 0
        0xC6, 0x05, 0x11, 0x22, 0x33, 0x44, 0xFF,              // 3  match
        0xC6, 0x05, 0xAA, 0xBB, 0xCC, 0xDD, 0x00,              // 10 near miss (last byte)
        0xC6, 0x05, 0xDE, 0xAD, 0xBE, 0xEF, 0xFF,              // 17 match
        0x90, 0x90,                                            // 24
    };

    const auto pattern = mem::parse_pattern("C6 05 ? ? ? ? FF");
    const auto hits = mem::find_all(region_of(buffer), *pattern);

    check(hits.size() == 2, "finds exactly the two real matches");
    if (hits.size() == 2) {
        const auto base = reinterpret_cast<std::uintptr_t>(buffer.data());
        check(hits[0] - base == 3, "first hit offset");
        check(hits[1] - base == 17, "second hit offset");
        check(hits[0] < hits[1], "hits are in ascending order");
    }

    // A pattern starting with a wildcard must not take the memchr fast path.
    const auto leading_wildcard = mem::parse_pattern("? 05 ? ? ? ? FF");
    check(mem::find_all(region_of(buffer), *leading_wildcard).size() == 2,
          "handles a leading wildcard");

    // Nothing to find.
    const auto absent = mem::parse_pattern("DE AD C0 DE FA CE");
    check(mem::find_all(region_of(buffer), *absent).empty(), "reports no hits for an absent pattern");

    // A pattern longer than the region must not read past the end.
    std::vector<std::uint8_t> tiny = {0xC6, 0x05};
    check(mem::find_all(region_of(tiny), *pattern).empty(), "pattern longer than the region");

    // Appending overload accumulates.
    std::vector<std::uintptr_t> accumulated;
    mem::find_all(region_of(buffer), *pattern, accumulated);
    mem::find_all(region_of(buffer), *pattern, accumulated);
    check(accumulated.size() == 4, "the appending overload accumulates across calls");
}

void test_unreadable_pages() {
    std::puts("unreadable pages");

    const SIZE_T page = 0x1000;
    auto* base = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, page * 3, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(base != nullptr, "allocated a three page scratch region");
    if (base == nullptr) {
        return;
    }

    // Put a match in page 0 and another in page 2, then make page 1 poison.
    const std::uint8_t needle[] = {0xC6, 0x05, 0x11, 0x22, 0x33, 0x44, 0xFF};
    std::memcpy(base, needle, sizeof(needle));
    std::memcpy(base + page * 2, needle, sizeof(needle));

    DWORD old = 0;
    check(VirtualProtect(base + page, page, PAGE_NOACCESS, &old) != 0, "made the middle page NOACCESS");

    const auto pattern = mem::parse_pattern("C6 05 ? ? ? ? FF");
    const mem::Region region{reinterpret_cast<std::uintptr_t>(base), page * 3};
    const auto hits = mem::find_all(region, *pattern);

    // If the scanner ignored page protection this would have crashed instead.
    check(hits.size() == 2, "scans around an unreadable page without faulting");

    VirtualFree(base, 0, MEM_RELEASE);
}

void test_rip_relative() {
    std::puts("RIP-relative resolution");

    // C6 05 <disp32> FF  ->  mov byte ptr [rip+disp], 0FFh, 7 bytes long.
    std::vector<std::uint8_t> buffer(64, 0x90);
    buffer[0] = 0xC6;
    buffer[1] = 0x05;
    const std::int32_t displacement = 0x20;
    std::memcpy(buffer.data() + 2, &displacement, sizeof(displacement));
    buffer[6] = 0xFF;

    const auto insn = reinterpret_cast<std::uintptr_t>(buffer.data());
    const auto target = mem::resolve_rip_relative(insn, 2, 7);
    check(target == insn + 7 + 0x20, "forward displacement");

    const std::int32_t backwards = -0x10;
    std::memcpy(buffer.data() + 2, &backwards, sizeof(backwards));
    check(mem::resolve_rip_relative(insn, 2, 7) == insn + 7 - 0x10, "negative displacement");

    check(mem::resolve_rip_relative(0, 2, 7) == 0, "a null instruction resolves to null");
}

void test_module_introspection() {
    std::puts("module introspection");

    const mem::Region module = mem::main_module();
    check(static_cast<bool>(module), "reads its own module headers");
    check(module.contains(reinterpret_cast<std::uintptr_t>(&test_module_introspection)),
          "the module region contains our own code");

    const mem::Region text = mem::section(module, ".text");
    check(static_cast<bool>(text), "finds .text");
    check(text.contains(reinterpret_cast<std::uintptr_t>(&test_module_introspection)),
          ".text contains our own code");
    check(mem::section(module, ".nope").base == 0, "a missing section yields an empty region");

    const auto executables = mem::executable_sections(module);
    check(!executables.empty(), "enumerates executable sections");

    // This test executable carries no VERSIONINFO resource, so the only thing
    // worth asserting here is that the absence is handled rather than crashed
    // on. The real check is the version line the plugin logs from RDR2.exe.
    const std::string version = mem::main_module_version();
    check(version.empty() || version.find('.') != std::string::npos,
          "version lookup returns either nothing or a dotted version");
    std::printf("        version string: \"%s\"\n", version.c_str());
}

void test_framing() {
    std::puts("framing maths");

    const double k = framing::correction_factor(3440, 1440);
    check_near(k, (16.0 / 9.0) / (3440.0 / 1440.0), 1e-9, "k at 3440x1440");
    check_near(k, 0.744186, 1e-5, "k matches the value from the design notes");

    check_near(framing::correction_factor(1920, 1080), 1.0, 1e-12, "16:9 needs no correction");
    check_near(framing::correction_factor(1600, 1200), 1.0, 1e-12, "4:3 is left alone");
    check(framing::correction_factor(0, 0) == 1.0, "degenerate resolution is a no-op");

    // 32:9 must clamp to the 2.4 aspect, not scale all the way down.
    const double super = framing::correction_factor(5120, 1440);
    check_near(super, (16.0 / 9.0) / framing::kMaxCorrectedAspect, 1e-9, "32:9 clamps");
    check(super > (16.0 / 9.0) / (5120.0 / 1440.0), "the clamp is less aggressive than the raw ratio");

    // The correction is in tangent space: scaling the angle directly would give
    // 60 * 0.744186 = 44.65 degrees, which is a different (wrong) number.
    const double corrected = framing::corrected_vfov_deg(60.0, k);
    check_near(corrected, 2.0 * std::atan(k * std::tan(30.0 * std::numbers::pi / 180.0)) * 180.0 /
                              std::numbers::pi,
               1e-9, "60 deg vFOV corrected");
    check(std::fabs(corrected - 60.0 * k) > 1.0, "tangent space differs from naive angle scaling");
    check_near(framing::corrected_vfov_deg(60.0, 1.0), 60.0, 1e-9, "k = 1 is the identity");

    // Deriving k from the game's own bar fraction. The constants are the values
    // measured in the running game, see docs/messungen.md.
    constexpr double kBarPerWeight = 0.127907;  // = (1 - k)/2 at 3440x1440
    check_near(framing::correction_factor_from_bars(kBarPerWeight * 1.0, 1.0), k, 1e-5,
               "k recovered from the fully letterboxed frame");
    check_near(framing::correction_factor_from_bars(kBarPerWeight * 0.25, 0.25), k, 1e-5,
               "k recovered mid-ramp, weight cancels out");
    check_near(framing::correction_factor_from_bars(0.0, 0.0), 1.0, 1e-12,
               "no letterbox on screen -> no answer, returns identity");
    check_near(framing::correction_factor_from_bars(0.0001, 0.0001), 1.0, 1e-12,
               "a weight too small to divide by is rejected rather than amplified");

    // Blending: gameplay untouched, full letterbox fully corrected.
    check_near(framing::blended_factor(k, 0.0), 1.0, 1e-12, "no letterbox -> no correction");
    check_near(framing::blended_factor(k, 1.0), k, 1e-12, "full letterbox -> full correction");
    check_near(framing::blended_factor(k, 0.5), 1.0 + (k - 1.0) * 0.5, 1e-12, "half way");
    check_near(framing::blended_factor(k, 5.0), k, 1e-12, "out of range input is clamped");
    check_near(framing::blended_factor(k, -1.0), 1.0, 1e-12, "negative input is clamped");
}

void test_logger_fallback() {
    std::puts("logger fallback");

    // A module handle that was never loaded makes GetModuleFileNameW fail, so
    // the "beside the .asi" candidate drops out and the fallback has to carry
    // it. That is exactly the situation in the game folder, which grants the
    // user ReadAndExecute only: existing files may be written, new ones may not
    // be created.
    const auto bogus = reinterpret_cast<HMODULE>(static_cast<std::uintptr_t>(0xDEAD0000));

    check(logger::open(bogus), "opens a log even when the module path is unusable");
    const std::wstring path = logger::path();
    check(!path.empty(), "reports where it ended up");

    logger::info("self-test wrote this line");
    logger::close();

    if (!path.empty()) {
        std::printf("        fell back to: %s\n", std::filesystem::path(path).string().c_str());
        check(std::filesystem::exists(path), "the fallback log really exists on disk");
        check(std::filesystem::file_size(path) > 0, "and it has content");

        wchar_t local[MAX_PATH]{};
        const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        check(written > 0 && written < MAX_PATH && path.find(local) == 0,
              "the fallback is under %LOCALAPPDATA%");

        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

}  // namespace

int main() {
    test_pattern_parsing();
    test_scanning();
    test_unreadable_pages();
    test_rip_relative();
    test_module_introspection();
    test_framing();
    test_logger_fallback();

    std::printf("\n%s\n", g_failures == 0 ? "all checks passed" : "THERE WERE FAILURES");
    return g_failures == 0 ? 0 : 1;
}
