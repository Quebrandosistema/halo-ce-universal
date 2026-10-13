/*
WIN32_D3D11_SHADERS.C

The Direct3D 11 backend's shader service and state objects
(port/windows/D3D11.md, phase 5), the backend's third unit beside
win32_d3d11.c and win32_d3d11_memory.c, sharing win32_d3d11.h. A
Windows-facing unit, compiled against the Windows SDK only.

- The compiler: d3dcompiler_47.dll loaded at the first compile (moved here
  from win32_d3d11.c: load_compiler and d3d11_backend_compile, unchanged),
  and its file version, which names the cache's folder.
- The cache on the disk: %LOCALAPPDATA%\halo\d3d11_shaders\<generator
  version>-<compiler version>\, one file a shader (<v|p><hash>.dxbc: a header
  with a CRC-32, then the bytecode), written as .tmp and moved into place;
  listed once at the start (no file opened), a file checked when it is asked
  for. The other versions' folders are removed at the start when they hold
  nothing but .dxbc and .tmp files. failed\ beside them holds the HLSL of the
  compiles that failed, for tools/d3d11_shader_check.py.
- The shaders by handle, compiled at the draw that first needs one
  (synchronously: the Decisions row "Shader compiler"), or made from their
  file; a vertex shader's bytecode kept for the layouts.
- The input layouts, by the hash of their elements and their vertex shader's.
- The blend, depth-stencil, rasterizer and sampler state objects, by the
  hash of their descriptions.

Nothing here is bound: phase 6's draws bind what the handles name. Everything
runs on the game's thread, as the device's calls do.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32_d3d11.h"
#include "hlsl_shaders.h"

/* the device's numbers for Direct3D's enumerants (d3d11_backend.h) */
_Static_assert(D3D11_BACKEND_FORMAT_R32G32B32A32_FLOAT == DXGI_FORMAT_R32G32B32A32_FLOAT, "format");
_Static_assert(D3D11_BACKEND_FORMAT_R32G32B32_FLOAT == DXGI_FORMAT_R32G32B32_FLOAT, "format");
_Static_assert(D3D11_BACKEND_FORMAT_R16G16B16A16_SNORM == DXGI_FORMAT_R16G16B16A16_SNORM, "format");
_Static_assert(D3D11_BACKEND_FORMAT_R16G16B16A16_SINT == DXGI_FORMAT_R16G16B16A16_SINT, "format");
_Static_assert(D3D11_BACKEND_FORMAT_R32G32_FLOAT == DXGI_FORMAT_R32G32_FLOAT, "format");
_Static_assert(D3D11_BACKEND_FORMAT_R8G8B8A8_UNORM == DXGI_FORMAT_R8G8B8A8_UNORM, "format");
_Static_assert(D3D11_BACKEND_FORMAT_R16G16_SNORM == DXGI_FORMAT_R16G16_SNORM, "format");
_Static_assert(D3D11_BACKEND_FORMAT_R16G16_SINT == DXGI_FORMAT_R16G16_SINT, "format");
_Static_assert(D3D11_BACKEND_FORMAT_R32_FLOAT == DXGI_FORMAT_R32_FLOAT, "format");
_Static_assert(D3D11_BACKEND_FORMAT_R32_UINT == DXGI_FORMAT_R32_UINT, "format");
_Static_assert(D3D11_BACKEND_FORMAT_R8G8_UNORM == DXGI_FORMAT_R8G8_UNORM, "format");
_Static_assert(D3D11_BACKEND_FORMAT_R16_SNORM == DXGI_FORMAT_R16_SNORM, "format");
_Static_assert(D3D11_BACKEND_FORMAT_R16_SINT == DXGI_FORMAT_R16_SINT, "format");
_Static_assert(D3D11_BACKEND_FORMAT_R8_UNORM == DXGI_FORMAT_R8_UNORM, "format");
_Static_assert(D3D11_BACKEND_FORMAT_B8G8R8A8_UNORM == DXGI_FORMAT_B8G8R8A8_UNORM, "format");
_Static_assert(D3D11_BACKEND_BLEND_ZERO == D3D11_BLEND_ZERO && D3D11_BACKEND_BLEND_ONE == D3D11_BLEND_ONE &&
	D3D11_BACKEND_BLEND_SRC_COLOR == D3D11_BLEND_SRC_COLOR && D3D11_BACKEND_BLEND_INV_SRC_COLOR == D3D11_BLEND_INV_SRC_COLOR &&
	D3D11_BACKEND_BLEND_SRC_ALPHA == D3D11_BLEND_SRC_ALPHA && D3D11_BACKEND_BLEND_INV_SRC_ALPHA == D3D11_BLEND_INV_SRC_ALPHA &&
	D3D11_BACKEND_BLEND_DEST_ALPHA == D3D11_BLEND_DEST_ALPHA &&
	D3D11_BACKEND_BLEND_INV_DEST_ALPHA == D3D11_BLEND_INV_DEST_ALPHA &&
	D3D11_BACKEND_BLEND_DEST_COLOR == D3D11_BLEND_DEST_COLOR &&
	D3D11_BACKEND_BLEND_INV_DEST_COLOR == D3D11_BLEND_INV_DEST_COLOR &&
	D3D11_BACKEND_BLEND_SRC_ALPHA_SAT == D3D11_BLEND_SRC_ALPHA_SAT &&
	D3D11_BACKEND_BLEND_BLEND_FACTOR == D3D11_BLEND_BLEND_FACTOR &&
	D3D11_BACKEND_BLEND_INV_BLEND_FACTOR == D3D11_BLEND_INV_BLEND_FACTOR, "blend");
_Static_assert(D3D11_BACKEND_BLEND_OP_ADD == D3D11_BLEND_OP_ADD && D3D11_BACKEND_BLEND_OP_SUBTRACT == D3D11_BLEND_OP_SUBTRACT &&
	D3D11_BACKEND_BLEND_OP_REV_SUBTRACT == D3D11_BLEND_OP_REV_SUBTRACT && D3D11_BACKEND_BLEND_OP_MIN == D3D11_BLEND_OP_MIN &&
	D3D11_BACKEND_BLEND_OP_MAX == D3D11_BLEND_OP_MAX, "blend op");
_Static_assert(D3D11_BACKEND_COMPARISON_NEVER == D3D11_COMPARISON_NEVER &&
	D3D11_BACKEND_COMPARISON_ALWAYS == D3D11_COMPARISON_ALWAYS && D3D11_COMPARISON_LESS == 2 &&
	D3D11_COMPARISON_EQUAL == 3 && D3D11_COMPARISON_LESS_EQUAL == 4 && D3D11_COMPARISON_GREATER == 5 &&
	D3D11_COMPARISON_NOT_EQUAL == 6 && D3D11_COMPARISON_GREATER_EQUAL == 7, "comparison");
_Static_assert(D3D11_BACKEND_STENCIL_OP_KEEP == D3D11_STENCIL_OP_KEEP && D3D11_BACKEND_STENCIL_OP_ZERO == D3D11_STENCIL_OP_ZERO &&
	D3D11_BACKEND_STENCIL_OP_REPLACE == D3D11_STENCIL_OP_REPLACE &&
	D3D11_BACKEND_STENCIL_OP_INCR_SAT == D3D11_STENCIL_OP_INCR_SAT &&
	D3D11_BACKEND_STENCIL_OP_DECR_SAT == D3D11_STENCIL_OP_DECR_SAT &&
	D3D11_BACKEND_STENCIL_OP_INVERT == D3D11_STENCIL_OP_INVERT && D3D11_BACKEND_STENCIL_OP_INCR == D3D11_STENCIL_OP_INCR &&
	D3D11_BACKEND_STENCIL_OP_DECR == D3D11_STENCIL_OP_DECR, "stencil op");
_Static_assert(D3D11_BACKEND_FILL_WIREFRAME == D3D11_FILL_WIREFRAME && D3D11_BACKEND_FILL_SOLID == D3D11_FILL_SOLID &&
	D3D11_BACKEND_CULL_NONE == D3D11_CULL_NONE && D3D11_BACKEND_CULL_FRONT == D3D11_CULL_FRONT &&
	D3D11_BACKEND_CULL_BACK == D3D11_CULL_BACK, "rasterizer");
_Static_assert(D3D11_BACKEND_FILTER(0, 0, 0) == D3D11_FILTER_MIN_MAG_MIP_POINT &&
	D3D11_BACKEND_FILTER(0, 0, 1) == D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR &&
	D3D11_BACKEND_FILTER(0, 1, 0) == D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT &&
	D3D11_BACKEND_FILTER(0, 1, 1) == D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR &&
	D3D11_BACKEND_FILTER(1, 0, 0) == D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT &&
	D3D11_BACKEND_FILTER(1, 0, 1) == D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR &&
	D3D11_BACKEND_FILTER(1, 1, 0) == D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT &&
	D3D11_BACKEND_FILTER(1, 1, 1) == D3D11_FILTER_MIN_MAG_MIP_LINEAR &&
	D3D11_BACKEND_FILTER_ANISOTROPIC == D3D11_FILTER_ANISOTROPIC, "filter");
_Static_assert(D3D11_BACKEND_ADDRESS_WRAP == D3D11_TEXTURE_ADDRESS_WRAP &&
	D3D11_BACKEND_ADDRESS_MIRROR == D3D11_TEXTURE_ADDRESS_MIRROR &&
	D3D11_BACKEND_ADDRESS_CLAMP == D3D11_TEXTURE_ADDRESS_CLAMP &&
	D3D11_BACKEND_ADDRESS_BORDER == D3D11_TEXTURE_ADDRESS_BORDER, "address");
_Static_assert(D3D11_BACKEND_CLEAR_R == D3D11_COLOR_WRITE_ENABLE_RED && D3D11_BACKEND_CLEAR_G == D3D11_COLOR_WRITE_ENABLE_GREEN &&
	D3D11_BACKEND_CLEAR_B == D3D11_COLOR_WRITE_ENABLE_BLUE && D3D11_BACKEND_CLEAR_A == D3D11_COLOR_WRITE_ENABLE_ALPHA,
	"write mask");
_Static_assert(D3D11_BACKEND_ATTRIBUTE_SLOT < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, "attribute slot");

/* the inputs a vertex shader can have (vs_4_0: sixteen) */
#define VERTEX_INPUTS 16

/* ---------- the compiler (win32_d3d11.c's, moved) */

static struct
{
	HMODULE library;
	pD3DCompile compile;
	/* the DLL's file version ("10.0.26100.1" or "unknown"), read once */
	char version[48];
} compiler;

/* d3dcompiler_47.dll, as the probe loads it (win32_d3d11_probe.c, load_compiler) */
static int load_compiler(void)
{
	if (compiler.compile)
		return 1;
	compiler.library = LoadLibraryA("d3dcompiler_47.dll");
	if (compiler.library)
		compiler.compile = (pD3DCompile)(void *)GetProcAddress(compiler.library, "D3DCompile");
	if (!compiler.compile)
	{
		platform_log("d3d11: the shader compiler is not available (d3dcompiler_47.dll: %s, error %lu): the "
			"clears that are drawn are skipped and the window stays black", compiler.library ? "no D3DCompile" :
			"not loaded", GetLastError());
		return 0;
	}
	return 1;
}

int d3d11_backend_compile(const char *source, size_t size, const char *name, const char *entry, const char *target,
	ID3DBlob **code)
{
	ID3DBlob *messages = NULL;
	HRESULT result;

	if (!load_compiler())
		return 0;
	result = compiler.compile(source, size, name, NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code,
		&messages);
	if (FAILED(result))
		platform_log("d3d11: the %s shader %s (%s) did not compile (%s): %.*s", name, entry, target, hr_text(result),
			messages ? (int)ID3D10Blob_GetBufferSize(messages) : 0,
			messages ? (const char *)ID3D10Blob_GetBufferPointer(messages) : "");
	release(&messages);
	return SUCCEEDED(result);
}

int d3d11_backend_shader_compiles(const char *source, size_t size, const char *name, const char *entry,
	const char *target)
{
	ID3DBlob *code = NULL;
	int compiled = d3d11_backend_compile(source, size, name, entry, target, &code);

	release(&code);
	return compiled;
}

/* the DLL's file version, as the probe reads it (load_compiler there), with
version.dll's functions found at run time (the executable links no
version.lib) */
static const char *compiler_version(void)
{
	typedef DWORD(WINAPI * size_function)(LPCSTR, LPDWORD);
	typedef BOOL(WINAPI * info_function)(LPCSTR, DWORD, DWORD, LPVOID);
	typedef BOOL(WINAPI * query_function)(LPCVOID, LPCSTR, LPVOID *, PUINT);
	char path[MAX_PATH];
	HMODULE version;

	if (compiler.version[0])
		return compiler.version;
	snprintf(compiler.version, sizeof(compiler.version), "unknown");
	if (!load_compiler() || !GetModuleFileNameA(compiler.library, path, sizeof(path)))
		return compiler.version;
	version = LoadLibraryA("version.dll");
	if (version)
	{
		size_function info_size = (size_function)(void *)GetProcAddress(version, "GetFileVersionInfoSizeA");
		info_function info = (info_function)(void *)GetProcAddress(version, "GetFileVersionInfoA");
		query_function query = (query_function)(void *)GetProcAddress(version, "VerQueryValueA");
		DWORD handle = 0, size = info_size ? info_size(path, &handle) : 0;
		void *data = size && info && query ? malloc(size) : NULL;
		VS_FIXEDFILEINFO *fixed;
		UINT length;

		if (data && info(path, 0, size, data) && query(data, "\\", (void **)&fixed, &length) &&
			length >= sizeof(*fixed))
		{
			snprintf(compiler.version, sizeof(compiler.version), "%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS),
				LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
		}
		free(data);
		FreeLibrary(version);
	}
	return compiler.version;
}

/* ---------- small things */

/* FNV-1a, 64 bits */
static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
	const unsigned char *bytes = data;
	size_t index;

	for (index = 0; index < size; index++)
		hash = (hash ^ bytes[index]) * 1099511628211ULL;
	return hash;
}

#define HASH_START 14695981039346656037ULL

/* CRC-32 (the zip's: reflected, 0xedb88320), from a table made at the first use */
static uint32_t crc32_of(const void *data, size_t size)
{
	static uint32_t table[256];
	static int made;
	const unsigned char *bytes = data;
	uint32_t crc = 0xffffffffu;
	size_t index;

	if (!made)
	{
		uint32_t value, bit;

		for (index = 0; index < 256; index++)
		{
			value = (uint32_t)index;
			for (bit = 0; bit < 8; bit++)
				value = (value & 1) ? 0xedb88320u ^ (value >> 1) : value >> 1;
			table[index] = value;
		}
		made = 1;
	}
	for (index = 0; index < size; index++)
		crc = table[(crc ^ bytes[index]) & 0xff] ^ (crc >> 8);
	return crc ^ 0xffffffffu;
}

static double now_microseconds(void)
{
	static LARGE_INTEGER frequency;
	LARGE_INTEGER counter;

	if (!frequency.QuadPart)
		QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&counter);
	return (double)counter.QuadPart * 1000000.0 / (double)frequency.QuadPart;
}

/* a map of 64-bit keys to handles (from 1), open addressing; the keys are
hashes, so the low bits index */
struct index
{
	uint64_t *keys;
	uint32_t *values;
	uint32_t capacity, count;
};

static uint32_t index_find(const struct index *index, uint64_t key)
{
	uint32_t slot;

	if (!index->capacity)
		return 0;
	for (slot = (uint32_t)key & (index->capacity - 1); index->values[slot]; slot = (slot + 1) & (index->capacity - 1))
	{
		if (index->keys[slot] == key)
			return index->values[slot];
	}
	return 0;
}

static int index_add(struct index *index, uint64_t key, uint32_t value)
{
	uint32_t slot;

	if ((index->count + 1) * 2 > index->capacity)
	{
		struct index grown;
		uint32_t old;

		grown.capacity = index->capacity ? index->capacity * 2 : 256;
		grown.count = 0;
		grown.keys = calloc(grown.capacity, sizeof(*grown.keys));
		grown.values = calloc(grown.capacity, sizeof(*grown.values));
		if (!grown.keys || !grown.values)
		{
			free(grown.keys);
			free(grown.values);
			return 0;
		}
		for (old = 0; old < index->capacity; old++)
		{
			if (index->values[old])
				index_add(&grown, index->keys[old], index->values[old]);
		}
		free(index->keys);
		free(index->values);
		*index = grown;
	}
	for (slot = (uint32_t)key & (index->capacity - 1); index->values[slot]; slot = (slot + 1) & (index->capacity - 1))
	{
		if (index->keys[slot] == key)
		{
			index->values[slot] = value;
			return 1;
		}
	}
	index->keys[slot] = key;
	index->values[slot] = value;
	index->count++;
	return 1;
}

static void index_free(struct index *index)
{
	free(index->keys);
	free(index->values);
	memset(index, 0, sizeof(*index));
}

/* a table of objects by handle (from 1), grown as needed */
struct objects
{
	struct index index;
	IUnknown **items;
	uint32_t count, capacity;
};

/* the handle of a new object (taken by the table), or 0 */
static uint32_t objects_add(struct objects *objects, uint64_t key, IUnknown *object)
{
	if (objects->count == objects->capacity)
	{
		uint32_t capacity = objects->capacity ? objects->capacity * 2 : 64;
		IUnknown **grown = realloc(objects->items, capacity * sizeof(*grown));

		if (!grown)
			return 0;
		objects->items = grown;
		objects->capacity = capacity;
	}
	if (!index_add(&objects->index, key, objects->count + 1))
		return 0;
	objects->items[objects->count++] = object;
	return objects->count;
}

static void objects_free(struct objects *objects)
{
	uint32_t index;

	for (index = 0; index < objects->count; index++)
		release(&objects->items[index]);
	free(objects->items);
	index_free(&objects->index);
	memset(objects, 0, sizeof(*objects));
}

/* ---------- the shaders and the cache */

struct shader
{
	uint64_t hash;
	uint32_t stage;
	/* NULL until made, or after the self-test let it go */
	IUnknown *object;
	/* a vertex shader's bytecode (the layouts need it) and its input
	signature: the TEXCOORD<n> it declares and those it reads */
	void *bytecode;
	uint32_t bytecode_size;
	uint32_t inputs_declared, inputs_read;
	/* its compile failed this run (logged once) */
	int failed;
};

static struct
{
	/* d3d11_backend_shader_cache_enable was called */
	int said;
	/* the folder is usable */
	int on;
	char root[MAX_PATH];
	char folder[MAX_PATH];
	char failed_folder[MAX_PATH];
	/* the files the folder had at the start, and those written since (1),
	or deleted since (FILE_GONE: an index's entry is never removed): the keys
	are the hashes with the top bit flipped for pixel shaders (hashes are
	FNV-1a over different inputs, so this only keeps the two stages apart) */
	struct index files;
	/* a write failed (logged once) */
	int write_failed_logged;

	struct shader *shaders;
	uint32_t shader_count, shader_capacity;
	struct index by_stage[2];

	struct objects layouts, blends, depth_stencils, rasterizers, samplers;
	uint32_t unfed_layouts;
	/* a state object, a layout or a shader's object that failed, logged once
	each kind */
	int object_failed_logged[6];

	uint32_t cached, compiled, failed;
	double cached_microseconds, compile_microseconds;
} C;

#define FILE_GONE 2u

static uint64_t file_key(uint32_t stage, uint64_t hash)
{
	return stage == D3D11_BACKEND_STAGE_PIXEL ? hash ^ 0x8000000000000000ULL : hash;
}

static void shader_file_name(char *name, size_t size, uint32_t stage, uint64_t hash, const char *extension)
{
	snprintf(name, size, "%c%016llx%s", stage == D3D11_BACKEND_STAGE_VERTEX ? 'v' : 'p', (unsigned long long)hash,
		extension);
}

static void shader_path(char *path, size_t size, const char *folder, uint32_t stage, uint64_t hash,
	const char *extension)
{
	char name[40];

	shader_file_name(name, sizeof(name), stage, hash, extension);
	snprintf(path, size, "%s\\%s", folder, name);
}

/* "v0123456789abcdef.dxbc": the stage and the hash; 0 if the name is not one */
static int parse_shader_file_name(const char *name, uint32_t *stage, uint64_t *hash)
{
	uint64_t value = 0;
	int index;

	if ((name[0] != 'v' && name[0] != 'p') || strlen(name) != 22 || _stricmp(name + 17, ".dxbc"))
		return 0;
	for (index = 1; index <= 16; index++)
	{
		char c = name[index];
		int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;

		if (digit < 0)
			return 0;
		value = value << 4 | (uint64_t)digit;
	}
	*stage = name[0] == 'v' ? D3D11_BACKEND_STAGE_VERTEX : D3D11_BACKEND_STAGE_PIXEL;
	*hash = value;
	return 1;
}

static int has_extension(const char *name, const char *extension)
{
	size_t length = strlen(name), extension_length = strlen(extension);

	return length >= extension_length && !_stricmp(name + length - extension_length, extension);
}

/* another version's folder under d3d11_shaders\: its files deleted and the
folder removed when it holds nothing but .dxbc and .tmp files; else left,
and logged */
static void remove_other_version(const char *name)
{
	char pattern[MAX_PATH], path[MAX_PATH];
	WIN32_FIND_DATAA found;
	HANDLE search;
	uint32_t files = 0;
	char stray[MAX_PATH] = "";

	snprintf(pattern, sizeof(pattern), "%s\\%s\\*", C.root, name);
	search = FindFirstFileA(pattern, &found);
	if (search != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, ".."))
				continue;
			if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
				(!has_extension(found.cFileName, ".dxbc") && !has_extension(found.cFileName, ".tmp")))
			{
				snprintf(stray, sizeof(stray), "%s", found.cFileName);
				break;
			}
			files++;
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	if (stray[0])
	{
		platform_log("d3d11: the shader cache's folder %s (another version's) is left as it is: it holds %s, which "
			"the cache did not write", name, stray);
		return;
	}
	search = FindFirstFileA(pattern, &found);
	if (search != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			snprintf(path, sizeof(path), "%s\\%s\\%s", C.root, name, found.cFileName);
			DeleteFileA(path);
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	snprintf(path, sizeof(path), "%s\\%s", C.root, name);
	if (RemoveDirectoryA(path))
		platform_log("d3d11: the shader cache's folder %s (another version's) was removed, with its %u files", name,
			files);
	else
		platform_log("d3d11: the shader cache's folder %s (another version's) was not removed (error %lu)", name,
			GetLastError());
}

static int make_folder(const char *path)
{
	return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

void d3d11_backend_shader_cache_enable(int enabled)
{
	char local[MAX_PATH], pattern[MAX_PATH], version_name[96];
	WIN32_FIND_DATAA found;
	HANDLE search;
	DWORD length;
	uint32_t files = 0;

	if (C.said)
		return;
	C.said = 1;
	if (!enabled)
	{
		C.on = 0;
		platform_log("d3d11: shader cache off (a debug.gpu_debug_* setting changes the shaders' text): every shader "
			"is compiled, and none is read or written");
		return;
	}
	length = GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof(local));
	if (!length || length >= sizeof(local))
	{
		platform_log("d3d11: no shader cache (LOCALAPPDATA is not set): every shader is compiled each run");
		return;
	}
	snprintf(version_name, sizeof(version_name), "%d-%s", HLSL_SHADER_GENERATOR_VERSION, compiler_version());
	snprintf(pattern, sizeof(pattern), "%s\\halo", local);
	snprintf(C.root, sizeof(C.root), "%s\\halo\\d3d11_shaders", local);
	snprintf(C.folder, sizeof(C.folder), "%s\\%s", C.root, version_name);
	snprintf(C.failed_folder, sizeof(C.failed_folder), "%s\\failed", C.root);
	if (!make_folder(pattern) || !make_folder(C.root) || !make_folder(C.folder))
	{
		platform_log("d3d11: no shader cache (%s could not be made: error %lu): every shader is compiled each run",
			C.folder, GetLastError());
		return;
	}
	/* the other folders: other versions', removed when they hold only the
	cache's files */
	snprintf(pattern, sizeof(pattern), "%s\\*", C.root);
	search = FindFirstFileA(pattern, &found);
	if (search != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !strcmp(found.cFileName, ".") ||
				!strcmp(found.cFileName, "..") || !_stricmp(found.cFileName, "failed") ||
				!_stricmp(found.cFileName, version_name))
			{
				continue;
			}
			remove_other_version(found.cFileName);
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	/* this version's files, by name only */
	snprintf(pattern, sizeof(pattern), "%s\\*.dxbc", C.folder);
	search = FindFirstFileA(pattern, &found);
	if (search != INVALID_HANDLE_VALUE)
	{
		do
		{
			uint32_t stage;
			uint64_t hash;

			if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
				parse_shader_file_name(found.cFileName, &stage, &hash) && index_add(&C.files, file_key(stage, hash), 1))
			{
				files++;
			}
		} while (FindNextFileA(search, &found));
		FindClose(search);
	}
	C.on = 1;
	platform_log("d3d11: shader cache %s: %u shaders on the disk", C.folder, files);
}

/* the file's header: magic, the format's version, the generator's, the hash,
the target, the bytecode's size and CRC-32 */
#define CACHE_MAGIC "HDXB"
#define CACHE_FORMAT_VERSION 1u
#define CACHE_HEADER_SIZE 40

static void header_make(unsigned char header[CACHE_HEADER_SIZE], uint64_t hash, const char *target, const void *code,
	uint32_t size)
{
	uint32_t format = CACHE_FORMAT_VERSION, generator = HLSL_SHADER_GENERATOR_VERSION, reserved = 0, crc;
	char target_bytes[8] = { 0 };

	crc = crc32_of(code, size);
	strncpy(target_bytes, target, sizeof(target_bytes) - 1);
	memcpy(header, CACHE_MAGIC, 4);
	memcpy(header + 4, &format, 4);
	memcpy(header + 8, &generator, 4);
	memcpy(header + 12, &reserved, 4);
	memcpy(header + 16, &hash, 8);
	memcpy(header + 24, target_bytes, 8);
	memcpy(header + 32, &size, 4);
	memcpy(header + 36, &crc, 4);
}

/* the file written as .tmp and moved into place; a failure logged once, the
.tmp left */
static void cache_write(uint32_t stage, uint64_t hash, const char *target, const void *code, uint32_t size)
{
	char temporary[MAX_PATH], path[MAX_PATH];
	unsigned char header[CACHE_HEADER_SIZE];
	HANDLE file;
	DWORD written = 0, written_code = 0;
	BOOL ok;

	if (!C.on)
		return;
	shader_path(temporary, sizeof(temporary), C.folder, stage, hash, ".tmp");
	shader_path(path, sizeof(path), C.folder, stage, hash, ".dxbc");
	header_make(header, hash, target, code, size);
	file = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	ok = file != INVALID_HANDLE_VALUE;
	if (ok)
	{
		ok = WriteFile(file, header, CACHE_HEADER_SIZE, &written, NULL) && written == CACHE_HEADER_SIZE &&
			WriteFile(file, code, size, &written_code, NULL) && written_code == size;
		ok = CloseHandle(file) && ok;
	}
	if (ok)
		ok = MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING);
	if (!ok)
	{
		if (!C.write_failed_logged)
		{
			C.write_failed_logged = 1;
			platform_log("d3d11: the shader cache's file %s was not written (error %lu; the .tmp may be left; logged "
				"once): the shader is compiled again next run", path, GetLastError());
		}
		return;
	}
	index_add(&C.files, file_key(stage, hash), 1);
}

/* the bytecode of the file in the cache (malloc'd), checked; NULL if there is
none, or it was damaged (deleted and logged) */
static void *cache_read(uint32_t stage, uint64_t hash, uint32_t *code_size)
{
	char path[MAX_PATH], why[96] = "";
	unsigned char header[CACHE_HEADER_SIZE];
	HANDLE file;
	LARGE_INTEGER size;
	DWORD read = 0;
	unsigned char *data = NULL;
	uint32_t stored_size, stored_crc, format, generator;
	uint64_t stored_hash;
	char target[9] = { 0 };

	if (!C.on || index_find(&C.files, file_key(stage, hash)) != 1)
		return NULL;
	shader_path(path, sizeof(path), C.folder, stage, hash, ".dxbc");
	file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
	{
		/* (gone since the start: compiled again) */
		index_add(&C.files, file_key(stage, hash), FILE_GONE);
		return NULL;
	}
	if (!GetFileSizeEx(file, &size) || size.QuadPart < CACHE_HEADER_SIZE || size.QuadPart > 16 * 1024 * 1024)
	{
		snprintf(why, sizeof(why), "its size");
	}
	else
	{
		data = malloc((size_t)size.QuadPart);
		if (!data || !ReadFile(file, data, (DWORD)size.QuadPart, &read, NULL) || read != (DWORD)size.QuadPart)
			snprintf(why, sizeof(why), "not read");
	}
	CloseHandle(file);
	if (!why[0])
	{
		memcpy(header, data, CACHE_HEADER_SIZE);
		memcpy(&format, header + 4, 4);
		memcpy(&generator, header + 8, 4);
		memcpy(&stored_hash, header + 16, 8);
		memcpy(target, header + 24, 8);
		memcpy(&stored_size, header + 32, 4);
		memcpy(&stored_crc, header + 36, 4);
		if (memcmp(header, CACHE_MAGIC, 4) || format != CACHE_FORMAT_VERSION || generator != HLSL_SHADER_GENERATOR_VERSION)
			snprintf(why, sizeof(why), "its header");
		else if (stored_hash != hash || strncmp(target, stage == D3D11_BACKEND_STAGE_VERTEX ? "vs_" : "ps_", 3))
			snprintf(why, sizeof(why), "another shader's header");
		else if ((uint64_t)stored_size + CACHE_HEADER_SIZE != (uint64_t)size.QuadPart)
			snprintf(why, sizeof(why), "its size: %u bytes of bytecode named, %lld in the file", stored_size,
				(long long)size.QuadPart - CACHE_HEADER_SIZE);
		else if (crc32_of(data + CACHE_HEADER_SIZE, stored_size) != stored_crc)
			snprintf(why, sizeof(why), "its CRC");
	}
	if (why[0])
	{
		free(data);
		DeleteFileA(path);
		index_add(&C.files, file_key(stage, hash), FILE_GONE);
		platform_log("d3d11: the cached shader %s is damaged (%s, %lld bytes); deleted, to be compiled again", path, why,
			(long long)size.QuadPart);
		return NULL;
	}
	memmove(data, data + CACHE_HEADER_SIZE, stored_size);
	*code_size = stored_size;
	return data;
}

/* the TEXCOORD<n> inputs of a vertex shader's bytecode (its ISGN chunk):
those declared and those it reads */
static void input_signature(const unsigned char *code, uint32_t size, uint32_t *declared, uint32_t *read)
{
	uint32_t chunks, chunk, offset, chunk_size, count, element;

	*declared = *read = 0;
	if (size < 32 || memcmp(code, "DXBC", 4))
		return;
	memcpy(&chunks, code + 28, 4);
	for (chunk = 0; chunk < chunks && 32 + chunk * 4 + 4 <= size; chunk++)
	{
		const unsigned char *data;

		memcpy(&offset, code + 32 + chunk * 4, 4);
		if (offset + 8 > size || memcmp(code + offset, "ISGN", 4))
			continue;
		memcpy(&chunk_size, code + offset + 4, 4);
		data = code + offset + 8;
		if (offset + 8 + (uint64_t)chunk_size > size || chunk_size < 8)
			return;
		memcpy(&count, data, 4);
		for (element = 0; element < count && 8 + (element + 1) * 24 <= chunk_size; element++)
		{
			const unsigned char *entry = data + 8 + element * 24;
			uint32_t name, semantic_index, system_value;

			memcpy(&name, entry, 4);
			memcpy(&semantic_index, entry + 4, 4);
			memcpy(&system_value, entry + 8, 4);
			if (name >= chunk_size || system_value || semantic_index >= VERTEX_INPUTS ||
				strncmp((const char *)data + name, "TEXCOORD", chunk_size - name))
			{
				continue;
			}
			*declared |= 1u << semantic_index;
			if (entry[21])
				*read |= 1u << semantic_index;
		}
		return;
	}
}

static void object_failed(int kind, const char *what, HRESULT result, uint64_t hash)
{
	if (C.object_failed_logged[kind])
		return;
	C.object_failed_logged[kind] = 1;
	platform_log("d3d11: %s %016llx was not made (%s; logged once)", what, (unsigned long long)hash, hr_text(result));
}

/* the shader's slot, made if the hash has none; NULL without memory */
static struct shader *shader_slot(uint32_t stage, uint64_t hash)
{
	uint32_t handle = index_find(&C.by_stage[stage], hash);
	struct shader *shader;

	if (handle)
		return &C.shaders[handle - 1];
	if (C.shader_count == C.shader_capacity)
	{
		uint32_t capacity = C.shader_capacity ? C.shader_capacity * 2 : 256;
		struct shader *grown = realloc(C.shaders, capacity * sizeof(*grown));

		if (!grown)
			return NULL;
		C.shaders = grown;
		C.shader_capacity = capacity;
	}
	if (!index_add(&C.by_stage[stage], hash, C.shader_count + 1))
		return NULL;
	shader = &C.shaders[C.shader_count++];
	memset(shader, 0, sizeof(*shader));
	shader->hash = hash;
	shader->stage = stage;
	return shader;
}

static uint32_t shader_handle(const struct shader *shader)
{
	return (uint32_t)(shader - C.shaders) + 1;
}

/* the object made from the bytecode (taken: kept for a vertex shader, freed
for a pixel shader); 0 if it was not made */
static int shader_object_make(struct shader *shader, void *code, uint32_t size)
{
	HRESULT result;

	if (shader->stage == D3D11_BACKEND_STAGE_VERTEX)
		result = ID3D11Device_CreateVertexShader(B.device, code, size, NULL, (ID3D11VertexShader **)&shader->object);
	else
		result = ID3D11Device_CreatePixelShader(B.device, code, size, NULL, (ID3D11PixelShader **)&shader->object);
	if (backend_failed(result, "a shader object"))
	{
		shader->object = NULL;
		object_failed(0, shader->stage == D3D11_BACKEND_STAGE_VERTEX ? "the vertex shader" : "the pixel shader", result,
			shader->hash);
		free(code);
		return 0;
	}
	if (shader->stage == D3D11_BACKEND_STAGE_VERTEX)
	{
		free(shader->bytecode);
		shader->bytecode = code;
		shader->bytecode_size = size;
		input_signature(code, size, &shader->inputs_declared, &shader->inputs_read);
	}
	else
	{
		free(code);
	}
	return 1;
}

uint32_t d3d11_backend_shader_find(uint32_t stage, uint64_t hash)
{
	uint32_t handle, size = 0;
	struct shader *shader;
	void *code;
	double start;

	if (!B.device || B.lost || stage > D3D11_BACKEND_STAGE_PIXEL)
		return 0;
	handle = index_find(&C.by_stage[stage], hash);
	if (handle && C.shaders[handle - 1].object)
		return handle;
	if (handle && C.shaders[handle - 1].failed)
		return 0;
	start = now_microseconds();
	code = cache_read(stage, hash, &size);
	if (!code)
		return 0;
	shader = shader_slot(stage, hash);
	if (!shader)
	{
		free(code);
		return 0;
	}
	if (!shader_object_make(shader, code, size))
		return 0;
	C.cached++;
	C.cached_microseconds += now_microseconds() - start;
	return shader_handle(shader);
}

/* the failed compile's HLSL in failed\, for tools/d3d11_shader_check.py */
static void failed_write(uint32_t stage, uint64_t hash, const char *source, size_t size)
{
	char path[MAX_PATH];
	FILE *file;

	if (!C.on || !make_folder(C.failed_folder))
		return;
	shader_path(path, sizeof(path), C.failed_folder, stage, hash, ".hlsl");
	file = fopen(path, "wb");
	if (file)
	{
		fwrite(source, 1, size, file);
		fclose(file);
	}
}

uint32_t d3d11_backend_shader_make(uint32_t stage, uint64_t hash, const char *source, size_t size, const char *entry,
	const char *target)
{
	struct shader *shader;
	ID3DBlob *code = NULL, *messages = NULL;
	HRESULT result;
	double start;
	void *copy;
	uint32_t code_size;
	char name[40];

	if (!B.device || B.lost || stage > D3D11_BACKEND_STAGE_PIXEL)
		return 0;
	shader = shader_slot(stage, hash);
	if (!shader)
		return 0;
	if (shader->object)
		return shader_handle(shader);
	if (shader->failed || !load_compiler())
		return 0;
	/* (the name in the compiler's messages: the cache's, as failed\ has it) */
	shader_file_name(name, sizeof(name), stage, hash, ".hlsl");
	start = now_microseconds();
	result = compiler.compile(source, size, name, NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
		&messages);
	C.compile_microseconds += now_microseconds() - start;
	if (FAILED(result))
	{
		shader->failed = 1;
		C.failed++;
		platform_log("d3d11: the %s shader %s (%s) did not compile (%s): %.*s", stage == D3D11_BACKEND_STAGE_VERTEX ?
			"vertex" : "pixel", name, target, hr_text(result), messages ? (int)ID3D10Blob_GetBufferSize(messages) : 0,
			messages ? (const char *)ID3D10Blob_GetBufferPointer(messages) : "");
		failed_write(stage, hash, source, size);
		release(&messages);
		release(&code);
		return 0;
	}
	release(&messages);
	C.compiled++;
	code_size = (uint32_t)ID3D10Blob_GetBufferSize(code);
	copy = malloc(code_size);
	if (!copy)
	{
		release(&code);
		return 0;
	}
	memcpy(copy, ID3D10Blob_GetBufferPointer(code), code_size);
	release(&code);
	cache_write(stage, hash, target, copy, code_size);
	if (!shader_object_make(shader, copy, code_size))
		return 0;
	return shader_handle(shader);
}

/* the self-test's: the shader's object let go, as if this run had not made
it (its file stays) */
static void shader_forget(uint32_t handle)
{
	struct shader *shader = &C.shaders[handle - 1];

	release(&shader->object);
	free(shader->bytecode);
	shader->bytecode = NULL;
	shader->bytecode_size = 0;
}

/* ---------- input layouts */

uint32_t d3d11_backend_layout_make(const struct d3d11_backend_layout_element *elements, uint32_t count,
	uint32_t vertex_shader)
{
	D3D11_INPUT_ELEMENT_DESC descriptions[VERTEX_INPUTS * 2];
	const struct shader *shader;
	ID3D11InputLayout *layout = NULL;
	uint32_t index, handle, fed = 0, made = 0;
	uint64_t key;
	HRESULT result;

	if (!B.device || B.lost || !vertex_shader || vertex_shader > C.shader_count || count > VERTEX_INPUTS)
		return 0;
	shader = &C.shaders[vertex_shader - 1];
	if (shader->stage != D3D11_BACKEND_STAGE_VERTEX || !shader->bytecode)
		return 0;
	key = hash_bytes(hash_bytes(hash_bytes(HASH_START, &shader->hash, sizeof(shader->hash)), &count, sizeof(count)),
		elements, count * sizeof(*elements));
	handle = index_find(&C.layouts.index, key);
	if (handle)
		return handle;
	for (index = 0; index < count; index++)
	{
		D3D11_INPUT_ELEMENT_DESC *description = &descriptions[made++];

		description->SemanticName = "TEXCOORD";
		description->SemanticIndex = elements[index].semantic_index;
		description->Format = (DXGI_FORMAT)elements[index].format;
		description->InputSlot = elements[index].slot;
		description->AlignedByteOffset = elements[index].offset;
		description->InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
		description->InstanceDataStepRate = 0;
		if (elements[index].semantic_index < VERTEX_INPUTS)
			fed |= 1u << elements[index].semantic_index;
	}
	/* the inputs the shader declares and the elements leave out, from the
	attribute slot */
	for (index = 0; index < VERTEX_INPUTS; index++)
	{
		D3D11_INPUT_ELEMENT_DESC *description;

		if (!(shader->inputs_declared & ~fed & (1u << index)))
			continue;
		description = &descriptions[made++];
		description->SemanticName = "TEXCOORD";
		description->SemanticIndex = index;
		description->Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		description->InputSlot = D3D11_BACKEND_ATTRIBUTE_SLOT;
		description->AlignedByteOffset = index * 16;
		description->InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
		description->InstanceDataStepRate = 0;
	}
	result = ID3D11Device_CreateInputLayout(B.device, descriptions, made, shader->bytecode, shader->bytecode_size,
		&layout);
	if (backend_failed(result, "an input layout"))
	{
		object_failed(1, "the input layout for the vertex shader", result, shader->hash);
		return 0;
	}
	handle = objects_add(&C.layouts, key, (IUnknown *)layout);
	if (!handle)
	{
		release(&layout);
		return 0;
	}
	/* (an input the shader reads that the elements leave out: what phase 6's
	attribute slot is for; the first few said) */
	if (shader->inputs_read & ~fed)
	{
		if (C.unfed_layouts++ < 16)
			platform_log("d3d11: the layout %u for the vertex shader v%016llx reads inputs from the attribute slot "
				"(TEXCOORD mask %04x; the elements feed %04x)", handle, (unsigned long long)shader->hash,
				shader->inputs_read & ~fed, fed);
	}
	return handle;
}

/* ---------- state objects */

static uint64_t description_key(const void *description, size_t size)
{
	return hash_bytes(HASH_START, description, size);
}

uint32_t d3d11_backend_blend_make(const struct d3d11_backend_blend *in)
{
	uint64_t key = description_key(in, sizeof(*in));
	uint32_t handle = index_find(&C.blends.index, key);
	D3D11_BLEND_DESC description;
	ID3D11BlendState *state = NULL;
	HRESULT result;

	if (handle || !B.device || B.lost)
		return handle;
	memset(&description, 0, sizeof(description));
	description.AlphaToCoverageEnable = FALSE;
	description.IndependentBlendEnable = FALSE;
	description.RenderTarget[0].BlendEnable = in->enable ? TRUE : FALSE;
	description.RenderTarget[0].SrcBlend = (D3D11_BLEND)in->source;
	description.RenderTarget[0].DestBlend = (D3D11_BLEND)in->destination;
	description.RenderTarget[0].BlendOp = (D3D11_BLEND_OP)in->operation;
	description.RenderTarget[0].SrcBlendAlpha = (D3D11_BLEND)in->source_alpha;
	description.RenderTarget[0].DestBlendAlpha = (D3D11_BLEND)in->destination_alpha;
	description.RenderTarget[0].BlendOpAlpha = (D3D11_BLEND_OP)in->operation_alpha;
	description.RenderTarget[0].RenderTargetWriteMask = (UINT8)(in->write_mask & D3D11_COLOR_WRITE_ENABLE_ALL);
	result = ID3D11Device_CreateBlendState(B.device, &description, &state);
	if (backend_failed(result, "a blend state"))
	{
		object_failed(2, "the blend state", result, key);
		return 0;
	}
	handle = objects_add(&C.blends, key, (IUnknown *)state);
	if (!handle)
		release(&state);
	return handle;
}

uint32_t d3d11_backend_depth_stencil_make(const struct d3d11_backend_depth_stencil *in)
{
	uint64_t key = description_key(in, sizeof(*in));
	uint32_t handle = index_find(&C.depth_stencils.index, key);
	D3D11_DEPTH_STENCIL_DESC description;
	ID3D11DepthStencilState *state = NULL;
	HRESULT result;

	if (handle || !B.device || B.lost)
		return handle;
	memset(&description, 0, sizeof(description));
	description.DepthEnable = in->depth_enable ? TRUE : FALSE;
	description.DepthWriteMask = in->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
	description.DepthFunc = (D3D11_COMPARISON_FUNC)in->depth_function;
	description.StencilEnable = in->stencil_enable ? TRUE : FALSE;
	description.StencilReadMask = (UINT8)in->stencil_read_mask;
	description.StencilWriteMask = (UINT8)in->stencil_write_mask;
	description.FrontFace.StencilFailOp = (D3D11_STENCIL_OP)in->stencil_fail;
	description.FrontFace.StencilDepthFailOp = (D3D11_STENCIL_OP)in->stencil_depth_fail;
	description.FrontFace.StencilPassOp = (D3D11_STENCIL_OP)in->stencil_pass;
	description.FrontFace.StencilFunc = (D3D11_COMPARISON_FUNC)in->stencil_function;
	description.BackFace = description.FrontFace;
	result = ID3D11Device_CreateDepthStencilState(B.device, &description, &state);
	if (backend_failed(result, "a depth-stencil state"))
	{
		object_failed(3, "the depth-stencil state", result, key);
		return 0;
	}
	handle = objects_add(&C.depth_stencils, key, (IUnknown *)state);
	if (!handle)
		release(&state);
	return handle;
}

uint32_t d3d11_backend_rasterizer_make(const struct d3d11_backend_rasterizer *in)
{
	uint64_t key = description_key(in, sizeof(*in));
	uint32_t handle = index_find(&C.rasterizers.index, key);
	D3D11_RASTERIZER_DESC description;
	ID3D11RasterizerState *state = NULL;
	HRESULT result;

	if (handle || !B.device || B.lost)
		return handle;
	memset(&description, 0, sizeof(description));
	description.FillMode = (D3D11_FILL_MODE)in->fill;
	description.CullMode = (D3D11_CULL_MODE)in->cull;
	description.FrontCounterClockwise = in->front_counter_clockwise ? TRUE : FALSE;
	description.DepthBias = in->depth_bias;
	description.DepthBiasClamp = in->depth_bias_clamp;
	description.SlopeScaledDepthBias = in->slope_scaled_depth_bias;
	description.DepthClipEnable = in->depth_clip ? TRUE : FALSE;
	description.ScissorEnable = in->scissor ? TRUE : FALSE;
	description.MultisampleEnable = in->multisample ? TRUE : FALSE;
	description.AntialiasedLineEnable = in->antialiased_line ? TRUE : FALSE;
	result = ID3D11Device_CreateRasterizerState(B.device, &description, &state);
	if (backend_failed(result, "a rasterizer state"))
	{
		object_failed(4, "the rasterizer state", result, key);
		return 0;
	}
	handle = objects_add(&C.rasterizers, key, (IUnknown *)state);
	if (!handle)
		release(&state);
	return handle;
}

uint32_t d3d11_backend_sampler_make(const struct d3d11_backend_sampler *in)
{
	uint64_t key = description_key(in, sizeof(*in));
	uint32_t handle = index_find(&C.samplers.index, key);
	D3D11_SAMPLER_DESC description;
	ID3D11SamplerState *state = NULL;
	HRESULT result;

	if (handle || !B.device || B.lost)
		return handle;
	memset(&description, 0, sizeof(description));
	description.Filter = (D3D11_FILTER)in->filter;
	description.AddressU = (D3D11_TEXTURE_ADDRESS_MODE)in->address_u;
	description.AddressV = (D3D11_TEXTURE_ADDRESS_MODE)in->address_v;
	description.AddressW = (D3D11_TEXTURE_ADDRESS_MODE)in->address_w;
	description.MipLODBias = in->mip_lod_bias;
	description.MaxAnisotropy = in->max_anisotropy;
	description.ComparisonFunc = (D3D11_COMPARISON_FUNC)in->comparison;
	memcpy(description.BorderColor, in->border_color, sizeof(description.BorderColor));
	description.MinLOD = in->min_lod;
	description.MaxLOD = in->max_lod;
	result = ID3D11Device_CreateSamplerState(B.device, &description, &state);
	if (backend_failed(result, "a sampler state"))
	{
		object_failed(5, "the sampler state", result, key);
		return 0;
	}
	handle = objects_add(&C.samplers, key, (IUnknown *)state);
	if (!handle)
		release(&state);
	return handle;
}

/* ---------- the statistics, the release */

void d3d11_backend_shaders_statistics(uint32_t counts[D3D11_BACKEND_STATISTICS])
{
	counts[D3D11_BACKEND_STATISTIC_CACHED] = C.cached;
	counts[D3D11_BACKEND_STATISTIC_CACHED_MICROSECONDS] = (uint32_t)C.cached_microseconds;
	counts[D3D11_BACKEND_STATISTIC_COMPILED] = C.compiled;
	counts[D3D11_BACKEND_STATISTIC_COMPILE_MICROSECONDS] = (uint32_t)C.compile_microseconds;
	counts[D3D11_BACKEND_STATISTIC_FAILED] = C.failed;
	counts[D3D11_BACKEND_STATISTIC_LAYOUTS] = C.layouts.count;
	counts[D3D11_BACKEND_STATISTIC_UNFED_LAYOUTS] = C.unfed_layouts;
	counts[D3D11_BACKEND_STATISTIC_BLEND] = C.blends.count;
	counts[D3D11_BACKEND_STATISTIC_DEPTH_STENCIL] = C.depth_stencils.count;
	counts[D3D11_BACKEND_STATISTIC_RASTERIZER] = C.rasterizers.count;
	counts[D3D11_BACKEND_STATISTIC_SAMPLERS] = C.samplers.count;
}

void d3d11_backend_shaders_release(void)
{
	uint32_t index;

	objects_free(&C.samplers);
	objects_free(&C.rasterizers);
	objects_free(&C.depth_stencils);
	objects_free(&C.blends);
	objects_free(&C.layouts);
	for (index = 0; index < C.shader_count; index++)
		shader_forget(index + 1);
	free(C.shaders);
	C.shaders = NULL;
	C.shader_count = C.shader_capacity = 0;
	index_free(&C.by_stage[0]);
	index_free(&C.by_stage[1]);
	index_free(&C.files);
	C.on = 0;
	C.said = 0;
}

/* ---------- the self-test (debug.d3d11_self_test, the fourth line, first part) */

static const char self_test_vertex_hlsl[] =
	"struct vs_in { float4 a : TEXCOORD0; float4 b : TEXCOORD1; float4 unused : TEXCOORD2; };\n"
	"float4 main(vs_in i) : SV_Position { return i.a + i.b; }\n";
static const char self_test_pixel_hlsl[] =
	"float4 main(float4 p : SV_Position) : SV_Target { return p * 0.5; }\n";
/* (the Vulkan plan's case was glslang's "'this' : Reserved word", which HLSL
compiles: a name not declared here) */
static const char self_test_bad_hlsl[] =
	"float4 main() : SV_Target { return not_declared; }\n";

/* the test's hashes: not FNV values of anything the game makes */
#define SELF_TEST_VERTEX 0x5e1f7e5700000001ULL
#define SELF_TEST_PIXEL 0x5e1f7e5700000002ULL
#define SELF_TEST_BAD 0x5e1f7e5700000003ULL

static int file_exists(const char *path)
{
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* the shader's file with one byte of its bytecode changed */
static int damage_file(uint32_t stage, uint64_t hash)
{
	char path[MAX_PATH];
	HANDLE file;
	unsigned char byte = 0;
	DWORD done = 0;
	LONG at = CACHE_HEADER_SIZE + 4;
	int ok;

	shader_path(path, sizeof(path), C.folder, stage, hash, ".dxbc");
	file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	ok = SetFilePointer(file, at, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER && ReadFile(file, &byte, 1, &done, NULL) &&
		done == 1;
	byte ^= 0x5a;
	ok = ok && SetFilePointer(file, at, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
		WriteFile(file, &byte, 1, &done, NULL) && done == 1;
	CloseHandle(file);
	return ok;
}

int d3d11_backend_shaders_self_test(char *line, size_t line_size)
{
	static const struct d3d11_backend_layout_element elements[2] = {
		{ 0, D3D11_BACKEND_FORMAT_R32G32B32A32_FLOAT, 0, 0 },
		{ 1, D3D11_BACKEND_FORMAT_B8G8R8A8_UNORM, 0, 16 },
	};
	struct d3d11_backend_blend blend;
	struct d3d11_backend_depth_stencil depth;
	struct d3d11_backend_rasterizer rasterizer;
	struct d3d11_backend_sampler sampler;
	uint32_t before[D3D11_BACKEND_STATISTICS], after[D3D11_BACKEND_STATISTICS];
	uint32_t vertex, pixel, again, bad, layout, handles[4];
	char path[MAX_PATH];
	const char *failed = NULL;
	int files = C.on;

	if (!B.device || B.lost)
	{
		snprintf(line, line_size, "d3d11: shader service self-test: not run (no device)");
		return 0;
	}
	d3d11_backend_shaders_statistics(before);
	/* (its files from an earlier run gone, so that it compiles) */
	if (files)
	{
		shader_path(path, sizeof(path), C.folder, D3D11_BACKEND_STAGE_VERTEX, SELF_TEST_VERTEX, ".dxbc");
		DeleteFileA(path);
		index_add(&C.files, file_key(D3D11_BACKEND_STAGE_VERTEX, SELF_TEST_VERTEX), FILE_GONE);
		shader_path(path, sizeof(path), C.folder, D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_PIXEL, ".dxbc");
		DeleteFileA(path);
		index_add(&C.files, file_key(D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_PIXEL), FILE_GONE);
		shader_path(path, sizeof(path), C.failed_folder, D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_BAD, ".hlsl");
		DeleteFileA(path);
	}

	/* a good pair: made, its files written; found again from memory */
	vertex = d3d11_backend_shader_make(D3D11_BACKEND_STAGE_VERTEX, SELF_TEST_VERTEX, self_test_vertex_hlsl,
			sizeof(self_test_vertex_hlsl) - 1, "main", "vs_4_0");
	pixel = d3d11_backend_shader_make(D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_PIXEL, self_test_pixel_hlsl,
		sizeof(self_test_pixel_hlsl) - 1, "main", "ps_4_0");
	d3d11_backend_shaders_statistics(after);
	if (!vertex || !pixel || after[D3D11_BACKEND_STATISTIC_COMPILED] != before[D3D11_BACKEND_STATISTIC_COMPILED] + 2)
		failed = "the good pair was not compiled and made";
	if (!failed && files)
	{
		shader_path(path, sizeof(path), C.folder, D3D11_BACKEND_STAGE_VERTEX, SELF_TEST_VERTEX, ".dxbc");
		if (!file_exists(path))
			failed = "the good vertex shader's file was not written";
		shader_path(path, sizeof(path), C.folder, D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_PIXEL, ".dxbc");
		if (!failed && !file_exists(path))
			failed = "the good pixel shader's file was not written";
	}
	if (!failed && (d3d11_backend_shader_find(D3D11_BACKEND_STAGE_VERTEX, SELF_TEST_VERTEX) != vertex ||
		d3d11_backend_shader_find(D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_PIXEL) != pixel))
	{
		failed = "the good pair was not found again from memory";
	}
	/* let go, and found again from the files */
	if (!failed && files)
	{
		shader_forget(vertex);
		shader_forget(pixel);
		d3d11_backend_shaders_statistics(before);
		if (d3d11_backend_shader_find(D3D11_BACKEND_STAGE_VERTEX, SELF_TEST_VERTEX) != vertex ||
			d3d11_backend_shader_find(D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_PIXEL) != pixel)
		{
			failed = "the good pair was not found again from its files";
		}
		d3d11_backend_shaders_statistics(after);
		if (!failed && (after[D3D11_BACKEND_STATISTIC_CACHED] != before[D3D11_BACKEND_STATISTIC_CACHED] + 2 ||
			after[D3D11_BACKEND_STATISTIC_COMPILED] != before[D3D11_BACKEND_STATISTIC_COMPILED]))
		{
			failed = "the good pair was compiled again, not read from its files";
		}
	}
	/* a bad shader: failed, logged once, its HLSL in failed\; asked again,
	failed without a compile */
	if (!failed)
	{
		bad = d3d11_backend_shader_make(D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_BAD, self_test_bad_hlsl,
			sizeof(self_test_bad_hlsl) - 1, "main", "ps_4_0");
		d3d11_backend_shaders_statistics(before);
		again = d3d11_backend_shader_make(D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_BAD, self_test_bad_hlsl,
			sizeof(self_test_bad_hlsl) - 1, "main", "ps_4_0");
		d3d11_backend_shaders_statistics(after);
		shader_path(path, sizeof(path), C.failed_folder, D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_BAD, ".hlsl");
		if (bad || again || after[D3D11_BACKEND_STATISTIC_FAILED] != before[D3D11_BACKEND_STATISTIC_FAILED] ||
			d3d11_backend_shader_find(D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_BAD))
		{
			failed = "the bad shader was not failed once";
		}
		else if (files && !file_exists(path))
		{
			failed = "the bad shader's HLSL was not written to failed\\";
		}
	}
	/* a damaged file: deleted when asked for, and its shader made again */
	if (!failed && files)
	{
		shader_forget(pixel);
		shader_path(path, sizeof(path), C.folder, D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_PIXEL, ".dxbc");
		if (!damage_file(D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_PIXEL))
			failed = "the test could not damage its file";
		else if (d3d11_backend_shader_find(D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_PIXEL) || file_exists(path))
			failed = "the damaged file was not deleted";
		else if (d3d11_backend_shader_make(D3D11_BACKEND_STAGE_PIXEL, SELF_TEST_PIXEL, self_test_pixel_hlsl,
			sizeof(self_test_pixel_hlsl) - 1, "main", "ps_4_0") != pixel || !file_exists(path))
		{
			failed = "the damaged file's shader was not made again";
		}
	}
	/* a layout with an input left out (TEXCOORD2, from the attribute slot),
	found again */
	if (!failed)
	{
		layout = d3d11_backend_layout_make(elements, 2, vertex);
		if (!layout || d3d11_backend_layout_make(elements, 2, vertex) != layout)
			failed = "the layout was not made, or not found again";
	}
	/* one state object of each kind, found again by its description */
	if (!failed)
	{
		memset(&blend, 0, sizeof(blend));
		blend.enable = 1;
		blend.source = D3D11_BACKEND_BLEND_BLEND_FACTOR;
		blend.destination = D3D11_BACKEND_BLEND_INV_SRC_COLOR;
		blend.operation = D3D11_BACKEND_BLEND_OP_REV_SUBTRACT;
		blend.source_alpha = D3D11_BACKEND_BLEND_BLEND_FACTOR;
		blend.destination_alpha = D3D11_BACKEND_BLEND_INV_SRC_ALPHA;
		blend.operation_alpha = D3D11_BACKEND_BLEND_OP_REV_SUBTRACT;
		blend.write_mask = D3D11_BACKEND_CLEAR_R | D3D11_BACKEND_CLEAR_A;
		memset(&depth, 0, sizeof(depth));
		depth.depth_enable = 1;
		depth.depth_write = 1;
		depth.depth_function = D3D11_BACKEND_COMPARISON_NEVER + 3;
		depth.stencil_enable = 1;
		depth.stencil_read_mask = 0x0f;
		depth.stencil_write_mask = 0xf0;
		depth.stencil_fail = D3D11_BACKEND_STENCIL_OP_INVERT;
		depth.stencil_depth_fail = D3D11_BACKEND_STENCIL_OP_INCR;
		depth.stencil_pass = D3D11_BACKEND_STENCIL_OP_DECR_SAT;
		depth.stencil_function = D3D11_BACKEND_COMPARISON_ALWAYS;
		memset(&rasterizer, 0, sizeof(rasterizer));
		rasterizer.fill = D3D11_BACKEND_FILL_WIREFRAME;
		rasterizer.cull = D3D11_BACKEND_CULL_FRONT;
		rasterizer.front_counter_clockwise = 1;
		rasterizer.depth_bias = -8;
		rasterizer.slope_scaled_depth_bias = -2.0f;
		rasterizer.depth_clip = 1;
		rasterizer.scissor = 1;
		memset(&sampler, 0, sizeof(sampler));
		sampler.filter = D3D11_BACKEND_FILTER_ANISOTROPIC;
		sampler.address_u = D3D11_BACKEND_ADDRESS_MIRROR;
		sampler.address_v = D3D11_BACKEND_ADDRESS_BORDER;
		sampler.address_w = D3D11_BACKEND_ADDRESS_CLAMP;
		sampler.mip_lod_bias = -0.5f;
		sampler.max_anisotropy = 4;
		sampler.comparison = D3D11_BACKEND_COMPARISON_NEVER;
		sampler.border_color[0] = 1.0f;
		sampler.border_color[3] = 0.5f;
		sampler.min_lod = 1.0f;
		sampler.max_lod = D3D11_FLOAT32_MAX;
		handles[0] = d3d11_backend_blend_make(&blend);
		handles[1] = d3d11_backend_depth_stencil_make(&depth);
		handles[2] = d3d11_backend_rasterizer_make(&rasterizer);
		handles[3] = d3d11_backend_sampler_make(&sampler);
		d3d11_backend_shaders_statistics(before);
		if (!handles[0] || !handles[1] || !handles[2] || !handles[3])
			failed = "a state object was not made";
		else if (d3d11_backend_blend_make(&blend) != handles[0] || d3d11_backend_depth_stencil_make(&depth) != handles[1] ||
			d3d11_backend_rasterizer_make(&rasterizer) != handles[2] || d3d11_backend_sampler_make(&sampler) != handles[3])
		{
			failed = "a state object was not found again by its description";
		}
		d3d11_backend_shaders_statistics(after);
		if (!failed && memcmp(before, after, sizeof(before)))
			failed = "a state object was made twice";
	}
	if (failed)
	{
		snprintf(line, line_size, "d3d11: shader service self-test: FAILED (%s)", failed);
		return 0;
	}
	if (files)
		snprintf(line, line_size, "d3d11: shader service self-test: ok (a good pair compiled, its files written, found "
			"again from memory and from its files; a bad shader failed once and written to failed\\; a damaged file "
			"deleted and its shader made again; a layout with an input from the attribute slot; one state object of "
			"each kind found again by its description)");
	else
		snprintf(line, line_size, "d3d11: shader service self-test: ok, without the cache's cases (no cache: a good "
			"pair compiled and found again from memory; a bad shader failed once; a layout with an input from the "
			"attribute slot; one state object of each kind found again by its description)");
	return 1;
}
