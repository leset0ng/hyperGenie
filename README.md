# HyperHomeAnimation

A libxposed / LSPosed API 102 module integrating the recovered iOS Genie
content animation into the verified Android 17 / HyperOS 4 launcher.

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
corner mask. The floating icon's real BBQ child uses the same mesh and source
clock. Its first shader state and parent viewport are submitted atomically;
subsequent updates wait for transaction commit. The destination's tall virtual
crop remains separate from the square icon's sampled source footprint, so the
padded canvas is not compressed into the final icon. Platform alpha handoff and
completion callbacks remain active.

The host viewport and source mask are Android adaptations around the recovered
mesh. They do not establish a pixel-for-pixel iOS screen trajectory. The source
document leaves the full parent trajectory, complete radius(t), and some UIKit
retarget/gesture behavior unverified.

## Integration

```text
Native-only launcher
  start / adjust / stopSFAnimation hooks
  -> actual icon/window endpoints + generation ordering
  -> pre-start reparent correlates Shell leash with actual Task SC
  -> asynchronous authenticated Binder geometry request

system_server: WMS.onTransact
  -> authenticate launcher, validate protocol, post worker
  -> under WMS lock: match trusted Task/container and copy BLAST controls
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
│   ├── SnapshotGuard.kt      # prevent snapshots of deformed Task content
│   └── ContentAnimator.kt    # central JNI boundary
├── cpp/
│   ├── hha_native.cpp        # launcher event/geometry correlation
│   ├── hha_bridge.h/.cpp     # asynchronous Binder dispatcher
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

The current build passes Android assembly/lint and 758 native checks. On the
AVD, Settings and Calculator enter forced CLIENT composition during the sampled
open/close transitions and return to normal composition afterward. The recorded
matrix also checks cleanup after rapid reversal, runtime disable and zero
animation scale, verifies that neither target loads the module, and keeps
system_server, SurfaceFlinger and launcher PIDs stable. No new crash-buffer
entries appeared during that matrix; radio/camera failures already present at
its start are retained in the evidence. Rotation samples contained no target
output layers and do not establish successful mid-animation rotation rendering.

Frame-by-frame captures still show occasional unmasked rectangular content or
a tall icon during some transitions. Correct square icon proportions have been
captured near closing completion, but this is not a complete visual pass. Static
ROM analysis confirms that a linear/LUT color-processing branch can return
before wrapping the SELF shader, even under CLIENT composition. That branch's
presence is proven; its execution during the remaining flashes has not been
measured. A subsequent per-layer dimming experiment produced no reliable visual
improvement and was discarded. Its anomalous-frame samples did not verify the
intended dimming state, so it cannot exclude brightness scaling as a cause. The
ROM's native MSKP capture was also blocked from writing its output; diagnostic
properties and the experimental APK were restored. The module therefore remains
an experimental integration, not a finished iOS animation replacement.

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

Private native ABI use is gated to the verified ROM fingerprint. Other builds
keep stock behavior. The module declares minSdk 29 for packaging, which does not
mean private animation integration is supported on Android 10.

Ordinary full-screen icon launches are the implemented path. Source-free opens,
big widgets, secure windows and non-full-screen tasks are rejected where they
can be identified. Multi-surface video/game content, freeform/split-screen,
full gesture tracking and exact iOS corner masks remain unsupported. Rotation
retires incompatible active content instead of extrapolating old geometry.
The analytical oscillator preserves 19 current positions/velocities; it does
not claim to reproduce undocumented UIKit seed/projection internals.
