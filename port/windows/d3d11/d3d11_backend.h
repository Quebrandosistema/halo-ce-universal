/*
D3D11_BACKEND.H

The boundary between the Direct3D 11 renderer's two halves
(port/windows/D3D11.md, "Where the renderer lives"): the Direct3D 8 device
the game drives (d3d8_d3d11.c, compiled against the Xbox SDK's headers) and
the Direct3D 11 backend (win32_d3d11.c, win32_d3d11_present.c and
win32_d3d11_memory.c, against the Windows SDK's). The two SDKs define the same names differently, so no unit
sees both: this header includes neither, and only fixed-width integers, int,
strings and plain pointers cross it, as in update.h and posix.h.

Phase 1: the device, the adapter, the debug layer, the log line. Phase 2: the
swap chain and the present, the render targets, the clears, the read-back,
the clears' self-test, the lost device. Phase 3: the stream and index rings,
the mirror's segments and their upload ring, the memory self-test. Phase 4:
a shader's compile, for the generator self-test.
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

/* ---------- the game's memory (win32_d3d11_memory.c)

Everything the device hands these is copied during the call: nothing of the
game's memory is read later. */

/* the stream ring (stream_size bytes, vertices), the index ring (index_size
bytes, 16-bit indices), both D3D11_USAGE_DYNAMIC, and the mirror's upload
ring. 1 with all three; 0 without, after which every call below answers 0.
line holds the log line either way ("d3d11: buffers: ..." with the address
space left, or "d3d11: no buffers (...)"), which the caller logs. Called
once. */
int d3d11_backend_memory_initialize(uint32_t stream_size, uint32_t index_size, char *line, size_t line_size);

/* the two rings */
#define D3D11_BACKEND_RING_STREAM 0u
#define D3D11_BACKEND_RING_INDEX 1u

/* the next write to the ring maps it with D3D11_MAP_WRITE_DISCARD: the
ring's earlier bytes stay what earlier draws read, and the caller writes from
0 again */
void d3d11_backend_ring_orphan(uint32_t ring);

/* size bytes of data into the ring at offset: a no-overwrite map, the
caller's promise that no bytes written since the last orphan are written
again (the first write after an orphan, or the ring's first, discards). 1 on
success; 0 for a range past the ring's end (logged once), a map that failed
(logged once; a lost device through d3d11_backend_lost) or no buffers. */
int d3d11_backend_ring_write(uint32_t ring, uint32_t offset, const void *data, uint32_t size);

/* the D3D11_MAP_WRITE_DISCARD maps of the two rings since the start */
uint32_t d3d11_backend_ring_discards(void);

/* a segment of the mirror: a DEFAULT buffer of size bytes for vertices and
indices, its contents undefined until written. Its handle, from 1; 0 if it
could not be made (logged). Never freed. */
uint32_t d3d11_backend_segment_create(uint32_t size);

/* how many segments exist */
uint32_t d3d11_backend_segments_alive(void);

/* size bytes of data into the segment at offset, through the upload ring and
a copy queued in the context, in order with the draws: a draw after the call
reads these bytes, one before it what the segment held. 1 on success; 0 for
a range outside the segment or larger than the upload ring (logged once), a
failure (logged once; a lost device through d3d11_backend_lost) or no
buffers. */
int d3d11_backend_segment_write(uint32_t segment, uint32_t offset, const void *data, uint32_t size);

/* debug.d3d11_self_test's second test: bytes in ordinary memory written
through the stream ring (appended, and across an orphan), a segment (the same
bytes rewritten in place) and the index ring, each drawn into a target of
the test's own after each write and read back. 1 if every draw had the colour
its bytes had at the draw; line says so, or which case and draw differed. */
int d3d11_backend_memory_self_test(char *line, size_t line_size);

/* ---------- shaders (win32_d3d11.c) */

/* HLSL compiled (d3dcompiler_47.dll, D3DCOMPILE_OPTIMIZATION_LEVEL3) and the
bytecode let go: 1 if it compiles; 0 if not, or without the compiler, the
compiler's messages logged with name. Phase 4's generator self-test; the
backend keeps no shader of the game's yet (phase 5). */
int d3d11_backend_shader_compiles(const char *source, size_t size, const char *name, const char *entry,
	const char *target);

#endif
