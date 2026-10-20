// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>

namespace hha::genie {

struct Point {
    double x;
    double y;
};

struct Rect {
    double left;
    double top;
    double width;
    double height;
};

constexpr double kOpeningResponse = 0.24;
constexpr double kClosingResponse = 0.336;

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
    double response = kOpeningResponse;
    double damping_ratio = 1.0;
    double multiplier = 0.5;
};

// Ordinary iPhone/icon branch: icon and window are in the same screen units;
// velocity uses those units/second. The resulting I and W share window-local
// coordinates. pixel_scale is pixels per input unit (use 1 for pixel rectangles).
// d preserves the icon's aspect ratio and follows 0x2fc168-0x2fc1d8's reference
// size selection; it is neither the raw icon size nor an App-aspect fit.
// Invalid/unsupported geometry leaves *result unchanged. This branch uses the
// ordinary 2.5 counter-scale; widgets, pad and flexible windows need other rules.
bool ParametersForIcon(const Rect& icon, const Rect& window, Parameters* result,
                       Point velocity = {0.0, 0.0}, double pixel_scale = 1.0);

struct SpringSettings {
    double response;
    double damping_ratio;
};

struct GestureSettings {
    SpringSettings layout;
    SpringSettings position;
    SpringSettings scale;
};

// Recovered section-11 close configuration; weight is the caller's frame-derived
// t, not clamped progress. Velocity must already use the source modifier's units
// (thresholds 3000/5000 and 1000/4000); this API does not infer Android unit mapping
// or inject that velocity into any scalar. Invalid results leave *result intact.
bool SettingsForGesture(double weight, Point velocity, GestureSettings* result);

struct Scalar {
    double value = 0.0;
    double velocity = 0.0;
    double target = 0.0;
    double response = kOpeningResponse;
};

// Standard oscillator reconstruction for independent parent position/scale
// channels. Returns false without mutation for invalid input/numerical failure.
// This models an oscillator, not UIKit's unverified projection/seed rules.
bool AdvanceScalar(Scalar* scalar, double seconds, double damping_ratio);

// Recovered Genie geometry plus an analytical oscillator model. The model
// preserves all 19 positions and velocities on Retarget; it does not claim to
// reproduce UIKit's unverified seed/retarget/projection rules.
class Animation {
public:
    // Invalid parameters leave an existing animation untouched. Reset seeds the
    // selected endpoint at rest; call Retarget(false) to open from minimized.
    bool Reset(const Parameters& parameters, bool minimized = true);
    bool Retarget(bool minimized);
    // Updates targets and dynamics transactionally, preserving every scalar's
    // current value and velocity. Changing geometry changes Mesh=F(Z); callers
    // must rebase parent/local coordinates if they require screen continuity.
    bool Retarget(const Parameters& parameters, bool minimized);
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
