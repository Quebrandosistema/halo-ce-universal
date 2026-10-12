/*
WIN32_D3D11.C

The Direct3D 11 backend of the Windows build's Direct3D renderer
(port/windows/D3D11.md): the half that talks to Direct3D 11 and DXGI, called
by the Direct3D 8 device (d3d8_d3d11.c) through d3d11_backend.h. A
Windows-facing unit, compiled against the Windows SDK and not the Xbox SDK's
headers, as port/windows/src/win32_*.c are.

Phase 1: the factory, the adapter display.d3d11_adapter names, the device at
feature level 11_0 or above, the debug layer when debug.d3d11_debug asks for
it, and the one line that says what was made. Nothing is drawn yet: no swap
chain, no resources (phase 2 on).

hr_text, drain_debug (as backend_drain_debug), choose_adapter and
create_device are copied from phase 0's probe (win32_d3d11_probe.c), where they
were tried on this machine's Adreno and on WARP, and changed only for what the
backend keeps and reports: a name that matches no adapter takes the first
hardware adapter (the game has a window to keep), and below feature level 11_0
there is no device.
*/

#define COBJMACROS
#include <windows.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <dxgi1_6.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d11_backend.h"

/* (the platform layer's log, declared by hand as win32_memory_watch.c does:
platform.h is the Xbox SDK's side) */
void platform_log(const char *format, ...);

/* the debug layer's queue keeps this many messages between drains (the
probe's figure); a frame that makes more says how many were lost */
#define DEBUG_MESSAGE_LIMIT 65536

static struct
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
} B;

/* an HRESULT as hex and, for the common ones, its name (four buffers, so one line can show several) */
static const char *hr_text(HRESULT result)
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

static void release(void *object)
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
		for (index = 0; IDXGIFactory1_EnumAdapters1(B.factory, index, &adapter) != DXGI_ERROR_NOT_FOUND; index++)
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
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	HRESULT result;

	snprintf(layer, layer_size, "debug layer off");
	result = device_at(levels, sizeof(levels) / sizeof(levels[0]), flags | (debug_layer ? D3D11_CREATE_DEVICE_DEBUG : 0));
	if (debug_layer && result == DXGI_ERROR_SDK_COMPONENT_MISSING)
	{
		snprintf(layer, layer_size, "debug layer missing (DXGI_ERROR_SDK_COMPONENT_MISSING; install the Graphics "
			"Tools feature)");
		debug_layer = 0;
		result = device_at(levels, sizeof(levels) / sizeof(levels[0]), flags);
	}
	if (FAILED(result))
	{
		/* below 11_0 (no level asked for is there): asked again only to name
		the level the adapter has */
		if ((result == DXGI_ERROR_UNSUPPORTED || result == E_INVALIDARG) &&
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
