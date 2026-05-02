# Architecture

This document describes how the V0 build is structured and the trade-offs
behind the choices that affect cold-start latency and rendering
correctness.

## Process model

Volchay-fastcut runs as a single Win32 process. There is **no UI
process / engine process split**, no sandbox, and no IPC. Cross-process
overhead is one of the largest contributors to slow start in Electron
based editors and we deliberately avoid it.

## Threading

V0 runs everything on the UI thread:

* Win32 message pump
* ImGui frame
* Direct3D 11 immediate context
* Media Foundation `IMFSourceReader::ReadSample` (synchronous mode)

This keeps the implementation small and predictable. A V1 audio stream
will require a dedicated decode thread and a producer/consumer queue;
that is straightforward to bolt on because the project model is already
mutation-by-value friendly.

## Cold-start path

`wWinMain` does the following, in order:

1. Initialise `StartupTrace` (`QueryPerformanceCounter` based clock).
2. Open / create the log file under `%LOCALAPPDATA%\Volchay`.
3. `CoInitializeEx(APARTMENTTHREADED)`.
4. `SetProcessDpiAwarenessContext(PerMonitorV2)`.
5. Parse `CommandLineToArgvW` for an optional video path.
6. Create the `App`, which:
   1. Registers the window class and `CreateWindowExW`.
   2. `ShowWindow` — at this point the user already sees a blank window.
   3. `D3D11CreateDevice` (HW first, WARP fallback) and
      `CreateSwapChainForHwnd`.
   4. `ImGui_ImplWin32_Init` + `ImGui_ImplDX11_Init`.
7. If a path was passed in, open the video.
8. Enter the message loop.

`MFStartup` is **not** called here; it runs lazily the first time the
user opens a video. That avoids paying ~30 ms of MF initialisation for
sessions that only edit existing projects.

The status strip at the bottom of the window shows the running cold-start
total. View → "Show startup trace" displays each milestone live.

## Rendering

Direct3D 11 with a flip-discard swap chain at the window resolution.
ImGui's official DX11 backend handles draw lists.

For the video preview the `MfPlayer` decodes into a dynamic
`B8G8R8A8_UNORM` texture and gives the UI a stable
`ID3D11ShaderResourceView`. The Viewer panel calls
`ImDrawList::AddImage` directly so the video is rendered as part of the
existing ImGui draw call (no extra render pass). NV12 + a YUV→RGB pixel
shader (zero-copy GPU path) is on the V1 list.

## Editing model

```
Project
├── std::vector<Media>   (registered files: path + probed metadata)
└── std::vector<Clip>    (placements on the (single) timeline)
```

`Clip` holds a `(media_id, src_in, src_out)` slice and a `(t_in, speed)`
position on the timeline, plus a per-clip `volume`. The model is
intentionally tiny in V0 — no tracks, no transitions, no effects — and
will grow incrementally.

`TimeUs` is `int64_t` microseconds. We chose microseconds (rather than
the MFTIME 100 ns ticks) because they round-trip cleanly to floating
point seconds for UI display and are wide enough for any real timeline.

## Shell integration

`volchay-register.exe` writes `HKEY_CURRENT_USER\Software\Classes` keys:

* `Volchay.Fastcut.Video.1` ProgID with a `shell\open\command`.
* For each registered extension:
  * `…\<ext>\OpenWithProgids\Volchay.Fastcut.Video.1`
  * `SystemFileAssociations\<ext>\shell\OpenWithVolchayFastcut\command`

This makes the editor appear in:

* the Open-with picker;
* the legacy right-click menu (under "Show more options" on Windows 11).

A native Windows 11 entry requires a Sparse MSIX package + an
`IExplorerCommand` COM handler signed with a trusted certificate. That
work is tracked separately and lives in V1.

## Build system

CMake is the source of truth. The MinGW toolchain file
`cmake/mingw-w64-toolchain.cmake` is used unchanged by the CI workflow
on Ubuntu and by anyone cross-compiling locally. The Visual Studio
generator works on Windows without the toolchain file because CMake
auto-detects the host. Both paths use the same CMakeLists.

Static linking of libstdc++ and libgcc is enabled in the toolchain file
so the produced `.exe` has no extra runtime dependencies.

## What is intentionally missing in V0

* Audio playback (the source reader is video-only right now).
* Hardware-decode path (we set `MF_SOURCE_READER_DISABLE_DXVA = TRUE`
  in V0 for predictable behaviour during initial development; this will
  flip back on once the NV12 shader path lands).
* Multi-track timeline.
* Transitions, effects, colour grading.
* Export.
* Project save / load (the data model already serialises cleanly to
  JSON; we just have not wired it up).
* Sparse MSIX package and IExplorerCommand handler for the new Windows
  11 right-click menu.

Each item above has been considered and budgeted; none of them block the
cold-start goal which is the differentiator we care about.
