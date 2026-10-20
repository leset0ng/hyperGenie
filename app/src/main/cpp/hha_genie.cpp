// SPDX-License-Identifier: Apache-2.0
#include "hha_genie.h"

#include <cmath>

namespace hha::genie {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kPositionTolerance = 1e-4;
constexpr double kVelocityTolerance = 1e-3;

bool Positive(double value) {
    return std::isfinite(value) && value > 0.0;
}

bool Validate(const Parameters& p) {
    const double values[] = {
        p.width, p.height, p.minimized_width, p.minimized_height,
        p.minimized_scale, p.genie_scale, p.icon_x, p.icon_y,
        p.window_x, p.window_y, p.outset_x, p.outset_y,
        p.genie_amount_x, p.genie_amount_y, p.response,
        p.damping_ratio, p.multiplier,
    };
    for (double value : values) {
        if (!std::isfinite(value)) return false;
    }
    return Positive(p.width) && Positive(p.height) &&
        Positive(p.minimized_width) && Positive(p.minimized_height) &&
        Positive(p.width + p.outset_x) && Positive(p.height + p.outset_y) &&
        Positive(p.minimized_width + p.outset_x) &&
        Positive(p.minimized_height + p.outset_y) &&
        Positive(p.minimized_scale) && p.minimized_scale <= 1.0 &&
        Positive(p.genie_scale) && p.genie_scale <= 1.0 &&
        p.outset_x >= 0.0 && p.outset_y >= 0.0 &&
        Positive(p.response) && Positive(p.damping_ratio) &&
        p.multiplier >= 0.0 && Positive(2.0 * kPi / p.response);
}

double SpatialWeight(double position, double signed_amount, double outset, double size) {
    const double curve = signed_amount < 0.0
        ? 1.0 - std::sqrt(1.0 - position) : std::sqrt(position);
    return -outset / (2.0 * size) + (1.0 + outset / size) * curve;
}

std::array<Point, 9> Identity() {
    std::array<Point, 9> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = {static_cast<double>(i % 3) / 2.0,
                     static_cast<double>(i / 3) / 2.0};
    }
    return result;
}

bool Step(Scalar* scalar, double seconds, double damping_ratio) {
    const double omega = 2.0 * kPi / scalar->response;
    const double error = scalar->value - scalar->target;
    const double velocity = scalar->velocity;
    double next_error;
    double next_velocity;
    if (damping_ratio == 1.0) {
        const double tau = omega * seconds;
        // Avoid inf*0 for very large elapsed intervals; e^-700 is already far
        // beneath presentation tolerances. No ordinary interval is shortened.
        if (tau > 700.0) {
            next_error = 0.0;
            next_velocity = 0.0;
        } else {
            const double decay = std::exp(-tau);
            const double coefficient = velocity + omega * error;
            next_error = (error + coefficient * seconds) * decay;
            next_velocity = (velocity - coefficient * tau) * decay;
        }
    } else if (damping_ratio < 1.0) {
        const double alpha = damping_ratio * omega;
        const double beta = omega * std::sqrt(1.0 - damping_ratio * damping_ratio);
        if (alpha * seconds > 700.0) {
            next_error = 0.0;
            next_velocity = 0.0;
        } else {
            const double decay = std::exp(-alpha * seconds);
            const double cosine = std::cos(beta * seconds);
            const double sine = std::sin(beta * seconds);
            next_error = decay * (error * cosine + (velocity + alpha * error) / beta * sine);
            next_velocity = decay * (velocity * cosine -
                (alpha * velocity + omega * omega * error) / beta * sine);
        }
    } else {
        const double root = std::sqrt(damping_ratio - 1.0) * std::sqrt(damping_ratio + 1.0);
        // The quotient form avoids cancellation in zeta-sqrt(zeta*zeta-1).
        const double slow = -omega / (damping_ratio + root);
        const double fast = -omega * (damping_ratio + root);
        if (-slow * seconds > 700.0) {
            next_error = 0.0;
            next_velocity = 0.0;
        } else {
            const double a = (velocity - fast * error) / (slow - fast);
            const double b = error - a;
            const double first = a * std::exp(slow * seconds);
            const double second = b * std::exp(fast * seconds);
            next_error = first + second;
            next_velocity = slow * first + fast * second;
        }
    }
    const double next_value = scalar->target + next_error;
    if (!std::isfinite(next_value) || !std::isfinite(next_velocity)) return false;
    scalar->value = next_value;
    scalar->velocity = next_velocity;
    return true;
}
}  // namespace

bool Animation::ConfigureEndpoint(bool minimized, std::array<Scalar, 19>* result) const {
    const auto& p = parameters_;
    const double size_x = minimized ? p.minimized_width : p.width;
    const double size_y = minimized ? p.minimized_height : p.height;
    const double q = minimized ? 1.0 - p.genie_scale : 0.0;
    const double h_x = minimized ? 0.5 + (p.icon_x - p.window_x) / p.width : 0.5;
    const double position_x = p.outset_x / 2.0 + h_x * size_x;
    const double position_y = p.outset_y / 2.0 + 0.5 * size_y;
    const double amount_x = minimized ? -p.genie_amount_x : p.genie_amount_x;
    const double amount_y = minimized ? -p.genie_amount_y : p.genie_amount_y;
    // signedG = G when minimized, -G when opening. The spatial curve therefore
    // keeps its sign across reversal while the response gains change direction.
    const double signed_x = -p.genie_amount_x;
    const double signed_y = -p.genie_amount_y;
    for (std::size_t i = 0; i < 9; ++i) {
        const double u = static_cast<double>(i % 3) / 2.0;
        const double v = static_cast<double>(i / 3) / 2.0;
        const double weight_x = SpatialWeight(u, signed_x, p.outset_x, size_x);
        const double weight_y = SpatialWeight(v, signed_y, p.outset_y, size_y);
        const double gain_x = 1.0 + p.multiplier * amount_x * (1.0 - 2.0 * weight_x);
        const double gain_y = 1.0 + p.multiplier * amount_y * (1.0 - 2.0 * weight_y);
        const double response = p.response * gain_x * gain_y;
        const double target_x = (1.0 - q) * u + q * position_x / (size_x + p.outset_x);
        const double target_y = (1.0 - q) * v + q * position_y / (size_y + p.outset_y);
        if (!Positive(response) || !Positive(2.0 * kPi / response) ||
            !Positive(gain_x) || !Positive(gain_y) ||
            !std::isfinite(target_x) || !std::isfinite(target_y)) return false;
        (*result)[i * 2].target = target_x;
        (*result)[i * 2 + 1].target = target_y;
        (*result)[i * 2].response = response;
        (*result)[i * 2 + 1].response = response;
    }
    (*result)[18].target = q;
    (*result)[18].response = p.response;
    return true;
}

bool Animation::Reset(const Parameters& parameters, bool minimized) {
    if (!Validate(parameters)) return false;
    Animation candidate;
    candidate.parameters_ = parameters;
    // Both endpoints must be usable, including their distinct outset-dependent
    // response fields. A later Retarget cannot leave a half-configured state.
    if (!candidate.ConfigureEndpoint(!minimized, &candidate.scalars_) ||
        !candidate.ConfigureEndpoint(minimized, &candidate.scalars_)) return false;
    for (auto& scalar : candidate.scalars_) {
        scalar.value = scalar.target;
        scalar.velocity = 0.0;
    }
    candidate.initialized_ = true;
    *this = candidate;
    return true;
}

bool Animation::Retarget(bool minimized) {
    if (!initialized_) return false;
    auto candidate = scalars_;
    if (!ConfigureEndpoint(minimized, &candidate)) return false;
    scalars_ = candidate;
    return true;
}

void Animation::Advance(double delta_seconds) {
    if (!initialized_ || !Positive(delta_seconds)) return;
    auto candidate = scalars_;
    for (auto& scalar : candidate) {
        if (!Step(&scalar, delta_seconds, parameters_.damping_ratio)) return;
    }
    scalars_ = candidate;
}

std::array<Point, 9> Animation::Mesh() const {
    if (!initialized_) return Identity();
    const auto& p = parameters_;
    const double progress = scalars_[18].value;
    const double full_x = p.width + p.outset_x;
    const double full_y = p.height + p.outset_y;
    const double small_x = p.minimized_width + p.outset_x;
    const double small_y = p.minimized_height + p.outset_y;
    const double bounds_x = full_x + progress * (small_x - full_x);
    const double bounds_y = full_y + progress * (small_y - full_y);
    const double a = p.minimized_scale * p.genie_scale;
    const double denominator = a + (1.0 - progress) * (1.0 - a);
    if (!Positive(bounds_x) || !Positive(bounds_y) || !Positive(denominator)) return Identity();
    const double k = (p.minimized_scale + (1.0 - progress) *
                     (1.0 - p.minimized_scale)) / denominator;
    // Coffset / (1/g-1) = -(Ix-Wx)/Dw * dw*g. Cancelling (1-g)
    // avoids a 0/0 in the valid g=1 identity case.
    const double translation_x = -(p.icon_x - p.window_x) / p.width *
        (p.minimized_width / small_x) * p.genie_scale * (k - 1.0);
    const double center_x = scalars_[8].value;
    const double center_y = scalars_[9].value;
    std::array<Point, 9> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const double x_hat = center_x + k * (scalars_[i * 2].value - center_x) + translation_x;
        const double y_hat = center_y + k * (scalars_[i * 2 + 1].value - center_y);
        result[i] = {(-p.outset_x / 2.0 + bounds_x * x_hat) / p.width,
                     (-p.outset_y / 2.0 + bounds_y * y_hat) / p.height};
        if (!std::isfinite(result[i].x) || !std::isfinite(result[i].y)) return Identity();
    }
    return result;
}

bool Animation::Settled() const {
    if (!initialized_) return true;
    for (const auto& scalar : scalars_) {
        if (std::abs(scalar.value - scalar.target) > kPositionTolerance ||
            std::abs(scalar.velocity) > kVelocityTolerance) return false;
    }
    return true;
}

}  // namespace hha::genie
