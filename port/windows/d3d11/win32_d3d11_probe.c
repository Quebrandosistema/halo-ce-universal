/*
WIN32_D3D11_PROBE.C

The Direct3D 11 probe (port/windows/D3D11.md, phase 0): measures what this
machine's Direct3D 11 offers and costs, before a renderer is built on it. A
console program of its own, built from this one Windows-facing unit
(`ninja windows_d3d11_probe`, build/windows/d3d11_probe.exe):

	d3d11_probe.exe [steps] [--adapter hardware|warp|<part of a name>]
		[--shaders <folder>] [--report <file>] [--sync 0|1] [--quiet]

(--quiet: the present step plays no tone.) It is written to measure, not to be the backend: every step builds what it
needs, times it and releases it. Results are lines "section.key: value" in
the report (d3d11_probe.txt in the current folder unless --report names
another; flushed after every line, so a probe that dies leaves the line it
died after) and on standard output with the prefix "d3d11 probe:".

Steps, run in this order whatever order they are asked for in:
	caps      what the adapter is and offers (the plan's step 1)
	compile   D3DCompile's cost on game-sized shaders (3)
	memory    buffers: the stream ring, the mirror, rewrites, texture uploads (4)
	queries   occlusion queries' latency and exactness (5)
	present   an SDL window with a swap chain, its transitions and pacing (2;
	          it waits for a tester, so it is last)

caps always runs: every other step needs what it finds. A step that kills
the probe is written to d3d11_probe_running.txt beside the report first; the
next run finds the file, adds the name to d3d11_probe_skip.txt and leaves that
step out, and says so.
*/

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <SDL3/SDL.h>

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HALO_PROBE_BUILD: the commit the probe is built from, written at every build (tools/android_build_stamp.py) */
#include "d3d11_probe_build.h"

/* ---------- the report */

static FILE *report_file;

static void rep(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void rep(const char *format, ...)
{
	char line[2048];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (report_file)
	{
		fprintf(report_file, "%s\n", line);
		fflush(report_file);
	}
	printf("d3d11 probe: %s\n", line);
	fflush(stdout);
}

static const char *yesno(int value)
{
	return value ? "yes" : "no";
}

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

/* ---------- time */

static double counter_frequency;

static double now_ms(void)
{
	LARGE_INTEGER counter;

	QueryPerformanceCounter(&counter);
	return (double)counter.QuadPart * 1000.0 / counter_frequency;
}

/* this thread's CPU time (user and kernel), milliseconds */
static double cpu_ms(void)
{
	FILETIME creation, exit, kernel, user;
	ULARGE_INTEGER k, u;

	if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user))
		return 0.0;
	k.LowPart = kernel.dwLowDateTime;
	k.HighPart = kernel.dwHighDateTime;
	u.LowPart = user.dwLowDateTime;
	u.HighPart = user.dwHighDateTime;
	return (double)(k.QuadPart + u.QuadPart) / 10000.0;
}

static int compare_doubles(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

/* the first value on its own, then the minimum and median of the rest (milliseconds) */
static void rep_times(const char *key, const double *values, int count)
{
	double *sorted;

	if (count < 1)
		return;
	if (count == 1)
	{
		rep("%s: first %.3f ms", key, values[0]);
		return;
	}
	sorted = malloc(sizeof(double) * (size_t)(count - 1));
	memcpy(sorted, values + 1, sizeof(double) * (size_t)(count - 1));
	qsort(sorted, (size_t)(count - 1), sizeof(double), compare_doubles);
	rep("%s: first %.3f ms, then min %.3f ms, median %.3f ms, max %.3f ms (%d more)", key, values[0], sorted[0],
		sorted[(count - 1) / 2], sorted[count - 2], count - 1);
	free(sorted);
}

/* min, median, p99 and max of all the values, milliseconds; values below 0 (not measured) are left out */
static void rep_spread(const char *key, const double *values, int count)
{
	double *sorted = malloc(sizeof(double) * (size_t)(count > 0 ? count : 1));
	int index, kept = 0;

	for (index = 0; index < count; index++)
		if (values[index] >= 0.0)
			sorted[kept++] = values[index];
	if (kept > 0)
	{
		qsort(sorted, (size_t)kept, sizeof(double), compare_doubles);
		rep("%s: min %.3f ms, median %.3f ms, p99 %.3f ms, max %.3f ms (%d)", key, sorted[0], sorted[kept / 2],
			sorted[(kept * 99) / 100 < kept ? (kept * 99) / 100 : kept - 1], sorted[kept - 1], kept);
	}
	else
	{
		rep("%s: not measured", key);
	}
	free(sorted);
}

/* ---------- the probe's state */

static struct
{
	char adapter_asked[128];
	char shaders_dir[MAX_PATH];
	char report_dir[MAX_PATH];
	int sync;
	int quiet;
	int failed;

	IDXGIFactory1 *factory;
	IDXGIAdapter1 *adapter;
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	ID3D11InfoQueue *info;
	D3D_FEATURE_LEVEL level;
	int debug_layer;
	int is_warp;
	unsigned long long available_virtual_before;

	HMODULE compiler;
	pD3DCompile compile;

	int debug_messages, debug_errors, debug_warnings;
} P;

static void release(void *object)
{
	IUnknown **pointer = object;

	if (*pointer)
	{
		IUnknown_Release(*pointer);
		*pointer = NULL;
	}
}

/* a call that failed: the step's failure line, and the probe will exit 1 */
static int step_failed(const char *step, const char *where, HRESULT result)
{
	rep("step.%s: failed (%s, %s)", step, where, hr_text(result));
	P.failed = 1;
	return 0;
}

#define CHECK(step, call) do { HRESULT check_result_ = (call); \
	if (FAILED(check_result_)) { step_failed(step, #call, check_result_); goto done; } } while (0)

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

/* every message the layer stored since the last call, reported and counted; returns how many */
static int drain_debug(const char *where)
{
	UINT64 count, index;
	int reported = 0;

	if (!P.info)
		return 0;
	count = ID3D11InfoQueue_GetNumStoredMessages(P.info);
	for (index = 0; index < count; index++)
	{
		SIZE_T length = 0;
		D3D11_MESSAGE *message;

		if (FAILED(ID3D11InfoQueue_GetMessage(P.info, index, NULL, &length)) || !length)
			continue;
		message = malloc(length);
		if (SUCCEEDED(ID3D11InfoQueue_GetMessage(P.info, index, message, &length)))
		{
			rep("debug.message: %s %s %d %.*s", where, severity_name(message->Severity), (int)message->ID,
				(int)(message->DescriptionByteLength ? message->DescriptionByteLength - 1 : 0),
				message->pDescription ? message->pDescription : "");
			P.debug_messages++;
			if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
				P.debug_errors++;
			else if (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING)
				P.debug_warnings++;
			reported++;
		}
		free(message);
	}
	if (ID3D11InfoQueue_GetNumMessagesDiscardedByMessageCountLimit(P.info))
		rep("debug.discarded: %s %llu (over the queue's limit)", where,
			(unsigned long long)ID3D11InfoQueue_GetNumMessagesDiscardedByMessageCountLimit(P.info));
	ID3D11InfoQueue_ClearStoredMessages(P.info);
	return reported;
}

/* ---------- the machine */

static const char *machine_name(USHORT machine)
{
	switch (machine)
	{
	case IMAGE_FILE_MACHINE_UNKNOWN: return "unknown (not under WOW64)";
	case IMAGE_FILE_MACHINE_I386: return "x86";
	case IMAGE_FILE_MACHINE_AMD64: return "x64";
	case IMAGE_FILE_MACHINE_ARM64: return "arm64";
	case IMAGE_FILE_MACHINE_ARMNT: return "arm";
	default: return "other";
	}
}

static const char *architecture_name(WORD architecture)
{
	switch (architecture)
	{
	case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
	case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
	case PROCESSOR_ARCHITECTURE_ARM: return "arm";
	case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
	default: return "other";
	}
}

static void report_address_space(const char *key)
{
	MEMORYSTATUSEX status;

	status.dwLength = sizeof(status);
	if (GlobalMemoryStatusEx(&status))
		rep("%s: %.1f MB available of %.1f MB virtual", key, (double)status.ullAvailVirtual / 1048576.0,
			(double)status.ullTotalVirtual / 1048576.0);
}

static unsigned long long available_virtual(void)
{
	MEMORYSTATUSEX status;

	status.dwLength = sizeof(status);
	return GlobalMemoryStatusEx(&status) ? status.ullAvailVirtual : 0;
}

static void report_machine(void)
{
	typedef LONG (WINAPI *rtl_get_version)(OSVERSIONINFOW *);
	typedef BOOL (WINAPI *is_wow64_process2)(HANDLE, USHORT *, USHORT *);
	SYSTEM_INFO system;
	OSVERSIONINFOW version;
	rtl_get_version get_version = (rtl_get_version)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
	is_wow64_process2 wow64 = (is_wow64_process2)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),
		"IsWow64Process2");
	char processor[256];
	DWORD size = sizeof(processor);
	double start;
	double step = 0.0, first, now;

	rep("build.git: %s", HALO_PROBE_BUILD);
	GetNativeSystemInfo(&system);
	rep("machine.native_system_info: %s (%u processors)", architecture_name(system.wProcessorArchitecture),
		(unsigned)system.dwNumberOfProcessors);
	if (wow64)
	{
		USHORT process = 0, native = 0;

		if (wow64(GetCurrentProcess(), &process, &native))
			rep("machine.is_wow64_process2: process %s (0x%04x), native %s (0x%04x)", machine_name(process),
				process, machine_name(native), native);
		else
			rep("machine.is_wow64_process2: failed (%lu)", GetLastError());
	}
	else
	{
		rep("machine.is_wow64_process2: not offered by kernel32");
	}
	if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
		RRF_RT_REG_SZ, NULL, processor, &size) == ERROR_SUCCESS)
		rep("machine.processor: %s", processor);
	memset(&version, 0, sizeof(version));
	version.dwOSVersionInfoSize = sizeof(version);
	if (get_version && get_version(&version) == 0)
		rep("machine.os: Windows %lu.%lu.%lu", version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);

	/* GetThreadTimes' resolution, so a thread CPU figure is read for what it is */
	first = cpu_ms();
	start = now_ms();
	while (now_ms() - start < 200.0)
	{
		now = cpu_ms();
		if (now != first)
		{
			step = now - first;
			break;
		}
	}
	rep("timing.thread_cpu_step: %.4f ms (the smallest change of GetThreadTimes seen)", step);
	rep("timing.counter_frequency: %.0f Hz", counter_frequency);
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

/* every adapter DXGI lists, and the one --adapter asks for (left in P.adapter; NULL for WARP) */
static int choose_adapter(void)
{
	IDXGIFactory6 *factory6 = NULL;
	IDXGIAdapter1 *adapter;
	UINT index;
	HRESULT result;

	result = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&P.factory);
	if (FAILED(result))
		return step_failed("setup", "CreateDXGIFactory1", result);
	rep("dxgi.factory6: %s", yesno(SUCCEEDED(IDXGIFactory1_QueryInterface(P.factory, &IID_IDXGIFactory6,
		(void **)&factory6))));

	for (index = 0; IDXGIFactory1_EnumAdapters1(P.factory, index, &adapter) != DXGI_ERROR_NOT_FOUND; index++)
	{
		DXGI_ADAPTER_DESC1 description;
		LARGE_INTEGER umd;
		char name[256];

		if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &description)))
		{
			adapter_text(adapter, name, sizeof(name));
			rep("adapter.%u.description: %s", index, name);
			rep("adapter.%u.vendor_id: 0x%04x", index, description.VendorId);
			rep("adapter.%u.device_id: 0x%04x", index, description.DeviceId);
			rep("adapter.%u.subsys_id: 0x%08x, revision %u", index, description.SubSysId, description.Revision);
			rep("adapter.%u.dedicated_video_memory: %.1f MB", index,
				(double)description.DedicatedVideoMemory / 1048576.0);
			rep("adapter.%u.dedicated_system_memory: %.1f MB", index,
				(double)description.DedicatedSystemMemory / 1048576.0);
			rep("adapter.%u.shared_system_memory: %.1f MB", index, (double)description.SharedSystemMemory / 1048576.0);
			rep("adapter.%u.software: %s", index, yesno(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE));
			result = IDXGIAdapter1_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &umd);
			if (SUCCEEDED(result))
				rep("adapter.%u.driver_version: %u.%u.%u.%u", index, HIWORD(umd.HighPart), LOWORD(umd.HighPart),
					HIWORD(umd.LowPart), LOWORD(umd.LowPart));
			else
				rep("adapter.%u.driver_version: not given (%s)", index, hr_text(result));
		}
		IDXGIAdapter1_Release(adapter);
	}

	if (!strcmp(P.adapter_asked, "warp"))
	{
		P.is_warp = 1;
		rep("adapter.chosen: WARP (D3D_DRIVER_TYPE_WARP, asked for)");
	}
	else if (!strcmp(P.adapter_asked, "hardware"))
	{
		if (factory6)
		{
			for (index = 0; SUCCEEDED(IDXGIFactory6_EnumAdapterByGpuPreference(factory6, index,
				DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1, (void **)&adapter)); index++)
			{
				DXGI_ADAPTER_DESC1 description;

				if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &description)) &&
					!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
				{
					P.adapter = adapter;
					rep("adapter.chosen_by: IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE), "
						"position %u, the first that is not software", index);
					break;
				}
				IDXGIAdapter1_Release(adapter);
			}
		}
		if (!P.adapter && SUCCEEDED(IDXGIFactory1_EnumAdapters1(P.factory, 0, &adapter)))
		{
			P.adapter = adapter;
			rep("adapter.chosen_by: the first adapter EnumAdapters1 lists (%s)",
				factory6 ? "no hardware adapter by preference" : "no IDXGIFactory6");
		}
	}
	else
	{
		for (index = 0; IDXGIFactory1_EnumAdapters1(P.factory, index, &adapter) != DXGI_ERROR_NOT_FOUND; index++)
		{
			char name[256];

			adapter_text(adapter, name, sizeof(name));
			if (name_contains(name, P.adapter_asked))
			{
				P.adapter = adapter;
				rep("adapter.chosen_by: the first whose description contains \"%s\" (position %u)", P.adapter_asked,
					index);
				break;
			}
			IDXGIAdapter1_Release(adapter);
		}
	}
	release(&factory6);
	if (!P.is_warp && !P.adapter)
	{
		rep("adapter.chosen: none matches \"%s\"", P.adapter_asked);
		P.failed = 1;
		return 0;
	}
	return 1;
}

static int create_device(void)
{
	static const D3D_FEATURE_LEVEL levels[] = {
		D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
	};
	D3D_DRIVER_TYPE type = P.is_warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN;
	IDXGIAdapter *adapter = P.adapter ? (IDXGIAdapter *)P.adapter : NULL;
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	HRESULT result, debug_result;
	double start;

	P.available_virtual_before = available_virtual();
	report_address_space("address_space.before_device");
	start = now_ms();
	debug_result = D3D11CreateDevice(adapter, type, NULL, flags | D3D11_CREATE_DEVICE_DEBUG, levels,
		sizeof(levels) / sizeof(levels[0]), D3D11_SDK_VERSION, &P.device, &P.level, &P.context);
	if (SUCCEEDED(debug_result))
	{
		P.debug_layer = 1;
		result = debug_result;
	}
	else
	{
		result = D3D11CreateDevice(adapter, type, NULL, flags, levels, sizeof(levels) / sizeof(levels[0]),
			D3D11_SDK_VERSION, &P.device, &P.level, &P.context);
	}
	rep("device.create_time: %.3f ms (both attempts)", now_ms() - start);
	rep("debug_layer: %s (%s)", yesno(P.debug_layer), hr_text(debug_result));
	if (FAILED(result))
		return step_failed("setup", "D3D11CreateDevice", result);
	rep("device.feature_level: %s", level_name(P.level));
	report_address_space("address_space.after_device");
	rep("address_space.device_cost: %.1f MB",
		(double)((long long)P.available_virtual_before - (long long)available_virtual()) / 1048576.0);

	/* the adapter the device is on: for WARP, the only way to its name */
	{
		IDXGIDevice *dxgi = NULL;
		IDXGIAdapter *on = NULL;
		DXGI_ADAPTER_DESC description;

		if (SUCCEEDED(ID3D11Device_QueryInterface(P.device, &IID_IDXGIDevice, (void **)&dxgi)) &&
			SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &on)) && SUCCEEDED(IDXGIAdapter_GetDesc(on, &description)))
		{
			char name[256];

			WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof(name), NULL, NULL);
			rep("device.adapter: %s (vendor 0x%04x, device 0x%04x)", name, description.VendorId, description.DeviceId);
			if (description.VendorId == 0x5143 || description.VendorId == 0x4D4F4351)
				rep("device.on_qualcomm: yes");
			else if (description.VendorId == 0x1414)
				rep("device.on_qualcomm: no (Microsoft: %s)", name);
			else
				rep("device.on_qualcomm: no");
		}
		release(&on);
		release(&dxgi);
	}

	if (P.debug_layer &&
		SUCCEEDED(ID3D11Device_QueryInterface(P.device, &IID_ID3D11InfoQueue, (void **)&P.info)))
	{
		ID3D11InfoQueue_SetMessageCountLimit(P.info, 65536);
		drain_debug("setup");
	}
	return 1;
}

/* ---------- small things the steps share */

static HRESULT compile_text(const char *text, size_t length, const char *name, const char *entry, const char *target,
	UINT flags, ID3DBlob **code, char *errors, size_t errors_size)
{
	ID3DBlob *messages = NULL;
	HRESULT result;

	if (errors_size)
		errors[0] = 0;
	if (!P.compile)
		return E_NOTIMPL;
	result = P.compile(text, length, name, NULL, NULL, entry, target, flags, 0, code, &messages);
	if (messages)
	{
		if (errors_size)
		{
			size_t index, out = 0, size = ID3D10Blob_GetBufferSize(messages);
			const char *in = ID3D10Blob_GetBufferPointer(messages);

			/* on one line */
			for (index = 0; index < size && in[index] && out + 3 < errors_size; index++)
			{
				if (in[index] == '\n')
				{
					errors[out++] = ' ';
					errors[out++] = '|';
					errors[out++] = ' ';
				}
				else if (in[index] != '\r')
				{
					errors[out++] = in[index];
				}
			}
			errors[out] = 0;
		}
		ID3D10Blob_Release(messages);
	}
	return result;
}

static int load_compiler(void)
{
	double start;
	char path[MAX_PATH];

	if (P.compile)
		return 1;
	start = now_ms();
	P.compiler = LoadLibraryA("d3dcompiler_47.dll");
	if (P.compiler)
		P.compile = (pD3DCompile)(void *)GetProcAddress(P.compiler, "D3DCompile");
	rep("compiler.load: %.3f ms", now_ms() - start);
	if (!P.compile)
	{
		rep("compiler.loaded: no (%lu)", GetLastError());
		return 0;
	}
	if (GetModuleFileNameA(P.compiler, path, sizeof(path)))
	{
		DWORD handle, size = GetFileVersionInfoSizeA(path, &handle);
		void *data = size ? malloc(size) : NULL;
		VS_FIXEDFILEINFO *fixed;
		UINT length;

		rep("compiler.path: %s", path);
		if (data && GetFileVersionInfoA(path, 0, size, data) && VerQueryValueA(data, "\\", (void **)&fixed, &length))
			rep("compiler.version: %u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
				HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
		free(data);
	}
	return 1;
}

/* the built-in pipeline the memory and query steps draw with: a position in clip space and a colour */
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

/* 16 bytes a vertex: 65,536 is a whole number of them */
struct vertex
{
	float x, y;
	uint32_t colour; /* R8G8B8A8_UNORM: red in the low byte */
	uint32_t pad;
};

#define RED 0xff0000ffu
#define GREEN 0xff00ff00u

struct pipeline
{
	ID3D11VertexShader *vs;
	ID3D11PixelShader *ps;
	ID3D11InputLayout *layout;
};

static int pipeline_make(struct pipeline *pipeline, const char *step)
{
	static const D3D11_INPUT_ELEMENT_DESC elements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	ID3DBlob *vs = NULL, *ps = NULL;
	char errors[1024];
	HRESULT result;
	int ok = 0;

	memset(pipeline, 0, sizeof(*pipeline));
	if (!load_compiler())
		return step_failed(step, "load_compiler", E_FAIL);
	result = compile_text(colour_hlsl, sizeof(colour_hlsl) - 1, "colour", "vs_main", "vs_4_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3, &vs, errors, sizeof(errors));
	if (FAILED(result))
	{
		rep("%s.pipeline.vs: %s", step, errors);
		step_failed(step, "compile colour vs", result);
		goto done;
	}
	result = compile_text(colour_hlsl, sizeof(colour_hlsl) - 1, "colour", "ps_main", "ps_4_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3, &ps, errors, sizeof(errors));
	if (FAILED(result))
	{
		rep("%s.pipeline.ps: %s", step, errors);
		step_failed(step, "compile colour ps", result);
		goto done;
	}
	CHECK(step, ID3D11Device_CreateVertexShader(P.device, ID3D10Blob_GetBufferPointer(vs),
		ID3D10Blob_GetBufferSize(vs), NULL, &pipeline->vs));
	CHECK(step, ID3D11Device_CreatePixelShader(P.device, ID3D10Blob_GetBufferPointer(ps),
		ID3D10Blob_GetBufferSize(ps), NULL, &pipeline->ps));
	CHECK(step, ID3D11Device_CreateInputLayout(P.device, elements, 2, ID3D10Blob_GetBufferPointer(vs),
		ID3D10Blob_GetBufferSize(vs), &pipeline->layout));
	ok = 1;
done:
	release(&vs);
	release(&ps);
	return ok;
}

static void pipeline_free(struct pipeline *pipeline)
{
	release(&pipeline->layout);
	release(&pipeline->ps);
	release(&pipeline->vs);
}

static void pipeline_bind(const struct pipeline *pipeline)
{
	ID3D11DeviceContext_IASetInputLayout(P.context, pipeline->layout);
	ID3D11DeviceContext_IASetPrimitiveTopology(P.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_VSSetShader(P.context, pipeline->vs, NULL, 0);
	ID3D11DeviceContext_PSSetShader(P.context, pipeline->ps, NULL, 0);
}

/* an offscreen colour target, RGBA8 */
struct target
{
	ID3D11Texture2D *texture;
	ID3D11RenderTargetView *view;
	UINT width, height;
};

static HRESULT target_make(struct target *target, UINT width, UINT height)
{
	D3D11_TEXTURE2D_DESC description;
	HRESULT result;

	memset(target, 0, sizeof(*target));
	memset(&description, 0, sizeof(description));
	description.Width = width;
	description.Height = height;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = D3D11_BIND_RENDER_TARGET;
	result = ID3D11Device_CreateTexture2D(P.device, &description, NULL, &target->texture);
	if (SUCCEEDED(result))
		result = ID3D11Device_CreateRenderTargetView(P.device, (ID3D11Resource *)target->texture, NULL, &target->view);
	target->width = width;
	target->height = height;
	return result;
}

static void target_free(struct target *target)
{
	release(&target->view);
	release(&target->texture);
}

static void target_bind(const struct target *target)
{
	D3D11_VIEWPORT viewport = { 0.0f, 0.0f, (float)target->width, (float)target->height, 0.0f, 1.0f };

	ID3D11DeviceContext_OMSetRenderTargets(P.context, 1, &target->view, NULL);
	ID3D11DeviceContext_RSSetViewports(P.context, 1, &viewport);
}

static void target_clear(const struct target *target)
{
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	ID3D11DeviceContext_ClearRenderTargetView(P.context, target->view, black);
}

/* the pixel at (x, y) of a target, read back through a staging texture; 0 if it could not be read */
static uint32_t target_read(const struct target *target, UINT x, UINT y, HRESULT *result_out)
{
	D3D11_TEXTURE2D_DESC description;
	ID3D11Texture2D *staging = NULL;
	D3D11_MAPPED_SUBRESOURCE mapped;
	uint32_t value = 0;
	HRESULT result;

	ID3D11Texture2D_GetDesc(target->texture, &description);
	description.Usage = D3D11_USAGE_STAGING;
	description.BindFlags = 0;
	description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	result = ID3D11Device_CreateTexture2D(P.device, &description, NULL, &staging);
	if (SUCCEEDED(result))
	{
		ID3D11DeviceContext_CopyResource(P.context, (ID3D11Resource *)staging, (ID3D11Resource *)target->texture);
		result = ID3D11DeviceContext_Map(P.context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
		if (SUCCEEDED(result))
		{
			value = *(const uint32_t *)((const char *)mapped.pData + y * mapped.RowPitch + x * 4);
			ID3D11DeviceContext_Unmap(P.context, (ID3D11Resource *)staging, 0);
		}
	}
	release(&staging);
	if (result_out)
		*result_out = result;
	return value;
}

static HRESULT buffer_make(ID3D11Buffer **buffer, UINT size, D3D11_USAGE usage, UINT bind, UINT cpu, const void *data)
{
	D3D11_BUFFER_DESC description;
	D3D11_SUBRESOURCE_DATA initial;

	memset(&description, 0, sizeof(description));
	description.ByteWidth = size;
	description.Usage = usage;
	description.BindFlags = bind;
	description.CPUAccessFlags = cpu;
	initial.pSysMem = data;
	initial.SysMemPitch = 0;
	initial.SysMemSlicePitch = 0;
	return ID3D11Device_CreateBuffer(P.device, &description, data ? &initial : NULL, buffer);
}

/* a GPU time: a disjoint query around two timestamps */
struct gpu_timer
{
	ID3D11Query *disjoint, *begin, *end;
};

static HRESULT gpu_timer_make(struct gpu_timer *timer)
{
	D3D11_QUERY_DESC description = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
	HRESULT result;

	memset(timer, 0, sizeof(*timer));
	result = ID3D11Device_CreateQuery(P.device, &description, &timer->disjoint);
	description.Query = D3D11_QUERY_TIMESTAMP;
	if (SUCCEEDED(result))
		result = ID3D11Device_CreateQuery(P.device, &description, &timer->begin);
	if (SUCCEEDED(result))
		result = ID3D11Device_CreateQuery(P.device, &description, &timer->end);
	return result;
}

static void gpu_timer_free(struct gpu_timer *timer)
{
	release(&timer->end);
	release(&timer->begin);
	release(&timer->disjoint);
}

static void gpu_timer_begin(struct gpu_timer *timer)
{
	ID3D11DeviceContext_Begin(P.context, (ID3D11Asynchronous *)timer->disjoint);
	ID3D11DeviceContext_End(P.context, (ID3D11Asynchronous *)timer->begin);
}

static void gpu_timer_end(struct gpu_timer *timer)
{
	ID3D11DeviceContext_End(P.context, (ID3D11Asynchronous *)timer->end);
	ID3D11DeviceContext_End(P.context, (ID3D11Asynchronous *)timer->disjoint);
}

/* waits for the result: milliseconds, or -1 if the counter was disjoint or did not answer */
static double gpu_timer_read(struct gpu_timer *timer)
{
	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
	UINT64 begin, end;
	double start = now_ms();

	while (ID3D11DeviceContext_GetData(P.context, (ID3D11Asynchronous *)timer->disjoint, &disjoint,
		sizeof(disjoint), 0) == S_FALSE)
	{
		if (now_ms() - start > 5000.0)
			return -1.0;
		Sleep(0);
	}
	if (disjoint.Disjoint || !disjoint.Frequency)
		return -1.0;
	if (ID3D11DeviceContext_GetData(P.context, (ID3D11Asynchronous *)timer->begin, &begin, sizeof(begin), 0) != S_OK ||
		ID3D11DeviceContext_GetData(P.context, (ID3D11Asynchronous *)timer->end, &end, sizeof(end), 0) != S_OK)
		return -1.0;
	return (double)(end - begin) * 1000.0 / (double)disjoint.Frequency;
}

/* waits until the GPU has done everything submitted so far */
static void wait_idle(void)
{
	D3D11_QUERY_DESC description = { D3D11_QUERY_EVENT, 0 };
	ID3D11Query *event = NULL;
	BOOL done = FALSE;
	double start = now_ms();

	if (FAILED(ID3D11Device_CreateQuery(P.device, &description, &event)))
		return;
	ID3D11DeviceContext_End(P.context, (ID3D11Asynchronous *)event);
	while (ID3D11DeviceContext_GetData(P.context, (ID3D11Asynchronous *)event, &done, sizeof(done), 0) == S_FALSE &&
		now_ms() - start < 10000.0)
		Sleep(0);
	release(&event);
}

/* at most three frames queued, as a swap chain allows: frame n waits for frame n - 3 */
#define FRAME_LATENCY 3

struct frame_pacer
{
	ID3D11Query *events[FRAME_LATENCY];
	int frame;
};

static HRESULT pacer_make(struct frame_pacer *pacer)
{
	D3D11_QUERY_DESC description = { D3D11_QUERY_EVENT, 0 };
	HRESULT result = S_OK;
	int index;

	memset(pacer, 0, sizeof(*pacer));
	for (index = 0; index < FRAME_LATENCY && SUCCEEDED(result); index++)
		result = ID3D11Device_CreateQuery(P.device, &description, &pacer->events[index]);
	return result;
}

static void pacer_wait(struct frame_pacer *pacer)
{
	BOOL done;

	if (pacer->frame >= FRAME_LATENCY)
	{
		while (ID3D11DeviceContext_GetData(P.context, (ID3D11Asynchronous *)pacer->events[pacer->frame % FRAME_LATENCY],
			&done, sizeof(done), 0) == S_FALSE)
			Sleep(0);
	}
}

static void pacer_end(struct frame_pacer *pacer)
{
	ID3D11DeviceContext_End(P.context, (ID3D11Asynchronous *)pacer->events[pacer->frame % FRAME_LATENCY]);
	ID3D11DeviceContext_Flush(P.context);
	pacer->frame++;
}

static void pacer_free(struct frame_pacer *pacer)
{
	int index;

	for (index = 0; index < FRAME_LATENCY; index++)
		release(&pacer->events[index]);
}

/* ---------- step 1: what the adapter is (caps) */

static const struct { DXGI_FORMAT format; const char *name; } formats[] = {
	{ DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM" },
	{ DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM" },
	{ DXGI_FORMAT_B8G8R8X8_UNORM, "B8G8R8X8_UNORM" },
	{ DXGI_FORMAT_B5G6R5_UNORM, "B5G6R5_UNORM" },
	{ DXGI_FORMAT_B5G5R5A1_UNORM, "B5G5R5A1_UNORM" },
	{ DXGI_FORMAT_B4G4R4A4_UNORM, "B4G4R4A4_UNORM" },
	{ DXGI_FORMAT_R8_UNORM, "R8_UNORM" },
	{ DXGI_FORMAT_R8G8_UNORM, "R8G8_UNORM" },
	{ DXGI_FORMAT_A8_UNORM, "A8_UNORM" },
	{ DXGI_FORMAT_R16_UNORM, "R16_UNORM" },
	{ DXGI_FORMAT_R16G16_UNORM, "R16G16_UNORM" },
	{ DXGI_FORMAT_R8G8_SNORM, "R8G8_SNORM" },
	{ DXGI_FORMAT_R16G16_SNORM, "R16G16_SNORM" },
	{ DXGI_FORMAT_BC1_UNORM, "BC1_UNORM" },
	{ DXGI_FORMAT_BC2_UNORM, "BC2_UNORM" },
	{ DXGI_FORMAT_BC3_UNORM, "BC3_UNORM" },
	{ DXGI_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT" },
	{ DXGI_FORMAT_D32_FLOAT_S8X24_UINT, "D32_FLOAT_S8X24_UINT" },
	{ DXGI_FORMAT_D16_UNORM, "D16_UNORM" },
	{ DXGI_FORMAT_D32_FLOAT, "D32_FLOAT" },
	{ DXGI_FORMAT_R16_UINT, "R16_UINT" },
	/* the vertex formats attribute_format (d3d8_gl.c) maps the declarations' types to */
	{ DXGI_FORMAT_R32_FLOAT, "R32_FLOAT" },
	{ DXGI_FORMAT_R32G32_FLOAT, "R32G32_FLOAT" },
	{ DXGI_FORMAT_R32G32B32_FLOAT, "R32G32B32_FLOAT" },
	{ DXGI_FORMAT_R32G32B32A32_FLOAT, "R32G32B32A32_FLOAT" },
	{ DXGI_FORMAT_R16_SINT, "R16_SINT" },
	{ DXGI_FORMAT_R16G16_SINT, "R16G16_SINT" },
	{ DXGI_FORMAT_R16G16B16A16_SINT, "R16G16B16A16_SINT" },
	{ DXGI_FORMAT_R16_SNORM, "R16_SNORM" },
	{ DXGI_FORMAT_R16G16B16A16_SNORM, "R16G16B16A16_SNORM" },
	{ DXGI_FORMAT_R8G8B8A8_UINT, "R8G8B8A8_UINT" },
	{ DXGI_FORMAT_R32_UINT, "R32_UINT" },
};

static const struct { UINT bit; const char *name; } format_bits[] = {
	{ D3D11_FORMAT_SUPPORT_TEXTURE2D, "texture2d" },
	{ D3D11_FORMAT_SUPPORT_TEXTURE3D, "texture3d" },
	{ D3D11_FORMAT_SUPPORT_TEXTURECUBE, "texturecube" },
	{ D3D11_FORMAT_SUPPORT_SHADER_SAMPLE, "shader_sample" },
	{ D3D11_FORMAT_SUPPORT_MIP_AUTOGEN, "mip_autogen" },
	{ D3D11_FORMAT_SUPPORT_RENDER_TARGET, "render_target" },
	{ D3D11_FORMAT_SUPPORT_BLENDABLE, "blendable" },
	{ D3D11_FORMAT_SUPPORT_DEPTH_STENCIL, "depth_stencil" },
	{ D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET, "multisample_rendertarget" },
	{ D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE, "multisample_resolve" },
	{ D3D11_FORMAT_SUPPORT_DISPLAY, "display" },
	{ D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER, "ia_vertex_buffer" },
	{ D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER, "ia_index_buffer" },
};

static void caps_formats(void)
{
	size_t index, bit;

	for (index = 0; index < sizeof(formats) / sizeof(formats[0]); index++)
	{
		UINT support = 0;
		HRESULT result = ID3D11Device_CheckFormatSupport(P.device, formats[index].format, &support);
		char text[512];
		size_t length = 0;

		text[0] = 0;
		if (FAILED(result))
		{
			rep("caps.format.%s: not supported (%s)", formats[index].name, hr_text(result));
			continue;
		}
		for (bit = 0; bit < sizeof(format_bits) / sizeof(format_bits[0]); bit++)
			if (support & format_bits[bit].bit)
				length += (size_t)snprintf(text + length, sizeof(text) - length, "%s%s", length ? " " : "",
					format_bits[bit].name);
		rep("caps.format.%s: %s", formats[index].name, length ? text : "(none of the bits asked about)");
	}
	{
		static const DXGI_FORMAT msaa[] = { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
			DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_D32_FLOAT };
		static const char *const msaa_names[] = { "B8G8R8A8_UNORM", "R8G8B8A8_UNORM", "D24_UNORM_S8_UINT",
			"D32_FLOAT_S8X24_UINT", "D32_FLOAT" };
		static const UINT samples[] = { 2, 4, 8 };

		for (index = 0; index < sizeof(msaa) / sizeof(msaa[0]); index++)
		{
			UINT quality[3] = { 0, 0, 0 };

			for (bit = 0; bit < 3; bit++)
				if (FAILED(ID3D11Device_CheckMultisampleQualityLevels(P.device, msaa[index], samples[bit], &quality[bit])))
					quality[bit] = 0;
			rep("caps.msaa.%s: 2x %u levels, 4x %u levels, 8x %u levels", msaa_names[index], quality[0], quality[1],
				quality[2]);
		}
	}
}

static void step_caps(void)
{
	void *pointer;
	HRESULT result;

	rep("step.caps: start");
	rep("caps.feature_level: %s", level_name(P.level));
	rep("caps.feature_level_11_0_or_better: %s", yesno(P.level >= D3D_FEATURE_LEVEL_11_0));

	/* the runtime's interfaces: which version of Direct3D 11 it offers */
#define HAS_INTERFACE(object, iid, key) \
	pointer = NULL; \
	result = IUnknown_QueryInterface((IUnknown *)(object), &iid, &pointer); \
	rep("caps.interface.%s: %s", key, yesno(SUCCEEDED(result))); \
	if (pointer) IUnknown_Release((IUnknown *)pointer);
	HAS_INTERFACE(P.device, IID_ID3D11Device1, "ID3D11Device1")
	HAS_INTERFACE(P.device, IID_ID3D11Device2, "ID3D11Device2")
	HAS_INTERFACE(P.device, IID_ID3D11Device3, "ID3D11Device3")
	HAS_INTERFACE(P.device, IID_ID3D11Device4, "ID3D11Device4")
	HAS_INTERFACE(P.device, IID_ID3D11Device5, "ID3D11Device5")
	HAS_INTERFACE(P.context, IID_ID3D11DeviceContext1, "ID3D11DeviceContext1")
	HAS_INTERFACE(P.context, IID_ID3D11DeviceContext2, "ID3D11DeviceContext2")
	HAS_INTERFACE(P.context, IID_ID3D11DeviceContext3, "ID3D11DeviceContext3")
	HAS_INTERFACE(P.context, IID_ID3D11DeviceContext4, "ID3D11DeviceContext4")
	HAS_INTERFACE(P.factory, IID_IDXGIFactory2, "IDXGIFactory2")
	HAS_INTERFACE(P.factory, IID_IDXGIFactory5, "IDXGIFactory5")
	HAS_INTERFACE(P.factory, IID_IDXGIFactory6, "IDXGIFactory6")
#undef HAS_INTERFACE

	/* the options */
	{
		D3D11_FEATURE_DATA_THREADING threading;

		result = ID3D11Device_CheckFeatureSupport(P.device, D3D11_FEATURE_THREADING, &threading, sizeof(threading));
		if (SUCCEEDED(result))
		{
			rep("caps.threading.concurrent_creates: %s", yesno(threading.DriverConcurrentCreates));
			rep("caps.threading.command_lists: %s", yesno(threading.DriverCommandLists));
		}
		else
		{
			rep("caps.threading: not given (%s)", hr_text(result));
		}
	}
	{
		D3D11_FEATURE_DATA_D3D11_OPTIONS options;

		result = ID3D11Device_CheckFeatureSupport(P.device, D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));
		if (SUCCEEDED(result))
		{
			rep("caps.options.map_no_overwrite_on_dynamic_constant_buffer: %s",
				yesno(options.MapNoOverwriteOnDynamicConstantBuffer));
			rep("caps.options.map_no_overwrite_on_dynamic_buffer_srv: %s",
				yesno(options.MapNoOverwriteOnDynamicBufferSRV));
			rep("caps.options.constant_buffer_offsetting: %s", yesno(options.ConstantBufferOffsetting));
			rep("caps.options.constant_buffer_partial_update: %s", yesno(options.ConstantBufferPartialUpdate));
			rep("caps.options.output_merger_logic_op: %s", yesno(options.OutputMergerLogicOp));
			rep("caps.options.discard_apis_seen_by_driver: %s", yesno(options.DiscardAPIsSeenByDriver));
			rep("caps.options.flags_for_update_and_copy_seen_by_driver: %s",
				yesno(options.FlagsForUpdateAndCopySeenByDriver));
			rep("caps.options.clear_view: %s", yesno(options.ClearView));
			rep("caps.options.copy_with_overlap: %s", yesno(options.CopyWithOverlap));
			rep("caps.options.multisample_rtv_with_forced_sample_count_one: %s",
				yesno(options.MultisampleRTVWithForcedSampleCountOne));
		}
		else
		{
			rep("caps.options: not given (%s)", hr_text(result));
		}
	}
	{
		D3D11_FEATURE_DATA_D3D11_OPTIONS1 options1;

		result = ID3D11Device_CheckFeatureSupport(P.device, D3D11_FEATURE_D3D11_OPTIONS1, &options1, sizeof(options1));
		if (SUCCEEDED(result))
			rep("caps.options1.tiled_resources_tier: %d", (int)options1.TiledResourcesTier);
		else
			rep("caps.options1: not given (%s)", hr_text(result));
	}
	{
		D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2;

		result = ID3D11Device_CheckFeatureSupport(P.device, D3D11_FEATURE_D3D11_OPTIONS2, &options2, sizeof(options2));
		if (SUCCEEDED(result))
		{
			rep("caps.options2.map_on_default_textures: %s", yesno(options2.MapOnDefaultTextures));
			rep("caps.options2.unified_memory_architecture: %s", yesno(options2.UnifiedMemoryArchitecture));
			rep("caps.options2.typed_uav_load_additional_formats: %s", yesno(options2.TypedUAVLoadAdditionalFormats));
			rep("caps.options2.rovs_supported: %s", yesno(options2.ROVsSupported));
			rep("caps.options2.standard_swizzle: %s", yesno(options2.StandardSwizzle));
		}
		else
		{
			rep("caps.options2: not given (%s)", hr_text(result));
		}
	}
	{
		D3D11_FEATURE_DATA_D3D9_OPTIONS1 options9;

		result = ID3D11Device_CheckFeatureSupport(P.device, D3D11_FEATURE_D3D9_OPTIONS1, &options9, sizeof(options9));
		if (SUCCEEDED(result))
		{
			rep("caps.d3d9_options1.full_non_pow2_texture_supported: %s",
				yesno(options9.FullNonPow2TextureSupported));
			rep("caps.d3d9_options1.depth_as_texture_with_less_equal_comparison_filter: %s",
				yesno(options9.DepthAsTextureWithLessEqualComparisonFilterSupported));
			rep("caps.d3d9_options1.simple_instancing: %s", yesno(options9.SimpleInstancingSupported));
			rep("caps.d3d9_options1.texture_cube_face_render_target_with_non_cube_depth_stencil: %s",
				yesno(options9.TextureCubeFaceRenderTargetWithNonCubeDepthStencilSupported));
		}
		else
		{
			rep("caps.d3d9_options1: not given (%s)", hr_text(result));
		}
	}

	caps_formats();

	/* the limits: fixed by the feature level, named for the record */
	{
		int fl11 = P.level >= D3D_FEATURE_LEVEL_11_0;

		rep("caps.limit.constant_buffer_float4s: %d (vertex constants need 192)", D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT);
		rep("caps.limit.constant_buffer_slots: %d", D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT);
		rep("caps.limit.input_slots: %d (the Xbox has 16)",
			fl11 ? D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT : D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT);
		rep("caps.limit.input_elements: %d",
			fl11 ? D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT : D3D10_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT);
		rep("caps.limit.sampler_slots: %d", D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
		rep("caps.limit.shader_resource_slots: %d", D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
		rep("caps.limit.render_targets: %d", D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
		rep("caps.limit.texture2d_dimension: %d",
			fl11 ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION : D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION);
		rep("caps.limit.texture3d_dimension: %d", D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION);
		rep("caps.limit.texturecube_dimension: %d",
			fl11 ? D3D11_REQ_TEXTURECUBE_DIMENSION : D3D10_REQ_TEXTURECUBE_DIMENSION);
		rep("caps.limit.max_anisotropy: %d", D3D11_REQ_MAXANISOTROPY);
		rep("caps.limit.resource_size_mb: %d (D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM)",
			D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM);
		rep("caps.limit.draw_vertex_count_bits: %d", D3D11_REQ_DRAW_VERTEX_COUNT_2_TO_EXP);
		rep("caps.limit.draw_indexed_index_count_bits: %d", D3D11_REQ_DRAWINDEXED_INDEX_COUNT_2_TO_EXP);
		rep("caps.limit.mip_levels: %d", D3D11_REQ_MIP_LEVELS);
		rep("caps.limit.texture2d_array_size: %d", D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION);
	}
	report_address_space("caps.address_space");
	rep("step.caps: done");
}

/* ---------- step 3: the shader compiler (compile) */

#define COMPILE_RUNS 10

struct shader_file
{
	char name[MAX_PATH];
	char *text;
	size_t length;
	int pixel;
};

static char *read_file(const char *path, size_t *length)
{
	FILE *file = fopen(path, "rb");
	char *text;
	long size;

	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	text = calloc(1, (size_t)size + 1);
	if (size > 0 && fread(text, 1, (size_t)size, file) != (size_t)size)
	{
		free(text);
		fclose(file);
		return NULL;
	}
	fclose(file);
	if (length)
		*length = (size_t)size;
	return text;
}

static int compare_shader_files(const void *a, const void *b)
{
	return strcmp(((const struct shader_file *)a)->name, ((const struct shader_file *)b)->name);
}

/* every vs_*.hlsl and ps_*.hlsl in the shaders folder */
static int find_shaders(struct shader_file *files, int capacity)
{
	WIN32_FIND_DATAA found;
	char pattern[MAX_PATH + 16];
	HANDLE search;
	int count = 0;

	snprintf(pattern, sizeof(pattern), "%s\\*.hlsl", P.shaders_dir);
	search = FindFirstFileA(pattern, &found);
	if (search == INVALID_HANDLE_VALUE)
		return 0;
	do
	{
		char path[MAX_PATH * 2];
		int pixel;

		if (!strncmp(found.cFileName, "vs_", 3))
			pixel = 0;
		else if (!strncmp(found.cFileName, "ps_", 3))
			pixel = 1;
		else
			continue;
		if (count >= capacity)
			break;
		snprintf(path, sizeof(path), "%s\\%s", P.shaders_dir, found.cFileName);
		files[count].text = read_file(path, &files[count].length);
		if (!files[count].text)
		{
			rep("compile.read: %s failed", path);
			continue;
		}
		snprintf(files[count].name, sizeof(files[count].name), "%s", found.cFileName);
		files[count].pixel = pixel;
		count++;
	} while (FindNextFileA(search, &found));
	FindClose(search);
	qsort(files, (size_t)count, sizeof(files[0]), compare_shader_files);
	return count;
}

static void write_blob(const char *name, const char *target, ID3DBlob *blob)
{
	char path[MAX_PATH * 2];
	FILE *file;

	snprintf(path, sizeof(path), "%s\\d3d11_probe_shaders", P.report_dir);
	CreateDirectoryA(path, NULL);
	snprintf(path, sizeof(path), "%s\\d3d11_probe_shaders\\%.*s.%s.dxbc", P.report_dir,
		(int)(strlen(name) > 5 ? strlen(name) - 5 : strlen(name)), name, target);
	file = fopen(path, "wb");
	if (file)
	{
		fwrite(ID3D10Blob_GetBufferPointer(blob), 1, ID3D10Blob_GetBufferSize(blob), file);
		fclose(file);
	}
}

/* ten compiles of each shader at one model, and the shader object made from each blob */
static void compile_model(struct shader_file *files, int count, const char *model, UINT flags, const char *label,
	int *first_done)
{
	int index, run;

	for (index = 0; index < count; index++)
	{
		double wall[COMPILE_RUNS], cpu[COMPILE_RUNS], create[COMPILE_RUNS];
		char target[16], name[64], key[MAX_PATH + 64], errors[2048];
		size_t size = 0;
		int done = 0;

		snprintf(target, sizeof(target), "%s_%s", files[index].pixel ? "ps" : "vs", model);
		snprintf(name, sizeof(name), "%s%s", target, label);
		for (run = 0; run < COMPILE_RUNS; run++)
		{
			ID3DBlob *blob = NULL;
			double start = now_ms(), start_cpu = cpu_ms();
			HRESULT result = compile_text(files[index].text, files[index].length, files[index].name, "main", target,
				flags, &blob, errors, sizeof(errors));

			wall[run] = now_ms() - start;
			cpu[run] = cpu_ms() - start_cpu;
			if (FAILED(result))
			{
				rep("compile.%s.%s: failed (%s): %s", files[index].name, name, hr_text(result), errors);
				P.failed = 1;
				release(&blob);
				break;
			}
			if (!*first_done)
			{
				rep("compile.first_of_process: %s %s, %.3f ms wall, %.3f ms thread CPU", files[index].name, name,
					wall[run], cpu[run]);
				*first_done = 1;
			}
			if (run == 0 && errors[0])
				rep("compile.%s.%s.warnings: %s", files[index].name, name, errors);
			size = ID3D10Blob_GetBufferSize(blob);
			start = now_ms();
			if (files[index].pixel)
			{
				ID3D11PixelShader *shader = NULL;

				result = ID3D11Device_CreatePixelShader(P.device, ID3D10Blob_GetBufferPointer(blob), size, NULL, &shader);
				release(&shader);
			}
			else
			{
				ID3D11VertexShader *shader = NULL;

				result = ID3D11Device_CreateVertexShader(P.device, ID3D10Blob_GetBufferPointer(blob), size, NULL,
					&shader);
				release(&shader);
			}
			create[run] = now_ms() - start;
			if (FAILED(result))
			{
				rep("compile.%s.%s.create: failed (%s)", files[index].name, name, hr_text(result));
				P.failed = 1;
			}
			if (run == 0)
				write_blob(files[index].name, name, blob);
			release(&blob);
			done++;
		}
		if (done < COMPILE_RUNS)
			continue;
		rep("compile.%s.%s.source_bytes: %u", files[index].name, name, (unsigned)files[index].length);
		rep("compile.%s.%s.bytecode_bytes: %u", files[index].name, name, (unsigned)size);
		snprintf(key, sizeof(key), "compile.%s.%s.wall", files[index].name, name);
		rep_times(key, wall, COMPILE_RUNS);
		snprintf(key, sizeof(key), "compile.%s.%s.thread_cpu", files[index].name, name);
		rep_times(key, cpu, COMPILE_RUNS);
		snprintf(key, sizeof(key), "compile.%s.%s.create_shader", files[index].name, name);
		rep_times(key, create, COMPILE_RUNS);
	}
}

/* whether `precise` on the position compiles at vs_4_0 (the generators' invariant gl_Position) */
static void compile_precise(void)
{
	static const char member[] =
		"cbuffer constants : register(b0) { float4 c[192]; };\n"
		"struct vs_out { precise float4 position : SV_Position; float4 t0 : TEXCOORD0; };\n"
		"vs_out main(float4 v0 : TEXCOORD0)\n"
		"{\n"
		"	vs_out o;\n"
		"	o.position = float4(dot(v0, c[0]), dot(v0, c[1]), dot(v0, c[2]), dot(v0, c[3]));\n"
		"	o.t0 = v0;\n"
		"	return o;\n"
		"}\n";
	static const char local[] =
		"cbuffer constants : register(b0) { float4 c[192]; };\n"
		"struct vs_out { float4 position : SV_Position; float4 t0 : TEXCOORD0; };\n"
		"vs_out main(float4 v0 : TEXCOORD0)\n"
		"{\n"
		"	vs_out o;\n"
		"	precise float4 r = float4(dot(v0, c[0]), dot(v0, c[1]), dot(v0, c[2]), dot(v0, c[3]));\n"
		"	o.position = r;\n"
		"	o.t0 = v0;\n"
		"	return o;\n"
		"}\n";
	static const char plain[] =
		"cbuffer constants : register(b0) { float4 c[192]; };\n"
		"struct vs_out { float4 position : SV_Position; float4 t0 : TEXCOORD0; };\n"
		"vs_out main(float4 v0 : TEXCOORD0)\n"
		"{\n"
		"	vs_out o;\n"
		"	o.position = float4(dot(v0, c[0]), dot(v0, c[1]), dot(v0, c[2]), dot(v0, c[3]));\n"
		"	o.t0 = v0;\n"
		"	return o;\n"
		"}\n";
	static const struct { const char *name; const char *text; size_t length; UINT flags; } cases[] = {
		{ "output_member", member, sizeof(member) - 1, D3DCOMPILE_OPTIMIZATION_LEVEL3 },
		{ "local_variable", local, sizeof(local) - 1, D3DCOMPILE_OPTIMIZATION_LEVEL3 },
		{ "ieee_strictness", plain, sizeof(plain) - 1, D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS },
	};
	static const char *const targets[] = { "vs_4_0", "vs_5_0" };
	size_t index, target;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
	{
		for (target = 0; target < 2; target++)
		{
			ID3DBlob *blob = NULL;
			char errors[1024];
			HRESULT result = compile_text(cases[index].text, cases[index].length, cases[index].name, "main",
				targets[target], cases[index].flags, &blob, errors, sizeof(errors));

			if (SUCCEEDED(result))
				rep("compile.precise.%s.%s: compiles (%u bytes)%s%s", cases[index].name, targets[target],
					(unsigned)ID3D10Blob_GetBufferSize(blob), errors[0] ? "; " : "", errors);
			else
				rep("compile.precise.%s.%s: fails (%s): %s", cases[index].name, targets[target], hr_text(result),
					errors);
			release(&blob);
		}
	}
}

static void step_compile(void)
{
	struct shader_file files[32];
	int count, index, first_done = 0;

	rep("step.compile: start");
	if (!load_compiler())
	{
		step_failed("compile", "LoadLibrary d3dcompiler_47.dll", E_FAIL);
		return;
	}
	memset(files, 0, sizeof(files));
	count = find_shaders(files, 32);
	rep("compile.shaders_folder: %s (%d shaders)", P.shaders_dir, count);
	if (!count)
	{
		step_failed("compile", "no vs_*.hlsl or ps_*.hlsl in the shaders folder", E_FAIL);
		return;
	}
	compile_model(files, count, "4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, "", &first_done);
	compile_model(files, count, "5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, "", &first_done);
	/* for the record: what the optimiser costs */
	compile_model(files, count, "4_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, "_o1", &first_done);
	compile_model(files, count, "4_0", D3DCOMPILE_SKIP_OPTIMIZATION, "_skip_optimization", &first_done);
	compile_precise();
	for (index = 0; index < count; index++)
		free(files[index].text);
	drain_debug("compile");
	rep("step.compile: done");
}

/* ---------- step 4: buffers (memory) */

#define FRAMES 300
#define RING_SIZE (32u * 1024u * 1024u)
#define PIECE (64u * 1024u)
#define RING_FRAME_BYTES (4u * 1024u * 1024u)
#define SEGMENT (4u * 1024u * 1024u)
#define PAGE 4096u

/* a block of vertices: whole triangles over the 16x16 target, so every draw writes every pixel */
static void fill_vertices(void *memory, size_t bytes, uint32_t colour)
{
	struct vertex *vertices = memory;
	size_t index, count = bytes / sizeof(struct vertex);

	for (index = 0; index < count; index++)
	{
		static const float corners[3][2] = { { -1.0f, -1.0f }, { -1.0f, 3.0f }, { 3.0f, -1.0f } };

		vertices[index].x = corners[index % 3][0];
		vertices[index].y = corners[index % 3][1];
		vertices[index].colour = colour;
		vertices[index].pad = 0;
	}
}

static void bind_vertices(ID3D11Buffer *buffer)
{
	UINT stride = sizeof(struct vertex), offset = 0;

	ID3D11DeviceContext_IASetVertexBuffers(P.context, 0, 1, &buffer, &stride, &offset);
}

static void report_frames(const char *key, const double *cpu, const double *gpu, int count)
{
	char name[128];

	snprintf(name, sizeof(name), "%s.cpu_per_frame", key);
	rep_spread(name, cpu, count);
	snprintf(name, sizeof(name), "%s.gpu_per_frame", key);
	rep_spread(name, gpu, count);
}

/* 4.1: the stream ring, a DYNAMIC buffer written with no-overwrite in 64 KB pieces, DISCARD at the wrap */
static void memory_ring(const struct target *target)
{
	ID3D11Buffer *ring = NULL;
	struct gpu_timer *timers = calloc(FRAMES, sizeof(*timers));
	struct frame_pacer pacer;
	double *cpu = calloc(FRAMES, sizeof(double)), *gpu = calloc(FRAMES, sizeof(double)), *maps = NULL;
	void *source = malloc(PIECE);
	UINT offset = RING_SIZE;
	int frame, piece, discards = 0, map_count = 0;
	double start;

	memset(&pacer, 0, sizeof(pacer));
	maps = calloc((size_t)FRAMES * (RING_FRAME_BYTES / PIECE), sizeof(double));
	fill_vertices(source, PIECE, GREEN);
	CHECK("memory", buffer_make(&ring, RING_SIZE, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER,
		D3D11_CPU_ACCESS_WRITE, NULL));
	CHECK("memory", pacer_make(&pacer));
	for (frame = 0; frame < FRAMES; frame++)
		CHECK("memory", gpu_timer_make(&timers[frame]));
	target_bind(target);
	bind_vertices(ring);
	start = now_ms();
	for (frame = 0; frame < FRAMES; frame++)
	{
		double frame_cpu = 0.0;

		pacer_wait(&pacer);
		gpu_timer_begin(&timers[frame]);
		target_clear(target);
		for (piece = 0; piece < (int)(RING_FRAME_BYTES / PIECE); piece++)
		{
			D3D11_MAPPED_SUBRESOURCE mapped;
			D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
			double piece_start = now_ms();
			HRESULT result;

			if (offset + PIECE > RING_SIZE)
			{
				mode = D3D11_MAP_WRITE_DISCARD;
				offset = 0;
				discards++;
			}
			result = ID3D11DeviceContext_Map(P.context, (ID3D11Resource *)ring, 0, mode, 0, &mapped);
			if (FAILED(result))
			{
				step_failed("memory", "Map (ring)", result);
				goto done;
			}
			memcpy((char *)mapped.pData + offset, source, PIECE);
			ID3D11DeviceContext_Unmap(P.context, (ID3D11Resource *)ring, 0);
			maps[map_count++] = now_ms() - piece_start;
			frame_cpu += now_ms() - piece_start;
			ID3D11DeviceContext_Draw(P.context, 3, offset / sizeof(struct vertex));
			offset += PIECE;
		}
		gpu_timer_end(&timers[frame]);
		pacer_end(&pacer);
		cpu[frame] = frame_cpu;
	}
	wait_idle();
	rep("memory.ring.wall: %.1f ms for %d frames of %u KB in %u pieces (%d discards)", now_ms() - start, FRAMES,
		RING_FRAME_BYTES / 1024, RING_FRAME_BYTES / PIECE, discards);
	for (frame = 0; frame < FRAMES; frame++)
		gpu[frame] = gpu_timer_read(&timers[frame]);
	report_frames("memory.ring", cpu, gpu, FRAMES);
	rep_spread("memory.ring.map_copy_unmap_per_piece", maps, map_count);
done:
	for (frame = 0; frame < FRAMES; frame++)
		gpu_timer_free(&timers[frame]);
	pacer_free(&pacer);
	release(&ring);
	free(timers);
	free(cpu);
	free(gpu);
	free(maps);
	free(source);
}

/* 4.2: the mirror's segment, a DEFAULT buffer updated a page at a time; with staging: through STAGING buffers
and CopySubresourceRegion */
static void memory_mirror(const struct target *target, int pages_per_frame, int staging)
{
	ID3D11Buffer *segment = NULL, *stagings[FRAME_LATENCY] = { NULL };
	struct gpu_timer *timers = calloc(FRAMES, sizeof(*timers));
	struct frame_pacer pacer;
	double *cpu = calloc(FRAMES, sizeof(double)), *gpu = calloc(FRAMES, sizeof(double));
	char *source = malloc(SEGMENT);
	int frame, page, index;
	char key[96];
	double start;

	memset(&pacer, 0, sizeof(pacer));
	fill_vertices(source, SEGMENT, GREEN);
	CHECK("memory", buffer_make(&segment, SEGMENT, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, NULL));
	if (staging)
		for (index = 0; index < FRAME_LATENCY; index++)
			CHECK("memory", buffer_make(&stagings[index], SEGMENT, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_WRITE, NULL));
	CHECK("memory", pacer_make(&pacer));
	for (frame = 0; frame < FRAMES; frame++)
		CHECK("memory", gpu_timer_make(&timers[frame]));
	target_bind(target);
	bind_vertices(segment);
	start = now_ms();
	for (frame = 0; frame < FRAMES; frame++)
	{
		int first = (frame * pages_per_frame) % (int)(SEGMENT / PAGE);
		double frame_start;

		pacer_wait(&pacer);
		gpu_timer_begin(&timers[frame]);
		target_clear(target);
		frame_start = now_ms();
		if (staging)
		{
			ID3D11Buffer *buffer = stagings[frame % FRAME_LATENCY];
			D3D11_MAPPED_SUBRESOURCE mapped;
			HRESULT result = ID3D11DeviceContext_Map(P.context, (ID3D11Resource *)buffer, 0, D3D11_MAP_WRITE, 0, &mapped);

			if (FAILED(result))
			{
				step_failed("memory", "Map (staging)", result);
				goto done;
			}
			for (page = 0; page < pages_per_frame; page++)
			{
				UINT at = (UINT)((first + page) % (int)(SEGMENT / PAGE)) * PAGE;

				memcpy((char *)mapped.pData + at, source + at, PAGE);
			}
			ID3D11DeviceContext_Unmap(P.context, (ID3D11Resource *)buffer, 0);
			for (page = 0; page < pages_per_frame; page++)
			{
				UINT at = (UINT)((first + page) % (int)(SEGMENT / PAGE)) * PAGE;
				D3D11_BOX box = { at, 0, 0, at + PAGE, 1, 1 };

				ID3D11DeviceContext_CopySubresourceRegion(P.context, (ID3D11Resource *)segment, 0, at, 0, 0,
					(ID3D11Resource *)buffer, 0, &box);
			}
		}
		else
		{
			for (page = 0; page < pages_per_frame; page++)
			{
				UINT at = (UINT)((first + page) % (int)(SEGMENT / PAGE)) * PAGE;
				D3D11_BOX box = { at, 0, 0, at + PAGE, 1, 1 };

				ID3D11DeviceContext_UpdateSubresource(P.context, (ID3D11Resource *)segment, 0, &box, source + at, 0, 0);
			}
		}
		cpu[frame] = now_ms() - frame_start;
		for (page = 0; page < pages_per_frame; page++)
		{
			UINT at = (UINT)((first + page) % (int)(SEGMENT / PAGE)) * PAGE;

			ID3D11DeviceContext_Draw(P.context, 3, at / sizeof(struct vertex));
		}
		gpu_timer_end(&timers[frame]);
		pacer_end(&pacer);
	}
	wait_idle();
	snprintf(key, sizeof(key), "memory.mirror_%s_%d", staging ? "staging" : "update", pages_per_frame);
	rep("%s.wall: %.1f ms for %d frames of %d pages", key, now_ms() - start, FRAMES, pages_per_frame);
	for (frame = 0; frame < FRAMES; frame++)
		gpu[frame] = gpu_timer_read(&timers[frame]);
	report_frames(key, cpu, gpu, FRAMES);
done:
	for (frame = 0; frame < FRAMES; frame++)
		gpu_timer_free(&timers[frame]);
	pacer_free(&pacer);
	for (index = 0; index < FRAME_LATENCY; index++)
		release(&stagings[index]);
	release(&segment);
	free(timers);
	free(cpu);
	free(gpu);
	free(source);
}

/* 4.3: written red and drawn, rewritten green through the same path and drawn again in the same frame */
enum rewrite_path { REWRITE_RING_APPEND, REWRITE_RING_DISCARD, REWRITE_UPDATE, REWRITE_STAGING };

static void memory_rewrite(enum rewrite_path path)
{
	static const char *const names[] = { "ring_no_overwrite_append", "ring_discard", "mirror_update_subresource",
		"mirror_staging_copy" };
	struct target first, second;
	ID3D11Buffer *buffer = NULL, *staging = NULL;
	struct vertex source[3];
	uint32_t seen[2];
	HRESULT results[2] = { S_OK, S_OK };
	int pass;

	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	CHECK("memory", target_make(&first, 16, 16));
	CHECK("memory", target_make(&second, 16, 16));
	if (path == REWRITE_RING_APPEND || path == REWRITE_RING_DISCARD)
		CHECK("memory", buffer_make(&buffer, 64 * 1024, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER,
			D3D11_CPU_ACCESS_WRITE, NULL));
	else
		CHECK("memory", buffer_make(&buffer, 64 * 1024, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, NULL));
	if (path == REWRITE_STAGING)
		CHECK("memory", buffer_make(&staging, 64 * 1024, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_WRITE, NULL));
	bind_vertices(buffer);
	for (pass = 0; pass < 2; pass++)
	{
		const struct target *target = pass ? &second : &first;
		UINT at = 0;
		D3D11_MAPPED_SUBRESOURCE mapped;

		/* the game's memory, as the renderer would read it at the draw */
		fill_vertices(source, sizeof(source), pass ? GREEN : RED);
		switch (path)
		{
		case REWRITE_RING_APPEND:
			/* the first write starts the ring, the second goes after it */
			at = pass ? sizeof(source) : 0;
			CHECK("memory", ID3D11DeviceContext_Map(P.context, (ID3D11Resource *)buffer, 0,
				pass ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			memcpy((char *)mapped.pData + at, source, sizeof(source));
			ID3D11DeviceContext_Unmap(P.context, (ID3D11Resource *)buffer, 0);
			break;
		case REWRITE_RING_DISCARD:
			CHECK("memory", ID3D11DeviceContext_Map(P.context, (ID3D11Resource *)buffer, 0, D3D11_MAP_WRITE_DISCARD, 0,
				&mapped));
			memcpy(mapped.pData, source, sizeof(source));
			ID3D11DeviceContext_Unmap(P.context, (ID3D11Resource *)buffer, 0);
			break;
		case REWRITE_UPDATE:
			{
				D3D11_BOX box = { 0, 0, 0, sizeof(source), 1, 1 };

				ID3D11DeviceContext_UpdateSubresource(P.context, (ID3D11Resource *)buffer, 0, &box, source, 0, 0);
			}
			break;
		case REWRITE_STAGING:
			{
				D3D11_BOX box = { 0, 0, 0, sizeof(source), 1, 1 };

				CHECK("memory", ID3D11DeviceContext_Map(P.context, (ID3D11Resource *)staging, 0, D3D11_MAP_WRITE, 0,
					&mapped));
				memcpy(mapped.pData, source, sizeof(source));
				ID3D11DeviceContext_Unmap(P.context, (ID3D11Resource *)staging, 0);
				ID3D11DeviceContext_CopySubresourceRegion(P.context, (ID3D11Resource *)buffer, 0, 0, 0, 0,
					(ID3D11Resource *)staging, 0, &box);
			}
			break;
		}
		target_bind(target);
		target_clear(target);
		ID3D11DeviceContext_Draw(P.context, 3, at / sizeof(struct vertex));
	}
	seen[0] = target_read(&first, 8, 8, &results[0]);
	seen[1] = target_read(&second, 8, 8, &results[1]);
	rep("memory.rewrite.%s: first 0x%08x (%s), second 0x%08x (%s): %s", names[path], seen[0],
		seen[0] == RED ? "red" : "NOT red", seen[1], seen[1] == GREEN ? "green" : "NOT green",
		seen[0] == RED && seen[1] == GREEN && SUCCEEDED(results[0]) && SUCCEEDED(results[1]) ? "ok" : "FAILED");
	if (seen[0] != RED || seen[1] != GREEN)
		P.failed = 1;
done:
	release(&staging);
	release(&buffer);
	target_free(&second);
	target_free(&first);
}

/* 4.4: textures by UpdateSubresource */
static void memory_texture(const char *name, DXGI_FORMAT format, UINT size, UINT row_pitch, int count)
{
	D3D11_TEXTURE2D_DESC description;
	ID3D11Texture2D *texture = NULL;
	double *times = calloc((size_t)count, sizeof(double));
	size_t bytes = (size_t)row_pitch * (format == DXGI_FORMAT_BC1_UNORM ? size / 4 : size);
	char *texels = malloc(bytes);
	char key[96];
	double start;
	int index;

	memset(texels, 0x5a, bytes);
	memset(&description, 0, sizeof(description));
	description.Width = size;
	description.Height = size;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Format = format;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	CHECK("memory", ID3D11Device_CreateTexture2D(P.device, &description, NULL, &texture));
	start = now_ms();
	for (index = 0; index < count; index++)
	{
		double call = now_ms();

		ID3D11DeviceContext_UpdateSubresource(P.context, (ID3D11Resource *)texture, 0, NULL, texels, row_pitch, 0);
		times[index] = now_ms() - call;
	}
	ID3D11DeviceContext_Flush(P.context);
	wait_idle();
	snprintf(key, sizeof(key), "memory.texture.%s.per_upload", name);
	if (count > 1)
		rep_times(key, times, count);
	else
		rep("%s: %.3f ms", key, times[0]);
	rep("memory.texture.%s.all_done: %.1f ms for %d uploads of %.1f KB, the GPU done", name, now_ms() - start, count,
		(double)bytes / 1024.0);
done:
	release(&texture);
	free(times);
	free(texels);
}

/* 4.5: the address space with 64 MB of buffers alive */
static void memory_address_space(void)
{
	ID3D11Buffer *buffers[16] = { NULL };
	char *zero = calloc(1, SEGMENT);
	int index, made = 0;

	report_address_space("memory.address_space.before_buffers");
	for (index = 0; index < 16; index++)
	{
		HRESULT result = buffer_make(&buffers[index], SEGMENT, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, zero);

		if (FAILED(result))
		{
			rep("memory.address_space.buffer_%d: failed (%s)", index, hr_text(result));
			break;
		}
		made++;
	}
	wait_idle();
	rep("memory.address_space.buffers_alive: %d MB", made * 4);
	report_address_space("memory.address_space.with_64mb_buffers");
	rep("memory.address_space.used_since_before_device: %.1f MB",
		(double)((long long)P.available_virtual_before - (long long)available_virtual()) / 1048576.0);
	for (index = 0; index < 16; index++)
		release(&buffers[index]);
	report_address_space("memory.address_space.after_release");
	free(zero);
}

static void step_memory(void)
{
	struct pipeline pipeline;
	struct target target;

	rep("step.memory: start");
	memset(&target, 0, sizeof(target));
	if (!pipeline_make(&pipeline, "memory"))
		return;
	CHECK("memory", target_make(&target, 16, 16));
	pipeline_bind(&pipeline);
	report_address_space("memory.address_space.start");
	memory_ring(&target);
	drain_debug("memory.ring");
	report_address_space("memory.address_space.after_ring");
	memory_mirror(&target, 1024, 0);
	memory_mirror(&target, 64, 0);
	memory_mirror(&target, 1024, 1);
	memory_mirror(&target, 64, 1);
	drain_debug("memory.mirror");
	report_address_space("memory.address_space.after_mirror");
	memory_rewrite(REWRITE_RING_APPEND);
	memory_rewrite(REWRITE_RING_DISCARD);
	memory_rewrite(REWRITE_UPDATE);
	memory_rewrite(REWRITE_STAGING);
	drain_debug("memory.rewrite");
	memory_texture("bc1_512", DXGI_FORMAT_BC1_UNORM, 512, 512 / 4 * 8, FRAMES);
	memory_texture("bgra8_512", DXGI_FORMAT_B8G8R8A8_UNORM, 512, 512 * 4, FRAMES);
	memory_texture("bgra8_2048", DXGI_FORMAT_B8G8R8A8_UNORM, 2048, 2048 * 4, 1);
	drain_debug("memory.texture");
	report_address_space("memory.address_space.after_textures");
	memory_address_space();
	drain_debug("memory.address_space");
	rep("step.memory: done");
done:
	ID3D11DeviceContext_ClearState(P.context);
	target_free(&target);
	pipeline_free(&pipeline);
}

/* ---------- step 5: queries (queries) */

#define QUERY_COUNT 64
#define QUERY_TARGET 256
#define QUERY_FRAMES 180
#define QUERY_FRAME_MS (1000.0 / 60.0)

/* a square of n x n pixels at the target's corner, as two triangles in clip space */
static void square_vertices(struct vertex *vertices, int n, uint32_t colour)
{
	float right = -1.0f + 2.0f * (float)n / QUERY_TARGET, bottom = 1.0f - 2.0f * (float)n / QUERY_TARGET;
	const float corners[6][2] = { { -1.0f, 1.0f }, { right, 1.0f }, { -1.0f, bottom },
		{ -1.0f, bottom }, { right, 1.0f }, { right, bottom } };
	int index;

	for (index = 0; index < 6; index++)
	{
		vertices[index].x = corners[index][0];
		vertices[index].y = corners[index][1];
		vertices[index].colour = colour;
		vertices[index].pad = 0;
	}
}

/* 64 occlusion queries issued in one frame, polled once a frame with DONOTFLUSH and never waited on */
static void queries_latency(const struct target *target, ID3D11Buffer *squares, int flush)
{
	ID3D11Query *queries[QUERY_COUNT] = { NULL };
	int answered_at[QUERY_COUNT];
	UINT64 counts[QUERY_COUNT];
	double frames_taken[QUERY_COUNT], answered_ms[QUERY_COUNT];
	D3D11_QUERY_DESC description = { D3D11_QUERY_OCCLUSION, 0 };
	int index, frame, answered = 0, exact = 0;
	double start, frame_start;
	const char *mode = flush ? "flush" : "no_flush";

	for (index = 0; index < QUERY_COUNT; index++)
	{
		answered_at[index] = -1;
		CHECK("queries", ID3D11Device_CreateQuery(P.device, &description, &queries[index]));
	}
	target_bind(target);
	bind_vertices(squares);
	start = now_ms();
	for (frame = 0; frame < QUERY_FRAMES && answered < QUERY_COUNT; frame++)
	{
		frame_start = now_ms();
		target_clear(target);
		if (frame == 0)
		{
			for (index = 0; index < QUERY_COUNT; index++)
			{
				ID3D11DeviceContext_Begin(P.context, (ID3D11Asynchronous *)queries[index]);
				ID3D11DeviceContext_Draw(P.context, 6, (UINT)index * 6);
				ID3D11DeviceContext_End(P.context, (ID3D11Asynchronous *)queries[index]);
			}
		}
		else
		{
			/* the frame's other work: a draw */
			ID3D11DeviceContext_Draw(P.context, 6, 0);
		}
		if (flush)
			ID3D11DeviceContext_Flush(P.context);
		for (index = 0; index < QUERY_COUNT; index++)
		{
			HRESULT result;

			if (answered_at[index] >= 0)
				continue;
			result = ID3D11DeviceContext_GetData(P.context, (ID3D11Asynchronous *)queries[index], &counts[index],
				sizeof(counts[index]), D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (result == S_OK)
			{
				answered_at[index] = frame;
				frames_taken[answered] = frame;
				answered_ms[answered] = now_ms() - start;
				answered++;
				if (counts[index] == (UINT64)(index + 1) * (UINT64)(index + 1))
					exact++;
			}
			else if (result != S_FALSE)
			{
				rep("queries.%s.get_data_%d: %s", mode, index, hr_text(result));
				answered_at[index] = frame;
			}
		}
		/* a frame of 60 Hz (Sleep(1) sleeps a whole tick of 15.6 ms) */
		while (now_ms() - frame_start < QUERY_FRAME_MS)
			Sleep(0);
	}
	rep("queries.%s.answered: %d of %d within %d frames of %.1f ms", mode, answered, QUERY_COUNT, frame,
		QUERY_FRAME_MS);
	if (answered)
	{
		qsort(frames_taken, (size_t)answered, sizeof(double), compare_doubles);
		qsort(answered_ms, (size_t)answered, sizeof(double), compare_doubles);
		rep("queries.%s.frames_to_answer: min %.0f, median %.0f, max %.0f", mode, frames_taken[0],
			frames_taken[answered / 2], frames_taken[answered - 1]);
		rep("queries.%s.ms_to_answer: min %.1f, median %.1f, max %.1f", mode, answered_ms[0], answered_ms[answered / 2],
			answered_ms[answered - 1]);
	}
	rep("queries.%s.exact: %d of %d answered are exactly n*n", mode, exact, answered);
	for (index = 0; index < QUERY_COUNT; index++)
	{
		if (answered_at[index] >= 0 && counts[index] != (UINT64)(index + 1) * (UINT64)(index + 1))
			rep("queries.%s.inexact_%d: %llu, expected %d", mode, index + 1, (unsigned long long)counts[index],
				(index + 1) * (index + 1));
	}
	if (answered < QUERY_COUNT)
	{
		/* whether they were ever going to answer: a flush and a wait, now */
		int late = 0;

		ID3D11DeviceContext_Flush(P.context);
		wait_idle();
		for (index = 0; index < QUERY_COUNT; index++)
			if (answered_at[index] < 0 && ID3D11DeviceContext_GetData(P.context, (ID3D11Asynchronous *)queries[index],
				&counts[index], sizeof(counts[index]), 0) == S_OK)
				late++;
		rep("queries.%s.answered_after_flush_and_wait: %d", mode, late);
	}
done:
	for (index = 0; index < QUERY_COUNT; index++)
		release(&queries[index]);
}

static void queries_predicate(const struct target *target, ID3D11Buffer *squares)
{
	static const int sizes[] = { 0, 1, 16 };
	D3D11_QUERY_DESC description = { D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
	size_t index;

	target_bind(target);
	bind_vertices(squares);
	for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++)
	{
		ID3D11Predicate *predicate = NULL;
		BOOL visible = FALSE;
		HRESULT result = ID3D11Device_CreatePredicate(P.device, &description, &predicate);
		double start;

		if (FAILED(result))
		{
			rep("queries.predicate: failed (%s)", hr_text(result));
			return;
		}
		ID3D11DeviceContext_Begin(P.context, (ID3D11Asynchronous *)predicate);
		if (sizes[index])
			ID3D11DeviceContext_Draw(P.context, 6, (UINT)(sizes[index] - 1) * 6);
		ID3D11DeviceContext_End(P.context, (ID3D11Asynchronous *)predicate);
		start = now_ms();
		while ((result = ID3D11DeviceContext_GetData(P.context, (ID3D11Asynchronous *)predicate, &visible,
			sizeof(visible), 0)) == S_FALSE && now_ms() - start < 5000.0)
			Sleep(0);
		rep("queries.predicate.%dx%d: %s (%s)", sizes[index], sizes[index], visible ? "visible" : "not visible",
			hr_text(result));
		release(&predicate);
	}
}

/* the game's 4,096 slots, made at once */
static void queries_many(void)
{
	enum { MANY = 4096 };
	ID3D11Query **queries = calloc(MANY, sizeof(*queries));
	D3D11_QUERY_DESC description = { D3D11_QUERY_OCCLUSION, 0 };
	int index, made = 0;
	HRESULT result = S_OK;
	double start = now_ms();

	for (index = 0; index < MANY; index++)
	{
		result = ID3D11Device_CreateQuery(P.device, &description, &queries[index]);
		if (FAILED(result))
			break;
		made++;
	}
	rep("queries.many: %d of %d made in %.3f ms%s%s", made, MANY, now_ms() - start, made < MANY ? ", stopped at " : "",
		made < MANY ? hr_text(result) : "");
	drain_debug("queries.many");
	start = now_ms();
	for (index = 0; index < MANY; index++)
		release(&queries[index]);
	rep("queries.many.release: %.3f ms", now_ms() - start);
	free(queries);
}

static void step_queries(void)
{
	struct pipeline pipeline;
	struct target target;
	ID3D11Buffer *squares = NULL;
	struct vertex *vertices = calloc(QUERY_COUNT * 6, sizeof(struct vertex));
	int index;

	rep("step.queries: start");
	memset(&target, 0, sizeof(target));
	if (!pipeline_make(&pipeline, "queries"))
	{
		free(vertices);
		return;
	}
	for (index = 0; index < QUERY_COUNT; index++)
		square_vertices(vertices + index * 6, index + 1, GREEN);
	CHECK("queries", buffer_make(&squares, QUERY_COUNT * 6 * sizeof(struct vertex), D3D11_USAGE_IMMUTABLE,
		D3D11_BIND_VERTEX_BUFFER, 0, vertices));
	CHECK("queries", target_make(&target, QUERY_TARGET, QUERY_TARGET));
	pipeline_bind(&pipeline);
	queries_latency(&target, squares, 0);
	drain_debug("queries.no_flush");
	queries_latency(&target, squares, 1);
	drain_debug("queries.flush");
	queries_predicate(&target, squares);
	drain_debug("queries.predicate");
	queries_many();
	rep("step.queries: done");
done:
	ID3D11DeviceContext_ClearState(P.context);
	release(&squares);
	target_free(&target);
	pipeline_free(&pipeline);
	free(vertices);
}

/* ---------- step 2: a frame on the screen (present) */

static const char triangle_hlsl[] =
	"float4 vs_main(uint id : SV_VertexID) : SV_Position\n"
	"{\n"
	"	/* a small triangle at the top left, its point up: the picture's orientation */\n"
	"	const float2 corners[3] = { float2(-0.8, 0.9), float2(-0.65, 0.6), float2(-0.95, 0.6) };\n"
	"	return float4(corners[id], 0.0, 1.0);\n"
	"}\n"
	"float4 ps_main(float4 position : SV_Position) : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }\n";

#define PACING_WINDOW_MS 10000.0
#define IDLE_END_MS 120000.0

struct presenter
{
	SDL_Window *window;
	HWND hwnd;
	IDXGIFactory2 *factory;
	IDXGISwapChain1 *swap_chain;
	ID3D11RenderTargetView *view;
	ID3D11VertexShader *vs;
	ID3D11PixelShader *ps;
	UINT swap_flags;
	int tearing;
	int width, height;

	/* the pacing of the current window of ten seconds */
	double *intervals;
	int interval_count, interval_capacity;
	double window_start, last_present;

	/* Present's results */
	HRESULT results[16];
	int result_counts[16];
	int result_kinds;
	int presents;

	/* the transition in progress, if any */
	char transition[64];
	double transition_start;
	int transition_resized;
	int transition_waiting;

	double last_input;
	int escapes;
	int answer_right, answer_wrong;
	int keys, buttons;
};

static int make_view(struct presenter *presenter)
{
	ID3D11Texture2D *back = NULL;
	HRESULT result = IDXGISwapChain1_GetBuffer(presenter->swap_chain, 0, &IID_ID3D11Texture2D, (void **)&back);

	if (SUCCEEDED(result))
		result = ID3D11Device_CreateRenderTargetView(P.device, (ID3D11Resource *)back, NULL, &presenter->view);
	release(&back);
	if (FAILED(result))
		rep("present.back_buffer_view: failed (%s)", hr_text(result));
	return SUCCEEDED(result);
}

static void transition_begin(struct presenter *presenter, const char *name)
{
	snprintf(presenter->transition, sizeof(presenter->transition), "%s", name);
	presenter->transition_start = now_ms();
	presenter->transition_resized = 0;
	presenter->transition_waiting = 1;
	rep("present.transition.%s: asked at %.0f ms", name, presenter->transition_start);
}

static void resize(struct presenter *presenter)
{
	int width = 0, height = 0;
	double start = now_ms();
	HRESULT result;

	SDL_GetWindowSizeInPixels(presenter->window, &width, &height);
	if (width == presenter->width && height == presenter->height)
		return;
	ID3D11DeviceContext_OMSetRenderTargets(P.context, 0, NULL, NULL);
	release(&presenter->view);
	ID3D11DeviceContext_Flush(P.context);
	result = IDXGISwapChain1_ResizeBuffers(presenter->swap_chain, 0, 0, 0, DXGI_FORMAT_UNKNOWN, presenter->swap_flags);
	rep("present.resize: %dx%d to %dx%d, ResizeBuffers %s in %.3f ms", presenter->width, presenter->height, width,
		height, hr_text(result), now_ms() - start);
	presenter->width = width;
	presenter->height = height;
	if (SUCCEEDED(result))
		make_view(presenter);
	if (presenter->transition_waiting)
		presenter->transition_resized = 1;
}

static void pacing_report(struct presenter *presenter, int final)
{
	double *sorted = presenter->intervals, median;
	int index, slow = 0;

	if (presenter->interval_count < 2)
	{
		if (final)
			rep("present.pacing: fewer than two presents in the last window");
		presenter->interval_count = 0;
		return;
	}
	qsort(sorted, (size_t)presenter->interval_count, sizeof(double), compare_doubles);
	median = sorted[presenter->interval_count / 2];
	for (index = 0; index < presenter->interval_count; index++)
		if (sorted[index] > 1.5 * median)
			slow++;
	rep("present.pacing: %d intervals, min %.3f ms, median %.3f ms (%.1f Hz), max %.3f ms, %d over 1.5x the median%s",
		presenter->interval_count, sorted[0], median, median > 0.0 ? 1000.0 / median : 0.0,
		sorted[presenter->interval_count - 1], slow, final ? " (the last, partial window)" : "");
	presenter->interval_count = 0;
}

static void count_result(struct presenter *presenter, HRESULT result)
{
	int index;

	for (index = 0; index < presenter->result_kinds; index++)
	{
		if (presenter->results[index] == result)
		{
			presenter->result_counts[index]++;
			return;
		}
	}
	if (presenter->result_kinds < 16)
	{
		presenter->results[presenter->result_kinds] = result;
		presenter->result_counts[presenter->result_kinds++] = 1;
	}
	rep("present.result_first_seen: %s at present %d (%.0f ms)%s%s", hr_text(result), presenter->presents, now_ms(),
		presenter->transition[0] ? ", during " : "", presenter->transition);
}

/* the display's refresh, as SDL sees it */
static void report_display(struct presenter *presenter, const char *when)
{
	SDL_DisplayID display = SDL_GetDisplayForWindow(presenter->window);
	const SDL_DisplayMode *mode = display ? SDL_GetCurrentDisplayMode(display) : NULL;

	if (mode)
		rep("present.display.%s: %dx%d at %.2f Hz (pixel density %.2f)", when, mode->w, mode->h, mode->refresh_rate,
			mode->pixel_density);
}

static void play_tone(void)
{
	SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, 48000 };
	SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	float *samples;
	int index;

	if (!stream)
	{
		rep("present.audio: no stream (%s)", SDL_GetError());
		return;
	}
	samples = malloc(sizeof(float) * 48000);
	for (index = 0; index < 48000; index++)
		samples[index] = 0.2f * sinf(2.0f * 3.14159265f * 440.0f * (float)index / 48000.0f);
	SDL_PutAudioStreamData(stream, samples, sizeof(float) * 48000);
	SDL_ResumeAudioStreamDevice(stream);
	free(samples);
	rep("present.audio: stream opened, a one-second tone of 440 Hz queued");
	/* left open: SDL_Quit closes it, after the tone */
}

static const char *const window_sizes[3][2] = { { "960", "720" }, { "1280", "720" }, { "640", "480" } };

static void handle_key(struct presenter *presenter, const SDL_KeyboardEvent *key, int *running)
{
	SDL_Keycode code = key->key;
	int alt = (key->mod & SDL_KMOD_ALT) != 0;

	presenter->keys++;
	rep("present.key: %s%s", alt ? "Alt+" : "", SDL_GetKeyName(code));
	if (code != SDLK_ESCAPE)
		presenter->escapes = 0;
	if (alt && (code == SDLK_RETURN || code == SDLK_KP_ENTER))
	{
		transition_begin(presenter, "alt_enter");
		return;
	}
	switch (code)
	{
	case SDLK_F11:
		transition_begin(presenter, "borderless");
		SDL_SetWindowFullscreenMode(presenter->window, NULL);
		if (!SDL_SetWindowFullscreen(presenter->window, true))
			rep("present.sdl: SDL_SetWindowFullscreen (borderless) failed: %s", SDL_GetError());
		break;
	case SDLK_F10:
		{
			SDL_DisplayID display = SDL_GetDisplayForWindow(presenter->window);
			const SDL_DisplayMode *mode = display ? SDL_GetCurrentDisplayMode(display) : NULL;

			transition_begin(presenter, "exclusive");
			if (!mode || !SDL_SetWindowFullscreenMode(presenter->window, mode))
				rep("present.sdl: SDL_SetWindowFullscreenMode failed: %s", SDL_GetError());
			if (!SDL_SetWindowFullscreen(presenter->window, true))
				rep("present.sdl: SDL_SetWindowFullscreen (exclusive) failed: %s", SDL_GetError());
		}
		break;
	case SDLK_ESCAPE:
		if (++presenter->escapes >= 2)
		{
			rep("present.end: Escape twice");
			*running = 0;
			break;
		}
		transition_begin(presenter, "windowed");
		if (!SDL_SetWindowFullscreen(presenter->window, false))
			rep("present.sdl: SDL_SetWindowFullscreen (windowed) failed: %s", SDL_GetError());
		break;
	case SDLK_F9:
		{
			static int next;
			const char *const *size = window_sizes[next++ % 3];

			transition_begin(presenter, "window_size");
			SDL_SetWindowSize(presenter->window, atoi(size[0]), atoi(size[1]));
		}
		break;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
		presenter->answer_right++;
		rep("present.tester: the picture looks right");
		break;
	case SDLK_BACKSPACE:
		presenter->answer_wrong++;
		rep("present.tester: the picture does NOT look right");
		break;
	default:
		break;
	}
}

static void handle_button(struct presenter *presenter, SDL_GamepadButton button, int *running)
{
	presenter->buttons++;
	rep("present.gamepad_button: %s", SDL_GetGamepadStringForButton(button));
	switch (button)
	{
	case SDL_GAMEPAD_BUTTON_SOUTH:
		presenter->answer_right++;
		rep("present.tester: the picture looks right");
		break;
	case SDL_GAMEPAD_BUTTON_NORTH:
		presenter->answer_wrong++;
		rep("present.tester: the picture does NOT look right");
		break;
	case SDL_GAMEPAD_BUTTON_EAST:
		rep("present.end: B");
		*running = 0;
		break;
	default:
		break;
	}
}

static void step_present(void)
{
	struct presenter presenter;
	DXGI_SWAP_CHAIN_DESC1 description;
	ID3DBlob *vs = NULL, *ps = NULL;
	IDXGIDevice *dxgi = NULL;
	IDXGIAdapter *adapter = NULL;
	IDXGIFactory5 *factory5 = NULL;
	SDL_Gamepad *gamepad = NULL;
	char errors[1024];
	int running = 1, index;
	double start;
	HRESULT result;

	rep("step.present: start");
	memset(&presenter, 0, sizeof(presenter));
	presenter.interval_capacity = 65536;
	presenter.intervals = malloc(sizeof(double) * (size_t)presenter.interval_capacity);
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO))
	{
		rep("present.sdl_init: %s", SDL_GetError());
		if (!SDL_Init(SDL_INIT_VIDEO))
		{
			step_failed("present", "SDL_Init", E_FAIL);
			free(presenter.intervals);
			return;
		}
	}
	rep("present.sdl_version: %d.%d.%d", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
		SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
	rep("present.sdl_video_driver: %s", SDL_GetCurrentVideoDriver());
	presenter.window = SDL_CreateWindow("Direct3D 11 probe", 960, 720,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!presenter.window)
	{
		rep("present.window: %s", SDL_GetError());
		step_failed("present", "SDL_CreateWindow", E_FAIL);
		goto done;
	}
	presenter.hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(presenter.window),
		SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
	rep("present.hwnd: %p", (void *)presenter.hwnd);
	if (!presenter.hwnd)
	{
		step_failed("present", "SDL_PROP_WINDOW_WIN32_HWND_POINTER", E_FAIL);
		goto done;
	}
	report_display(&presenter, "start");

	/* the swap chain's factory is the device's own */
	CHECK("present", ID3D11Device_QueryInterface(P.device, &IID_IDXGIDevice, (void **)&dxgi));
	CHECK("present", IDXGIDevice_GetAdapter(dxgi, &adapter));
	CHECK("present", IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&presenter.factory));
	if (SUCCEEDED(IDXGIFactory2_QueryInterface(presenter.factory, &IID_IDXGIFactory5, (void **)&factory5)))
	{
		BOOL allow = FALSE;

		result = IDXGIFactory5_CheckFeatureSupport(factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow));
		presenter.tearing = SUCCEEDED(result) && allow;
		rep("present.allow_tearing: %s (%s)", yesno(presenter.tearing), hr_text(result));
	}
	else
	{
		rep("present.allow_tearing: no (no IDXGIFactory5)");
	}
	presenter.swap_flags = presenter.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

	SDL_GetWindowSizeInPixels(presenter.window, &presenter.width, &presenter.height);
	memset(&description, 0, sizeof(description));
	description.Width = 0;
	description.Height = 0;
	description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	description.SampleDesc.Count = 1;
	description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	description.BufferCount = 2;
	description.Scaling = DXGI_SCALING_NONE;
	description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	description.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	description.Flags = presenter.swap_flags;
	start = now_ms();
	result = IDXGIFactory2_CreateSwapChainForHwnd(presenter.factory, (IUnknown *)P.device, presenter.hwnd, &description,
		NULL, NULL, &presenter.swap_chain);
	rep("present.swap_chain: %s in %.3f ms (FLIP_DISCARD, 2 buffers, B8G8R8A8_UNORM, SCALING_NONE, %dx%d%s)",
		hr_text(result), now_ms() - start, presenter.width, presenter.height,
		presenter.tearing ? ", ALLOW_TEARING" : "");
	if (FAILED(result))
	{
		step_failed("present", "CreateSwapChainForHwnd", result);
		goto done;
	}
	result = IDXGIFactory2_MakeWindowAssociation(presenter.factory, presenter.hwnd,
		DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
	rep("present.make_window_association: %s", hr_text(result));
	if (!make_view(&presenter))
	{
		step_failed("present", "the back buffer's view", E_FAIL);
		goto done;
	}
	drain_debug("present.setup");

	/* the step's own proof that the compiler is reachable from this process */
	if (!load_compiler())
	{
		step_failed("present", "load_compiler", E_FAIL);
		goto done;
	}
	start = now_ms();
	result = compile_text(triangle_hlsl, sizeof(triangle_hlsl) - 1, "triangle", "vs_main", "vs_4_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3, &vs, errors, sizeof(errors));
	if (SUCCEEDED(result))
		result = compile_text(triangle_hlsl, sizeof(triangle_hlsl) - 1, "triangle", "ps_main", "ps_4_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, &ps, errors, sizeof(errors));
	rep("present.triangle_compile: %s in %.3f ms%s%s", hr_text(result), now_ms() - start, errors[0] ? ": " : "", errors);
	if (FAILED(result))
	{
		step_failed("present", "D3DCompile (triangle)", result);
		goto done;
	}
	CHECK("present", ID3D11Device_CreateVertexShader(P.device, ID3D10Blob_GetBufferPointer(vs),
		ID3D10Blob_GetBufferSize(vs), NULL, &presenter.vs));
	CHECK("present", ID3D11Device_CreatePixelShader(P.device, ID3D10Blob_GetBufferPointer(ps),
		ID3D10Blob_GetBufferSize(ps), NULL, &presenter.ps));

	if (P.quiet)
		rep("present.audio: no tone (--quiet)");
	else if (SDL_WasInit(SDL_INIT_AUDIO))
		play_tone();
	else
		rep("present.audio: SDL's audio did not start");

	rep("present.sync_interval: %d%s", P.sync, P.sync == 0 && presenter.tearing ? " (with DXGI_PRESENT_ALLOW_TEARING)" : "");
	rep("present.script: watch the colour cycle, with a white triangle pointing up at the top left; resize the window "
		"by its edge; F11 borderless, F10 exclusive, Escape windowed; F9 cycles three window sizes; minimise and "
		"restore; Alt+Enter (nothing should happen); A or Enter if the picture looks right, Y or Backspace if not; "
		"B or Escape twice to end. 120 s without input ends the step.");

	start = now_ms();
	presenter.window_start = start;
	presenter.last_input = start;
	while (running)
	{
		SDL_Event event;
		double now;
		float phase, clear[4];
		D3D11_VIEWPORT viewport;
		UINT flags;

		while (SDL_PollEvent(&event))
		{
			switch (event.type)
			{
			case SDL_EVENT_QUIT:
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				rep("present.end: the window was closed");
				running = 0;
				break;
			case SDL_EVENT_KEY_DOWN:
				presenter.last_input = now_ms();
				if (!event.key.repeat)
					handle_key(&presenter, &event.key, &running);
				break;
			case SDL_EVENT_GAMEPAD_ADDED:
				if (!gamepad)
				{
					gamepad = SDL_OpenGamepad(event.gdevice.which);
					rep("present.gamepad: %s", gamepad ? SDL_GetGamepadName(gamepad) : SDL_GetError());
				}
				break;
			case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
				presenter.last_input = now_ms();
				handle_button(&presenter, (SDL_GamepadButton)event.gbutton.button, &running);
				break;
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				rep("present.event: pixel size changed to %dx%d at %.0f ms", event.window.data1, event.window.data2,
					now_ms());
				presenter.last_input = now_ms();
				resize(&presenter);
				break;
			case SDL_EVENT_WINDOW_MINIMIZED:
				presenter.last_input = now_ms();
				transition_begin(&presenter, "minimised");
				break;
			case SDL_EVENT_WINDOW_RESTORED:
				presenter.last_input = now_ms();
				transition_begin(&presenter, "restored");
				break;
			case SDL_EVENT_WINDOW_MAXIMIZED:
				transition_begin(&presenter, "maximised");
				break;
			case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
				rep("present.event: entered fullscreen at %.0f ms", now_ms());
				report_display(&presenter, "fullscreen");
				break;
			case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
				rep("present.event: left fullscreen at %.0f ms", now_ms());
				break;
			case SDL_EVENT_WINDOW_OCCLUDED:
				rep("present.event: occluded at %.0f ms", now_ms());
				break;
			case SDL_EVENT_WINDOW_EXPOSED:
				break;
			case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
				report_display(&presenter, "changed");
				break;
			default:
				break;
			}
		}
		now = now_ms();
		if (now - presenter.last_input > IDLE_END_MS)
		{
			rep("present.end: 120 s without input");
			break;
		}

		/* the frame */
		phase = (float)fmod((now - start) / 4000.0, 1.0);
		clear[0] = 0.5f + 0.5f * sinf(6.2831853f * phase);
		clear[1] = 0.5f + 0.5f * sinf(6.2831853f * (phase + 1.0f / 3.0f));
		clear[2] = 0.5f + 0.5f * sinf(6.2831853f * (phase + 2.0f / 3.0f));
		clear[3] = 1.0f;
		if (presenter.view)
		{
			viewport.TopLeftX = 0.0f;
			viewport.TopLeftY = 0.0f;
			viewport.Width = (float)presenter.width;
			viewport.Height = (float)presenter.height;
			viewport.MinDepth = 0.0f;
			viewport.MaxDepth = 1.0f;
			ID3D11DeviceContext_OMSetRenderTargets(P.context, 1, &presenter.view, NULL);
			ID3D11DeviceContext_RSSetViewports(P.context, 1, &viewport);
			ID3D11DeviceContext_ClearRenderTargetView(P.context, presenter.view, clear);
			ID3D11DeviceContext_IASetInputLayout(P.context, NULL);
			ID3D11DeviceContext_IASetPrimitiveTopology(P.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ID3D11DeviceContext_VSSetShader(P.context, presenter.vs, NULL, 0);
			ID3D11DeviceContext_PSSetShader(P.context, presenter.ps, NULL, 0);
			ID3D11DeviceContext_Draw(P.context, 3, 0);
		}
		flags = P.sync == 0 && presenter.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
		result = IDXGISwapChain1_Present(presenter.swap_chain, (UINT)P.sync, flags);
		now = now_ms();
		presenter.presents++;
		count_result(&presenter, result);
		if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
		{
			rep("present.device_removed_reason: %s", hr_text(ID3D11Device_GetDeviceRemovedReason(P.device)));
			step_failed("present", "Present", result);
			break;
		}
		if (result == S_OK)
		{
			if (presenter.last_present > 0.0 && presenter.interval_count < presenter.interval_capacity)
				presenter.intervals[presenter.interval_count++] = now - presenter.last_present;
			presenter.last_present = now;
			if (presenter.transition_waiting)
			{
				rep("present.transition.%s: presenting (S_OK) %.1f ms after it was asked%s", presenter.transition,
					now - presenter.transition_start, presenter.transition_resized ? ", after a resize" : ", no resize");
				presenter.transition_waiting = 0;
				presenter.transition[0] = 0;
			}
		}
		else
		{
			/* occluded (minimised): nothing is shown, so do not spin */
			presenter.last_present = 0.0;
			Sleep(16);
		}
		if (now - presenter.window_start >= PACING_WINDOW_MS)
		{
			pacing_report(&presenter, 0);
			presenter.window_start = now;
			drain_debug("present");
		}
	}
	pacing_report(&presenter, 1);
	rep("present.presents: %d in %.1f s", presenter.presents, (now_ms() - start) / 1000.0);
	for (index = 0; index < presenter.result_kinds; index++)
		rep("present.result: %s %d times", hr_text(presenter.results[index]), presenter.result_counts[index]);
	rep("present.input: %d keys, %d gamepad buttons%s", presenter.keys, presenter.buttons,
		gamepad ? "" : " (no gamepad opened)");
	rep("present.tester_answers: %d right, %d not right", presenter.answer_right, presenter.answer_wrong);
	drain_debug("present");
	rep("step.present: done");
done:
	ID3D11DeviceContext_ClearState(P.context);
	ID3D11DeviceContext_Flush(P.context);
	if (gamepad)
		SDL_CloseGamepad(gamepad);
	release(&presenter.ps);
	release(&presenter.vs);
	release(&presenter.view);
	if (presenter.swap_chain)
	{
		/* a windowed swap chain: nothing to leave fullscreen */
		release(&presenter.swap_chain);
	}
	release(&factory5);
	release(&presenter.factory);
	release(&adapter);
	release(&dxgi);
	release(&ps);
	release(&vs);
	if (presenter.window)
		SDL_DestroyWindow(presenter.window);
	SDL_Delay(200);
	SDL_Quit();
	free(presenter.intervals);
}

/* ---------- running it */

static const struct { const char *name; void (*run)(void); } step_table[] = {
	{ "caps", step_caps },
	{ "compile", step_compile },
	{ "memory", step_memory },
	{ "queries", step_queries },
	{ "present", step_present },
};
#define STEP_COUNT ((int)(sizeof(step_table) / sizeof(step_table[0])))

static char skip_path[MAX_PATH + 32], running_path[MAX_PATH + 32];

static int in_list(const char *list, const char *name)
{
	size_t length = strlen(name);
	const char *at = list;

	while (list && (at = strstr(at, name)))
	{
		int start = at == list || at[-1] == ',' || at[-1] == '\n' || at[-1] == '\r' || at[-1] == ' ';
		int end = at[length] == 0 || at[length] == ',' || at[length] == '\n' || at[length] == '\r' || at[length] == ' ';

		if (start && end)
			return 1;
		at += length;
	}
	return 0;
}

/* the step being run, which the running marker goes back to when a sub-step's guard ends: a crash later in the
same step must still name the step */
static char current_step[64];

static void mark_running(const char *name)
{
	if (name)
	{
		FILE *file = fopen(running_path, "wb");

		if (file)
		{
			fputs(name, file);
			fclose(file);
		}
	}
	else
	{
		remove(running_path);
	}
}

/* a risky sub-step: true if it may run (it did not kill the probe before) */
static int guard_begin(const char *name)
{
	char *skipped = read_file(skip_path, NULL);
	int skip = in_list(skipped, name);

	free(skipped);
	if (skip)
	{
		rep("guard.%s: skipped (it killed the probe once; delete d3d11_probe_skip.txt to try again)", name);
		return 0;
	}
	mark_running(name);
	return 1;
}

static void guard_end(void)
{
	mark_running(current_step[0] ? current_step : NULL);
}

static void teardown(void)
{
	ID3D11Debug *debug = NULL;

	if (P.context)
	{
		ID3D11DeviceContext_ClearState(P.context);
		ID3D11DeviceContext_Flush(P.context);
	}
	if (P.compiler)
		FreeLibrary(P.compiler);
	P.compiler = NULL;
	P.compile = NULL;
	release(&P.context);
	/* with the layer, whether anything the probe made is still alive: the device itself (held here, and by the
	info queue) is the one expected */
	if (P.device && P.debug_layer &&
		SUCCEEDED(ID3D11Device_QueryInterface(P.device, &IID_ID3D11Debug, (void **)&debug)) &&
		guard_begin("live_objects"))
	{
		int before = P.debug_messages;

		ID3D11Debug_ReportLiveDeviceObjects(debug, D3D11_RLDO_SUMMARY);
		drain_debug("live_objects");
		rep("debug.live_object_messages: %d (the device's own summary line is expected)", P.debug_messages - before);
		guard_end();
	}
	release(&debug);
	release(&P.info);
	release(&P.device);
	release(&P.adapter);
	release(&P.factory);
}

static void usage(void)
{
	printf("d3d11_probe.exe [all|caps,compile,memory,queries,present] [--adapter hardware|warp|<part of a name>]\n"
		"\t[--shaders <folder>] [--report <file>] [--sync 0|1] [--quiet]\n");
}

/* the committed shaders (port/windows/probe), found from the executable's folder (build/windows) */
static void default_shaders(void)
{
	char exe[MAX_PATH], *slash;

	if (!GetModuleFileNameA(NULL, exe, sizeof(exe)))
		exe[0] = 0;
	slash = strrchr(exe, '\\');
	if (slash)
		*slash = 0;
	snprintf(P.shaders_dir, sizeof(P.shaders_dir), "%s\\..\\..\\port\\windows\\probe", exe);
	if (GetFileAttributesA(P.shaders_dir) == INVALID_FILE_ATTRIBUTES)
		snprintf(P.shaders_dir, sizeof(P.shaders_dir), "probe");
}

int main(int argc, char **argv)
{
	const char *steps = "all", *report = "d3d11_probe.txt";
	char full[MAX_PATH], *name, *crashed, *skipped;
	LARGE_INTEGER frequency;
	int index, setup = 0;

	QueryPerformanceFrequency(&frequency);
	counter_frequency = (double)frequency.QuadPart;
	snprintf(P.adapter_asked, sizeof(P.adapter_asked), "hardware");
	P.sync = 1;
	default_shaders();
	for (index = 1; index < argc; index++)
	{
		if (!strcmp(argv[index], "--adapter") && index + 1 < argc)
			snprintf(P.adapter_asked, sizeof(P.adapter_asked), "%s", argv[++index]);
		else if (!strcmp(argv[index], "--shaders") && index + 1 < argc)
			snprintf(P.shaders_dir, sizeof(P.shaders_dir), "%s", argv[++index]);
		else if (!strcmp(argv[index], "--report") && index + 1 < argc)
			report = argv[++index];
		else if (!strcmp(argv[index], "--sync") && index + 1 < argc)
			P.sync = atoi(argv[++index]) ? 1 : 0;
		else if (!strcmp(argv[index], "--quiet"))
			P.quiet = 1;
		else if (!strcmp(argv[index], "--help") || !strcmp(argv[index], "-h"))
		{
			usage();
			return 0;
		}
		else if (argv[index][0] != '-')
			steps = argv[index];
		else
		{
			usage();
			return 2;
		}
	}
	if (!GetFullPathNameA(report, sizeof(full), full, &name))
		snprintf(full, sizeof(full), "%s", report);
	snprintf(P.report_dir, sizeof(P.report_dir), "%s", full);
	name = strrchr(P.report_dir, '\\');
	if (name)
		*name = 0;
	else
		snprintf(P.report_dir, sizeof(P.report_dir), ".");
	snprintf(skip_path, sizeof(skip_path), "%s\\d3d11_probe_skip.txt", P.report_dir);
	snprintf(running_path, sizeof(running_path), "%s\\d3d11_probe_running.txt", P.report_dir);
	report_file = fopen(full, "wb");
	if (!report_file)
		fprintf(stderr, "d3d11 probe: cannot write %s\n", full);

	rep("probe.start: %s", steps);
	report_machine();
	rep("probe.steps_asked: %s", steps);
	rep("probe.adapter_asked: %s", P.adapter_asked);
	rep("probe.sync_asked: %d", P.sync);
	rep("probe.report: %s", full);

	/* a step that died last time is left out this time */
	crashed = read_file(running_path, NULL);
	if (crashed && crashed[0])
	{
		FILE *file = fopen(skip_path, "ab");

		rep("probe.crashed_last_time: %s (left out from now on)", crashed);
		if (file)
		{
			fprintf(file, "%s\n", crashed);
			fclose(file);
		}
	}
	free(crashed);
	mark_running(NULL);

	if (guard_begin("setup"))
	{
		setup = choose_adapter() && create_device();
		mark_running(NULL);
	}
	if (!setup)
	{
		rep("probe.setup: failed");
		P.failed = 1;
	}
	else
	{
		skipped = read_file(skip_path, NULL);
		rep("probe.setup: ok");
		for (index = 0; index < STEP_COUNT; index++)
		{
			int asked = !strcmp(steps, "all") || in_list(steps, step_table[index].name);

			/* caps is the report's base: it is run whatever is asked for, and costs nothing */
			if (!asked && index != 0)
				continue;
			if (in_list(skipped, step_table[index].name))
			{
				rep("step.%s: skipped (it killed the probe once; delete d3d11_probe_skip.txt to try again)",
					step_table[index].name);
				continue;
			}
			snprintf(current_step, sizeof(current_step), "%s", step_table[index].name);
			mark_running(current_step);
			step_table[index].run();
			drain_debug(step_table[index].name);
			if (P.device)
			{
				HRESULT removed = ID3D11Device_GetDeviceRemovedReason(P.device);

				if (removed != S_OK)
				{
					rep("device.removed_after_%s: %s", step_table[index].name, hr_text(removed));
					P.failed = 1;
				}
			}
			current_step[0] = 0;
			mark_running(NULL);
		}
		free(skipped);
	}
	teardown();
	rep("debug.summary: %d messages, %d errors or corruption, %d warnings%s", P.debug_messages, P.debug_errors,
		P.debug_warnings, P.debug_layer ? "" : " (no debug layer)");
	rep("probe.done: yes");
	rep("probe.exit: %d", P.failed ? 1 : 0);
	mark_running(NULL);
	if (report_file)
		fclose(report_file);
	return P.failed ? 1 : 0;
}
