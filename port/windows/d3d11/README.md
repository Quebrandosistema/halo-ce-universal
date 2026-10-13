# Direct3D 11

This folder is for the Windows build's Direct3D 11 renderer, which is
planned in [port/windows/D3D11.md](../D3D11.md). The renderer is in
development (phase 5: the window shows only the clears of the game; the
vertices and indices of each draw are copied into Direct3D buffers, and the
shaders, the input layout and the state objects of each draw are made, but
nothing is drawn).

| File | Contents |
| --- | --- |
| `d3d8_d3d11.c` | The Direct3D 8 device of `halo_d3d11.exe`: a copy of `port/linux/src/d3d8_gl.c` without OpenGL. It keeps the state of the game, finds the render targets of the game, clears them and presents the back buffer. It keeps the mirror of the game's memory and copies the data of each draw into the buffers of the backend. At each draw it makes the pixel shader key and chooses the vertex shader, writes the shaders that `debug.gpu_dump_shaders` asks for, and asks the backend for the shaders (compiled the first time), the input layout and the blend, depth-stencil, rasterizer and sampler states of the draw, which it translates from the Xbox values. It does not draw yet. It uses the Xbox SDK declarations. |
| `nv2a_vsh_hlsl.c`, `nv2a_psh_hlsl.c` | The shader generators: copies of `port/linux/src/nv2a_vsh.c` and `nv2a_psh.c` that write HLSL. Only the grammar of the shaders is different. `tools/d3d11_shader_check.py --copies` lists the changed lines by the item of the dialect (port/windows/D3D11.md, phase 4). They use the Xbox SDK declarations. |
| `hlsl_shaders.h` | The constant buffers (registers, sizes and the place of each member, checked against the C structures when the files compile), the interface between the two stages, the version of the generators and their functions. It uses neither SDK. |
| `win32_d3d11.c` | The Direct3D 11 backend: the adapter, the device, the debug layer, the `renderer:` line of the log, the render targets, the clears, the built-in shaders, the read-back for screenshots, the self-test of the clears and the lost device. It uses only the Windows SDK. |
| `win32_d3d11_present.c` | The swap chain on the window, its resize, the presentation of the back buffer (letterboxed) and the marker (`debug.d3d11_present_marker`). It uses only the Windows SDK. |
| `win32_d3d11_memory.c` | The buffers for the game's memory: the stream ring and the index ring (`DYNAMIC`), the segments of the mirror (`DEFAULT`), the upload ring that writes the segments, and the self-test of these copies. It uses only the Windows SDK. |
| `win32_d3d11_shaders.c` | The shader service: the shader compiler (`d3dcompiler_47.dll`, also for the built-in shaders and the self-test of the generators), the shaders by handle, their cache on the disk (`%LOCALAPPDATA%\halo\d3d11_shaders`), the input layouts, the state objects and the self-test of these. It uses only the Windows SDK. |
| `win32_d3d11.h` | The state and the functions that the four backend files share. It uses only the Windows SDK. |
| `d3d11_backend.h` | The functions of the backend that the device calls. It uses neither SDK. |
| `win32_d3d11_probe.c` | The probe of phase 0 (below). |

`ninja windows` builds `build\windows\halo_d3d11.exe` from these files
(except the probe) and the other objects of `halo.exe`. Refer to
[port/windows/README.md](../README.md) for its settings
(`display.d3d11_adapter`, `debug.d3d11_debug`, `debug.d3d11_present_marker`,
`debug.d3d11_self_test`) and for the shader dump (`debug.gpu_dump_shaders`)
and its check (`tools/d3d11_shader_check.py`).

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
