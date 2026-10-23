# Fallback routing repair validation

## Current result

The confirmed square-frame mechanisms and exact startup-preview close handoff have been repaired. Final `fixed17-cold`, `fixed17-hold` and `fixed17-rapid` captures do not show the earlier sharp App plates or nested Settings content in the inspected contact sheets. This is bounded visual evidence, not an every-frame or every-ROM guarantee.

The startup-preview close path was exercised twice with the Gallery/Calculator main processes deliberately paused and then resumed. Both exact preview closes were accepted in their host transactions. Across the three latest runs there were no terminal group rejection replies; one ordinary cold-start request exceeded the64ms real-first-buffer wait and returned Deferred. Deferred recovery still needs a subsequent real host callback. Nonuniform OEM crops remain unsupported; no domain tolerance was relaxed.

The installed debug APK on **emulator-5580** has SHA256:

```text
9815fccd6d0156576fea7cad81f5078009289ba3544f7f50cf6c0e2ada91b234
```

Launcher PID16412 and system_server PID14263 both map the exact installed APK. Native Launcher hooks were verified before the first latest recording; subsequent recordings revalidated the unchanged PIDs, current APK hash and maps. No phone was accessed. The source, regression tests and this validation report are committed together; local recordings remain under the ignored `temp/` directory.

## Repairs

- Closing worker lookup cannot select an arbitrary newly appearing starting window through its final fallback. An established close also rejects content replacement.
- Ownership supersession uses exact parent+epoch revocation, stopping old closing writers. A new epoch on the same host ends old discovery without prematurely discarding the native contour; actual owner changes still revoke.
- Starting/color masks outlive motion cancellation, deadlines and supersession until another authenticated publication overwrites them or the platform removes the layer. Ordinary main-window inputs still detach. Explicit disable can clear live previews. Late attachments carry the starting flag through JNI.
- Exact OEM StartingData ownership is observed through creation, reset and transfer. Current, pending-remove and older registry-backed keys can coexist. `isCleared` is not used as proof that a fading color disappeared. Solid-color layers use plain SC copies rather than BBQ resolution.
- Synchronous group preparation resolves the exact Task and includes companions in the same shader/visibility transaction returned to Launcher. A missing source mapping triggers a verified lookup/preparation attempt.
- The Binder capability endpoint can wait up to64ms for an independently committed real first buffer without holding WMS, registry, SC or renderer locks. It revalidates ownership before publication. A timeout preserves stock geometry and reports Deferred; a later real host callback can retry the same epoch. This does not manufacture another callback or admit an empty output.
- Portrait-only admission is enforced in the native path rather than inferred from an aspect window. Both `Observe` and `TuneParent` call `genie::model::PortraitWindow`, which requires the expanded endpoint of the eased motion to be strictly taller than wide and rejects nonfinite, zero or overflowing inputs. The previous `0.8`/`1.25` ratio band is gone, so a square or landscape window can never be admitted by rounding, and a landscape host keeps its complete OEM animation. The shrunken icon endpoint is exempt because it is legitimately square.
- Exact previously authenticated starting WindowState grants can authorize a preview close when no normal source is available. Revalidation checks original producer SC, resolved BBQ identity, Activity/Task objects, latest launch token, current Task ancestry and uniform domain; the preview and companions join the actual closing host transaction. Generic closing-to-starting discovery remains excluded. Tokenless/zero-token option assignments revoke the cached current generation. Late same-launch normal content is guarded without inheriting the old closing writer.
- Same-epoch worker discovery immediately inherits the already-published mesh, corners, spring velocities, clock and generation. Previously, it replaced the pending command with endpoint state; linked-icon publication then wrote an identity mesh and zero corner over the correct prime.

## Direct reproduction and regression

`fixed15/native.log` records the last failure precisely. At2239.727 the correct pending close publishes `continuation=1 appliedCorner=0.169854` and a deformed mesh. At2239.728 a second publication for the **same layer, epoch and transaction** writes `continuation=0 appliedCorner=0` and identity bounds. Nine such resets were detected in that run.

The new regression exercises both directions, a zero-corner close input, worker insertion between App/icon publications, extra companion ownership, and both host commit orders:

| Executable on AVD | Result |
| --- | --- |
| Before state-copy fix, with new regression | Exit1; regression fails |
| After state-copy fix | Exit0;12,776 renderer checks pass |

The earlier SELF-presence checks could not detect this failure because the shader remained attached with wrong uniforms. Final captures include actual publication-state diagnostics in addition to SF draw records.

## Latest preview-close verification

| Evidence | fixed17-cold | fixed17-hold | fixed17-rapid |
| --- | --- | --- | --- |
| Scenario |Cold App opens, close after250/650ms |Pause Gallery/Calculator main process, close preview, resume |52 ordinary/gesture/rapid/cross-App actions |
| Requested logged actions |20 |4 |52 |
| Stable core PIDs/crash baseline/config |Yes |Yes |Yes |
| Detailed relevant physical draws with SELF |501/501 |83/83 |447/447 |
| Publication diagnostics |57 |12 |90 |
| Detected inherited-contour resets |0 |0 |0 |
| Ready group replies |6 |2 |21 |
| Terminal group rejections |0 |0 |0 |
| Deferred replies |1 |0 |0 |
| Explicit preview-close route hit |Not observed |2 |Not observed |

The held-main test is intentional fault injection, not ordinary App startup performance. The App processes were resumed in cleanup, both displays restored and trace gate reset. Its blank starting previews are expected because the App processes are paused; closing retains the contour instead of exposing an unmasked rectangular backing layer.

All1031 detailed relevant draws retain SELF across these latest three trace windows, with159 publication diagnostics and no detected contour reset. Latest identity/bridge JVM suite passes20 checks against actual compiled classes; it covers exact identity/producer/buffer/launch/domain rejection, current token invalidation, grant integration, restoration and publication encoding. The reviewer-confirmed stale token issue was corrected before installation.

Artifacts are `fixed17-cold`, `fixed17-hold`, and `fixed17-rapid` under the existing validation directory. Each includes deployment hashes/maps, results, native/module logs, video, trace, parsed draw records and `measured.json`. The earlier final renderer build (`fixed16`, SHA80923ccb) and its timings remain useful comparison evidence below.

## Earlier renderer-fix captures (fixed16)

Each run requests52 actions including ordinary opens, gestures,10 rapid open/home/reopen/close cycles, and cross-App actions. Swallowed taps mean these are requested actions, not52 distinct transitions or a statistical failure rate.

| Evidence | fixed16 | fixed16-alt |
| --- | --- | --- |
| Rapid opening delays |80/180/300/500ms |120/240/420/650ms |
| Stable core PIDs, crash log, module config |Yes |Yes |
| Detailed physical composed-output draws with SELF |239/239 |357/357 |
| Detailed physical manager-color draws with SELF |43/43 |58/58 |
| Detailed physical starting-window draws with SELF |43/43 |7/7 |
| Detailed physical controller-preview draws with SELF |49/49 |21/21 |
| Publication diagnostics inspected |79 |97 |
| Detected inherited-contour resets |0 |0 |
| Successful synchronous group replies |15 |21 |
| Rejected synchronous group replies |1 |0 |
| Deferred synchronous group replies |0 |0 |

Across the two detailed trace windows, all817 sampled draws in these four content categories retain SELF. The trace detail gate is active for approximately12 seconds per run and restored afterward; records without detailed corner fields are excluded. The videos cover each full action sequence, while contact sheets sample at10/15fps. No claim of exhaustive frame inspection is made.

Artifacts under `temp/fallback-routing-validation/`:

- `fixed16/physical.webm`, `fixed16-alt/physical.webm`: final recordings.
- Each final directory: `deployment.json`, `results.json`, `native.log`, `modules.log`, `trace.txt`, `trace-state.json`, `draw-records.json`, `measured.json`, and contact sheets.
- `fixed15`: last reproduction with9 diagnosed contour resets.
- `fixed11`: before general preview-mask retention; color511 lost SELF after an opening cancel.
- `fixed14/INVALID.md`: excludes the recording made without loaded native hooks. `fixed14-valid` is a separate authenticated intermediate run.

`modules.log` is cumulative for a daemon session. `measure-repair.py` filters the current system_server and matches failure leashes against actual `accepted=0/2` replies in that run's native log; historical module lines are not counted as current rejections.

## Checks

- `./gradlew :app:assembleDebug :app:lintDebug --offline`: passed.
- `tests/run-native-tests.sh`:790 checks passed, including14 new portrait-admission cases (square, landscape, tablet-portrait, wide-icon, degenerate, nonfinite and overflowing endpoints).
- `tests/run-launcher-device-tests.sh`:127 checks passed.
- `tests/run-renderer-device-tests.sh`:12,776 checks passed using actual `genie_content.cpp`.
- `tests/run-preview-close-jvm-tests.py`:20 exact-preview identity and bridge checks passed against actual compiled classes.
- Earlier actual compiled PreStartingSurfaces JVM harness:12 lifecycle checks passed; temporary harness at `/tmp/hha-prestarting-lifetime-check`.
- Targeted nonmutating reviews found no additional demonstrable medium/high defect in final synchronous coverage, bounded waiting, deferred admission or the new state-copy branch. The earlier preview-retirement finding was corrected and retested.
- `git diff --check`: passed.

These checks establish modeled lifecycle/geometry behavior and build health; actual visual evidence is reported separately above.

## Remaining scope

A closing request can arrive before a normal App producer exists. The new exact-preview continuation covers the previously authenticated, still-live WindowState path and was observed twice in `fixed17-hold`. Unauthenticated/controller-only previews, changed launch identity, invalid ancestry or unsupported domains still reject; the general closing worker fallback remains excluded. This does not claim that every possible OEM prestart representation is covered.

A first buffer taking longer than64ms can still defer. Recovery afterward requires another real Launcher callback; no asynchronous rewrite of an expired host transaction is performed. Full crop/texture conversion for nonuniform stock starts is not implemented. Other ROMs and physical-phone behavior have not been validated.

Landscape hosts are now excluded by construction rather than by a ratio band. The portrait rule is a native gate; the Java bridge additionally rejects any Activity, preview or subtree domain where `height <= width`, requires fullscreen windowing mode, no letterboxing and no fixed-rotation transform. A landscape transition therefore keeps its complete OEM animation. This could not be exercised end to end on the validation AVD: both of its displays report a locked `mCurrentOrientation=0` and `wm size`, emulator rotation and `settings put user_rotation` all left the logical frame at1120x2436 or912x596. The rule is covered by the host kernel suite and by a positive portrait control, not by a recorded landscape transition.

Artifacts for the portrait control are `fixed18-landscape/probe.json` and `fixed18-landscape/portrait-control.json`. The control ran against APK `b580b8574cf22f59876045f38e4203d9ce09366f208bd9f3c2836086d3724791` on `emulator-5580` and recorded four `mesh viewport` publications with `start=329.3x716.2 end=1120.0x2436.0`, no `portrait guard keeps stock` lines and no group domain rejections. Launcher hooks were verified loaded in PID6288; system_server was PID3948.

The earlier root-cause investigation is retained in `fallback-root-cause.md`; its observations describe the pre-repair implementation.
