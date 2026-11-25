# amd-vram-replay

English | [日本語](README.ja.md) · Download (Windows zip): [BOOTH](https://4yubootg.booth.pm/items/9218665)

A tiny instant-replay recorder for Windows and AMD Radeon GPUs that keeps the replay buffer **in VRAM**.

The screen is captured by the AMD driver itself (AMF DisplayCapture), with DXGI Desktop Duplication as a fallback. It is encoded on the GPU with AMD's AMF encoder (through FFmpeg), and the encoded packets go into one big `ID3D11Buffer` ring on the graphics card. Raw frames never touch system memory, and only the small encoded packets pass through it on their way in. Press a hotkey, and the last *N* seconds are read back and written to an MP4 with no re-encoding.

The idea is the same as ShadowPlay's or Adrenalin's Instant Replay, built from FFmpeg's libraries instead of a vendor app.

The app's UI is in Japanese.

## Features

- **AMD driver capture**: frames come straight from the display driver (AMF DisplayCapture via FFmpeg's `vsrc_amf`) into the encoder, the same path Adrenalin uses. Desktop Duplication is the fallback (`--capture=dda` forces it).
- **Instant replay**: keeps the last 5 to 180 seconds (default 30) and saves them with `Ctrl+Alt+Shift+F10`.
  - Clips are named after the game in front, for example `Overwatch_2026-11-25_07-46-47.mp4`.
- **Manual recording**: `Ctrl+Alt+Shift+F11` starts and stops a recording that is written to disk as it goes.
  - It is a fragmented MP4, so a crash still leaves a playable file.
- **Screenshots**: `Ctrl+Alt+Shift+F1` saves a PNG of the current frame.
- **On-screen notifications**: a small panel slides in at the top right when you save a replay, start or stop a recording, or take a screenshot.
  - The panel is excluded from capture, so it never ends up in your clips.
  - It shows over borderless games, but not over exclusive fullscreen.
- **Audio**:
  - Desktop audio (WASAPI loopback) and a microphone, mixed into AAC.
  - It follows default-device changes, and you can pick the microphone yourself.
  - The microphone can be boosted up to +30 dB, with a soft limiter.
- **Clip editor (`VramEdit`)**: a small local web UI that opens in an Edge app window.
  - Gallery with a per-game filter, playback, trim/split/cut on a filmstrip timeline.
  - Lossless (stream copy) or frame-accurate GPU export.
  - A settings page with a live mixer: vertical peak meters and faders that apply instantly.
- **Stays out of the game's way**:
  - The capture and encode work run at below-normal GPU scheduling priority.
  - The app sits in the tray, can start at logon (a Task Scheduler task), and holds a VRAM ring of about 60 MB to 1.9 GB depending on length and quality (336 MB at the default 30 s / 25 Mbps).

## Requirements

- Windows 10 2004 or later (Windows 11 recommended).
- An AMD GPU with AMF (`h264_amf`). Other encoders can be selected with `--codec=` but are untested.
- To build:
  - [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw) (UCRT).
  - A shared FFmpeg build with headers and import libraries (tested with FFmpeg 9.1, the Gyan.dev "full shared" build).
  - Both can be installed with winget:

    ```
    winget install MartinStorsjo.LLVM-MinGW.UCRT Gyan.FFmpeg.Shared
    ```

## Build

```sh
sh build.sh
```

This produces `bin/VramReplay.exe` (the recorder) and `bin/VramEdit.exe` (the editor), copies `editor.html` and the FFmpeg DLLs next to them. `build.sh` finds the winget install locations automatically. Stop both programs before rebuilding, because Windows locks running executables.

## Usage

Run `bin/VramReplay.exe`. It lives in the notification area; right-click the icon for the menu.

| Action | Hotkey |
|---|---|
| Save the replay | `Ctrl+Alt+Shift+F10` |
| Start / stop recording | `Ctrl+Alt+Shift+F11` |
| Screenshot | `Ctrl+Alt+Shift+F1` |

Settings are stored in `%LOCALAPPDATA%\VramReplay\config.ini` and are easiest to change from the editor's settings page. The log is in the same folder. Clips go to `Videos\VramReplay` by default.

### Control pipe

Other tools (stream decks, scripts) can drive the recorder through the named pipe `\\.\pipe\VramReplay`, one command per connection. Each command returns a JSON reply:

| Command | What it does |
|---|---|
| `save [seconds]` | Save the replay (optionally only the last *N* seconds) |
| `record start` / `record stop` / `record` | Control manual recording; `record` toggles |
| `screenshot` | Save a PNG of the current frame |
| `status` | Recorder state, ring usage and recording time |
| `level` | Live audio peak levels |
| `gain <desktop %> <mic %>` | Change volumes live |
| `restart` | Restart and re-read `config.ini` |
| `quit` | Exit |

PowerShell example:

```powershell
$c = New-Object IO.Pipes.NamedPipeClientStream('.', 'VramReplay', 'InOut'); $c.Connect(2000)
$w = New-Object IO.StreamWriter($c); $w.AutoFlush = $true; $w.Write('save')
(New-Object IO.StreamReader($c)).ReadLine(); $c.Dispose()
```

## How it works

```
AMD driver capture (vsrc_amf) ─────────► h264_amf (GPU colour conversion + encode)
  └ fallback: Desktop Duplication ─► D3D11 texture ─┘
                                              │ encoded packets
WASAPI loopback + mic ─► resample/mix ─► AAC ─┤
                                              ▼
                              ring buffer in VRAM (ID3D11Buffer)
                                              │  hotkey
                                              ▼
                       staging read-back ─► MP4 mux (stream copy)
```

- `src/video.cpp`: capture loop, frame pacing, and the AMF encoder.
  - Every frame gets its own texture, because AMF mis-encodes slices of a texture array.
  - The replay is cleared when capture resumes after the lock screen or UAC.
- `src/ring.cpp`: the VRAM ring. Packets never wrap; a small index stays in RAM.
- `src/saver.cpp`, `src/mux.cpp`: read-back and MP4 muxing.
- `src/recorder.cpp`: manual recording; a writer thread muxes queued packet copies to disk.
- `src/audio.cpp`: WASAPI capture, mixing, limiter and AAC.
- `src/overlay.cpp`: the notification panel, a layered, click-through window that is excluded from capture.
- `src/tray.cpp`: tray icon, hotkeys, foreground-app tracking for clip names, and autostart.
- `editor/`: the editor's HTTP server (127.0.0.1 only) and its single-page UI.

## Limitations

- Only one monitor is captured (the first output of the GPU; `--monitor=N` to change).
- Notifications can't appear over exclusive-fullscreen games. Vendor overlays get there by hooking the game's renderer, which this project deliberately doesn't do, because anti-cheat systems may flag it.
- The UI text is Japanese only.

## License

MIT for the code in this repository; see [LICENSE](LICENSE). FFmpeg is not included. It is loaded as shared libraries, under LGPL or GPL depending on the build you use, so keep that in mind if you redistribute binaries.
