# Current AVD fallback validation

**Task3: BLOCKED — visual acceptance has not passed.** Square application plates,
small Settings content inside a large light backdrop, and some OEM fallback
remain reproducible. Assertion counts and `accepted=1` do not establish a visual
fix. No claim of universal elimination is supported.

The tested and currently installed debug APK on **emulator-5580** has SHA256:

```text
82fc2e7ccf63ee4d4a9651704df0514e65c94c9c327b33b681ee88fba91fdda5
```

Before recording, installed/build hashes matched, Launcher native hooks and three
OEM capture hooks installed, and Launcher/system_server mapped the current APK.
LSPosed retained the old package path after installation; its module APK path
was repaired with backups while preserving scope/enablement. Final runtime
checks found Launcher30179, system_server29365 and SurfaceFlinger29264, current
APK mappings, display1 ON and diagnostic trace byte restored0. Neither recorded
run changed core PIDs, the crash buffer, or module configuration. No phone was
used; changes remain uncommitted.

## Supported implementation changes

A recreated WindowState can create a second group for the same Task. The old
group could remain visible while its SELF was cleared after being inherited as
an ordinary preview. `LiveSubtrees.activate` now retires predecessor visibility
in the new group's publication transaction. Native preview preservation excludes
the previous composed output while preserving real OEM previews and linked
icons. An actual-renderer regression verifies this ownership and masked
retirement behavior.

SnapshotGuard now covers the ROM's three direct OEM capture entry points:
`captureSplashLayer`, `getBuffer`, and `captureActivity`. These bypass the regular
TaskSnapshotController capture hook. Installation was verified, but the bounded
captures did not exercise the blocked OEM branch, so prevention/concurrency is
not runtime-validated. These hooks do not fix the demonstrated color backdrop.

Existing source identity/capability checks, first-buffer readiness and crop
admission checks remain enforced.

## Commands and observations

- `./gradlew :app:assembleDebug :app:lintDebug --offline`: passed.
- `tests/run-native-tests.sh`:775 checks passed.
- `tests/run-launcher-device-tests.sh`:111 checks passed.
- `tests/run-renderer-device-tests.sh`:10,948 checks passed. Uses actual renderer
  code with fake surfaces/transactions; does not validate pixels or Kotlin order.
- `git diff --check`: passed.

Both current-artifact runs requested52 actions, including ten rapid cycles,
normal/gesture closes and cross-app taps. Some taps may be swallowed; command
counts are not transition counts or failure rates. Contact sheets rendered with
ffmpeg at0,5,10,15s were inspected; an additional30fps sheet examined the fixed9
square interval. Inspection was sampled, not exhaustive.

| Run | Duration | Rapid open delays | Detailed physical group draws with SELF | Group admission rejects |
| --- | --- | --- | --- | --- |
| fixed9 |20.835s |80/180/300/500ms |347/347 |0 |
| fixed10-alt |21.320s |120/240/420/650ms |375/375 |2 |

Detailed counts exclude records after the diagnostic gate closes. Before this
iteration, fixed8-diagnostic showed12 detailed draws of obsolete output411 with
no SELF/zero radii while output507 also drew. An earlier49-count included37
incomplete records and is invalid. The current samples did not reproduce that
old-output path, but both recordings still contain visual failures.

## Concrete unresolved route

In fixed10-alt, `preSplashStarting#499` has32 detailed physical draws with no
SELF and zero corner radii. `preSplashStarting#539` has55 with no SELF;8 have
zero radii, others retain OEM crop/radius. Both trace sources are **solidColor**,
not screenshot buffers. A large unmasked OEM backdrop can therefore surround
small, already-deformed App content. This is not proof of poisoned snapshot
pixels or a mirror cycle. The current-artifact samples include131 Settings
mirror input draws without SELF; other caches/paths remain unproven.

The two admission rejects on leash554 are a source not yet first-buffer ready,
and closing presentation1098.0137x2358.5083 against1120x2436. Uniform scaling
would require height2388.1798, a29.6715px mismatch. The latter needs a conversion
that carries the real sampled crop/initial contour. Removing the aspect check
or accepting an empty group would not be a supported correction.

Task3 remains blocked on exact authenticated protection of the OEM preSplash
solid-color companion route and safe nonuniform crop conversion. Other ROM
signatures, OEM capture blocked-path concurrency, every-frame correctness and
phone behavior were not validated.

Local ignored evidence is under `temp/fallback-routing-validation/`: the full
`FINAL-REPORT.md`, `STATUS.md`, `measured-final.json`, deployment/runtime JSON,
build/test logs, fixed9 and fixed10-alt physical.webm/native.log/modules.log,
contact sheets and detailed trace/draw records. The final report includes exact
trace line references. No additional experiment was started after bounded
validation completed.
