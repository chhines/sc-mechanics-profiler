# Native GUI Refactor Inventory and Checklist

This document inventories the existing user-facing surface before the GUI refactor and tracks
implementation checkpoints. The analysis algorithms and compact `.nav` format are out of scope
for redesign.

## Existing interactive menu

| Console action | Existing implementation | Native GUI equivalent |
| --- | --- | --- |
| Toggle automatic detector | `AutomaticDetectorToggle` / `automaticRecord()` | Main status toggle and tray status/action |
| Calibrate minimap | `runCalibration()` | Settings and Main calibration buttons with progress/status |
| Test live detection | manual `record` with navigation/region diagnostics | Main debug toggle and live diagnostic log |
| Show configuration | print `config.json` | Settings controls plus an advanced open-file action |
| Show latest session summary | latest `_session.txt` | Results: latest game and current/latest session views |
| Show command-line help | `printUsage()` | About / command-line reference |
| Exit | stop automatic thread and return | Main and tray Exit with clean resource shutdown |

Closing the GUI will hide it to the notification area. Exit remains explicit.

## Existing command-line surface retained for debug/automation

- `record` with `--debug-navigation`, `--debug-regions`, `--show-raw-events`, `--save-raw`,
  `--verbose`, and `--quiet`.
- `auto` with the recording options above.
- `debug`.
- `calibrate` (`detect-layout` alias).
- `config`.
- `summary <latest|session-id>`.
- `compare <session-id> <session-id>` and `compare last <N>`.
- `export <latest|session-id> --csv`.
- `help`, `--help`, and `-h`.

The normal no-argument entry point becomes the GUI. Argument-driven CLI behavior remains routed
through the existing command implementation.

## Existing configuration inventory

Core `config.json` settings:

- StarCraft process executable name.
- Control-group double-tap interval.
- Location recall hotkeys.
- Automatic screen-region use and calibration-derived geometry.
- Normalized calibrated minimap rectangle.
- Calibration capture key.
- Edge-pan margin and minimum dwell.
- Storage flush interval.

GUI preferences will use a separate compact `gui-config.json` so corrupt presentation state cannot
prevent the profiler core from starting. It will contain:

- Visible report/statistic groups.
- Minimize-to-tray preference.
- Last valid main-window position and size.

Missing or corrupt GUI preferences fall back to safe defaults.

## Current architecture

```text
Raw Input Collector / Minimap Start Monitor / LastReplay Watcher
                             |
                             v
Analyzer + Production / Replay / Macro / Control-Group Analysis
                             |
                             v
       SessionWriter + AnalysisResult + ProductionAnalysis
                  /                         \
        .nav/.json storage            Console reporting
```

The analysis and storage layers are already mostly presentation-independent. The main coupling is
inside `src/cli/commands.cpp`, where recording/automatic lifecycle state, console output, and
thread ownership currently live together. The refactor will expose a reusable controller and
state-change callbacks while retaining the existing CLI wrappers.

Target presentation flow:

```text
Existing capture / lifecycle / replay pipeline
                    |
                    v
       Application controller/state
             /                \
       Win32 GUI/tray       Existing storage/CLI reports
```

Metric calculations stay in the existing analysis modules. Win32 controls consume derived state
or JSON/view models only.

## GUI information architecture

- **Main:** automatic detector state, StarCraft foreground state, recording/replay state, latest
  result availability, start/stop, calibration, live detection, open data folder, and Exit.
- **Results:** Latest Game and Session selectors; camera navigation, worker macro, army macro,
  access style, army control-group, and scouting activity sections. Native GDI bars accompany
  textual values for distributions and timing comparisons.
- **Settings:** core validated settings, calibration, report-group visibility, Select all,
  Reset defaults, Save, and advanced config-file access.
- **About:** version/build, CLI reference, data/timing model, and grounded explanations and
  limitations for every statistic group.
- **Tray:** Open, concise current status, automatic detector toggle, open latest result, and Exit.

## Checkpoints

- [x] 1. Inventory console actions, configuration, reports, lifecycle, and architecture.
- [x] 2. Add tested GUI preferences and UI-independent results/view-model derivation.
- [x] 3. Expose reusable controller/state callbacks while preserving CLI behavior.
- [x] 4. Add native Win32 window, pages, tray icon, restore/hide, and explicit Exit.
- [x] 5. Port every interactive console action and existing setting.
- [x] 6. Add latest-game/session results and minimal native visualizations.
- [x] 7. Add About/statistics help and retained command-line reference.
- [x] 8. Persist preferences and window placement; handle missing/corrupt state safely.
- [x] 9. Run full build/tests and programmatic/manual GUI validation.
- [x] 10. Replace the canonical executable without changing its filename.

## Progress log

- Inventory completed: existing menu, CLI commands/options, core configuration, automatic
  lifecycle, replay readiness/correlation, session storage, aggregate reporting, and analysis JSON
  categories inspected.
- Added a fault-tolerant, tested `gui-config.json` model and report-group-filtered game/session
  results view model. Missing/corrupt preferences use defaults.
- Added a reusable application controller over the existing recorder, automatic lifecycle, replay
  correlation, storage, and session aggregation. State/result/diagnostic callbacks replace GUI-side
  console parsing; CLI wrappers remain available.
- Added the native Win32 shell with Main, Results, Settings, and About pages, notification-area
  behavior, event-driven updates, restrained GDI bars, settings validation, report visibility,
  calibration/live-debug controls, and explicit clean Exit.
- Release build and complete automated suite pass (192/192 tests). The executable is linked as a
  Windows GUI subsystem application, so normal launch opens no console window.
- Programmatic shell validation confirmed main-window launch, four-page control inventory,
  About/statistics help, close-to-tray, process continuity while hidden, tray restore, tray Exit,
  and window-placement persistence across restart.
- Programmatic lifecycle validation confirmed automatic detection reaches Waiting for game,
  remains active while hidden, stops through the tray command, and exits cleanly.
- Visual captures of Main, Results, Settings, and About were inspected. Results show existing
  finalized-game data with native distribution/timing bars and adjacent numeric values.
- Not exercised without an active StarCraft game: capturing two real minimap calibration points,
  producing live debug events from StarCraft Raw Input, and completing an automatic game through a
  real LastReplay change. Their underlying paths retain existing tested collector/lifecycle code;
  the GUI actions and callbacks were verified up to the required foreground-game boundary.
- Replaced the root `Starcraft Mechanics Profiler.exe` with the verified release build under the
  same filename; its SHA-256 matches the build output.
