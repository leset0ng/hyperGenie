# Live child content and rapid preview handoffs

## Behavior

Independent SurfaceView video now participates in the same Genie contour as
its ordinary window. The central system-server module mirrors the verified
window's live content subtree into a private virtual display, whose output feeds
one explicitly owned BLAST buffer. SELF deforms that single composed buffer.
The source remains live: this is neither a screenshot nor a frozen Task snapshot.
No hooks or module libraries are installed in target application processes.

System-server-owned legacy `[BBQ]` previews use a smaller direct route. Their
original native SurfaceControl owns the actual buffer child returned by
`getParentingLayer()`. Resolve that child before making a normal SC copy:
the copy constructor discards the local producer/child references. `Surface`
`copyFrom` creation events protect newly materialized preview buffers before
their first post, using the original producer's verified registry/domain proof.

## Composition and publication

The source animator BLAST and its SurfaceView descendants retain their normal
relative transforms, crop, alpha and buffer updates inside the mirrored tree.
The mirror is reparented to a regular Builder-created root container on a
private layer stack. A parentless mirror is offscreen on this ROM even after
`setLayerStack/show`, so the explicit host is necessary.

The physical WindowState container outside the mirrored root is hidden while
its grouped output is visible. Hiding that external ancestor does not hide the
mirror. The output is a sibling under the verified Task, outside the mirrored
source subtree, and has its own BLASTBufferQueue rather than `Surface(outputSC)`;
that latter constructor would create another undiscovered `[BBQ]` buffer.

Readiness requires the first actual BLAST buffer transaction and its commit.
Display creation, Surface allocation and a successful apply alone are not
readiness signals. Opening publication holds back the physical source while a
retained protected preview remains available. Closing uses an authenticated
capability-only bridge request before Launcher rewrites the host geometry.

That request primes the grouped buffer and returns its owned output capability
plus a serialized visibility transaction. Launcher merges this transaction into
its exact first host transaction and appends the one-shot initial SELF state.
The visibility transaction carries the system-server commit listener across
the parcel/merge: a publication arriving after cancellation repairs visibility
against the latest owner, rather than leaving a formerly borrowed hide active.
Subsequent frames have one owner, the central renderer.

The native session explicitly excludes the unwarped source from inherited and
explicit companions, clears its previous SELF/CLIENT guard in the handoff, and
rejects late attempts to attach it again. Otherwise the mirror would sample an
already-deformed source and apply the mesh a second time.

The synchronous group endpoint holds no WM, SC or renderer lock while obtaining
snapshot protection. It serializes with an already-running pristine Task
snapshot. WMS creation/show hooks retain their nonblocking `tryBlock` behavior.
Both paths register cleanup even when the window disappears before worker lookup.

## Rapid switching

Three OEM lifecycle details previously made valid previews unavailable:

- Snapshot preparation schedules drawing before remote animation starts. Its
  exact zero-rotation buffer/domain proof is recorded before enqueue and
  rechecked at the real draw entry; draw failure removes the proof.
- Merged animations leave the top-level registry before their preview/leash is
  destroyed. Registration is retained until the exact animation's cleanup.
- Reused Activities can receive new options while an earlier opening survives.
  Live token bindings are retained and selected by exact token plus leash.

Parallel animation is now accepted when the same complete proof succeeds:
registered animation, its own opening leash, an OPEN Change with zero end
relative offset, matching full display bounds, zero rotations and verified
producer domain. OEM parallel and nonparallel start transactions attach the
same producer to that opening leash without an extra content transform. The
Home visual-rotation exception remains excluded by the rotation checks.

## Validation

Final installed debug APK SHA-256:
`05c65cdc9d052a25a757c272d4ddf1570ca580e6329e4a4c3ee5d7d9ce3949aa`.
All device operations targeted emulator-5580. SurfaceFlinger remained PID 7352;
only the framework was restarted between module builds.

| Final capture | Verified result |
|---|---|
| video-group-completed-surface | 95 live video input draws in the private composition; 18 physical grouped draws, all SELF; zero raw video draws on the physical display |
| video-group-completed-texture | 32 physical grouped draws, all SELF; no independent video buffer |
| rapid-switch-group-completed | 4 actual supported openings / 16 directly covered buffers; zero first-draw misses, uncovered BBQ descendants or rejected prepares |
| cold-start-group-completed | 2 confirmed cold openings / 9 directly covered buffers; zero first-draw misses, uncovered BBQ descendants or rejected prepares |

Physical closing frames, left to right, show the SurfaceView colors following
the window's curved silhouette:

![SurfaceView closing sequence](../temp/ios-module-integration/resumed-validation/video-group-completed-surface/physical-close-sequence.png)

The preceding atomic captures also exercised six rapid preview preparations
successfully and physical SurfaceView/TextureView deformation. An intermediate
video capture exposed a planar source frame during asynchronous close; that run
is not a passing result and led to the first-host-transaction handoff above.
Captures without actual Genie dispatch were rejected, including the initial
wrong-desktop-page control and the OEM animation-scale cache left at zero by a
test that deleted its setting instead of explicitly restoring the default.

Android assemble/lint pass, as do 784 geometry checks and 1,301 actual-renderer
regression checks. New regressions verify that grouped handoff preserves preview
companions, excludes/clears the source, waits for outstanding publication,
rejects late source attachment and supports a one-shot closing host publication.

The final cleanup run covers Settings/Calculator open/close, rapid reversal,
runtime disable, zero animation scale and rotation. Core process IDs and crash
buffer remained unchanged. Runtime disable removed the virtual displays and
outputs; idle caches had no visible output. Target-App process maps, including
the video probe, contained no module. Trace gates, settings and displays were
restored, and the temporary video APK was uninstalled after validation.

Local evidence and analyzers are under the ignored
`temp/ios-module-integration/resumed-validation/` directory. The consolidated
assertions are in `grouped-content-completed-evidence.json`; captures include
physical recordings, module/native logs, per-layer draw records and trace lines.
Earlier static ABI evidence and the isolated live-tree prototype are in
`temp/child-surface-coverage/`.

## Scope and costs

The verified domain remains nonsecure fullscreen portrait with no rotation,
embedding or letterboxing. The video control is ordinary SDR H.264. HDR, protected
video and arbitrary freeform/rotated mappings are not established by these tests.
Unproven capabilities retain coherent platform animation.

Live grouping adds a GPU composition pass and an intermediate buffer queue.
Power, end-to-end latency, colorimetric fidelity and high-refresh performance
have not been benchmarked. Up to four live pipelines are retained, with active
or guarded owners pinned until cleanup and idle entries evicted first. Actual
source removal destroys its output, mirror, display and queue. This route fixes
content coverage; it does not claim the same cost as per-layer SELF.
