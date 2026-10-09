# Direct3D 11

This folder is for the Windows build's Direct3D 11 renderer, which is
planned in [port/windows/D3D11.md](../D3D11.md). For now it contains only the
probe of phase 0, `win32_d3d11_probe.c`. The probe is a console program that
reports what Direct3D 11 offers on a computer and what it costs. To build it,
run `python configure.py --portable`, then `ninja windows_d3d11_probe`. The
`windows` target does not build it. To run it, enter
`build\windows\d3d11_probe.exe [all|caps,compile,memory,queries,present] [--adapter hardware|warp|<part of a name>] [--shaders <folder>] [--report <file>] [--sync 0|1]`.
The probe writes one fact on each line of `d3d11_probe.txt` in the current
folder, and also prints each line to the console. The `compile` step reads
the shaders in `port/windows/probe`. The `present` step opens a window and
prints its instructions for the tester. If a step crashes the probe, the next
run skips that step (`d3d11_probe_skip.txt`). Delete that file to run the
step again.
