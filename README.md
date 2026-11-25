# Media Pill Taskbar Widget

Native Windhawk mod (`explorer.exe`) that embeds a compact Apple-style media controller pill inside the Windows 11 taskbar (`SystemTrayFrameGrid`).

## Features
- Centered album art filling a `106×34` rounded capsule (`Windows.UI.Composition` clip, handles large 3000×3000+ covers)
- Circular progress arc synced to SMTC players and AIMP live position/duration (`AIMP2_RemoteInfo` + `WM_AIMP_PROPERTY`)
- Centered `Previous`, `Play/Pause`, and `Next` buttons with GPU fade on hover (stays visible when paused or without artwork)
- Right-click context menu & double-click to focus the active player (supports UWP/Store, AIMP, Zen/Firefox, Chrome/Edge, etc.)

## Install
1. Install [Windhawk](https://windhawk.net/).
2. Create a new local mod and paste the contents of `media-pill-taskbar.wh.cpp`.
3. Compile (`Ctrl + B`) and exit editing mode.
