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

bool init(const std::vector<mem::NamedRegion>& sections);

// true = leave the caller's values alone (overlay fills the screen)
bool set_stretched(bool stretched);

[[nodiscard]] bool stretched();
[[nodiscard]] bool found();

// Call count and callers. If this is zero after a cutscene with the effect on
// screen, the hook is on the wrong function and no amount of looking at the
// picture would have said so.
void report(std::uintptr_t module_base);

void restore();

}  // namespace overlay
