#include "config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <string_view>

#include "log.h"

namespace config {
namespace {

constexpr const wchar_t* kFileName = L"RDR2UltrawideCutsceneFix.ini";

Settings g_settings;

std::string trim(std::string_view text) {
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    auto begin = std::find_if(text.begin(), text.end(), not_space);
    auto end = std::find_if(text.rbegin(), text.rend(), not_space).base();
    return begin < end ? std::string(begin, end) : std::string();
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Accepts what people actually write, not just what a parser would prefer.
bool parse_bool(std::string_view value, bool& out) {
    const std::string v = lowered(trim(value));
    if (v == "true" || v == "1" || v == "yes" || v == "on") {
        out = true;
        return true;
    }
    if (v == "false" || v == "0" || v == "no" || v == "off") {
        out = false;
        return true;
    }
    return false;
}

std::filesystem::path next_to_plugin(HMODULE self) {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(self, path, MAX_PATH) == 0) {
        return {};
    }
    std::filesystem::path result(path);
    result.replace_filename(kFileName);
    return result;
}

std::filesystem::path next_to_log() {
    std::filesystem::path result = logger::path();
    if (result.empty()) {
        return {};
    }
    result.replace_filename(kFileName);
    return result;
}

bool read_from(const std::filesystem::path& file, Settings& settings) {
    std::ifstream input(file);
    if (!input) {
        return false;
    }

    logger::info("config: reading {}", file.string());

    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;

        // Comments and section headers.
        const auto comment = line.find_first_of(";#");
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '[') {
            continue;
        }

        const auto equals = trimmed.find('=');
        if (equals == std::string::npos) {
            logger::info("config:   line {}: no '=', ignored: {}", line_number, trimmed);
            continue;
        }

        const std::string key = lowered(trim(std::string_view(trimmed).substr(0, equals)));
        const std::string value = trim(std::string_view(trimmed).substr(equals + 1));

        if (key == "expandcutscenessideways") {
            bool parsed = false;
            if (parse_bool(value, parsed)) {
                settings.expand_cutscenes_sideways = parsed;
                logger::info("config:   ExpandCutscenesSideways = {}", parsed ? "true" : "false");
            } else {
                logger::info("config:   line {}: '{}' is not true or false, keeping the default",
                             line_number, value);
            }
        } else if (key == "removeallblackbars") {
            bool parsed = false;
            if (parse_bool(value, parsed)) {
                settings.remove_all_black_bars = parsed;
                logger::info("config:   RemoveAllBlackBars = {}", parsed ? "true" : "false");
            } else {
                logger::info("config:   line {}: '{}' is not true or false, keeping the default",
                             line_number, value);
            }
        } else {
            logger::info("config:   line {}: unknown setting '{}', ignored", line_number, key);
        }
    }
    return true;
}

}  // namespace

Settings load(HMODULE self) {
    Settings settings;  // defaults are the recommended values

    const std::filesystem::path beside_plugin = next_to_plugin(self);
    if (!beside_plugin.empty() && read_from(beside_plugin, settings)) {
        g_settings = settings;
        return settings;
    }
    if (!beside_plugin.empty()) {
        logger::info("config: no file at {}", beside_plugin.string());
    }

    const std::filesystem::path beside_log = next_to_log();
    if (!beside_log.empty() && read_from(beside_log, settings)) {
        g_settings = settings;
        return settings;
    }
    if (!beside_log.empty()) {
        logger::info("config: no file at {}", beside_log.string());
    }

    logger::info("config: no ini found -- using the recommended defaults");
    logger::info("config:   ExpandCutscenesSideways = false");
    g_settings = settings;
    return settings;
}

const Settings& current() { return g_settings; }

}  // namespace config
