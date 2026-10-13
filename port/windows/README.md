# Windows

`ninja windows` compiles the game with clang for 32-bit x86 Windows. The
result is a native executable, `build/windows/halo.exe`, with `SDL3.dll`
next to it. You must build on Windows.

The Windows build uses the platform layer of the Linux build
(`port/linux/src`). The graphics, the sound, the input, the settings and
the multiplayer functions are the same as on Linux. Refer to
[port/linux/README.md](../linux/README.md).

## Requirements

You do not need the Xbox SDK.

- Visual Studio or the Visual Studio Build Tools, with the C++ workload.
  The build uses the 32-bit (x86) MSVC libraries and a Windows 10 or 11
  SDK. It does not use the MSVC compiler.
- LLVM (clang and lld), Python 3 and ninja in the `PATH`. For example,
  enter `scoop install llvm python ninja`.
- A network connection for the first build. `configure.py` downloads the
  Visual C++ development package of SDL 3.4.16 to
  `build/windows/third_party`.

## Build the game

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja windows`.

`configure.py` generates the Windows build only on Windows.

The build uses all the instructions of the processor of the computer that
builds it (`-march=native`). Such a build does not always start on a
different computer. To make a build for other computers, enter
`python configure.py --portable`. Refer to the build options in the main
[README](../../README.md#build-options).

For a new optimization profile (`--pgo=train`), the build compiles the
profile runtime of LLVM for 32-bit x86 (`pgo/halo_profile_runtime.c`).
LLVM for Windows supplies this runtime only for x86-64.

## Start the game

Enter `build\windows\halo.exe`.

The game finds the game data as on Linux. Refer to "Start the game" in
[port/linux/README.md](../linux/README.md#start-the-game).

| Item | Location |
| --- | --- |
| Settings | `config.toml` next to `halo.exe` |
| Saved games | `%APPDATA%\halo`, or `paths.saves` in `config.toml` |
| Log | `debug.txt` in the data root (the folder that contains `maps\`) |
| Log of the port | The console. The release build has no console: `halo.log` next to `halo.exe` |
| Crash reports | `crashes\` next to `halo.exe`, until the game sends them |

### The Direct3D 11 executable (in development)

`ninja windows` also makes `build\windows\halo_d3d11.exe`. This executable
is the same game with a renderer that uses Direct3D 11 instead of OpenGL. The
renderer is in development: refer to [D3D11.md](D3D11.md). At this time the
window shows only the clears of the game: the plain colours that the game
fills its screen with before it draws. The renderer copies the vertices and
the indices of each draw of the game into Direct3D buffers, but it does not
draw them yet. The menus, the game, the sound and the input operate, but the
executable draws nothing else. To play, use `halo.exe`.

The two executables use the same `config.toml`, the same game data and the
same saved games. These settings apply only to `halo_d3d11.exe`:

| Setting | Default | Environment variable | Description |
| --- | --- | --- | --- |
| `display.d3d11_adapter` | `""` | `HALO_D3D11_ADAPTER` | The graphics adapter. Empty for the fastest GPU, `"warp"` for the software renderer of Microsoft, or a part of the name of an adapter. If no adapter has that name, the executable uses the fastest GPU. |
| `debug.d3d11_debug` | `false` | `HALO_D3D11_DEBUG` | Writes the messages of the Direct3D debug layer to the log. The debug layer is in the Graphics Tools feature of Windows (Settings, Optional features). |
| `debug.d3d11_present_marker` | `false` | `HALO_D3D11_PRESENT_MARKER` | Draws a red square at the top left and a green square at the top right of the picture. Thus a screenshot of the window shows the orientation and the edges of the picture. |
| `debug.d3d11_self_test` | `false` | `HALO_D3D11_SELF_TEST` | At start, the renderer tests its clears on small targets of its own, then the copies of the game's memory into its buffers. It writes the two results to the log (`d3d11: clears self-test: ok` and `d3d11: memory self-test: ok`, or `FAILED` and the first difference). |

The log of the port has a `renderer:` line. This line gives the
adapter, the feature level, the version of the driver and the state of the
debug layer, or the reason why Direct3D 11 is not available. The next line
(`d3d11: swap chain ...`) gives the swap chain on the window. The line after
it (`d3d11: buffers: ...`) gives the buffers for the game's memory and the
free address space of the process. The
`debug.screenshot_every` and `debug.screenshot_directory` settings operate
as in `halo.exe`.

## How the port operates

The game is 32-bit code, as on Linux, because its data contains 32-bit
pointers. The clang target `i686-pc-windows-msvc` gives the ABI of MSVC:
the structure layout, the 16-bit `wchar_t` and the calling conventions.
Thus the Windows build needs fewer changes than the Linux build.

The executable is large-address-aware, because the platform layer reserves
the Xbox memory at `0x80000000`.

### Headers

- The game and the platform layer use the Xbox SDK declarations of
  `port/include/xdk`, not the Windows SDK declarations with the same names.
  The compiler reads `port/include/xdk` before the Windows SDK.
- The C runtime is the static UCRT of Windows.
- `include/halo_windows_prefix.h` is the first header of each file. It gives
  Windows names to the Xbox SDK functions of the platform layer (for example
  `CreateFileA`, `ReadFile`, `Sleep`). Thus the code for Windows gets to
  Windows. The list of names is in `include/halo_windows_api_names.h`.
- `include/crt` changes some C runtime headers. The file functions of the
  game (`fopen`, `open`, `mkdir`) go through the translation of Xbox paths
  (`src/windows_crt.c`).
- `include/posix` declares the POSIX functions that the platform layer uses.

### Code for Windows

These files use only the Windows SDK:

| File | Contents |
| --- | --- |
| `src/win32_files.c`, `src/win32_net.c` | The file and socket functions of `port/linux/src/posix.h`. |
| `src/win32_posix.c` | The POSIX functions on Windows threads, critical sections, condition variables, `VirtualAlloc` and the performance counter. |
| `src/win32_memory_watch.c` | The write tracking of textures, with a vectored exception handler. |
| `src/win32_crash.c` | The crash reports. Refer to "Crash reports". |
| `d3d11/win32_d3d11.c` | The Direct3D 11 backend of `halo_d3d11.exe`: the adapter, the device, the debug layer, the render targets and the clears. |
| `d3d11/win32_d3d11_present.c` | The swap chain of `halo_d3d11.exe` and the presentation of each frame. |
| `d3d11/win32_d3d11_memory.c` | The buffers of `halo_d3d11.exe` for the game's memory: the stream and index rings, the segments of the mirror and their upload ring, and the self-test of the copies. |

`port.json` gives the Linux files that these files replace, and the Windows
libraries of the link.

`halo_d3d11.exe` has the objects of `halo.exe`, except `d3d8_gl.c`,
`xgpu_post.c` and `gl_functions.c`. In their place it has the units of
`d3d11/` (except the probe):

- `d3d11/d3d8_d3d11.c` is the Direct3D 8 device of the game. It is a copy
  of `port/linux/src/d3d8_gl.c` without OpenGL, and uses the Xbox SDK
  declarations.
- `d3d11/win32_d3d11.c`, `d3d11/win32_d3d11_present.c` and
  `d3d11/win32_d3d11_memory.c` use only the Windows SDK.
  `d3d11/win32_d3d11.h` is shared by these three files.
- `d3d11/d3d11_backend.h` is the boundary between the two. It uses neither
  SDK.
- `build/windows/gl_stubs.c` (generated by `tools/windows_gl_stubs.py`)
  replaces the OpenGL functions that the shared files still call. Each
  function counts its calls and does nothing.

The platform layer makes the window of `halo_d3d11.exe` without an OpenGL
context (`platform_video_without_gl` in `port/linux/src/sdl_platform.c`).

### Inline functions

MSVC makes a COMDAT copy of an external `__inline` function where it does
not inline a call. Some files call such functions through a prototype.
clang inlines more calls. Thus a function can have no copy. The build
prevents this:

- `port/linux/game/msvc_comdat.c` gives one weak definition of each header
  inline function.
- A wrapper takes the address of each `__inline` function in a `.c` file.
  Thus the compiler makes an external copy (`inline_export_wrapper` in
  `tools/windows_build.py`).

MSVC gives file scope to a structure tag in a prototype. clang does not.
Thus the build also includes the declarations of the Linux build
(`build/windows/halo_msvc_tags.h`).

## Crash reports

A crash writes the faulting address and the calls that led to it to
`debug.txt` and to the log. The builds of the workflow (the releases)
also send a crash report to the Sentry project of the developers:

1. The game starts a second copy of `halo.exe` (`halo.exe --crash-report`).
   This copy writes a minidump of the game to `crashes\` next to
   `halo.exe`, and the game stops. A minidump contains the stacks and the
   registers of the threads, and the list of the modules. It does not
   contain the other memory of the game.
2. At the first crash, the copy asks the player whether to send crash
   reports. `crash_reports.upload` in `config.toml` keeps the answer:
   `"yes"` sends this report and all the reports after it, and `"no"` sends
   no reports and does not ask again. The answer `"no"` deletes the
   minidump.
3. The copy sends the minidump and a copy of `halo.log` to Sentry, then
   deletes them. If it cannot send them (for example, with no network
   connection), the game sends them when it starts the next time
   (`halo.exe --crash-upload`, in the background). `crashes\` keeps at
   most 8 reports.

Sentry finds the function names and the source lines of the minidump in
the PDBs (`halo.pdb`, `SDL3.pdb`). The `release` job of the workflow
uploads the PDBs and the executables of each build of main to Sentry. This
needs the `SENTRY_AUTH_TOKEN` secret of the repository: an organization
auth token from the settings of the Sentry organization (the token gives
the organization and its region). The `SENTRY_PROJECT` variable of the
repository can give the slug of the project; without it, the job uses the
id of the project in the DSN. For a personal token instead, also set the
`SENTRY_ORG` variable (the slug of the organization) and the `SENTRY_URL`
variable (`https://de.sentry.io`).

Each release also has the PDBs as `halo-windows-<configuration>-symbols.zip`.
`tools/symbolize_crash.py` uses them to add the function names and the
source lines to the crash lines of a `debug.txt` or a `halo.log`:

```
python tools/symbolize_crash.py debug.txt halo.exe
```

Give the `halo.exe` of the build that crashed, with its `halo.pdb` next to
it. The tool needs `llvm-symbolizer` (LLVM).

Builds without a build number (a local build, or a build of a branch other
than main) send no crash reports. To test the crash reports with such a
build, set the `HALO_CRASH_REPORTS_ANY_BUILD` environment variable.

The crash reports also include the crashes of `abort()` and of an invalid
argument to a function of the C runtime.

## Find problems

`build\windows\d3d11_probe.exe` (built by `ninja windows_d3d11_probe`)
reports what Direct3D 11 offers on this computer: the graphics adapters,
the formats, the costs of the shader compiler and of buffers, and the
behaviour of a window that presents. It writes `d3d11_probe.txt` in the
current folder. Refer to [d3d11/README.md](d3d11/README.md).

## Limits

- Bink video is not available. The game skips the movies.
- The game uses only the ANSI file functions. A path to the data or the
  saved games with characters that are not in the code page of the system
  does not always operate.
