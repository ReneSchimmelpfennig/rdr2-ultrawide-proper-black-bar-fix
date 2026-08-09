# RDR2.exe is packed -- what that means for AOB scanning

Established on 2026-08-09 against `RDR2.exe` 1.0.1491.50 (85.41 MB).

## The finding

Neither of the two signatures extracted from `RDR2NoBlackBars.asi` occurs in the
file on disk, not even once:

| Signature | Hits in the file |
|---|---|
| `C6 05 ? ? ? ? FF 0F 28 74 24 60` | 0 |
| `48 8B C4 48 89 58 08 56 57 41 56 48 81 EC C0 00 00 00 0F 29 70 D8` | 0 |

This is not a scanner bug. Control test with `48 89 5C 24 08` (`mov [rsp+8],
rbx`, one of the most common x64 prologues there is): **15 hits** in 85 MB. A
normal MSVC binary of that size has tens of thousands.

Then the section table -- eleven entries, among them **two** called `.text` and
two with no name at all:

| # | Name | RVA | Virtual size |
|---|---|---|---|
| 1 | `.text` | 0x1000 | 0x32D7000 (~51 MB) |
| 2 | `.rdata` | 0x32D8000 | 0x63E400 |
| 3 | `.data` | 0x3917000 | 0x239A6F8 |
| 4 | `.pdata` | 0x5CB2000 | 0x231E00 |
| 5 | *(unnamed)* | 0x5EE4000 | 0x200 |
| 6 | `_RDATA` | 0x5EE5000 | 0xA000 |
| 7 | *(unnamed)* | 0x5EEF000 | 0x200 |
| 8 | `.rsrc` | 0x5EF0000 | 0x61800 |
| 9 | `.reloc` | 0x5F52000 | 0xDBE00 |
| 10 | `.idata` | 0x602E000 | 0x4C00 |
| 11 | `.text` | 0x6033000 | 0x1364600 (~20 MB) |

That is the typical Arxan signature: the actual code sits encrypted in the file
and is only unpacked at runtime.

## Consequences for the plugin

1. **Scan memory only, never the file.** This is exactly why the existing
   `RDR2NoBlackBars.asi` uses `K32GetModuleInformation` and not
   `CreateFileMapping`.

2. **Scan every executable section, not `.text`.** A `GetSection(".text")` only
   catches the first of the two. `mem::executable_sections()` goes by
   `IMAGE_SCN_MEM_EXECUTE` and therefore also covers the unnamed ones.

3. **The scan has to be retried.** At `DllMain` time the code may not be
   decrypted yet. `scan_until_found()` tries every 2 s, up to 60 times. The log
   records which attempt found the signature -- that number is the interesting
   measurement on the first run in the game. (In practice: attempt 1.)

4. **Unreadable pages are normal.** Arxan leaves `PAGE_NOACCESS` and guard pages
   inside the executable sections. `mem::find_all()` therefore asks via
   `VirtualQuery` and skips them instead of producing an access violation.
   Adjacent readable ranges are merged so a signature straddling a protection
   boundary is still found. The self-test covers exactly this case
   (`test_unreadable_pages`).

## Consequence for static analysis: the dumper

Pointing Ghidra at the file on disk is pointless -- at `+0x320545` there is
garbage. So the plugin writes the decrypted image out itself (`src/dump.*`), from
the process it is running in anyway. No debugger, and therefore no risk of waking
Arxan's anti-debug.

**When:** once, after a complete cutscene has been observed. The timing is
deliberate -- that way the cutscene code is guaranteed to be decrypted and
resident. If the file already exists it is not rewritten; delete it to force a
fresh dump.

**Where:** `%LOCALAPPDATA%\RDR2UltrawideCutsceneFix\RDR2.dump.exe`, about 115 MB.
It does not belong in the repository -- `dumps/` and `*.exe` are in `.gitignore`.

**What the dump can do:** it is *realigned*. Every section header gets
`PointerToRawData = VirtualAddress`, and `FileAlignment` is raised to
`SectionAlignment`. File offsets therefore equal RVAs: what the log reports as
`module +0x320545` sits at offset `0x320545` in the dump. The self-test verifies
exactly this property by comparing one of its own functions through both paths.

**What the dump cannot do:** the import table is *not* rebuilt. The IAT holds
resolved addresses from the dumping process, so imported calls appear in Ghidra
as bare pointers rather than named functions. For our purpose -- reading code and
following data references -- that is enough. Named imports would need
ImpRec-style rework.

Unreadable pages (the Arxan holes) are written as zeros and counted in the log.
In the first real dump there were **0 bytes** -- the image was fully readable in
memory.

### Validating the dump (2026-08-09)

| Check | Result |
|---|---|
| Size | 121,206,272 bytes = `SizeOfImage`, 0 unreadable |
| Letterbox anchor | exactly at offset `0x320545`, as in the log |
| Unknown prologue | exactly at offset `0x57A458`, as in the log |
| Control pattern `48 89 5C 24 08` | **67,455** hits (file on disk: 15) |
| RIP resolution recomputed | `disp32 = 0x3654C68`, target `0x39751B4` ✓ |

The jump from 15 to 67,455 hits on the control pattern is the proof that the dump
is decrypted.

### Pitfall: SizeOfImage is not section-aligned

`SizeOfImage` of RDR2.exe is `0x7397600` -- **not** a multiple of
`SectionAlignment`. Rounding the last section's `SizeOfRawData` up blindly makes
it claim `0xA00` bytes past the end of the file, and Ghidra then reports a
truncated section. `realign_headers()` therefore clamps against the actual dump
size.

The self-test checks the invariant but **does not reproduce the bug**:
`scanner_test.exe` has a page-aligned `SizeOfImage`, so there the rounding
coincides with the end of the file. The bug was found against the real file, not
in the test.

### Addresses inside the dump

`ImageBase` in the dump is `0x7FF6750A0000` -- the Windows loader writes the
actual load address into the mapped header, not the preferred `0x140000000`.
Ghidra therefore maps the dump exactly there, and the absolute addresses from
that run's log apply directly:

| Item | Ghidra address |
|---|---|
| Letterbox anchor (the store) | `0x7FF6753C0545` |
| Letterbox struct (anchor byte) | `0x7FF678A151B4` |
| Weight (anchor − 8) | `0x7FF678A151AC` |
| Unknown prologue | `0x7FF67561A458` |

A new dump shifts the base through ASLR -- the *offsets* stay.

## Settled: Arxan tolerates detours

The open question used to be whether Arxan runs integrity checks over the
unpacked code, which would make a MinHook trampoline risky. It does not, at least
not in the regions we touch: the first real hook ran without a crash, without
being silently reverted, and the plugin kept working afterwards. A hardware
watchpoint on 19 threads was equally undisturbed.
