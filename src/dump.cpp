#include "dump.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>
#include <windows.h>

namespace dump {
namespace {

constexpr std::size_t kChunk = 0x10000;

// Copies [addr, addr+len) into dest, zero-filling anything not readable.
// Returns how many bytes had to be zero-filled.
//
// A packed image is full of holes -- Arxan leaves PAGE_NOACCESS and guard pages
// inside the sections -- so this must never simply memcpy the whole range.
std::size_t copy_readable(std::uintptr_t addr, std::uint8_t* dest, std::size_t len) {
    std::memset(dest, 0, len);

    std::size_t missing = 0;
    std::size_t offset = 0;
    while (offset < len) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr + offset), &mbi, sizeof(mbi)) == 0) {
            missing += len - offset;
            break;
        }

        const auto block_base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const std::uintptr_t block_end = block_base + mbi.RegionSize;
        const std::size_t available =
            static_cast<std::size_t>(block_end - (addr + offset));
        const std::size_t take = (std::min)(available, len - offset);
        if (take == 0) {
            missing += len - offset;
            break;
        }

        const bool readable = mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD) == 0 &&
                              (mbi.Protect & PAGE_NOACCESS) == 0;
        if (readable) {
            std::memcpy(dest + offset, reinterpret_cast<const void*>(addr + offset), take);
        } else {
            missing += take;  // already zeroed above
        }
        offset += take;
    }
    return missing;
}

// Rewrites the section table in a copy of the headers so that file offsets and
// RVAs coincide. `image_size` is how many bytes the dump will actually contain.
// Returns false if the headers do not look like a PE.
bool realign_headers(std::uint8_t* headers, std::size_t size, std::size_t image_size) {
    if (size < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(headers);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    if (dos->e_lfanew < 0 || static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) >
                                 size) {
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(headers + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const DWORD alignment = nt->OptionalHeader.SectionAlignment;
    if (alignment == 0) {
        return false;
    }
    nt->OptionalHeader.FileAlignment = alignment;

    auto* section = IMAGE_FIRST_SECTION(nt);
    const auto section_table_end =
        reinterpret_cast<std::uint8_t*>(section + nt->FileHeader.NumberOfSections);
    if (section_table_end > headers + size) {
        return false;
    }

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const DWORD rva = section[i].VirtualAddress;
        const DWORD virtual_size = section[i].Misc.VirtualSize;
        DWORD rounded = (virtual_size + alignment - 1) / alignment * alignment;

        // SizeOfImage is not necessarily a multiple of SectionAlignment, so
        // rounding the last section up can claim bytes past the end of the
        // dump. A PE loader that trusts the header then reads past EOF --
        // Ghidra reports a truncated section and may refuse the file. Clamp.
        if (rva >= image_size) {
            rounded = 0;
        } else if (static_cast<std::size_t>(rva) + rounded > image_size) {
            rounded = static_cast<DWORD>(image_size - rva);
        }

        section[i].PointerToRawData = rva;
        section[i].SizeOfRawData = rounded;
    }
    return true;
}

}  // namespace

Result write_image(const mem::Region& module, const std::filesystem::path& out) {
    Result result;
    if (!module) {
        return result;
    }

    LARGE_INTEGER start{}, stop{}, freq{};
    QueryPerformanceCounter(&start);

    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);

    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    if (!file) {
        return result;
    }

    std::vector<std::uint8_t> buffer(kChunk);

    // First chunk carries the headers and is the only one that gets edited.
    const std::size_t first = (std::min)(kChunk, module.size);
    result.unreadable_bytes += copy_readable(module.base, buffer.data(), first);
    if (!realign_headers(buffer.data(), first, module.size)) {
        return result;  // not a PE we understand; better to write nothing
    }
    file.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(first));
    result.bytes_written += first;

    for (std::size_t offset = first; offset < module.size; offset += kChunk) {
        const std::size_t len = (std::min)(kChunk, module.size - offset);
        result.unreadable_bytes += copy_readable(module.base + offset, buffer.data(), len);
        file.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(len));
        if (!file) {
            return result;
        }
        result.bytes_written += len;
    }

    file.flush();
    result.ok = static_cast<bool>(file);
    file.close();

    QueryPerformanceCounter(&stop);
    QueryPerformanceFrequency(&freq);
    result.seconds =
        static_cast<double>(stop.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
    return result;
}

}  // namespace dump
