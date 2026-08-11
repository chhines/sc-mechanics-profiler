# Starcraft Mechanics Profiler

Starcraft Mechanics Profiler is a lightweight Windows camera-navigation profiler for StarCraft: Remastered. It records Raw Input only while `StarCraft.exe` owns the foreground window and recognizes control-group jumps, F2/F3/F4 location recalls, minimap clicks, and edge pans.

The profiler does not read game memory, inspect network traffic, inject code, modify input, or infer gameplay decisions. While automatic detection is waiting, it captures only the calibrated minimap rectangle at approximately 20 Hz.

## First-time minimap calibration

The minimap must be calibrated once before minimap clicks can be recognized. No Alt+Tab is required between calibration points:

1. Double-click `Starcraft Mechanics Profiler.exe`.
2. Choose **Calibrate minimap**.
3. Read both instructions, then switch to StarCraft once.
4. Move to the top-left boundary of the clickable minimap and press F9.
5. Move to the bottom-right boundary and press F9.
6. Calibration saves automatically.

F9 is configurable through `calibration.capture_key` in `config.json`. The key is observed but not blocked, so it may still reach StarCraft.

The two captured points are stored as normalized positions relative to StarCraft’s deterministic 4:3 game area:

```json
{
  "calibration": {
    "capture_key": "F9"
  },
  "screen_regions": {
    "minimap": {
      "left_norm": 0.025694,
      "top_norm": 0.739815,
      "right_norm": 0.193750,
      "bottom_norm": 0.962963
    }
  }
}
```

The numbers above are examples. Calibration saves the actual captured values.

## Stable screen geometry

All cursor positions and UI rectangles use inclusive desktop coordinates. The app converts StarCraft’s client rectangle to screen coordinates, then calculates the largest centered 4:3 area inside it. A 1920×1080 client therefore always produces:

```text
Client:                  (0,0) -> (1919,1079)     1920x1080
Derived 4:3 game area:   (240,0) -> (1679,1079)   1440x1080
```

Windows cursor clipping is not used to calculate geometry, so Alt+Tab cannot change the result when the client rectangle is unchanged. The calibrated minimap rectangle is reconstructed from its normalized values whenever StarCraft gains focus.

If Windows briefly reports an unavailable client rectangle during activation, the profiler retries every 100 ms while StarCraft remains active and begins region-based detection as soon as geometry is available. Saved absolute rectangles are not used as a runtime fallback.

## Recording and validation

For automatic recording, choose **Turn automatic detector on** and leave the profiler open. The menu shows whether the detector is ON or OFF and remains available while detection runs. While waiting, the profiler samples only the calibrated minimap at approximately 20 Hz and starts after the white camera viewport outline is detected in two consecutive samples. Sampling stops completely during recording. Windows then stops the session when `Documents\Starcraft\maps\replays\LastReplay.rep` genuinely changes from its start-of-game metadata. Before another game can start, the outline must be absent for two captured samples and then reappear. Each game creates a separate `.nav` session and derived `.json` analysis.

Choose **Turn automatic detector off** to stop automatic mode. If a game is currently being recorded, the existing clean finalization and session-summary behavior is preserved. Exiting the app also turns the detector off cleanly.

To validate your calibration and test all camera-navigation detectors without PowerShell:

1. Double-click `Starcraft Mechanics Profiler.exe`.
2. Choose **Test live detection (debug mode)**.
3. Switch to StarCraft and try minimap clicks, control-group jumps, location hotkeys, and edge scrolling.
4. Press Ctrl+C in the profiler console when finished. You will return to the main menu.

Debug mode prints mouse-button interactions without logging every mouse movement:

```text
LEFT_DOWN x=338 y=869 region=MINIMAP
LEFT_UP   x=338 y=869 region=MINIMAP
LEFT_DOWN x=900 y=500 region=VIEWPORT
```

After returning from Alt+Tab, debug mode reports whether the client, derived game area, and reconstructed minimap are unchanged.

`--debug-navigation` prints detected camera-navigation events immediately:

```text
  12.381  CG_JUMP       group=1
  17.104  MINIMAP       x=338 y=869
  19.881  EDGE_SCROLL   RIGHT duration=242ms
  24.012  LOCATION      F2
```

## Commands

```text
"Starcraft Mechanics Profiler.exe" record [--debug-navigation] [--debug-regions]
    [--show-raw-events] [--save-raw] [--verbose] [--quiet]
"Starcraft Mechanics Profiler.exe" auto [same options as record]
"Starcraft Mechanics Profiler.exe" debug
"Starcraft Mechanics Profiler.exe" calibrate
"Starcraft Mechanics Profiler.exe" config
"Starcraft Mechanics Profiler.exe" summary <latest|session-id>
"Starcraft Mechanics Profiler.exe" compare <session-id> <session-id>
"Starcraft Mechanics Profiler.exe" compare last <N>
"Starcraft Mechanics Profiler.exe" export <latest|session-id> --csv
```

## Session data

Normal recording creates one compact, versioned navigation session and one derived analysis file:

```text
sessions/
  <local-timestamp>.nav
  <local-timestamp>.json
```

The `.nav` file is the source of truth. It contains an `SCNV` header, compact camera-navigation records, and a separate compact stream of discrete mechanical inputs such as accepted key presses, control-group actions, location-hotkey actions, mouse buttons, and wheel events. Mouse movement, cursor polling, key-up events, and focus transitions are not retained in this compact stream. The `.json` file contains compact derived camera, production-visit, and worker/army macro analysis; it never duplicates the mechanical event stream. Summary and comparison commands do not create additional files.

Schema version 5 stores camera and mechanical records in separate sections using the same two time models. Active event time remains pause-excluded for gameplay metrics, while every record also stores its exact QPC offset from a first-active QPC/Unix-nanosecond anchor for future replay synchronization. `sessionStartUnixMs` remains file-creation metadata and must not be combined with active time as an exact wall-clock event timestamp. Versions 1 through 4 remain readable; older files simply have an empty mechanical stream.

## Production visits and macro cycles

After each automatic recording finishes, the profiler uses the hotkey snapshot captured before that game to infer conservative low-level `ProductionVisit` records. It then parses the settled `Documents\Starcraft\maps\replays\LastReplay.rep` with the bundled `icza/screp` helper, identifies the recorded player from the monotonic control-group selection sequence, and correlates replay frames to the live active timeline using matched control-group anchors. Manual `record` mode captures the replay metadata before recording and performs the same bounded analysis only when `LastReplay.rep` genuinely changes during that recording. Replay work runs only after recording has stopped.

Replay-confirmed visits are classified independently by product type (`Worker` or `Army`) and access method (control group, location-hotkey plus click, minimap plus click, or direct screen click). A click visit requires an ordered replay selection transition followed by compatible replay production; proximity to a production command alone is not sufficient. Replay evidence may also recover an otherwise sub-threshold control-group visit when the live group selection, physical key, replay hotkey selection, and replay production agree in order. Nearby same-product visits are grouped into worker or army macro passes. Repeated physical presses remain part of the visit duration even when fewer successful production commands appear in the replay. Real QPC time prevents passes from merging across Alt+Tab, while pause-excluded active time remains available for timeline plotting.

If hotkeys cannot be interpreted, the replay is unavailable, the replay belongs to a different game, or player matching is ambiguous, camera analysis and `.nav` storage continue. Available heuristic production visits are retained, while semantic worker/army macro reporting is marked unavailable instead of guessing.

The bundled replay helper is `screp` v1.13.3 by Andras Belicza, distributed under the Apache License 2.0. Its license and provenance are in `third_party/screp/`.

Raw Input storage is available only when explicitly requested:

```text
"Starcraft Mechanics Profiler.exe" record --save-raw
```

That produces the normal `.nav` and `.json` plus `<local-timestamp>.events.bin` using the unchanged raw-event format. Live debug output does not save raw events unless `--save-raw` is also supplied.

CSV is generated only by the explicit export command and is written under `exports/`:

```text
"Starcraft Mechanics Profiler.exe" export latest --csv
```

## Build and test

From a Developer PowerShell for Visual Studio 2022:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

The project targets Windows 10/11, uses C++20 and CMake 3.22+, and has no third-party runtime dependencies.
