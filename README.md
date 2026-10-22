# HyperHomeAnimation

A libxposed / LSPosed API 102 module integrating the recovered iOS Genie
content animation into compatible HyperOS 4+ launchers.

The module runs in **launcher + system_server only**. Target applications need no
hooks or LSPosed scope. The former constant perspective MVP and Settings-specific
entry have been removed.

## Animation

The implementation follows [the iOS analysis](docs/ios-app-launch-animation-analysis.md)
for ordinary icon geometry: 3×3 vertices, 18 independent coordinate springs and
one bounds spring, directional row response, B/K/T scale and position
compensation, and current-state retargeting. Actual icon/window rectangles come
from the launcher's submitted animation parameters. Opening uses response `.24`,
ordinary closing `.336`, with critical damping and multiplier `.5`.

A central controller applies SELF shaders to real App content and aligned
starting content. On the verified portrait fullscreen path, it also retains the
OEM pre-snapshot, pre-splash and pre-start preview controls by their actual
Activity/transition ownership. All attached surfaces share the live 19-state
model; companion changes preserve it. Effects are cleared after opening settles,
or when closing ownership ends, rather than exposing a closing backing canvas
as soon as its springs converge.

SurfaceFlinger still schedules parent/icon motion and delivers the platform's
completion callbacks. For supported App transitions, the parent now carries a
full-buffer viewport: stock aspect cropping and stock corner channels are
removed, and the content shader draws the visible mesh contour and its source
corner mask. The floating icon's real BBQ child joins the system-server motion owner, with
its square source footprint retained separately from the tall virtual viewport.
App, preview and icon frames share one solver and one transaction. First shader
states and commit listeners return in the actual Launcher host transaction;
subsequent updates wait for every publication dependency. Live source pairing
has no separate 500 ms cutoff. The server validates the exact live request and buffer capability before a
floating icon joins; separate OEM construction transaction IDs do not disable
normal icon animation. An unavailable floating buffer is suppressed rather than
shown with stock geometry alongside an already-customized App.
See [shared animation frames](docs/animation-frame-sync.md).
SELF attachment also sets a
fully transparent border (`width=1e-6`, packed color zero) on actual buffer
children. Both ordinary and SF-animation snapshot updates retain CLIENT for
positive border width. Standard shadow radius remains zero after the earlier
shadow guard produced a dark exterior. Detachment clears the effect and border
together. Initial publication, frame
writes and retirement share one controller lock. Replacements keep shared
primary/companion layers, and icon stops match both parent and request epoch.
Different App/icon streams keep independent overlapping sessions. A superseded
closing silhouette stays protected through its existing retirement deadline.
Superseded primed transactions remain owned until commit and cleanup; snapshot
capture waits for every detach completion. The destination's tall virtual
crop remains separate from the square icon's sampled source footprint, so the
padded canvas is not compressed into the final icon. Platform alpha handoff and
completion callbacks remain active.

The host viewport and source mask are Android adaptations around the recovered
mesh. They do not establish a pixel-for-pixel iOS screen trajectory. The source
document leaves the full parent trajectory, complete radius(t), and some UIKit
retarget/gesture behavior unverified. Recovered gesture-response formulas remain
in `tests/genie_gesture_reference.h` for numerical reference and are not used by
the production renderer.

## Integration

```text
Native-only launcher
  start / adjust / stopSFAnimation hooks
  -> actual icon/window endpoints + generation ordering
  -> pre-start reparent correlates Shell leash with actual Task SC
  -> exact pre-start preview capability -> shader in initial host transaction
  -> asynchronous authenticated Binder geometry/Task updates

system_server: WMS.onTransact
  -> authenticate launcher, validate protocol, post worker
  -> under WMS lock: match trusted Task/container and copy BLAST controls
  -> surface creation/draw/show events, with first-state writes in show transactions
  -> outside WMS lock: native display-event worker
  -> 19 springs -> SELF mesh on App + aligned starting content
  -> handoff / reverse / clear and release
```

The real content is `WindowState.mWinAnimator.mSurfaceControl`.
`WindowState.mSurfaceControl` is its parent container. Numeric layer IDs only
correlate server-owned objects; no handle is manufactured from an ID.

See [the implementation and validation guide](docs/ios-module-integration.md)
and [the original content-route evidence](docs/general-content-route.md).

## Build and enable

```sh
./gradlew :app:assembleDebug :app:lintDebug
tests/run-native-tests.sh
adb install -r -g app/build/outputs/apk/debug/app-debug.apk
adb reboot
```

Enable `ng.lst.hha` for **`com.miui.home` and `system`** in LSPosed. Do not add
Settings, Calculator or other App scopes. A headless AVD needs those same two
entries in its LSPosed configuration. Reboot after changing scope or installing
an updated native module; automatic hot reload is disabled because persistent
native workers pin their library until process exit.

On a root ADB AVD, `tests/run-renderer-device-tests.sh` runs the actual renderer
controller in an isolated executable with fake surface capabilities and empty
SDK transactions. It covers frame/prime ordering, retained layers, scoped stops,
commit order, independent detach completions, and atomic CLIENT-guard lifetime.
The current suite passes 12,776 checks; `ADB_SERIAL` selects a device. It does not
install the module or touch real App layers. `tests/run-launcher-device-tests.sh`
adds 127 checks of the actual Launcher hooks with isolated parameter trees and
fake capabilities, including separate OEM construction transactions, reused
pre-start closing, exclusive preview/group preparation, admission retries, and
failure fallbacks.

The module app has no Activity. Its metadata is shown by the framework.

## Runtime switches

Launcher reads `/data/local/tmp/hha.conf`, or `/data/adb/hha.conf` if absent,
at most every 250 ms. Preserve launcher read permissions when replacing the file.

```ini
enabled=1
content_enabled=1
verbose=1
```

Both enable switches default to `1` when no configuration is present. Setting
either to `0` stops new custom requests and cancels an ongoing effect through
the central bridge. system_server does not read shell configuration files.
Legacy tilt/camera and `mesh_probe` keys no longer control this implementation.
A zero system animation scale bypasses the custom renderer.

## Source layout

```text
app/src/main/
├── kotlin/ng/lst/hha/
│   ├── HookEntry.kt          # system_server module entry
│   ├── CentralBridge.kt      # authenticated lookup, ownership and lifecycle
│   ├── PreStartingSurfaces.kt # owned OEM preview copies and domain checks
│   ├── SurfaceEvents.kt      # first-show transactions and lifecycle wakeups
│   ├── SnapshotGuard.kt      # prevent snapshots of deformed Task content
│   ├── OptionalMembers.kt    # shared per-component reflection caches
│   └── ContentAnimator.kt    # central JNI boundary
├── cpp/
│   ├── hha_native.cpp        # launcher event/geometry correlation
│   ├── hha_bridge.h/.cpp     # authenticated preview preparation and async updates
│   ├── hha_content.cpp       # SELF renderer, vsync, handoff and retarget
│   ├── hha_genie.h/.cpp      # recovered geometry and oscillator model
│   └── hha_viewport.h        # uniform host and mesh contour adaptation
└── resources/META-INF/xposed/
    ├── java_init.list
    ├── native_init.list
    ├── module.prop
    └── scope.list            # launcher + system only
```

## Validation status

**The confirmed square-frame paths and exact startup-preview close continuation
have been repaired.** Latest AVD cold, held-main and rapid captures do not show the
earlier square App plates or nested Settings content in the inspected contact
sheets. All1031 detailed sampled relevant draws retain SELF and159 publication
diagnostics contain no inherited-contour reset. Held Gallery/Calculator startup
previews both closed through the new authenticated path. One ordinary cold request
still exceeded the64ms first-buffer wait and deferred; nonuniform OEM crops also
retain the stock route until their sampled crop can be converted safely. See
[current fallback validation](docs/fallback-routing-validation.md) for the installed
SHA, recordings, sample limits and remaining boundaries.

The current build passes Android assembly/lint, 775 native geometry/reference
checks, 12,776 isolated checks of the actual renderer and 127 Launcher-hook
integration checks, plus20 actual-compiled-class JVM checks for exact preview-close
authorization and bridge integration. Shared frame regression covers publication order, distinct
texture domains, source-authenticated rapid reversal, composed-output replacement,
preview-mask retirement and worker/icon coalescence of a pending prime. The last
race has a failing-before/passing-after device regression. Earlier installed AVD captures
confirm six ordinary App/icon registrations and a close-to-open adjust with
`accepted=1 primed=1`, stable core processes and no new crash entries. See
[shared animation frames](docs/animation-frame-sync.md). Retired helper-only
tests have been removed; actual detach completion and JNI cancellation checks
cover the production paths. Sampled Settings/Calculator
transitions verify content access, composition and cleanup; these checks do not
establish complete visual fidelity. The earlier positive-shadow CLIENT guard
caused a visible dark exterior and has been replaced by a fully transparent
positive-width border, with standard shadow radius left zero.

Browser/gallery opening captures exposed a separate problem: OEM preview
buffers could remain visible after their registry entries disappeared, while
the renderer had already cleared their shader. Already-verified opening previews
now keep the shared mesh until final retirement of that same request, and a
starting-to-App handoff primes its inherited state immediately. Actual icon and
device corner radii replace the former hard-coded .225-to-zero mask; the AVD's
configured device radius is 180px. Position and velocity survive reversal.

OEM preview selection also uses the verified type mapping: `1=Snapshot`,
`2=Splash`. Snapshot requires its observed identity draw domain; Splash uses its
fixed full-size Canvas domain. An earlier installed capture found no shader
loss after attachment across 14 sampled preview layers and no shadow draw
scopes; core processes and the crash buffer remained unchanged. Those bounded
samples do not establish current visual acceptance.

Cold startup and rapid switching now use event-driven content discovery rather
than 16/50ms polling. Standard App/starting buffers are protected at creation,
client attachment and first layout, before an already-shown container can
expose the producer's first buffer. Client attach also appends SELF to the real
pending/sync publication transactions; ordinary show and manager-preview
hooks reinforce their actual show transaction. Early effect-only transactions
are owned by the bridge, while borrowed platform transactions are only appended.
The independent OEM
pre-start preview is resolved by exact ownership through a restricted capability
request, then primed in the launcher's first host transaction before stock
cropping is removed. That request never takes the WM lock or waits for rendering;
ordinary App capabilities remain server-only. An unavailable proof keeps both
App/icon stock viewports.

Same-Task/UID/fullscreen Activity handoffs preserve motion instead of cancelling
on every token change. Different Apps keep independent overlapping lifetimes,
per-stream event ordering, and all outstanding publication commit barriers.
Per-request snapshot ownership also covers a window cancelled before worker
lookup, and old cleanup cannot unblock or replace a newer request's release.
See [first-frame publication and validation](docs/first-frame-publication.md)
for the preceding publication repair. Independent SurfaceView content now uses
a live subtree mirror, private virtual display and owned BLAST output; SELF
warps that composed buffer once. Local legacy `[BBQ]` previews resolve their
actual buffer child before copying the producer control. Closing transfers the
tracked hide/show transaction into Launcher's first host transaction, so a
planar video child cannot leak while the bridge worker catches up.

Rapid parallel previews now use the exact opening Change/domain proof, including
merged-animation lifetime and pre-enqueue Snapshot validation. Earlier cold
and rapid captures had zero rejected preparations, first-draw misses or
uncovered BBQ descendants in their samples. See [live child content and validation](docs/live-child-content.md)
for measured coverage, lifecycle behavior and the extra composition cost.

Gesture-release closing keeps the dragged presentation rectangle separate from
the full content buffer. The authenticated group preparation returns its actual
buffer dimensions to Launcher for App/icon publication; scaled closing endpoints
are checked for uniform scale instead of being required to equal fullscreen
size. One-shot and tracked publications share the full content origin and the
release corner metric. The regression suite covers translated/scaled closing,
first-frame commit/cleanup, and rejection of distorted, oversized or incomplete
opening domains. Installed AVD checks pass five gesture closes (including a
fresh Launcher with no cached opening, two swipe lengths, Settings, Contacts
and Calculator) plus two Home-key closes. Every close publishes both App/icon
viewports with `accepted=1 primed=1`; core process IDs, crash logs and runtime
configuration remain stable. Production Binder probes also reject distorted
and oversized closes, a partial opening and the older protocol version. Sampled
recording frames show the closing contour deforming before icon handoff.
This validates the sampled release path; current fallback and visual failures
remain recorded above. Continuous finger-tracking Genie deformation
remains outside the implemented path.

The emulator has a separate verified host-capture buffer-selection problem
when its secondary display is enabled; recordings temporarily disable it and
restore it afterward. HDR/color-processing bypasses, other display
configurations, full gesture fidelity and production performance remain outside
current validation. The module remains an experimental integration.

Ignored local evidence is under `temp/ios-module-integration/validation/` and
`temp/ios-module-integration/resumed-validation/`, including physical recordings,
SurfaceFlinger dumps, source-footprint comparisons and module logs.

## Requirements and scope

| Item | Verified value |
| --- | --- |
| Runtime | Android 17 / API 37, HyperOS OS4.0, arm64-v8a |
| Launcher | RELEASE-8.01.02.7722-260904-09221445-R |
| Framework | LSPosed 2.2.1 (7930), API 102, HYOS-spawner support |
| Build | compileSdk 37, NDK 30.0.14904198, CMake 3.22.1 |
| Toolchain | AGP 9.4.1, Gradle 9.8.1, JDK 21 |

The physical pandora / 25098PN5AC on `OS4.0.4.0.XBLCNXM` and launcher
`RELEASE-8.01.02.8565-260918-10121010-R` also passes the runtime animation
layout probe. After installation and reboot, both launcher hooks and the
system-server/icon SELF renderers initialized, and real open/close/retarget
requests reached them. This confirms activation, not visual fidelity for cold
launches or rapid App switching.

Private native integration checks HyperOS 4+ and arm64, then verifies the
required interfaces and animation parameter layout before installing hooks.
There is no device, fingerprint, launcher-release or exact Android API whitelist.
Missing capabilities keep stock behavior. OS version alone does not prove that
an OEM private ABI is compatible. The module declares minSdk 29 for packaging,
which does not mean private animation integration is supported on Android 10.

Ordinary full-screen icon launches are the implemented path. Source-free opens,
big widgets, secure windows and non-full-screen tasks are rejected where they
can be identified. Multi-surface video/game content, freeform/split-screen,
full gesture tracking and exact iOS corner masks remain unsupported. Rotation
retires incompatible active content instead of extrapolating old geometry.
The analytical oscillator preserves 19 current positions/velocities; it does
not claim to reproduce undocumented UIKit seed/projection internals.
