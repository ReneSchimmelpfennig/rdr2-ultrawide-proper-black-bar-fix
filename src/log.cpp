#include "log.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <vector>

namespace logger {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
std::wstring g_path;

// GetEnvironmentVariableW rather than _wgetenv: no CRT deprecation warning, and
// no dependence on the CRT's copy of the environment block.
std::wstring environment_variable(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    value.resize(written);
    return value;
}

// Always %LOCALAPPDATA%\RDR2UltrawideCutsceneFix\, never the game folder.
//
// It used to try beside the .asi first, which seemed friendlier. Two problems
// came of that. The game folder is normally read-only, so it silently fell
// through -- until it became writable, at which point the log moved and every
// path derived from it moved with it. The plugin then looked for its image dump
// in the new place, did not find it, and took the "no dump yet" branch: no FOV
// hook, no bar removal, no sign of anything wrong. The fix appeared to have
// stopped working.
//
// Writing into the game directory is also something this project does not do
// without being asked, and a log is not an exception.
std::vector<std::filesystem::path> candidate_paths(HMODULE) {
    std::vector<std::filesystem::path> candidates;

    if (const std::wstring local = environment_variable(L"LOCALAPPDATA"); !local.empty()) {
        std::filesystem::path dir(local);
        dir /= L"RDR2UltrawideCutsceneFix";

        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (!ec) {
            candidates.push_back(dir / L"plugin.log");
        }
    }

    // Last resort only: somewhere writable is better than nowhere.
    if (const std::wstring temp = environment_variable(L"TEMP"); !temp.empty()) {
        candidates.push_back(std::filesystem::path(temp) / L"RDR2UltrawideCutsceneFix.log");
    }

    return candidates;
}

}  // namespace

bool open(HMODULE self) {
    std::lock_guard lock(g_mutex);

    for (const auto& candidate : candidate_paths(self)) {
        g_file.open(candidate, std::ios::out | std::ios::trunc);
        if (g_file.is_open()) {
            g_path = candidate.wstring();
            OutputDebugStringW(L"[RDR2UltrawideCutsceneFix] logging to ");
            OutputDebugStringW(g_path.c_str());
            OutputDebugStringW(L"\n");
            return true;
        }
        g_file.clear();
    }

    OutputDebugStringW(L"[RDR2UltrawideCutsceneFix] no writable log location\n");
    return false;
}

std::wstring path() {
    std::lock_guard lock(g_mutex);
    return g_path;
}

void close() {
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) {
        g_file.flush();
        g_file.close();
    }
}

void write(std::string_view line) {
    // GetLocalTime rather than std::chrono::current_zone(): no tzdata lookup,
    // which we cannot rely on inside a foreign process with a static CRT.
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const std::string stamped = std::format("[{:02}:{:02}:{:02}.{:03}] {}\n", t.wHour, t.wMinute,
                                            t.wSecond, t.wMilliseconds, line);

    // Always mirror to the debugger. If every file location was unwritable this
    // is the only way anything is ever seen, and diagnosing "no output at all"
    // from outside the process is otherwise guesswork.
    OutputDebugStringA(stamped.c_str());

    std::lock_guard lock(g_mutex);
    if (!g_file.is_open()) {
        return;
    }
    // Flush every line: if the game crashes, the last line is the interesting one.
    g_file << stamped << std::flush;
}

}  // namespace logger
