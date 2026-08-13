#pragma once

#include <windows.h>

#include <filesystem>

// Settings read from RDR2UltrawideCutsceneFix.ini.
//
// Everything in here concerns displays wider than 21:9 only. On 21:9 and
// narrower nothing below is consulted at all -- see framing::kUltrawideThreshold
// -- so a missing or malformed file cannot change the behaviour anyone is
// currently getting.
namespace config {

struct Settings {
    // Behaviour for aspect ratios wider than 21:9.
    //
    // false (the recommended default): black bars on the left and right, the
    // intended composition kept intact.
    //
    // true: expand the cutscene sideways instead, which shows scene the shot was
    // never framed to include.
    bool expand_cutscenes_sideways = false;
};

// Looks next to the plugin first, because that is where people expect a mod's
// ini to be, and falls back to the log directory, which the game process can
// certainly read. A missing file is not an error: the defaults are the
// recommended settings.
//
// Logs the path it used, every setting it understood, and every line it did not.
// After the fov.txt episode -- where a file appeared to exist, was looked for in
// the right place, and still could not be found because it had been written into
// a sandbox -- nothing here is assumed to work silently.
Settings load(HMODULE self);

[[nodiscard]] const Settings& current();

}  // namespace config
