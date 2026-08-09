#include "log.h"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace logger {
namespace {

std::mutex g_mutex;
std::ofstream g_file;

}  // namespace

void open(HMODULE self) {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(self, path, MAX_PATH) == 0) {
        return;
    }

    std::filesystem::path log_path(path);
    log_path.replace_extension(L".log");

    std::lock_guard lock(g_mutex);
    g_file.open(log_path, std::ios::out | std::ios::trunc);
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

    std::lock_guard lock(g_mutex);
    if (!g_file.is_open()) {
        return;
    }
    // Flush every line: if the game crashes, the last line is the interesting one.
    g_file << std::format("[{:02}:{:02}:{:02}.{:03}] ", t.wHour, t.wMinute, t.wSecond,
                          t.wMilliseconds)
           << line << '\n'
           << std::flush;
}

}  // namespace logger
