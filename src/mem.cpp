#include "mem.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <windows.h>

#include <winver.h>

namespace mem {
namespace {

std::optional<std::uint8_t> hex_byte(std::string_view token) {
    if (token.size() != 2) {
        return std::nullopt;
    }
    std::uint8_t value = 0;
    for (const char c : token) {
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= static_cast<std::uint8_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<std::uint8_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= static_cast<std::uint8_t>(c - 'A' + 10);
        } else {
            return std::nullopt;
        }
    }
    return value;
}

}  // namespace

Region main_module() {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) {
        return {};
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return {};
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return {};
    }
    return {base, nt->OptionalHeader.SizeOfImage};
}

Region section(const Region& module, std::string_view name) {
    if (!module) {
        return {};
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
    const auto* first = IMAGE_FIRST_SECTION(nt);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER& sec = first[i];
        // Section names are 8 bytes, not necessarily NUL-terminated.
        const auto* raw = reinterpret_cast<const char*>(sec.Name);
        const std::string_view sec_name(raw, ::strnlen(raw, IMAGE_SIZEOF_SHORT_NAME));
        if (sec_name == name) {
            return {module.base + sec.VirtualAddress, sec.Misc.VirtualSize};
        }
    }
    return {};
}

std::vector<NamedRegion> executable_sections(const Region& module) {
    std::vector<NamedRegion> result;
    if (!module) {
        return result;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module.base + dos->e_lfanew);
    const auto* first = IMAGE_FIRST_SECTION(nt);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER& sec = first[i];
        if ((sec.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        if (sec.Misc.VirtualSize == 0) {
            continue;
        }
        const auto* raw = reinterpret_cast<const char*>(sec.Name);
        std::string name(raw, ::strnlen(raw, IMAGE_SIZEOF_SHORT_NAME));
        if (name.empty()) {
            name = std::format("<unnamed #{}>", i + 1);
        }
        result.push_back(
            {std::move(name), Region{module.base + sec.VirtualAddress, sec.Misc.VirtualSize}});
    }
    return result;
}

std::string main_module_version() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return {};
    }

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0) {
        return {};
    }

    std::vector<std::byte> buffer(size);
    if (!GetFileVersionInfoW(path, 0, size, buffer.data())) {
        return {};
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &info_size) ||
        info == nullptr) {
        return {};
    }

    return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                       HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

std::optional<Pattern> parse_pattern(std::string_view signature) {
    Pattern pattern;

    std::size_t i = 0;
    while (i < signature.size()) {
        if (std::isspace(static_cast<unsigned char>(signature[i]))) {
            ++i;
            continue;
        }

        std::size_t end = i;
        while (end < signature.size() && !std::isspace(static_cast<unsigned char>(signature[end]))) {
            ++end;
        }
        const std::string_view token = signature.substr(i, end - i);
        i = end;

        if (token == "?" || token == "??") {
            pattern.bytes.push_back(0);
            pattern.mask.push_back(false);
            continue;
        }
        const auto value = hex_byte(token);
        if (!value) {
            return std::nullopt;
        }
        pattern.bytes.push_back(*value);
        pattern.mask.push_back(true);
    }

    if (pattern.bytes.empty()) {
        return std::nullopt;
    }
    return pattern;
}

// A packed binary is full of holes: Arxan leaves PAGE_NOACCESS and guard pages
// inside the executable sections, and blindly walking them access-violates.
// Split the region into the parts we may actually read, merging neighbours so a
// signature straddling a protection change is still found.
std::vector<Region> readable_subranges(const Region& region) {
    std::vector<Region> out;

    std::uintptr_t addr = region.base;
    while (addr < region.end()) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) {
            break;
        }
        const auto block_base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t block_end = block_base + mbi.RegionSize;

        const bool readable = mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD) == 0 &&
                              (mbi.Protect & PAGE_NOACCESS) == 0;
        if (readable) {
            const std::uintptr_t start = (std::max)(addr, block_base);
            const std::uintptr_t stop = (std::min)(region.end(), block_end);
            if (stop > start) {
                if (!out.empty() && out.back().end() == start) {
                    out.back().size += stop - start;
                } else {
                    out.push_back({start, stop - start});
                }
            }
        }

        addr = block_end > addr ? block_end : addr + 0x1000;
    }
    return out;
}

namespace {

void scan_range(const Region& region, const Pattern& pattern, std::vector<std::uintptr_t>& hits) {
    if (!region || region.size < pattern.bytes.size()) {
        return;
    }

    const auto* const data = reinterpret_cast<const std::uint8_t*>(region.base);
    const std::size_t last = region.size - pattern.bytes.size();
    const std::size_t len = pattern.bytes.size();

    // Fast path: when the signature starts with a fixed byte we can use memchr
    // to skip ahead instead of testing every offset.
    const bool anchored = pattern.mask.front();
    const std::uint8_t anchor = pattern.bytes.front();

    std::size_t offset = 0;
    while (offset <= last) {
        if (anchored) {
            const void* found = std::memchr(data + offset, anchor, last - offset + 1);
            if (found == nullptr) {
                break;
            }
            offset = static_cast<std::size_t>(static_cast<const std::uint8_t*>(found) - data);
        }

        bool match = true;
        for (std::size_t j = 1; j < len; ++j) {
            if (pattern.mask[j] && data[offset + j] != pattern.bytes[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            hits.push_back(region.base + offset);
        }
        ++offset;
    }
}

}  // namespace

void find_all(const Region& region, const Pattern& pattern, std::vector<std::uintptr_t>& out) {
    if (!region || pattern.bytes.empty()) {
        return;
    }
    for (const Region& sub : readable_subranges(region)) {
        scan_range(sub, pattern, out);
    }
}

std::vector<std::uintptr_t> find_all(const Region& region, const Pattern& pattern) {
    std::vector<std::uintptr_t> hits;
    find_all(region, pattern, hits);
    return hits;
}

std::uintptr_t find_first(const Region& region, std::string_view signature) {
    const auto pattern = parse_pattern(signature);
    if (!pattern) {
        return 0;
    }
    const auto hits = find_all(region, *pattern);
    return hits.empty() ? 0 : hits.front();
}

std::uintptr_t resolve_rip_relative(std::uintptr_t insn, std::size_t disp_offset,
                                    std::size_t insn_length) {
    if (insn == 0) {
        return 0;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, reinterpret_cast<const void*>(insn + disp_offset),
                sizeof(displacement));
    return insn + insn_length + static_cast<std::intptr_t>(displacement);
}

}  // namespace mem
