/*
WIN32_D3D11_DRAW.C

The Direct3D 11 backend's draw (port/windows/D3D11.md, phase 6): the
device's record (d3d11_backend.h, struct d3d11_backend_draw) bound and drawn.
The backend keeps what it last bound, as d3d8_gl.c's gl_state kept what GL
held, and sets only what differs: the shaders, the layout, the state objects
with their dynamic values, the viewport and the scissor (the same rectangle:
the NV2A's scissor follows the viewport, which keeps split screen apart),
the vertex buffers of the slots that changed (the stream ring, or a segment
of the mirror), the index buffer, the topology, the textures' views and the
samplers of the stages that changed; the attribute slot's buffer (the
sixteen current values of the input registers, read by the inputs the
layout's elements leave out, as per-instance data) written when the 256
bytes differ; the four constant buffers, written by the device when its
serials say they changed and bound at every draw. The shadow is forgotten
by whatever binds behind it (the clears, the present, the passes, the
self-tests: d3d11_backend_state_forget).

One place Direct3D 11 is stricter than GL: a resource bound as a target and
as a texture at once. GL let the back buffer be sampled while drawn into
(d3d8_gl.c prepare_draw's comment); Direct3D 11 unbinds the view and the
layer warns. A stage whose target is the bound colour target samples a copy
(CopyResource into a scratch texture of the target's size and format, one
kept a size, counted as a self-sampling copy), and when a target is about
to be bound any stage holding its view is unbound first
(d3d11_backend_draw_target_unbind). The fifth unit of the backend, sharing
win32_d3d11.h.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32_d3d11.h"
#include "hlsl_shaders.h"

_Static_assert(D3D11_BACKEND_TOPOLOGY_POINTLIST == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST &&
	D3D11_BACKEND_TOPOLOGY_LINELIST == D3D11_PRIMITIVE_TOPOLOGY_LINELIST &&
	D3D11_BACKEND_TOPOLOGY_LINESTRIP == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP &&
	D3D11_BACKEND_TOPOLOGY_TRIANGLELIST == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
	D3D11_BACKEND_TOPOLOGY_TRIANGLESTRIP == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, "topology");
_Static_assert(D3D11_BACKEND_STREAM_COUNT == D3D11_BACKEND_ATTRIBUTE_SLOT, "the attribute slot follows the streams");

/* the attribute slot's bytes: sixteen float4s */
#define ATTRIBUTES_SIZE 256

/* the self-sampling copies' scratch textures, one a size */
#define SCRATCH_COUNT 8

static const uint32_t constants_sizes[4] = { HLSL_VERTEX_CONSTANTS_SIZE, HLSL_VERTEX_PARAMETERS_SIZE,
	HLSL_PIXEL_PARAMETERS_SIZE, HLSL_MODEL_LIGHTS_SIZE };

static struct
{
	/* made at the first draw: the constant buffers and the attribute slot's */
	int made, failed;
	ID3D11Buffer *constants[4];
	ID3D11Buffer *attributes;
	/* the attribute slot's bytes as last written (valid once written) */
	unsigned char attribute_bytes[ATTRIBUTES_SIZE];
	int attributes_written;

	/* what is bound (valid: since the last forget) */
	int valid;
	uint32_t vertex_shader, pixel_shader, layout;
	uint32_t blend, depth_stencil, rasterizer;
	float blend_factor[4];
	uint32_t stencil_reference;
	int32_t viewport[4];
	float min_depth, max_depth;
	int viewport_valid;
	struct
	{
		ID3D11Buffer *buffer;
		UINT offset, stride;
	} slots[D3D11_BACKEND_STREAM_COUNT + 1];
	ID3D11Buffer *index_buffer;
	D3D11_PRIMITIVE_TOPOLOGY topology;
	ID3D11ShaderResourceView *views[D3D11_BACKEND_STAGE_COUNT];
	uint32_t samplers[D3D11_BACKEND_STAGE_COUNT];
	int constants_bound;

	/* the self-sampling copies' scratch textures */
	struct
	{
		ID3D11Texture2D *texture;
		ID3D11ShaderResourceView *view;
		uint32_t width, height;
	} scratch[SCRATCH_COUNT];
	uint32_t scratch_next;

	/* an empty handle, logged once a kind */
	int missing_logged[8];

	uint32_t draws, binds_skipped, self_sampling_copies;
} D;

static int draw_objects_make(void)
{
	D3D11_BUFFER_DESC description;
	HRESULT result = S_OK;
	int index;

	if (D.made)
		return !D.failed;
	D.made = 1;
	memset(&description, 0, sizeof(description));
	description.Usage = D3D11_USAGE_DYNAMIC;
	description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	for (index = 0; index < 4 && SUCCEEDED(result); index++)
	{
		description.ByteWidth = constants_sizes[index];
		result = ID3D11Device_CreateBuffer(B.device, &description, NULL, &D.constants[index]);
	}
	if (SUCCEEDED(result))
	{
		description.ByteWidth = ATTRIBUTES_SIZE;
		description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		result = ID3D11Device_CreateBuffer(B.device, &description, NULL, &D.attributes);
	}
	if (backend_failed(result, "the draw's buffers"))
	{
		platform_log("d3d11: the draw's constant buffers were not made (%s): nothing is drawn", hr_text(result));
		D.failed = 1;
		return 0;
	}
	return 1;
}

void d3d11_backend_state_forget(void)
{
	D.valid = 0;
	D.viewport_valid = 0;
	D.constants_bound = 0;
	memset(D.views, 0, sizeof(D.views));
	memset(D.slots, 0, sizeof(D.slots));
	D.index_buffer = NULL;
	D.topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

void d3d11_backend_draw_viewport_forget(void)
{
	D.viewport_valid = 0;
}

void d3d11_backend_draw_target_unbind(ID3D11ShaderResourceView *view)
{
	ID3D11ShaderResourceView *none = NULL;
	uint32_t stage;

	if (!view)
		return;
	for (stage = 0; stage < D3D11_BACKEND_STAGE_COUNT; stage++)
	{
		if (D.views[stage] == view)
		{
			ID3D11DeviceContext_PSSetShaderResources(B.context, stage, 1, &none);
			D.views[stage] = NULL;
		}
	}
}

/* a DYNAMIC buffer written whole (Map DISCARD) */
static int buffer_write(ID3D11Buffer *buffer, const void *data, uint32_t size, const char *what)
{
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT result = ID3D11DeviceContext_Map(B.context, (ID3D11Resource *)buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);

	if (backend_failed(result, what))
		return 0;
	memcpy(mapped.pData, data, size);
	ID3D11DeviceContext_Unmap(B.context, (ID3D11Resource *)buffer, 0);
	return 1;
}

void d3d11_backend_constants_write(uint32_t which, const void *data, uint32_t size)
{
	if (!B.device || B.lost || which >= 4 || !draw_objects_make())
		return;
	if (size != constants_sizes[which])
	{
		if (!D.missing_logged[4])
		{
			D.missing_logged[4] = 1;
			platform_log("d3d11: constant buffer %u written with %u bytes, not %u (logged once)", which, size,
				constants_sizes[which]);
		}
		return;
	}
	buffer_write(D.constants[which], data, size, "Map (a constant buffer)");
}

/* a scratch texture of the target's size and format, with the target's pixels
copied into it, for a stage that samples the target being drawn into */
static ID3D11ShaderResourceView *self_sampling_copy(struct target *target)
{
	uint32_t index;
	D3D11_TEXTURE2D_DESC description;
	HRESULT result;

	for (index = 0; index < SCRATCH_COUNT; index++)
	{
		if (D.scratch[index].texture && D.scratch[index].width == target->width &&
			D.scratch[index].height == target->height)
		{
			break;
		}
	}
	if (index == SCRATCH_COUNT)
	{
		index = D.scratch_next++ % SCRATCH_COUNT;
		release(&D.scratch[index].view);
		release(&D.scratch[index].texture);
		memset(&description, 0, sizeof(description));
		description.Width = target->width;
		description.Height = target->height;
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		result = ID3D11Device_CreateTexture2D(B.device, &description, NULL, &D.scratch[index].texture);
		if (SUCCEEDED(result))
			result = ID3D11Device_CreateShaderResourceView(B.device, (ID3D11Resource *)D.scratch[index].texture, NULL,
				&D.scratch[index].view);
		if (backend_failed(result, "a self-sampling copy's texture"))
		{
			release(&D.scratch[index].view);
			release(&D.scratch[index].texture);
			return NULL;
		}
		D.scratch[index].width = target->width;
		D.scratch[index].height = target->height;
	}
	ID3D11DeviceContext_CopyResource(B.context, (ID3D11Resource *)D.scratch[index].texture,
		(ID3D11Resource *)target->texture);
	D.self_sampling_copies++;
	return D.scratch[index].view;
}

static void missing(int kind, const char *what, uint32_t handle)
{
	if (D.missing_logged[kind])
		return;
	D.missing_logged[kind] = 1;
	platform_log("d3d11: a draw names %s %u, which does not exist: the draw is skipped (logged once)", what, handle);
}

int d3d11_backend_draw(const struct d3d11_backend_draw *draw)
{
	struct target *color = d3d11_backend_bound_color(), *depth = d3d11_backend_bound_depth();
	ID3D11VertexShader *vertex_shader;
	ID3D11PixelShader *pixel_shader;
	ID3D11InputLayout *layout;
	ID3D11BlendState *blend;
	ID3D11DepthStencilState *depth_stencil;
	ID3D11RasterizerState *rasterizer;
	uint32_t stage, slot;

	if (!B.device || B.lost || !draw_objects_make())
		return 0;
	if (!color && !depth)
		return 0;
	vertex_shader = (ID3D11VertexShader *)d3d11_backend_shader_object(D3D11_BACKEND_STAGE_VERTEX, draw->vertex_shader);
	pixel_shader = (ID3D11PixelShader *)d3d11_backend_shader_object(D3D11_BACKEND_STAGE_PIXEL, draw->pixel_shader);
	layout = d3d11_backend_layout_object(draw->layout);
	blend = d3d11_backend_blend_object(draw->blend);
	depth_stencil = d3d11_backend_depth_stencil_object(draw->depth_stencil);
	rasterizer = d3d11_backend_rasterizer_object(draw->rasterizer);
	if (!vertex_shader || !pixel_shader || !layout)
	{
		missing(0, !vertex_shader ? "the vertex shader" : !pixel_shader ? "the pixel shader" : "the input layout",
			!vertex_shader ? draw->vertex_shader : !pixel_shader ? draw->pixel_shader : draw->layout);
		return 0;
	}
	if (!blend || !depth_stencil || !rasterizer)
	{
		missing(1, !blend ? "the blend state" : !depth_stencil ? "the depth-stencil state" : "the rasterizer state",
			!blend ? draw->blend : !depth_stencil ? draw->depth_stencil : draw->rasterizer);
		return 0;
	}
	/* (a viewport of no width or height draws nothing, as GL's did) */
	if (draw->viewport[2] <= 0 || draw->viewport[3] <= 0)
	{
		D.draws++;
		return 1;
	}

	/* the shaders and the layout */
	if (!D.valid || D.vertex_shader != draw->vertex_shader)
	{
		ID3D11DeviceContext_VSSetShader(B.context, vertex_shader, NULL, 0);
		D.vertex_shader = draw->vertex_shader;
	}
	else
		D.binds_skipped++;
	if (!D.valid || D.pixel_shader != draw->pixel_shader)
	{
		ID3D11DeviceContext_PSSetShader(B.context, pixel_shader, NULL, 0);
		D.pixel_shader = draw->pixel_shader;
	}
	else
		D.binds_skipped++;
	if (!D.valid || D.layout != draw->layout)
	{
		ID3D11DeviceContext_IASetInputLayout(B.context, layout);
		D.layout = draw->layout;
	}
	else
		D.binds_skipped++;

	/* the state objects with their dynamic values */
	if (!D.valid || D.blend != draw->blend || memcmp(D.blend_factor, draw->blend_factor, sizeof(D.blend_factor)))
	{
		ID3D11DeviceContext_OMSetBlendState(B.context, blend, draw->blend_factor, 0xffffffffu);
		D.blend = draw->blend;
		memcpy(D.blend_factor, draw->blend_factor, sizeof(D.blend_factor));
	}
	else
		D.binds_skipped++;
	if (!D.valid || D.depth_stencil != draw->depth_stencil || D.stencil_reference != draw->stencil_reference)
	{
		ID3D11DeviceContext_OMSetDepthStencilState(B.context, depth_stencil, draw->stencil_reference & 0xff);
		D.depth_stencil = draw->depth_stencil;
		D.stencil_reference = draw->stencil_reference;
	}
	else
		D.binds_skipped++;
	if (!D.valid || D.rasterizer != draw->rasterizer)
	{
		ID3D11DeviceContext_RSSetState(B.context, rasterizer);
		D.rasterizer = draw->rasterizer;
	}
	else
		D.binds_skipped++;

	/* the viewport, and the scissor over the same rectangle */
	if (!D.viewport_valid || memcmp(D.viewport, draw->viewport, sizeof(D.viewport)) || D.min_depth != draw->min_depth ||
		D.max_depth != draw->max_depth)
	{
		D3D11_VIEWPORT viewport;
		D3D11_RECT scissor;

		viewport.TopLeftX = (float)draw->viewport[0];
		viewport.TopLeftY = (float)draw->viewport[1];
		viewport.Width = (float)draw->viewport[2];
		viewport.Height = (float)draw->viewport[3];
		viewport.MinDepth = draw->min_depth < 0.0f ? 0.0f : draw->min_depth > 1.0f ? 1.0f : draw->min_depth;
		viewport.MaxDepth = draw->max_depth < viewport.MinDepth ? viewport.MinDepth : draw->max_depth > 1.0f ? 1.0f :
			draw->max_depth;
		scissor.left = draw->viewport[0];
		scissor.top = draw->viewport[1];
		scissor.right = draw->viewport[0] + draw->viewport[2];
		scissor.bottom = draw->viewport[1] + draw->viewport[3];
		ID3D11DeviceContext_RSSetViewports(B.context, 1, &viewport);
		ID3D11DeviceContext_RSSetScissorRects(B.context, 1, &scissor);
		memcpy(D.viewport, draw->viewport, sizeof(D.viewport));
		D.min_depth = draw->min_depth;
		D.max_depth = draw->max_depth;
		D.viewport_valid = 1;
	}
	else
		D.binds_skipped++;

	/* the vertex buffers: the slots with data, and the attribute slot */
	for (slot = 0; slot < D3D11_BACKEND_STREAM_COUNT; slot++)
	{
		const struct d3d11_backend_draw_stream *stream = &draw->streams[slot];
		ID3D11Buffer *buffer = NULL;
		UINT offset = stream->offset, stride = stream->stride;

		if (stream->kind == D3D11_BACKEND_DATA_RING)
			buffer = d3d11_backend_ring_buffer(D3D11_BACKEND_RING_STREAM);
		else if (stream->kind == D3D11_BACKEND_DATA_SEGMENT)
			buffer = d3d11_backend_segment_buffer(stream->segment);
		if (!buffer)
			continue;
		if (D.slots[slot].buffer == buffer && D.slots[slot].offset == offset && D.slots[slot].stride == stride)
		{
			D.binds_skipped++;
			continue;
		}
		ID3D11DeviceContext_IASetVertexBuffers(B.context, slot, 1, &buffer, &stride, &offset);
		D.slots[slot].buffer = buffer;
		D.slots[slot].offset = offset;
		D.slots[slot].stride = stride;
	}
	if (!D.attributes_written || memcmp(D.attribute_bytes, draw->attributes, ATTRIBUTES_SIZE))
	{
		if (buffer_write(D.attributes, draw->attributes, ATTRIBUTES_SIZE, "Map (the attribute slot)"))
		{
			memcpy(D.attribute_bytes, draw->attributes, ATTRIBUTES_SIZE);
			D.attributes_written = 1;
		}
	}
	if (D.slots[D3D11_BACKEND_ATTRIBUTE_SLOT].buffer != D.attributes)
	{
		UINT offset = 0, stride = ATTRIBUTES_SIZE;

		ID3D11DeviceContext_IASetVertexBuffers(B.context, D3D11_BACKEND_ATTRIBUTE_SLOT, 1, &D.attributes, &stride, &offset);
		D.slots[D3D11_BACKEND_ATTRIBUTE_SLOT].buffer = D.attributes;
		D.slots[D3D11_BACKEND_ATTRIBUTE_SLOT].offset = 0;
		D.slots[D3D11_BACKEND_ATTRIBUTE_SLOT].stride = ATTRIBUTES_SIZE;
	}

	/* the constant buffers, the same four at every draw */
	if (!D.constants_bound)
	{
		ID3D11DeviceContext_VSSetConstantBuffers(B.context, 0, 2, &D.constants[D3D11_BACKEND_CONSTANTS_VERTEX]);
		ID3D11DeviceContext_PSSetConstantBuffers(B.context, 0, 2, &D.constants[D3D11_BACKEND_CONSTANTS_PIXEL_PARAMETERS]);
		D.constants_bound = 1;
	}

	/* the textures and the samplers of the stages that changed; a stage
	whose target is the one drawn into samples a copy */
	for (stage = 0; stage < D3D11_BACKEND_STAGE_COUNT; stage++)
	{
		const struct d3d11_backend_draw_texture *texture = &draw->textures[stage];
		ID3D11ShaderResourceView *view = NULL;
		ID3D11SamplerState *sampler;
		uint32_t width, height;

		if (texture->kind == D3D11_BACKEND_TEXTURE_CACHE)
			view = d3d11_backend_texture_view(texture->handle);
		else if (texture->kind == D3D11_BACKEND_TEXTURE_TARGET)
		{
			struct target *target = d3d11_backend_target_at(texture->handle);

			/* (a multisampled target is drawn into its multisampled
			texture, and its own was resolved by the device) */
			if (target && target == color && !target->samples)
				view = self_sampling_copy(target);
			else
				view = d3d11_backend_target_view(texture->handle, &width, &height);
		}
		else if (texture->kind == D3D11_BACKEND_TEXTURE_COMPOSITE)
			view = d3d11_backend_composite_view(texture->handle);
		if (texture->kind != D3D11_BACKEND_TEXTURE_NONE && !view && !D.missing_logged[2])
		{
			D.missing_logged[2] = 1;
			platform_log("d3d11: a draw samples a texture of kind %u, handle %u, that has no view: it reads zero (logged "
				"once)", texture->kind, texture->handle);
		}
		if (!D.valid || D.views[stage] != view)
		{
			ID3D11DeviceContext_PSSetShaderResources(B.context, stage, 1, &view);
			D.views[stage] = view;
		}
		else
			D.binds_skipped++;
		if (!D.valid || D.samplers[stage] != draw->samplers[stage])
		{
			sampler = draw->samplers[stage] ? d3d11_backend_sampler_object(draw->samplers[stage]) : NULL;
			ID3D11DeviceContext_PSSetSamplers(B.context, stage, 1, &sampler);
			D.samplers[stage] = draw->samplers[stage];
		}
		else
			D.binds_skipped++;
	}

	/* the topology, the indices, the draw */
	if (D.topology != (D3D11_PRIMITIVE_TOPOLOGY)draw->topology)
	{
		ID3D11DeviceContext_IASetPrimitiveTopology(B.context, (D3D11_PRIMITIVE_TOPOLOGY)draw->topology);
		D.topology = (D3D11_PRIMITIVE_TOPOLOGY)draw->topology;
	}
	D.valid = 1;
	if (draw->index_kind != D3D11_BACKEND_DATA_NONE)
	{
		ID3D11Buffer *buffer = draw->index_kind == D3D11_BACKEND_DATA_RING ? d3d11_backend_ring_buffer(D3D11_BACKEND_RING_INDEX) :
			d3d11_backend_segment_buffer(draw->index_segment);

		if (!buffer)
		{
			missing(3, "an index buffer (a segment)", draw->index_segment);
			return 0;
		}
		if (D.index_buffer != buffer)
		{
			ID3D11DeviceContext_IASetIndexBuffer(B.context, buffer, DXGI_FORMAT_R16_UINT, 0);
			D.index_buffer = buffer;
		}
		ID3D11DeviceContext_DrawIndexed(B.context, draw->index_count, draw->index_offset / 2, draw->base_vertex);
	}
	else
	{
		ID3D11DeviceContext_Draw(B.context, draw->vertex_count, 0);
	}
	D.draws++;
	return 1;
}

/* the visibility tests begun (below) */
static uint32_t queries_begun(void);

void d3d11_backend_draw_statistics(uint32_t counts[D3D11_BACKEND_DRAW_STATISTICS])
{
	memset(counts, 0, D3D11_BACKEND_DRAW_STATISTICS * sizeof(counts[0]));
	counts[D3D11_BACKEND_DRAW_STATISTIC_DRAWS] = D.draws;
	counts[D3D11_BACKEND_DRAW_STATISTIC_BINDS_SKIPPED] = D.binds_skipped;
	counts[D3D11_BACKEND_DRAW_STATISTIC_SELF_SAMPLING_COPIES] = D.self_sampling_copies;
	counts[D3D11_BACKEND_DRAW_STATISTIC_RESOLVES] = d3d11_backend_target_resolves();
	counts[D3D11_BACKEND_DRAW_STATISTIC_QUERIES] = queries_begun();
	d3d11_backend_textures_statistics(counts);
}

/* ---------- visibility tests (step 6)

D3D11_QUERY_OCCLUSION (a 64-bit count of the samples that passed, exact at
feature level 11_0) in the game's slots, as d3d8_gl.c's desktop path keeps
its GL queries: a test begins on a scratch query, and at its end the
scratch is put in the slot. A query spans target changes (Direct3D 11 has no
render pass). Results are asked for with D3D11_ASYNC_GETDATA_DONOTFLUSH and
never waited for: the device keeps the latest count known.

The slot's earlier query is not begun again at once, as GL's is: the game
reads only a slot's latest test, so the earlier one's count may never have
been read, and a Begin on a query whose results were not obtained abandons
them (the debug layer's warning 408). It is retired instead, and its count
read (and dropped) at later begins until it has one; then it is free for
another test. The queries are made as the free ones run out, and kept. */

#define QUERY_SLOTS 4096
#define QUERY_RETIRED 256

static struct
{
	ID3D11Query *slots[QUERY_SLOTS];
	/* the scratch, and whether a test is open on it */
	ID3D11Query *scratch;
	int open;
	/* queries replaced in their slot whose count is not read yet, and those
	read, free for a test */
	ID3D11Query *retired[QUERY_RETIRED];
	uint32_t retired_count;
	ID3D11Query *free[QUERY_RETIRED];
	uint32_t free_count;
	uint32_t begun;
	int failed_logged;
} Q;

static ID3D11Query *query_make(void)
{
	D3D11_QUERY_DESC description;
	ID3D11Query *query = NULL;
	HRESULT result;

	description.Query = D3D11_QUERY_OCCLUSION;
	description.MiscFlags = 0;
	result = ID3D11Device_CreateQuery(B.device, &description, &query);
	if (backend_failed(result, "an occlusion query"))
	{
		if (!Q.failed_logged)
		{
			Q.failed_logged = 1;
			platform_log("d3d11: an occlusion query was not made (%s): its tests answer 0 (logged once)",
				hr_text(result));
		}
		return NULL;
	}
	return query;
}

/* the retired queries whose counts the GPU has given: read, and freed */
static void queries_retire(void)
{
	uint32_t index = 0;

	while (index < Q.retired_count)
	{
		UINT64 count;
		HRESULT result = ID3D11DeviceContext_GetData(B.context, (ID3D11Asynchronous *)Q.retired[index], &count,
			sizeof(count), D3D11_ASYNC_GETDATA_DONOTFLUSH);

		if (result == S_FALSE)
		{
			index++;
			continue;
		}
		if (FAILED(result))
			backend_failed(result, "GetData (a retired occlusion query)");
		if (Q.free_count < QUERY_RETIRED)
			Q.free[Q.free_count++] = Q.retired[index];
		else
			release(&Q.retired[index]);
		Q.retired[index] = Q.retired[--Q.retired_count];
	}
}

void d3d11_backend_query_begin(uint32_t slot)
{
	(void)slot;
	if (!B.device || B.lost || Q.open)
		return;
	queries_retire();
	if (!Q.scratch)
		Q.scratch = Q.free_count ? Q.free[--Q.free_count] : query_make();
	if (!Q.scratch)
		return;
	ID3D11DeviceContext_Begin(B.context, (ID3D11Asynchronous *)Q.scratch);
	Q.open = 1;
	Q.begun++;
}

void d3d11_backend_query_end(uint32_t slot)
{
	ID3D11Query *ended;

	if (!B.device || B.lost || !Q.open)
		return;
	Q.open = 0;
	ID3D11DeviceContext_End(B.context, (ID3D11Asynchronous *)Q.scratch);
	if (slot >= QUERY_SLOTS)
		return;
	ended = Q.scratch;
	Q.scratch = NULL;
	/* (the slot's earlier query retired; with too many retired, the oldest
	is let go of) */
	if (Q.slots[slot])
	{
		if (Q.retired_count == QUERY_RETIRED)
		{
			release(&Q.retired[0]);
			Q.retired[0] = Q.retired[--Q.retired_count];
		}
		Q.retired[Q.retired_count++] = Q.slots[slot];
	}
	Q.slots[slot] = ended;
}

int d3d11_backend_query_result(uint32_t slot, uint64_t *samples)
{
	UINT64 count = 0;
	HRESULT result;

	if (!B.device || B.lost || slot >= QUERY_SLOTS || !Q.slots[slot])
		return 0;
	result = ID3D11DeviceContext_GetData(B.context, (ID3D11Asynchronous *)Q.slots[slot], &count, sizeof(count),
		D3D11_ASYNC_GETDATA_DONOTFLUSH);
	/* (S_FALSE: not known yet) */
	if (result != S_OK)
	{
		if (FAILED(result))
			backend_failed(result, "GetData (an occlusion query)");
		return 0;
	}
	*samples = count;
	return 1;
}

static uint32_t queries_begun(void)
{
	return Q.begun;
}

void d3d11_backend_draw_release(void)
{
	uint32_t index;

	for (index = 0; index < QUERY_SLOTS; index++)
		release(&Q.slots[index]);
	for (index = 0; index < Q.retired_count; index++)
		release(&Q.retired[index]);
	for (index = 0; index < Q.free_count; index++)
		release(&Q.free[index]);
	release(&Q.scratch);
	memset(&Q, 0, sizeof(Q));

	for (index = 0; index < SCRATCH_COUNT; index++)
	{
		release(&D.scratch[index].view);
		release(&D.scratch[index].texture);
	}
	for (index = 0; index < 4; index++)
		release(&D.constants[index]);
	release(&D.attributes);
	memset(&D, 0, sizeof(D));
}
