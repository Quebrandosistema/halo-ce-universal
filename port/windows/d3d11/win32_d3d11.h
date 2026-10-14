/*
WIN32_D3D11.H

What the Direct3D 11 backend's own units share (port/windows/D3D11.md):
win32_d3d11.c (the device, the targets, the clears, the built-in shaders),
win32_d3d11_present.c (the swap chain and the present), win32_d3d11_memory.c
(the rings, the segments, the upload ring), win32_d3d11_shaders.c (the
compiler, the shader service and its cache, the layouts, the state objects),
win32_d3d11_draw.c (the draw: the record bound and drawn, the constant
buffers, the self-sampling copies) and win32_d3d11_textures.c (the textures
and their views). Windows SDK's types: only the Windows-facing units include
it; the device (d3d8_d3d11.c) sees d3d11_backend.h alone.
*/

#ifndef WIN32_D3D11_H
#define WIN32_D3D11_H

#define COBJMACROS
#include <windows.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#include <stdint.h>

#include "d3d11_backend.h"

/* (the platform layer's log, declared by hand as win32_memory_watch.c does:
platform.h is the Xbox SDK's side) */
void platform_log(const char *format, ...);

struct d3d11_backend_state
{
	IDXGIFactory1 *factory;
	/* the adapter the device is on (for WARP, the one the device reports) */
	IDXGIAdapter1 *adapter;
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	/* with the debug layer, its message queue */
	ID3D11InfoQueue *info;
	D3D_FEATURE_LEVEL level;
	int debug_layer;
	int is_warp;
	/* the adapter's description and its user-mode driver's version */
	char description[256];
	char driver_version[64];
	int exit_registered;
	/* a call answered DXGI_ERROR_DEVICE_REMOVED or _RESET (backend_failed):
	from then on every entry point returns at once */
	int lost;
	char lost_reason[256];
};

extern struct d3d11_backend_state d3d11_backend;
#define B d3d11_backend

/* an HRESULT as hex and, for the common ones, its name */
const char *d3d11_backend_hr_text(HRESULT result);
#define hr_text d3d11_backend_hr_text

/* releases a COM object through a pointer to its pointer, and clears it */
void d3d11_backend_release(void *object);
#define release d3d11_backend_release

/* whether result failed; a removed or reset device is also logged once and
leaves the backend lost (B.lost) */
int d3d11_backend_failed(HRESULT result, const char *call);
#define backend_failed d3d11_backend_failed

/* the built-in shaders: the full-target triangle's vertex shader (from
SV_VertexID, its texture coordinates beside it), the clear's pixel shader
(colour from the constant buffer, SV_Depth) and the present's (the texture in
slot 0 through the sampler in slot 0); compiled at the first use. 0 if the
compiler or a compile failed (logged once). */
int d3d11_backend_shaders(void);

/* a rectangle of the bound targets drawn as a clear: the colour channels in
mask (D3D11_BACKEND_CLEAR_R to _A), depth and stencil when asked (and a depth
target is bound), x, y, width, height in pixels from the top left. Sets every
state it uses; the scissor is the rectangle. 0 without the shaders. */
int d3d11_backend_draw_clear(uint32_t channels, const float color[4], float z, uint32_t stencil,
	const D3D11_RECT *rectangle);

/* the bound targets, as d3d11_backend_targets_set left them, are forgotten
(the present binds the swap chain's view in their place) */
void d3d11_backend_targets_forget(void);

/* the targets bound (NULL for none), and a target by its handle (NULL for
none) */
struct target *d3d11_backend_bound_color(void);
struct target *d3d11_backend_bound_depth(void);
struct target *d3d11_backend_target_at(uint32_t handle);

/* the present's objects released (d3d11_backend_destroy, before the rest) */
void d3d11_backend_present_release(void);

/* the built-in shaders and the sampler the present's draw uses; the copy's
pixel shader (a texel of the texture in slot 0 at each pixel, all four
channels: a target's pixels into its multisampled texture) */
extern ID3D11VertexShader *d3d11_backend_triangle_vs;
extern ID3D11PixelShader *d3d11_backend_present_ps;
extern ID3D11PixelShader *d3d11_backend_copy_ps;
extern ID3D11SamplerState *d3d11_backend_linear_sampler;
extern ID3D11BlendState *d3d11_backend_write_all;
extern ID3D11DepthStencilState *d3d11_backend_depth_off;
extern ID3D11RasterizerState *d3d11_backend_scissored;

/* a target's shader resource view (0 or a depth target: NULL) */
ID3D11ShaderResourceView *d3d11_backend_target_view(uint32_t handle, uint32_t *width, uint32_t *height);

/* HLSL compiled through d3dcompiler_47.dll (loaded at the first call) at
D3DCOMPILE_OPTIMIZATION_LEVEL3; name is the source's in messages. 1 with the
bytecode in code; 0 if the compiler is missing or the source did not compile
(logged with the compiler's messages). The shader service's unit's
(win32_d3d11_shaders.c); the built-in shaders compile through it, outside the
service's cache. */
int d3d11_backend_compile(const char *source, size_t size, const char *name, const char *entry, const char *target,
	ID3DBlob **code);

/* a target of the backend's: the game's (d3d11_backend_target_create) and
the self-tests' own */
struct target
{
	ID3D11Texture2D *texture;
	ID3D11RenderTargetView *color;
	ID3D11DepthStencilView *depth;
	ID3D11ShaderResourceView *view;
	uint32_t width, height;
	int is_depth;
	/* with multisampling (d3d11_backend_target_multisample): a texture of
	the same size and format with samples a pixel (0: none) and its view,
	which draws and clears go to while it exists; the texture above has its
	pixels after a resolve */
	ID3D11Texture2D *multisampled;
	ID3D11RenderTargetView *multisampled_color;
	ID3D11DepthStencilView *multisampled_depth;
	uint32_t samples;
	/* a count the device refused (logged once): not asked for again */
	uint32_t samples_refused;
};

/* a colour (B8G8R8A8_UNORM) or depth target made and cleared once; 0 if it
was not made (logged). (These three are defined in win32_d3d11.c under their
d3d11_backend_ names; the short names are macros for the backend's units.) */
int d3d11_backend_target_make(struct target *target, uint32_t width, uint32_t height, int depth);
#define target_make d3d11_backend_target_make
void d3d11_backend_target_free(struct target *target);
#define target_free d3d11_backend_target_free
/* a colour target's pixels, BGRA rows from the top, through a staging
texture (the Map waits for the GPU); 0 if not read (logged) */
int d3d11_backend_read_target(struct target *target, void *pixels, size_t size);
#define read_target d3d11_backend_read_target

/* the memory unit's objects released (d3d11_backend_destroy, after the
present's and before the targets') */
void d3d11_backend_memory_release(void);

/* the shader service's shaders, layouts and state objects released
(d3d11_backend_destroy, after the memory unit's) */
void d3d11_backend_shaders_release(void);

/* the service's objects by handle, for the draw (NULL for 0 or a handle
that names nothing) */
IUnknown *d3d11_backend_shader_object(uint32_t stage, uint32_t handle);
ID3D11InputLayout *d3d11_backend_layout_object(uint32_t handle);
ID3D11BlendState *d3d11_backend_blend_object(uint32_t handle);
ID3D11DepthStencilState *d3d11_backend_depth_stencil_object(uint32_t handle);
ID3D11RasterizerState *d3d11_backend_rasterizer_object(uint32_t handle);
ID3D11SamplerState *d3d11_backend_sampler_object(uint32_t handle);

/* the memory unit's buffers by handle, for the draw (NULL for none) */
ID3D11Buffer *d3d11_backend_ring_buffer(uint32_t ring);
ID3D11Buffer *d3d11_backend_segment_buffer(uint32_t segment);

/* the draw unit (win32_d3d11_draw.c): the viewport's shadow forgotten when
the targets are bound (which sets the whole target's viewport); a target
about to be bound unbound from the stages that sample it; its objects
released (d3d11_backend_destroy, after the shader service's) */
void d3d11_backend_draw_viewport_forget(void);
void d3d11_backend_draw_target_unbind(ID3D11ShaderResourceView *view);
void d3d11_backend_draw_release(void);

/* the textures unit (win32_d3d11_textures.c): a texture's or a composite's
view by handle (NULL for none); its counts into the draw statistics
(textures alive and their bytes, writes and their bytes, composite copies);
its objects released */
ID3D11ShaderResourceView *d3d11_backend_texture_view(uint32_t handle);
ID3D11ShaderResourceView *d3d11_backend_composite_view(uint32_t handle);
void d3d11_backend_textures_statistics(uint32_t counts[D3D11_BACKEND_DRAW_STATISTICS]);
void d3d11_backend_textures_release(void);

/* the targets' resolves since the start (win32_d3d11.c), for the draw
statistics */
uint32_t d3d11_backend_target_resolves(void);

/* the view a target is drawn into: its multisampled texture's while it has
one (NULL for none) */
ID3D11RenderTargetView *d3d11_backend_target_color_view(struct target *target);
ID3D11DepthStencilView *d3d11_backend_target_depth_view(struct target *target);

/* the bound targets bound again by the next draw or clear (the passes bind
their own) */
void d3d11_backend_targets_rebind(void);

/* a sampler's register in a compiled shader, by its name (D3DReflect, the
compiler's other export); 0 if it has none (logged) */
int d3d11_backend_sampler_register(ID3DBlob *code, const char *name, UINT *slot);

/* the anti-aliasing passes' objects released (win32_d3d11_post.c) */
void d3d11_backend_post_release(void);

#endif
