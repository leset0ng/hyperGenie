# 本仓库因为有一堆神秘bug作者修不明白，所以放弃开发。

---

# hyperGenie

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

Enable `ng.lst.genie` for **`com.miui.home` and `system`** in LSPosed. Do not add
Settings, Calculator or other App scopes. A headless AVD needs those same two
entries in its LSPosed configuration. Reboot after changing scope or installing
an updated native module; automatic hot reload is disabled because persistent
native workers pin their library until process exit.

On an AVD with an existing root ADB shell, the isolated device suites exercise
the production renderer and Launcher hooks without installing the module or touching App layers:

```sh
ADB_SERIAL=emulator-5580 tests/run-renderer-device-tests.sh
ADB_SERIAL=emulator-5580 tests/run-launcher-device-tests.sh
```

They cover shared frame publication, paused cancellation and reversal, exact
source/epoch ownership, pending commit barriers, closing host publication,
private readback geometry, and retirement of the current floating icon.

The module app has no Activity. Its metadata is shown by the framework.

## Runtime switches

Launcher reads `/data/local/tmp/genie.conf`, or `/data/adb/genie.conf` if absent,
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
├── kotlin/ng/lst/genie/
│   ├── HookEntry.kt          # system_server module entry
│   ├── CentralBridge.kt      # authenticated lookup, ownership and lifecycle
│   ├── LiveSubtrees.kt       # composed content, publication and closing hosts
│   ├── PreStartingSurfaces.kt # owned OEM preview copies and domain checks
│   ├── SurfaceEvents.kt      # first-show transactions and lifecycle wakeups
│   ├── SnapshotGuard.kt      # prevent snapshots of deformed Task content
│   ├── OptionalMembers.kt    # shared per-component reflection caches
│   └── ContentAnimator.kt    # central JNI boundary
├── cpp/
│   ├── genie_native.cpp        # launcher event/geometry correlation
│   ├── genie_bridge.h/.cpp     # authenticated preview preparation and async updates
│   ├── genie_content.cpp       # SELF renderer, vsync, handoff and retarget
│   ├── genie_model.h/.cpp      # recovered geometry and oscillator model
│   └── genie_viewport.h        # uniform host and mesh contour adaptation
└── resources/META-INF/xposed/
    ├── java_init.list
    ├── native_init.list
    ├── module.prop
    └── scope.list            # launcher + system only
```

## Gesture automation

`tests/avd-gesture-runner.py` compiles one persistent Android input helper and
repeats touch gestures on `emulator-5580`, display 0. Confirm the icon positions
on the current Launcher before using this 1120×2436 example:

```sh
python3 tests/avd-gesture-runner.py \
  --phone 166,2202 --settings 955,1769 \
  --home-swipe 560,2420,300,1500 --size 1120,2436 \
  --cycles 6 --open-dwell-ms 1000 --home-gap-ms 120 \
  --cycle-gap-ms 1000 --focus-mode scheduled \
  --native-close-gate-ms 60
```

Add `--dry-run` to print the plan without connecting. The runner rejects other
serials, physical devices and displays. Its default focus mode checks Launcher
before every tap. `scheduled` checks only the first tap, allowing subsequent
touches during an animation without adding focus-query latency.

The optional gate requires `verbose=1` and waits for the current Phone close
before sending the next tap. Its delay measures close-to-touch time; actual App
startup must be checked separately in native and ActivityTaskManager logs. A
missing close aborts the dependent tap. JSONL output records injected event times,
actual timing, gate observations and rejected input. Use `--open-dwell-ms 200`
for startup interruption, or `--pattern same-phone --cycle-gap-ms 0` for reversal.
`--sdk` and `--java-home` select local toolchains.

## Current behavior

Parallel App transitions keep independent motion owners and bounded publication
lifetimes. An interrupted opening pauses at its current mesh state so a verified
closing continuation can inherit position and velocity. Closing composed content
uses a separate SurfaceFlinger host in the same initial transaction, preserving
the OEM alpha curve while the old Task fades. Its relative layer follows the
old Task's ordering below a newly opening App. Only module-owned output and host
layers are trusted overlays, so their retained visual content does not block
Launcher touches.

The floating icon retains its real square buffer domain. Private SF-to-Launcher
readback reports the logical final icon rather than the tall shader viewport;
when a verified closing host takes over, the stopped icon's parent and current
buffer are hidden in the original Stop transaction. Late publications and
restores wait for their matching commits, keeping an old request from exposing
or hiding the next App. A confirmed return to the same Task through Launcher's
local animation ends only that closing owner, allowing its original content to
return through the existing atomic restore.

Current AVD recordings cover Phone/Settings parallel switching, startup-preview
interruption and gesture closing. Physical-device visual acceptance, rotation,
other display configurations, HDR/color processing and full finger-tracking
remain outside this validation. The module remains an experimental integration.

## Requirements and scope

| Item      | Verified value                                             |
| --------- | ---------------------------------------------------------- |
| Runtime   | Android 17 / API 37, HyperOS OS4.0.24.0.XFRCNXM, arm64-v8a |
| Launcher  | RELEASE-8.01.02.8244-260915-09172210-R                     |
| Framework | LSPosed 2.2.1 (7912), API 102, HYOS-spawner support        |
| Build     | compileSdk 37, NDK 30.0.14904198, CMake 3.22.1             |
| Toolchain | AGP 9.4.1, Gradle 9.8.1, JDK 21                            |

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
