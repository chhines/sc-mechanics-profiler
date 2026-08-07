# scmechanics

`scmechanics` is a lightweight Windows console application that profiles mechanical execution in StarCraft: Remastered from keyboard and mouse input alone. It records only while the configured StarCraft executable owns the foreground window.

The profiler does not read process memory, inspect network traffic, capture the screen, inject code, modify input, automate actions, or make gameplay judgments. Its PACs, context switches, production attempts, micro bursts, and EAPM are explicitly input-derived inferences.

## Requirements

- Windows 10 or Windows 11
- Visual Studio 2022 Build Tools (Desktop development with C++)
- CMake 3.22 or newer
- A C++20 compiler

## Build and test

From a Developer PowerShell for Visual Studio:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

The debug executable is written to `out/build/windows-debug/Debug/scmechanics.exe`. For an optimized build, use the `windows-release` configure and build presets.

The project has no third-party runtime dependencies.

## First use

Run commands from the directory where you want `config.json` and `sessions/` to live. The first command creates a default `config.json` if one is not present.

Calibrate the three screen regions before recording:

```powershell
scmechanics calibrate
```

Calibration only reads the cursor position after each Enter press. It does not capture or inspect the screen.

Then start a session:

```powershell
scmechanics record
```

The recorder waits until `StarCraft.exe` is in the foreground. It pauses immediately when another process takes focus and resumes when StarCraft returns. Press Ctrl+C in the console to finish, persist the remaining buffered events, and print the report.

## Commands

```text
scmechanics record
scmechanics calibrate
scmechanics config
scmechanics summary latest
scmechanics summary <session-id>
scmechanics compare <session-id> <session-id>
scmechanics compare last <N>
scmechanics export <session-id> --csv
scmechanics export latest --csv
```

Recording diagnostics are opt-in:

```text
--verbose
--show-raw-events
--show-logical-events
--show-pacs
--show-macro
--quiet
```

Normal recording does not print event logs or continuously refresh metrics.

## Macro configuration

Worker and army production are inferred only from mappings you configure. For example:

```json
"macro": {
  "recognition_interval_ms": 750,
  "episode_gap_ms": 2000,
  "worker": [
    {"group": 4, "train_keys": ["P"]}
  ],
  "army": [
    {"group": 5, "train_keys": ["D", "Z"]},
    {"group": 6, "train_keys": ["D", "Z"]},
    {"group": 7, "train_keys": ["D", "Z"]},
    {"group": 8, "train_keys": ["O", "S", "V"]}
  ]
}
```

These mappings indicate input sequences only; the profiler does not know which buildings or units are present in the game.

## Session data

Each recording produces:

```text
sessions/<local-timestamp>/
  summary.json
  events.bin
  logical_events.bin
  metrics.csv
```

`events.bin` and `logical_events.bin` start with a versioned header containing the event size and QueryPerformanceCounter frequency. Raw events retain scan codes and virtual-key codes; they are never interpreted as typed text. `summary.json` uses schema version 1 and analysis version 0.1.0.

CSV exports are copied to `exports/scmechanics_<session-id>.csv`.

## Architecture and safety properties

- A message-only window receives Windows Raw Input without hooks or interception.
- Foreground-window identity is checked synchronously before a keyboard or mouse packet can enter the collector queue.
- The collector timestamps compact fixed-size events with QueryPerformanceCounter and pushes them into a preallocated SPSC ring buffer.
- The analyzer runs independently of the collector and produces normalized logical events and metrics.
- A third execution path drains bounded storage queues and performs batched binary writes and periodic flushes.
- Queue overflow is never silent; every overflow increments the prominently reported dropped-event count.
- No disk I/O, JSON work, metric calculation, per-event allocation, or console printing occurs in the Raw Input callback.

Statistics use deterministic R-7 percentile interpolation. Distributions are reported as `N/A` until at least five observations exist. Load-bin metrics additionally use the configured minimum observation count (10 by default), and mechanical lapses require at least 20 observations.

## Metric terminology

The output intentionally uses terms such as **Inferred PAC**, **Inferred context switch**, **Input-derived EAPM**, **Worker production attempt**, **Army production attempt**, **Probable re-selection**, **Micro-burst heuristic**, **Estimated mechanical capacity breakpoint**, and **Late-session change**. None of these imply knowledge of game state or correctness.
