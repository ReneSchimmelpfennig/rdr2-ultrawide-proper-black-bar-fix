#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

// The whole fix in one place.
//
// RDR2's cutscene cameras are Hor+: vertical FOV is authored once for 16:9 and
// held constant, so a wider display widens the horizontal FOV instead of
// cropping it. Removing the letterbox alone therefore reveals more of the scene
// than the shot was framed for. To keep the original framing we shrink the
// vertical FOV by the aspect ratio difference -- but in tangent space, not in
// degrees.
namespace framing {

inline constexpr double kReferenceAspect = 16.0 / 9.0;

// Above roughly 21:9 the correction starts cropping harder than it gains, and
// 32:9 ends up noticeably zoomed in. Clamp the aspect the correction is derived
// from; 2.4 covers 3440x1440 (2.3889) with a little headroom.
inline constexpr double kMaxCorrectedAspect = 2.4;

// Tangent-space scalar. NOTE: this multiplies tan(fov/2), never the angle.
[[nodiscard]] inline double correction_factor(int width, int height) {
    if (width <= 0 || height <= 0) {
        return 1.0;
    }
    const double aspect = static_cast<double>(width) / static_cast<double>(height);
    if (aspect <= kReferenceAspect) {
        return 1.0;  // 16:9 or narrower: nothing to correct.
    }
    return kReferenceAspect / std::min(aspect, kMaxCorrectedAspect);
}

// The game computes the same quantity we do. Its letterbox struct carries a
// float equal to weight * (1 - k)/2 -- the bar height for the real display
// aspect. Inverting that yields k straight from the game's own view of the
// backbuffer, which beats GetSystemMetrics: it is already correct in windowed
// mode, at non-native resolutions and with render scaling.
//
// Only meaningful while a letterbox is on screen; returns 1.0 when the weight
// is too small to divide by. Measured agreement with correction_factor() on
// 3440x1440 was exact to six decimals.
[[nodiscard]] inline double correction_factor_from_bars(double bar_fraction_display,
                                                        double weight) {
    if (weight < 1e-3) {
        return 1.0;
    }
    return 1.0 - 2.0 * (bar_fraction_display / weight);
}

// vFOV_new = 2 * atan(k * tan(vFOV_old / 2))
[[nodiscard]] inline double corrected_vfov_rad(double vfov_rad, double k) {
    return 2.0 * std::atan(k * std::tan(vfov_rad * 0.5));
}

[[nodiscard]] inline double corrected_vfov_deg(double vfov_deg, double k) {
    constexpr double kDegToRad = std::numbers::pi / 180.0;
    constexpr double kRadToDeg = 180.0 / std::numbers::pi;
    return corrected_vfov_rad(vfov_deg * kDegToRad, k) * kRadToDeg;
}

// The letterbox amount doubles as the blend weight: 0 = no bars (gameplay,
// leave the FOV alone), 1 = fully letterboxed (apply the full correction).
// Interpolating k rather than the resulting FOV keeps the transition smooth.
// How fast the correction follows the bars.
//
// Tying it one-to-one to the letterbox amount is the obvious choice and it reads
// badly on the cinematic camera: the bars ease in over about 1.3 s, so the
// framing only arrives at the end, and since the player pressed the button
// themselves it feels like lag rather than like a transition. In a cutscene the
// same curve is pleasant, because nobody is waiting for it.
//
// Reaching full correction at 40% of the fade keeps the movement smooth -- it is
// still an ease, just a shorter one -- while the framing settles early enough to
// feel immediate. The remaining 60% of the fade then has a stable field of view,
// which is also the part where the game cuts.
inline constexpr double kCorrectionReachesFullAt = 0.4;

[[nodiscard]] inline double blended_factor(double k, double letterbox_amount) {
    const double t = std::clamp(letterbox_amount / kCorrectionReachesFullAt, 0.0, 1.0);
    return 1.0 + (k - 1.0) * t;
}

}  // namespace framing
