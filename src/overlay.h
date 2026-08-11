#pragma once

#include <cstdint>
#include <vector>

#include "mem.h"

// Stretches the cutscene's full-screen overlays across the whole screen.
//
// This is the first candidate in the 2D investigation that arrived with numbers
// attached rather than a plausible story. RenderDoc's uniforms for the intro
// filter carry 1.343750 and -0.171875, which is exactly what
// `patterns::kOverlayFit` returns for the inputs (1.0, 0.0). The shader samples
// a 1920x1080 texture -- 16:9 -- with `uv.x * 1.34375 - 0.171875`, so the
// texture's left edge lands at x = 0.127907 and everything outside runs past
// it. A clamping sampler repeats the edge column there, which is the horizontal
// smearing in the strip the bars used to hide.
//
// Neutralised, the caller's untouched 1.0 and 0.0 reach the shader and the
// overlay covers the full screen. For grain, vignette and the photographic
// filter that means a 34% horizontal stretch of an effect layer, which is not
// something the eye can catch -- the smearing is.
//
// Hooked rather than byte-patched on purpose. Three earlier attempts patched a
// byte, saw no change, and could not tell "irrelevant" from "never ran". The
// call counter answers that first.
namespace overlay {

// A 16:9 asset on a 21:9 screen cannot be shown whole, undistorted and
// full-width at once. Something has to give, and which of the three is least
// objectionable is a judgement about pixels, not a calculation.
enum class Mode {
    // What the game does: the asset sits in the middle 74% of the width and the
    // sampler smears its edge columns across the rest.
    Fitted,
    // Full width, 34% wider than authored. Invisible on grain and vignette,
    // visible on text and faces.
    Stretched,
    // Proportions kept: scaled up until it covers the width, cropping 25% of
    // the height. The same trade the field-of-view correction makes.
    Cover,
};

bool init(const std::vector<mem::NamedRegion>& sections);

bool set_mode(Mode mode);

[[nodiscard]] Mode mode();
[[nodiscard]] const char* mode_name();
[[nodiscard]] bool found();

// Call count and callers. If this is zero after a cutscene with the effect on
// screen, the hook is on the wrong function and no amount of looking at the
// picture would have said so.
void report(std::uintptr_t module_base);

void restore();

}  // namespace overlay
