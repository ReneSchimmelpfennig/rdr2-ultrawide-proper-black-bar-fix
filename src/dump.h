#pragma once

#include <cstddef>
#include <filesystem>

#include "mem.h"

// Writes the decrypted image out of the running process, so it can be analysed
// statically. RDR2.exe on disk is Arxan-encrypted and useless in a
// disassembler; the copy in memory is not.
namespace dump {

struct Result {
    bool ok = false;
    std::size_t bytes_written = 0;
    std::size_t unreadable_bytes = 0;  // holes, written as zeros
    double seconds = 0.0;
};

// The dump is realigned: every section header gets PointerToRawData set to its
// VirtualAddress and FileAlignment is raised to SectionAlignment, so file
// offsets equal RVAs. A disassembler then maps the file exactly as the loader
// mapped it, and an address logged by the plugin as "module +0x320545" is at
// file offset 0x320545.
//
// What this does NOT do is rebuild the import table. The IAT in the dump holds
// resolved addresses from this process, so imported calls show up as raw
// pointers rather than named functions. That is fine for following code and
// data references, which is what we need it for.
Result write_image(const mem::Region& module, const std::filesystem::path& out);

}  // namespace dump
