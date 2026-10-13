/*
WIN32_D3D11_MEMORY.C

The Direct3D 11 backend's buffers for the game's memory (port/windows/D3D11.md,
phase 3): the stream ring and the index ring, D3D11_USAGE_DYNAMIC buffers the
device writes each draw's streamed vertices and indices into; the mirror's
segments, DEFAULT buffers holding the contiguous window's pages; and the
upload ring the segments are written through, a DYNAMIC buffer from which
each run of pages is copied into its segment by CopySubresourceRegion. The
device (d3d8_d3d11.c) keeps the bookkeeping, d3d8_gl.c's: which pages are
current, which ring offsets are free. This unit only makes the buffers and
writes bytes into them, in the call: nothing of the game's memory is read
later. The backend's second split, sharing win32_d3d11.h with win32_d3d11.c
and win32_d3d11_present.c.

Every write is a Map with D3D11_MAP_WRITE_NO_OVERWRITE, or DISCARD once after
an orphan (a ring the device found full) or when the upload ring wraps: no
Map here waits for the GPU. The only wait is the self-test's read-back.

ring_write and the self-test's cases are copied from phase 0's probe
(win32_d3d11_probe.c: memory_ring's no-overwrite and discard maps,
memory_rewrite's red and green writes and draws, colour_hlsl, struct vertex
and pipeline_make's input layout, buffer_make), where they were tried on this
machine's Adreno and on WARP; the probe's staging buffers became the upload
ring, a DYNAMIC buffer (the plan's "Decisions", "The game's memory").
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32_d3d11.h"

/* the upload ring's size: the device writes at most 256 pages (1 MB) at a
time (mirror_refresh) */
#define UPLOAD_RING_SIZE (8u * 1024u * 1024u)

struct ring
{
	ID3D11Buffer *buffer;
	uint32_t size;
	/* the next map discards: the ring's first, and the first after an
	orphan */
	int discard_next;
	/* a range refused, and a map that failed, each logged once */
	int refused_logged, failed_logged;
	const char *name;
};

static struct
{
	/* made by d3d11_backend_memory_initialize; without them nothing works */
	int ready;
	struct ring rings[2];
	struct ring upload;
	/* where the next write to the upload ring goes */
	uint32_t upload_offset;
	/* the segments, by handle - 1, and their sizes */
	ID3D11Buffer **segments;
	uint32_t *segment_sizes;
	uint32_t segment_count, segment_capacity;
	/* the two rings' DISCARD maps since the start */
	uint32_t discards;
	/* a segment write refused, or whose copy could not be made, logged once */
	int segment_refused_logged;
} M;

/* as the probe's buffer_make */
static HRESULT buffer_make(ID3D11Buffer **buffer, UINT size, D3D11_USAGE usage, UINT bind, UINT cpu)
{
	D3D11_BUFFER_DESC description;

	memset(&description, 0, sizeof(description));
	description.ByteWidth = size;
	description.Usage = usage;
	description.BindFlags = bind;
	description.CPUAccessFlags = cpu;
	return ID3D11Device_CreateBuffer(B.device, &description, NULL, buffer);
}

/* the free virtual address space in MB, with a thousands separator (the
process is 32-bit, and the DYNAMIC buffers are mapped into it) */
static void address_space_text(char *text, size_t size)
{
	MEMORYSTATUSEX status;
	unsigned long megabytes;

	status.dwLength = sizeof(status);
	if (!GlobalMemoryStatusEx(&status))
	{
		snprintf(text, size, "not known");
		return;
	}
	megabytes = (unsigned long)(status.ullAvailVirtual / (1024 * 1024));
	if (megabytes >= 1000)
		snprintf(text, size, "%lu,%03lu MB", megabytes / 1000, megabytes % 1000);
	else
		snprintf(text, size, "%lu MB", megabytes);
}

int d3d11_backend_memory_initialize(uint32_t stream_size, uint32_t index_size, char *line, size_t line_size)
{
	static const char *const names[2] = { "the stream ring", "the index ring" };
	static const UINT binds[2] = { D3D11_BIND_VERTEX_BUFFER, D3D11_BIND_INDEX_BUFFER };
	const uint32_t sizes[2] = { stream_size, index_size };
	char space[64], before[64];
	HRESULT result = S_OK;
	const char *which = "";
	int index;

	if (!B.device || B.lost)
	{
		snprintf(line, line_size, "d3d11: no buffers (no device)");
		return 0;
	}
	if (M.ready)
	{
		snprintf(line, line_size, "d3d11: buffers made before");
		return 1;
	}
	/* (the free space before them too: what they cost) */
	address_space_text(before, sizeof(before));
	for (index = 0; index < 2 && SUCCEEDED(result); index++)
	{
		M.rings[index].size = sizes[index];
		M.rings[index].discard_next = 1;
		M.rings[index].name = names[index];
		which = names[index];
		result = buffer_make(&M.rings[index].buffer, sizes[index], D3D11_USAGE_DYNAMIC, binds[index],
			D3D11_CPU_ACCESS_WRITE);
	}
	/* (a DYNAMIC buffer needs a bind flag: the upload ring's is never used) */
	if (SUCCEEDED(result))
	{
		M.upload.size = UPLOAD_RING_SIZE;
		M.upload.discard_next = 1;
		M.upload.name = "the upload ring";
		which = M.upload.name;
		result = buffer_make(&M.upload.buffer, UPLOAD_RING_SIZE, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER,
			D3D11_CPU_ACCESS_WRITE);
	}
	if (FAILED(result))
	{
		backend_failed(result, which);
		snprintf(line, line_size, "d3d11: no buffers (%s: %s)", which, hr_text(result));
		d3d11_backend_memory_release();
		return 0;
	}
	M.ready = 1;
	address_space_text(space, sizeof(space));
	snprintf(line, line_size, "d3d11: buffers: stream ring %u MB, index ring %u MB, upload ring %u MB; address space "
		"%s free (%s before them)", stream_size / (1024 * 1024), index_size / (1024 * 1024),
		UPLOAD_RING_SIZE / (1024 * 1024), space, before);
	return 1;
}

void d3d11_backend_memory_release(void)
{
	uint32_t index;

	release(&M.rings[0].buffer);
	release(&M.rings[1].buffer);
	release(&M.upload.buffer);
	for (index = 0; index < M.segment_count; index++)
		release(&M.segments[index]);
	free(M.segments);
	free(M.segment_sizes);
	memset(&M, 0, sizeof(M));
}

/* ---------- the rings */

/* size bytes of data at offset in the ring, by a no-overwrite map, or a
discard one when the ring asks for it (the probe's memory_ring) */
static int ring_map_write(struct ring *ring, uint32_t offset, const void *data, uint32_t size)
{
	D3D11_MAPPED_SUBRESOURCE mapped;
	D3D11_MAP mode = ring->discard_next ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
	HRESULT result;

	if (offset > ring->size || size > ring->size - offset)
	{
		if (!ring->refused_logged)
		{
			ring->refused_logged = 1;
			platform_log("d3d11: a write of %u bytes at %u is past the end of %s (%u bytes), and is refused (logged "
				"once)", size, offset, ring->name, ring->size);
		}
		return 0;
	}
	result = ID3D11DeviceContext_Map(B.context, (ID3D11Resource *)ring->buffer, 0, mode, 0, &mapped);
	if (backend_failed(result, "Map (a ring)"))
	{
		if (!B.lost && !ring->failed_logged)
		{
			ring->failed_logged = 1;
			platform_log("d3d11: %s's Map failed (%s; logged once)", ring->name, hr_text(result));
		}
		return 0;
	}
	memcpy((char *)mapped.pData + offset, data, size);
	ID3D11DeviceContext_Unmap(B.context, (ID3D11Resource *)ring->buffer, 0);
	if (ring->discard_next)
	{
		ring->discard_next = 0;
		if (ring != &M.upload)
			M.discards++;
	}
	return 1;
}

void d3d11_backend_ring_orphan(uint32_t ring)
{
	if (ring < 2)
		M.rings[ring].discard_next = 1;
}

int d3d11_backend_ring_write(uint32_t ring, uint32_t offset, const void *data, uint32_t size)
{
	if (!M.ready || B.lost || ring >= 2)
		return 0;
	return ring_map_write(&M.rings[ring], offset, data, size);
}

uint32_t d3d11_backend_ring_discards(void)
{
	return M.discards;
}

/* ---------- the segments */

static ID3D11Buffer *segment_make(uint32_t size)
{
	ID3D11Buffer *buffer = NULL;
	HRESULT result = buffer_make(&buffer, size, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER,
		0);

	if (backend_failed(result, "a segment"))
	{
		platform_log("d3d11: a segment of %u bytes was not made (%s)", size, hr_text(result));
		return NULL;
	}
	return buffer;
}

uint32_t d3d11_backend_segment_create(uint32_t size)
{
	ID3D11Buffer *buffer;

	if (!M.ready || B.lost || !size)
		return 0;
	if (M.segment_count == M.segment_capacity)
	{
		uint32_t capacity = M.segment_capacity ? M.segment_capacity * 2 : 32;
		ID3D11Buffer **segments = realloc(M.segments, capacity * sizeof(*segments));
		uint32_t *sizes;

		if (!segments)
			return 0;
		M.segments = segments;
		sizes = realloc(M.segment_sizes, capacity * sizeof(*sizes));
		if (!sizes)
			return 0;
		M.segment_sizes = sizes;
		M.segment_capacity = capacity;
	}
	buffer = segment_make(size);
	if (!buffer)
		return 0;
	M.segments[M.segment_count] = buffer;
	M.segment_sizes[M.segment_count] = size;
	return ++M.segment_count;
}

uint32_t d3d11_backend_segments_alive(void)
{
	return M.segment_count;
}

/* size bytes of data into buffer at offset: through the upload ring, at its
running offset (16-aligned; a discard where the bytes do not fit from there
on), and a copy of that range into the buffer queued in the context */
static int buffer_write(ID3D11Buffer *buffer, uint32_t offset, const void *data, uint32_t size)
{
	uint32_t at = (M.upload_offset + 15u) & ~15u;
	D3D11_BOX box;

	if (size > M.upload.size)
	{
		if (!M.segment_refused_logged)
		{
			M.segment_refused_logged = 1;
			platform_log("d3d11: a segment write of %u bytes is larger than the upload ring (%u bytes), and is "
				"refused (logged once)", size, M.upload.size);
		}
		return 0;
	}
	if (!size)
		return 1;
	if (at > M.upload.size || size > M.upload.size - at)
	{
		M.upload.discard_next = 1;
		at = 0;
	}
	if (!ring_map_write(&M.upload, at, data, size))
		return 0;
	M.upload_offset = at + size;
	box.left = at;
	box.top = 0;
	box.front = 0;
	box.right = at + size;
	box.bottom = 1;
	box.back = 1;
	ID3D11DeviceContext_CopySubresourceRegion(B.context, (ID3D11Resource *)buffer, 0, offset, 0, 0,
		(ID3D11Resource *)M.upload.buffer, 0, &box);
	return 1;
}

int d3d11_backend_segment_write(uint32_t segment, uint32_t offset, const void *data, uint32_t size)
{
	if (!M.ready || B.lost)
		return 0;
	if (!segment || segment > M.segment_count || offset > M.segment_sizes[segment - 1] ||
		size > M.segment_sizes[segment - 1] - offset)
	{
		if (!M.segment_refused_logged)
		{
			M.segment_refused_logged = 1;
			platform_log("d3d11: a write of %u bytes at %u into segment %u is outside it, and is refused (logged "
				"once)", size, offset, segment);
		}
		return 0;
	}
	return buffer_write(M.segments[segment - 1], offset, data, size);
}

/* ---------- the memory self-test

The Vulkan renderer's data self-test (port/android/VULKAN.md, phase 3) as
the probe's memory_rewrite ran it, on the backend's own rings and upload
ring: bytes in ordinary memory, "the game's memory", written, drawn into a
target, rewritten in place with another colour, written again through the
same path and drawn into a second target; both targets read back and the
texel at (8, 8) compared. The draws are queued between the writes, so each
must see the bytes as they were at its draw. */

#define TEST_SIZE 16
/* R8G8B8A8_UNORM, red in the low byte, and as the B8G8R8A8 target reads
back */
#define TEST_RED 0xff0000ffu
#define TEST_GREEN 0xff00ff00u

static const char colour_hlsl[] =
	"struct vs_in { float2 position : POSITION; float4 colour : COLOR; };\n"
	"struct vs_out { float4 position : SV_Position; float4 colour : COLOR; };\n"
	"vs_out vs_main(vs_in input)\n"
	"{\n"
	"	vs_out output;\n"
	"	output.position = float4(input.position, 0.0, 1.0);\n"
	"	output.colour = input.colour;\n"
	"	return output;\n"
	"}\n"
	"float4 ps_main(vs_out input) : SV_Target { return input.colour; }\n";

/* 16 bytes a vertex */
struct test_vertex
{
	float x, y;
	uint32_t colour;
	uint32_t pad;
};

struct test_pipeline
{
	ID3D11VertexShader *vs;
	ID3D11PixelShader *ps;
	ID3D11InputLayout *layout;
};

/* the probe's pipeline_make */
static int test_pipeline_make(struct test_pipeline *pipeline)
{
	static const D3D11_INPUT_ELEMENT_DESC elements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	ID3DBlob *vs = NULL, *ps = NULL;
	HRESULT result = E_FAIL;

	memset(pipeline, 0, sizeof(*pipeline));
	if (d3d11_backend_compile(colour_hlsl, sizeof(colour_hlsl) - 1, "memory self-test's", "vs_main", "vs_4_0", &vs) &&
		d3d11_backend_compile(colour_hlsl, sizeof(colour_hlsl) - 1, "memory self-test's", "ps_main", "ps_4_0", &ps))
	{
		result = ID3D11Device_CreateVertexShader(B.device, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs),
			NULL, &pipeline->vs);
		if (SUCCEEDED(result))
			result = ID3D11Device_CreatePixelShader(B.device, ID3D10Blob_GetBufferPointer(ps),
				ID3D10Blob_GetBufferSize(ps), NULL, &pipeline->ps);
		if (SUCCEEDED(result))
			result = ID3D11Device_CreateInputLayout(B.device, elements, 2, ID3D10Blob_GetBufferPointer(vs),
				ID3D10Blob_GetBufferSize(vs), &pipeline->layout);
		backend_failed(result, "the memory self-test's pipeline");
	}
	release(&vs);
	release(&ps);
	return SUCCEEDED(result);
}

static void test_pipeline_free(struct test_pipeline *pipeline)
{
	release(&pipeline->layout);
	release(&pipeline->ps);
	release(&pipeline->vs);
}

/* three vertices covering the target (the probe's fill_vertices), and three
more after them when count is 6 */
static void test_fill(struct test_vertex *vertices, int count, uint32_t colour)
{
	static const float corners[3][2] = { { -1.0f, -1.0f }, { -1.0f, 3.0f }, { 3.0f, -1.0f } };
	int index;

	for (index = 0; index < count; index++)
	{
		vertices[index].x = corners[index % 3][0];
		vertices[index].y = corners[index % 3][1];
		vertices[index].colour = colour;
		vertices[index].pad = 0;
	}
}

/* the target bound, cleared black and drawn into: three vertices from
vertices at vertex_offset, or (indices set) three indices from it at
index_offset */
static void test_draw(struct target *target, ID3D11Buffer *vertices, uint32_t vertex_offset, ID3D11Buffer *indices,
	uint32_t index_offset)
{
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (float)TEST_SIZE, (float)TEST_SIZE, 0.0f, 1.0f };
	D3D11_RECT scissor = { 0, 0, TEST_SIZE, TEST_SIZE };
	UINT stride = sizeof(struct test_vertex), offset = vertex_offset;

	ID3D11DeviceContext_OMSetRenderTargets(B.context, 1, &target->color, NULL);
	ID3D11DeviceContext_RSSetViewports(B.context, 1, &viewport);
	ID3D11DeviceContext_RSSetScissorRects(B.context, 1, &scissor);
	ID3D11DeviceContext_ClearRenderTargetView(B.context, target->color, black);
	ID3D11DeviceContext_IASetVertexBuffers(B.context, 0, 1, &vertices, &stride, &offset);
	if (indices)
	{
		ID3D11DeviceContext_IASetIndexBuffer(B.context, indices, DXGI_FORMAT_R16_UINT, index_offset);
		ID3D11DeviceContext_DrawIndexed(B.context, 3, 0, 0);
	}
	else
	{
		ID3D11DeviceContext_Draw(B.context, 3, 0);
	}
}

/* the texel at (8, 8), B G R A, of a target; 0 if it was not read */
static int test_texel(struct target *target, unsigned char texel[4])
{
	unsigned char pixels[TEST_SIZE * TEST_SIZE * 4];

	if (!read_target(target, pixels, sizeof(pixels)))
		return 0;
	memcpy(texel, pixels + (8 * TEST_SIZE + 8) * 4, 4);
	return 1;
}

int d3d11_backend_memory_self_test(char *line, size_t line_size)
{
	static const char *const draw_names[2] = { "first", "second" };
	static const unsigned char wanted[2][4] = { { 0, 0, 255, 255 }, { 0, 255, 0, 255 } };
	struct test_pipeline pipeline;
	struct target targets[2];
	ID3D11Buffer *segment = NULL;
	ID3D11Buffer *none_buffer = NULL;
	UINT zero = 0;
	/* "the game's memory": the same bytes, rewritten in place */
	struct test_vertex vertices[6];
	uint16_t indices[8];
	uint32_t discards = M.discards;
	int test_case, pass, draws = 0, ok = 1;

	if (!B.device || B.lost)
	{
		snprintf(line, line_size, "d3d11: memory self-test: not run (no device)");
		return 0;
	}
	if (!M.ready)
	{
		snprintf(line, line_size, "d3d11: memory self-test: not run (no buffers)");
		return 0;
	}
	if (!test_pipeline_make(&pipeline))
	{
		test_pipeline_free(&pipeline);
		snprintf(line, line_size, "d3d11: memory self-test: FAILED (its pipeline was not made)");
		return 0;
	}
	if (!target_make(&targets[0], TEST_SIZE, TEST_SIZE, 0))
	{
		test_pipeline_free(&pipeline);
		snprintf(line, line_size, "d3d11: memory self-test: FAILED (its targets were not made)");
		return 0;
	}
	if (!target_make(&targets[1], TEST_SIZE, TEST_SIZE, 0))
	{
		target_free(&targets[0]);
		test_pipeline_free(&pipeline);
		snprintf(line, line_size, "d3d11: memory self-test: FAILED (its targets were not made)");
		return 0;
	}
	/* a segment of the test's own, made and written as the mirror's are */
	segment = segment_make(64 * 1024);
	if (!segment)
	{
		target_free(&targets[1]);
		target_free(&targets[0]);
		test_pipeline_free(&pipeline);
		snprintf(line, line_size, "d3d11: memory self-test: FAILED (its segment was not made)");
		return 0;
	}
	ID3D11DeviceContext_IASetInputLayout(B.context, pipeline.layout);
	ID3D11DeviceContext_IASetPrimitiveTopology(B.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_VSSetShader(B.context, pipeline.vs, NULL, 0);
	ID3D11DeviceContext_PSSetShader(B.context, pipeline.ps, NULL, 0);
	ID3D11DeviceContext_OMSetBlendState(B.context, d3d11_backend_write_all, NULL, 0xffffffff);
	ID3D11DeviceContext_OMSetDepthStencilState(B.context, d3d11_backend_depth_off, 0);
	ID3D11DeviceContext_RSSetState(B.context, d3d11_backend_scissored);

	for (test_case = 1; ok && test_case <= 4; test_case++)
	{
		unsigned char seen[2][4];
		int written = 1;

		for (pass = 0; written && pass < 2; pass++)
		{
			uint32_t colour = pass ? TEST_GREEN : TEST_RED;

			switch (test_case)
			{
			case 1:
				/* the stream ring, appended: red at 0 with a discard, green
				after it with no-overwrite */
				test_fill(vertices, 3, colour);
				if (!pass)
					d3d11_backend_ring_orphan(D3D11_BACKEND_RING_STREAM);
				written = d3d11_backend_ring_write(D3D11_BACKEND_RING_STREAM, pass ? 48 : 0, vertices, 48);
				if (written)
					test_draw(&targets[pass], M.rings[0].buffer, pass ? 48 : 0, NULL, 0);
				break;
			case 2:
				/* the stream ring across an orphan: red after case 1's
				bytes, then an orphan and green at 0 again, as the device
				writes after a wrap */
				test_fill(vertices, 3, colour);
				if (pass)
					d3d11_backend_ring_orphan(D3D11_BACKEND_RING_STREAM);
				written = d3d11_backend_ring_write(D3D11_BACKEND_RING_STREAM, pass ? 0 : 96, vertices, 48);
				if (written)
					test_draw(&targets[pass], M.rings[0].buffer, pass ? 0 : 96, NULL, 0);
				break;
			case 3:
				/* a segment through the upload ring: red, then green at the
				same offset, both copies queued before anything is read */
				test_fill(vertices, 3, colour);
				written = buffer_write(segment, 256, vertices, 48);
				if (written)
					test_draw(&targets[pass], segment, 256, NULL, 0);
				break;
			default:
				/* the index ring: the vertices in the segment, red then
				green after them; three indices naming the red ones, then
				three after them in the ring naming the green ones */
				if (!pass)
				{
					test_fill(vertices, 3, TEST_RED);
					test_fill(vertices + 3, 3, TEST_GREEN);
					written = buffer_write(segment, 4096, vertices, sizeof(vertices));
				}
				memset(indices, 0, sizeof(indices));
				indices[0] = (uint16_t)(pass * 3);
				indices[1] = (uint16_t)(pass * 3 + 1);
				indices[2] = (uint16_t)(pass * 3 + 2);
				if (written)
					written = d3d11_backend_ring_write(D3D11_BACKEND_RING_INDEX, pass ? 16 : 0, indices, 16);
				if (written)
					test_draw(&targets[pass], segment, 4096, M.rings[1].buffer, pass ? 16 : 0);
				break;
			}
			draws += written;
		}
		if (!written)
		{
			snprintf(line, line_size, "d3d11: memory self-test: FAILED (case %d: a write failed)", test_case);
			ok = 0;
			break;
		}
		if (!test_texel(&targets[0], seen[0]) || !test_texel(&targets[1], seen[1]))
		{
			snprintf(line, line_size, "d3d11: memory self-test: FAILED (case %d: a target was not read back)",
				test_case);
			ok = 0;
			break;
		}
		for (pass = 0; pass < 2; pass++)
		{
			if (memcmp(seen[pass], wanted[pass], 4))
			{
				snprintf(line, line_size, "d3d11: memory self-test: FAILED (case %d: %s draw B G R A %u %u %u %u, "
					"wanted %u %u %u %u)", test_case, draw_names[pass], seen[pass][0], seen[pass][1], seen[pass][2],
					seen[pass][3], wanted[pass][0], wanted[pass][1], wanted[pass][2], wanted[pass][3]);
				ok = 0;
				break;
			}
		}
	}
	if (ok)
		snprintf(line, line_size, "d3d11: memory self-test: ok (the stream ring appended and across a discard, a "
			"segment through the upload ring rewritten in place, the index ring: %d draws, each the colour its bytes "
			"had at the draw)", draws);

	/* nothing of the test's left bound; its objects released. The rings and
	the upload ring stay as the test left them: the device starts each ring
	with an orphan (backend_initialize), and the upload ring's offset is this
	unit's own. The test's discards are not the game's. */
	ID3D11DeviceContext_IASetVertexBuffers(B.context, 0, 1, &none_buffer, &zero, &zero);
	ID3D11DeviceContext_IASetIndexBuffer(B.context, NULL, DXGI_FORMAT_R16_UINT, 0);
	ID3D11DeviceContext_IASetInputLayout(B.context, NULL);
	ID3D11DeviceContext_VSSetShader(B.context, NULL, NULL, 0);
	ID3D11DeviceContext_PSSetShader(B.context, NULL, NULL, 0);
	ID3D11DeviceContext_OMSetRenderTargets(B.context, 0, NULL, NULL);
	d3d11_backend_targets_forget();
	M.discards = discards;
	release(&segment);
	target_free(&targets[1]);
	target_free(&targets[0]);
	test_pipeline_free(&pipeline);
	return ok;
}
