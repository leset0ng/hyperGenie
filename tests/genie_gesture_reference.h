// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "genie_model.h"

#include <algorithm>
#include <cmath>

// Recovered gesture configuration retained for numerical reference tests only.
// Production SF velocities have not been mapped to these UIKit units.
namespace genie::model::reference {

struct SpringSettings {
    double response;
    double damping_ratio;
};

struct GestureSettings {
    SpringSettings layout;
    SpringSettings position;
    SpringSettings scale;
};

// Weight is the caller's frame-derived t, not clamped progress. Velocity must
// already use the source modifier's units; it is not injected into any scalar.
// Invalid results leave *result intact.
inline bool SettingsForGesture(double weight, Point velocity, GestureSettings* result) {
    constexpr double kPi = 3.14159265358979323846;
    auto Positive = [](double value) { return std::isfinite(value) && value > 0.0; };
    if (result == nullptr || !std::isfinite(weight) ||
        !std::isfinite(velocity.x) || !std::isfinite(velocity.y)) return false;
    const double y_weight = std::clamp((-velocity.y - 3000.0) / 2000.0, 0.0, 1.0);
    const double x_weight = std::clamp((std::abs(velocity.x) - 1000.0) / 3000.0, 0.0, 1.0);
    const GestureSettings candidate{
        {(0.425 + (0.375 - 0.425) * weight) * 0.8 * (1.0 - 0.3 * y_weight), 1.1},
        {(0.45 + (0.55 - 0.45) * weight) * 0.8 * (1.0 + 0.2 * x_weight),
         0.8 + (0.86 - 0.8) * weight},
        {0.55 * 0.8, 0.84},
    };
    for (const auto& settings : {candidate.layout, candidate.position, candidate.scale}) {
        if (!Positive(settings.response) || !Positive(settings.damping_ratio) ||
            !Positive(2.0 * kPi / settings.response)) return false;
    }
    *result = candidate;
    return true;
}

}  // namespace genie::model::reference
