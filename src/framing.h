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
[[nodiscard]] inline double blended_factor(double k, double letterbox_amount) {
    const double t = std::clamp(letterbox_amount, 0.0, 1.0);
    return 1.0 + (k - 1.0) * t;
}

}  // namespace framing
