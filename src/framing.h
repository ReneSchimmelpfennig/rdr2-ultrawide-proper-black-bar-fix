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
//
// CAVEAT: this clamp only applies to correction_factor(), which is the fallback.
// The path that actually runs is correction_factor_from_bars(), which has no
// clamp -- so on 32:9 the correction is currently about twice as strong as
// intended. Nothing is done about that yet on purpose; see kUltrawideThreshold.
inline constexpr double kMaxCorrectedAspect = 2.4;

// Where "wider than 21:9" begins.
//
// 3440x1440 is 2.3889 and must stay on the narrow side of this with room to
// spare, because the behaviour there is finished and must not change. 2.45 is
// comfortably above it and comfortably below 32:9 (3.5556).
//
// Everything to do with wider displays hangs off this test, so that a 21:9
// screen never enters any of it.
inline constexpr double kUltrawideThreshold = 2.45;

// The display aspect, recovered from the correction factor.
//
// k = (16/9) / aspect by construction, and k comes from the bar height the game
// computes from its actual backbuffer -- so this is correct in windowed mode and
// at non-native resolutions, where GetSystemMetrics is not.
[[nodiscard]] inline double aspect_from_correction(double k) {
    return k > 1e-6 ? kReferenceAspect / k : kReferenceAspect;
}

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
// How fast the correction follows the bars. 1.0 = exactly with them.
//
// This was briefly 0.4, on my reading that the cinematic camera felt laggy
// because the correction trailed the bars. That reading was wrong: what looked
// like lag was a single uncorrected frame at the cut into the shot, and it is
// fixed where it belongs -- in the focal-length clamp, which sees a new camera a
// frame before the shader constant does.
//
// Shortening the ramp therefore fixed nothing and cost something: the full-length
// ease follows the game's own letterbox animation and simply looks better.
inline constexpr double kCorrectionReachesFullAt = 1.0;

[[nodiscard]] inline double blended_factor(double k, double letterbox_amount) {
    const double t = std::clamp(letterbox_amount / kCorrectionReachesFullAt, 0.0, 1.0);
    return 1.0 + (k - 1.0) * t;
}

}  // namespace framing
