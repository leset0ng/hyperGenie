// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>

namespace hha::genie {

struct Point {
    double x;
    double y;
};

struct Parameters {
    double width = 1.0;
    double height = 1.0;
    double minimized_width = 1.0;
    double minimized_height = 1.0;
    double minimized_scale = 1.0;
    double genie_scale = 0.4;
    double icon_x = 0.5;
    double icon_y = 0.5;
    double window_x = 0.5;
    double window_y = 0.5;
    double outset_x = 0.0;
    double outset_y = 0.0;
    // G for the opening (active=false) endpoint. Closing reverses its sign.
    double genie_amount_x = 0.0;
    double genie_amount_y = -0.8;
    double response = 0.24;
    double damping_ratio = 1.0;
    double multiplier = 0.5;
};

struct Scalar {
    double value = 0.0;
    double velocity = 0.0;
    double target = 0.0;
    double response = 0.24;
};

// Recovered Genie geometry plus an analytical oscillator model. The model
// preserves all 19 positions and velocities on Retarget; it does not claim to
// reproduce UIKit's unverified seed/retarget/projection rules.
class Animation {
public:
    // Invalid parameters leave an existing animation untouched. Reset seeds the
    // selected endpoint at rest; call Retarget(false) to open from minimized.
    bool Reset(const Parameters& parameters, bool minimized = true);
    bool Retarget(bool minimized);
    // Advances the full elapsed interval without a fixed-step or frame cap.
    // Nonpositive/nonfinite intervals and nonfinite numerical results are ignored.
    void Advance(double delta_seconds);
    // Row-major 3x3 destinations: localPosition.x / width, localPosition.y /
    // height, including the container's -outset/2 offset. These are NOT screen
    // coordinates or CAMeshTransform's normalization by (D+O). Parent transforms,
    // source-coordinate adaptation, clipping and corner radii belong to callers.
    // An uninitialized or numerically degenerate mesh returns the identity grid.
    std::array<Point, 9> Mesh() const;
    bool Settled() const;

    // X00,Y00,...,X22,Y22,p; values are property-space, before B/K/T compensation.
    const std::array<Scalar, 19>& Scalars() const { return scalars_; }

private:
    bool ConfigureEndpoint(bool minimized, std::array<Scalar, 19>* result) const;

    Parameters parameters_{};
    std::array<Scalar, 19> scalars_{};
    bool initialized_ = false;
};

}  // namespace hha::genie
