# Capture continuity: priority #7

## Root cause

The collector already assigns nonzero RawInputEvent sequence numbers before queue
insertion. Failed insertions therefore leave authoritative gaps between surviving
live events. Analyzer previously ignored those gaps, and compact NAV streams did
not retain boundaries. Transient modifiers, tap candidates, selection evidence,
and physical production/camera relationships could bridge missing input.

## Live handling and trailing loss

Analyzer now tracks sequences in O(1) time before processing each raw event. A
forward gap, initial sequence greater than one, or duplicate/backward sequence
starts a new 64-bit capture epoch. Forward missing positions are counted without
negative arithmetic; additions saturate rather than overflow. The boundary clears
keys, pending control-group taps, candidate/active edge state, cached cursor, and
symbolic camera context. No input is reconstructed. The surviving event is processed
normally after invalidation. Zero sequences mean unavailable information, so legacy
fixtures do not manufacture boundaries; comparisons do not bridge unnumbered input.
Geometry handling is unchanged. Foreground-transition recovery preserves monotonic
active time as described below.

Before: Ctrl down seq 100 â†’ seq 101 lost â†’ 1 down seq 102 could inherit Ctrl and
become an assignment. After: the gap clears transient state; the surviving 1 is
recorded as a selection in a new epoch.

After collector.stop() and complete queue drain, recording calls
reconcileCollectorDrops(collector.droppedEvents()) before finalize(). Collector loss
not already accounted for by sequence gaps invalidates state once, including stale
edge candidates at the end of recording. Repeated reconciliation is idempotent.
Optional raw-writer drops never enter this API; combined total-drop reporting stays
unchanged. Live boundary/missing-position diagnostics remain internal AnalysisResult
fields, not new derived JSON fields.

## Dropped foreground transition recovery

A dropped ForegroundGained previously left Analyzer inactive after an observed loss,
so later surviving input could be discarded for the rest of that interval. A dropped
ForegroundLost followed by a surviving gain could instead restart the active segment
without carrying forward its accumulated time, making later activeMs move backward.

Numbered ordinary Collector input is emitted only while StarCraft is foreground-active.
When sequence positions are missing and Analyzer believes it is inactive, that surviving
input resynchronizes capture at its own timestamp. It receives the current accumulated
active time and the new epoch; subsequent inputs advance from that point. Unnumbered
fixtures, contiguous input without missing positions, and duplicate/backward sequence
invalidation do not implicitly reactivate foreground state.

When a gain arrives after missing sequence positions while Analyzer still believes it
is active, retain the old segment only through its last successfully observed active
raw event, including key-up, mouse movement, and suppressed autorepeat. Start the new
segment at the surviving gain. A gap followed by a surviving loss similarly closes the
old segment at the last observed active event. Repeated losses while inactive do not
double-count time. Duplicate gains without missing positions keep the clock continuous.
Ordinary mouse/key gaps while active do not manufacture a pause.

The uncertain interval is excluded from confirmed active gameplay time. Because the
existing duration model has active and paused buckets, that excluded interval is
included conservatively in pausedDurationSeconds; this is not a claim that every
millisecond was actually foreground-inactive. This does not recover the exact timestamp
of the missing foreground transition or reconstruct lost input.

For example, gain at 0, loss at 100, dropped gain, and surviving D at 1000 emits D at
activeMs=100; Q at 1100 emits at activeMs=200. Finalizing at 1200 reports 300 ms active
and 900 ms conservatively non-active time. With an observed D at 400 and key-up at 410,
a dropped loss and gain at 1000 instead preserve 410 ms active; Q at 1100 emits at
activeMs=510. Finalizing at 1200 reports 610 ms active and 590 ms excluded time.

This completes the same capture-continuity-1 semantics. Epoch handling, NAV schema 6,
provenance, downstream production rules, queue behavior, raw layout, cursor sampling,
autorecord lifecycle, and replay mapping remain unchanged.

## Downstream handling

Central emitters stamp mechanical, navigation, and recenter events. Army selection
acquisition clears pending Down, latest acquisition, and direct-click history on
an epoch transition. This complements the existing symmetric 25 ms QPC/active-time
check and configurable 5,000 ms gesture lifetime from #6.

Production control-group visits finish at epoch changes; incomplete visits with no
presses are discarded, while observed pre-boundary presses are preserved. Assignment
generations reset per epoch: generation zero means no assignment observed in the
current evidence epoch, not no assignment in the whole session. Location-hotkey
context uses only assignments and recalls from the candidate's epoch.

Click scans and replay-confirmed second-pass burst extension stop at epoch changes.
Camera episodes, visits, context identities, duplicate minimap evidence, and existing
visit matching include epoch context. Physical macro cycles cannot merge visits from
different epochs. Equal QPC timestamps cannot bypass these checks.

Legacy scouting selection-active state clears at a boundary and can be established
again by observed selection. The replay-tag scouting paths derive command activity
from independently observed replay unit identity rather than carrying a physical
selection-active flag. Replay-native army and ability commands and player/timeline
alignment remain intact; missing physical input is not treated as a missing replay
command. Group operations actually observed remain recorded, with unsupported army
selection acquisition represented as ExistingSelection.

## Persistence and sizes

NAV advances from schema 5 to 6. Capture epochs persist for navigation, recenters,
and mechanical records. Readers retain v1-v5 compatibility with epoch zero. Older
NAV total-drop counts do not localize loss; readers do not invent boundaries.

| Evidence | Previous bytes | Current bytes |
| --- | ---: | ---: |
| In-memory MechanicalInputEvent | 40 | 48 |
| In-memory CameraNavigationEvent | 56 | 64 |
| In-memory CameraRecenterEvent | 32 | 40 |
| NAV camera/recenter record | 44 | 52 |
| NAV mechanical record | 34 | 42 |
| NAV header and section table | 84 | 84 |
| RawInputEvent / SMPRAW1 raw record | 48 | 48 |

Struct sizes were checked with the existing x64 LLVM/MinGW compiler. Epoch tracking
adds no allocation, locks, event-history scan, or gap-vector search on the raw hot
path. Existing evidence-vector allocation behavior is unchanged. Derived JSON
schema_version remains 4 and has no new per-event epoch fields. Stored legacy
metrics remain readable without recomputation.

## Validation

502/502 tests pass through the full CTest suite. New regressions cover:

- Dropped gain/loss recovery, unchanged contiguous focus transitions and ordinary
  active gaps, zero-sequence compatibility, safely observed suppressed input, and
  finalization. An exhaustive property check covers all 8,192 omission combinations
  of a valid 13-event live stream, verifying retention of surviving mechanical inputs,
  monotonic active time for each evidence stream, finite nonnegative final durations,
  and duration accounting bounded by session elapsed time.

- Stale modifiers, double taps, edge candidates/active episodes, and symbolic camera
  context, including equal timestamps and recovery with later clean input.
- Zero sequences, initial loss, duplicate/backward sequences, large sequence values,
  cumulative missing counts, and retention of surviving events.
- A real capacity-two SpscRingBuffer: seq 1 pushes, seq 2 fails, seq 3 pushes;
  Analyzer observes 1 and 3 and creates exactly one boundary.
- Unbracketed trailing collector loss, idempotent reconciliation, already bracketed
  loss, and raw-writer-only loss; stale edge state cannot fabricate a final episode.
- Army Down/Up, modifiers, click history, Shift chains, selection-to-operation, and
  scouting selection-active state across epochs.
- Control-group visits and assignment generations, click visits, confirmed bursts,
  camera-access attribution, minimap identity, location context, and macro grouping.
- Replay-native army/ability observations retained through a physical capture gap.
- NAV v6 round-trip with distinct 64-bit epochs, including values beyond uint32;
  equal-timestamp NAV reanalysis; explicit v5 compatibility; strict layout rejection.
  Existing v1-v4 compatibility and unchanged raw-header/layout regressions pass.
- Exact new provenance serialization and legacy saved-analysis loading.

## Read-only corpus check

Scanned 12 historical NAV sessions, all schema 5: zero sessions with reported drops,
zero total reported drops, and no saved .events.bin streams. No historical evidence
was rewritten. Schema 5 cannot distinguish collector queue loss from optional
raw-writer loss or identify exact capture boundary locations. A persisted raw
sequence gap would also be ambiguous between collector loss and raw-storage loss;
no historical reconstruction is claimed.

## Provenance and remaining uncertainty

`capture-continuity-1` means raw sequence discontinuities invalidate transient
physical-input state and prevent derived physical relationships from spanning
known dropped-capture boundaries. Other provenance components are unchanged.

Full analysis_version:
`capture-continuity-1-input-spatial-1-camera-nav-4-production-macro-4-army-control-group-management-6-army-command-1-ability-activity-1-replay-timeline-2`

The profiler can now detect and respect known missing capture input, but it cannot reconstruct what the missing input was.

Cursor coordinates on WM_INPUT are still sampled at message-processing time rather than being historical hardware-event cursor coordinates.

That cursor uncertainty is priority #8. Queue capacity/overflow policy, raw schemas,
geometry snapshots, lifecycle, replay timeline mapping, and production match windows
were not redesigned.
