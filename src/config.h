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

    // Whether the second bar drawer is suppressed everywhere or only in
    // cutscenes.
    //
    // It is the function the reference mod kills outright with a `ret`, and it
    // is what puts bars on the pause menu, shops and photo mode as well as in
    // the scene after the intro video. Suppressing it only while the letterbox
    // weight is above zero leaves the menus exactly as the game intends them.
    //
    // true removes them everywhere. That is what the reference mod does, and it
    // has been in wide use since December 2024, so it is not dangerous -- but its
    // own description warns that it does not fix the screen effects behind those
    // bars. Those effects are scissored to the 16:9 window, and where the bar was
    // you can then see the unprocessed strip. The same artefact this plugin had
    // to solve for the intro overlay.
    bool remove_all_black_bars = false;
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
