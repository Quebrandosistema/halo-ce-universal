# Direct3D 11

This folder is for the Windows build's Direct3D 11 renderer, which is
planned in [port/windows/D3D11.md](../D3D11.md). The renderer is in
development (phase 1: the executable runs the game with a black window).

| File | Contents |
| --- | --- |
| `d3d8_d3d11.c` | The Direct3D 8 device of `halo_d3d11.exe`: a copy of `port/linux/src/d3d8_gl.c` without OpenGL. It keeps the state of the game and draws nothing yet. It uses the Xbox SDK declarations. |
| `win32_d3d11.c` | The Direct3D 11 backend: the adapter, the device, the debug layer and the `renderer:` line of the log. It uses only the Windows SDK. |
| `d3d11_backend.h` | The functions of the backend that the device calls. It uses neither SDK. |
| `win32_d3d11_probe.c` | The probe of phase 0 (below). |

`ninja windows` builds `build\windows\halo_d3d11.exe` from these files
(except the probe) and the other objects of `halo.exe`. Refer to
[port/windows/README.md](../README.md) for its settings
(`display.d3d11_adapter`, `debug.d3d11_debug`).

## The probe

The probe is a console program that reports what Direct3D 11 offers on a
computer and what it costs. To build it, run
`python configure.py --portable`, then `ninja windows_d3d11_probe`. The
`windows` target does not build it. To run it, enter
`build\windows\d3d11_probe.exe [all|caps,compile,memory,queries,present] [--adapter hardware|warp|<part of a name>] [--shaders <folder>] [--report <file>] [--sync 0|1]`.
The probe writes one fact on each line of `d3d11_probe.txt` in the current
folder, and also prints each line to the console. The `compile` step reads
the shaders in `port/windows/probe`. The `present` step opens a window and
prints its instructions for the tester. If a step crashes the probe, the next
run skips that step (`d3d11_probe_skip.txt`). Delete that file to run the
step again.
