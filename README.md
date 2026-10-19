# HyperHomeAnimation

A libxposed (LSPosed, API 102) module that changes the HyperOS home screen
(`com.miui.home`) app-open animation.

The MVP in this repository adds a **real 3D perspective tilt** to the app window
while the launcher's stock open animation runs. The stock animation is kept;
one extra `RotateState` is written onto the same SurfaceFlinger transaction.
The app camera distance remains `1600`. Floating-icon distance and anchor offsets
are converted from native icon units to the app's units, keeping the expanded
icon background aligned with the app throughout the morph.

## Why a native module

On Android 17 / HyperOS 4 the launcher ships **no dex at all**. It is a
Rust/Flutter program started by `/system_ext/bin/hyos_spawner`, so
`java_init.list` class hooks never execute in that process. The module therefore
declares `META-INF/xposed/native_init.list` and does all of its work from
`libhha_native.so`.

## Where the hook goes

```
launcher (Rust/Flutter)
  hyper_os_ui::SurfaceControlTransaction::start_sf_animation
    libhyper_os_ui_public.so    SurfaceControlTransaction_start_sf_animation
      libhyper_os_ui_private.dylib.so   transaction_abi_impl::start_sf_animation
        libgui.so    Transaction::startSFAnimation(sc, params)     <-- hooked
          original()                        stock morph, untouched
          setSurfaceRotation(sc, ...)       this module's tilt
        libgui.so    Transaction::apply()
          SurfaceFlinger   MiRenderEngine::applyRotateState()
            real X/Y/Z rotation + perspective on the Skia canvas
```

`Transaction::startSFAnimation` is the one safe native boundary on this ROM:

| Candidate | Why not |
| --- | --- |
| `SurfaceControlTransaction_start_sf_animation` (public Rust C ABI) | Nine arguments with a register/stack split; a mismatched trampoline silently corrupts the callee's stack and the launcher segfaults. |
| `transaction_abi_impl::start_sf_animation` (Rust) | Builds `SfAnimationParams` through an sret (`x8`) return, which a plain function pointer cannot express. Hooking it crashes inside `start_sf_animation_v2_inner`. |
| `Transaction::startSFAnimation` (C++) | Ordinary member function: two `sp<>` by reference, returns `*this`. Round-trips cleanly. |

`MiRenderEngine::applyRotateState` in SurfaceFlinger already implements X/Y/Z
rotation plus a perspective coefficient, so no SurfaceFlinger injection is
needed.

## RotateState slot layout

Taken from `setSurfaceRotation` and verified against the compositor consumer:

| Slot | Meaning |
| --- | --- |
| 0..2 | X/Y/Z angles in **radians** (the Java wrapper misleadingly calls them `rotDegrees`) |
| 3..4 | rotation anchor offsets relative to the bounds centre |
| 5..7 | extra X/Y/Z translation |
| 8 | camera field-of-view in **degrees** |
| 9 | camera distance (not a ratio of window height) |

## Runtime tuning

Config is re-read from `/data/local/tmp/hha.conf` at most every 250 ms, so angles
can be changed over adb without rebuilding or restarting the launcher.

```sh
adb shell 'su -c "cat > /data/local/tmp/hha.conf" ' <<'EOF'
enabled=1
y_deg=26
camera_degrees=45
camera_distance=1600
normalize_icon_perspective=1
tilt_adjusts=1
verbose=1
EOF
```

| Key | Meaning |
| --- | --- |
| `enabled` | master switch |
| `x_deg` / `y_deg` / `z_deg` | tilt in degrees |
| `center_x` / `center_y` | rotation anchor offset |
| `offset_x` / `offset_y` / `offset_z` | extra translation |
| `camera_degrees` | perspective FOV |
| `camera_distance` | app perspective distance; default `1600`; icon distance is converted automatically |
| `normalize_icon_perspective` | convert icon distance and anchor units; default `1`; `0` reproduces the original double plate for comparison |
| `tilt_app_type` / `tilt_icon_type` | apply rotation to app / floating-icon animations (both enabled by default) |
| `tilt_adjusts` | also re-assert the tilt on every `adjustSFAnimation` |
| `verbose` | log the first 40 injections with their layer ids |

If the file is absent the compiled-in defaults are used. The launcher runs as the
app UID and cannot create files under `/data/local/tmp`, so create it as root.

## Layout

```
app/src/main/
├── AndroidManifest.xml            # label/description come from resources
├── kotlin/ng/lst/hha/
│   └── HookEntry.kt               # XposedModule entry (no-op on Android 17)
├── cpp/
│   ├── CMakeLists.txt             # arm64-v8a only
│   ├── exports.map                # exports native_init only
│   └── hha_native.cpp             # the actual implementation
├── resources/META-INF/xposed/
│   ├── java_init.list             # entry class (Android 16 and below)
│   ├── native_init.list           # libhha_native.so (Android 17)
│   ├── module.prop                # API versions, staticScope, exceptionMode
│   └── scope.list                 # target packages
└── res/values/strings.xml
```

The module app has **no Activity**: it only exposes metadata to the framework.

## Requirements

| Item | Value |
| --- | --- |
| Xposed API | 102 (`io.github.libxposed`) |
| minSdk / compileSdk | 29 / 37 |
| NDK | 30.0.14904198 |
| CMake | 3.22.1 |
| AGP / Gradle | 9.4.1 / 9.8.1 |
| ABI | arm64-v8a |

`io.github.libxposed:api` is `compileOnly` — it is provided by the framework at
runtime and must never be bundled.

## Build, install, enable

```sh
./gradlew :app:assembleDebug
adb install -r -g app/build/outputs/apk/debug/app-debug.apk
```

Enable the module for `com.miui.home` in the LSPosed manager. This AVD does not
ship the manager app, so on a headless emulator write the enable state directly
and reboot:

```sh
# modules_config.db lives in /data/adb/lspd/config; set modules_state.enabled=1
# and insert scope('ng.lst.hha','com.miui.home',0), then:
adb reboot
```

Restarting only `lspd` is not enough — its bridge to `system_server` is
established during boot.

## Verified on

- AVD `emulator-5580`, Android 17 / API 37, HyperOS OS4.0, arm64-v8a
- Launcher `RELEASE-8.01.02.7722-260904-09221445-R`
- LSPosed 2.2.1 (7930) with Zygisk Next 1.5.2 and HYOS-spawner support

Observed with `camera_distance=1600`, `y_deg=26`: aligned perspective during
settings, calculator and market launches, clean return to identity, no new
tombstones. On the same APK, `normalize_icon_perspective=0` reproduces the double
plate and `1` removes it. Final verification ran with the diagnostic agent detached.

## Perspective alignment

SF computes its projective coefficient as `-1 / (distance * length(basisY))`.
The app and floating icon have the same screen-space morph rectangle but use
different native coordinates. A captured frame had app basis scale `0.6892327`
and icon scale `4.5142727`, exactly the ratio `1120 / 171`. Passing `1600` to both
made the expanded icon much less tilted than the app, exposing its rounded fill.

The hook reads the icon's own spring endpoints and valid content span:

```text
referenceExtent = max(spring.start, spring.end)
iconSpan = validIconWidth - validIconLeft       # width-based tiles
scale = iconSpan / referenceExtent
iconDistance = appDistance * scale
iconAnchor = appAnchor * scale

AVD example: 1600 * 171 / 1120 = 244.2857
```

Height-based tiles use `validIconHeight - validIconTop` and the height spring.
Both distances then produce the same coefficient in parent coordinates. App
perspective remains unchanged. The original mask, snapshots, SF shaders and
transaction animation parameters are preserved. The scale is remembered by layer
ID for adjust packets, including packets without an animation type.

This uses the verified ROM's arm64 libc++ map ABI: spring/fixed roots at
`SfAnimationParams+0x18/+0x30`, node key/value at `+0x20/+0x38`, and spring
start/end at property `+0x10/+0x14`. It only reads existing entries. No sizes are
hardcoded. See [validation notes](docs/perspective-fix.md) for the captured evidence.

## Known limits

- App-open animation is the primary tested path. Widget morphs and gesture
  takeover need separate validation.
- The tilt is constant for the whole animation; it is not spring-driven and does
  not yet follow push/pop reversal or gesture takeover velocity.
- Perspective normalization is verified for full-screen portrait launches with
  width-based tiles (`tileType=2`). Height-based tiles, nonzero icon insets,
  nonzero anchors, split-screen and freeform windows are not device-tested.
  A spring endpoint is not necessarily the app's native extent during resizing.
- The tilt targets both app and floating-icon animation submissions, including
  reverse submissions using those same types. This MVP does not identify every
  return-home or widget transition.
- App and floating-icon animations are both rotated; wallpaper and blur are not.
- Extreme angles or very small `camera_distance` can push the projection through
  `w = 0`; the SF consumer computes the coefficient as
  `-1 / (distance * originalYAxisLength)`.
- Tuning is verified against the 7722 AVD launcher. The user's 8565 APK was not
  installed on this device.
- `MiuiBackGestureHook` protects its hook pages from the launcher's
  `MADV_DONTNEED` cleanup. This MVP does not, and has not needed to, but the hook
  pages are unprotected against that behaviour.
