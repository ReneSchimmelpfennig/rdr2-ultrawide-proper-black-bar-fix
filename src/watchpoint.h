#pragma once

#include <cstdint>

// Finds who writes a given address, using the CPU's debug registers.
//
// Ghidra cannot answer this: the FOV master is written through a computed
// address, so there is no static reference to follow. A data breakpoint does
// not care -- it triggers on the actual store, whatever computed it.
namespace watchpoint {

// Arms a 4-byte write watchpoint on `address` across every thread, collects the
// instruction pointers that trip it for `duration_ms`, then disarms and logs
// them grouped by frequency, as module offsets.
//
// Note on what gets logged: a data breakpoint traps *after* the store retires,
// so the reported RIP is the instruction following the write. The store itself
// ends where the report begins.
//
// This is the one step where Arxan might object -- anti-tamper commonly checks
// the debug registers. If the game crashes or the values are silently cleared,
// that is the answer, and byte-patching the writer becomes the fallback.
void find_writers(std::uintptr_t address, std::uintptr_t module_base, unsigned int duration_ms);

}  // namespace watchpoint
