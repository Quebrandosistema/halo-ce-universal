/*
D3D11_BACKEND.H

The boundary between the Direct3D 11 renderer's two halves
(port/windows/D3D11.md, "Where the renderer lives"): the Direct3D 8 device
the game drives (d3d8_d3d11.c, compiled against the Xbox SDK's headers) and
the Direct3D 11 backend (win32_d3d11.c, against the Windows SDK's). The two
SDKs define the same names differently, so no unit sees both: this header
includes neither, and only fixed-width integers, int, strings and plain
pointers cross it, as in update.h and posix.h.

Phase 1: the device, the adapter, the debug layer, the log line.
*/

#ifndef D3D11_BACKEND_H
#define D3D11_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* makes the Direct3D 11 device: on display.d3d11_adapter's adapter
(adapter: empty for the first hardware adapter by GPU preference, "warp" for
Microsoft's software renderer, else the first adapter whose name contains it,
case aside, or the first hardware adapter if none does), at feature level
11_0 or above, with the debug layer when debug_layer is set and the layer is
installed. 1 with a device, 0 without; either way line holds the one log
line ("renderer: Direct3D 11 ..."), which the caller logs. */
int d3d11_backend_create(const char *adapter, int debug_layer, char *line, size_t line_size);

/* the debug layer's messages stored since the last call, each logged as
"d3d11 debug: <severity> <id> <text>" and cleared; how many (0 at once
without the layer) */
uint32_t d3d11_backend_drain_debug(void);

/* with the layer, its live-object summary to the log; then everything
released. Also run at exit (atexit), once. */
void d3d11_backend_destroy(void);

#endif
