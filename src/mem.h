#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mem {

struct Region {
    std::uintptr_t base = 0;
    std::size_t size = 0;

    [[nodiscard]] std::uintptr_t end() const { return base + size; }
    [[nodiscard]] bool contains(std::uintptr_t addr) const {
        return addr >= base && addr < end();
    }
    [[nodiscard]] explicit operator bool() const { return base != 0 && size != 0; }
};

// The process' main module (RDR2.exe when we are loaded correctly).
Region main_module();

// A named PE section of `module`, e.g. ".text". Empty region if not found.
// Careful: RDR2.exe ships TWO sections called ".text" (Arxan), so this returns
// the first one only. Prefer executable_sections() for scanning.
Region section(const Region& module, std::string_view name);

struct NamedRegion {
    std::string name;
    Region region;
};

// Every section with IMAGE_SCN_MEM_EXECUTE, in header order.
std::vector<NamedRegion> executable_sections(const Region& module);

// File version of the main module, as "1.0.1491.50". Empty if unavailable.
std::string main_module_version();

// IDA-style signature: "C6 05 ? ? ? ? FF 0F 28 74 24 60".
// Accepts '?' and '??' as wildcards; whitespace between tokens is required.
struct Pattern {
    std::vector<std::uint8_t> bytes;
    std::vector<bool> mask;  // true = compare this byte
};

// Returns nullopt when the signature string is malformed.
std::optional<Pattern> parse_pattern(std::string_view signature);

// All matches of `pattern` inside `region`, in ascending address order.
// Unreadable pages inside the region are skipped rather than faulted on.
std::vector<std::uintptr_t> find_all(const Region& region, const Pattern& pattern);

// Same, appending to `out` -- for scanning several regions into one result.
void find_all(const Region& region, const Pattern& pattern, std::vector<std::uintptr_t>& out);

// Convenience: first match, or 0.
std::uintptr_t find_first(const Region& region, std::string_view signature);

// Resolves the target of a RIP-relative instruction. `insn` points at the first
// byte of the instruction, `disp_offset` at its 4-byte displacement field, and
// `insn_length` is the full instruction length.
//
//   C6 05 xx xx xx xx FF   ->  mov byte ptr [rip+disp], 0FFh
//                              disp_offset = 2, insn_length = 7
std::uintptr_t resolve_rip_relative(std::uintptr_t insn, std::size_t disp_offset,
                                    std::size_t insn_length);

}  // namespace mem
