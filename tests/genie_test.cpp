// SPDX-License-Identifier: Apache-2.0
#include "hha_genie.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
using hha::genie::Animation;
using hha::genie::Parameters;
using hha::genie::Point;
using hha::genie::Rect;
using hha::genie::Scalar;
int checks = 0;

void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        std::exit(1);
    }
}

void Near(double actual, double expected, double tolerance, const char* description) {
    ++checks;
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::fprintf(stderr, "FAIL: %s: got %.17g, expected %.17g (+/- %.3g)\n",
                     description, actual, expected, tolerance);
        std::exit(1);
    }
}

void EndpointsAndResponse() {
    Animation animation;
    Check(!animation.Retarget(false), "retarget requires initialization");
    Check(animation.Settled(), "uninitialized controller has no motion");
    Check(animation.Reset(Parameters{}), "default geometry is accepted");
    Check(animation.Settled(), "reset seeds an endpoint at rest");
    Near(animation.Scalars()[0].value, 0.3, 1e-15, "minimized x starts at .3");
    Near(animation.Scalars()[4].value, 0.7, 1e-15, "minimized property width is g=.4");
    Near(animation.Scalars()[18].value, 0.6, 1e-15, "bounds is a separate q=.6 scalar");
    Check(animation.Retarget(false), "opening retarget accepted");
    Check(!animation.Settled(), "new endpoint requires animation");
    Near(animation.Scalars()[0].response, 0.144, 1e-14, "documented top response");
    Near(animation.Scalars()[6].response, 0.2797645019878171, 1e-14,
         "documented middle response");
    Near(animation.Scalars()[12].response, 0.336, 1e-14, "documented bottom response");
    animation.Advance(5.0);
    Check(animation.Settled(), "opening converges");
    const auto mesh = animation.Mesh();
    for (std::size_t i = 0; i < mesh.size(); ++i) {
        Near(mesh[i].x, static_cast<double>(i % 3) / 2.0, 1e-12, "window endpoint x identity");
        Near(mesh[i].y, static_cast<double>(i / 3) / 2.0, 1e-12, "window endpoint y identity");
    }

    Parameters unity;
    unity.genie_scale = 1.0;
    Check(animation.Reset(unity), "g=1 has a well-defined identity limit");
    for (std::size_t i = 0; i < 9; ++i) {
        Near(animation.Mesh()[i].x, static_cast<double>(i % 3) / 2.0, 1e-14,
             "g=1 compensation does not divide by zero");
    }
}

void PublishedOpeningTable() {
    // Rounded property-space widths from section 5.2 of the iOS analysis.
    const double time[] = {0.0, 0.03, 0.06, 0.12, 0.24};
    const double expected[][3] = {
        {0.4000, 0.4000, 0.4000},
        {0.6258, 0.4880, 0.4655},
        {0.8416, 0.6340, 0.5854},
        {0.9801, 0.8503, 0.7936},
        {0.9998, 0.9825, 0.9630},
    };
    for (std::size_t t = 0; t < 5; ++t) {
        Animation animation;
        Check(animation.Reset(Parameters{}), "table reset");
        Check(animation.Retarget(false), "table open");
        animation.Advance(time[t]);
        for (std::size_t row = 0; row < 3; ++row) {
            const auto& scalar = animation.Scalars();
            Near(scalar[row * 6 + 4].value - scalar[row * 6].value,
                 expected[t][row], 0.000051, "published opening width");
        }
    }
}

void PublishedClosingTable() {
    Parameters p;
    p.response = 0.32;
    p.damping_ratio = 1.1;
    const double expected[][3] = {
        {0.9606, 0.9086, 0.8523},
        {0.8812, 0.7657, 0.6696},
        {0.7159, 0.5614, 0.4811},
        {0.5154, 0.4271, 0.4066},
    };
    const double time[] = {0.03, 0.06, 0.12, 0.24};
    for (std::size_t t = 0; t < 4; ++t) {
        Animation animation;
        Check(animation.Reset(p, false), "closing seed at window");
        Check(animation.Retarget(true), "closing retarget");
        animation.Advance(time[t]);
        for (std::size_t row = 0; row < 3; ++row) {
            const auto& scalar = animation.Scalars();
            Near(scalar[row * 6 + 4].value - scalar[row * 6].value,
                 expected[t][row], 0.000051, "published overdamped closing width");
        }
    }
}

void CompensatedMeshReference() {
    Parameters p;
    p.width = 1120.0;
    p.height = 2436.0;
    p.minimized_width = 1120.0;
    p.minimized_height = 1120.0;
    p.minimized_scale = 0.38;
    p.icon_x = 200.0;
    p.icon_y = 900.0;
    p.window_x = 560.0;
    p.window_y = 1218.0;
    p.outset_x = 24.0;
    p.outset_y = 48.0;
    p.genie_amount_y = -0.65;
    // Golden output from the independent Python reference in docs/evidence:
    // target_vertex -> vertex_response -> spring_state -> mesh_destination;
    // convert F-normalized destinations to (destination*F-O/2)/D.
    const Point expected[] = {
        {0.0024461259909474122, 0.036965981201779144},
        {0.4566659291999467, 0.036965981201779144},
        {0.91088573240894621, 0.036965981201779144},
        {0.037279136109021761, 0.41338774341028883},
        {0.39751553465982042, 0.41338774341028883},
        {0.75775193321061918, 0.41338774341028883},
        {0.046226986742844395, 0.69191617103575331},
        {0.38232107131936682, 0.69191617103575331},
        {0.71841515589588922, 0.69191617103575331},
    };
    Animation animation;
    Check(animation.Reset(p), "outset geometry accepted");
    Check(animation.Retarget(false), "outset open");
    animation.Advance(0.06);
    Near(animation.Scalars()[18].value, 0.3206496307789308, 1e-14,
         "bounds scalar follows base response, not a vertex row");
    const auto mesh = animation.Mesh();
    for (std::size_t i = 0; i < mesh.size(); ++i) {
        Near(mesh[i].x, expected[i].x, 1e-13, "reference B/K/T compensated x");
        Near(mesh[i].y, expected[i].y, 1e-13, "reference B/K/T compensated y");
    }
    animation.Advance(5.0);
    Near(animation.Mesh()[0].x, -12.0 / 1120.0, 1e-12,
         "outset endpoint includes local negative offset");
    Near(animation.Mesh()[8].y, 1.0 + 24.0 / 2436.0, 1e-12,
         "outset endpoint extends beyond normalized content");
}

void ContinuityAndTimePartition() {
    for (double damping : {0.84, 1.0, 1.1}) {
        Parameters p;
        p.damping_ratio = damping;
        Animation a;
        Check(a.Reset(p) && a.Retarget(false), "initialize partition test");
        Animation b = a;
        a.Advance(0.13);
        b.Advance(0.03);
        b.Advance(0.10);
        for (std::size_t i = 0; i < 19; ++i) {
            Near(a.Scalars()[i].value, b.Scalars()[i].value, 1e-13,
                 "analytic positions independent of frame partition");
            Near(a.Scalars()[i].velocity, b.Scalars()[i].velocity, 1e-11,
                 "analytic velocities independent of frame partition");
        }
    }

    Animation animation;
    Check(animation.Reset(Parameters{}) && animation.Retarget(false), "continuity seed");
    animation.Advance(0.06);
    const auto before = animation.Scalars();
    const auto mesh_before = animation.Mesh();
    Check(animation.Retarget(true), "reverse from live state");
    for (std::size_t i = 0; i < 19; ++i) {
        Near(animation.Scalars()[i].value, before[i].value, 0.0,
             "retarget preserves all 19 positions exactly");
        Near(animation.Scalars()[i].velocity, before[i].velocity, 0.0,
             "retarget preserves all 19 velocities exactly");
    }
    for (std::size_t i = 0; i < 9; ++i) {
        Near(animation.Mesh()[i].x, mesh_before[i].x, 0.0, "visible retarget x is continuous");
        Near(animation.Mesh()[i].y, mesh_before[i].y, 0.0, "visible retarget y is continuous");
    }
    animation.Advance(0.001);
    Check(animation.Scalars()[0].value < before[0].value,
          "momentum continues briefly before reversing");
    animation.Advance(5.0);
    Check(animation.Settled(), "reversed animation converges");
    Near(animation.Scalars()[0].value, 0.3, 1e-12, "reversal reaches original endpoint");
}

void OscillatorReferenceStates() {
    // Position AND derivative from the independent Python analytical reference.
    const double damping[] = {0.84, 1.0, 1.1};
    const double position[] = {0.055908493137097386, 0.07917877666744137,
                               0.09150111023031961};
    const double velocity[] = {-2.6453416370619314, -2.499924902826294,
                               -2.4200505934320553};
    for (std::size_t i = 0; i < 3; ++i) {
        Parameters p;
        p.damping_ratio = damping[i];
        Animation animation;
        Check(animation.Reset(p) && animation.Retarget(false), "oscillator reference seed");
        animation.Advance(0.06);
        Near(animation.Scalars()[0].value, position[i], 1e-14,
             "oscillator position matches independent reference");
        Near(animation.Scalars()[0].velocity, velocity[i], 1e-13,
             "oscillator derivative matches independent reference");
    }
    Parameters p;
    p.genie_amount_y = 0.8;
    Animation opposite;
    Check(opposite.Reset(p) && opposite.Retarget(false), "opposite icon direction");
    Near(opposite.Scalars()[0].response, 0.336, 1e-14, "opposite direction slows top");
    Near(opposite.Scalars()[12].response, 0.144, 1e-14, "opposite direction speeds bottom");
}

void InvalidInputs() {
    Animation animation;
    Check(animation.Reset(Parameters{}) && animation.Retarget(false), "invalid input seed");
    animation.Advance(0.06);
    const auto before = animation.Scalars();
    double Parameters::* fields[] = {
        &Parameters::width, &Parameters::height, &Parameters::minimized_width,
        &Parameters::minimized_height, &Parameters::minimized_scale,
        &Parameters::genie_scale, &Parameters::response, &Parameters::damping_ratio,
    };
    for (const auto field : fields) {
        Parameters p;
        p.*field = 0.0;
        Check(!animation.Reset(p), "zero denominator or response rejected");
    }
    for (double invalid : {std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        Parameters p;
        p.icon_y = invalid;
        Check(!animation.Reset(p), "nonfinite geometry rejected even if unused by local mesh");
        animation.Advance(invalid);
    }
    Parameters negative_outset;
    negative_outset.outset_x = -1.0;
    Check(!animation.Reset(negative_outset), "negative outset rejected");
    Parameters degenerate_response;
    degenerate_response.genie_amount_y = 2.0;
    Check(!animation.Reset(degenerate_response), "zero vertex response rejected");
    animation.Advance(-1.0);
    animation.Advance(0.0);
    for (std::size_t i = 0; i < 19; ++i) {
        Near(animation.Scalars()[i].value, before[i].value, 0.0,
             "rejected input preserves running positions");
        Near(animation.Scalars()[i].velocity, before[i].velocity, 0.0,
             "rejected input preserves running velocities");
    }
    animation.Advance(std::numeric_limits<double>::max());
    Check(animation.Settled(), "very long elapsed interval is finite and settled");
}
void IconParameterFactory() {
    const Rect icon{130.0, 800.0, 60.0, 60.0};
    const Rect window{100.0, 200.0, 400.0, 800.0};
    Parameters p;
    Check(hha::genie::ParametersForIcon(icon, window, &p), "ordinary icon factory");
    Near(p.width, 400.0, 0.0, "factory content width");
    Near(p.height, 800.0, 0.0, "factory content height");
    Near(p.minimized_width, 400.0, 0.0, "square d uses window reference, not icon pixels");
    Near(p.minimized_height, 400.0, 0.0, "square d preserves icon aspect, not App aspect");
    Near(p.minimized_scale, 0.375, 1e-15, "s=max(icon/window)*2.5");
    Near(p.genie_scale, 0.4, 0.0, "ordinary g=1/2.5");
    Near(p.icon_x, 30.0, 0.0, "Ix includes r*dw/2 and subtracts origin once");
    Near(p.icon_y, 630.0, 0.0, "Iy subtracts screen origin once");
    Near(p.window_x, 200.0, 0.0, "window center in local coordinates");
    Near(p.window_y, 400.0, 0.0, "window center in local coordinates");
    Near(p.genie_amount_x, 0.0, 0.0, "ordinary phone has no horizontal G");
    Near(p.genie_amount_y, -0.575, 1e-15, "G follows icon position, not fixed amount");
    Near(p.response, hha::genie::kOpeningResponse, 0.0, "factory uses recovered opening response");

    Parameters translated;
    Check(hha::genie::ParametersForIcon({1130.0, 300.0, 60.0, 60.0},
        {1100.0, -300.0, 400.0, 800.0}, &translated), "translated screen rectangles");
    Near(translated.icon_x, p.icon_x, 0.0, "shared screen translation leaves I unchanged");
    Near(translated.icon_y, p.icon_y, 0.0, "shared screen translation leaves I unchanged");
    Near(translated.genie_amount_y, p.genie_amount_y, 0.0, "shared translation leaves G unchanged");

    Parameters fraction;
    Check(hha::genie::ParametersForIcon({130.25, 800.25, 60.0, 60.0}, window,
        &fraction, {0.0, 0.0}, 2.0), "screen scale accepted");
    Near(fraction.icon_x, 30.5, 0.0, "Ix rounds to a physical pixel");
    Near(fraction.icon_y, 630.5, 0.0, "Iy rounds to a physical pixel");

    Parameters rectangle;
    Check(hha::genie::ParametersForIcon({130.0, 800.0, 40.0, 60.0}, window,
        &rectangle), "portrait icon geometry");
    Near(rectangle.minimized_width, 400.0 * 2.0 / 3.0, 1e-12,
         "minimized frame retains portrait icon aspect");
    Near(rectangle.minimized_height, 400.0, 1e-12, "minimized reference chooses longer icon edge");
    Check(hha::genie::ParametersForIcon({130.0, 800.0, 120.0, 30.0},
        {100.0, 200.0, 800.0, 400.0}, &rectangle), "wide icon source branch");
    Near(rectangle.minimized_width, 800.0, 1e-12,
         "icon aspect above window reference selects Dw");
    Near(rectangle.minimized_height, 200.0, 1e-12, "wide icon aspect retained");

    Check(hha::genie::ParametersForIcon(icon, window, &rectangle, {500.0, 1000.0}),
          "velocity prediction accepted");
    Near(rectangle.genie_amount_x, 0.0, 0.0, "horizontal speed does not enable phone Gx");
    Near(rectangle.genie_amount_y, -0.2, 1e-15, "Vy predicts window center by .15 seconds");
    Check(hha::genie::ParametersForIcon({130.0, 300.0, 60.0, 60.0}, window,
        &rectangle), "upper icon direction");
    Near(rectangle.genie_amount_y, 0.50625, 1e-15, "upper direction has .75 attenuation");
    Check(hha::genie::ParametersForIcon({130.0, -2000.0, 60.0, 60.0}, window,
        &rectangle), "offscreen icon retains clamped direction");
    Near(rectangle.genie_amount_y, 0.75, 0.0, "upper G clamp before attenuation");

    // Independent geometry_reference.py output at 60ms for the original factory
    // case. These are raw compensated coordinates, not bounding-box normalized.
    const Point expected[] = {
        {-0.00031030057105486605, 0.056225183499724114},
        {0.4327290644950892, 0.056225183499724114},
        {0.8657684295612331, 0.056225183499724114},
        {0.01141897046338335, 0.4198375923052673},
        {0.36626319529993906, 0.4198375923052673},
        {0.7211074201364948, 0.4198375923052673},
        {0.014625253350490964, 0.6998432411352462},
        {0.3480942589396625, 0.6998432411352462},
        {0.6815632645288341, 0.6998432411352462},
    };
    Animation animation;
    Check(animation.Reset(p) && animation.Retarget(false), "factory geometry opens");
    animation.Advance(0.06);
    for (std::size_t i = 0; i < 9; ++i) {
        Near(animation.Mesh()[i].x, expected[i].x, 1e-13, "factory raw B/K/T x reference");
        Near(animation.Mesh()[i].y, expected[i].y, 1e-13, "factory raw B/K/T y reference");
    }
    animation.Advance(5.0);
    for (std::size_t i = 0; i < 9; ++i) {
        Near(animation.Mesh()[i].x, static_cast<double>(i % 3) / 2.0, 1e-12,
             "factory converges to content x");
        Near(animation.Mesh()[i].y, static_cast<double>(i / 3) / 2.0, 1e-12,
             "factory converges to content y");
    }

    Parameters unchanged = p;
    for (const Rect& invalid : {Rect{0, 0, 0, 60}, Rect{0, 0, -1, 60},
                              Rect{0, 0, 400, 800},
                              Rect{0, std::numeric_limits<double>::infinity(), 60, 60}}) {
        Check(!hha::genie::ParametersForIcon(invalid, window, &unchanged),
              "invalid or oversized ordinary icon rejected");
        Near(unchanged.icon_x, p.icon_x, 0.0, "rejected factory preserves output");
        Near(unchanged.minimized_scale, p.minimized_scale, 0.0, "rejected factory preserves scale");
    }
    Check(!hha::genie::ParametersForIcon(icon, window, nullptr), "null factory output rejected");
    Check(!hha::genie::ParametersForIcon(icon, window, &unchanged, {0, 0}, 0.0),
          "zero pixel scale rejected");
    Check(!hha::genie::ParametersForIcon(icon, {0, 0, 0, 800}, &unchanged),
          "zero window extent rejected");
}

void UpdatedParametersAndCancel() {
    Animation animation;
    Parameters p;
    Check(!animation.Retarget(p, false), "parameter retarget requires live state");
    Check(animation.Reset(p) && animation.Retarget(false), "updated config seed");
    animation.Advance(0.06);
    const auto before = animation.Scalars();
    const auto mesh = animation.Mesh();
    p.response = hha::genie::kClosingResponse;
    Check(animation.Retarget(p, true), "close updates recovered dynamics");
    for (std::size_t i = 0; i < 19; ++i) {
        Near(animation.Scalars()[i].value, before[i].value, 0.0,
             "parameter retarget preserves every position");
        Near(animation.Scalars()[i].velocity, before[i].velocity, 0.0,
             "parameter retarget preserves every velocity");
    }
    for (std::size_t i = 0; i < 9; ++i) {
        Near(animation.Mesh()[i].x, mesh[i].x, 0.0, "dynamics-only update leaves visible x continuous");
        Near(animation.Mesh()[i].y, mesh[i].y, 0.0, "dynamics-only update leaves visible y continuous");
    }
    Near(animation.Scalars()[0].response, 0.4704, 1e-14, "close slows top row");
    Near(animation.Scalars()[12].response, 0.2016, 1e-14, "close speeds bottom row");
    Near(animation.Scalars()[18].response, 0.336, 0.0, "close bounds uses separate base spring");
    animation.Advance(0.001);
    Check(animation.Scalars()[0].value < before[0].value,
          "close inherits outward momentum before turnaround");

    const auto closing = animation.Scalars();
    p.response = 0.15;
    p.damping_ratio = 0.86;
    Check(animation.Retarget(p, false), "cancel returns to App with new dynamics");
    for (std::size_t i = 0; i < 19; ++i) {
        Near(animation.Scalars()[i].value, closing[i].value, 0.0, "cancel preserves live position");
        Near(animation.Scalars()[i].velocity, closing[i].velocity, 0.0, "cancel preserves live velocity");
    }
    auto invalid = p;
    invalid.genie_amount_y = 2.0;
    const auto prior = animation.Scalars();
    Check(!animation.Retarget(invalid, false), "invalid updated response field rejected");
    for (std::size_t i = 0; i < 19; ++i) {
        Near(animation.Scalars()[i].value, prior[i].value, 0.0, "failed update preserves positions");
        Near(animation.Scalars()[i].velocity, prior[i].velocity, 0.0, "failed update preserves velocities");
        Near(animation.Scalars()[i].target, prior[i].target, 0.0, "failed update preserves targets");
        Near(animation.Scalars()[i].response, prior[i].response, 0.0, "failed update preserves dynamics");
    }
    animation.Advance(5.0);
    Check(animation.Settled(), "cancel converges after rejected request");
    Near(animation.Mesh()[8].x, 1.0, 1e-12, "cancel endpoint is full App");
}

void ParentScalarAndGestureSettings() {
    Scalar scalar{0.6793503692, 5.1292229541, 0.4, 0.36};
    Check(hha::genie::AdvanceScalar(&scalar, 0.01, 1.0), "independent parent scalar advances");
    Near(scalar.value, 0.7186374734118445, 1e-14, "reference turnaround state preserves momentum");
    Near(scalar.velocity, 2.841255856778858, 1e-13, "reference turnaround derivative");
    Check(scalar.velocity > 0.0, "parent scalar still moves forward after target reverses");
    const auto before = scalar;
    for (double bad : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN()}) {
        Check(!hha::genie::AdvanceScalar(&scalar, bad, 1.0), "invalid parent delta rejected");
        Near(scalar.value, before.value, 0.0, "bad delta preserves parent position");
        Near(scalar.velocity, before.velocity, 0.0, "bad delta preserves parent velocity");
    }
    Check(!hha::genie::AdvanceScalar(nullptr, .01, 1.0), "null parent scalar rejected");
    Check(!hha::genie::AdvanceScalar(&scalar, .01, 0.0), "invalid parent damping rejected");

    hha::genie::GestureSettings settings{};
    Check(hha::genie::SettingsForGesture(0.5, {0.0, 0.0}, &settings), "ordinary gesture settings");
    Near(settings.layout.response, 0.32, 1e-15, "published gesture layout response");
    Near(settings.layout.damping_ratio, 1.1, 0.0, "gesture mesh is overdamped");
    Near(settings.position.response, 0.4, 1e-15, "position has its own response");
    Near(settings.position.damping_ratio, 0.83, 1e-15, "position has its own damping");
    Near(settings.scale.response, 0.44, 1e-15, "scale response independent of layout");
    Near(settings.scale.damping_ratio, 0.84, 0.0, "scale has its own damping");
    Check(hha::genie::SettingsForGesture(0.5, {-4000.0, -5000.0}, &settings), "fast flick settings");
    Near(settings.layout.response, 0.224, 1e-15, "upward speed shortens layout response");
    Near(settings.position.response, 0.48, 1e-15, "horizontal speed lengthens position response");
    Check(hha::genie::SettingsForGesture(0.5, {40000.0, -50000.0}, &settings), "speed modifiers clamp");
    Near(settings.layout.response, 0.224, 1e-15, "layout speed weight capped");
    Near(settings.position.response, 0.48, 1e-15, "position speed weight capped");
    Check(hha::genie::SettingsForGesture(1.5, {0.0, 0.0}, &settings), "frame weight not clamped");
    Near(settings.layout.response, 0.28, 1e-15, "frame weight extrapolates source formula");
    const auto old = settings;
    Check(!hha::genie::SettingsForGesture(100.0, {0.0, 0.0}, &settings), "nonphysical extrapolation rejected");
    Near(settings.layout.response, old.layout.response, 0.0, "invalid gesture preserves settings");
    Check(!hha::genie::SettingsForGesture(0.5,
        {std::numeric_limits<double>::quiet_NaN(), 0.0}, &settings), "nonfinite gesture rejected");
    Check(!hha::genie::SettingsForGesture(0.5, {0.0, 0.0}, nullptr), "null gesture result rejected");
}
}  // namespace

int main() {
    EndpointsAndResponse();
    PublishedOpeningTable();
    PublishedClosingTable();
    CompensatedMeshReference();
    ContinuityAndTimePartition();
    OscillatorReferenceStates();
    InvalidInputs();
    IconParameterFactory();
    UpdatedParametersAndCancel();
    ParentScalarAndGestureSettings();
    std::printf("Genie kernel: %d checks passed\n", checks);
    return 0;
}
