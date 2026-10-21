# First-frame content publication

## Problem and repair

A standard SnapshotStartingWindow could draw for four frames (about 66 ms)
before WindowStateAnimator.showRobustly ran. Waiting for that callback was too
late: the OEM starting-window manager can show the window container before the
snapshot producer submits its buffer. The client's buffer transaction can then
make content visible without a new animator show-state transition.

The system-server bridge now observes three earlier boundaries:

- A server-created animator SurfaceControl is protected after createSurfaceLocked,
  before returning it to the producer.
- A client-created content surface is protected before
  attachClientSurfaceWithSync reparents it into the window.
- First layout retries protection when client attachment preceded the local
  frame needed to prove the fullscreen identity domain.

Creation uses a bridge-owned effect-only transaction. Client attachment also
appends protection to the platform's actual pending and, when used, sync
publication transactions. Existing show hooks remain as atomic reinforcement.
Borrowed transactions are never applied, closed or retained across callbacks.
There is no periodic content discovery or target-App hook.

Each request/layer gets at most one independent early publication. Actual
attach/show transactions retain their own initial effect and commit barriers.
Surface identity, Task/UID ownership, rotation and local frame checks remain
mandatory; zero builder dimensions alone do not establish a valid domain.

Early snapshot protection registers its verified Task for cleanup even if the
window disappears before worker lookup. Snapshot block ownership and delayed
release tokens carry the event timestamp. An old request cannot block/unblock a
new owner, replace its cleanup token, or cancel its pending release.

The same change set also contains the preceding event-driven discovery,
verified same-Task Activity handoff, independent overlapping native sessions,
per-request cancellation and retained publication commit dependencies. Those
changes prevent a new App or preview handoff from prematurely stripping a
still-visible predecessor's effect.

## Validation

Android assembly/lint, 784 geometry checks, and 1,221 isolated checks of the
actual native renderer pass. Renderer regressions cover overlapping requests,
reordered commits, delayed preview coalescence, initial publication in borrowed
transactions, and global cleanup.

On emulator-5580, the final installed APK SHA-256 was
`895ea5d826784c2e22fa2c526210292089cd76d398f4146431d3ec29a772aff5`.
SurfaceFlinger stayed PID 7352; the framework was restarted to load the module.

| Capture | Supported openings | Directly covered visible buffers | First-draw misses |
|---|---:|---:|---:|
| cold-start-first-frame-release | 2 | 5 | 0 |
| warm-start-first-frame-release | 4 | 16 | 0 |
| rapid-switch-first-frame-release | 1 | 1 | 0 |

Three standard snapshot windows in the cold/warm captures had creation-phase
priming and SELF on their first main-display draw. Warm validation also covers
real browser and gallery content plus splash handoffs. Both cold target
processes were force-stopped and confirmed absent before the first tap.

The cleanup run covers Settings/Calculator open/close, rapid reversal, runtime
disable, zero animator scale and rotation. No leftover forced-CLIENT state was
observed after cleanup. Target-App process maps contained no module. Core PIDs
and the crash buffer were unchanged during the valid captures/checks, and the
trace gate and display configuration were restored afterward.

Local ignored evidence is under
`temp/ios-module-integration/resumed-validation/`, including the capture
folders above, `first-frame-evidence.json`, `first-frame-release-cleanup/`,
physical recordings, module logs and trace line references. Captures without an
actual supported Genie opening were rejected by the analyzer.

## Remaining limits at this revision

The independent-child and parallel-preview gaps below were subsequently
addressed by [live child content and rapid preview handoffs](live-child-content.md).
The counts above remain the historical validation of the preceding repair.

These checks establish first-frame protection for directly selected surfaces;
they do not establish complete content-tree coverage. Independently generated
`[BBQ]` preview descendants were observed without SELF and are listed separately
in the evidence. SurfaceView video has the same class of missing child coverage.
This change does not implement that repair.

Several rapid-switch openings deliberately retained stock animation because
the exact early-preview capability/domain proof was unavailable. They are not
counted as protected Genie openings. The valid rapid capture exercised one
supported open plus multiple closes/cancels, not universal rapid-switch
coverage. HDR/color-processing bypasses and production performance remain
outside this validation.
