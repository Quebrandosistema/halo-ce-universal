/*
WIN32_D3D11.H

What the Direct3D 11 backend's own units share (port/windows/D3D11.md):
win32_d3d11.c (the device, the targets, the clears, the built-in shaders) and
win32_d3d11_present.c (the swap chain and the present). Windows SDK's types:
only the Windows-facing units include it; the device (d3d8_d3d11.c) sees
d3d11_backend.h alone.
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

/* the present's objects released (d3d11_backend_destroy, before the rest) */
void d3d11_backend_present_release(void);

/* the built-in shaders and the sampler the present's draw uses */
extern ID3D11VertexShader *d3d11_backend_triangle_vs;
extern ID3D11PixelShader *d3d11_backend_present_ps;
extern ID3D11SamplerState *d3d11_backend_linear_sampler;
extern ID3D11BlendState *d3d11_backend_write_all;
extern ID3D11DepthStencilState *d3d11_backend_depth_off;
extern ID3D11RasterizerState *d3d11_backend_scissored;

/* a target's shader resource view (0 or a depth target: NULL) */
ID3D11ShaderResourceView *d3d11_backend_target_view(uint32_t handle, uint32_t *width, uint32_t *height);

#endif
