# Volchay-fastcut

Native Windows 11 video editor focused on **instant cold start** and
frame-accurate editing. No Electron, no Qt, no Java, no telemetry.

> Status: V0 skeleton. Plays a single video, renders a Lightroom-style
> dark UI, and registers itself as a Windows context-menu handler for
> common video formats. Multi-track editing, effects and export land in
> later releases.

## Goals

| Aspect             | Volchay-fastcut V0 target | Reference (CapCut)     |
| ------------------ | ------------------------- | ---------------------- |
| Cold start         | < 300 ms                  | 5 – 10 s               |
| Open file from menu to first frame | < 800 ms (1080p) | 6 – 12 s            |
| Distributable size | 2 – 4 MB                  | ~ 500 MB               |
| Background daemons | none                      | several                |
| Shipped runtimes   | none (Windows-only APIs)  | Electron / Chromium    |

The combination chosen makes those targets reachable on a stock Windows 11
machine:

* **Dear ImGui (docking branch) + Direct3D 11** for UI — single static
  binary, no UI toolkit DLL load on startup.
* **Media Foundation** for decoding — already loaded by Windows, supports
  H.264 / HEVC / VP9 / AV1 with hardware acceleration.
* **MinGW-w64** for building from Linux, MSVC supported on Windows.

## Repository layout

```
volchay-fastcut/
├── CMakeLists.txt
├── cmake/
│   └── mingw-w64-toolchain.cmake     # cross-compile from Linux
├── src/
│   ├── main.cpp                      # WinMain entry
│   ├── app/                          # window + app loop
│   ├── render/                       # D3D11 device + swap chain
│   ├── media/                        # Media Foundation player
│   ├── core/                         # editing model (Project, Clip)
│   ├── ui/                           # ImGui theme + panels
│   ├── shell/                        # Windows context-menu registration
│   ├── platform/                     # common Windows headers
│   └── util/                         # logging / timing helpers
├── third_party/
│   └── imgui/                        # vendored Dear ImGui (docking)
├── installer/
│   ├── register-context-menu.bat
│   └── unregister-context-menu.bat
└── docs/
    └── ARCHITECTURE.md
```

## Building

See [BUILD_WINDOWS.md](BUILD_WINDOWS.md) for the full instructions. Quick
versions:

### Cross-compile from Linux (MinGW-w64)

```bash
sudo apt-get install mingw-w64 cmake ninja-build
cmake -S . -B build -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build
# Output: build/volchay-fastcut.exe (and build/volchay-register.exe)
```

### Native Visual Studio 2022 build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

## Running

```
volchay-fastcut.exe                         # start with empty project
volchay-fastcut.exe path\to\video.mp4       # open a video on launch
```

When invoked via the Windows "Open with" menu, Explorer passes the file
path as `argv[1]`, which is exactly what the second form does.

## Registering the right-click context menu

After building, run **once** as the user that should see the new menu:

```
installer\register-context-menu.bat
```

This adds an `Open with Volchay-fastcut` entry to the right-click menu of
`.mp4`, `.mov`, `.mkv`, `.webm`, `.avi`, `.m4v` and `.wmv` files. No admin
rights are needed; the entries are written under
`HKEY_CURRENT_USER\Software\Classes`.

In Windows 11 the entry appears in the legacy menu accessible via
**Show more options** (Shift+F10). A native Win11 shell extension built
on `IExplorerCommand` + a Sparse MSIX package is on the V1 roadmap.

To remove the entries:

```
installer\unregister-context-menu.bat
```

## Logging

Each run appends to `%LOCALAPPDATA%\Volchay\fastcut.log`. The file shows
the cold-start trace (with milliseconds for each phase) followed by any
runtime warnings or errors.

The View → "Show startup trace" menu opens the same data live in the UI.

## License

MIT — see [LICENSE](LICENSE).
