/*
D3D11_BACKEND.H

The boundary between the Direct3D 11 renderer's two halves
(port/windows/D3D11.md, "Where the renderer lives"): the Direct3D 8 device
the game drives (d3d8_d3d11.c, compiled against the Xbox SDK's headers) and
the Direct3D 11 backend (win32_d3d11.c and win32_d3d11_present.c, against the
Windows SDK's). The two SDKs define the same names differently, so no unit
sees both: this header includes neither, and only fixed-width integers, int,
strings and plain pointers cross it, as in update.h and posix.h.

Phase 1: the device, the adapter, the debug layer, the log line. Phase 2: the
swap chain and the present, the render targets, the clears, the read-back,
the clears' self-test, the lost device.
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

/* ---------- the swap chain and the present (win32_d3d11_present.c) */

/* the swap chain on the window (hwnd: the platform layer's HWND); 1 with
one. line holds its log line either way ("d3d11: swap chain ..." or "d3d11:
no swap chain (...)"), which the caller logs. */
int d3d11_backend_window_set(void *hwnd, char *line, size_t line_size);

/* the back buffer's target drawn letterboxed into the swap chain's buffer
and presented, at sync_interval (1: the display's refresh; 0: at once, with
tearing where it is allowed). The window's pixel size, polled each frame, is
the swap chain's resize. 1 if a picture was presented; 0 if nothing was (the
window is minimised or occluded, there is no swap chain, or the present
failed, logged); -1 if the device is lost. */
int d3d11_backend_present(uint32_t back_buffer, uint32_t window_width, uint32_t window_height, int sync_interval);

/* debug.d3d11_present_marker: a red square at the top left and a green one
at the top right of the presented picture */
void d3d11_backend_present_marker(int on);

/* the swap chain's resizes since the start */
uint32_t d3d11_backend_resizes(void);

/* ---------- render targets, clears (win32_d3d11.c) */

/* a render target of that size: colour (B8G8R8A8_UNORM) or depth
(D24_UNORM_S8_UINT), cleared once (black with alpha 1, or depth 1 and stencil
0). Its handle, from 1; 0 if it could not be made (logged). Never freed. */
uint32_t d3d11_backend_target_create(uint32_t pixel_width, uint32_t pixel_height, int depth);

/* how many targets exist */
uint32_t d3d11_backend_targets_alive(void);

/* the targets later clears (and, from phase 6, draws) go to; either 0 */
void d3d11_backend_targets_set(uint32_t color, uint32_t depth);

/* what d3d11_backend_clear's channels name */
#define D3D11_BACKEND_CLEAR_R 0x01u
#define D3D11_BACKEND_CLEAR_G 0x02u
#define D3D11_BACKEND_CLEAR_B 0x04u
#define D3D11_BACKEND_CLEAR_A 0x08u
#define D3D11_BACKEND_CLEAR_COLOR 0x0fu
#define D3D11_BACKEND_CLEAR_DEPTH 0x10u
#define D3D11_BACKEND_CLEAR_STENCIL 0x20u

/* clears the bound targets: the channels named, color (red, green, blue,
alpha, 0 to 1), z and stencil, in count rectangles of x, y, width, height in
the targets' pixels with row 0 at the top. All four colour channels in one
rectangle that is the whole colour target is a clear call, as is depth or
stencil over the whole depth target; anything else is drawn. 1 if all of it
went through clear calls, 2 if any of it was drawn, 0 if nothing was cleared
(nothing bound, or the drawn path without its shaders). */
uint32_t d3d11_backend_clear(uint32_t channels, const float color[4], float z, uint32_t stencil,
	const int32_t *rectangles, uint32_t count);

/* a colour target's pixels, BGRA rows from the top, pixel_width * 4 bytes
each, into pixels (size bytes); 1 on success, 0 (logged) otherwise */
int d3d11_backend_target_read(uint32_t handle, void *pixels, size_t size);

/* debug.d3d11_self_test: the clears tried on targets of the backend's own and
read back. 1 if right; line says what was tried or what differed. */
int d3d11_backend_self_test(char *line, size_t line_size);

/* 1 once the device was removed or reset (the backend logged why, in
d3d11_backend_lost_reason's words); from then on the backend does nothing */
int d3d11_backend_lost(void);
const char *d3d11_backend_lost_reason(void);

#endif
