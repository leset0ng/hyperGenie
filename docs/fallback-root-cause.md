# Remaining fallback and nested-window root cause

> Historical investigation of the pre-repair implementation. The subsequent repair,
> newly identified pending-command/icon publication race, final recordings and
> remaining unsupported cases are documented in [current validation](fallback-routing-validation.md).

The current patch has **not passed visual acceptance**. This investigation reads the existing fixed10-alt capture, module implementation, and extracted ROM code. It does not install another APK or change animation code. Its new finding is that a stale closing session can still discover a new starting window through a worker lookup fallback, despite the first-show guard added earlier.

## Findings and confidence

| Finding | Confidence | Evidence |
| --- | --- | --- |
| An older close attaches its shader to a newly appearing starting window before the new opening is registered | Confirmed in code and capture | lookup permits TYPE_APPLICATION_STARTING in its final fallback even for close; transition to content545 explicitly has close=1 and an 822ms-old epoch |
| Superseding that session does not promptly stop its closing shader updates | Confirmed in code and capture | native cancellation only ends non-closing sessions;545 finishes at its original deadline |
| OEM stock crop and old custom shader coexist during the new opening | Confirmed in capture |1538.466 frame shows VRI545 and output454 with SELF and OEM texture-Y scale0.638342, next to raw color539 |
| The manager color background is outside successful composed-App coverage | Confirmed in capture and implementation structure |499 and539 have no SELF; synchronous prepareGroup primes only the composed output; asynchronous discovery recorded neither color |
| Missing pending-remove key support explains these exact two color misses | Confirmed static capability gap, **现场 branch not observed** | ROM permits showing layers after current StartingData is null; module copy only uses current StartingData; capture lacks per-key lookup diagnostics |
| A first-buffer rejection has no autonomous recovery when that buffer subsequently arrives | Confirmed in code and capture | failed prepare publishes no transition, worker rechecks existing sessions only; ready about40ms later, custom admission returns only at next adjust |
| All nested pixels were previously captured in a distorted snapshot | **Not established** | no pixel comparison proves this; stale shader and unmasked color already explain the observed composition |

## 1. The old-close discovery guard is incomplete

`CentralBridge.kt:932` deliberately builds `startingContents` with `!request.closing`. However, the preferred-window chain ends at line936 with:

```kotlin
.ifEmpty { candidates.filter { it.type == 3 } }
```

This unconditional final fallback reinstates starting windows for a closing request when normal candidates are absent. With a reused Task and Activity, the later owner identity checks can still pass. The worker then starts the new buffer with the **old closing request**, because `start()` uses `request.closing` and `request.eventNs`. The first-show callback's `if (request.closing) return` at line757 does not protect this separate worker path.

Actual evidence:

- `fixed10-alt/native.log:253`: old closing event1537508370941 starts on content454 under Task871.
- `native.log:268`: at1538.330, content545 is submitted with `close=1`, lag822.0ms.
- `native.log:269`:454 hands off to545 with `starting=1` and13 retained companions.
- `modules.log:465`: worker confirms `attached parent=529 content=545 companions=[] starting=true ... closing=true`.
- Only at1538.379 does the new opening request for leash554 arrive.

The renderer has13 inherited companions even though the bridge's immediate list is empty because `PreserveOpeningPreviews()` retains previous starting companions across related epochs. This is inheritance, not13 new lookup results. No single companion count establishes that the pixels were captured twice.

## 2. Java session retirement is not native closing-session retirement

`CentralBridge.stop()` at1051 removes the worker request and calls `ContentAnimator.stopRequest()`. The native function marks a matching session with `cancel_ns = now +48ms` (`genie_content.cpp:1285`). But `FrameCurrent()` at670 only finishes on that timestamp when `!session.closing`.

The existing cancellation rule deliberately keeps a closing silhouette until its deadline because stopping an OEM animation does not itself hide its surfaces. That rule also applies to **superseded ownership**, since the stop API carries no reason/retirement mode. Thus stopping Java discovery prevents future lookups but leaves the already-installed closing effect on the newly claimed starting window.

The capture matches this implementation: after the new open supersedes the old session,545 does not finish until1539.021 (`native.log:284`), with the original request1537508370941 and `reason=deadline`. At1538.466 the new OEM opening is already rendering545 with the old SELF shader. Its geometry has a1120x1554.62 crop and texture-Y scale0.638342. Output454 also remains drawn with SELF under the same OEM crop. This combines a custom inner deformation with a stock outer crop/transform; it is not a clean switch to stock.

Specific SF evidence begins at `fixed10-alt/trace.txt:689193`; the VRI545 and output454 draw records follow the raw color539 in the same frame. There are7 detailed physical VRI545 draws, all with SELF. The trace does not expose intact numerical shader uniforms, so this report does not claim a reconstructed vertex-by-vertex mesh. The old epoch and closing direction come from native and bridge logs.

## 3. The raw rectangular background is a separate manager layer

The ROM contains two independent prestart systems:

- `PreStartingManager.java:559` allocates `preSplashStarting` using `setColorLayer()` and stores it under a StartingData key. `getParentForPreSurface()` at380 may parent it directly to the Task for a Home launch; it is not the controller's BLAST preview.
- `PreStartupWindowController.java:205` creates the other `Pre Starting surface`; `PreStartupAnimation.java:271` reparents that object into `mOpeningLeash`. Tracking only this animation's preview cannot discover every manager color layer.

The module's synchronous `prepareGroup()` (`CentralBridge.kt:206`, prime at259) primes output/source but passes no manager or WindowState companions. The separate worker `lookup()` collects them later. Consequently, successful output admission alone does not make every visible layer below the animated Task safe for a mask-free custom host.

This is observable without a failed preparation:

- At1533.545, group454 is successfully prepared before host mutation on leash504.
- At1533.618629, `preSplashStarting#499` and group454 share transform scale0.622247 and translation(398.271606,1045.702637).
- The color has alpha0.723633, zero corner radii and **no SELF**. Group454 has SELF. A full rectangular backdrop therefore remains behind the deformed inner App.
-499 is drawn32 times with detailed fields and53955 times; neither has SELF. The module log contains no priming or companion attachment record for either exact layer in the current system_server run. This absence does not identify which lookup early return occurred.

`fixed10-alt/trace.txt:319217` and the following group draw show the first paired frame directly.

### Proven lifetime gap, but not the observed branch for499/539

ROM `PreStartingManager.resetIfNeeded()` at1457 can retain the old StartingData in `mPendingRemoveStartingData`; `showPreStarting()` at1895 explicitly reads that map if `ar.mStartingData` is null, then shows its existing color/preview.

Module `PreStartingSurfaces.copy()` at321 reads only `activity.mStartingData`. Its manager discovery therefore loses these still-live pending-remove objects. `SurfaceEvents.showPreStarting` does handle the pending key during a show event, but that is a separate callback and `surfaceShowing()` can reject a closing request or have no matching current opening. It does not guarantee later worker coverage.

ROM retirement is asynchronous too: `resetWithAnimation()` and its commit callback (`PreStartingManager.java:1568`) set `isCleared=true` **before** a150ms alpha animation; registry removal and the remove transaction follow at animation end. Consequently, neither current-key absence nor `isCleared=true` proves that a manager layer has stopped drawing. New-key preload and old-key retirement can overlap. This is a contract required for a future repair, not a claim that this exact exit branch was sampled for499/539.

This is a source-confirmed ownership-lifetime mismatch. **The existing run does not log StartingData object identities or exclusion reasons, so it is not established that pending-remove was the exact reason499 or539 was missed.** A diagnostic for that question must log exact activity/task/data identity, both registry keys, color layer ID, and the return branch at the same OEM show/lookup event. No new unrestricted layer-name matching is justified.

ROM preload additionally skips bitmap splash creation within10 seconds when a cached color background exists (`PreStartingManager.java:893`,2727), explaining why rapid reopens exercise a color-only path frequently.

## 4. A transient first-buffer miss becomes an entire stock opening

This is an admission/recovery problem in addition to the visible layer mismatch:

1. `Observe()` (`genie_native.cpp:331`) defers initial `QueueTransition` for anchored streams until first-frame preparation succeeds.
2. `TuneParent()` at442 marks the stream unsupported and returns if `PrepareContentGroup` fails. It correctly preserves stock host geometry for that transaction.
3. `CentralBridge.prepareGroup()` returns before `publish()` when `subtrees.ready(source)` is null. It therefore records no new transition/session for the rejected opening.
4. `surfacesChanged()` at529 rechecks **existing sessions**. The later buffer-ready event cannot resume a rejected Launcher's transaction or re-authorize its host rewrite. A newer Launcher animation callback is required.

Actual chronology:

| Monotonic time | Observed event |
| --- | --- |
|1538.330 | Old close claims starting buffer545 |
|1538.379 | New opening554 submitted |
|1538.380 | Group rejected: source543 not ready |
|about1538.421 | `subtree ready source=543 output=548 generation=7` |
|1538.466 | Stock opening crop and stale SELF both render, beside raw color539 |
|1538.758 | Close554 rejected for nonuniform presentation |
|1539.021 | Old closing shader545 reaches deadline |
|1539.557 | Newer adjust554 successfully prepares output548 and changes host |

The ready event is approximately41ms after rejection, derived by aligning same-run module timestamps: rejection01:14:27.838, readiness01:14:27.879. Millisecond native/module logging introduces small alignment uncertainty. It is not a slow APK-load/module-disabled failure: current hooks are active, ready arrives, and the later adjust succeeds.

The native test for higher-ID adjust recovery passes because it explicitly supplies that future callback; it does not cover an otherwise unchanged opening with no subsequent callback.

## 5. Nonuniform crop rejection is a second capability boundary

The close at1538.758 requests1098.0137x2358.5083 from a1120x2436 domain (`modules.log:470`). Uniform scaling at that width would require2388.1798px height, a29.6715px difference. `CentralBridge.kt:232` and native `PrimeContentPublication()` require an identity/full domain or a uniformly scaled close.

The current protocol carries endpoints, velocities and corner values but not the full applied crop/texture mapping. It cannot preserve this stock start merely by relaxing the2px aspect tolerance. The earlier rejection leaves the stock opening path in place; the following nonuniform stock presentation can then fail the close admission too. This ordering is observed. It does not prove every nonuniform request on other runs comes from an earlier rejected opening.

## Repair boundaries established by this investigation

Before tuning mesh timing or adding screenshot hooks, a repair needs to:

1. Prevent a closing worker lookup from discovering a new starting window; ownership must include window/launch generation rather than only a reused Task/Activity.
2. Distinguish retaining an opaque canceled close from revoking its ownership of content now used by a new launch. Transfer/hide/restore must be atomic with the host contract; simply clearing all closing shaders would reintroduce raw output rectangles.
3. Include exact manager color companions, including supported pending-remove lifetimes, in publication coverage before disabling the host mask. Otherwise defer the host conversion.
4. Provide an explicit coordinated recovery path after first-buffer readiness, or retain an authenticated valid preview until group admission; never admit an empty group to hide the race.
5. Separately represent sampled stock crop/texture transforms when supporting nonuniform OEM starts.

This document identifies the failure mechanisms. No correction or successful visual retest is claimed. Machine-extracted evidence and its extraction script are under `temp/fallback-routing-validation/root-cause/`.
