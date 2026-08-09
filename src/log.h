#pragma once

#include <format>
#include <string>
#include <string_view>
#include <windows.h>

// Not `log`: <math.h> comes in via windows.h and already declares ::log.
namespace logger {

// Opens a log file, truncating it. Tries next to the .asi first and falls back
// to %LOCALAPPDATA%\RDR2UltrawideCutsceneFix\ -- the game directory only grants
// ReadAndExecute to normal users, which permits writing files that already
// exist but not creating new ones.
//
// Returns false when no location was writable. Every line also goes to
// OutputDebugString, so a debugger sees the log even then.
bool open(HMODULE self);
void close();

// Where open() ended up, for reporting. Empty if it failed.
[[nodiscard]] std::wstring path();

void write(std::string_view line);

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace logger
