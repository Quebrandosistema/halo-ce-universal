/*
WIN32_D3D11_PRESENT.C

The Direct3D 11 backend's swap chain and present (port/windows/D3D11.md,
phase 2): the swap chain on the game's window, its resize when the window's
pixel size changes, the back buffer's target drawn letterboxed into it, the
marker (debug.d3d11_present_marker) and the present with display.vsync's
sync interval. The first unit the backend split into, as the plan's "Files"
allows; what it shares with win32_d3d11.c is in win32_d3d11.h.

The swap chain's making (the device's own factory, the tearing check, the
description, CreateSwapChainForHwnd, MakeWindowAssociation), make_view and
resize are copied from phase 0's probe (win32_d3d11_probe.c, step_present,
make_view, resize), where they were tried on this machine's Adreno and on
WARP through every window transition, and changed only for what the backend
keeps and reports: the size the device polls is the one asked for, and the
results go to the log. Fullscreen is SDL's alone: SetFullscreenState is never
called, and Alt+Enter is left to nothing (DXGI_MWA_NO_ALT_ENTER).
*/

#include <stdio.h>
#include <string.h>

#include "win32_d3d11.h"

/* the marker's squares, in the swap chain's pixels */
#define MARKER_SIZE 16

static struct
{
	HWND hwnd;
	IDXGIFactory2 *factory;
	IDXGISwapChain1 *swap_chain;
	ID3D11RenderTargetView *view;
	UINT swap_flags;
	int tearing;
	/* the swap chain's buffers' size; a size ResizeBuffers failed at, not
	asked for again until the window's size changes */
	uint32_t width, height;
	uint32_t failed_width, failed_height;
	uint32_t resizes;
	int marker;
	/* Present's results other than S_OK seen, each logged once */
	HRESULT seen[16];
	int seen_count;
} P;

static int make_view(void)
{
	ID3D11Texture2D *back = NULL;
	HRESULT result = IDXGISwapChain1_GetBuffer(P.swap_chain, 0, &IID_ID3D11Texture2D, (void **)&back);

	if (SUCCEEDED(result))
		result = ID3D11Device_CreateRenderTargetView(B.device, (ID3D11Resource *)back, NULL, &P.view);
	release(&back);
	if (backend_failed(result, "the swap chain's view"))
		platform_log("d3d11: the swap chain's buffer has no view (%s): nothing is presented", hr_text(result));
	return SUCCEEDED(result);
}

void d3d11_backend_present_release(void)
{
	release(&P.view);
	release(&P.swap_chain);
	release(&P.factory);
}

int d3d11_backend_window_set(void *hwnd, char *line, size_t line_size)
{
	DXGI_SWAP_CHAIN_DESC1 description;
	IDXGIDevice *dxgi = NULL;
	IDXGIAdapter *adapter = NULL;
	IDXGIFactory5 *factory5 = NULL;
	const char *call = "";
	HRESULT result;

	if (!B.device || B.lost || !hwnd)
	{
		snprintf(line, line_size, "d3d11: no swap chain (%s)", !hwnd ? "no window" : "no device");
		return 0;
	}
	if (P.swap_chain)
	{
		snprintf(line, line_size, "d3d11: swap chain %ux%u, made before", P.width, P.height);
		return 1;
	}
	/* the swap chain's factory is the device's own */
	result = ID3D11Device_QueryInterface(B.device, &IID_IDXGIDevice, (void **)&dxgi);
	call = "QueryInterface (IDXGIDevice)";
	if (SUCCEEDED(result))
	{
		result = IDXGIDevice_GetAdapter(dxgi, &adapter);
		call = "IDXGIDevice::GetAdapter";
	}
	if (SUCCEEDED(result))
	{
		result = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&P.factory);
		call = "IDXGIAdapter::GetParent (IDXGIFactory2)";
	}
	release(&adapter);
	release(&dxgi);
	if (FAILED(result))
	{
		backend_failed(result, call);
		snprintf(line, line_size, "d3d11: no swap chain (%s: %s)", call, hr_text(result));
		d3d11_backend_present_release();
		return 0;
	}
	if (SUCCEEDED(IDXGIFactory2_QueryInterface(P.factory, &IID_IDXGIFactory5, (void **)&factory5)))
	{
		BOOL allow = FALSE;

		result = IDXGIFactory5_CheckFeatureSupport(factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow));
		P.tearing = SUCCEEDED(result) && allow;
		release(&factory5);
	}
	P.swap_flags = P.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
	P.hwnd = (HWND)hwnd;

	memset(&description, 0, sizeof(description));
	/* (0: the window's client area) */
	description.Width = 0;
	description.Height = 0;
	description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	description.SampleDesc.Count = 1;
	description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	description.BufferCount = 2;
	description.Scaling = DXGI_SCALING_NONE;
	description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	description.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	description.Flags = P.swap_flags;
	result = IDXGIFactory2_CreateSwapChainForHwnd(P.factory, (IUnknown *)B.device, (HWND)hwnd, &description, NULL, NULL,
		&P.swap_chain);
	if (FAILED(result))
	{
		backend_failed(result, "CreateSwapChainForHwnd");
		snprintf(line, line_size, "d3d11: no swap chain (CreateSwapChainForHwnd: %s)", hr_text(result));
		d3d11_backend_present_release();
		return 0;
	}
	result = IDXGIFactory2_MakeWindowAssociation(P.factory, (HWND)hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
	if (FAILED(result))
		platform_log("d3d11: MakeWindowAssociation failed (%s): Alt+Enter may reach DXGI", hr_text(result));
	if (SUCCEEDED(IDXGISwapChain1_GetDesc1(P.swap_chain, &description)))
	{
		P.width = description.Width;
		P.height = description.Height;
	}
	if (!make_view())
	{
		snprintf(line, line_size, "d3d11: no swap chain (its buffer's view)");
		d3d11_backend_present_release();
		return 0;
	}
	snprintf(line, line_size, "d3d11: swap chain %ux%u, FLIP_DISCARD, 2 buffers, B8G8R8A8_UNORM, %s", P.width, P.height,
		P.tearing ? "tearing allowed" : "tearing not supported");
	return 1;
}

void d3d11_backend_present_marker(int on)
{
	P.marker = on;
}

uint32_t d3d11_backend_resizes(void)
{
	return P.resizes;
}

/* the buffers made again at the window's pixel size (the probe's resize):
nothing may hold them, so the view goes and the context lets go of it first */
static void resize(uint32_t width, uint32_t height)
{
	HRESULT result;

	ID3D11DeviceContext_OMSetRenderTargets(B.context, 0, NULL, NULL);
	d3d11_backend_targets_forget();
	release(&P.view);
	ID3D11DeviceContext_Flush(B.context);
	result = IDXGISwapChain1_ResizeBuffers(P.swap_chain, 0, width, height, DXGI_FORMAT_UNKNOWN, P.swap_flags);
	platform_log("d3d11: swap chain resized %ux%u to %ux%u (%s)", P.width, P.height, width, height, hr_text(result));
	P.resizes++;
	if (backend_failed(result, "ResizeBuffers"))
	{
		P.failed_width = width;
		P.failed_height = height;
		return;
	}
	P.failed_width = P.failed_height = 0;
	P.width = width;
	P.height = height;
	make_view();
}

/* a frame with nothing shown (minimised): the probe's rest, so that a game
with no frame limit does not spin while nothing can be seen */
static int nothing_shown(void)
{
	Sleep(16);
	return 0;
}

int d3d11_backend_present(uint32_t back_buffer, uint32_t window_width, uint32_t window_height, int sync_interval)
{
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	static const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
	static const float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
	ID3D11ShaderResourceView *picture, *none = NULL;
	uint32_t picture_width = 0, picture_height = 0;
	D3D11_VIEWPORT viewport;
	D3D11_RECT rectangle;
	long width, height, x, y;
	HRESULT result;
	int index;

	if (B.lost)
		return -1;
	if (!B.device || !P.swap_chain)
		return 0;
	/* 1. the resize; a size of 0, or a minimised window, presents nothing
	(SDL keeps the size the window had, and a minimised window's present
	answers S_OK in the flip model, as phase 0 saw) */
	if (!window_width || !window_height || IsIconic(P.hwnd))
		return nothing_shown();
	if ((window_width != P.width || window_height != P.height) &&
		(window_width != P.failed_width || window_height != P.failed_height))
	{
		resize(window_width, window_height);
	}
	if (B.lost)
		return -1;
	if (!P.view)
		return 0;

	/* 2. the letterbox, as d3d8_gl.c's Present works it out: the largest
	rectangle of the back buffer's shape that fits, centred */
	picture = d3d11_backend_target_view(back_buffer, &picture_width, &picture_height);
	if (!picture_width || !picture_height)
	{
		picture_width = P.width;
		picture_height = P.height;
	}
	width = (long)P.width;
	height = (long)((long long)P.width * picture_height / picture_width);
	if (height > (long)P.height)
	{
		height = (long)P.height;
		width = (long)((long long)P.height * picture_width / picture_height);
	}
	x = ((long)P.width - width) / 2;
	y = ((long)P.height - height) / 2;

	/* 3. the draw: the swap chain's buffer bound (the game's targets with
	it, so the back buffer is not bound while it is sampled), cleared
	black, the picture drawn into the letterbox */
	ID3D11DeviceContext_OMSetRenderTargets(B.context, 1, &P.view, NULL);
	d3d11_backend_targets_forget();
	ID3D11DeviceContext_ClearRenderTargetView(B.context, P.view, black);
	if (picture && d3d11_backend_shaders())
	{
		viewport.TopLeftX = (float)x;
		viewport.TopLeftY = (float)y;
		viewport.Width = (float)width;
		viewport.Height = (float)height;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		rectangle.left = x;
		rectangle.top = y;
		rectangle.right = x + width;
		rectangle.bottom = y + height;
		ID3D11DeviceContext_RSSetViewports(B.context, 1, &viewport);
		ID3D11DeviceContext_IASetInputLayout(B.context, NULL);
		ID3D11DeviceContext_IASetPrimitiveTopology(B.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ID3D11DeviceContext_VSSetShader(B.context, d3d11_backend_triangle_vs, NULL, 0);
		ID3D11DeviceContext_PSSetShader(B.context, d3d11_backend_present_ps, NULL, 0);
		ID3D11DeviceContext_PSSetShaderResources(B.context, 0, 1, &picture);
		ID3D11DeviceContext_PSSetSamplers(B.context, 0, 1, &d3d11_backend_linear_sampler);
		ID3D11DeviceContext_OMSetBlendState(B.context, d3d11_backend_write_all, NULL, 0xffffffff);
		ID3D11DeviceContext_OMSetDepthStencilState(B.context, d3d11_backend_depth_off, 0);
		ID3D11DeviceContext_RSSetState(B.context, d3d11_backend_scissored);
		ID3D11DeviceContext_RSSetScissorRects(B.context, 1, &rectangle);
		ID3D11DeviceContext_Draw(B.context, 3, 0);
		/* (the back buffer is a target again at the next clear) */
		ID3D11DeviceContext_PSSetShaderResources(B.context, 0, 1, &none);

		/* 4. the marker, over the picture: red at its top left, green at its
		top right, through the clears' drawn path */
		if (P.marker && width >= MARKER_SIZE && height >= MARKER_SIZE)
		{
			viewport.TopLeftX = 0.0f;
			viewport.TopLeftY = 0.0f;
			viewport.Width = (float)P.width;
			viewport.Height = (float)P.height;
			ID3D11DeviceContext_RSSetViewports(B.context, 1, &viewport);
			rectangle.left = x;
			rectangle.top = y;
			rectangle.right = x + MARKER_SIZE;
			rectangle.bottom = y + MARKER_SIZE;
			d3d11_backend_draw_clear(D3D11_BACKEND_CLEAR_COLOR, red, 0.0f, 0, &rectangle);
			rectangle.left = x + width - MARKER_SIZE;
			rectangle.right = x + width;
			d3d11_backend_draw_clear(D3D11_BACKEND_CLEAR_COLOR, green, 0.0f, 0, &rectangle);
		}
	}

	/* 5. the present: tearing where the interval is 0 and it is allowed (the
	probe's rule) */
	result = IDXGISwapChain1_Present(P.swap_chain, (UINT)sync_interval,
		sync_interval == 0 && P.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
	if (result == S_OK)
		return 1;
	if (result == DXGI_STATUS_OCCLUDED)
		return nothing_shown();
	if (backend_failed(result, "Present") && B.lost)
		return -1;
	for (index = 0; index < P.seen_count && P.seen[index] != result; index++)
	{
	}
	if (index == P.seen_count && P.seen_count < (int)(sizeof(P.seen) / sizeof(P.seen[0])))
	{
		P.seen[P.seen_count++] = result;
		platform_log("d3d11: Present answered %s (logged once)", hr_text(result));
	}
	return 0;
}
