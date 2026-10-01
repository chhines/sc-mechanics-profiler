# Capture-time screen geometry

Previously, `runRecordingSession` read `collector.screenRegions()` when draining
each raw input. A queued click captured under geometry A could therefore be
classified under geometry B, detected after the click, simply because the
consumer was behind. Minimap, edge, display-mode, and debug region attribution
could change with consumer scheduling.

The collector now queues `CapturedInputEvent`, containing the unchanged
`RawInputEvent` and a copied `std::optional<ScreenRegions>`. The snapshot contains
client area, game area, viewport, display mode, and collector-side regions.
Only the collector thread writes its effective geometry. `push` copies that
state after the message's foreground update:

- `WM_INPUT`: observe foreground, refresh on gain, decode input, attach snapshot.
- `WM_TIMER`: observe foreground and refresh geometry, then attach that geometry
  to the polled cursor sample.
- Foreground gain: replace any previous focus geometry with the detection result
  before queuing `ForegroundGained`, including an empty optional on failure.
- A failed periodic refresh retains the last valid snapshot in the same focus
  session, preserving the existing detection policy.

The consumer applies `CapturedGeometry` before analyzing each input. It resolves
the minimap from that event's base snapshot and the configured Original Aspect
or Widescreen calibration profile. Missing geometry clears active regions; it
never triggers a lookup of subsequently detected geometry. Changes call
`Analyzer::setScreenRegions`, preserving its edge-state reset. Unchanged
snapshots allow edge dwell to continue normally.

`printRegionDebug` receives the very same resolved geometry applied to the
analyzer. Debug edge state resets on geometry changes too. The diagnostic
overlay follows resolved consumed observations; it is a UI visualization, not a
source of event classification evidence. Calibration's queue reader likewise
uses the snapshot attached to its capture-key event.

For example, a point at `(100, 400)` belongs to the automatic minimap in a
640-by-480 Original Aspect client at desktop x=0. Moving that client to x=400
while the click waits in the queue previously removed its minimap attribution.
Now the old click retains geometry A and becomes `MinimapJump`; a subsequent
click at the same desktop point carries geometry B and does not.

Each successful queue item carries its complete snapshot even if preceding
items were dropped. There is no independently lossy geometry-control message,
snapshot registry, or snapshot lifetime dependency. Geometry changes still
apply if the first input observing a change is dropped and a later one survives.

## Runtime and storage cost

On the project's Windows x64 toolchain, measured `sizeof` values are:

| Item | Bytes | Backing storage at 65,536 entries |
| --- | ---: | ---: |
| Previous `RawInputEvent` capture queue | 48 | 3 MiB |
| `CapturedInputEvent` capture queue | 136 | 8.5 MiB |
| Increase | 88 | 5.5 MiB (5,767,168 bytes) |

This bounded increase is acceptable for an unambiguous value snapshot. The
envelope is statically required to be trivially copyable. The queue allocates
its backing array once; attaching a snapshot introduces no per-input heap
allocation or additional lock. The separate raw storage queue retains its
48-byte items.

`RawInputEvent` remains persisted input evidence; `CapturedInputEvent` is runtime
input plus geometry context. `SessionWriter::submitRaw` receives only
`captured.event`. The `SMPRAW1` header, raw schema version 1, 48-byte event size,
and field offsets remain unchanged. NAV structure and JSON `schema_version`
also remain unchanged.

## Provenance and limits

New serialization uses:

```text
camera-nav-4-production-macro-4-army-control-group-management-5-army-command-1-ability-activity-1-replay-timeline-2
```

`camera-nav-4` means geometry-sensitive input classification uses geometry known
at capture time, rather than shared geometry read at consumer time. Older
analyses remain readable with their saved metrics intact.

Existing NAV files and optional `.events.bin` files lack event-time geometry
snapshots. They cannot authoritatively reconstruct these historical
classifications, and this change does not rewrite or repair historical files.

Cursor coordinates for `WM_INPUT` are still sampled when its message is
processed. They are not reconstructed hardware-event cursor coordinates. This
fix binds the known geometry to that capture observation; it does not fully
validate historical spatial attribution. QPC, replay alignment, active-time
mapping, geometry detection, and edge-scroll heuristics are unchanged.

## Regression coverage

`test_capture_geometry.cpp` reproduces the previous latest-global lookup and
tests an actual queued envelope across a later geometry refresh. It covers two
queued snapshots, minimap and edge flips in both directions, a same-client
Original Aspect/Widescreen transition, unknown geometry becoming available,
focus regain with and without geometry, candidate and active edge resets, and
queue saturation followed by surviving inputs with new geometry.

A deterministic capture sequence is drained immediately, in batches of 2, 3,
5, and 8, and entirely after capture. All navigation, recenter, and mechanical
event fields must match. Debug region geometry is checked against the analyzer
on each consumed event. Storage coverage pins raw field offsets and verifies
the original raw header, payload size, and all raw fields through file I/O.
Serialization and legacy-read tests cover camera-nav-4 and retained camera-nav-3
metrics alongside earlier provenance.
