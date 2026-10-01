# Edge episodes at camera-action boundaries

An edge episode used to be emitted at completion with its original start
timestamp. Completion also changed the live camera context to Manual. If a
location recall, control-group double tap, or minimap jump intervened, the old
episode could therefore overwrite the newer context. Delayed completion also
hid qualifying pre-action edge activity from jump-versus-recenter classification.

Before:

```text
edge starts at 0 -> F2 at 100 -> edge completes at 300
one edge episode [0, 300] crosses F2; completion overwrites F2 context
```

After:

```text
edge [0, 100] -> F2 at 100 -> optional new edge [100, 300]
```

The shared boundary helper completes the preceding candidate before classifying
or applying the explicit action. If there was a candidate and the action's
cursor snapshot is still in an edge zone, it seeds a new candidate at the action
timestamp. Seeding does not itself change camera context. With a cursor outside
the edge, no continuation survives. Each segment must independently reach
`edgeMinimumDwellMs`, even when its predecessor already qualified. Short
pre-action or post-action segments are discarded; dwell is never borrowed across
the boundary. A continuing qualified edge can now produce two legitimate pans.

Only actual camera actions split candidates. Single group selections, group
assignments/adds, location assignments, production keys, and ordinary clicks do
not. Foreground loss, gain, finalization, and screen-region reset behavior is
unchanged. The normal uninterrupted edge keeps its start timestamp and duration.

Production camera-access annotation and NAV/CSV ordering put a continuation
after an explicit action at the same timestamp. This also keeps the mechanical
location-recall duplicate adjacent to its navigation evidence for deduplication.
Production matching and other metrics are unchanged. Navigation counts and rates
consume the resulting segments normally; episode IDs reflect legitimate segments.

## Verification

Before implementation, the new stale-context regression failed after the later
mouse move: context was Manual instead of LocationHotkey. The other 438 tests
passed. After the fix, all 447 tests pass through CTest.

New regressions cover:

- F2 away from the edge, later completion, and subsequent normal recenter.
- Continued edge activity split around F2, including pre-action jump classification.
- Independently short/qualified dwell on both sides of the action.
- Group jump and recenter boundaries with and without an existing same-group context.
- Minimap completion at the camera-action timestamp.
- Non-navigation inputs preserving one uninterrupted candidate.
- Foreground completion, clean gain, and finalization of the last segment.
- Production camera-access ordering and episode IDs for jumps and recenters,
  without duplicate location-recall episodes.
- NAV round trip and CSV ordering for a continuation sharing a recenter timestamp.
- New serialization provenance and preservation of camera-nav-2 saved metrics.

The existing continuous-edge start/duration, edge geometry, foreground timing,
control-group semantics, production camera access, and legacy NAV tests remain
passing.

## Saved-session scan

A read-only scan of all 12 `sessions/*.nav` files (all NAV schema 5) found 3,494
edge episodes. Of those, 120 cross at least one strictly interior explicit
camera-action timestamp. There are 134 crossing action/episode pairs:

| Action | Crossing boundaries |
| --- | ---: |
| Location recall/recenter | 14 |
| Control-group jump/recenter | 120 |
| Minimap jump | 0 |

The scan compared each edge's persisted active-time start and duration with
navigation and recenter timestamps. Multiple actions can cross one old episode.
These are observed old crossings, not a count of faithfully reconstructed new
segments. NAV/mechanical evidence lacks the complete raw mouse-move and
foreground streams; there are no `.events.bin` files in this corpus. Consequently
faithful re-analysis and a verified historical after-count are unavailable. No
historical source files or stored metrics were modified.

## Provenance and limits

New analyses use:

```text
camera-nav-3-production-macro-4-army-control-group-management-5-army-command-1-ability-activity-1-replay-timeline-2
```

Persisted structures and schema versions are unchanged. Older analyses remain
readable without automatically rewriting their stored metrics.

This fix establishes causal event ordering. Edge dwell remains an inference;
it does not prove actual camera displacement, including at map boundaries.
