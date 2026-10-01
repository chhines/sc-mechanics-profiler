# Replay-to-active-time monotonicity fix

## Investigation and root cause

`identifyReplayPlayer` aligns control-group selection sequences with a longest
common subsequence (up to 4,000 events), then applies match coverage and player
separation thresholds. `makeTimelineAnchors` pairs each matched replay frame with
the live event's foreground-active milliseconds. It previously skipped duplicate
or decreasing frame/time coordinates, but allowed non-finite times.

The old mapping clamped every segment to 5–80 ms/frame while keeping the original
anchor coordinates. The line therefore did not necessarily reach its endpoint.
Below 5 ms/frame it overshot and jumped backward at the next anchor; above 80 it
undershot and jumped forward. This affected the final anchor as well as internal
anchors. Single-anchor and endpoint extrapolation used positive rates, but signed
frame subtraction could overflow for extreme int64 input. Non-finite anchors
could also defeat ordering checks. Existing tests covered ordinary replay
correlation, but not this mapping invariant.

All five direct consumers use the same mapping: production events, selections,
control-group edit snapshots, unit-command snapshots, and ability candidates.
These feed production matching/cycles, control-group matching, army activity,
scouting evidence/activity, and ability activity. No downstream timestamp clamp
has been introduced.

## Fix and clock semantics

The internal `replay_timeline.h` helper keeps finite, strictly increasing anchors,
preserving the existing first-usable-anchor policy for duplicates/contradictions.
Between anchors it uses `std::lerp` with the actual frame fraction. Ordered finite
endpoints and a nondecreasing fraction give monotonic interpolation with exact
endpoints, including floating-point rounding at boundaries. Frame offsets use
unsigned differences to avoid signed overflow and preserve adjacent int64 frames.

The 5–80 ms/frame limits still bound extrapolation, attached directly to the first
or last anchor. The existing 42 ms/frame nominal fallback remains for one anchor
(and the internal empty-anchor helper); production still rejects zero anchors.

Interior slope limits are quality checks rather than hard geometric constraints.
Replay simulation frames and foreground-active time are different clocks:
foreground-inactive intervals can compress the latter. Clamping an interior rate
cannot preserve both measured endpoints. Keeping those endpoints and reporting
unusual rates avoids inventing replacement timing or cascading anchor rejection.
No replay time is substituted for QPC/wall time.

The existing replay correlation diagnostic object and its JSON now expose:

- `rejected_timeline_anchors`: duplicate, contradictory, non-finite or unusable matches.
- `shallow_timeline_segments`: accepted intervals below 5 ms/frame.
- `steep_timeline_segments`: accepted intervals above 80 ms/frame.
- `nominal_timeline_fallback`: only one usable anchor, so the rate is assumed.

These diagnostics do not imply that a high sequence score validates timing.

## Tests and saved-session regression

Before changing the algorithm, the extracted original implementation failed the
new shallow-anchor and segment-boundary tests; the ordinary interpolation test
passed. After the fix, all **424 tests pass**, both directly and through CTest.
The replay integration smoke executable also passes against the saved session.
Validation used portable LLVM-MinGW 20260922 (Clang 23.1.2), CMake 4.4.3, and Ninja
1.13.2 in ignored `out/tools`; libc++ required `-fexperimental-library` for the
repository's existing `osyncstream` use. The MSVC preset was not run.

Seven mapping tests cover the original failure, ordinary interpolation, rates
below/at/above both bounds, multiple boundaries, extrapolation on both sides,
duplicate/decreasing/non-finite/near-duplicate anchors, nominal fallbacks, int64
extremes, 100 seeded noisy fixtures queried in both orders, and the actual saved
session's anchors. Two correlation tests cover the resulting ability stream,
quality counts, diagnostic serialization, and single-anchor fallback reporting.

For `(0, 0 ms), (100, 100 ms)`, frames 99 and 100 previously mapped to
**495 → 100 ms**. They now map to **99 → 100 ms**.

The local regression used `sessions/2026-08-23_185246.nav` and
`192216,(4)KnockOut1.4.rep` from the August 23 replay autosave directory. The same
player, 2,037 matched selections and 2,013 accepted anchors were recovered.
The old formula reproduces the reported pair to the NAV file's microsecond
precision (the original JSON retained additional precision):

| Frames | Old mapped active time (ms) | New mapped active time (ms) |
| --- | ---: | ---: |
| 18051 | 760208.949000 | 758826.649062 |
| 18079 | 759653.928523 | 759653.928523 |

The five direct mapping streams were checked in replay-frame order:

| Stream | Events | Old decreases | New decreases |
| --- | ---: | ---: | ---: |
| Production | 750 | 0 | 0 |
| Selections | 892 | 1 | 0 |
| Control-group edits | 174 | 0 | 0 |
| Unit commands | 2,339 | 1 | 0 |
| Abilities | 62 | 0 | 0 |

Re-running full correlation also produced 1,616 army observations and 62 ability
observations with no decreases. Army observation count and production correlation
counts match the saved output. All 4,177 events in ordinary intervals or
extrapolation differed from the old formula by at most **2.33e-10 ms**.
The diagnostic reports 24 rejected anchors, 2 shallow intervals and 12 steep
intervals; nominal fallback is false.

Changes inside those abnormal intervals are intentional: the largest unit-command
shift is **10,906.120077 ms**, because the old clamped line substantially missed its
endpoint. This fix guarantees ordering, not correctness of those anchors. Bad
player/alignment matches can still produce inaccurate but monotonic timestamps;
the new quality diagnostics expose the conditions without changing matching or
detector semantics.

Local validation artifacts are `out/verify_timeline_session.cpp`,
`out/session-verification.txt`, `out/session-anchors.csv`, `out/session-after.json`,
`out/session-smoke.txt`, and `out/build/timeline/Testing/Temporary/LastTest.log`.
The saved input files were not overwritten. The real-anchor regression is checked
in, so that failure remains testable without personal session files.

## Analysis provenance

Newly serialized analyses append `-replay-timeline-2` to `analysis_version`:
`camera-nav-production-macro-3-army-control-group-management-5-army-command-1-ability-activity-1-replay-timeline-2`.
This identifies the monotonic timeline implementation independently of the other
detectors' versions. The older slope-clamped implementation had no timeline suffix;
absence of the suffix is legacy/unspecified provenance, not proof of monotonic
mapping. Outputs made before this provenance follow-up are not retroactively tagged.

`schema_version` remains 4 and the NAV format is unchanged. Existing JSON with the
old analysis version, or no analysis version, remains readable with its stored
metrics intact. Reading a saved analysis does not retime it or change its version.
Re-analysis with the current implementation produces the new provenance string.

Historical metrics can differ when re-analysis maps events in abnormal intervals:

- Army-command timestamps and gap statistics, plus counts/rates if changed timing
  affects session-boundary filtering or replay-derived role classification.
- Ability timestamps and counts/rates when events cross the active-session bounds.
- Production-event and selection matching to live visits, confirmation/extension
  counts, visit timings, worker/army cycle durations, gaps and aggregates.
- Replay binding of control-group edits and resulting scope classification;
  dependent scouting command timing, activity spans, gaps and outcomes.
- Derived timeline views, time-window activity summaries and session aggregates
  that consume any of those changed results.

Recorded QPC timestamps, foreground-active duration and raw camera/navigation
events are unchanged. Purely input-derived metrics are unaffected by this mapping
change. The version distinguishes computation semantics; it does not certify that
the replay/player alignment is accurate.

The serialization regression asserts the complete new version string and unchanged
schema version. A compatibility regression loads JSON with legacy or absent
analysis provenance and checks that stored metrics and source JSON are preserved.

## Changed files

- `src/analysis/replay_timeline.h`: isolated mapping, anchor validation and safe offsets.
- `src/analysis/replay_analysis.cpp`: use the shared helper and populate quality diagnostics.
- `src/analysis/production_visit.h`: diagnostic fields.
- `src/storage/session.cpp`: serialize those diagnostics.
- `tests/test_replay_timeline.cpp`: focused mapping and real-anchor regressions.
- `tests/test_production_visit.cpp`: correlation and diagnostic regression tests.
- `CMakeLists.txt`: register the new test file.
- `docs/replay-timeline-mapping.md`: investigation, behavior and verification report.
