# Army selection continuity validation

## Root cause and fix

Army selection inference sees mechanical events without explicit foreground markers.
It retained `leftDown`, `latest`, and `previousDirectClick` without verifying that
related inputs occurred within one continuous active gameplay interval.

One internal helper now requires forward QPC and active time, and rejects intervals
where the absolute difference between real QPC elapsed and active gameplay elapsed
exceeds 25 ms. The original one-sided check admitted impossible intervals when active
time advanced substantially more than QPC time: 100 ms minus 1,000 ms is -900 ms,
which passed the old <=25 ms predicate. The symmetric check allows small rounding
differences in either direction. It applies to
Down→Up, double-click pairing, Shift-selection chaining, and selection→operation.
Down→Up also has a separate configurable `maximumSelectionGestureMs` of 5,000 ms.
Other relationships retain their existing semantic windows and spatial thresholds.
Invalid gestures clear pending and cached acquisition state; a later clean gesture
works normally. Direct clicks retain zero reported selection duration.

Before: Down → five-second Alt-Tab → Up → Ctrl+1 could become BoxSelect → Assign.
After: the stale pair is rejected; Ctrl+1 remains recorded as ExistingSelection,
with unavailable acquisition duration, latency, and total execution time.
Normal Down → Up → Ctrl+1 behavior is unchanged, including a 180 ms box selection,
Ctrl-click, Shift selection, and genuine double-click inference.

## Validation

470/470 tests pass in the existing LLVM/MinGW test build. Added regressions cover
inactive gaps, stale Ctrl/Shift modifiers, excessive holds, backward QPC/active time,
recovery, pending-down replacement, cached-state clearing, double-click and Shift-chain
gaps, selection→operation gaps, configurable lifetime, rounding tolerance, and
assignment/addition method and per-group accounting. Existing replay binding,
classification, timing, serialization, and legacy saved-analysis tests pass.
Follow-up regressions reject real=100 ms / active=1,000 ms; accept real=200 ms /
active=180 ms and real=180 ms / active=200 ms; accept continuous 2,500 ms and
5,000 ms box gestures; and reject a 5,001 ms gesture. The custom-threshold
regression remains in place.
Production click scanning already checks continuity before processing each subsequent
event. A replay-correlated regression verifies a pre-gap Down cannot create a
click-based production visit using a post-gap release and production key.
Production implementation and production-macro provenance are unchanged.

The full application target in the existing build fails to link D3DCompile and WinMain.
The core library and complete test executable build successfully.

## Read-only saved-session scan

The original read-only scan found all 12 saved NAV sessions use schema 5 and
examined 6,982 Down→Up pairs. These historical scan results are preserved;
the follow-up does not reinterpret sessions or modify source evidence files.
Rejection counts (independent reasons): inactive gap >25 ms: 0;
duration >2,000 ms: 0; backward timing: 0.
Historical double-click merges rejected for inactive gaps: 0.
Historical selection→operation attributions crossing inactive gaps: 0.
Saved session files were not changed.

| Duration (ms) | All accepted pairs | Box pairs (5,408) |
| --- | ---: | ---: |
| Median | 113.203 | 115.932 |
| P90 | 171.576 | 180.075 |
| P99 | 389.267 | 402.388 |
| Maximum | 1,968.047 | 1,968.047 |

The observed maximum was approximately 1.968 seconds. The original 2,000 ms
default left only about 32 ms of headroom in 12 sessions, too close to observed
evidence for a conservative stale-state guard. The new 5,000 ms default provides
substantial safety margin while still preventing effectively indefinite stale
pairing. It remains configurable and is separate from the attribution window.
An inferred inactive interval proves elapsed QPC time was not active gameplay time;
it does not identify the exact focus-loss timestamp.

## Provenance and remaining uncertainty

New analysis_version:
`camera-nav-4-production-macro-4-army-control-group-management-6-army-command-1-ability-activity-1-replay-timeline-2`

JSON schema_version, NAV schemas, and RawInputEvent layout remain unchanged.
Older stored analyses remain readable with their stored metrics preserved.

This fix rejects selection relationships that demonstrably cross inactive time or exceed a bounded gesture lifetime. It does not detect arbitrary missing input caused by dropped capture events while StarCraft remains active.

That remaining problem is priority #7; queue-drop propagation and capture schemas
were not redesigned.
