/*
WIN32_D3D11_POST.C

The Direct3D 11 backend's anti-aliasing passes (port/windows/D3D11.md, phase
6, step 5): display.anti_aliasing's FXAA and SMAA over a window's 3D view,
as port/linux/src/xgpu_post.c runs them under OpenGL, every piece of a pass a
Direct3D object. Each pass draws one triangle over the rectangle (the
built-in triangle's vertex shader, from SV_VertexID, the viewport the
rectangle) and finds its texels from SV_Position: a target's rows are the
picture's from the top, as the passes' own textures' are, so nothing is
turned over. The target's pixels are copied first (CopySubresourceRegion of
the rectangle), so that no pass reads what it writes, and only colour is
written back: the game keeps values of its own in destination alpha. The
shaders are compiled when the setting is chosen (d3d11_backend_anti_alias_
prepare), not in the middle of a frame.

FXAA is xgpu_post.c's, in HLSL (SV_Position for gl_FragCoord, SampleLevel
for textureLod, a constant buffer for the uniforms). SMAA is the embedded
port/third_party/smaa/SMAA.hlsl (xgpu_smaa_shader, tools/embed_assets.py),
compiled at its HIGH preset with SMAA_HLSL_4 and SMAA_RT_METRICS a constant
buffer's member, with xgpu_post.c's three main functions; its LinearSampler
and PointSampler are its own declarations, whose registers are asked of the
compiled shader (D3DReflect). Its lookup textures' texels are the device's:
it inflates them with the port's zlib, which the Windows SDK's units do not
have. The seventh unit of the backend, sharing win32_d3d11.h.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32_d3d11.h"

/* port/third_party/smaa, embedded by tools/embed_assets.py: the shader's text,
ending in a NUL (size 0 where the build had no such file) */
extern const unsigned int xgpu_smaa_shader[];
extern const unsigned long xgpu_smaa_shader_size;

enum
{
	PASS_FXAA,
	PASS_SMAA_EDGES,
	PASS_SMAA_WEIGHTS,
	PASS_SMAA_BLEND,
	PASS_COUNT
};

/* the passes' constant buffer: 1 / width, 1 / height, width, height; and
FXAA's bounds, the rectangle's outermost texel centres as texture
coordinates */
static const char constants_hlsl[] =
	"cbuffer post_constants : register(b0)\n"
	"{\n"
	"\tfloat4 metrics;\n"
	"\tfloat4 bounds;\n"
	"};\n";

/* xgpu_post.c's fxaa_source, in HLSL */
static const char fxaa_hlsl[] =
	"Texture2D color_texture : register(t0);\n"
	"SamplerState linear_clamp : register(s0);\n"
	"\n"
	"static const float EDGE_THRESHOLD = 0.125;\n"
	"static const float EDGE_THRESHOLD_MINIMUM = 0.0312;\n"
	"static const float SUBPIXEL_QUALITY = 0.75;\n"
	"static const int STEPS = 12;\n"
	"static const float STEP_LENGTHS[12] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };\n"
	"\n"
	"float3 color_at(float2 position)\n"
	"{\n"
	"\treturn color_texture.SampleLevel(linear_clamp, clamp(position, bounds.xy, bounds.zw), 0.0).rgb;\n"
	"}\n"
	"\n"
	"float luma_at(float2 position)\n"
	"{\n"
	"\treturn dot(color_at(position), float3(0.299, 0.587, 0.114));\n"
	"}\n"
	"\n"
	"float4 main(float4 fragment : SV_Position) : SV_Target\n"
	"{\n"
	"\tfloat2 position = fragment.xy * metrics.xy;\n"
	"\tfloat luma = luma_at(position);\n"
	"\tfloat up = luma_at(position + float2(0.0, -metrics.y));\n"
	"\tfloat down = luma_at(position + float2(0.0, metrics.y));\n"
	"\tfloat left = luma_at(position + float2(-metrics.x, 0.0));\n"
	"\tfloat right = luma_at(position + float2(metrics.x, 0.0));\n"
	"\tfloat highest = max(luma, max(max(up, down), max(left, right)));\n"
	"\tfloat range = highest - min(luma, min(min(up, down), min(left, right)));\n"
	"\n"
	"\t/* no edge: the target has the pixel already */\n"
	"\tif (range < max(EDGE_THRESHOLD_MINIMUM, highest * EDGE_THRESHOLD))\n"
	"\t\tdiscard;\n"
	"\tfloat up_left = luma_at(position + float2(-metrics.x, -metrics.y));\n"
	"\tfloat up_right = luma_at(position + float2(metrics.x, -metrics.y));\n"
	"\tfloat down_left = luma_at(position + float2(-metrics.x, metrics.y));\n"
	"\tfloat down_right = luma_at(position + float2(metrics.x, metrics.y));\n"
	"\n"
	"\t/* how much the pixel stands out from its neighbourhood (a feature\n"
	"\tthinner than a pixel) */\n"
	"\tfloat average = (2.0 * (up + down + left + right) + up_left + up_right + down_left + down_right) / 12.0;\n"
	"\tfloat subpixel = smoothstep(0.0, 1.0, clamp(abs(average - luma) / range, 0.0, 1.0));\n"
	"\tsubpixel = subpixel * subpixel * SUBPIXEL_QUALITY;\n"
	"\n"
	"\t/* an edge along the rows where the luma changes more from row to row\n"
	"\tthan from column to column; across it, the side it changes more to */\n"
	"\tbool horizontal = abs(up_left + down_left - 2.0 * left) + 2.0 * abs(up + down - 2.0 * luma) +\n"
	"\t\tabs(up_right + down_right - 2.0 * right) >=\n"
	"\t\tabs(up_left + up_right - 2.0 * up) + 2.0 * abs(left + right - 2.0 * luma) +\n"
	"\t\tabs(down_left + down_right - 2.0 * down);\n"
	"\tfloat before = horizontal ? up : left;\n"
	"\tfloat after = horizontal ? down : right;\n"
	"\tbool before_steeper = abs(before - luma) >= abs(after - luma);\n"
	"\tfloat gradient = 0.25 * max(abs(before - luma), abs(after - luma));\n"
	"\tfloat edge_luma = 0.5 * ((before_steeper ? before : after) + luma);\n"
	"\tfloat2 across = (horizontal ? float2(0.0, metrics.y) : float2(metrics.x, 0.0)) * (before_steeper ? -1.0 : 1.0);\n"
	"\tfloat2 along = horizontal ? float2(metrics.x, 0.0) : float2(0.0, metrics.y);\n"
	"\n"
	"\t/* along the edge, half a pixel across, to where its luma ends either way */\n"
	"\tfloat2 end_before = position + 0.5 * across - along;\n"
	"\tfloat2 end_after = position + 0.5 * across + along;\n"
	"\tfloat luma_before = luma_at(end_before) - edge_luma;\n"
	"\tfloat luma_after = luma_at(end_after) - edge_luma;\n"
	"\tbool reached_before = abs(luma_before) >= gradient;\n"
	"\tbool reached_after = abs(luma_after) >= gradient;\n"
	"\t[loop] for (int index = 1; index < STEPS && !(reached_before && reached_after); index++)\n"
	"\t{\n"
	"\t\tif (!reached_before)\n"
	"\t\t{\n"
	"\t\t\tend_before -= along * STEP_LENGTHS[index];\n"
	"\t\t\tluma_before = luma_at(end_before) - edge_luma;\n"
	"\t\t\treached_before = abs(luma_before) >= gradient;\n"
	"\t\t}\n"
	"\t\tif (!reached_after)\n"
	"\t\t{\n"
	"\t\t\tend_after += along * STEP_LENGTHS[index];\n"
	"\t\t\tluma_after = luma_at(end_after) - edge_luma;\n"
	"\t\t\treached_after = abs(luma_after) >= gradient;\n"
	"\t\t}\n"
	"\t}\n"
	"\n"
	"\t/* blended across the edge the more, the nearer the pixel is to an end;\n"
	"\tonly where the luma at that end changes the way the pixel's does */\n"
	"\tfloat distance_before = horizontal ? position.x - end_before.x : position.y - end_before.y;\n"
	"\tfloat distance_after = horizontal ? end_after.x - position.x : end_after.y - position.y;\n"
	"\tfloat pixel_offset = 0.5 - min(distance_before, distance_after) / (distance_before + distance_after);\n"
	"\tbool varies = ((distance_before < distance_after ? luma_before : luma_after) < 0.0) != (luma < edge_luma);\n"
	"\treturn float4(color_at(position + max(varies ? pixel_offset : 0.0, subpixel) * across), 1.0);\n"
	"}\n";

/* the main functions of SMAA's passes (xgpu_post.c's smaa_mains), its vertex
functions' offsets found for each pixel */
static const char *const smaa_mains[3] =
{
	/* edges */
	"Texture2D color_texture : register(t0);\n"
	"\n"
	"float4 main(float4 fragment : SV_Position) : SV_Target\n"
	"{\n"
	"\tfloat2 texcoord = fragment.xy * metrics.xy;\n"
	"\tfloat4 offset[3];\n"
	"\tSMAAEdgeDetectionVS(texcoord, offset);\n"
	"\treturn float4(SMAALumaEdgeDetectionPS(texcoord, offset, color_texture), 0.0, 0.0);\n"
	"}\n",
	/* weights */
	"Texture2D edges_texture : register(t0);\n"
	"Texture2D area_texture : register(t1);\n"
	"Texture2D search_texture : register(t2);\n"
	"\n"
	"float4 main(float4 fragment : SV_Position) : SV_Target\n"
	"{\n"
	"\tfloat2 texcoord = fragment.xy * metrics.xy;\n"
	"\tfloat2 pixcoord;\n"
	"\tfloat4 offset[3];\n"
	"\tSMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);\n"
	"\treturn SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edges_texture, area_texture,\n"
	"\t\tsearch_texture, float4(0.0, 0.0, 0.0, 0.0));\n"
	"}\n",
	/* blend */
	"Texture2D color_texture : register(t0);\n"
	"Texture2D weights_texture : register(t1);\n"
	"\n"
	"float4 main(float4 fragment : SV_Position) : SV_Target\n"
	"{\n"
	"\tfloat2 texcoord = fragment.xy * metrics.xy;\n"
	"\tfloat4 offset;\n"
	"\tSMAANeighborhoodBlendingVS(texcoord, offset);\n"
	"\treturn SMAANeighborhoodBlendingPS(texcoord, offset, color_texture, weights_texture);\n"
	"}\n",
};

static const char *const pass_names[PASS_COUNT] =
{
	"FXAA", "SMAA edge detection", "SMAA blending weight", "SMAA neighborhood blending"
};

static struct
{
	ID3D11PixelShader *shaders[PASS_COUNT];
	/* SMAA's LinearSampler and PointSampler, each pass's registers for them
	(-1: the pass does not sample through it) */
	int linear_slot[PASS_COUNT], point_slot[PASS_COUNT];
	/* FXAA's, and SMAA's, shaders or textures could not be made (once
	failed, they stay so) */
	int failed[2];
	ID3D11Buffer *constants;
	/* linear and clamped (FXAA, SMAA's LinearSampler: MIN_MAG_LINEAR_MIP_POINT),
	point and clamped (SMAA's PointSampler) */
	ID3D11SamplerState *linear, *point;
	/* colour only (the last pass), and all four channels (SMAA's
	intermediates) */
	ID3D11BlendState *color_only, *all;
	/* the size of the textures below */
	uint32_t width, height;
	/* the target's pixels before the pass (its format, so that the copy is
	exact), and SMAA's edges and blending weights */
	ID3D11Texture2D *color, *edges, *weights;
	ID3D11ShaderResourceView *color_view, *edges_view, *weights_view;
	ID3D11RenderTargetView *edges_target, *weights_target;
	/* SMAA's lookup textures */
	ID3D11Texture2D *area, *search;
	ID3D11ShaderResourceView *area_view, *search_view;
	int textures_failed_logged;
} P;

static void textures_release(void)
{
	release(&P.color_view);
	release(&P.edges_view);
	release(&P.weights_view);
	release(&P.edges_target);
	release(&P.weights_target);
	release(&P.color);
	release(&P.edges);
	release(&P.weights);
	P.width = P.height = 0;
}

void d3d11_backend_post_release(void)
{
	int which;

	textures_release();
	for (which = 0; which < PASS_COUNT; which++)
		release(&P.shaders[which]);
	release(&P.constants);
	release(&P.linear);
	release(&P.point);
	release(&P.color_only);
	release(&P.all);
	release(&P.area_view);
	release(&P.search_view);
	release(&P.area);
	release(&P.search);
	memset(&P, 0, sizeof(P));
}

/* a pass's pixel shader; 0 if it does not compile (logged by the compiler) */
static int shader_make(int which)
{
	size_t smaa_length = xgpu_smaa_shader_size ? strlen((const char *)xgpu_smaa_shader) : 0;
	size_t size = sizeof(constants_hlsl) + sizeof(fxaa_hlsl) + smaa_length + 1024;
	char *source = malloc(size);
	ID3DBlob *code = NULL;
	HRESULT result;
	UINT slot;
	int length;

	if (!source)
		return 0;
	if (which == PASS_FXAA)
		length = snprintf(source, size, "%s%s", constants_hlsl, fxaa_hlsl);
	else if (smaa_length)
		length = snprintf(source, size, "%s#define SMAA_HLSL_4 1\n#define SMAA_RT_METRICS metrics\n"
			"#define SMAA_PRESET_HIGH 1\n%s\n%s", constants_hlsl, (const char *)xgpu_smaa_shader,
			smaa_mains[which - PASS_SMAA_EDGES]);
	else
	{
		platform_log("anti-aliasing: the build has no SMAA");
		free(source);
		return 0;
	}
	if (!d3d11_backend_compile(source, (size_t)length, pass_names[which], "main", "ps_4_0", &code))
	{
		free(source);
		return 0;
	}
	free(source);
	result = ID3D11Device_CreatePixelShader(B.device, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
		NULL, &P.shaders[which]);
	P.linear_slot[which] = P.point_slot[which] = -1;
	if (which == PASS_FXAA)
		P.linear_slot[which] = 0;
	else
	{
		if (d3d11_backend_sampler_register(code, "LinearSampler", &slot))
			P.linear_slot[which] = (int)slot;
		if (d3d11_backend_sampler_register(code, "PointSampler", &slot))
			P.point_slot[which] = (int)slot;
	}
	release(&code);
	if (backend_failed(result, "an anti-aliasing pass's shader"))
	{
		platform_log("anti-aliasing: %s's pixel shader was not made (%s)", pass_names[which], hr_text(result));
		return 0;
	}
	return 1;
}

/* a texture of the passes, with its view, and a render target view if asked
for; initial texels if given (row_pitch bytes a row) */
static int texture_make(DXGI_FORMAT format, uint32_t width, uint32_t height, int target, const void *texels,
	uint32_t row_pitch, ID3D11Texture2D **texture, ID3D11ShaderResourceView **view, ID3D11RenderTargetView **render)
{
	D3D11_TEXTURE2D_DESC description;
	D3D11_SUBRESOURCE_DATA data;
	HRESULT result;

	memset(&description, 0, sizeof(description));
	description.Width = width;
	description.Height = height;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = format;
	description.SampleDesc.Count = 1;
	description.Usage = texels ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
	description.BindFlags = D3D11_BIND_SHADER_RESOURCE | (target ? D3D11_BIND_RENDER_TARGET : 0);
	data.pSysMem = texels;
	data.SysMemPitch = row_pitch;
	data.SysMemSlicePitch = 0;
	result = ID3D11Device_CreateTexture2D(B.device, &description, texels ? &data : NULL, texture);
	if (SUCCEEDED(result))
		result = ID3D11Device_CreateShaderResourceView(B.device, (ID3D11Resource *)*texture, NULL, view);
	if (SUCCEEDED(result) && target)
		result = ID3D11Device_CreateRenderTargetView(B.device, (ID3D11Resource *)*texture, NULL, render);
	if (backend_failed(result, "an anti-aliasing pass's texture"))
	{
		if (!P.textures_failed_logged)
		{
			P.textures_failed_logged = 1;
			platform_log("anti-aliasing: a texture of %ux%u was not made (%s; logged once)", width, height,
				hr_text(result));
		}
		return 0;
	}
	return 1;
}

int d3d11_backend_anti_alias_prepare(int smaa, const void *area, const void *search)
{
	int first = smaa ? PASS_SMAA_EDGES : PASS_FXAA, last = smaa ? PASS_SMAA_BLEND : PASS_FXAA;
	int which;
	HRESULT result = S_OK;

	smaa = smaa != 0;
	if (!B.device || B.lost || P.failed[smaa])
		return 0;
	if (!d3d11_backend_shaders())
	{
		P.failed[smaa] = 1;
		return 0;
	}
	for (which = first; which <= last; which++)
	{
		if (!P.shaders[which] && !shader_make(which))
		{
			P.failed[smaa] = 1;
			return 0;
		}
	}
	if (smaa && !P.area)
	{
		if (!area || !search ||
			!texture_make(DXGI_FORMAT_R8G8_UNORM, 160, 560, 0, area, 160 * 2, &P.area, &P.area_view, NULL) ||
			!texture_make(DXGI_FORMAT_R8_UNORM, 64, 16, 0, search, 64, &P.search, &P.search_view, NULL))
		{
			platform_log("anti-aliasing: SMAA's lookup textures were not made");
			release(&P.area_view);
			release(&P.area);
			P.failed[1] = 1;
			return 0;
		}
	}
	if (!P.constants)
	{
		D3D11_BUFFER_DESC buffer;
		D3D11_SAMPLER_DESC sampler;
		D3D11_BLEND_DESC blend;

		memset(&buffer, 0, sizeof(buffer));
		buffer.ByteWidth = 32;
		buffer.Usage = D3D11_USAGE_DYNAMIC;
		buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		result = ID3D11Device_CreateBuffer(B.device, &buffer, NULL, &P.constants);
		memset(&sampler, 0, sizeof(sampler));
		sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler.MaxAnisotropy = 1;
		sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sampler.MaxLOD = D3D11_FLOAT32_MAX;
		if (SUCCEEDED(result))
			result = ID3D11Device_CreateSamplerState(B.device, &sampler, &P.linear);
		sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		if (SUCCEEDED(result))
			result = ID3D11Device_CreateSamplerState(B.device, &sampler, &P.point);
		memset(&blend, 0, sizeof(blend));
		blend.RenderTarget[0].BlendEnable = FALSE;
		blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blend.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
		blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
			D3D11_COLOR_WRITE_ENABLE_BLUE;
		if (SUCCEEDED(result))
			result = ID3D11Device_CreateBlendState(B.device, &blend, &P.color_only);
		blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (SUCCEEDED(result))
			result = ID3D11Device_CreateBlendState(B.device, &blend, &P.all);
		if (backend_failed(result, "the anti-aliasing passes' objects"))
		{
			platform_log("anti-aliasing: the passes' objects were not made (%s)", hr_text(result));
			release(&P.constants);
			P.failed[0] = P.failed[1] = 1;
			return 0;
		}
	}
	return 1;
}

/* the passes' textures, of the target's size (SMAA's only for SMAA) */
static int textures_fit(int smaa, uint32_t width, uint32_t height)
{
	if (P.width != width || P.height != height)
	{
		textures_release();
		if (!texture_make(DXGI_FORMAT_B8G8R8A8_UNORM, width, height, 0, NULL, 0, &P.color, &P.color_view, NULL))
			return 0;
		P.width = width;
		P.height = height;
	}
	if (smaa && !P.edges)
	{
		if (!texture_make(DXGI_FORMAT_R8G8_UNORM, width, height, 1, NULL, 0, &P.edges, &P.edges_view,
			&P.edges_target) ||
			!texture_make(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, NULL, 0, &P.weights, &P.weights_view,
			&P.weights_target))
		{
			textures_release();
			return 0;
		}
	}
	return 1;
}

/* a pass's triangle drawn into a target over the rectangle, with these
textures in slots 0 to 2 (the rest none); color_only keeps the target's
alpha */
static void pass_draw(int which, ID3D11RenderTargetView *target, const D3D11_RECT *rectangle, int color_only,
	ID3D11ShaderResourceView *t0, ID3D11ShaderResourceView *t1, ID3D11ShaderResourceView *t2)
{
	ID3D11ShaderResourceView *views[3] = { NULL, NULL, NULL };
	D3D11_VIEWPORT viewport;

	/* (the target first, which lets go of a texture that was the last
	pass's target, then the textures) */
	ID3D11DeviceContext_PSSetShaderResources(B.context, 0, 3, views);
	ID3D11DeviceContext_OMSetRenderTargets(B.context, 1, &target, NULL);
	views[0] = t0;
	views[1] = t1;
	views[2] = t2;
	ID3D11DeviceContext_PSSetShaderResources(B.context, 0, 3, views);
	viewport.TopLeftX = (float)rectangle->left;
	viewport.TopLeftY = (float)rectangle->top;
	viewport.Width = (float)(rectangle->right - rectangle->left);
	viewport.Height = (float)(rectangle->bottom - rectangle->top);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	ID3D11DeviceContext_RSSetViewports(B.context, 1, &viewport);
	ID3D11DeviceContext_RSSetScissorRects(B.context, 1, rectangle);
	ID3D11DeviceContext_OMSetBlendState(B.context, color_only ? P.color_only : P.all, NULL, 0xffffffff);
	ID3D11DeviceContext_PSSetShader(B.context, P.shaders[which], NULL, 0);
	if (P.linear_slot[which] >= 0)
		ID3D11DeviceContext_PSSetSamplers(B.context, (UINT)P.linear_slot[which], 1, &P.linear);
	if (P.point_slot[which] >= 0)
		ID3D11DeviceContext_PSSetSamplers(B.context, (UINT)P.point_slot[which], 1, &P.point);
	ID3D11DeviceContext_Draw(B.context, 3, 0);
}

/* an intermediate emptied, all of it, so that no edge of another window's
pass is found: SMAA's passes write only where there is an edge */
static void intermediate_clear(ID3D11RenderTargetView *target)
{
	static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	ID3D11DeviceContext_ClearRenderTargetView(B.context, target, zero);
}

int d3d11_backend_anti_alias(int smaa, uint32_t handle, const int32_t corners[4])
{
	struct target *target = d3d11_backend_target_at(handle);
	ID3D11ShaderResourceView *none[3] = { NULL, NULL, NULL };
	D3D11_MAPPED_SUBRESOURCE mapped;
	D3D11_RECT rectangle;
	D3D11_BOX box;
	float *constants;
	HRESULT result;

	smaa = smaa != 0;
	/* (prepared already, as the setting was chosen) */
	if (!B.device || B.lost || !target || target->is_depth || target->samples ||
		!d3d11_backend_anti_alias_prepare(smaa, NULL, NULL))
	{
		return 0;
	}
	rectangle.left = corners[0] > 0 ? corners[0] : 0;
	rectangle.top = corners[1] > 0 ? corners[1] : 0;
	rectangle.right = corners[2] < (int32_t)target->width ? corners[2] : (LONG)target->width;
	rectangle.bottom = corners[3] < (int32_t)target->height ? corners[3] : (LONG)target->height;
	if (rectangle.left >= rectangle.right || rectangle.top >= rectangle.bottom)
		return 0;
	if (!textures_fit(smaa, target->width, target->height))
		return 0;

	result = ID3D11DeviceContext_Map(B.context, (ID3D11Resource *)P.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (backend_failed(result, "Map (the anti-aliasing passes' constants)"))
		return 0;
	constants = mapped.pData;
	constants[0] = 1.0f / (float)P.width;
	constants[1] = 1.0f / (float)P.height;
	constants[2] = (float)P.width;
	constants[3] = (float)P.height;
	/* (split screen: no texel of the next window is read) */
	constants[4] = ((float)rectangle.left + 0.5f) / (float)P.width;
	constants[5] = ((float)rectangle.top + 0.5f) / (float)P.height;
	constants[6] = ((float)rectangle.right - 0.5f) / (float)P.width;
	constants[7] = ((float)rectangle.bottom - 0.5f) / (float)P.height;
	ID3D11DeviceContext_Unmap(B.context, (ID3D11Resource *)P.constants, 0);

	/* the target's pixels, read while the target is drawn into; the target's
	view taken from the draws' stages before it is bound */
	d3d11_backend_draw_target_unbind(target->view);
	box.left = (UINT)rectangle.left;
	box.top = (UINT)rectangle.top;
	box.right = (UINT)rectangle.right;
	box.bottom = (UINT)rectangle.bottom;
	box.front = 0;
	box.back = 1;
	ID3D11DeviceContext_CopySubresourceRegion(B.context, (ID3D11Resource *)P.color, 0, box.left, box.top, 0,
		(ID3D11Resource *)target->texture, 0, &box);

	ID3D11DeviceContext_IASetInputLayout(B.context, NULL);
	ID3D11DeviceContext_IASetPrimitiveTopology(B.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_VSSetShader(B.context, d3d11_backend_triangle_vs, NULL, 0);
	ID3D11DeviceContext_PSSetConstantBuffers(B.context, 0, 1, &P.constants);
	ID3D11DeviceContext_OMSetDepthStencilState(B.context, d3d11_backend_depth_off, 0);
	ID3D11DeviceContext_RSSetState(B.context, d3d11_backend_scissored);
	if (!smaa)
	{
		ID3D11DeviceContext_PSSetSamplers(B.context, 0, 1, &P.linear);
		pass_draw(PASS_FXAA, target->color, &rectangle, 1, P.color_view, NULL, NULL);
	}
	else
	{
		intermediate_clear(P.edges_target);
		pass_draw(PASS_SMAA_EDGES, P.edges_target, &rectangle, 0, P.color_view, NULL, NULL);
		intermediate_clear(P.weights_target);
		pass_draw(PASS_SMAA_WEIGHTS, P.weights_target, &rectangle, 0, P.edges_view, P.area_view, P.search_view);
		pass_draw(PASS_SMAA_BLEND, target->color, &rectangle, 1, P.color_view, P.weights_view, NULL);
	}
	/* (the textures let go of; what the draws bound, bound again by the next
	draw) */
	ID3D11DeviceContext_PSSetShaderResources(B.context, 0, 3, none);
	d3d11_backend_targets_rebind();
	d3d11_backend_state_forget();
	return 1;
}
