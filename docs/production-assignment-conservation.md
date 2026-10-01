# Replay production assignment conservation

## Cause and assignment path

Control-group candidates and click/direct-selection candidates converge on the same
assignment block in `correlateProductionVisitsWithReplay`.

1. `shortWindowPhysicalPresses` is the initial candidate's physical-key count. The
   first replay scan finds unused, key-compatible events within the replay context
   and time window, up to that provisional count.
2. `collectConfirmedPhysicalBurst` uses those replay commands as semantic evidence
   for physical keys. It walks forward from the physical context, stopping at hard
   context boundaries, incompatible/nonproduction keys, excessive continuation
   gaps, or QPC/active-time pause discrepancies. It can extend the initial burst;
   it does not manufacture presses from replay events.
3. `applyConfirmedPhysicalBurst` installs the confirmed keys, count, and timing.
   Filtering removes replay commands incompatible with these keys; resizing caps
   the provisional assignment at the confirmed physical count.
4. The second scan fills unused capacity using the confirmed visit's time window
   and keys. Previously its insertion came before its capacity check:

   ```cpp
   assigned.push_back(production.event);
   if (assigned.size() >= physicalPresses.size())
       break;
   ```

   An already-full N-command assignment could therefore become N+1 as soon as
   another eligible event was encountered. The check now runs at the top of the
   loop, before eligibility scanning or insertion. An underfilled assignment can
   still fill exactly its remaining slots.
5. Only the final assigned event pointers are marked `production.used`. Both
   scans reject used events; the second also rejects pointers already assigned
   to this candidate. Replay context boundaries further partition candidates.
6. `applyReplayEvents` is the only production-analysis writer of
   `replayProductionCommands`; it sets the count from the assigned vector, adds
   one produced-unit label per command, and marks the visit replay-confirmed.
   `matchedReplayProductionEvents` increases by that same vector size. Unmatched
   replay commands are counted from unused mapped events, and matched/unmatched
   visits from final replay-confirmed flags.

No other overassignment path was found in the normal fresh-analysis pipeline.
The first scan is provisional and already capped after burst confirmation; both
final application branches share the corrected second scan. This does not add a
missing-input exception or change compatibility, ordering, timing, or context rules.

## Reproduction and tests

Production source was unchanged when the new regression suite first ran:
426/429 cases passed. The one-Q/two-Dark-Templar test, capacity matrix, and
overlapping-window test failed conservation assertions. The burst-extension fill
test passed before and after the fix.

The exact regression has one Q press and two otherwise eligible Dark Templar
commands. Before: one physical press, two assigned commands. After: one physical
press, one assigned command/unit label, one unmatched replay command.

Added cases in `test_production_visit.cpp`:

- `one physical Q press cannot receive two Dark Templar replay commands`: the
  already-full second-pass regression, confirmation, labels, and accounting.
- `production assignment conserves exact and excess capacity for group and click visits`:
  both access paths, N=1 and N=3, exact capacity and two excess commands; an
  incompatible Probe command precedes compatible commands and remains unmatched.
- `second production scan fills only slots added by confirmed burst extension`:
  three initial slots extend to six physical presses; six of eight commands match,
  with three extended presses and two unmatched commands.
- `overlapping production windows retain excess events without double assignment`:
  two nearby Q candidates receive distinct Dark Templar/Observer evidence; the
  extra Dark Templar remains unmatched across the context boundary.

The shared correlation helper now checks conservation, label counts, visit
confirmation totals, and command accounting across existing correlation fixtures.
The serialization test expects `production-macro-4`. The visualization legacy-read
test covers absent provenance and both older macro-3 variants, including a saved
one-press/two-command discrepancy; reading leaves the stored JSON unchanged.

The replay integration smoke tool now reports and checks conservation, unit-label
counts, command accounting, and visit accounting. For available correlation:

```text
sum(visit.replayProductionCommands) = matchedReplayProductionEvents
sum(visit.producedUnits.size()) = matchedReplayProductionEvents
matchedReplayProductionEvents + unmatchedReplayProductionEvents
    = matched-player replay events with known production classification
matchedProductionVisits + unmatchedProductionVisits = productionVisits.size()
```

Unknown-classification and other-player commands are excluded from the eligible
total. Unavailable correlation is not subject to these matched-player checks.

Validation used the existing Ninja/LLVM-MinGW build in `out/build/timeline`:

```powershell
& out/tools/cmake/data/bin/cmake.exe --build out/build/timeline --target starcraft_mechanics_profiler_tests starcraft_mechanics_profiler_replay_smoke -j 4
& out/tools/cmake/data/bin/ctest.exe --test-dir out/build/timeline --output-on-failure
```

Final result: **429/429 test cases passed**, including all production/macro cases;
CTest passed its full registered suite. `git diff --check` passed.

## Saved-session validation

All 12 saved NAV sessions were attempted with their corresponding autosaved
replays (the first replay completion after each session start). Eleven had
available correlation; `2026-08-23_204244` had no high-confidence player sequence
match and zero production visits. Both runs used the same current hotkey profile,
bundled screp v1.13.3 parser, and replay-timeline-2 mapping. Only assignment capacity
changed between the before/after runs. Original NAV, replay, and saved JSON evidence
was not modified.

| Session | Visits | Violations before | Violations after | Matched before | Matched after |
|---|---:|---:|---:|---:|---:|
| 2026-08-17_234952 | 100 | 0 | 0 | 344 | 344 |
| 2026-08-23_185246 | 293 | 34 | 0 | 695 | 661 |
| 2026-08-23_192221 | 139 | 5 | 0 | 210 | 205 |
| 2026-08-23_194405 | 160 | 9 | 0 | 288 | 279 |
| 2026-08-23_200544 | 81 | 3 | 0 | 214 | 211 |
| 2026-08-23_201648 | 163 | 7 | 0 | 359 | 352 |
| 2026-08-23_204244 (unavailable) | 0 | 0 | 0 | 0 | 0 |
| 2026-08-23_210916 | 54 | 3 | 0 | 195 | 192 |
| 2026-08-23_225312 | 83 | 0 | 0 | 191 | 191 |
| 2026-08-24_003724 | 53 | 2 | 0 | 187 | 185 |
| 2026-08-24_004520 | 115 | 6 | 0 | 412 | 406 |
| 2026-08-24_005941 | 40 | 0 | 0 | 102 | 102 |
| **Total** | **1,281** | **69** | **0** | **3,197** | **3,128** |

Unmatched commands increased from **1,155 to 1,224**, exactly the 69 removed
assignments. All 4,352 eligible commands remain accounted for. The original saved
JSON corpus also contains 69 violations. One saved example in `2026-08-23_185246`
at active time 415461.8383 ms has control group 6, physical key 81 (Q), one physical
press, and two Dark Templar labels.

All 11 available after-runs pass the smoke tool's accounting checks. Matched visits
remain 1,166; unmatched visits 115; extended visits 143; extended physical presses
1,297; worker cycles 372; army cycles 343. The local before/after CSV files and
per-game smoke logs are in `out/production-before*` and `out/production-after*`;
`out/validate-production-corpus.ps1` records the local replay pairings and invocation.

## Provenance and limits

`analysis_version` advances only `production-macro-3` to `production-macro-4`.
`schema_version` and `replay-timeline-2` are unchanged. Older analyses remain readable
without rewriting their stored metrics.

The correction changes matched/unmatched command counts and attached produced-unit
labels. Production classifications, macro grouping/timing, and derived session
aggregates can consequently differ; observed visit and cycle counts in this corpus
did not change. Conservation does not establish that each surviving command belongs
to the correct physical press or building context. Missing physical input remains
a separate capture-quality uncertainty, not permission to invent matched presses.
