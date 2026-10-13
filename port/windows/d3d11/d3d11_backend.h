/*
D3D11_BACKEND.H

The boundary between the Direct3D 11 renderer's two halves
(port/windows/D3D11.md, "Where the renderer lives"): the Direct3D 8 device
the game drives (d3d8_d3d11.c, compiled against the Xbox SDK's headers) and
the Direct3D 11 backend (win32_d3d11.c, win32_d3d11_present.c and
win32_d3d11_memory.c and win32_d3d11_shaders.c, against the Windows SDK's). The two SDKs define the same names
differently, so no unit
sees both: this header includes neither, and only fixed-width integers, int,
strings and plain pointers cross it, as in update.h and posix.h.

Phase 1: the device, the adapter, the debug layer, the log line. Phase 2: the
swap chain and the present, the render targets, the clears, the read-back,
the clears' self-test, the lost device. Phase 3: the stream and index rings,
the mirror's segments and their upload ring, the memory self-test. Phase 4:
a shader's compile, for the generator self-test. Phase 5: the shader service
with its cache on the disk, the input layouts and the state objects.
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

/* ---------- shaders and state objects (win32_d3d11_shaders.c)

The shader service: a shader is known by its stage and a 64-bit hash the
device makes (FNV-1a over the generator's version and the program's words and
masks, or the pixel shader key: hlsl_shaders.h); its bytecode is kept on the
disk (%LOCALAPPDATA%\halo\d3d11_shaders\<generator version>-<compiler
version>\), so that each is compiled once a machine. Handles count from 1; 0
is none. Nothing here is bound: the draws (phase 6) bind what the handles
name. */

/* HLSL compiled (d3dcompiler_47.dll, D3DCOMPILE_OPTIMIZATION_LEVEL3) and the
bytecode let go: 1 if it compiles; 0 if not, or without the compiler, the
compiler's messages logged with name. Phase 4's generator self-test. */
int d3d11_backend_shader_compiles(const char *source, size_t size, const char *name, const char *entry,
	const char *target);

#define D3D11_BACKEND_STAGE_VERTEX 0u
#define D3D11_BACKEND_STAGE_PIXEL 1u

/* the cache on the disk used (1) or not (0: a debug.gpu_debug_* setting
changes the generators' text, so nothing is read or written); called once,
before the first shader. With 1, the cache's folder is made and listed (no
file opened), and the other versions' folders are removed; the log says
what was found. Without the call, there is no cache. */
void d3d11_backend_shader_cache_enable(int enabled);

/* the shader's handle: made this run already, or now from its file in the
cache (checked: a damaged file is deleted, logged, and 0 answered); 0 if it
has neither, or failed this run */
uint32_t d3d11_backend_shader_find(uint32_t stage, uint64_t hash);

/* the shader compiled now from source (copied during the call) at entry and
target, its file written to the cache, its object made; its handle, or 0 if
it did not compile (the compiler's text logged once for the hash, and the
source written to the cache's failed\ folder) or its object was not made */
uint32_t d3d11_backend_shader_make(uint32_t stage, uint64_t hash, const char *source, size_t size, const char *entry,
	const char *target);

/* the vertex formats the device's layouts use: DXGI_FORMAT's numbers, which
the backend checks against dxgiformat.h when it compiles */
#define D3D11_BACKEND_FORMAT_R32G32B32A32_FLOAT 2u
#define D3D11_BACKEND_FORMAT_R32G32B32_FLOAT 6u
#define D3D11_BACKEND_FORMAT_R16G16B16A16_SNORM 13u
#define D3D11_BACKEND_FORMAT_R16G16B16A16_SINT 14u
#define D3D11_BACKEND_FORMAT_R32G32_FLOAT 16u
#define D3D11_BACKEND_FORMAT_R8G8B8A8_UNORM 28u
#define D3D11_BACKEND_FORMAT_R16G16_SNORM 37u
#define D3D11_BACKEND_FORMAT_R16G16_SINT 38u
#define D3D11_BACKEND_FORMAT_R32_FLOAT 41u
#define D3D11_BACKEND_FORMAT_R32_UINT 42u
#define D3D11_BACKEND_FORMAT_R8G8_UNORM 49u
#define D3D11_BACKEND_FORMAT_R16_SNORM 58u
#define D3D11_BACKEND_FORMAT_R16_SINT 59u
#define D3D11_BACKEND_FORMAT_R8_UNORM 61u
#define D3D11_BACKEND_FORMAT_B8G8R8A8_UNORM 87u

/* an attribute of a vertex layout: the vertex shader's input TEXCOORD<n>
(n: the NV2A's input register) read in format from the vertex buffer in slot
(the declaration's stream) at offset bytes into each vertex */
struct d3d11_backend_layout_element
{
	uint32_t semantic_index;
	uint32_t format;
	uint32_t slot;
	uint32_t offset;
};

/* the slot an input the elements do not feed is read from: the backend adds
an element for each input of the vertex shader that count elements leave
out, R32G32B32A32_FLOAT at semantic_index * 16 bytes, so that the layout can
be made (a vertex buffer there with stride 0 gives every vertex the input's
current value: phase 6) */
#define D3D11_BACKEND_ATTRIBUTE_SLOT 16u

/* the input layout of these elements for the vertex shader's handle (whose
input signature it is checked against): made once, then found by its hash;
0 if it could not be made (logged once with the shader's hash) */
uint32_t d3d11_backend_layout_make(const struct d3d11_backend_layout_element *elements, uint32_t count,
	uint32_t vertex_shader);

/* Direct3D 11's enumerants as their numbers (D3D11_BLEND, D3D11_BLEND_OP,
D3D11_COMPARISON_FUNC, D3D11_STENCIL_OP, D3D11_FILL_MODE, D3D11_CULL_MODE,
D3D11_FILTER, D3D11_TEXTURE_ADDRESS_MODE), checked by the backend */
#define D3D11_BACKEND_BLEND_ZERO 1u
#define D3D11_BACKEND_BLEND_ONE 2u
#define D3D11_BACKEND_BLEND_SRC_COLOR 3u
#define D3D11_BACKEND_BLEND_INV_SRC_COLOR 4u
#define D3D11_BACKEND_BLEND_SRC_ALPHA 5u
#define D3D11_BACKEND_BLEND_INV_SRC_ALPHA 6u
#define D3D11_BACKEND_BLEND_DEST_ALPHA 7u
#define D3D11_BACKEND_BLEND_INV_DEST_ALPHA 8u
#define D3D11_BACKEND_BLEND_DEST_COLOR 9u
#define D3D11_BACKEND_BLEND_INV_DEST_COLOR 10u
#define D3D11_BACKEND_BLEND_SRC_ALPHA_SAT 11u
#define D3D11_BACKEND_BLEND_BLEND_FACTOR 14u
#define D3D11_BACKEND_BLEND_INV_BLEND_FACTOR 15u
#define D3D11_BACKEND_BLEND_OP_ADD 1u
#define D3D11_BACKEND_BLEND_OP_SUBTRACT 2u
#define D3D11_BACKEND_BLEND_OP_REV_SUBTRACT 3u
#define D3D11_BACKEND_BLEND_OP_MIN 4u
#define D3D11_BACKEND_BLEND_OP_MAX 5u
#define D3D11_BACKEND_COMPARISON_NEVER 1u
#define D3D11_BACKEND_COMPARISON_ALWAYS 8u
#define D3D11_BACKEND_STENCIL_OP_KEEP 1u
#define D3D11_BACKEND_STENCIL_OP_ZERO 2u
#define D3D11_BACKEND_STENCIL_OP_REPLACE 3u
#define D3D11_BACKEND_STENCIL_OP_INCR_SAT 4u
#define D3D11_BACKEND_STENCIL_OP_DECR_SAT 5u
#define D3D11_BACKEND_STENCIL_OP_INVERT 6u
#define D3D11_BACKEND_STENCIL_OP_INCR 7u
#define D3D11_BACKEND_STENCIL_OP_DECR 8u
#define D3D11_BACKEND_FILL_WIREFRAME 2u
#define D3D11_BACKEND_FILL_SOLID 3u
#define D3D11_BACKEND_CULL_NONE 1u
#define D3D11_BACKEND_CULL_FRONT 2u
#define D3D11_BACKEND_CULL_BACK 3u
/* D3D11_ENCODE_BASIC_FILTER(min, mag, mip, 0) with 1 for linear, 0 for point */
#define D3D11_BACKEND_FILTER(min_linear, mag_linear, mip_linear) \
	((uint32_t)((min_linear) ? 0x10 : 0) | ((mag_linear) ? 0x04 : 0) | ((mip_linear) ? 0x01 : 0))
#define D3D11_BACKEND_FILTER_ANISOTROPIC 0x55u
#define D3D11_BACKEND_ADDRESS_WRAP 1u
#define D3D11_BACKEND_ADDRESS_MIRROR 2u
#define D3D11_BACKEND_ADDRESS_CLAMP 3u
#define D3D11_BACKEND_ADDRESS_BORDER 4u

/* the four kinds of state object, each a Direct3D 11 description in
fixed-width fields (zeroed before they are filled, so that equal states hash
equal); one render target, both stencil faces the same. The dynamic values
(the blend factor, the stencil reference) are set at the bind (phase 6). */
struct d3d11_backend_blend
{
	uint32_t enable;
	uint32_t source, destination, operation;
	uint32_t source_alpha, destination_alpha, operation_alpha;
	/* D3D11_BACKEND_CLEAR_R to _A */
	uint32_t write_mask;
};

struct d3d11_backend_depth_stencil
{
	uint32_t depth_enable, depth_write, depth_function;
	uint32_t stencil_enable, stencil_read_mask, stencil_write_mask;
	uint32_t stencil_fail, stencil_depth_fail, stencil_pass, stencil_function;
};

struct d3d11_backend_rasterizer
{
	uint32_t fill, cull, front_counter_clockwise;
	int32_t depth_bias;
	float depth_bias_clamp, slope_scaled_depth_bias;
	uint32_t depth_clip, scissor, multisample, antialiased_line;
};

struct d3d11_backend_sampler
{
	uint32_t filter;
	uint32_t address_u, address_v, address_w;
	float mip_lod_bias;
	uint32_t max_anisotropy;
	uint32_t comparison;
	float border_color[4];
	float min_lod, max_lod;
};

/* the state object of the description: made once, then found by its hash
(FNV-1a, 64 bits, over the structure); 0 if it could not be made (logged
once) */
uint32_t d3d11_backend_blend_make(const struct d3d11_backend_blend *description);
uint32_t d3d11_backend_depth_stencil_make(const struct d3d11_backend_depth_stencil *description);
uint32_t d3d11_backend_rasterizer_make(const struct d3d11_backend_rasterizer *description);
uint32_t d3d11_backend_sampler_make(const struct d3d11_backend_sampler *description);

/* what d3d11_backend_shaders_statistics fills, totals since the start: the
shaders found in the cache's files and the microseconds that took, the
shaders compiled and the microseconds D3DCompile took, the compiles that
failed, the layouts made and those with an input left to the attribute slot
that the shader reads, the state objects of each kind */
#define D3D11_BACKEND_STATISTIC_CACHED 0
#define D3D11_BACKEND_STATISTIC_CACHED_MICROSECONDS 1
#define D3D11_BACKEND_STATISTIC_COMPILED 2
#define D3D11_BACKEND_STATISTIC_COMPILE_MICROSECONDS 3
#define D3D11_BACKEND_STATISTIC_FAILED 4
#define D3D11_BACKEND_STATISTIC_LAYOUTS 5
#define D3D11_BACKEND_STATISTIC_UNFED_LAYOUTS 6
#define D3D11_BACKEND_STATISTIC_BLEND 7
#define D3D11_BACKEND_STATISTIC_DEPTH_STENCIL 8
#define D3D11_BACKEND_STATISTIC_RASTERIZER 9
#define D3D11_BACKEND_STATISTIC_SAMPLERS 10
#define D3D11_BACKEND_STATISTICS 11
void d3d11_backend_shaders_statistics(uint32_t counts[D3D11_BACKEND_STATISTICS]);

/* debug.d3d11_self_test's fourth test, the service's own cases: a good pair
of shaders made (compiled, their files written), found again from memory,
let go and found again from the files; a bad shader failed, logged once and
written to failed\; a file damaged by the test deleted and its shader made
again; a layout over the test's vertex shader with an input left out; one
state object of each kind found again by its description. 1 if each did as
it should; line says so, or which case did not. */
int d3d11_backend_shaders_self_test(char *line, size_t line_size);

#endif
