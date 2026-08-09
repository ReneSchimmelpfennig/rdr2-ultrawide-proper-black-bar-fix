#pragma once

#include <format>
#include <string_view>
#include <windows.h>

// Not `log`: <math.h> comes in via windows.h and already declares ::log.
namespace logger {

// Opens <directory of this .asi>/<module name>.log, truncating it. Safe to call
// once from the worker thread; every write afterwards is serialised internally.
void open(HMODULE self);
void close();

void write(std::string_view line);

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace logger
