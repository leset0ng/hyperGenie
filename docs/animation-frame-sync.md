# Shared App and icon animation frames

## Problem

The previous renderer used separate native display-event workers in Launcher and
system_server. Both sampled the same request timestamp and solver, but their
ordinary frame transactions could reach SurfaceFlinger on different refreshes.
A publication barrier could also freeze only one worker. During the platform's
alpha handoff, simultaneously visible App, preview and floating-icon layers
could consequently expose different contours.

A separate 500 ms icon-source matching cutoff rejected otherwise live animation
requests. That left the floating icon on stock geometry after the App had
already accepted the custom viewport.

## One motion owner

The authenticated `PREPARE_ICON` bridge operation transfers the launcher's real
floating-icon buffer and parent capabilities to system_server. The server
validates the exact published source request, dimensions and floating-icon
identity without entering the WMS global lock or waiting for the bridge worker.
It returns a shader transaction containing the server's commit listener.
Launcher merges that transaction into its actual first host transaction.

The icon joins the App's native Session. Its mesh, corner state and publication
barriers are shared with App and starting content. Subsequent frames advance
one solver and write all those targets in one transaction. There is no
Launcher-owned icon animation session. If icon registration precedes App
adoption, it seeds a provisional owner; App handoff retains its live state and
icon capability, including closing and retargeted motion.

Each target retains its own source domain. An App samples its full buffer; a
floating icon samples its square texture while drawing into a tall virtual
viewport. Only the normalized mesh and corner metric are shared. Copying the
App's full-buffer uniforms unchanged onto the icon would distort its glyph.

Opening preview and grouped-content host publications also carry the server's
primed shader transaction. Launcher does not independently reconstruct their
initial solver state. Pending first-show transactions gate the entire shared
Session; unrelated App Sessions keep running independently.

## Pairing and fallback

Source matching follows the newest valid submission with the exact animation
ID, rather than expiring at 500 ms. Actual stop, supersession and the renderer's
existing retirement watchdog still bound ownership. The original event timestamp
is retained; late callbacks never reset animation age to bypass validation.

OEM constructs App and icon animation transactions separately and may merge
those transactions before presentation. Their construction IDs are diagnostic;
different IDs alone do not reject an otherwise valid icon. Registration follows
the server's exact live request and retained buffer capability. Canonical App
host velocities and channel delays are copied to accepted icon hosts, while
OEM alpha scheduling and completion callbacks remain active.

If the real buffer cannot join or its capability is rejected, Launcher suppresses
only that temporary floating-icon clone. App content keeps its continuous custom
animation. Stock icon geometry is not displayed alongside a custom App contour.
Desktop icons are unaffected. This is a capability-failure fallback, not the
normal transition path.

## Protocol and validation

The private bridge protocol is version 4. `PREPARE_ICON` appends texture width
and height, full-buffer width and height, and two native SurfaceControl parcels
to the standard request. Successful replies contain a serialized server-owned
transaction. The imported controls are released after native references are
retained. Java SurfaceControl parcels carry width, height and presence before
the native capability; the imported buffer's name includes both parent and
buffer layer IDs. Other operations continue to reject trailing payloads.

A newer icon adjust can reach the capability endpoint before its asynchronously
dispatched App geometry. Only an ordered update of the same established App
stream may use the previous epoch as its source. The native owner retargets
its current state without resetting positions or velocities; later App lookup
joins that new epoch. Missing, cancelled or superseded predecessors are rejected.

Regression coverage exercises late icon attachment, shared commit barriers,
matching mesh values and transaction identity after missed frames, distinct
source extents, icon-first App handoff, reversal, stale ownership, cancellation,
and cleanup. Isolated renderer tests use fake layer capabilities with real
empty SDK transactions. They establish controller ordering and lifetime, not
physical frame quality or a production performance benchmark.

The final build passes assembly/lint, 775 native geometry checks, 10,503
renderer assertions and 71 Launcher-hook checks. Installed emulator-5580
captures observed six successful ordinary icon registrations and an actual
close-to-open adjust, all with `accepted=1 primed=1`. Core process IDs were
stable and the crash buffer remained empty. The installed APK SHA-256 was
`80a82090f7aa02120d7e094c4d75874748dc7b89f4faa607db265b88059d5cab`.
Recordings, exact runtime logs and deployment metadata are retained locally
under the ignored `temp/frame-sync-validation/final/` directory.

Stock SF host execution remains platform-owned. Identical host parameters do
not prove an arbitrary delayed host starts on the same display refresh; the
captures establish the sampled transitions and successful shared SELF route,
not complete behavior under every production scheduling stall.
