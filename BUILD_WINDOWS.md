# Building Volchay-fastcut

There are two supported build paths. Both produce the same artefact:
`volchay-fastcut.exe` (and a tiny `volchay-register.exe`) statically linked
against libstdc++ / libgcc / pthreads when built with MinGW, or the MSVC
runtime when built with Visual Studio.

## 1. Cross-compile from Linux with MinGW-w64

This is the path used by the project's CI and the easiest way to produce
a Windows binary if you only have a Linux box.

### Prerequisites

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
        mingw-w64 mingw-w64-tools \
        cmake ninja-build
```

The `g++-mingw-w64-x86-64-posix` package (pulled in by `mingw-w64`) is
required because we use `std::thread`, `std::mutex`, etc.

### Build

```bash
cd /path/to/volchay-fastcut
cmake -S . -B build -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Output:

```
build/volchay-fastcut.exe   # main editor
build/volchay-register.exe  # context-menu registrar
```

Both are stripped, statically linked, and require no extra DLLs beyond
those that ship with Windows.

### Verifying the binary

You can inspect a Linux-cross-compiled binary without running it:

```bash
file       build/volchay-fastcut.exe
x86_64-w64-mingw32-objdump -p build/volchay-fastcut.exe | grep "DLL Name"
```

The DLL list should contain only `KERNEL32.DLL`, `USER32.DLL`, `D3D11.DLL`,
`MFPLAT.DLL`, etc. — all system libraries.

To run on Windows, copy `build/volchay-fastcut.exe`,
`build/volchay-register.exe`, `installer/register-context-menu.bat` and
`installer/unregister-context-menu.bat` to a folder of your choice and
launch the editor.

## 2. Native Visual Studio 2022 build

### Prerequisites

* **Visual Studio 2022** with the *Desktop development with C++* workload
  (any edition: Community is fine).
* The Windows 10/11 SDK (10.0.19041 or newer).

CMake 3.20+ is bundled with VS 2022.

### Build

From a "x64 Native Tools Command Prompt for VS 2022":

```
cd path\to\volchay-fastcut
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output: `build\Release\volchay-fastcut.exe`,
`build\Release\volchay-register.exe`.

Open the generated `.sln` in Visual Studio if you want IDE debugging.

## 3. Smoke test on Windows 11

After deploying the two `.exe` files plus the two `.bat` files to a folder
of your choice (for example `C:\Programs\Volchay`):

1. **Cold start.** Double-click `volchay-fastcut.exe`. The window should
   appear in well under a second. The status strip in the bottom-right
   corner displays the measured cold-start time, and the same number is
   logged to `%LOCALAPPDATA%\Volchay\fastcut.log`.

2. **Open a video by argument.** From `cmd`:
   ```
   volchay-fastcut.exe "C:\path\to\some video.mp4"
   ```
   Playback should start automatically.

3. **Drag & drop.** Drag a video file onto the running window.

4. **Context menu.** Run `register-context-menu.bat` once, then right-click
   any `.mp4` / `.mov` / `.mkv` / `.webm` / `.avi` file and choose
   *Open with Volchay-fastcut* (Win11: "Show more options" → entry).

5. **Inspect performance.** View → "Show startup trace" prints each phase
   of startup with millisecond resolution.

## 4. Troubleshooting

* **`The application was unable to start correctly (0xc000007b)`** —
  you are running a 32-bit `volchay-fastcut.exe` on a 64-bit system or
  vice-versa. The build always produces a 64-bit binary; double-check
  that you copied the right file.

* **`MFCreateSourceReaderFromURL failed 0xC00D36C4`** — the file uses
  a codec that the Windows Media Foundation stack does not support
  out-of-the-box (e.g. some HEVC files require the
  *HEVC Video Extensions* package from the Microsoft Store).

* **No first frame appears** — check `%LOCALAPPDATA%\Volchay\fastcut.log`
  for an error from the source reader; failure messages there usually
  explain what went wrong.

* **Context-menu entry is missing** — Windows 11 hides legacy entries
  unless you click "Show more options". A native (non-legacy) handler
  requires a Sparse MSIX package and is planned for V1.
