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

For automatic recording, choose **Turn automatic detector on** and leave the profiler open. The menu shows whether the detector is ON or OFF and remains available while detection runs. While waiting, the profiler samples only the calibrated minimap at approximately 20 Hz and starts after the white camera viewport outline is detected in two consecutive samples. Sampling stops completely during recording. Windows then stops the session when `Documents\Starcraft\maps\replays\LastReplay.rep` genuinely changes from its start-of-game metadata. Before another game can start, the outline must be absent for two captured samples and then reappear. Each game creates a separate `.nav` session.

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

Normal recording creates exactly one compact, versioned navigation session:

```text
sessions/
  <local-timestamp>.nav
```

The `.nav` file is the source of truth. It contains an `SCNV` header, compact camera-navigation records, and a separate compact stream of discrete mechanical inputs such as accepted key presses, control-group actions, location-hotkey actions, mouse buttons, and wheel events. Mouse movement, cursor polling, key-up events, and focus transitions are not retained in this compact stream. Summary and comparison commands continue to use the existing camera metrics and do not create JSON or CSV files.

Schema version 5 stores camera and mechanical records in separate sections using the same two time models. Active event time remains pause-excluded for gameplay metrics, while every record also stores its exact QPC offset from a first-active QPC/Unix-nanosecond anchor for future replay synchronization. `sessionStartUnixMs` remains file-creation metadata and must not be combined with active time as an exact wall-clock event timestamp. Versions 1 through 4 remain readable; older files simply have an empty mechanical stream.

Raw Input storage is available only when explicitly requested:

```text
"Starcraft Mechanics Profiler.exe" record --save-raw
```

That produces the normal `.nav` plus `<local-timestamp>.events.bin` using the unchanged raw-event format. Live debug output does not save raw events unless `--save-raw` is also supplied.

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
