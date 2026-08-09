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

// Beside the .asi, then %LOCALAPPDATA%\RDR2UltrawideCutsceneFix\. The first is
// nicer to find, the second is the one that actually works when the game folder
// is read-only for the user the game runs as.
std::vector<std::filesystem::path> candidate_paths(HMODULE self) {
    std::vector<std::filesystem::path> candidates;

    wchar_t module_path[MAX_PATH]{};
    if (GetModuleFileNameW(self, module_path, MAX_PATH) != 0) {
        std::filesystem::path beside(module_path);
        beside.replace_extension(L".log");
        candidates.push_back(std::move(beside));
    }

    if (const std::wstring local = environment_variable(L"LOCALAPPDATA"); !local.empty()) {
        std::filesystem::path fallback(local);
        fallback /= L"RDR2UltrawideCutsceneFix";

        std::error_code ec;
        std::filesystem::create_directories(fallback, ec);
        if (!ec) {
            candidates.push_back(fallback / L"plugin.log");
        }
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
