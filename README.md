# Starcraft Mechanics Profiler

Starcraft Mechanics Profiler is a lightweight native Windows mechanical profiler for StarCraft: Remastered. It records Raw Input only while `StarCraft.exe` owns the foreground window, combines physical input timing with replay-derived context, and presents the results in a Dear ImGui + ImPlot desktop interface.

The profiler does **not** read game memory, inspect network traffic, inject code, modify input, or attempt to judge strategic decisions. Replay parsing happens after recording has stopped.

## What it measures

The current profiler focuses on mechanical behavior that can be derived reliably from physical input plus replay context:

- **Camera navigation** — control-group jumps, location-hotkey jumps, minimap jumps, and qualifying edge-pan episodes.
- **Worker and army macro cycles** — continuous production passes across one or more production buildings.
- **Production visits** — one occasion where a specific production building is accessed and at least one detected production attempt is made.
- **Macro access style** — control-group-only, location-hotkey plus click, control-group-center plus click, or mixed.
- **Army control-group management** — replay-confirmed Ctrl+number assignments and Shift+number additions, including how the selection was formed.
- **Scouting-unit activity** — early worker scouts identified and tracked by replay unit identity and command history.

Ambiguous observations are retained internally where useful but omitted from user-facing technique breakdowns rather than being presented as meaningful mechanics.

## Minimap geometry

The profiler includes automatic minimap geometry for both StarCraft display modes:

- **Original Aspect** — geometry is calculated relative to the centered 4:3 game area.
- **Widescreen** — geometry is calculated relative to the full StarCraft client area.

Manual calibration is therefore an **override**, not a required first-run step. If the automatic geometry does not match a particular setup, choose **Calibrate minimap override** in Settings. To return to the built-in geometry for the current display mode, choose **Use automatic minimap**.

Manual calibration requires only one switch into StarCraft:

1. Choose **Calibrate minimap override**.
2. Switch to StarCraft.
3. Move the cursor to the top-left boundary of the clickable minimap and press F9.
4. Move to the bottom-right boundary and press F9.
5. The normalized override is saved automatically for that display mode.

F9 is configurable through `calibration.capture_key` in `config.json`. The key is observed but not blocked, so it may still reach StarCraft.

The calibrated coordinates are normalized relative to the current game area rather than stored as fragile absolute desktop coordinates.

## Stable screen geometry

All cursor positions and UI rectangles use inclusive desktop coordinates. The app converts StarCraft's client rectangle to screen coordinates and, for Original Aspect, calculates the largest centered 4:3 area inside it. A 1920×1080 client therefore produces:

```text
Client:                  (0,0) -> (1919,1079)     1920x1080
Derived 4:3 game area:   (240,0) -> (1679,1079)   1440x1080
```

Windows cursor clipping is not used to calculate the playable area, so Alt+Tab cannot change the result while the client rectangle itself is unchanged. If Windows temporarily reports unavailable geometry during activation, the profiler retries while StarCraft remains active.

## Automatic recording

Choose **Turn automatic detector on** on the Main page. While waiting for a game, the profiler samples only the resolved minimap region at approximately 20 Hz and starts recording after the camera viewport outline is detected consistently. Sampling stops while a game is being recorded.

A recording is finalized when `Documents\Starcraft\maps\replays\LastReplay.rep` genuinely changes from its start-of-game metadata. Each completed game creates a compact `.nav` source file and a derived `.json` analysis file.

The **Minimize to tray** setting controls normal window behavior:

- When enabled, minimizing or closing the window hides it to the notification area and the tray icon remains available.
- When disabled, minimizing behaves like a normal Windows program and remains on the taskbar; closing the window, Alt+F4, or another normal Close action exits the profiler.

Choose **Turn automatic detector off** to stop automatic mode. If a game is currently recording, normal clean finalization is preserved.

## Live validation

Use **Test live detection** from Main to validate camera navigation and region detection without creating a normal automatic session.

Try minimap clicks, control-group jumps, location hotkeys, and edge scrolling in StarCraft, then return to the profiler to inspect the diagnostic log. Debug output intentionally avoids logging every mouse movement.

The retained `--debug-navigation` command-line option prints detected camera-navigation events immediately:

```text
  12.381  CG_JUMP       group=1
  17.104  MINIMAP       x=338 y=869
  19.881  EDGE_SCROLL   RIGHT duration=242ms
  24.012  LOCATION      F2
```

## Results and Analysis

The **Results** page provides the compact game/session summary. Report groups can be enabled or disabled in Settings without changing the underlying recorded data.

The **Analysis** page provides a full-game mechanics timeline with separate tracks for:

- camera navigation,
- worker macro cycles,
- army macro cycles,
- production visits,
- army control-group edits,
- scouting activity.

Production visits use distinct stage markers for access start, building access, first production attempt, and visit end. Timeline tracks can be enabled individually; **Select all** restores every track, while **Fit Game** controls whether the full game is fitted into the visible time range.

Below the timeline, categorical mechanic breakdowns use horizontal frequency bars with distinct category colors for readability:

- Camera Navigation Methods
- Worker Macro Access Styles
- Army Macro Access Styles
- Control-Group Assignment Selection Methods
- Control-Group Addition Selection Methods

Ambiguous `Other`, `Unknown`, and `Existing Selection` observations are omitted from these user-facing breakdowns, and percentages are normalized over the displayed categories.

## Production visits and macro cycles

A **production visit** is one occasion where a specific production building is accessed and at least one detected production attempt/input occurs. It is not the number of units produced.

Internally, the profiler keeps a production-context identity so it can tell which specific building a visit belongs to. That identity is implementation state, not a separate user action or statistic.

A **macro cycle** is a continuous production pass that can contain multiple production visits. For example:

```text
Gateway A -> Dragoon
Gateway B -> Dragoon
Robotics Facility -> Observer
```

can be one army macro cycle containing three production visits.

Replay data is used to validate production meaning and building identity where available. Physical QPC timing remains authoritative for mechanical timing, including time from building access to the first production attempt. Nearby visits are merged only when they satisfy the cycle-continuation rules; returning to the same known production building breaks the current cycle.

Macro access styles describe how the buildings in a cycle were reached:

- **Control Group Only** — visits were accessed directly through production control groups.
- **Location Hotkey Click** — a location hotkey moved the camera to the production area, followed by clicking/selecting buildings from that view.
- **Control Group Center Click** — a production control group was double-tapped to center the camera, followed by selecting other production buildings from that view.
- **Mixed** — more than one recognized access technique was used in the cycle.

## Army control-group management

The profiler correlates physical Ctrl+number and Shift+number operations with replay selection state. Production-building groups, scouting-unit groups, and ambiguous edits are excluded from headline army-control-group statistics.

For reportable army edits, the profiler can distinguish selection methods such as direct click, box select, Ctrl-click type selection, double-click type selection, Shift-click modification, Shift-box modification, and Ctrl+Shift-click type selection.

Timing such as selection-to-operation latency uses physical QPC timestamps rather than replay-frame timing.

## Scouting-unit detection

Scouting is based on **replay unit identity**, not a fragile assumption that a particular control group remains selected.

An early singleton Probe, SCV, or Drone assignment can become a scouting candidate. The replay reconstruction then follows commands issued to that same unit tag. A candidate is confirmed as a scout when its command history moves onto the opponent's side of the map relative to the occupied starting locations — specifically, when a command target is closer to the enemy spawn than to the player's own spawn.

Once confirmed, later left-clicks, selecting other control groups, changing the scout's hotkey, or overwriting that hotkey do not end tracking of the unit itself.

The observed scouting span ends at:

- a confirmed return-home command after the scout's final enemy-side excursion, or
- otherwise the final attributable command issued to that scout.

A temporary return toward home does not end scouting when the same unit later receives another enemy-side command. Brood War replays do not provide an authoritative per-unit death event, so the profiler does not invent a death time.

The return-home region is derived from the own/enemy spawn distance and kept within a bounded base-sized radius. In normal 1v1 replay data the actual occupied opponent start is used; map center is only a compatibility fallback for incomplete synthetic metadata.

## Session data

Normal recording creates one compact, versioned navigation session and one derived analysis file:

```text
sessions/
  <local-timestamp>.nav
  <local-timestamp>.json
```

The `.nav` file is the source of truth. It contains an `SCNV` header, camera-navigation records, and a compact stream of discrete mechanical inputs such as accepted key presses, control-group actions, location-hotkey actions, mouse buttons, and wheel events. Continuous mouse movement, key-up events, and focus transitions are not retained in the compact stream.

The `.json` file contains derived analysis and does not duplicate the complete mechanical event stream. Summary and comparison commands do not create additional files.

Schema version 5 stores camera and mechanical records in separate sections using the same time models. Active event time excludes pauses such as Alt+Tab, while records also preserve QPC-relative timing for replay synchronization. Versions 1 through 4 remain readable.

Replay analysis uses the bundled `icza/screp` helper. Replay work occurs only after recording has stopped. If replay matching is unavailable or ambiguous, camera analysis and `.nav` storage remain valid and semantic replay-dependent statistics fail closed instead of guessing.

Raw Input storage is available only when explicitly requested:

```text
"Starcraft Mechanics Profiler.exe" record --save-raw
```

That produces the normal `.nav` and `.json` plus `<local-timestamp>.events.bin`.

CSV is generated only by explicit export and is written under `exports/`:

```text
"Starcraft Mechanics Profiler.exe" export latest --csv
```

## Commands

A normal no-argument launch opens the desktop GUI. Existing command-line paths remain available for diagnostics and automation:

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

## Build and test

From a Developer PowerShell for Visual Studio 2022:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

The project targets Windows 10/11, uses C++20 and CMake 3.22+, and has no third-party runtime dependencies beyond the components bundled with the application.
