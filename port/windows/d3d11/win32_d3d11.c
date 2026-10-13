/*
WIN32_D3D11.C

The Direct3D 11 backend of the Windows build's Direct3D renderer
(port/windows/D3D11.md): the half that talks to Direct3D 11 and DXGI, called
by the Direct3D 8 device (d3d8_d3d11.c) through d3d11_backend.h. A
Windows-facing unit, compiled against the Windows SDK and not the Xbox SDK's
headers, as port/windows/src/win32_*.c are.

Phase 1: the factory, the adapter display.d3d11_adapter names, the device at
feature level 11_0 or above, the debug layer when debug.d3d11_debug asks for
it, and the one line that says what was made.

Phase 2: the game's render targets as textures, the clears (by clear calls
where they can be, drawn where they cannot), the built-in shaders and the
compiler's loading, the read-back for screenshots, the clears' self-test, and
the lost device. The swap chain and the present are win32_d3d11_present.c's;
what the two share is in win32_d3d11.h.

hr_text, drain_debug (as backend_drain_debug), choose_adapter and
create_device are copied from phase 0's probe (win32_d3d11_probe.c), where they
were tried on this machine's Adreno and on WARP, and changed only for what the
backend keeps and reports: a name that matches no adapter takes the first
hardware adapter (the game has a window to keep), and below feature level 11_0
there is no device. load_compiler is the probe's as well, without its report.
*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32_d3d11.h"

/* the debug layer's queue keeps this many messages between drains (the
probe's figure); a frame that makes more says how many were lost */
#define DEBUG_MESSAGE_LIMIT 65536

struct d3d11_backend_state d3d11_backend;

/* an HRESULT as hex and, for the common ones, its name (four buffers, so one line can show several) */
const char *d3d11_backend_hr_text(HRESULT result)
{
	static const struct { HRESULT value; const char *name; } names[] = {
		{ S_OK, "S_OK" },
		{ S_FALSE, "S_FALSE" },
		{ E_FAIL, "E_FAIL" },
		{ E_INVALIDARG, "E_INVALIDARG" },
		{ E_OUTOFMEMORY, "E_OUTOFMEMORY" },
		{ E_NOINTERFACE, "E_NOINTERFACE" },
		{ E_NOTIMPL, "E_NOTIMPL" },
		{ DXGI_ERROR_DEVICE_REMOVED, "DXGI_ERROR_DEVICE_REMOVED" },
		{ DXGI_ERROR_DEVICE_HUNG, "DXGI_ERROR_DEVICE_HUNG" },
		{ DXGI_ERROR_DEVICE_RESET, "DXGI_ERROR_DEVICE_RESET" },
		{ DXGI_ERROR_DRIVER_INTERNAL_ERROR, "DXGI_ERROR_DRIVER_INTERNAL_ERROR" },
		{ DXGI_ERROR_INVALID_CALL, "DXGI_ERROR_INVALID_CALL" },
		{ DXGI_ERROR_NOT_FOUND, "DXGI_ERROR_NOT_FOUND" },
		{ DXGI_ERROR_UNSUPPORTED, "DXGI_ERROR_UNSUPPORTED" },
		{ DXGI_ERROR_SDK_COMPONENT_MISSING, "DXGI_ERROR_SDK_COMPONENT_MISSING" },
		{ DXGI_ERROR_WAS_STILL_DRAWING, "DXGI_ERROR_WAS_STILL_DRAWING" },
		{ DXGI_ERROR_NOT_CURRENTLY_AVAILABLE, "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE" },
		{ DXGI_ERROR_ACCESS_DENIED, "DXGI_ERROR_ACCESS_DENIED" },
		{ DXGI_STATUS_OCCLUDED, "DXGI_STATUS_OCCLUDED" },
		{ DXGI_STATUS_MODE_CHANGED, "DXGI_STATUS_MODE_CHANGED" },
		{ DXGI_STATUS_MODE_CHANGE_IN_PROGRESS, "DXGI_STATUS_MODE_CHANGE_IN_PROGRESS" },
	};
	static char buffers[4][96];
	static int next;
	char *text = buffers[next++ & 3];
	size_t index;

	for (index = 0; index < sizeof(names) / sizeof(names[0]); index++)
	{
		if (names[index].value == result)
		{
			snprintf(text, sizeof(buffers[0]), "0x%08lx %s", (unsigned long)result, names[index].name);
			return text;
		}
	}
	snprintf(text, sizeof(buffers[0]), "0x%08lx", (unsigned long)result);
	return text;
}

void d3d11_backend_release(void *object)
{
	IUnknown **pointer = object;

	if (*pointer)
	{
		IUnknown_Release(*pointer);
		*pointer = NULL;
	}
}

/* ---------- the debug layer's messages */

static const char *severity_name(D3D11_MESSAGE_SEVERITY severity)
{
	switch (severity)
	{
	case D3D11_MESSAGE_SEVERITY_CORRUPTION: return "corruption";
	case D3D11_MESSAGE_SEVERITY_ERROR: return "error";
	case D3D11_MESSAGE_SEVERITY_WARNING: return "warning";
	case D3D11_MESSAGE_SEVERITY_INFO: return "info";
	case D3D11_MESSAGE_SEVERITY_MESSAGE: return "message";
	default: return "?";
	}
}

/* every message the layer stored since the last call, logged; returns how many */
static uint32_t backend_drain_debug(void)
{
	UINT64 count, index, discarded;
	uint32_t logged = 0;

	if (!B.info)
		return 0;
	count = ID3D11InfoQueue_GetNumStoredMessages(B.info);
	for (index = 0; index < count; index++)
	{
		SIZE_T length = 0;
		D3D11_MESSAGE *message;

		if (FAILED(ID3D11InfoQueue_GetMessage(B.info, index, NULL, &length)) || !length)
			continue;
		message = malloc(length);
		if (message && SUCCEEDED(ID3D11InfoQueue_GetMessage(B.info, index, message, &length)))
		{
			platform_log("d3d11 debug: %s %d %.*s", severity_name(message->Severity), (int)message->ID,
				(int)(message->DescriptionByteLength ? message->DescriptionByteLength - 1 : 0),
				message->pDescription ? message->pDescription : "");
			logged++;
		}
		free(message);
	}
	discarded = ID3D11InfoQueue_GetNumMessagesDiscardedByMessageCountLimit(B.info);
	if (discarded)
		platform_log("d3d11 debug: %llu more messages were lost (over the queue's limit of %d)",
			(unsigned long long)discarded, DEBUG_MESSAGE_LIMIT);
	ID3D11InfoQueue_ClearStoredMessages(B.info);
	return logged;
}

uint32_t d3d11_backend_drain_debug(void)
{
	return backend_drain_debug();
}

/* ---------- the adapter and the device */

static void adapter_text(IDXGIAdapter1 *adapter, char *text, size_t size)
{
	DXGI_ADAPTER_DESC1 description;

	text[0] = 0;
	if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &description)))
		WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, text, (int)size, NULL, NULL);
}

static int name_contains(const char *name, const char *part)
{
	char a[256], b[256];
	size_t index;

	snprintf(a, sizeof(a), "%s", name);
	snprintf(b, sizeof(b), "%s", part);
	for (index = 0; a[index]; index++)
		a[index] = (char)tolower((unsigned char)a[index]);
	for (index = 0; b[index]; index++)
		b[index] = (char)tolower((unsigned char)b[index]);
	return strstr(a, b) != NULL;
}

static const char *level_name(D3D_FEATURE_LEVEL level)
{
	switch (level)
	{
	case D3D_FEATURE_LEVEL_12_2: return "12_2";
	case D3D_FEATURE_LEVEL_12_1: return "12_1";
	case D3D_FEATURE_LEVEL_12_0: return "12_0";
	case D3D_FEATURE_LEVEL_11_1: return "11_1";
	case D3D_FEATURE_LEVEL_11_0: return "11_0";
	case D3D_FEATURE_LEVEL_10_1: return "10_1";
	case D3D_FEATURE_LEVEL_10_0: return "10_0";
	case D3D_FEATURE_LEVEL_9_3: return "9_3";
	default: return "other";
	}
}

/* the first hardware adapter: the first IDXGIFactory6 lists for high
performance that is not a software adapter, or without that interface (or
with no such adapter) the first adapter of all; NULL if there is none */
static IDXGIAdapter1 *first_hardware_adapter(void)
{
	IDXGIFactory6 *factory6 = NULL;
	IDXGIAdapter1 *adapter, *found = NULL;
	UINT index;

	if (SUCCEEDED(IDXGIFactory1_QueryInterface(B.factory, &IID_IDXGIFactory6, (void **)&factory6)))
	{
		for (index = 0; !found && SUCCEEDED(IDXGIFactory6_EnumAdapterByGpuPreference(factory6, index,
			DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, (void **)&adapter)); index++)
		{
			DXGI_ADAPTER_DESC1 description;

			if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &description)) &&
				!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
				found = adapter;
			else
				IDXGIAdapter1_Release(adapter);
		}
		release(&factory6);
	}
	if (!found && FAILED(IDXGIFactory1_EnumAdapters1(B.factory, 0, &found)))
		found = NULL;
	return found;
}

/* the adapter asked for, left in B.adapter (NULL for WARP); 0 and the reason
in why if there is none. A name that matches no adapter takes the first
hardware adapter, and says so in note. */
static int choose_adapter(const char *asked, char *why, size_t why_size, char *note, size_t note_size)
{
	IDXGIAdapter1 *adapter;
	UINT index;
	HRESULT result;

	result = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&B.factory);
	if (FAILED(result))
	{
		snprintf(why, why_size, "no adapter (CreateDXGIFactory1: %s)", hr_text(result));
		return 0;
	}
	if (!_stricmp(asked, "warp"))
	{
		B.is_warp = 1;
		return 1;
	}
	if (*asked)
	{
		/* (to the end of the list, or the first failure of any kind) */
		for (index = 0; SUCCEEDED(IDXGIFactory1_EnumAdapters1(B.factory, index, &adapter)); index++)
		{
			char name[256];

			adapter_text(adapter, name, sizeof(name));
			if (name_contains(name, asked))
			{
				B.adapter = adapter;
				return 1;
			}
			IDXGIAdapter1_Release(adapter);
		}
		snprintf(note, note_size, " (\"%s\" matched no adapter)", asked);
	}
	B.adapter = first_hardware_adapter();
	if (!B.adapter)
	{
		snprintf(why, why_size, "no adapter (%s)", hr_text(DXGI_ERROR_NOT_FOUND));
		return 0;
	}
	return 1;
}

static HRESULT device_at(const D3D_FEATURE_LEVEL *levels, UINT count, UINT flags)
{
	return D3D11CreateDevice(B.is_warp ? NULL : (IDXGIAdapter *)B.adapter,
		B.is_warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, NULL, flags, levels, count, D3D11_SDK_VERSION,
		&B.device, &B.level, &B.context);
}

/* the device at 11_1 or 11_0 (Decisions: 11_0 required), with the debug layer
when asked and installed; 0 and the reason in why without one. layer says
what became of the layer. */
static int create_device(int debug_layer, char *why, size_t why_size, char *layer, size_t layer_size)
{
	static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	static const D3D_FEATURE_LEVEL lower_levels[] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	const D3D_FEATURE_LEVEL *asked = levels;
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT, count = sizeof(levels) / sizeof(levels[0]);
	HRESULT result;

	snprintf(layer, layer_size, "debug layer off");
	result = device_at(asked, count, flags | (debug_layer ? D3D11_CREATE_DEVICE_DEBUG : 0));
	/* a runtime without Direct3D 11.1 answers E_INVALIDARG to a list that
	names 11_1 (Windows 7 without its platform update): asked again at 11_0
	alone */
	if (result == E_INVALIDARG)
	{
		asked = levels + 1;
		count = 1;
		result = device_at(asked, count, flags | (debug_layer ? D3D11_CREATE_DEVICE_DEBUG : 0));
	}
	if (debug_layer && result == DXGI_ERROR_SDK_COMPONENT_MISSING)
	{
		snprintf(layer, layer_size, "debug layer missing (DXGI_ERROR_SDK_COMPONENT_MISSING; install the Graphics "
			"Tools feature)");
		debug_layer = 0;
		result = device_at(asked, count, flags);
	}
	if (FAILED(result))
	{
		/* below 11_0 (no level asked for is there): asked again only to name
		the level the adapter has */
		if (result == DXGI_ERROR_UNSUPPORTED &&
			SUCCEEDED(device_at(lower_levels, sizeof(lower_levels) / sizeof(lower_levels[0]), flags)))
		{
			snprintf(why, why_size, "feature level %s is below 11_0", level_name(B.level));
			release(&B.context);
			release(&B.device);
		}
		else
		{
			snprintf(why, why_size, "D3D11CreateDevice failed (%s)", hr_text(result));
		}
		return 0;
	}
	if (debug_layer)
	{
		B.debug_layer = 1;
		snprintf(layer, layer_size, "debug layer on");
		if (SUCCEEDED(ID3D11Device_QueryInterface(B.device, &IID_ID3D11InfoQueue, (void **)&B.info)))
			ID3D11InfoQueue_SetMessageCountLimit(B.info, DEBUG_MESSAGE_LIMIT);
		else
			snprintf(layer, layer_size, "debug layer on, its messages not readable (no ID3D11InfoQueue)");
	}

	/* the adapter the device is on, its name and its driver's version (for
	WARP, the only way to its name) */
	{
		IDXGIDevice *dxgi = NULL;
		IDXGIAdapter *on = NULL;
		LARGE_INTEGER umd;

		if (SUCCEEDED(ID3D11Device_QueryInterface(B.device, &IID_IDXGIDevice, (void **)&dxgi)) &&
			SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &on)))
		{
			release(&B.adapter);
			if (FAILED(IDXGIAdapter_QueryInterface(on, &IID_IDXGIAdapter1, (void **)&B.adapter)))
				B.adapter = NULL;
		}
		release(&on);
		release(&dxgi);
		if (B.adapter)
		{
			adapter_text(B.adapter, B.description, sizeof(B.description));
			result = IDXGIAdapter1_CheckInterfaceSupport(B.adapter, &IID_IDXGIDevice, &umd);
			if (SUCCEEDED(result))
				snprintf(B.driver_version, sizeof(B.driver_version), "%u.%u.%u.%u", HIWORD(umd.HighPart),
					LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
			else
				snprintf(B.driver_version, sizeof(B.driver_version), "not given (%s)", hr_text(result));
		}
		if (!B.description[0])
			snprintf(B.description, sizeof(B.description), "%s", B.is_warp ? "WARP" : "an unnamed adapter");
		if (!B.driver_version[0])
			snprintf(B.driver_version, sizeof(B.driver_version), "not given");
	}
	return 1;
}

static void resources_release(void);

static void backend_release(void)
{
	release(&B.info);
	release(&B.context);
	release(&B.device);
	release(&B.adapter);
	release(&B.factory);
	B.debug_layer = 0;
}

static void exit_handler(void)
{
	d3d11_backend_destroy();
}

int d3d11_backend_create(const char *adapter, int debug_layer, char *line, size_t line_size)
{
	char why[256] = "", note[160] = "", layer[160] = "";

	if (B.device)
	{
		snprintf(line, line_size, "renderer: Direct3D 11 on %s%s, feature level %s, driver %s, made before",
			B.description, B.is_warp ? " (WARP)" : "", level_name(B.level), B.driver_version);
		return 1;
	}
	if (!choose_adapter(adapter ? adapter : "", why, sizeof(why), note, sizeof(note)) ||
		!create_device(debug_layer, why, sizeof(why), layer, sizeof(layer)))
	{
		backend_release();
		snprintf(line, line_size, "renderer: Direct3D 11 unavailable: %s", why);
		return 0;
	}
	snprintf(line, line_size, "renderer: Direct3D 11 on %s%s, feature level %s, driver %s, %s%s", B.description,
		B.is_warp ? " (WARP)" : "", level_name(B.level), B.driver_version, layer, note);
	if (!B.exit_registered)
	{
		B.exit_registered = 1;
		atexit(exit_handler);
	}
	return 1;
}

void d3d11_backend_destroy(void)
{
	ID3D11Debug *debug = NULL;

	if (!B.device)
		return;
	backend_drain_debug();
	ID3D11DeviceContext_ClearState(B.context);
	/* the present's objects, then the targets and the clears'; the flush
	after them lets the context destroy what it still held of them (the
	live-object report counts an object until then) */
	d3d11_backend_present_release();
	resources_release();
	ID3D11DeviceContext_Flush(B.context);
	release(&B.context);
	/* with the layer, what is still alive: the device's own objects are
	expected (phase 0 saw 9 lines for them) */
	if (B.debug_layer && SUCCEEDED(ID3D11Device_QueryInterface(B.device, &IID_ID3D11Debug, (void **)&debug)))
	{
		ID3D11Debug_ReportLiveDeviceObjects(debug, D3D11_RLDO_SUMMARY);
		release(&debug);
		platform_log("d3d11: the live objects at the end, from the debug layer: %u messages", backend_drain_debug());
	}
	backend_release();
	platform_log("d3d11: device released");
}

/* ---------- the lost device */

int d3d11_backend_failed(HRESULT result, const char *call)
{
	if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
	{
		if (!B.lost)
		{
			B.lost = 1;
			snprintf(B.lost_reason, sizeof(B.lost_reason), "%s: %s; reason %s", call, hr_text(result),
				B.device ? hr_text(ID3D11Device_GetDeviceRemovedReason(B.device)) : "unknown");
			platform_log("d3d11: the device was removed (%s)", B.lost_reason);
			backend_drain_debug();
		}
		return 1;
	}
	return FAILED(result);
}

int d3d11_backend_lost(void)
{
	return B.lost;
}

const char *d3d11_backend_lost_reason(void)
{
	return B.lost_reason;
}

/* ---------- the built-in shaders

The clears' and the present's: a triangle over the whole target from
SV_VertexID (no input layout, no vertex buffer), its texture coordinates from
(0, 0) at the top left; the clear's colour and depth from a constant buffer;
the present's picture sampled. Row 0 of a target is the top of the picture,
as "Clip space" in D3D11.md says: no flip. */

static const char builtin_hlsl[] =
	"struct v2p { float4 position : SV_Position; float2 uv : TEXCOORD0; };\n"
	"v2p triangle_vs(uint id : SV_VertexID)\n"
	"{\n"
	"	v2p o;\n"
	"	o.uv = float2((id << 1) & 2, id & 2);\n"
	"	o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
	"	return o;\n"
	"}\n"
	"cbuffer clear_values : register(b0) { float4 clear_color; float clear_z; };\n"
	"struct clear_out { float4 color : SV_Target; float depth : SV_Depth; };\n"
	"clear_out clear_ps(v2p i)\n"
	"{\n"
	"	clear_out o;\n"
	"	o.color = clear_color;\n"
	"	o.depth = clear_z;\n"
	"	return o;\n"
	"}\n"
	"Texture2D picture : register(t0);\n"
	"SamplerState linear_clamp : register(s0);\n"
	"float4 present_ps(v2p i) : SV_Target { return float4(picture.Sample(linear_clamp, i.uv).rgb, 1.0); }\n";

ID3D11VertexShader *d3d11_backend_triangle_vs;
ID3D11PixelShader *d3d11_backend_present_ps;
ID3D11SamplerState *d3d11_backend_linear_sampler;
ID3D11BlendState *d3d11_backend_write_all;
ID3D11DepthStencilState *d3d11_backend_depth_off;
ID3D11RasterizerState *d3d11_backend_scissored;

static struct
{
	HMODULE library;
	pD3DCompile compile;
	/* 1 made, -1 failed (logged once), 0 not tried */
	int state;
	ID3D11PixelShader *clear_ps;
	ID3D11Buffer *constants;
	/* by colour write mask (D3D11_BACKEND_CLEAR_R to _A, as Direct3D's
	D3D11_COLOR_WRITE_ENABLE_* are) */
	ID3D11BlendState *blend[16];
	/* by (depth asked, 1) | (stencil asked, 2) */
	ID3D11DepthStencilState *depth_stencil[4];
} S;

/* d3dcompiler_47.dll, as the probe loads it (win32_d3d11_probe.c, load_compiler) */
static int load_compiler(void)
{
	if (S.compile)
		return 1;
	S.library = LoadLibraryA("d3dcompiler_47.dll");
	if (S.library)
		S.compile = (pD3DCompile)(void *)GetProcAddress(S.library, "D3DCompile");
	if (!S.compile)
	{
		platform_log("d3d11: the shader compiler is not available (d3dcompiler_47.dll: %s, error %lu): the "
			"clears that are drawn are skipped and the window stays black", S.library ? "no D3DCompile" : "not loaded",
			GetLastError());
		return 0;
	}
	return 1;
}

static int compile(const char *entry, const char *target, ID3DBlob **code)
{
	ID3DBlob *messages = NULL;
	HRESULT result;

	result = S.compile(builtin_hlsl, sizeof(builtin_hlsl) - 1, "builtin", NULL, NULL, entry, target,
		D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code, &messages);
	if (FAILED(result))
		platform_log("d3d11: the built-in shader %s (%s) did not compile (%s): %.*s", entry, target, hr_text(result),
			messages ? (int)ID3D10Blob_GetBufferSize(messages) : 0,
			messages ? (const char *)ID3D10Blob_GetBufferPointer(messages) : "");
	release(&messages);
	return SUCCEEDED(result);
}

static int make_shaders(void)
{
	ID3DBlob *vs = NULL, *clear = NULL, *present = NULL;
	D3D11_BUFFER_DESC buffer;
	D3D11_SAMPLER_DESC sampler;
	D3D11_RASTERIZER_DESC rasterizer;
	D3D11_BLEND_DESC blend;
	D3D11_DEPTH_STENCIL_DESC depth;
	HRESULT result = S_OK;
	int index;

	if (!load_compiler() || !compile("triangle_vs", "vs_4_0", &vs) || !compile("clear_ps", "ps_4_0", &clear) ||
		!compile("present_ps", "ps_4_0", &present))
	{
		release(&vs);
		release(&clear);
		release(&present);
		return 0;
	}
	result = ID3D11Device_CreateVertexShader(B.device, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs),
		NULL, &d3d11_backend_triangle_vs);
	if (SUCCEEDED(result))
		result = ID3D11Device_CreatePixelShader(B.device, ID3D10Blob_GetBufferPointer(clear),
			ID3D10Blob_GetBufferSize(clear), NULL, &S.clear_ps);
	if (SUCCEEDED(result))
		result = ID3D11Device_CreatePixelShader(B.device, ID3D10Blob_GetBufferPointer(present),
			ID3D10Blob_GetBufferSize(present), NULL, &d3d11_backend_present_ps);
	release(&vs);
	release(&clear);
	release(&present);

	/* the clear's colour and depth: 32 bytes, rewritten at each drawn clear */
	memset(&buffer, 0, sizeof(buffer));
	buffer.ByteWidth = 32;
	buffer.Usage = D3D11_USAGE_DYNAMIC;
	buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (SUCCEEDED(result))
		result = ID3D11Device_CreateBuffer(B.device, &buffer, NULL, &S.constants);

	/* the present's: linear, clamped (the one sampler state of phase 2) */
	memset(&sampler, 0, sizeof(sampler));
	sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.MaxAnisotropy = 1;
	sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	if (SUCCEEDED(result))
		result = ID3D11Device_CreateSamplerState(B.device, &sampler, &d3d11_backend_linear_sampler);

	memset(&rasterizer, 0, sizeof(rasterizer));
	rasterizer.FillMode = D3D11_FILL_SOLID;
	rasterizer.CullMode = D3D11_CULL_NONE;
	rasterizer.DepthClipEnable = TRUE;
	rasterizer.ScissorEnable = TRUE;
	if (SUCCEEDED(result))
		result = ID3D11Device_CreateRasterizerState(B.device, &rasterizer, &d3d11_backend_scissored);

	/* the sixteen colour write masks, without blending */
	for (index = 0; index < 16 && SUCCEEDED(result); index++)
	{
		memset(&blend, 0, sizeof(blend));
		blend.RenderTarget[0].BlendEnable = FALSE;
		blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blend.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
		blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blend.RenderTarget[0].RenderTargetWriteMask = (UINT8)index;
		result = ID3D11Device_CreateBlendState(B.device, &blend, &S.blend[index]);
	}
	d3d11_backend_write_all = S.blend[15];

	/* depth written as asked, always; stencil replaced as asked, always */
	for (index = 0; index < 4 && SUCCEEDED(result); index++)
	{
		memset(&depth, 0, sizeof(depth));
		depth.DepthEnable = (index & 1) ? TRUE : FALSE;
		depth.DepthWriteMask = (index & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
		depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
		depth.StencilEnable = (index & 2) ? TRUE : FALSE;
		depth.StencilReadMask = 0xff;
		depth.StencilWriteMask = 0xff;
		depth.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		depth.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		depth.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
		depth.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
		depth.BackFace = depth.FrontFace;
		result = ID3D11Device_CreateDepthStencilState(B.device, &depth, &S.depth_stencil[index]);
	}
	d3d11_backend_depth_off = S.depth_stencil[0];

	if (backend_failed(result, "the built-in shaders' objects"))
	{
		platform_log("d3d11: the built-in shaders' objects were not made (%s): the clears that are drawn are "
			"skipped and the window stays black", hr_text(result));
		return 0;
	}
	return 1;
}

int d3d11_backend_shaders(void)
{
	if (!S.state && B.device && !B.lost)
		S.state = make_shaders() ? 1 : -1;
	return S.state == 1;
}

/* ---------- render targets

The game's, made when the device first finds a surface it draws into
(d3d8_d3d11.c, render_target_get), kept in a table the handle indexes (1
upward) and never freed, as d3d8_gl.c keeps its textures; and the self-test's
own, made and released by it. */

struct target
{
	ID3D11Texture2D *texture;
	ID3D11RenderTargetView *color;
	ID3D11DepthStencilView *depth;
	ID3D11ShaderResourceView *view;
	uint32_t width, height;
	int is_depth;
};

static struct target *targets;
static uint32_t target_count, target_capacity;

/* what is bound (d3d11_backend_targets_set), for the clears */
static struct target *bound_color, *bound_depth;

/* the read-back's staging texture, kept while the size is the same */
static ID3D11Texture2D *staging;
static uint32_t staging_width, staging_height;

static void target_free(struct target *target)
{
	release(&target->view);
	release(&target->depth);
	release(&target->color);
	release(&target->texture);
}

static int target_make(struct target *target, uint32_t width, uint32_t height, int depth)
{
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	D3D11_TEXTURE2D_DESC description;
	HRESULT result;

	memset(target, 0, sizeof(*target));
	memset(&description, 0, sizeof(description));
	description.Width = width;
	description.Height = height;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = depth ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_B8G8R8A8_UNORM;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	result = ID3D11Device_CreateTexture2D(B.device, &description, NULL, &target->texture);
	if (SUCCEEDED(result))
	{
		if (depth)
			result = ID3D11Device_CreateDepthStencilView(B.device, (ID3D11Resource *)target->texture, NULL,
				&target->depth);
		else
			result = ID3D11Device_CreateRenderTargetView(B.device, (ID3D11Resource *)target->texture, NULL,
				&target->color);
	}
	if (SUCCEEDED(result) && !depth)
		result = ID3D11Device_CreateShaderResourceView(B.device, (ID3D11Resource *)target->texture, NULL,
			&target->view);
	if (backend_failed(result, "a render target"))
	{
		platform_log("d3d11: a %s target of %ux%u was not made (%s)", depth ? "depth" : "colour", width, height,
			hr_text(result));
		target_free(target);
		return 0;
	}
	target->width = width;
	target->height = height;
	target->is_depth = depth;
	/* so that a target read before anything was drawn into it reads the same
	on every driver */
	if (depth)
		ID3D11DeviceContext_ClearDepthStencilView(B.context, target->depth, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
			1.0f, 0);
	else
		ID3D11DeviceContext_ClearRenderTargetView(B.context, target->color, black);
	return 1;
}

static struct target *target_from_handle(uint32_t handle)
{
	return handle && handle <= target_count ? &targets[handle - 1] : NULL;
}

uint32_t d3d11_backend_target_create(uint32_t pixel_width, uint32_t pixel_height, int depth)
{
	if (!B.device || B.lost || !pixel_width || !pixel_height)
		return 0;
	if (target_count == target_capacity)
	{
		uint32_t capacity = target_capacity ? target_capacity * 2 : 64;
		/* (the bound pointers follow the table: their places in it, taken
		before realloc frees the old one; -1 for none, or one not in it) */
		ptrdiff_t color_at = bound_color && bound_color >= targets && bound_color < targets + target_count ?
			bound_color - targets : -1;
		ptrdiff_t depth_at = bound_depth && bound_depth >= targets && bound_depth < targets + target_count ?
			bound_depth - targets : -1;
		struct target *grown = realloc(targets, capacity * sizeof(*targets));

		if (!grown)
			return 0;
		if (color_at >= 0)
			bound_color = grown + color_at;
		if (depth_at >= 0)
			bound_depth = grown + depth_at;
		targets = grown;
		target_capacity = capacity;
	}
	if (!target_make(&targets[target_count], pixel_width, pixel_height, depth))
		return 0;
	return ++target_count;
}

uint32_t d3d11_backend_targets_alive(void)
{
	return target_count;
}

ID3D11ShaderResourceView *d3d11_backend_target_view(uint32_t handle, uint32_t *width, uint32_t *height)
{
	struct target *target = target_from_handle(handle);

	if (!target || !target->view)
		return NULL;
	*width = target->width;
	*height = target->height;
	return target->view;
}

/* the views bound, and the viewport over the whole colour target (or the
depth one). Direct3D 11 binds a depth target with a colour target only of
the same size: one of another size is left out, logged once. */
static void targets_bind(struct target *color, struct target *depth)
{
	ID3D11RenderTargetView *views[1];
	D3D11_VIEWPORT viewport;
	struct target *whole;

	if (color && depth && (color->width != depth->width || color->height != depth->height))
	{
		static int logged;

		if (!logged)
		{
			logged = 1;
			platform_log("d3d11: a depth target of %ux%u named with a colour target of %ux%u is left out (Direct3D 11 "
				"binds the two only at one size; logged once)", depth->width, depth->height, color->width,
				color->height);
		}
		depth = NULL;
	}
	bound_color = color;
	bound_depth = depth;
	views[0] = color ? color->color : NULL;
	ID3D11DeviceContext_OMSetRenderTargets(B.context, 1, views, depth ? depth->depth : NULL);
	whole = color ? color : depth;
	if (whole)
	{
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = (float)whole->width;
		viewport.Height = (float)whole->height;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		ID3D11DeviceContext_RSSetViewports(B.context, 1, &viewport);
	}
}

void d3d11_backend_targets_set(uint32_t color, uint32_t depth)
{
	struct target *color_target = target_from_handle(color), *depth_target = target_from_handle(depth);

	if (!B.device || B.lost)
		return;
	targets_bind(color_target && !color_target->is_depth ? color_target : NULL,
		depth_target && depth_target->is_depth ? depth_target : NULL);
}

void d3d11_backend_targets_forget(void)
{
	bound_color = NULL;
	bound_depth = NULL;
}

/* ---------- clears */

int d3d11_backend_draw_clear(uint32_t channels, const float color[4], float z, uint32_t stencil,
	const D3D11_RECT *rectangle)
{
	D3D11_MAPPED_SUBRESOURCE mapped;
	float *values;
	int depth = (channels & D3D11_BACKEND_CLEAR_DEPTH) != 0;
	int stencil_asked = (channels & D3D11_BACKEND_CLEAR_STENCIL) != 0;
	HRESULT result;

	if (!d3d11_backend_shaders())
		return 0;
	result = ID3D11DeviceContext_Map(B.context, (ID3D11Resource *)S.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (backend_failed(result, "Map (the clear's constants)"))
		return 0;
	values = mapped.pData;
	memcpy(values, color, 4 * sizeof(float));
	values[4] = z;
	values[5] = values[6] = values[7] = 0.0f;
	ID3D11DeviceContext_Unmap(B.context, (ID3D11Resource *)S.constants, 0);

	ID3D11DeviceContext_IASetInputLayout(B.context, NULL);
	ID3D11DeviceContext_IASetPrimitiveTopology(B.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_VSSetShader(B.context, d3d11_backend_triangle_vs, NULL, 0);
	ID3D11DeviceContext_PSSetShader(B.context, S.clear_ps, NULL, 0);
	ID3D11DeviceContext_PSSetConstantBuffers(B.context, 0, 1, &S.constants);
	ID3D11DeviceContext_OMSetBlendState(B.context, S.blend[channels & D3D11_BACKEND_CLEAR_COLOR], NULL, 0xffffffff);
	ID3D11DeviceContext_OMSetDepthStencilState(B.context, S.depth_stencil[depth | stencil_asked << 1], stencil & 0xff);
	ID3D11DeviceContext_RSSetState(B.context, d3d11_backend_scissored);
	ID3D11DeviceContext_RSSetScissorRects(B.context, 1, rectangle);
	ID3D11DeviceContext_Draw(B.context, 3, 0);
	return 1;
}

uint32_t d3d11_backend_clear(uint32_t channels, const float color[4], float z, uint32_t stencil,
	const int32_t *rectangles, uint32_t count)
{
	struct target *whole;
	uint32_t index, drawn_channels;
	int color_call = 0, depth_call = 0, drawn = 0;

	if (!B.device || B.lost)
		return 0;
	if (!bound_color)
		channels &= ~D3D11_BACKEND_CLEAR_COLOR;
	if (!bound_depth)
		channels &= ~(D3D11_BACKEND_CLEAR_DEPTH | D3D11_BACKEND_CLEAR_STENCIL);
	whole = bound_color ? bound_color : bound_depth;
	if (!channels || !whole || !count)
		return 0;

	/* the whole target in one rectangle: clear calls (all four colour
	channels; depth, stencil or both) */
	if (count == 1 && rectangles[0] <= 0 && rectangles[1] <= 0 &&
		rectangles[0] + rectangles[2] >= (int32_t)whole->width && rectangles[1] + rectangles[3] >= (int32_t)whole->height)
	{
		if ((channels & D3D11_BACKEND_CLEAR_COLOR) == D3D11_BACKEND_CLEAR_COLOR)
		{
			ID3D11DeviceContext_ClearRenderTargetView(B.context, bound_color->color, color);
			color_call = 1;
		}
		if (channels & (D3D11_BACKEND_CLEAR_DEPTH | D3D11_BACKEND_CLEAR_STENCIL))
		{
			ID3D11DeviceContext_ClearDepthStencilView(B.context, bound_depth->depth,
				((channels & D3D11_BACKEND_CLEAR_DEPTH) ? D3D11_CLEAR_DEPTH : 0) |
				((channels & D3D11_BACKEND_CLEAR_STENCIL) ? D3D11_CLEAR_STENCIL : 0), z, (UINT8)stencil);
			depth_call = 1;
		}
	}
	/* everything else is drawn, a rectangle at a time, within the target */
	drawn_channels = channels & ~(color_call ? D3D11_BACKEND_CLEAR_COLOR : 0) &
		~(depth_call ? D3D11_BACKEND_CLEAR_DEPTH | D3D11_BACKEND_CLEAR_STENCIL : 0);
	if (!drawn_channels)
		return 1;
	for (index = 0; index < count; index++)
	{
		const int32_t *rectangle = rectangles + index * 4;
		D3D11_RECT scissor;

		scissor.left = rectangle[0] > 0 ? rectangle[0] : 0;
		scissor.top = rectangle[1] > 0 ? rectangle[1] : 0;
		scissor.right = rectangle[0] + rectangle[2] < (int32_t)whole->width ? rectangle[0] + rectangle[2] :
			(LONG)whole->width;
		scissor.bottom = rectangle[1] + rectangle[3] < (int32_t)whole->height ? rectangle[1] + rectangle[3] :
			(LONG)whole->height;
		if (scissor.left >= scissor.right || scissor.top >= scissor.bottom)
			continue;
		if (!d3d11_backend_draw_clear(drawn_channels, color, z, stencil, &scissor))
			return 0;
		drawn = 1;
	}
	return drawn ? 2 : color_call || depth_call ? 1 : 0;
}

/* ---------- the read-back */

static int read_target(struct target *target, void *pixels, size_t size)
{
	D3D11_TEXTURE2D_DESC description;
	D3D11_MAPPED_SUBRESOURCE mapped;
	uint32_t row;
	HRESULT result;

	if (target->is_depth)
	{
		platform_log("d3d11: a depth target is not read back");
		return 0;
	}
	if (size < (size_t)target->width * target->height * 4)
	{
		platform_log("d3d11: a read-back of %ux%u was given %lu bytes", target->width, target->height,
			(unsigned long)size);
		return 0;
	}
	if (!staging || staging_width != target->width || staging_height != target->height)
	{
		release(&staging);
		memset(&description, 0, sizeof(description));
		description.Width = target->width;
		description.Height = target->height;
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_STAGING;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		result = ID3D11Device_CreateTexture2D(B.device, &description, NULL, &staging);
		if (backend_failed(result, "the read-back's staging texture"))
		{
			platform_log("d3d11: the read-back's staging texture of %ux%u was not made (%s)", target->width,
				target->height, hr_text(result));
			return 0;
		}
		staging_width = target->width;
		staging_height = target->height;
	}
	ID3D11DeviceContext_CopyResource(B.context, (ID3D11Resource *)staging, (ID3D11Resource *)target->texture);
	result = ID3D11DeviceContext_Map(B.context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
	if (backend_failed(result, "Map (the read-back)"))
	{
		platform_log("d3d11: the read-back's Map failed (%s)", hr_text(result));
		return 0;
	}
	for (row = 0; row < target->height; row++)
		memcpy((char *)pixels + (size_t)row * target->width * 4,
			(const char *)mapped.pData + (size_t)row * mapped.RowPitch, (size_t)target->width * 4);
	ID3D11DeviceContext_Unmap(B.context, (ID3D11Resource *)staging, 0);
	return 1;
}

int d3d11_backend_target_read(uint32_t handle, void *pixels, size_t size)
{
	struct target *target = target_from_handle(handle);

	if (!B.device || B.lost || !target)
		return 0;
	return read_target(target, pixels, size);
}

/* ---------- the clears' self-test

As the Vulkan renderer's (port/android/VULKAN.md, "Phase 2 — summary"): an
8x8 colour and depth target of the backend's own, cleared in full, then alpha
only on the left half and red only (with depth) on the top right quarter,
both drawn; the colour read back and every texel compared with what the NV2A
would give. */

#define SELF_TEST_SIZE 8

int d3d11_backend_self_test(char *line, size_t line_size)
{
	static const float full[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	static const float alpha[4] = { 0.0f, 0.0f, 0.0f, 0.5f };
	static const float red[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
	static const int32_t whole[4] = { 0, 0, SELF_TEST_SIZE, SELF_TEST_SIZE };
	static const int32_t left[4] = { 0, 0, SELF_TEST_SIZE / 2, SELF_TEST_SIZE };
	static const int32_t top_right[4] = { SELF_TEST_SIZE / 2, 0, SELF_TEST_SIZE / 2, SELF_TEST_SIZE / 2 };
	struct target color, depth, *was_color = bound_color, *was_depth = bound_depth;
	unsigned char pixels[SELF_TEST_SIZE * SELF_TEST_SIZE * 4];
	uint32_t results[3];
	int x, y, by_calls = 0, drawn = 0, ok = 1;

	if (!B.device || B.lost)
	{
		snprintf(line, line_size, "d3d11: clears self-test: not run (no device)");
		return 0;
	}
	if (!target_make(&color, SELF_TEST_SIZE, SELF_TEST_SIZE, 0))
	{
		snprintf(line, line_size, "d3d11: clears self-test: FAILED (its colour target was not made)");
		return 0;
	}
	if (!target_make(&depth, SELF_TEST_SIZE, SELF_TEST_SIZE, 1))
	{
		target_free(&color);
		snprintf(line, line_size, "d3d11: clears self-test: FAILED (its depth target was not made)");
		return 0;
	}
	targets_bind(&color, &depth);
	results[0] = d3d11_backend_clear(D3D11_BACKEND_CLEAR_COLOR | D3D11_BACKEND_CLEAR_DEPTH | D3D11_BACKEND_CLEAR_STENCIL,
		full, 0.5f, 7, whole, 1);
	results[1] = d3d11_backend_clear(D3D11_BACKEND_CLEAR_A, alpha, 0.0f, 0, left, 1);
	results[2] = d3d11_backend_clear(D3D11_BACKEND_CLEAR_R | D3D11_BACKEND_CLEAR_DEPTH, red, 0.25f, 0, top_right, 1);
	for (x = 0; x < 3; x++)
	{
		by_calls += results[x] == 1;
		drawn += results[x] == 2;
	}
	if (!read_target(&color, pixels, sizeof(pixels)))
	{
		snprintf(line, line_size, "d3d11: clears self-test: FAILED (the colour target was not read back)");
		ok = 0;
	}
	for (y = 0; ok && y < SELF_TEST_SIZE; y++)
	{
		for (x = 0; ok && x < SELF_TEST_SIZE; x++)
		{
			/* what the NV2A gives: the full clear, alpha 0.5 on the left half,
			red 1 on the top right quarter (B, G, R, A in memory) */
			const unsigned char *texel = pixels + (y * SELF_TEST_SIZE + x) * 4;
			float want[4];
			int channel;

			want[0] = full[2] * 255.0f;
			want[1] = full[1] * 255.0f;
			want[2] = (x >= SELF_TEST_SIZE / 2 && y < SELF_TEST_SIZE / 2 ? red[0] : full[0]) * 255.0f;
			want[3] = (x < SELF_TEST_SIZE / 2 ? alpha[3] : full[3]) * 255.0f;
			for (channel = 0; ok && channel < 4; channel++)
			{
				if (texel[channel] < want[channel] - 1.0f || texel[channel] > want[channel] + 1.0f)
				{
					snprintf(line, line_size, "d3d11: clears self-test: FAILED at (%d, %d): B G R A %u %u %u %u, should "
						"be %.1f %.1f %.1f %.1f (the clears answered %u %u %u)", x, y, texel[0], texel[1], texel[2],
						texel[3], want[0], want[1], want[2], want[3], results[0], results[1], results[2]);
					ok = 0;
				}
			}
		}
	}
	if (ok && (by_calls != 1 || drawn != 2))
	{
		snprintf(line, line_size, "d3d11: clears self-test: FAILED (the right texels, but %d clears drawn and %d by "
			"clear calls, where 2 and 1 were expected)", drawn, by_calls);
		ok = 0;
	}
	if (ok)
		snprintf(line, line_size, "d3d11: clears self-test: ok (a full clear, alpha only on the left half, red only on "
			"the top right quarter; %d partial clears drawn, %d by clear calls; the depth target is not read back)",
			drawn, by_calls);
	/* (what was bound before, again) */
	ID3D11DeviceContext_OMSetRenderTargets(B.context, 0, NULL, NULL);
	if (was_color || was_depth)
		targets_bind(was_color, was_depth);
	else
		d3d11_backend_targets_forget();
	target_free(&depth);
	target_free(&color);
	return ok;
}

static void resources_release(void)
{
	uint32_t index;
	int slot;

	d3d11_backend_targets_forget();
	release(&staging);
	for (index = 0; index < target_count; index++)
		target_free(&targets[index]);
	free(targets);
	targets = NULL;
	target_count = target_capacity = 0;
	for (slot = 0; slot < 4; slot++)
		release(&S.depth_stencil[slot]);
	for (slot = 0; slot < 16; slot++)
		release(&S.blend[slot]);
	d3d11_backend_write_all = NULL;
	d3d11_backend_depth_off = NULL;
	release(&d3d11_backend_scissored);
	release(&d3d11_backend_linear_sampler);
	release(&S.constants);
	release(&d3d11_backend_present_ps);
	release(&S.clear_ps);
	release(&d3d11_backend_triangle_vs);
	S.state = 0;
}
