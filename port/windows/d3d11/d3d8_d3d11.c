/*
D3D8_D3D11.C

The Xbox Direct3D 8 device for the Windows build's Direct3D 11 renderer
(port/windows/D3D11.md, phase 1). Linked into halo_d3d11.exe in place of
port/linux/src/d3d8_gl.c, which halo.exe and every other build keep
unchanged.

It is d3d8_gl.c (commit 2765d816, the last to change that file) with
the desktop side of every HALO_ANDROID conditional and every OpenGL call taken
out: what drew, cleared or uploaded keeps its state, counts what it was asked
for and returns. What remains is what d3d8_gl.c does that is not OpenGL: the
screen's width and scale, the state the XDK's inline functions keep, the
vertical blank and its thread and callbacks, the reserved viewport constants,
render and texture stage state, transforms, vertex shaders and their
declarations, streams, immediate mode, the surfaces and render targets as the
game sees them, the menus' pointer and the statistics. Its shape follows the
Vulkan renderer's first device (port/android/guest/d3d8_vk.c at b5038d02).
Changes to d3d8_gl.c are not followed automatically: later phases copy
decisions from d3d8_gl.c as it is then, and say so.

Phase 2 brought back, with the backend's calls in place of the GL, d3d8_gl.c's
render target table (render_target_get, bind_targets), Clear's arithmetic,
the screenshot writer and the letterboxed present (the backend draws it),
and the menus' pointer as d3d8_gl.c has it: the game's clears are on the
screen.

Phase 3 brought back d3d8_gl.c's memory paths, with the backend's calls in
place of the GL (port/windows/d3d11/win32_d3d11_memory.c): the stream and
index rings' bookkeeping (stream_reserve, stream_upload, index_upload), the
mirror of the contiguous window with its pages, generations and volatile
rule (mirror_refresh, mirror_range) and the index extent cache; and, of the
draws, their data work: setup_streams' upload half as stream_data, the
indexed draw's mirror lookup and extent, quad lists' indices, immediate
mode's vertices. Each draw's data is copied into Direct3D buffers at the
draw, exactly as d3d8_gl.c copies it into GL buffers, and then the draw stops
where GL drew: its draws are still counted and dropped. One thing of
d3d8_gl.c's design is changed: the rings are not orphaned at every frame
(Present), since Map with D3D11_MAP_WRITE_NO_OVERWRITE promises what GL's
orphaning gave; they are orphaned when full. The answers the game reads
back are the OpenGL build's where they do not depend on the GPU; the one that
does, a visibility test, reports 0 samples. The Direct3D 11 device is the backend's
(port/windows/d3d11/win32_d3d11.c, through d3d11_backend.h), made with the
window, which the platform layer makes without an OpenGL context
(platform_video_without_gl). The GL calls the shared files still make go to
counted stubs (build/windows/gl_stubs.c, tools/windows_gl_stubs.py).

Phase 4 brought back prepare_draw's pixel shader key (pixel_key_make, with
bind_textures' decisions and no GL) and vertex_shader_get's choice of a
vertex shader (stream or immediate, lit or not), made at every draw past its
guards (draw_note), kept in tables as fragment_shader_get's, and each first
met dumped with debug.gpu_dump_shaders: its HLSL from the generators' copies
(nv2a_vsh_hlsl.c, nv2a_psh_hlsl.c), its GLSL twin from the originals, the
key's bytes and a manifest, for tools/d3d11_shader_check.py; and the
generators' self-test (debug.d3d11_self_test's third line).

Phase 5: at every draw draw_note also asks the backend's shader service
(win32_d3d11_shaders.c) for the two shaders' handles, compiling a shader the
first time it is met and not in the cache on the disk, as d3d8_gl.c compiles
its GLSL at the draw (a lit pair that fails sets lighting_failed as
prepare_draw does; a shader that fails skips its draws); for the input layout
from the declaration's elements (attribute_format's table as DXGI formats);
and for the blend, depth-stencil and rasterizer states and each sampled
stage's sampler, translated from the Xbox's enumerants as apply_raster_state
and configure_sampler read them. The handles are kept in device.draw for phase
6 to bind; nothing is bound or drawn yet. debug.d3d11_self_test's fourth line
is the service's.

Phase 6, step 2: the textures through the texture cache's copy
(xbox_textures_d3d11.c, xgpu_d3d11_texture_get: the handle, the kind and the
description, with the high-res flags the cache answers), bound through the
record; the cache's frame at Present; the texture statistics.

Phase 6: the draw. prepare_draw (d3d8_gl.c) is back whole in draw_note's
place as draw_prepare, in its order, with the backend's calls where the GL
was: the key and the textures, the targets after them (bind_targets), the
state objects, the shaders, the constant buffers (one DYNAMIC buffer for
every program's vertex constants, written whole when the serial moved; the
model lights; the other uniforms as hlsl_shaders.h's two structures, each
written when its inputs changed), then the record (d3d11_backend.h, struct
d3d11_backend_draw: the handles, the dynamic values, the viewport and its
depth range, the streams stream_data placed, the attribute slot's sixteen
values with zeros for a packed register, the primitives) and the backend's
draw where glDrawArrays and glDrawElementsBaseVertex were. Direct3D 11 has
no fan, no loop and no quads: quad lists, fans, polygons and line loops are
drawn from indices made for the draw (quad_indices, fan_indices,
loop_indices). The transparent meter's blend (the source factor the
constant's alpha, the destination's the constant's colour, which Direct3D
11's one blend factor cannot give) is drawn with the source factors ONE and
the pixel shader multiplying by the constant's alpha (by_constant_alpha, in
the pixel entry's identity). trace_draw is back as it is.
*/

#include "xgpu.h"
#include "halo_port_window.h"
#include "sdl_platform.h"
#include "halo_ui_pointer.h"
#include "port_config.h"
#include "d3d11_backend.h"
#include "hlsl_shaders.h"
#include "xbox_textures_d3d11.h"

/* the GL calls counted by the stubs since the start (build/windows/gl_stubs.c) */
unsigned long gl_stubs_calls(void);

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* whether two copies of a state are the same, compared a word at a time: the
guest's C library (musl) compares a byte at a time, and every draw compares
hundreds of bytes of state and shader keys against what GL has (as
port/switch/guest/d3d8_dk.c does). What is compared is words throughout. */
static int words_equal(const void *a, const void *b, size_t size)
{
	const uint32_t *x = a, *y = b;
	size_t index;

	for (index = 0; index < size / 4; index++)
	{
		if (x[index] != y[index])
			return 0;
	}
	return size % 4 ? !memcmp((const char *)a + size - size % 4, (const char *)b + size - size % 4, size % 4) : 1;
}

void d3d8_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);
void d3d8_surface_resize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);

/* ---------- the screen's width

The Xbox screen is 640x480. The native ports can draw a wider one: 480
lines, and as many columns as the display's shape gives. On Android that is
display.screen_width (port_config.c; 640 keeps 4:3); on the desktop, the
shape of the window (of the display while the game is fullscreen, or of
display.resolution), and 640 where display.resolution_scaling is "original".
The game's camera derives its horizontal field of view from the viewport, so
the 3D view simply widens. The menus and full-screen overlays are laid out
for 640 columns; while they draw (halo_screen_ui_offset), everything shifts
right to center them.

The desktop also draws at that resolution (platform_screen_mode): render
targets the size of the screen get that many pixels (screen_scale), and
viewports, clears and visibility counts are scaled to match, so the game
still works in its 480 lines; "original" draws 640x480, scaled up at
presentation. The width and the scale change only between frames, after one
is presented (halo_screen_commit). */

#define SCREEN_HEIGHT 480
#define SCREEN_MAXIMUM_WIDTH 1920

/* the width the game draws, 0 until first asked, and how many pixels a
render target the size of the screen has per unit of it */
static long screen_width;
static float screen_scale[2] = { 1.0f, 1.0f };
static long ui_offset;
#define UI_OFFSET ((GLint)ui_offset)

/* ---------- anti-aliasing

display.anti_aliasing, off unless it is set: the Xbox drew without any.
"fxaa" and "smaa" are passes over each window's 3D view before the HUD and
menus are drawn over it (halo_screen_anti_alias, xgpu_post.c); "ssaa2x"
draws the screen's targets at twice the resolution each way
(screen_mode_choose), which the display blit scales down; "msaa2x" to
"msaa8x" draw the back buffer and its depth buffer with that many samples a
pixel (render_target_multisample). The setting is read again between frames
(halo_screen_commit), so that a change applies from the next one. */

enum
{
	_anti_aliasing_off,
	_anti_aliasing_fxaa,
	_anti_aliasing_smaa,
	_anti_aliasing_ssaa,
	_anti_aliasing_msaa,
};

static const struct
{
	const char *name;
	int mode;
	int samples;
} anti_aliasing_values[] =
{
	{ "off", _anti_aliasing_off, 0 },
	{ "fxaa", _anti_aliasing_fxaa, 0 },
	{ "smaa", _anti_aliasing_smaa, 0 },
	{ "ssaa2x", _anti_aliasing_ssaa, 0 },
	{ "msaa2x", _anti_aliasing_msaa, 2 },
	{ "msaa4x", _anti_aliasing_msaa, 4 },
	{ "msaa8x", _anti_aliasing_msaa, 8 },
};

#define NUMBER_OF_ANTI_ALIASING_VALUES ((int)(sizeof(anti_aliasing_values) / sizeof(anti_aliasing_values[0])))

/* the value in effect, -1 until the setting is first read; and the GPU's
largest target, which supersampling is held to: 0 while nothing is drawn
(phase 6 asks the backend), so supersampling draws at the screen's scale */
static int anti_aliasing_value = -1;
static GLint maximum_target_size;

/* display.anti_aliasing's value (none of them: the first, off) */
static void anti_aliasing_read(void)
{
	const char *setting = config_string("display.anti_aliasing");
	int value;

	for (value = NUMBER_OF_ANTI_ALIASING_VALUES - 1;
		value > 0 && strcmp(setting, anti_aliasing_values[value].name);
		value--)
	{
	}
	if (value == anti_aliasing_value)
		return;
	anti_aliasing_value = value;
	if (strcmp(setting, anti_aliasing_values[value].name))
		platform_log("anti-aliasing: \"%s\" is unknown, so off", setting);
	else
		platform_log("anti-aliasing: %s", setting);
}

static int anti_aliasing(void)
{
	if (anti_aliasing_value < 0)
		anti_aliasing_read();
	return anti_aliasing_values[anti_aliasing_value].mode;
}

static void screen_mode_choose(long *width, float scale[2])
{
	long display_width, display_height;

	*width = 640;
	scale[0] = scale[1] = 1.0f;
	if (platform_screen_mode(&display_width, &display_height) && display_width > 0 && display_height > 0)
	{
		long wanted = (SCREEN_HEIGHT * display_width + display_height / 2) / display_height;

		*width = wanted < 640 ? 640 : wanted > SCREEN_MAXIMUM_WIDTH ? SCREEN_MAXIMUM_WIDTH : wanted & ~1L;
		scale[0] = (float)display_width / (float)*width;
		scale[1] = (float)display_height / (float)SCREEN_HEIGHT;
		/* a display narrower or wider than the game can be: the picture
		keeps its shape and the display blit letterboxes it */
		if (*width != wanted && *width != (wanted & ~1L))
			scale[0] = scale[1] = scale[0] < scale[1] ? scale[0] : scale[1];
		/* supersampling: twice the pixels each way, or as many as the GPU's
		largest target has (once it is known), which the display blit
		scales down; not with "original" resolution scaling, which draws
		the Xbox's 640x480 */
		if (anti_aliasing() == _anti_aliasing_ssaa && maximum_target_size > 0)
		{
			float factor = 2.0f;

			if (*width * scale[0] * factor > (float)maximum_target_size)
				factor = (float)maximum_target_size / (*width * scale[0]);
			if (SCREEN_HEIGHT * scale[1] * factor > (float)maximum_target_size)
				factor = (float)maximum_target_size / (SCREEN_HEIGHT * scale[1]);
			if (factor > 1.0f)
			{
				scale[0] *= factor;
				scale[1] *= factor;
			}
		}
	}
}

long halo_screen_width(void)
{
	if (!screen_width)
	{
		screen_mode_choose(&screen_width, screen_scale);
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", screen_width, SCREEN_HEIGHT,
			screen_width * screen_scale[0], SCREEN_HEIGHT * screen_scale[1]);
	}
	return screen_width;
}

/* the display's pixels for each of the 480 lines (text_hires.c) */
float halo_screen_pixel_scale(void)
{
	halo_screen_width();
	return screen_scale[1];
}

/* display.shadow_resolution: the size the shadow maps are drawn at.

Each object's shadow is drawn from above into a 128x128 map, blurred into
another and projected onto the level under it (rasterizer_xbox_shadows.c).
On a large screen, a shadow's 128 texels show as steps along its edge, which
crawl as the object moves. The two maps, the game's only R5G6B5 render
targets (rasterizer_xbox.c), can be drawn larger as the screen's targets are
(render_target_get): the game's viewports, clears and quads, in its 128 units,
scale up with them. The scale is a power of two up to 8 (1024x1024), which
the blur needs to cover the same part of the map as the Xbox's
(rasterizer_shadow_convolve); 1, the default, draws them as the Xbox did. It
changes only between frames (halo_screen_commit). */
#define SHADOW_MAP_SIZE 128
#define SHADOW_MAP_MAXIMUM_SCALE 8

/* 0 until first asked */
static long shadow_scale;
static unsigned long shadow_scale_read_at;

static long shadow_scale_choose(void)
{
	long resolution = config_integer("display.shadow_resolution");
	long scale = 1;

	while (scale < SHADOW_MAP_MAXIMUM_SCALE && SHADOW_MAP_SIZE * scale * 2 <= resolution)
		scale *= 2;
	return scale;
}

/* the shadow maps' pixels for each of their 128 texels each way */
long halo_shadow_map_scale(void)
{
	if (!shadow_scale)
	{
		shadow_scale_read_at = config_changes();
		shadow_scale = shadow_scale_choose();
		platform_log("shadow maps: %ldx%ld", SHADOW_MAP_SIZE * shadow_scale, SHADOW_MAP_SIZE * shadow_scale);
	}
	return shadow_scale;
}

void halo_screen_ui_offset(unsigned char centered)
{
	ui_offset = centered ? (halo_screen_width() - 640) / 2 : 0;
}

/* ---------- state the XDK header's inline functions read and write */

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;
BYTE D3D__StateBlockDirty[1024];

/* ---------- vertex shaders */

#define VERTEX_SHADER_SIGNATURE 0x76736864UL /* 'vshd' */
#define VERTEX_PROGRAM_SLOTS 136

struct vertex_element
{
	unsigned char reg;
	unsigned char stream;
	unsigned char type;
	unsigned char bytes;
	unsigned short offset;
};

struct vertex_variant;

/* the input layouts a declaration's draws met (draw_layout), the last few:
by the vertex shader variant and the streams with data, as d3d8_gl.c's
vertex_array and vertex_array_streams */
#define DECLARATION_LAYOUTS 4

struct declaration_layout
{
	const struct vertex_variant *variant;
	unsigned long streams;
	/* the backend's layout, 0 if it could not be made */
	uint32_t handle;
};

struct vertex_shader_object
{
	unsigned long signature;
	unsigned long id;
	DWORD *instructions;
	unsigned long instruction_count;
	struct vertex_element elements[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long element_count;
	unsigned long packed_mask;
	/* the declaration's attributes of signed 16-bit integers (D3DVSDT_SHORT1
	to 4: DXGI has no format that makes them floats, so the shader converts
	them), and of three components (D3DVSDT_SHORT3, NORMSHORT3, PBYTE3: no
	DXGI format has three 16-bit or 8-bit components, so the layout reads
	four and the shader makes w 1); with packed_mask, in the vertex shader's
	identity (hlsl_shaders.h) */
	unsigned long integer_mask;
	unsigned long three_component_mask;
	/* the program's vertex shaders as the draws met them, [lit][immediate]
	(d3d8_gl.c's shader[] and lit_shader[]: vertex_variant_get) */
	struct vertex_variant *variants[2][2];
	/* one of the game's model lighting programs (halo_vertex_shader_lighting),
	whose draws can be lit for each pixel (display.per_pixel_lighting): where
	its lighting's normal and position are (lighting.lights is 0 for the
	others) */
	struct nv2a_vertex_lighting lighting;
	/* a shader lit for each pixel failed to compile or link: lit as the
	vertex shader lights it from then on */
	BOOL lighting_failed;
	/* as a declaration: its draws' layouts (draw_layout) */
	struct declaration_layout layouts[DECLARATION_LAYOUTS];
	unsigned long layout_count;
	/* a stream of a stride its elements' formats do not align with was
	copied at another (stream_data), said once for the declaration */
	BOOL stride_logged;
};

/* ---------- the device */

/* the stream and index rings (the backend's, D3D11_USAGE_DYNAMIC): the
desktop's sizes in d3d8_gl.c */
#define STREAM_BUFFER_SIZE (32 * 1024 * 1024)
#define INDEX_BUFFER_SIZE (8 * 1024 * 1024)

struct d3d11_device
{
	D3DPRESENT_PARAMETERS presentation;
	D3DSurface back_buffer;
	D3DSurface depth_buffer;
	D3DSurface *render_target;
	D3DSurface *depth_stencil;
	D3DVIEWPORT8 viewport;
	D3DMATRIX transforms[D3DTS_MAX];
	D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
	D3DPalette *palettes[D3DTSS_MAXSTAGES];
	D3DSHADERCONSTANTMODE shader_constant_mode;

	struct vertex_shader_object *vertex_shader;
	struct vertex_shader_object *program_slots[VERTEX_PROGRAM_SLOTS];
	unsigned long program_address;
	float constants[XGPU_VERTEX_CONSTANT_COUNT][4];
	float viewport_scale[4];
	float viewport_offset[4];

	struct
	{
		DWORD data;
		UINT stride;
	} streams[16];
	/* SetIndices' base vertex: added to every index of an indexed draw (the
	dynamic vertex buffers keep each buffer's vertices at an offset into one
	vertex buffer, and their triangles count from 0: contrails, lightning) */
	UINT base_vertex_index;

	/* the current value of each input register (SetVertexData) */
	float attributes[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	BOOL immediate_active;
	D3DPRIMITIVETYPE immediate_type;
	float *immediate_vertices;
	unsigned long immediate_count;
	unsigned long immediate_capacity;

	/* where the next write goes in each ring (the GL buffers' names were
	here: the backend owns the two rings) */
	unsigned long stream_offset;
	unsigned long index_offset;

	BOOL visibility_test_active;

	unsigned long frame;
	unsigned long next_vertex_shader_id;
	/* the platform layer made the window (Direct3D_CreateDevice), and the
	backend a Direct3D 11 device on it (d3d11_backend_create) */
	BOOL window_ready;
	BOOL backend_ready;
	/* the backend made the rings and the upload ring
	(d3d11_backend_memory_initialize): without them the draws' data work is
	skipped */
	BOOL buffers_ready;
	BOOL created;
	/* the GL stubs' count after the self-test's call (backend_initialize):
	the statistics give the calls since */
	unsigned long gl_stub_calls_before;
	/* the backend's targets last bound (bind_targets), 0 for none */
	uint32_t bound_color, bound_depth;
	/* display.vsync, the present's sync interval, and
	debug.d3d11_present_marker, as of config_changes()'s count read_at */
	BOOL vsync;
	unsigned long settings_read_at;
	/* the swap chain's resizes, the rings' discards and the draw unit's
	counts when the statistics last said */
	uint32_t resizes_before;
	uint32_t discards_before;
	uint32_t draw_statistics_before[D3D11_BACKEND_DRAW_STATISTICS];
	/* the draw's record (draw_prepare fills it, stream_data and the entry
	points place its data, draw_submit hands it to the backend): the
	shaders, the input layout, the state objects and a sampler for each stage
	with a texture (0 for one without), the dynamic values, the streams, the
	attribute slot's values, the indices, the topology, the textures */
	struct d3d11_backend_draw draw;
	/* the shader service's statistics after the self-tests
	(backend_initialize): the statistics line gives the game's, since */
	uint32_t shader_statistics_before[D3D11_BACKEND_STATISTICS];
};

static struct d3d11_device device;

/* debug.gpu_stats prints these every 60 frames: what the game asked to be
drawn, which is dropped, and the draws skipped; the clears, and how the
backend made them; the bytes mirrored, streamed and uploaded; the presents;
and the debug layer's messages */
static struct
{
	unsigned long draws, immediate_draws, clears, presents;
	unsigned long skipped_no_program, skipped_no_buffer, skipped_no_target, skipped_not_drawn;
	unsigned long clears_by_calls, clears_drawn;
	unsigned long target_changes;
	/* vertex and index bytes drawn from the mirror, and streamed (as
	d3d8_gl.c counts them); and the bytes written into the mirror's segments */
	unsigned long mirrored_bytes, streamed_bytes, uploaded_bytes;
	unsigned long presented, not_presented;
	unsigned long debug_messages;
} stats;

static D3DDevice *device_pointer(void)
{
	return (D3DDevice *)&device;
}

static void color_to_vec4(D3DCOLOR color, float *out)
{
	out[0] = ((color >> 16) & 0xff) / 255.0f;
	out[1] = ((color >> 8) & 0xff) / 255.0f;
	out[2] = (color & 0xff) / 255.0f;
	out[3] = ((color >> 24) & 0xff) / 255.0f;
}

/* ---------- debugging settings, read once (backend_initialize) */

static struct
{
	/* debug.gpu_skip_vertex_shaders "<id>,<id>..." drops draws by vertex
	shader, for finding which pass produces something (port_config.c) */
	const char *skip_vertex_shaders;
	const char *dump_shaders;
	BOOL statistics;
} debug_settings;

/* xbox_textures.c, hud_hires.c and text_hires.c call this after their own GL
calls, which go to the counted stubs here: there is no GL state to forget */
void xgpu_gl_state_invalidate(void)
{
}

/* ---------- vertical blank emulation */

#define VERTICAL_BLANK_NANOSECONDS (1000000000L / 60)

static pthread_mutex_t vertical_blank_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vertical_blank_condition = PTHREAD_COND_INITIALIZER;
static D3DCALLBACK vertical_blank_callback;
static unsigned long vertical_blank_count;
static volatile unsigned int flip_count;
static unsigned long pending_flips;
static BOOL vertical_blank_thread_started = FALSE;

static void *vertical_blank_thread(void *unused)
{
	struct timespec next;

	(void)unused;
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (;;)
	{
		D3DCALLBACK callback;

		next.tv_nsec += VERTICAL_BLANK_NANOSECONDS;
		if (next.tv_nsec >= 1000000000L)
		{
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

		pthread_mutex_lock(&vertical_blank_lock);
		vertical_blank_count++;
		/* a presented frame becomes visible at the next vertical blank */
		if (pending_flips)
		{
			pending_flips--;
			flip_count++;
		}
		callback = vertical_blank_callback;
		pthread_cond_broadcast(&vertical_blank_condition);
		pthread_mutex_unlock(&vertical_blank_lock);

		if (callback)
			callback(0);
	}
	return NULL;
}

static void vertical_blank_start(void)
{
	pthread_mutex_lock(&vertical_blank_lock);
	if (!vertical_blank_thread_started)
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, vertical_blank_thread, NULL) == 0)
		{
			pthread_detach(thread);
			vertical_blank_thread_started = TRUE;
		}
		else
		{
			platform_log("cannot start the vertical blank thread");
		}
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

/* replaces main/d3d_intimacy.cpp, which reads the counter out of the Xbox
Direct3D runtime's private device structure */
volatile unsigned int *d3d_find_flipcount(void)
{
	return &flip_count;
}

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	pthread_mutex_lock(&vertical_blank_lock);
	vertical_blank_callback = callback;
	pthread_mutex_unlock(&vertical_blank_lock);
	vertical_blank_start();
}

void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
	unsigned long count;

	vertical_blank_start();
	pthread_mutex_lock(&vertical_blank_lock);
	count = vertical_blank_count;
	while (vertical_blank_count == count)
		pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
	pthread_mutex_unlock(&vertical_blank_lock);
}

/* ---------- render targets

The game's surfaces it draws into, as d3d8_gl.c finds them (its render
target table, copied), each with the backend's target in place of the GL
texture. */

struct render_target_entry
{
	struct render_target_entry *next;
	/* the next with the same address bucket (render_target_bucket) */
	struct render_target_entry *next_in_bucket;
	struct xgpu_render_target target;
	unsigned long last_rendered;
	/* the back buffer or its depth buffer, which the 3D view is drawn into:
	multisampled with multisampling (render_target_multisample) */
	BOOL screen_buffer;
	/* the backend's target (d3d11_backend_target_create), 0 while it has
	none: then every use of the entry is skipped */
	uint32_t backend_target;
};

/* every draw looks up its targets and whether its textures are render
targets, of which there are dozens */
#define RENDER_TARGET_BUCKET_COUNT 256

static struct render_target_entry *render_target_buckets[RENDER_TARGET_BUCKET_COUNT];

static struct render_target_entry **render_target_bucket(unsigned long data)
{
	return &render_target_buckets[((data >> 12) ^ (data >> 20)) % RENDER_TARGET_BUCKET_COUNT];
}

static struct render_target_entry *render_targets;

static void backend_lost_check(void);

static void surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height, BOOL *depth)
{
	struct xgpu_texture_description description;
	DWORD format;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	*width = description.width;
	*height = description.height;
	format = description.format;
	*depth = format == D3DFMT_D24S8 || format == D3DFMT_F24S8 || format == D3DFMT_D16 || format == D3DFMT_F16 ||
		format == D3DFMT_LIN_D24S8 || format == D3DFMT_LIN_F24S8 || format == D3DFMT_LIN_D16 || format == D3DFMT_LIN_F16;
}

/* the shadow maps: the game's only R5G6B5 render targets, 128x128
(rasterizer_xbox.c) */
static BOOL surface_is_shadow_map(const D3DSurface *surface)
{
	struct xgpu_texture_description description;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	return description.format == D3DFMT_R5G6B5 && description.width == SHADOW_MAP_SIZE &&
		description.height == SHADOW_MAP_SIZE;
}

/* the targets render_target_get found last, by what it found them from:
each draw asks again for the same two (bind_targets) */
#define RECENT_RENDER_TARGET_COUNT 4

static struct
{
	DWORD data, format, size;
	long screen_width;
	float scale[2];
	struct render_target_entry *entry;
} recent_render_targets[RECENT_RENDER_TARGET_COUNT];
static unsigned long recent_render_target_next;

static struct render_target_entry *render_target_remember(const D3DSurface *surface, long screen,
	struct render_target_entry *entry)
{
	unsigned long slot = recent_render_target_next++ % RECENT_RENDER_TARGET_COUNT;

	recent_render_targets[slot].data = surface->Data;
	recent_render_targets[slot].format = surface->Format;
	recent_render_targets[slot].size = surface->Size;
	recent_render_targets[slot].screen_width = screen;
	recent_render_targets[slot].scale[0] = screen_scale[0];
	recent_render_targets[slot].scale[1] = screen_scale[1];
	recent_render_targets[slot].entry = entry;
	return entry;
}

static struct render_target_entry *render_target_get(const D3DSurface *surface)
{
	struct render_target_entry *entry;
	unsigned long width, height, slot;
	long screen;
	BOOL depth;

	if (!surface || !surface->Data)
		return NULL;
	float scale[2] = { 1.0f, 1.0f };

	/* (the entries are never freed) */
	screen = halo_screen_width();
	for (slot = 0; slot < RECENT_RENDER_TARGET_COUNT; slot++)
	{
		if (recent_render_targets[slot].entry && recent_render_targets[slot].data == surface->Data &&
			recent_render_targets[slot].format == surface->Format && recent_render_targets[slot].size == surface->Size &&
			recent_render_targets[slot].screen_width == screen && recent_render_targets[slot].scale[0] == screen_scale[0] &&
			recent_render_targets[slot].scale[1] == screen_scale[1])
		{
			return recent_render_targets[slot].entry;
		}
	}
	surface_dimensions(surface, &width, &height, &depth);
	/* the screen's targets are drawn at the screen's scale, and the shadow
	maps at display.shadow_resolution's (halo_shadow_map_scale) */
	if (width == (unsigned long)halo_screen_width() && height == SCREEN_HEIGHT)
	{
		scale[0] = screen_scale[0];
		scale[1] = screen_scale[1];
	}
	else if (!depth && surface_is_shadow_map(surface))
	{
		scale[0] = (float)halo_shadow_map_scale();
		scale[1] = scale[0];
	}
	for (entry = *render_target_bucket(surface->Data); entry; entry = entry->next_in_bucket)
	{
		if (entry->target.data == surface->Data && entry->target.width == width &&
			entry->target.height == height && entry->target.depth == depth &&
			entry->target.scale[0] == scale[0] && entry->target.scale[1] == scale[1])
		{
			return render_target_remember(surface, screen, entry);
		}
	}
	entry = calloc(1, sizeof(*entry));
	entry->target.data = surface->Data;
	entry->target.width = width;
	entry->target.height = height;
	entry->target.depth = depth;
	entry->target.scale[0] = scale[0];
	entry->target.scale[1] = scale[1];
	entry->target.gl_width = (unsigned long)(width * scale[0] + 0.5f);
	entry->target.gl_height = (unsigned long)(height * scale[1] + 0.5f);
	/* (the GL texture's place: the backend's target, made while the backend
	is ready and logged with its size; one that cannot be made leaves the
	entry without, logged, and the device lost if that is why) */
	if (device.backend_ready)
	{
		entry->backend_target = d3d11_backend_target_create((uint32_t)entry->target.gl_width,
			(uint32_t)entry->target.gl_height, depth);
		if (entry->backend_target)
			platform_log("Direct3D 11: target %lu: %s %lux%lu for the surface at %08lx (%lux%lu)",
				(unsigned long)entry->backend_target, depth ? "depth" : "colour", entry->target.gl_width,
				entry->target.gl_height, (unsigned long)entry->target.data, width, height);
		else
		{
			platform_log("Direct3D 11: no %s target of %lux%lu for the surface at %08lx: what is drawn into it is "
				"skipped", depth ? "depth" : "colour", entry->target.gl_width, entry->target.gl_height,
				(unsigned long)entry->target.data);
			backend_lost_check();
		}
	}
	entry->screen_buffer = surface->Data == (depth ? device.depth_buffer.Data : device.back_buffer.Data);
	entry->next = render_targets;
	render_targets = entry;
	entry->next_in_bucket = *render_target_bucket(entry->target.data);
	*render_target_bucket(entry->target.data) = entry;
	return entry;
}

/* counts draws and clears into render targets (xgpu_render_target.written) */
static unsigned long render_target_write_serial;

/* the pixels per unit of the bound targets (render_target_get) */
static float target_scale[2] = { 1.0f, 1.0f };

/* the pixel edge of a coordinate in a target's units, at its scale:
floorf's, without its call (on 32-bit x86 it saves and restores the FPU's
rounding, several times a draw) */
static GLint scaled_pixel(float coordinate, float scale)
{
	float value = coordinate * scale + 0.5f;
	GLint pixel = (GLint)value;

	/* (the conversion is toward zero: a negative value with a fraction
	rounds down one more) */
	if ((float)pixel > value)
		pixel--;
	return pixel;
}

/* ... in the bound targets' units */
static GLint target_pixel(float coordinate, int axis)
{
	return scaled_pixel(coordinate, target_scale[axis]);
}

/* binds the current targets (d3d11_backend_targets_set); returns FALSE if
there is nothing to draw into */
static BOOL bind_targets(BOOL *has_depth)
{
	struct render_target_entry *color = render_target_get(device.render_target);
	struct render_target_entry *depth = render_target_get(device.depth_stencil);

	if (depth && !depth->target.depth)
		depth = NULL;
	/* (an entry the backend has no target for is not drawn into) */
	if (color && !color->backend_target)
		color = NULL;
	if (depth && !depth->backend_target)
		depth = NULL;
	if (!color && !depth)
		return FALSE;
	if (color)
	{
		color->last_rendered = device.frame + 1;
		color->target.written = ++render_target_write_serial;
	}
	/* viewports and clears are in the targets' units (render_target_get) */
	target_scale[0] = color ? color->target.scale[0] : depth->target.scale[0];
	target_scale[1] = color ? color->target.scale[1] : depth->target.scale[1];
	/* a pair other than the last one bound is a target change (the game
	names its targets again after each present) */
	if ((color ? color->backend_target : 0) != device.bound_color ||
		(depth ? depth->backend_target : 0) != device.bound_depth)
	{
		device.bound_color = color ? color->backend_target : 0;
		device.bound_depth = depth ? depth->backend_target : 0;
		stats.target_changes++;
	}
	d3d11_backend_targets_set(device.bound_color, device.bound_depth);
	*has_depth = depth != NULL;
	return TRUE;
}

/* ---------- device creation */

static void shader_self_test(char *line, size_t line_size);
static void shader_service_self_test(char *line, size_t line_size);

/* what gl_initialize does that is not OpenGL, with the Direct3D 11 device in
its place (port/windows/d3d11/win32_d3d11.c): the backend's one line goes to
the log as it is, whatever it says, and without a device the game runs with
the window and no picture, as with debug.null_renderer */
static void backend_initialize(void)
{
	char line[512];
	unsigned long before;
	int index;

	device.window_ready = TRUE;
	device.backend_ready = d3d11_backend_create(config_string("display.d3d11_adapter"),
		config_boolean("debug.d3d11_debug"), line, sizeof(line)) != 0;
	platform_log("%s", line);
	/* the swap chain on the window; without it the game runs with the window
	black, paced as without a device (Present) */
	if (device.backend_ready)
	{
		d3d11_backend_window_set(platform_video_native_window(), line, sizeof(line));
		platform_log("%s", line);
	}
	/* the stream and index rings and the mirror's upload ring; without them
	the game runs on, its draws' data work skipped (counted as no buffer) and
	the clears still shown */
	if (device.backend_ready)
	{
		device.buffers_ready = d3d11_backend_memory_initialize(STREAM_BUFFER_SIZE, INDEX_BUFFER_SIZE, line,
			sizeof(line)) != 0;
		platform_log("%s", line);
	}
	/* the shader cache on the disk, unless a debug.gpu_debug_* setting
	changes the generators' text (nv2a_psh_hlsl.c reads them as it writes) */
	if (device.backend_ready)
		d3d11_backend_shader_cache_enable(!*config_string("debug.gpu_debug_expression") &&
			!config_boolean("debug.gpu_debug_texture0") && !config_boolean("debug.gpu_debug_flat"));
	/* debug.d3d11_self_test: the clears tried on the backend's own targets,
	then the memory paths on its rings and a segment of its own, then the
	HLSL generators on synthetic shaders, then the shader service */
	if (device.backend_ready && config_boolean("debug.d3d11_self_test"))
	{
		d3d11_backend_self_test(line, sizeof(line));
		platform_log("%s", line);
		d3d11_backend_memory_self_test(line, sizeof(line));
		platform_log("%s", line);
		shader_self_test(line, sizeof(line));
		platform_log("%s", line);
		shader_service_self_test(line, sizeof(line));
		platform_log("%s", line);
	}
	/* (what the self-tests made is not the game's: the statistics count from
	here) */
	if (device.backend_ready)
	{
		d3d11_backend_shaders_statistics(device.shader_statistics_before);
	}
	/* the rings start full, so that the first write to each orphans it (a
	DISCARD map): the self-test's writes went to offsets the device does not
	know of, which a no-overwrite write must not touch */
	device.stream_offset = STREAM_BUFFER_SIZE;
	device.index_offset = INDEX_BUFFER_SIZE;
	/* the shared files' GL calls reach counted stubs, not null pointers
	(tools/windows_gl_stubs.py): one called here must be counted (glFlush,
	which no shared file calls, so that the first-call line it leaves is
	this one's) */
	before = gl_stubs_calls();
	halo_glFlush();
	if (gl_stubs_calls() != before + 1)
	{
		platform_log("gl stubs: self-test FAILED (%lu calls counted, %lu expected): the GL calls of the shared "
			"files would not be counted, and nothing is drawn", gl_stubs_calls() - before, 1UL);
		device.backend_ready = FALSE;
	}
	else
	{
		platform_log("gl stubs: self-test ok");
	}
	device.gl_stub_calls_before = gl_stubs_calls();
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
		device.attributes[index][3] = 1.0f;
	/* (the mirror (phase 3) and the texture cache (phase 6) expect it
	running from the start, as d3d8_gl.c starts it here) */
	memory_watch_initialize();
	debug_settings.skip_vertex_shaders = config_string("debug.gpu_skip_vertex_shaders");
	debug_settings.dump_shaders = *config_string("debug.gpu_dump_shaders") ?
		config_string("debug.gpu_dump_shaders") : NULL;
	debug_settings.statistics = config_boolean("debug.gpu_stats");
	if (anti_aliasing_value < 0)
		anti_aliasing_read();
	device.settings_read_at = config_changes() - 1;
}

/* the backend lost its device (d3d11_backend_lost): said once, and the
backend is called no more; the game goes on, paced without a present */
static void backend_lost_check(void)
{
	if (device.backend_ready && d3d11_backend_lost())
	{
		device.backend_ready = FALSE;
		platform_log("Direct3D 11: the device was lost (%s); the game goes on without a picture",
			d3d11_backend_lost_reason());
	}
}

Direct3D *WINAPI Direct3DCreate8(UINT sdk_version)
{
	(void)sdk_version;
	return (Direct3D *)1;
}

void WINAPI Direct3D_SetPushBufferSize(DWORD push_buffer_size, DWORD segment_count)
{
	(void)push_buffer_size;
	(void)segment_count;
}

/* each vertex constant register's serial is the value constants_serial took
when the register last changed; a program's registers are current up to
the serial it recorded when it last uploaded them. The serials are 64-bit:
the count rises with every register a draw changes (a skinned model changes
up to 132), and 32 bits wrapped within minutes at a high frame rate, after
which every program's next draw found none of its registers changed and
drew with what it last uploaded (another object's node matrices: vertices
flung across the screen for a frame). */
static unsigned long long constant_serials[XGPU_VERTEX_CONSTANT_COUNT];
static unsigned long long constants_serial;
/* (d3d8_gl.c's log of the latest changed registers and its checkpoint found
the range one program's uniform array needed among many programs'; here one
constant buffer serves every program and is written whole when the serial
moved, with a DISCARD map, which must write the whole buffer anyway: phase 6
records the difference) */

static void constants_store(unsigned long first, const void *data, unsigned long count)
{
	const float (*values)[4] = data;
	unsigned long index;

	for (index = 0; index < count; index++)
	{
		if (!words_equal(device.constants[first + index], values[index], sizeof(device.constants[0])))
		{
			memcpy(device.constants[first + index], values[index], sizeof(device.constants[0]));
			constant_serials[first + index] = ++constants_serial;
		}
	}
}

static void viewport_update_constants(void)
{
	/* Direct3D's reserved constants c[-38] and c[-37] map clip space to
	the screen; zscale is the depth buffer's range */
	float zscale = 16777215.0f;
	unsigned long width, height;
	BOOL depth;

	if (device.depth_stencil)
	{
		struct xgpu_texture_description description;

		xgpu_texture_describe(device.depth_stencil->Format, device.depth_stencil->Size, &description);
		if (description.format == D3DFMT_D16 || description.format == D3DFMT_LIN_D16 ||
			description.format == D3DFMT_F16 || description.format == D3DFMT_LIN_F16)
		{
			zscale = 65535.0f;
		}
	}
	(void)width; (void)height; (void)depth;
	device.viewport_scale[0] = device.viewport.Width * 0.5f;
	device.viewport_scale[1] = -(float)device.viewport.Height * 0.5f;
	device.viewport_scale[2] = zscale * (device.viewport.MaxZ - device.viewport.MinZ);
	device.viewport_scale[3] = 0.0f;
	device.viewport_offset[0] = device.viewport.X + device.viewport.Width * 0.5f;
	device.viewport_offset[1] = device.viewport.Y + device.viewport.Height * 0.5f;
	device.viewport_offset[2] = zscale * device.viewport.MinZ;
	device.viewport_offset[3] = 0.0f;
	if (!(device.shader_constant_mode & D3DSCM_NORESERVEDCONSTANTS))
	{
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 38, device.viewport_scale, 1);
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 37, device.viewport_offset, 1);
	}
}

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	unsigned long width, height;
	int index;

	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	if (!device.created)
	{
		memset(&device, 0, sizeof(device));
		if (presentation_parameters)
			device.presentation = *presentation_parameters;
		width = device.presentation.BackBufferWidth ? device.presentation.BackBufferWidth : 640;
		height = device.presentation.BackBufferHeight ? device.presentation.BackBufferHeight : 480;
		/* room for the widest screen, which F11 can switch to (the screen's
		width, above) */
		d3d8_surface_initialize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, SCREEN_MAXIMUM_WIDTH, height);
		d3d8_surface_initialize(&device.depth_buffer, D3DFMT_LIN_D24S8, SCREEN_MAXIMUM_WIDTH, height);
		d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, width, height);
		d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, width, height);
		device.render_target = &device.back_buffer;
		device.depth_stencil = &device.depth_buffer;
		for (index = 0; index < D3DTS_MAX; index++)
		{
			device.transforms[index]._11 = 1.0f;
			device.transforms[index]._22 = 1.0f;
			device.transforms[index]._33 = 1.0f;
			device.transforms[index]._44 = 1.0f;
		}
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MaxZ = 1.0f;
		device.next_vertex_shader_id = 1;
		D3D__RenderState[D3DRS_ZENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZWRITEENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
		D3D__RenderState[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
		D3D__RenderState[D3DRS_SRCBLEND] = D3DBLEND_ONE;
		D3D__RenderState[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
		D3D__RenderState[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
		D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
		D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
		D3D__RenderState[D3DRS_FILLMODE] = D3DFILL_SOLID;
		D3D__RenderState[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILWRITEMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
		for (index = 0; index < D3DTSS_MAXSTAGES; index++)
		{
			D3D__TextureState[index][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_MAGFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MINFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MAXANISOTROPY] = 1;
		}
		viewport_update_constants();

		/* (the window without an OpenGL context: the backend presents on it,
		from phase 2; sdl_platform.c) */
		platform_video_without_gl();
		if (!config_boolean("debug.null_renderer") && platform_video_initialize(width, height))
			backend_initialize();
		else
			platform_log("Direct3D: running without a window (nothing is displayed)");
		device.created = TRUE;
	}
	*returned_device = device_pointer();
	return S_OK;
}

/* ---------- the menus' pointer */

/* a point in the window, as SDL reports it, in the menus' coordinates: the
inverse of the letterboxed display blit at presentation, the screen's
width and the menus' centering (halo_screen_ui_offset); or, not centered,
the screen's (the game's drawing: the scoreboard's) */
static void ui_point_from_window_on(float window_x, float window_y, int centered, short *x, short *y)
{
	struct render_target_entry *back_buffer = render_target_get(&device.back_buffer);
	int window_width, window_height, pixel_width, pixel_height, width, height, left, top;
	float screen_x, screen_y;

	*x = *y = -1;
	if (!back_buffer)
		return;
	platform_video_window_size(&window_width, &window_height);
	platform_video_drawable_size(&pixel_width, &pixel_height);
	if (window_width <= 0 || window_height <= 0)
		return;
	width = pixel_width;
	height = (int)((long)pixel_width * back_buffer->target.gl_height / back_buffer->target.gl_width);
	if (height > pixel_height)
	{
		height = pixel_height;
		width = (int)((long)pixel_height * back_buffer->target.gl_width / back_buffer->target.gl_height);
	}
	left = (pixel_width - width) / 2;
	top = (pixel_height - height) / 2;
	screen_x = (window_x * pixel_width / window_width - left) * (float)back_buffer->target.width / (float)width;
	screen_y = (window_y * pixel_height / window_height - top) * (float)back_buffer->target.height / (float)height;
	*x = (short)floorf(screen_x - (centered ? (float)(halo_screen_width() - 640) / 2.0f : 0.0f));
	*y = (short)floorf(screen_y);
}

static void ui_point_from_window(float window_x, float window_y, short *x, short *y)
{
	ui_point_from_window_on(window_x, window_y, TRUE, x, y);
}

int halo_scoreboard_pointer_update(int offered, struct halo_ui_pointer *pointer)
{
	struct platform_ui_pointer state;

	memset(pointer, 0, sizeof(*pointer));
	if (!platform_scoreboard_pointer(offered != 0, &state) || !device.window_ready)
		return 0;
	ui_point_from_window_on(state.x, state.y, FALSE, &pointer->x, &pointer->y);
	ui_point_from_window_on(state.click_x, state.click_y, FALSE, &pointer->click_x, &pointer->click_y);
	pointer->moved = state.moved != FALSE;
	pointer->left_clicks = (unsigned char)(state.left_clicks < 255 ? state.left_clicks : 255);
	return 1;
}

int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	struct platform_ui_pointer state;

	platform_menus_set_active(menus_active != 0);
	platform_ui_pointer_set_active(menus_active != 0);
	if (!menus_active || !device.window_ready || !platform_ui_pointer_read(&state))
		return 0;
	memset(pointer, 0, sizeof(*pointer));
	ui_point_from_window(state.x, state.y, &pointer->x, &pointer->y);
	ui_point_from_window(state.click_x, state.click_y, &pointer->click_x, &pointer->click_y);
	pointer->moved = state.moved != FALSE;
	pointer->left_clicks = (unsigned char)(state.left_clicks < 255 ? state.left_clicks : 255);
	pointer->right_clicks = (unsigned char)(state.right_clicks < 255 ? state.right_clicks : 255);
	pointer->wheel_steps = (signed char)(state.wheel_steps < -8 ? -8 : state.wheel_steps > 8 ? 8 : state.wheel_steps);
	return 1;
}

/* takes up the display's shape and resolution, or the window's, if they
have changed; between frames, since the game's layout and the targets must
agree for a whole frame. Returns the width the game draws. */
long halo_screen_commit(void)
{
	long width;
	float scale[2];

	/* (display.anti_aliasing, as Settings or config.toml has it now:
	supersampling changes the scale below) */
	anti_aliasing_read();
	/* display.shadow_resolution, if it has changed: the maps' entries at
	the old scale stay (render_target_get), but are no longer found */
	if (shadow_scale && shadow_scale_read_at != config_changes())
	{
		long shadow = shadow_scale_choose();

		shadow_scale_read_at = config_changes();
		if (shadow != shadow_scale)
		{
			platform_log("shadow maps: %ldx%ld", SHADOW_MAP_SIZE * shadow, SHADOW_MAP_SIZE * shadow);
			shadow_scale = shadow;
			memset(recent_render_targets, 0, sizeof(recent_render_targets));
		}
	}
	if (!screen_width)
		return halo_screen_width();
	screen_mode_choose(&width, scale);
	if (width != screen_width || scale[0] != screen_scale[0] || scale[1] != screen_scale[1])
	{
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", width, SCREEN_HEIGHT,
			width * scale[0], SCREEN_HEIGHT * scale[1]);
		screen_width = width;
		screen_scale[0] = scale[0];
		screen_scale[1] = scale[1];
		if (device.created)
		{
			device.presentation.BackBufferWidth = (UINT)width;
			d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, (unsigned long)width, SCREEN_HEIGHT);
			d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, (unsigned long)width, SCREEN_HEIGHT);
		}
	}
	return screen_width;
}

ULONG WINAPI D3DDevice_Release(void)
{
	return 1;
}

void WINAPI D3DDevice_GetDeviceCaps(D3DCAPS8 *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = 4096;
	caps->MaxTextureHeight = 4096;
	caps->MaxVolumeExtent = 512;
	caps->MaxTextureRepeat = 8192;
	caps->MaxTextureAspectRatio = 4096;
	caps->MaxAnisotropy = 4;
	caps->MaxTextureBlendStages = 4;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxVertexBlendMatrices = 4;
	caps->MaxPointSize = 64.0f;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxVertexIndex = 0xffff;
	caps->MaxStreams = 16;
	caps->MaxStreamStride = 255;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 192;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	(void)back_buffer;
	(void)type;
	/* like Direct3D, the caller gets a reference it must release */
	device.back_buffer.Common++;
	*result = &device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	*result = device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

static BOOL trace_frame(void);

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	if (trace_frame())
		platform_log("set render target %08lx depth %08lx", render_target ? (unsigned long)render_target->Data : 0,
			depth_stencil ? (unsigned long)depth_stencil->Data : 0);
	if (render_target)
		device.render_target = render_target;
	device.depth_stencil = depth_stencil;
	/* like Direct3D, reset the viewport to the whole new target */
	if (device.render_target)
	{
		unsigned long width, height;
		BOOL depth;

		surface_dimensions(device.render_target, &width, &height, &depth);
		device.viewport.X = 0;
		device.viewport.Y = 0;
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MinZ = 0.0f;
		device.viewport.MaxZ = 1.0f;
	}
	viewport_update_constants();
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	device.viewport = *viewport;
	viewport_update_constants();
}

void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		device.transforms[state] = *matrix;
}

void WINAPI D3DDevice_GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		*matrix = device.transforms[state];
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter) { (void)filter; }
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable) { (void)enable; }

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	device.shader_constant_mode = mode;
	viewport_update_constants();
}

/* ---------- GPU synchronisation: nothing is drawn yet */

BOOL WINAPI D3DDevice_IsBusy(void)
{
	return FALSE;
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	(void)type;
	/* the "GPU" reaches the callback immediately */
	if (callback)
		callback(context);
}

/* ---------- visibility (occlusion) tests: none passes until the backend
counts samples (phase 6), so lens flares stay dark */

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
	if (!device.backend_ready || device.visibility_test_active)
		return;
	device.visibility_test_active = TRUE;
}

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	(void)index;
	if (!device.backend_ready || !device.visibility_test_active)
		return S_OK;
	device.visibility_test_active = FALSE;
	return S_OK;
}

/* the main menu's line under the version number (menu_functions.c) */
char const *d3d8_renderer_description(void)
{
	return "Direct3D 11";
}

/* 0 samples, always complete (never D3DERR_TESTINCOMPLETE, on which the game
would spin) */
HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	(void)index;
	if (time_stamp)
		*time_stamp = 0;
	if (result)
		*result = 0;
	return S_OK;
}

/* ---------- render and texture stage state */

void D3DFASTCALL D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
	/* callers also store the value in D3D__RenderState themselves */
	(void)method;
	(void)value;
}

void D3DFASTCALL D3DDevice_SetRenderState_Deferred(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value);

void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE state, DWORD value)
{
	if (state == D3DRS_ZBIAS)
		D3DDevice_SetRenderState_ZBias(value);
	else if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

/* As the Xbox's D3D8 does it: a z bias is a polygon offset of -bias depth
units plus -bias/4 times the polygon's depth slope, enabled for every fill
mode. Without the slope term, decals (biased by 8) fight with the surface
under them wherever it is seen at an angle. */
void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value)
{
	float offset = -(float)value;
	float slope = offset * 0.25f;
	DWORD enable = value != 0;

	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZSLOPESCALE], &slope, sizeof(slope));
	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZOFFSET], &offset, sizeof(offset));
	D3D__RenderState[D3DRS_POINTOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_WIREFRAMEOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_SOLIDOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_ZBIAS] = value;
}

#define COMPLEX_RENDER_STATE(name, state) \
	void WINAPI D3DDevice_SetRenderState_##name(DWORD value) { D3D__RenderState[state] = value; }

COMPLEX_RENDER_STATE(PSTextureModes, D3DRS_PSTEXTUREMODES)
COMPLEX_RENDER_STATE(VertexBlend, D3DRS_VERTEXBLEND)
COMPLEX_RENDER_STATE(FogColor, D3DRS_FOGCOLOR)
COMPLEX_RENDER_STATE(FillMode, D3DRS_FILLMODE)
COMPLEX_RENDER_STATE(BackFillMode, D3DRS_BACKFILLMODE)
COMPLEX_RENDER_STATE(TwoSidedLighting, D3DRS_TWOSIDEDLIGHTING)
COMPLEX_RENDER_STATE(NormalizeNormals, D3DRS_NORMALIZENORMALS)
COMPLEX_RENDER_STATE(ZEnable, D3DRS_ZENABLE)
COMPLEX_RENDER_STATE(StencilEnable, D3DRS_STENCILENABLE)
COMPLEX_RENDER_STATE(StencilFail, D3DRS_STENCILFAIL)
COMPLEX_RENDER_STATE(FrontFace, D3DRS_FRONTFACE)
COMPLEX_RENDER_STATE(CullMode, D3DRS_CULLMODE)
COMPLEX_RENDER_STATE(TextureFactor, D3DRS_TEXTUREFACTOR)
COMPLEX_RENDER_STATE(LogicOp, D3DRS_LOGICOP)
COMPLEX_RENDER_STATE(EdgeAntiAlias, D3DRS_EDGEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleAntiAlias, D3DRS_MULTISAMPLEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleMask, D3DRS_MULTISAMPLEMASK)
COMPLEX_RENDER_STATE(MultiSampleType, D3DRS_MULTISAMPLETYPE)
COMPLEX_RENDER_STATE(ShadowFunc, D3DRS_SHADOWFUNC)
COMPLEX_RENDER_STATE(LineWidth, D3DRS_LINEWIDTH)
COMPLEX_RENDER_STATE(Dxt1NoiseEnable, D3DRS_DXT1NOISEENABLE)
COMPLEX_RENDER_STATE(YuvEnable, D3DRS_YUVENABLE)
COMPLEX_RENDER_STATE(OcclusionCullEnable, D3DRS_OCCLUSIONCULLENABLE)
COMPLEX_RENDER_STATE(StencilCullEnable, D3DRS_STENCILCULLENABLE)
COMPLEX_RENDER_STATE(RopZCmpAlwaysRead, D3DRS_ROPZCMPALWAYSREAD)
COMPLEX_RENDER_STATE(RopZRead, D3DRS_ROPZREAD)
COMPLEX_RENDER_STATE(DoNotCullUncompressed, D3DRS_DONOTCULLUNCOMPRESSED)

void D3DFASTCALL D3DDevice_SetTextureState_Deferred(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] = value;
}

void WINAPI D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_BORDERCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_ColorKeyColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_COLORKEYCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_BumpEnv(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.textures[stage] = texture;
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.palettes[stage] = palette;
}

void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition)
{
	/* the definition's members are the pixel shader render states */
	if (!definition)
		return;
	memcpy(&D3D__RenderState[D3DRS_PSALPHAINPUTS0], definition->PSAlphaInputs, sizeof(definition->PSAlphaInputs));
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD] = definition->PSFinalCombinerInputsABCD;
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG] = definition->PSFinalCombinerInputsEFG;
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT0_0], definition->PSConstant0, sizeof(definition->PSConstant0));
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT1_0], definition->PSConstant1, sizeof(definition->PSConstant1));
	memcpy(&D3D__RenderState[D3DRS_PSALPHAOUTPUTS0], definition->PSAlphaOutputs, sizeof(definition->PSAlphaOutputs));
	memcpy(&D3D__RenderState[D3DRS_PSRGBINPUTS0], definition->PSRGBInputs, sizeof(definition->PSRGBInputs));
	D3D__RenderState[D3DRS_PSCOMPAREMODE] = definition->PSCompareMode;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0] = definition->PSFinalCombinerConstant0;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1] = definition->PSFinalCombinerConstant1;
	memcpy(&D3D__RenderState[D3DRS_PSRGBOUTPUTS0], definition->PSRGBOutputs, sizeof(definition->PSRGBOutputs));
	D3D__RenderState[D3DRS_PSCOMBINERCOUNT] = definition->PSCombinerCount;
	D3D__RenderState[D3DRS_PSTEXTUREMODES] = definition->PSTextureModes;
	D3D__RenderState[D3DRS_PSDOTMAPPING] = definition->PSDotMapping;
	D3D__RenderState[D3DRS_PSINPUTTEXTURE] = definition->PSInputTexture;
}

/* ---------- vertex shaders */

static unsigned long vertex_type_bytes(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return 4;
	case D3DVSDT_FLOAT2: return 8;
	case D3DVSDT_FLOAT3: return 12;
	case D3DVSDT_FLOAT4: return 16;
	case D3DVSDT_D3DCOLOR: return 4;
	case D3DVSDT_SHORT1: return 2;
	case D3DVSDT_SHORT2: return 4;
	case D3DVSDT_SHORT3: return 6;
	case D3DVSDT_SHORT4: return 8;
	case D3DVSDT_NORMSHORT1: return 2;
	case D3DVSDT_NORMSHORT2: return 4;
	case D3DVSDT_NORMSHORT3: return 6;
	case D3DVSDT_NORMSHORT4: return 8;
	case D3DVSDT_NORMPACKED3: return 4;
	case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_PBYTE2: return 2;
	case D3DVSDT_PBYTE3: return 3;
	case D3DVSDT_PBYTE4: return 4;
	case D3DVSDT_FLOAT2H: return 12;
	default: return 0;
	}
}

/* the vertex shader's masks an element sets (its identity: hlsl_shaders.h):
packed (D3DVSDT_NORMPACKED3), integer (D3DVSDT_SHORT1 to 4), three components
(D3DVSDT_SHORT3, NORMSHORT3, PBYTE3) */
static void element_masks(const struct vertex_element *element, unsigned long *packed_mask, unsigned long *integer_mask,
	unsigned long *three_component_mask)
{
	if (element->type == D3DVSDT_NORMPACKED3)
		*packed_mask |= 1UL << element->reg;
	if (element->type == D3DVSDT_SHORT1 || element->type == D3DVSDT_SHORT2 || element->type == D3DVSDT_SHORT3 ||
		element->type == D3DVSDT_SHORT4)
	{
		*integer_mask |= 1UL << element->reg;
	}
	if (element->type == D3DVSDT_SHORT3 || element->type == D3DVSDT_NORMSHORT3 || element->type == D3DVSDT_PBYTE3)
		*three_component_mask |= 1UL << element->reg;
}

static void parse_declaration(struct vertex_shader_object *object, const DWORD *declaration)
{
	unsigned long stream = 0;
	unsigned long offsets[16] = { 0 };

	for (; declaration && *declaration != D3DVSD_END(); declaration++)
	{
		DWORD token = *declaration;
		unsigned long token_type = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

		switch (token_type)
		{
		case D3DVSD_TOKEN_STREAM:
			stream = token & D3DVSD_STREAMNUMBERMASK;
			break;
		case D3DVSD_TOKEN_STREAMDATA:
			if (token & D3DVSD_DATALOADTYPEMASK)
			{
				/* skip: the count is in dwords, or in bytes with bit 27 */
				unsigned long count = (token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;

				offsets[stream] += (token & 0x08000000) ? count : count * 4;
			}
			else if (object->element_count < XGPU_VERTEX_ATTRIBUTE_COUNT)
			{
				struct vertex_element *element = &object->elements[object->element_count++];

				element->reg = (unsigned char)(token & D3DVSD_VERTEXREGMASK);
				element->stream = (unsigned char)stream;
				element->type = (unsigned char)((token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
				element->bytes = (unsigned char)vertex_type_bytes(element->type);
				element->offset = (unsigned short)offsets[stream];
				offsets[stream] += element->bytes;
				element_masks(element, &object->packed_mask, &object->integer_mask, &object->three_component_mask);
			}
			break;
		case D3DVSD_TOKEN_CONSTMEM:
			declaration += ((token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT) * 4;
			break;
		case D3DVSD_TOKEN_EXT:
			declaration += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
			break;
		default:
			break;
		}
	}
}

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader_object *object = calloc(1, sizeof(*object));

	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = VERTEX_SHADER_SIGNATURE;
	object->id = device.next_vertex_shader_id++;
	if (function)
	{
		/* header: program type in the low word, instruction count in the high */
		object->instruction_count = function[0] >> 16;
		object->instructions = malloc(object->instruction_count * 4 * sizeof(DWORD));
		memcpy(object->instructions, function + 1, object->instruction_count * 4 * sizeof(DWORD));
	}
	parse_declaration(object, declaration);
	/* odd values are FVF codes; programmable shader handles are even */
	*handle = (DWORD)object;
	return S_OK;
}

static struct vertex_shader_object *vertex_shader_from_handle(DWORD handle)
{
	struct vertex_shader_object *object = (struct vertex_shader_object *)handle;

	if (!handle || (handle & 1) || object->signature != VERTEX_SHADER_SIGNATURE)
		return NULL;
	return object;
}

/* the game names its model lighting programs as it creates them
(rasterizer_xbox_vertex_shaders_initialize.c), so that their draws can be lit
for each pixel (display.per_pixel_lighting); one whose lighting is not as
the pixel shader computes it stays lit for each vertex */
void halo_vertex_shader_lighting(unsigned long handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle((DWORD)handle);

	if (!object || !object->instructions)
		return;
	if (!nv2a_vertex_shader_lighting(object->instructions, object->instruction_count, &object->lighting))
	{
		memset(&object->lighting, 0, sizeof(object->lighting));
		platform_log("GPU: vertex shader %lu is not lit as the pixel shader would light it: lit for each vertex",
			object->id);
	}
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	/* programs stay cached; the object is small */
	(void)handle;
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
	{
		device.vertex_shader = object;
		device.program_address = 0;
		device.program_slots[0] = object;
	}
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_slots[address] = vertex_shader_from_handle(handle);
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
		device.vertex_shader = object;
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_address = address;
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	*size = object ? object->instruction_count : 0;
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	long first = reg + XGPU_VERTEX_CONSTANT_BIAS;

	if (first < 0 || first >= XGPU_VERTEX_CONSTANT_COUNT)
		return;
	if (first + (long)constant_count > XGPU_VERTEX_CONSTANT_COUNT)
		constant_count = XGPU_VERTEX_CONSTANT_COUNT - first;
	constants_store((unsigned long)first, constant_data, constant_count);
}

/* the program that runs: the one loaded at the selected address, else the
current shader's own */
static struct vertex_shader_object *current_program(void)
{
	struct vertex_shader_object *program = device.program_slots[device.program_address];

	return program ? program : device.vertex_shader;
}

/* ---------- tracing (debug.gpu_trace_frame) */

static BOOL trace_frame(void)
{
	static long frame = -2;

	if (frame == -2)
		frame = config_integer("debug.gpu_trace_frame");
	return frame >= 0 && device.frame == (unsigned long)frame;
}

/* ---------- the contiguous window in Direct3D buffers

Vertex and index buffers live in the Xbox's contiguous memory, where most
never change once loaded. The mirror keeps a copy of that memory in Direct3D
buffers (one per segment, created when first needed: the backend's DEFAULT
buffers) and uploads a page only when it is first drawn from or after the
game has written it: pages are write-protected once uploaded, as cached
textures are (memory_watch.c). Pages the game rewrites frame after frame
(dynamic vertices) would fault on every write; after a few such rewrites a
page counts as volatile for a while, and draws that use it stream their data
as before.

(d3d8_gl.c's, copied: the GL buffer names are the backend's segment handles,
its uploads the backend's segment writes, which go through an upload ring and
a copy queued in the context in order with the draws. buffer_upload's pieces
were Mesa's GL thread's and do not come.) */

#define MIRROR_SEGMENT_SIZE 0x400000UL
#define MIRROR_SEGMENT_COUNT (PLATFORM_CONTIGUOUS_MAXIMUM / MIRROR_SEGMENT_SIZE)
#define MIRROR_PAGE_SIZE 0x1000UL
#define MIRROR_PAGE_COUNT (PLATFORM_CONTIGUOUS_MAXIMUM / MIRROR_PAGE_SIZE)
/* rewrites no more than this many frames apart ... */
#define MIRROR_REWRITE_FRAMES 2
/* ... this many times in a row make a page volatile ... */
#define MIRROR_VOLATILE_REWRITES 4
/* ... for this many frames */
#define MIRROR_VOLATILE_FRAMES 600

enum
{
	_mirror_page_absent,
	_mirror_page_present,
	_mirror_page_volatile
};

static struct
{
	/* the backend's segment (d3d11_backend_segment_create), 0 while none */
	uint32_t segments[MIRROR_SEGMENT_COUNT];
	unsigned char state[MIRROR_PAGE_COUNT];
	unsigned char rewrites[MIRROR_PAGE_COUNT];
	/* the page's memory_watch generation when it was uploaded */
	unsigned long generation[MIRROR_PAGE_COUNT];
	unsigned long rewritten_frame[MIRROR_PAGE_COUNT];
} mirror;

/* uploads the pages of [first, last) that are absent or stale; FALSE if one
of them turns out to be volatile, or their segment cannot be made */
static BOOL mirror_refresh(unsigned long first, unsigned long last)
{
	unsigned long page, run;
	BOOL volatile_page = FALSE;
	unsigned char stale[256];
	unsigned long count = last - first;

	if (count > sizeof(stale))
	{
		/* a range this long is refreshed in pieces */
		for (page = first; page < last; page += sizeof(stale))
		{
			if (!mirror_refresh(page, page + sizeof(stale) < last ? page + sizeof(stale) : last))
				return FALSE;
		}
		return TRUE;
	}
	for (page = first; page < last; page++)
	{
		BOOL written = mirror.state[page] == _mirror_page_present &&
			memory_watch_generation(PLATFORM_CONTIGUOUS_BASE + page * MIRROR_PAGE_SIZE, MIRROR_PAGE_SIZE) >
			mirror.generation[page];

		stale[page - first] = mirror.state[page] != _mirror_page_present || written;
		if (written)
		{
			if (device.frame - mirror.rewritten_frame[page] <= MIRROR_REWRITE_FRAMES)
				mirror.rewrites[page]++;
			else
				mirror.rewrites[page] = 1;
			mirror.rewritten_frame[page] = device.frame;
			if (mirror.rewrites[page] >= MIRROR_VOLATILE_REWRITES)
			{
				mirror.state[page] = _mirror_page_volatile;
				volatile_page = TRUE;
			}
		}
	}
	if (volatile_page)
		return FALSE;
	for (page = first; page < last; page = run)
	{
		unsigned long segment = page * MIRROR_PAGE_SIZE / MIRROR_SEGMENT_SIZE;
		unsigned long address, size;

		if (!stale[page - first])
		{
			run = page + 1;
			continue;
		}
		/* (the segment is made before its pages are marked present, where
		d3d8_gl.c made its GL buffer after: a segment that cannot be made,
		logged by the backend, leaves them as they were, and the draw streams
		its data instead) */
		if (!mirror.segments[segment])
		{
			mirror.segments[segment] = d3d11_backend_segment_create(MIRROR_SEGMENT_SIZE);
			if (!mirror.segments[segment])
			{
				backend_lost_check();
				return FALSE;
			}
		}
		for (run = page; run < last && stale[run - first]; run++)
		{
			if (mirror.state[run] != _mirror_page_present)
			{
				mirror.rewrites[run] = 0;
				mirror.rewritten_frame[run] = device.frame;
			}
		}
		address = PLATFORM_CONTIGUOUS_BASE + page * MIRROR_PAGE_SIZE;
		size = (run - page) * MIRROR_PAGE_SIZE;
		/* protect first, so a write racing with the upload is noticed */
		memory_watch_protect(address, size);
		for (; page < run; page++)
		{
			mirror.generation[page] = memory_watch_generation(PLATFORM_CONTIGUOUS_BASE + page * MIRROR_PAGE_SIZE,
				MIRROR_PAGE_SIZE);
			mirror.state[page] = _mirror_page_present;
		}
		/* (a write that fails is the lost device's, which nothing reads after) */
		if (!d3d11_backend_segment_write(mirror.segments[segment],
			(uint32_t)(address - PLATFORM_CONTIGUOUS_BASE - segment * MIRROR_SEGMENT_SIZE), (const void *)address,
			(uint32_t)size))
		{
			backend_lost_check();
		}
		stats.uploaded_bytes += size;
	}
	return TRUE;
}

/* makes [address, address + size) current in the mirror, giving the segment
that holds it, the range's offset in that segment and the newest upload
generation of its pages (which changes whenever its contents do); FALSE if
the range is outside the window, spans two segments or is volatile */
static BOOL mirror_range(unsigned long address, unsigned long size, uint32_t *segment_handle, unsigned long *offset,
	unsigned long *generation)
{
	unsigned long start = address - PLATFORM_CONTIGUOUS_BASE;
	unsigned long segment, first, last, page, oldest = ~0UL, newest = 0;
	BOOL present = TRUE;

	if (!size || address < PLATFORM_CONTIGUOUS_BASE || start + size > PLATFORM_CONTIGUOUS_SIZE)
		return FALSE;
	segment = start / MIRROR_SEGMENT_SIZE;
	if ((start + size - 1) / MIRROR_SEGMENT_SIZE != segment)
		return FALSE;
	first = start / MIRROR_PAGE_SIZE;
	last = (start + size - 1) / MIRROR_PAGE_SIZE + 1;
	for (page = first; page < last; page++)
	{
		if (mirror.state[page] == _mirror_page_volatile)
		{
			if (device.frame - mirror.rewritten_frame[page] < MIRROR_VOLATILE_FRAMES)
				return FALSE;
			mirror.state[page] = _mirror_page_absent;
		}
		if (mirror.state[page] != _mirror_page_present)
		{
			present = FALSE;
		}
		else
		{
			if (mirror.generation[page] < oldest)
				oldest = mirror.generation[page];
			if (mirror.generation[page] > newest)
				newest = mirror.generation[page];
		}
	}
	if (!present || memory_watch_generation(address, size) > oldest)
	{
		if (!mirror_refresh(first, last))
			return FALSE;
		for (newest = 0, page = first; page < last; page++)
		{
			if (mirror.generation[page] > newest)
				newest = mirror.generation[page];
		}
	}
	*segment_handle = mirror.segments[segment];
	*offset = start - segment * MIRROR_SEGMENT_SIZE;
	if (generation)
		*generation = newest;
	stats.mirrored_bytes += size;
	return TRUE;
}

/* the smallest and largest index of an index range the mirror holds: the
same ranges are drawn frame after frame */
#define INDEX_RANGE_SLOTS 4096

static struct
{
	unsigned long address;
	unsigned long count;
	unsigned long generation;
	WORD minimum;
	WORD maximum;
} index_ranges[INDEX_RANGE_SLOTS];

static void index_extent(const WORD *indices, unsigned long count, unsigned long generation, BOOL cached,
	unsigned long *minimum, unsigned long *maximum)
{
	unsigned long slot = (((unsigned long)indices >> 1) ^ (count * 2654435761UL)) % INDEX_RANGE_SLOTS;
	unsigned long index, low = 0xffff, high = 0;

	if (cached && index_ranges[slot].address == (unsigned long)indices && index_ranges[slot].count == count &&
		index_ranges[slot].generation == generation)
	{
		*minimum = index_ranges[slot].minimum;
		*maximum = index_ranges[slot].maximum;
		return;
	}
	for (index = 0; index < count; index++)
	{
		if (indices[index] < low)
			low = indices[index];
		if (indices[index] > high)
			high = indices[index];
	}
	if (cached)
	{
		index_ranges[slot].address = (unsigned long)indices;
		index_ranges[slot].count = count;
		index_ranges[slot].generation = generation;
		index_ranges[slot].minimum = (WORD)low;
		index_ranges[slot].maximum = (WORD)high;
	}
	*minimum = low;
	*maximum = high;
}

/* ---------- vertex data */

/* makes room for size bytes of uploads, orphaning the stream ring if it is
full. A draw reserves room for all of its streams at once: orphaning between
two of them would leave the streams already placed in the ring reading its
new, empty storage. */
static void stream_reserve(unsigned long size)
{
	if (device.stream_offset + size > STREAM_BUFFER_SIZE)
	{
		/* orphan the ring and start again: the backend's next write to it
		maps it with D3D11_MAP_WRITE_DISCARD */
		d3d11_backend_ring_orphan(D3D11_BACKEND_RING_STREAM);
		device.stream_offset = 0;
	}
}

static unsigned long stream_upload(const void *data, unsigned long size)
{
	unsigned long offset;

	size = (size + 15) & ~15UL;
	stream_reserve(size);
	offset = device.stream_offset;
	/* (a write that fails is the lost device's, or one past the ring's end,
	logged by the backend) */
	if (!d3d11_backend_ring_write(D3D11_BACKEND_RING_STREAM, (uint32_t)offset, data, (uint32_t)size))
		backend_lost_check();
	device.stream_offset += size;
	return offset;
}

static unsigned long index_upload(const void *data, unsigned long size)
{
	unsigned long offset;

	size = (size + 15) & ~15UL;
	if (device.index_offset + size > INDEX_BUFFER_SIZE)
	{
		d3d11_backend_ring_orphan(D3D11_BACKEND_RING_INDEX);
		device.index_offset = 0;
	}
	offset = device.index_offset;
	if (!d3d11_backend_ring_write(D3D11_BACKEND_RING_INDEX, (uint32_t)offset, data, (uint32_t)size))
		backend_lost_check();
	device.index_offset += size;
	return offset;
}

/* a stream placed in the stream ring rather than in a segment (stream_data):
the GL renderer named its stream buffer there */
#define STREAMED_IN_RING 0xffffffffu

/* the bytes the input layout's format of an Xbox vertex type reads
(attribute_format, below) */
static unsigned long format_bytes(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return 4;
	case D3DVSDT_FLOAT2: return 8;
	case D3DVSDT_FLOAT3: case D3DVSDT_FLOAT2H: return 12;
	case D3DVSDT_FLOAT4: return 16;
	case D3DVSDT_SHORT1: case D3DVSDT_NORMSHORT1: return 2;
	case D3DVSDT_SHORT2: case D3DVSDT_NORMSHORT2: return 4;
	case D3DVSDT_SHORT3: case D3DVSDT_SHORT4: case D3DVSDT_NORMSHORT3: case D3DVSDT_NORMSHORT4: return 8;
	case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_PBYTE2: return 2;
	default: return 4;
	}
}

/* a stream whose stride is not a multiple of 4, which Direct3D 11's input
assembler wants of a vertex buffer that an element reads (the debug layer's
error 367), and the stride its vertices are copied at instead: the bytes its
elements read, rounded up to 4. The zsprites' texture coordinates are such a
stream (two bytes a vertex, rasterizer_xbox_transparent_geometry.c), which GL
read as they are. 0 when the stride is a multiple of 4 (or 0). */
static unsigned long stream_aligned_stride(const struct vertex_shader_object *declaration, unsigned long stream,
	unsigned long stride, unsigned long *span)
{
	unsigned long index, end = 0;

	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];
		unsigned long bytes = format_bytes(element->type);

		if (element->stream != stream || element->type == D3DVSDT_NONE)
			continue;
		if (element->offset + bytes > end)
			end = element->offset + bytes;
	}
	*span = end;
	if (!stride || stride % 4 == 0)
		return 0;
	return (end + 3) & ~3UL;
}

/* setup_streams' data work (d3d8_gl.c): each stream the draw's declaration
reads, from the mirror where it can be, else streamed, placed in the record
(the segment or the ring, the offset, the stride: the layout's elements name
the slots, draw_layout); the attributes' values are draw_prepare's. A stream
whose stride is not aligned for its formats is streamed with each vertex's
bytes copied at an aligned stride (stream_aligned_stride), the bytes GL read
for it: past the stream's last vertex, zeros. */
static void stream_data(unsigned long first, unsigned long count)
{
	struct vertex_shader_object *declaration = device.vertex_shader;
	uint32_t stream_buffers[16];
	unsigned long stream_offsets[16];
	unsigned long aligned_strides[16] = { 0 }, spans[16] = { 0 };
	BOOL placed[16] = { FALSE };
	unsigned long index, total = 0;

	memset(device.draw.streams, 0, sizeof(device.draw.streams));
	/* the mirror first; then one reservation for everything streamed */
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];
		unsigned long stream = element->stream;
		unsigned long stride = device.streams[stream].stride;
		unsigned long bytes = stride ? stride * count : 64;
		unsigned long base;

		if (!device.streams[stream].data || element->type == D3DVSDT_NONE || placed[stream])
			continue;
		placed[stream] = TRUE;
		stream_buffers[stream] = 0;
		aligned_strides[stream] = stream_aligned_stride(declaration, stream, stride, &spans[stream]);
		if (aligned_strides[stream])
		{
			if (!declaration->stride_logged)
			{
				declaration->stride_logged = TRUE;
				platform_log("Direct3D 11: stream %lu of stride %lu under declaration %lu (vertex shader %lu), whose "
					"elements read %lu bytes of each vertex: its vertices are copied at a stride of %lu (logged once a "
					"declaration)", stream, stride, declaration->id, current_program() ? current_program()->id : 0,
					spans[stream], aligned_strides[stream]);
			}
			total += (aligned_strides[stream] * count + 15) & ~15UL;
			continue;
		}
		base = (unsigned long)PLATFORM_PHYSICAL_TO_VIRTUAL(device.streams[stream].data) + first * stride;
		if (mirror_range(base, bytes, &stream_buffers[stream], &stream_offsets[stream], NULL))
			continue;
		stream_buffers[stream] = 0;
		total += (bytes + 15) & ~15UL;
	}
	stream_reserve(total);
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];
		unsigned long stream = element->stream;
		unsigned long stride = device.streams[stream].stride;

		if (!device.streams[stream].data || element->type == D3DVSDT_NONE)
			continue;
		if (!stream_buffers[stream] && aligned_strides[stream])
		{
			const unsigned char *base = PLATFORM_PHYSICAL_TO_VIRTUAL(device.streams[stream].data) + first * stride;
			unsigned long aligned = aligned_strides[stream], available = stride * count, vertex;
			unsigned char *copy = calloc(count, aligned);

			if (copy)
			{
				for (vertex = 0; vertex < count; vertex++)
				{
					unsigned long at = vertex * stride;
					unsigned long bytes = spans[stream];

					if (at + bytes > available)
						bytes = available - at;
					memcpy(copy + vertex * aligned, base + at, bytes);
				}
				stream_offsets[stream] = stream_upload(copy, aligned * count);
				free(copy);
			}
			else
			{
				stream_offsets[stream] = stream_upload(base, aligned * count < available ? aligned * count : available);
			}
			stream_buffers[stream] = STREAMED_IN_RING;
			stats.streamed_bytes += aligned * count;
			stride = aligned;
		}
		else if (aligned_strides[stream])
		{
			stride = aligned_strides[stream];
		}
		else if (!stream_buffers[stream])
		{
			const unsigned char *base = PLATFORM_PHYSICAL_TO_VIRTUAL(device.streams[stream].data);
			unsigned long bytes = stride ? stride * count : 64;

			stream_offsets[stream] = stream_upload(base + first * stride, bytes);
			stream_buffers[stream] = STREAMED_IN_RING;
			stats.streamed_bytes += bytes;
		}
		/* (state_vertex_buffer: the slot's buffer and offset, in the record) */
		device.draw.streams[stream].kind = stream_buffers[stream] == STREAMED_IN_RING ? D3D11_BACKEND_DATA_RING :
			D3D11_BACKEND_DATA_SEGMENT;
		device.draw.streams[stream].segment = stream_buffers[stream] == STREAMED_IN_RING ? 0 : stream_buffers[stream];
		device.draw.streams[stream].offset = (uint32_t)stream_offsets[stream];
		device.draw.streams[stream].stride = (uint32_t)stride;
	}
}

/* the record's indices: none (a plain draw of count vertices), or count of
them in the index ring or a segment at offset, counting from base_vertex */
static void draw_indices_none(unsigned long count)
{
	device.draw.index_kind = D3D11_BACKEND_DATA_NONE;
	device.draw.index_segment = 0;
	device.draw.index_offset = 0;
	device.draw.index_count = 0;
	device.draw.base_vertex = 0;
	device.draw.vertex_count = (uint32_t)count;
}

static void draw_indices_at(uint32_t kind, uint32_t segment, unsigned long offset, unsigned long count, long base_vertex)
{
	device.draw.index_kind = kind;
	device.draw.index_segment = segment;
	device.draw.index_offset = (uint32_t)offset;
	device.draw.index_count = (uint32_t)count;
	device.draw.base_vertex = (int32_t)base_vertex;
	device.draw.vertex_count = 0;
}

/* quads become two triangles each */
static WORD *quad_indices(const WORD *indices, unsigned long count, unsigned long *out_count)
{
	unsigned long quads = count / 4;
	WORD *result = malloc(quads * 6 * sizeof(WORD) + 2);
	unsigned long quad;

	for (quad = 0; quad < quads; quad++)
	{
		WORD v0 = indices ? indices[quad * 4] : (WORD)(quad * 4);
		WORD v1 = indices ? indices[quad * 4 + 1] : (WORD)(quad * 4 + 1);
		WORD v2 = indices ? indices[quad * 4 + 2] : (WORD)(quad * 4 + 2);
		WORD v3 = indices ? indices[quad * 4 + 3] : (WORD)(quad * 4 + 3);

		result[quad * 6 + 0] = v0;
		result[quad * 6 + 1] = v1;
		result[quad * 6 + 2] = v2;
		result[quad * 6 + 3] = v0;
		result[quad * 6 + 4] = v2;
		result[quad * 6 + 5] = v3;
	}
	*out_count = quads * 6;
	return result;
}

/* a triangle fan or a polygon (Direct3D 11 has neither) as the triangles
(0, i, i + 1), or of an indexed draw (i0, ii, ii + 1) */
static WORD *fan_indices(const WORD *indices, unsigned long count, unsigned long *out_count)
{
	unsigned long triangles = count >= 3 ? count - 2 : 0;
	WORD *result = malloc(triangles * 3 * sizeof(WORD) + 2);
	unsigned long triangle;

	for (triangle = 0; triangle < triangles; triangle++)
	{
		result[triangle * 3 + 0] = indices ? indices[0] : (WORD)0;
		result[triangle * 3 + 1] = indices ? indices[triangle + 1] : (WORD)(triangle + 1);
		result[triangle * 3 + 2] = indices ? indices[triangle + 2] : (WORD)(triangle + 2);
	}
	*out_count = triangles * 3;
	return result;
}

/* a line loop (Direct3D 11 has none) as a line strip with its first vertex
again at its end */
static WORD *loop_indices(const WORD *indices, unsigned long count, unsigned long *out_count)
{
	WORD *result = malloc((count + 1) * sizeof(WORD) + 2);
	unsigned long index;

	for (index = 0; index < count; index++)
		result[index] = indices ? indices[index] : (WORD)index;
	result[count] = count ? result[0] : 0;
	*out_count = count ? count + 1 : 0;
	return result;
}

/* whether the primitive type is drawn from indices made for the draw
(quad_indices, fan_indices, loop_indices) */
static BOOL primitive_made(D3DPRIMITIVETYPE type)
{
	return type == D3DPT_QUADLIST || type == D3DPT_TRIANGLEFAN || type == D3DPT_POLYGON || type == D3DPT_LINELOOP;
}

static WORD *made_indices(D3DPRIMITIVETYPE type, const WORD *indices, unsigned long count, unsigned long *out_count)
{
	switch (type)
	{
	case D3DPT_QUADLIST: return quad_indices(indices, count, out_count);
	case D3DPT_LINELOOP: return loop_indices(indices, count, out_count);
	default: return fan_indices(indices, count, out_count);
	}
}

/* primitive_mode (d3d8_gl.c) as Direct3D 11's topologies: a quad strip is a
triangle strip, a fan, a polygon and a quad list are triangles from made
indices, a line loop a strip from them */
static uint32_t primitive_topology(D3DPRIMITIVETYPE type)
{
	switch (type)
	{
	case D3DPT_POINTLIST: return D3D11_BACKEND_TOPOLOGY_POINTLIST;
	case D3DPT_LINELIST: return D3D11_BACKEND_TOPOLOGY_LINELIST;
	case D3DPT_LINELOOP:
	case D3DPT_LINESTRIP: return D3D11_BACKEND_TOPOLOGY_LINESTRIP;
	case D3DPT_TRIANGLESTRIP:
	case D3DPT_QUADSTRIP: return D3D11_BACKEND_TOPOLOGY_TRIANGLESTRIP;
	default: return D3D11_BACKEND_TOPOLOGY_TRIANGLELIST;
	}
}

/* ---------- the shaders a draw would use (phase 4)

Nothing is drawn yet (phase 6), and nothing is compiled at the draw (phase
5), but at every draw the device makes the pixel shader key as d3d8_gl.c's
prepare_draw makes it, and chooses the vertex shader's variant as it does,
without GL and without binding anything; each first met is counted and, with
debug.gpu_dump_shaders, dumped: its HLSL (nv2a_vsh_hlsl.c, nv2a_psh_hlsl.c),
its GLSL twin from the unchanged generators (the OpenGL executable's text for
the same shader), the key's bytes and a line of the manifest, which
tools/d3d11_shader_check.py takes as its corpus. Phase 5 gives the entries
their handles. */

/* size is a multiple of 4 (d3d8_gl.c's) */
static unsigned long hash_words(const void *data, unsigned long size)
{
	const DWORD *words = data;
	unsigned long hash = 2166136261UL;

	for (size /= 4; size; size--)
		hash = (hash ^ *words++) * 16777619UL;
	return hash;
}

typedef char pixel_shader_key_size_assert[sizeof(struct nv2a_pixel_shader_key) % 4 == 0 ? 1 : -1];

/* FNV-1a, 64 bits: a shader's identity (phase 5's cache names its files by
it) */
#define SHADER_HASH_START 14695981039346656037ULL

static uint64_t shader_hash(uint64_t hash, const void *data, size_t size)
{
	const unsigned char *bytes = data;
	size_t index;

	for (index = 0; index < size; index++)
		hash = (hash ^ bytes[index]) * 1099511628211ULL;
	return hash;
}

static unsigned long stage_texture_mode(int stage)
{
	return (D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f;
}

/* display.per_pixel_lighting: the model lighting programs' draws are lit for
each pixel (nv2a_psh.c model_lighting), from shaders of their own; read
again when a setting changes (d3d8_gl.c's) */
static BOOL per_pixel_lighting(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static BOOL enabled;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		enabled = config_boolean("display.per_pixel_lighting") != 0;
	}
	return enabled;
}

/* the render target with this physical address the game drew into last
(d3d8_gl.c's xgpu_render_target_find, which is not exported here: the
renderer's own table) */
static struct render_target_entry *render_target_find(unsigned long data)
{
	struct render_target_entry *entry, *best = NULL;

	for (entry = *render_target_bucket(data); entry; entry = entry->next_in_bucket)
	{
		if (entry->target.data == data && !entry->target.depth && (!best || entry->last_rendered > best->last_rendered))
			best = entry;
	}
	return best;
}

/* what bind_textures hands configure_sampler for a stage (d3d8_gl.c): whether
the stage has a texture, and so a sampler; whether the texture has mip levels;
whether it is a high-res HUD texture */
struct stage_sampling
{
	BOOL sampled, mipmapped, hires;
};

/* bind_textures' decisions for the key and the record (d3d8_gl.c): a stage
with no texture, or in modes 0, 0x04, 0x05, has no sampler, and in mode 0x11
a 2D one; a render target the stage samples is 2D (the target's handle in
the record, its linear scale from the target's size); otherwise the texture
cache's copy (xbox_textures_d3d11.c) gives the texture's handle, its kind (a
cube map's a cube, a volume's 3D) and its description, with the high-res
flags as the cache answers them (a text atlas's, a menu art's or a HUD
override's). Stage 0's coverage_alpha and point_threshold are the
description's. Each stage's sampling as configure_sampler is given it: a
render target's levels only where bind_textures composites its mip chain
(mip_composite_get), a bitmap's from its description, and high-res where the
cache says so (on any stage, as xbox_textures.c finds it). */
static void key_textures(struct nv2a_pixel_shader_key *key, struct stage_sampling sampling[D3DTSS_MAXSTAGES],
	float texture_scale[D3DTSS_MAXSTAGES][4])
{
	int stage;

	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		D3DBaseTexture *texture = device.textures[stage];
		unsigned long mode = stage_texture_mode(stage);
		struct xgpu_texture_description description;
		struct render_target_entry *target;

		memset(&sampling[stage], 0, sizeof(sampling[stage]));
		texture_scale[stage][0] = texture_scale[stage][1] = 1.0f;
		texture_scale[stage][2] = texture_scale[stage][3] = 1.0f;
		device.draw.textures[stage].kind = D3D11_BACKEND_TEXTURE_NONE;
		device.draw.textures[stage].handle = 0;
		if (!texture || !texture->Data || mode == 0 || mode == 0x04 || mode == 0x05 || mode == 0x11)
		{
			key->sampler_type[stage] = mode == 0x11 ? _xgpu_sampler_2d : _xgpu_sampler_none;
			continue;
		}
		sampling[stage].sampled = TRUE;
		/* (a render target: its description's high-res flags are clear) */
		target = render_target_find(texture->Data);
		if (target)
		{
			xgpu_texture_describe(texture->Format, texture->Size, &description);
			sampling[stage].mipmapped = !description.linear && !description.cube_map && description.levels > 1 &&
				target->target.width == description.width && target->target.height == description.height;
			key->sampler_type[stage] = _xgpu_sampler_2d;
			/* (the target's pixels, as bind_textures samples them; a linear
			texture's coordinates in its texels) */
			device.draw.textures[stage].kind = D3D11_BACKEND_TEXTURE_TARGET;
			device.draw.textures[stage].handle = target->backend_target;
			if (description.linear)
			{
				texture_scale[stage][0] = 1.0f / (float)target->target.width;
				texture_scale[stage][1] = 1.0f / (float)target->target.height;
			}
			continue;
		}
		{
			const D3DCOLOR *palette = device.palettes[stage] && device.palettes[stage]->Data ?
				(const D3DCOLOR *)PLATFORM_PHYSICAL_TO_VIRTUAL(device.palettes[stage]->Data) : NULL;
			struct xgpu_d3d11_texture result;

			xgpu_d3d11_texture_get((const DWORD *)texture, palette, &result);
			description = result.description;
			if (description.linear)
			{
				texture_scale[stage][0] = 1.0f / (float)description.width;
				texture_scale[stage][1] = 1.0f / (float)description.height;
			}
			device.draw.textures[stage].kind = D3D11_BACKEND_TEXTURE_CACHE;
			device.draw.textures[stage].handle = result.handle;
			sampling[stage].mipmapped = description.levels > 1;
			sampling[stage].hires = description.hires;
			if (stage == 0)
			{
				key->coverage_alpha = description.hires_coverage != FALSE;
				key->point_threshold = description.hires_point_threshold != FALSE;
			}
			key->sampler_type[stage] = result.kind == D3D11_BACKEND_TEXTURE_KIND_CUBE ? _xgpu_sampler_cube :
				result.kind == D3D11_BACKEND_TEXTURE_KIND_3D ? _xgpu_sampler_3d : _xgpu_sampler_2d;
		}
	}
}

/* prepare_draw's key (d3d8_gl.c), without GL: alpha_test_samples stays 0
(the target's samples are phase 6's) and count_samples 0 (the visibility
tests are occlusion queries, phase 6); and each stage's sampling */
static void pixel_key_make(struct nv2a_pixel_shader_key *key, struct stage_sampling sampling[D3DTSS_MAXSTAGES],
	float texture_scale[D3DTSS_MAXSTAGES][4])
{
	int stage;

	/* (zeroed first, so that the padding hashes the same) */
	memset(key, 0, sizeof(*key));
	memcpy(key->combiner_state, D3D__RenderState, sizeof(key->combiner_state));
	/* constants are uniforms, not part of the program */
	memset(&key->combiner_state[D3DRS_PSCONSTANT0_0], 0, 16 * sizeof(DWORD));
	key->combiner_state[D3DRS_PSFINALCOMBINERCONSTANT0] = 0;
	key->combiner_state[D3DRS_PSFINALCOMBINERCONSTANT1] = 0;
	key->texture_modes = D3D__RenderState[D3DRS_PSTEXTUREMODES];
	key_textures(key, sampling, texture_scale);
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		key->alpha_kill[stage] = D3D__TextureState[stage][D3DTSS_ALPHAKILL] == D3DTALPHAKILL_ENABLE;
		key->color_sign[stage] = (unsigned char)((D3D__TextureState[stage][D3DTSS_COLORSIGN] >> 28) & 0xf);
	}
	/* (only with the meter's blend: hud_hires.h, nv2a_pixel_shader_key) */
	key->coverage_alpha = key->coverage_alpha && D3D__RenderState[D3DRS_ALPHABLENDENABLE] &&
		D3D__RenderState[D3DRS_SRCBLEND] == D3DBLEND_CONSTANTCOLOR &&
		D3D__RenderState[D3DRS_DESTBLEND] == D3DBLEND_SRCALPHA;
	key->point_threshold = key->point_threshold && key->coverage_alpha;
	key->alpha_test_function = D3D__RenderState[D3DRS_ALPHATESTENABLE] ? D3D__RenderState[D3DRS_ALPHAFUNC] : 0;
	key->fog_enable = D3D__RenderState[D3DRS_FOGENABLE] != 0;
	key->fog_table_mode = (unsigned char)D3D__RenderState[D3DRS_FOGTABLEMODE];
}

/* a pixel shader the draws met, by its key (d3d8_gl.c's fragment_entry),
with the backend's handle once asked for (pixel_shader_handle) */
struct pixel_entry
{
	struct pixel_entry *next;
	unsigned long hash;
	struct nv2a_pixel_shader_key key;
	/* the draw blends with the source factor the constant's alpha and the
	destination's the constant's colour (the transparent meter): the shader
	multiplies by the alpha (HLSL_BY_CONSTANT_ALPHA, or the inverse), beside
	the key and in the identity */
	int by_constant_alpha;
	uint64_t identity;
	uint32_t handle;
	/* its compile failed: its draws are skipped for the run */
	BOOL failed;
};

/* a vertex shader variant the draws met: the program's words, the
declaration's three masks (0 for immediate mode) and lit (vertex_shader_get's
choice, d3d8_gl.c), with the backend's handle once asked for
(vertex_shader_handle) */
struct vertex_variant
{
	struct vertex_variant *next;
	uint64_t identity;
	const struct vertex_shader_object *program;
	uint32_t packed_mask, integer_mask, three_component_mask;
	BOOL lit;
	uint32_t handle;
	/* its compile failed: its draws are skipped for the run */
	BOOL failed;
	/* immediate mode's layout over it (draw_layout), once asked for */
	uint32_t immediate_layout;
	BOOL immediate_layout_made;
};

#define PIXEL_BUCKETS 1024
#define VERTEX_VARIANT_BUCKETS 1024

static struct pixel_entry *pixel_buckets[PIXEL_BUCKETS];
static struct vertex_variant *vertex_variant_buckets[VERTEX_VARIANT_BUCKETS];

/* met since the start, and dumped (debug.gpu_dump_shaders), and dumped when
the log last said */
static struct
{
	unsigned long pixel_keys, vertex_variants;
	unsigned long vertex_dumped, pixel_dumped;
	unsigned long vertex_dumped_said, pixel_dumped_said;
	FILE *manifest;
	BOOL manifest_failed;
	/* the draws skipped for a shader that failed and for a layout that
	could not be made, since the start */
	unsigned long skipped_no_shader, skipped_no_layout;
	/* the layouts the draws made (the backend's handles above layouts_seen
	are new), how many use each Xbox vertex type, and the count when the log
	last said */
	uint32_t layouts_seen;
	unsigned long layouts_made, layouts_said;
	unsigned long layout_types[256];
} shaders_met;

/* a pixel shader's identity: the generator's version, the key's bytes and
the constant-alpha flag */
static uint64_t pixel_identity(const struct nv2a_pixel_shader_key *key, int by_constant_alpha)
{
	uint32_t version = HLSL_SHADER_GENERATOR_VERSION, flag = (uint32_t)by_constant_alpha;

	return shader_hash(shader_hash(shader_hash(SHADER_HASH_START, &version, sizeof(version)), key, sizeof(*key)),
		&flag, sizeof(flag));
}

/* a vertex shader's: the generator's version, the program's words, the three
masks and lit */
static uint64_t vertex_identity(const struct vertex_shader_object *program, uint32_t packed_mask,
	uint32_t integer_mask, uint32_t three_component_mask, BOOL lit)
{
	uint32_t words[6];

	words[0] = HLSL_SHADER_GENERATOR_VERSION;
	words[1] = (uint32_t)program->instruction_count;
	words[2] = packed_mask;
	words[3] = integer_mask;
	words[4] = three_component_mask;
	words[5] = lit ? (uint32_t)program->lighting.lights : 0;
	return shader_hash(shader_hash(SHADER_HASH_START, words, sizeof(words)), program->instructions,
		program->instruction_count * 4 * sizeof(DWORD));
}

/* ---------- the dump (debug.gpu_dump_shaders) */

static void dump_manifest_line(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void dump_manifest_line(const char *format, ...)
{
	va_list arguments;

	if (!shaders_met.manifest && !shaders_met.manifest_failed)
	{
		char path[512];

		/* (appended: the runs of a corpus, the menus' and a map's, write
		into one folder; a shader met again is written again, the same) */
		snprintf(path, sizeof(path), "%s/manifest.txt", debug_settings.dump_shaders);
		shaders_met.manifest = fopen(path, "a");
		if (!shaders_met.manifest)
		{
			shaders_met.manifest_failed = TRUE;
			platform_log("shader dump: cannot write %s", path);
		}
	}
	if (!shaders_met.manifest)
		return;
	va_start(arguments, format);
	vfprintf(shaders_met.manifest, format, arguments);
	va_end(arguments);
	fputc('\n', shaders_met.manifest);
	fflush(shaders_met.manifest);
}

/* a file of the dump; text files are written as d3d8_gl.c writes its dump
("w"), so a GLSL twin is the OpenGL executable's file byte for byte */
static BOOL dump_file(const char *name, const void *data, size_t size, BOOL text)
{
	char path[512];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s", debug_settings.dump_shaders, name);
	if ((file = fopen(path, text ? "w" : "wb")) == NULL)
	{
		platform_log("shader dump: cannot write %s", path);
		return FALSE;
	}
	fwrite(data, 1, size, file);
	fclose(file);
	return TRUE;
}

static void dump_vertex_variant(const struct vertex_variant *variant, BOOL immediate)
{
	const struct vertex_shader_object *program = variant->program;
	char name[64], gl_name[64];
	char *text;

	snprintf(name, sizeof(name), "vs%016llx", (unsigned long long)variant->identity);
	/* (d3d8_gl.c's vertex_shader_get names its dump so) */
	snprintf(gl_name, sizeof(gl_name), "vs%03lu_%d%s.glsl", program->id, immediate ? 1 : 0, variant->lit ? "_lit" : "");
	text = nv2a_hlsl_vertex_shader((const uint32_t *)program->instructions, (uint32_t)program->instruction_count,
		variant->packed_mask, variant->integer_mask, variant->three_component_mask,
		variant->lit ? &program->lighting : NULL);
	if (text)
	{
		char file[80];

		snprintf(file, sizeof(file), "%s.hlsl", name);
		if (dump_file(file, text, strlen(text), TRUE))
		{
			dump_manifest_line("%s stage=vertex entry=%s target=%s packed=%04lx integer=%04lx three=%04lx lit=%d "
				"program=%lu instructions=%lu gl=%s", file, HLSL_ENTRY, HLSL_VERTEX_TARGET,
				(unsigned long)variant->packed_mask, (unsigned long)variant->integer_mask,
				(unsigned long)variant->three_component_mask, variant->lit ? program->lighting.lights : 0, program->id,
				program->instruction_count, gl_name);
		}
		free(text);
	}
	/* the twin: vertex_shader_get's call (d3d8_gl.c), the mask the packed
	one, the lighting the same */
	text = nv2a_vertex_shader_to_glsl(program->instructions, program->instruction_count, variant->packed_mask,
		variant->lit ? &program->lighting : NULL);
	if (text)
	{
		char file[80];

		snprintf(file, sizeof(file), "%s.glsl", name);
		if (dump_file(file, text, strlen(text), TRUE))
			dump_manifest_line("%s stage=vertex twin=%s.hlsl", file, name);
		free(text);
	}
	shaders_met.vertex_dumped++;
}

static void dump_pixel_entry(const struct pixel_entry *entry)
{
	char name[64], file[80], target[8];
	char *text;

	snprintf(name, sizeof(name), "ps%016llx", (unsigned long long)entry->identity);
	text = nv2a_hlsl_pixel_shader(&entry->key, entry->by_constant_alpha, target);
	if (text)
	{
		snprintf(file, sizeof(file), "%s.hlsl", name);
		if (dump_file(file, text, strlen(text), TRUE))
		{
			/* (d3d8_gl.c's fragment_shader_get names its dump by the key's
			hash_words; the constant-alpha mark is the dialect's item 11 for
			tools/d3d11_shader_check.py: the twin is the unflagged text) */
			dump_manifest_line("%s stage=pixel entry=%s target=%s program=%lu gl=ps_%08lx.glsl%s", file, HLSL_ENTRY,
				target, current_program() ? current_program()->id : 0, entry->hash,
				entry->by_constant_alpha == HLSL_BY_CONSTANT_ALPHA ? " constant_alpha=1" :
				entry->by_constant_alpha == HLSL_BY_INVERSE_CONSTANT_ALPHA ? " constant_alpha=2" : "");
		}
		free(text);
	}
	/* the twin: fragment_shader_get's call (d3d8_gl.c) */
	text = nv2a_pixel_shader_to_glsl(&entry->key);
	if (text)
	{
		snprintf(file, sizeof(file), "%s.glsl", name);
		if (dump_file(file, text, strlen(text), TRUE))
			dump_manifest_line("%s stage=pixel twin=%s.hlsl", file, name);
		free(text);
	}
	snprintf(file, sizeof(file), "%s.key", name);
	if (dump_file(file, &entry->key, sizeof(entry->key), FALSE))
		dump_manifest_line("%s stage=pixel key=%s.hlsl bytes=%lu", file, name, (unsigned long)sizeof(entry->key));
	shaders_met.pixel_dumped++;
}

/* the entry of a draw's pixel shader key (d3d8_gl.c's fragment_shader_get:
the four met last, then the buckets by hash_words), made the first time */
static struct pixel_entry *pixel_entry_get(const struct nv2a_pixel_shader_key *key, int by_constant_alpha)
{
	/* consecutive draws mostly use one of a few pixel shaders (an object's
	parts take turns) */
#define RECENT_PIXEL_COUNT 4
	static struct pixel_entry *recent[RECENT_PIXEL_COUNT];
	static unsigned long recent_next;
	unsigned long hash, index;
	struct pixel_entry **bucket;
	struct pixel_entry *entry;

	for (index = 0; index < RECENT_PIXEL_COUNT; index++)
	{
		if (recent[index] && recent[index]->by_constant_alpha == by_constant_alpha &&
			words_equal(&recent[index]->key, key, sizeof(*key)))
			return recent[index];
	}
	hash = hash_words(key, sizeof(*key));
	bucket = &pixel_buckets[hash % PIXEL_BUCKETS];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && entry->by_constant_alpha == by_constant_alpha && words_equal(&entry->key, key, sizeof(*key)))
		{
			recent[recent_next++ % RECENT_PIXEL_COUNT] = entry;
			return entry;
		}
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;
	entry->hash = hash;
	entry->key = *key;
	entry->by_constant_alpha = by_constant_alpha;
	entry->identity = pixel_identity(key, by_constant_alpha);
	entry->next = *bucket;
	*bucket = entry;
	recent[recent_next++ % RECENT_PIXEL_COUNT] = entry;
	shaders_met.pixel_keys++;
	if (debug_settings.dump_shaders)
		dump_pixel_entry(entry);
	return entry;
}

/* the entry of a draw's vertex shader (d3d8_gl.c's vertex_shader_get): the
program's variants as its shader[] and lit_shader[] were, stream or
immediate, lit or not, remembered on the program for the declaration's masks
they were met with; else found in the table by identity, or made */
static struct vertex_variant *vertex_variant_get(struct vertex_shader_object *program, BOOL immediate, BOOL lit)
{
	struct vertex_variant **remembered = &program->variants[lit ? 1 : 0][immediate ? 1 : 0];
	uint32_t packed_mask = immediate ? 0 : (uint32_t)device.vertex_shader->packed_mask;
	uint32_t integer_mask = immediate ? 0 : (uint32_t)device.vertex_shader->integer_mask;
	uint32_t three_component_mask = immediate ? 0 : (uint32_t)device.vertex_shader->three_component_mask;
	struct vertex_variant **bucket;
	struct vertex_variant *variant;
	uint64_t identity;

	variant = *remembered;
	if (variant && variant->packed_mask == packed_mask && variant->integer_mask == integer_mask &&
		variant->three_component_mask == three_component_mask)
	{
		return variant;
	}
	identity = vertex_identity(program, packed_mask, integer_mask, three_component_mask, lit);
	bucket = &vertex_variant_buckets[identity % VERTEX_VARIANT_BUCKETS];
	for (variant = *bucket; variant; variant = variant->next)
	{
		/* (the same words, masks and lighting: the same shader, whichever
		object holds the program) */
		if (variant->identity == identity && variant->packed_mask == packed_mask &&
			variant->integer_mask == integer_mask && variant->three_component_mask == three_component_mask &&
			variant->lit == lit && variant->program->instruction_count == program->instruction_count &&
			!memcmp(variant->program->instructions, program->instructions,
				program->instruction_count * 4 * sizeof(DWORD)) &&
			(!lit || !memcmp(&variant->program->lighting, &program->lighting, sizeof(program->lighting))))
		{
			*remembered = variant;
			return variant;
		}
	}
	variant = calloc(1, sizeof(*variant));
	if (!variant)
		return NULL;
	variant->identity = identity;
	variant->program = program;
	variant->packed_mask = packed_mask;
	variant->integer_mask = integer_mask;
	variant->three_component_mask = three_component_mask;
	variant->lit = lit;
	variant->next = *bucket;
	*bucket = variant;
	*remembered = variant;
	shaders_met.vertex_variants++;
	if (debug_settings.dump_shaders)
		dump_vertex_variant(variant, immediate);
	return variant;
}

/* ---------- the shaders' handles (phase 5)

Each entry asks the backend's shader service for its handle the first time a
draw needs it (d3d11_backend_shader_find: made this run, or from the cache's
file), and on a miss generates its HLSL (nv2a_vsh_hlsl.c, nv2a_psh_hlsl.c)
and has it compiled now (d3d11_backend_shader_make), as d3d8_gl.c compiles
its GLSL at the draw. A shader that fails stays failed for the run. */

static uint32_t vertex_shader_handle(struct vertex_variant *variant)
{
	const struct vertex_shader_object *program;
	char *text;

	if (!variant)
		return 0;
	if (variant->handle || variant->failed)
		return variant->handle;
	variant->handle = d3d11_backend_shader_find(D3D11_BACKEND_STAGE_VERTEX, variant->identity);
	if (!variant->handle)
	{
		program = variant->program;
		text = nv2a_hlsl_vertex_shader((const uint32_t *)program->instructions, (uint32_t)program->instruction_count,
			variant->packed_mask, variant->integer_mask, variant->three_component_mask,
			variant->lit ? &program->lighting : NULL);
		if (text)
			variant->handle = d3d11_backend_shader_make(D3D11_BACKEND_STAGE_VERTEX, variant->identity, text,
				strlen(text), HLSL_ENTRY, HLSL_VERTEX_TARGET);
		free(text);
		backend_lost_check();
	}
	variant->failed = !variant->handle;
	return variant->handle;
}

static uint32_t pixel_shader_handle(struct pixel_entry *entry)
{
	char target[8];
	char *text;

	if (!entry)
		return 0;
	if (entry->handle || entry->failed)
		return entry->handle;
	entry->handle = d3d11_backend_shader_find(D3D11_BACKEND_STAGE_PIXEL, entry->identity);
	if (!entry->handle)
	{
		text = nv2a_hlsl_pixel_shader(&entry->key, entry->by_constant_alpha, target);
		if (text)
			entry->handle = d3d11_backend_shader_make(D3D11_BACKEND_STAGE_PIXEL, entry->identity, text, strlen(text),
				HLSL_ENTRY, target);
		free(text);
		backend_lost_check();
	}
	entry->failed = !entry->handle;
	return entry->handle;
}

/* ---------- input layouts (phase 5)

A layout from the declaration's elements (d3d8_gl.c's setup_streams and
layout_attribute): one element an input register, the last that names it,
the streams with data only; the format from the Xbox type as attribute_format
reads it (below). Kept on the declaration by the vertex shader variant and the
streams with data (d3d8_gl.c keeps its vertex array so), and by the backend
by its hash. Immediate mode's: sixteen four-float attributes, one after
another (End). An input the elements leave out is read from the attribute
slot (d3d11_backend.h). */

/* attribute_format (d3d8_gl.c) as DXGI formats: GL's three-component 16-bit
and 8-bit formats read four here (the shader's three_component_mask gives w
its 1 back), its signed shorts as integers (integer_mask: the shader makes
them floats), a packed normal one 32-bit integer (packed_mask), D3DCOLOR in
its byte order (GL_BGRA) */
static uint32_t attribute_format(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return D3D11_BACKEND_FORMAT_R32_FLOAT;
	case D3DVSDT_FLOAT2: return D3D11_BACKEND_FORMAT_R32G32_FLOAT;
	case D3DVSDT_FLOAT3: case D3DVSDT_FLOAT2H: return D3D11_BACKEND_FORMAT_R32G32B32_FLOAT;
	case D3DVSDT_FLOAT4: return D3D11_BACKEND_FORMAT_R32G32B32A32_FLOAT;
	case D3DVSDT_D3DCOLOR: return D3D11_BACKEND_FORMAT_B8G8R8A8_UNORM;
	case D3DVSDT_SHORT1: return D3D11_BACKEND_FORMAT_R16_SINT;
	case D3DVSDT_SHORT2: return D3D11_BACKEND_FORMAT_R16G16_SINT;
	case D3DVSDT_SHORT3: case D3DVSDT_SHORT4: return D3D11_BACKEND_FORMAT_R16G16B16A16_SINT;
	case D3DVSDT_NORMSHORT1: return D3D11_BACKEND_FORMAT_R16_SNORM;
	case D3DVSDT_NORMSHORT2: return D3D11_BACKEND_FORMAT_R16G16_SNORM;
	case D3DVSDT_NORMSHORT3: case D3DVSDT_NORMSHORT4: return D3D11_BACKEND_FORMAT_R16G16B16A16_SNORM;
	case D3DVSDT_PBYTE1: return D3D11_BACKEND_FORMAT_R8_UNORM;
	case D3DVSDT_PBYTE2: return D3D11_BACKEND_FORMAT_R8G8_UNORM;
	case D3DVSDT_PBYTE3: case D3DVSDT_PBYTE4: return D3D11_BACKEND_FORMAT_R8G8B8A8_UNORM;
	case D3DVSDT_NORMPACKED3: return D3D11_BACKEND_FORMAT_R32_UINT;
	default: return D3D11_BACKEND_FORMAT_R32G32B32A32_FLOAT;
	}
}

static const char *vertex_type_name(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return "FLOAT1";
	case D3DVSDT_FLOAT2: return "FLOAT2";
	case D3DVSDT_FLOAT3: return "FLOAT3";
	case D3DVSDT_FLOAT4: return "FLOAT4";
	case D3DVSDT_FLOAT2H: return "FLOAT2H";
	case D3DVSDT_D3DCOLOR: return "D3DCOLOR";
	case D3DVSDT_SHORT1: return "SHORT1";
	case D3DVSDT_SHORT2: return "SHORT2";
	case D3DVSDT_SHORT3: return "SHORT3";
	case D3DVSDT_SHORT4: return "SHORT4";
	case D3DVSDT_NORMSHORT1: return "NORMSHORT1";
	case D3DVSDT_NORMSHORT2: return "NORMSHORT2";
	case D3DVSDT_NORMSHORT3: return "NORMSHORT3";
	case D3DVSDT_NORMSHORT4: return "NORMSHORT4";
	case D3DVSDT_PBYTE1: return "PBYTE1";
	case D3DVSDT_PBYTE2: return "PBYTE2";
	case D3DVSDT_PBYTE3: return "PBYTE3";
	case D3DVSDT_PBYTE4: return "PBYTE4";
	case D3DVSDT_NORMPACKED3: return "NORMPACKED3";
	default: return NULL;
	}
}

/* the declaration's elements as the layout's, for the streams with data
(a mask); their number. The Xbox types each element had, in types. */
static uint32_t layout_elements(const struct vertex_shader_object *declaration, unsigned long streams,
	struct d3d11_backend_layout_element elements[XGPU_VERTEX_ATTRIBUTE_COUNT],
	unsigned char types[XGPU_VERTEX_ATTRIBUTE_COUNT])
{
	long by_register[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long index;
	uint32_t count = 0;

	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
		by_register[index] = -1;
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];

		if (element->type == D3DVSDT_NONE || element->reg >= XGPU_VERTEX_ATTRIBUTE_COUNT ||
			!(streams & (1UL << element->stream)))
		{
			continue;
		}
		by_register[element->reg] = (long)index;
	}
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		const struct vertex_element *element;

		if (by_register[index] < 0)
			continue;
		element = &declaration->elements[by_register[index]];
		elements[count].semantic_index = (uint32_t)index;
		elements[count].format = attribute_format(element->type);
		elements[count].slot = element->stream;
		elements[count].offset = element->offset;
		types[count] = element->type;
		count++;
	}
	return count;
}

/* a layout the backend answered, if it is new: counted under each Xbox type
its elements have (once a type) */
static void layout_count(uint32_t handle, const unsigned char *types, uint32_t count)
{
	BOOL counted[256] = { FALSE };
	uint32_t index;

	/* (the self-tests' layouts are not the draws') */
	if (shaders_met.layouts_seen < device.shader_statistics_before[D3D11_BACKEND_STATISTIC_LAYOUTS])
		shaders_met.layouts_seen = device.shader_statistics_before[D3D11_BACKEND_STATISTIC_LAYOUTS];
	if (!handle || handle <= shaders_met.layouts_seen)
		return;
	shaders_met.layouts_seen = handle;
	shaders_met.layouts_made++;
	for (index = 0; index < count; index++)
	{
		if (!counted[types[index]])
			shaders_met.layout_types[types[index]]++;
		counted[types[index]] = TRUE;
	}
}

/* the streams the declaration reads that have data (stream_data's test) */
static unsigned long layout_streams(const struct vertex_shader_object *declaration)
{
	unsigned long index, streams = 0;

	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];

		if (element->type != D3DVSDT_NONE && element->stream < 16 && device.streams[element->stream].data)
			streams |= 1UL << element->stream;
	}
	return streams;
}

/* the draw's layout: immediate mode's over the variant, or the
declaration's for the variant and the streams with data; 0 if it could not
be made */
static uint32_t draw_layout(struct vertex_variant *variant, uint32_t vertex_shader, BOOL immediate)
{
	struct vertex_shader_object *declaration = device.vertex_shader;
	struct d3d11_backend_layout_element elements[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned char types[XGPU_VERTEX_ATTRIBUTE_COUNT];
	struct declaration_layout *slot;
	unsigned long streams, index;
	uint32_t count;

	if (immediate)
	{
		if (!variant->immediate_layout_made)
		{
			for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
			{
				elements[index].semantic_index = (uint32_t)index;
				elements[index].format = D3D11_BACKEND_FORMAT_R32G32B32A32_FLOAT;
				elements[index].slot = 0;
				elements[index].offset = (uint32_t)(index * 4 * sizeof(float));
				types[index] = D3DVSDT_FLOAT4;
			}
			variant->immediate_layout = d3d11_backend_layout_make(elements, XGPU_VERTEX_ATTRIBUTE_COUNT, vertex_shader);
			variant->immediate_layout_made = TRUE;
			layout_count(variant->immediate_layout, types, XGPU_VERTEX_ATTRIBUTE_COUNT);
			backend_lost_check();
		}
		return variant->immediate_layout;
	}
	streams = layout_streams(declaration);
	for (index = 0; index < DECLARATION_LAYOUTS && index < declaration->layout_count; index++)
	{
		slot = &declaration->layouts[index];
		if (slot->variant == variant && slot->streams == streams)
			return slot->handle;
	}
	slot = &declaration->layouts[declaration->layout_count++ % DECLARATION_LAYOUTS];
	count = layout_elements(declaration, streams, elements, types);
	slot->variant = variant;
	slot->streams = streams;
	slot->handle = d3d11_backend_layout_make(elements, count, vertex_shader);
	layout_count(slot->handle, types, count);
	backend_lost_check();
	return slot->handle;
}

/* the layouts' line, every 600 frames when more were made: how many, and how
many use each Xbox vertex type */
static void layouts_say(void)
{
	char text[512];
	size_t length = 0;
	int type;

	if (!debug_settings.statistics || device.frame % 600 || shaders_met.layouts_made == shaders_met.layouts_said)
		return;
	shaders_met.layouts_said = shaders_met.layouts_made;
	text[0] = 0;
	for (type = 0; type < 256 && length < sizeof(text) - 32; type++)
	{
		if (shaders_met.layout_types[type])
		{
			length += (size_t)snprintf(text + length, sizeof(text) - length, "%s%s %lu", length ? ", " : "",
				vertex_type_name((unsigned long)type) ? vertex_type_name((unsigned long)type) : "other",
				shaders_met.layout_types[type]);
		}
	}
	platform_log("Direct3D 11: %lu layouts made by the draws; the layouts using each vertex type: %s",
		shaders_met.layouts_made, text);
}

/* ---------- state objects (phase 5)

The draw's blend, depth-stencil and rasterizer state and each sampled
stage's sampler, from D3D__RenderState and D3D__TextureState as
apply_raster_state and configure_sampler (d3d8_gl.c) read them. The Xbox's
values are GL's enumerants (xdk_pdb.h), which GL took as they are: here they
are translated, entry by entry. Each description is zeroed, filled, and
handed to the backend for its handle when it differs from the last one of
its kind (as d3d8_gl.c's gl_state skips a call that changes nothing). A
part a state does not use is left at fixed values (blending off: ONE, ZERO,
ADD; no depth test: ALWAYS; no stencil: KEEP, ALWAYS, all bits), so that
states that draw the same are one object. */

static float dword_float(DWORD value)
{
	float result;

	memcpy(&result, &value, sizeof(result));
	return result;
}

/* a glBlendFunc factor; for the alpha channel (alpha), the colour factors as
their alpha ones, which is what GL's alpha channel reads (Direct3D 11 takes
no colour factor there). The constant's alpha (CONSTANTALPHA) is the blend
factor, whose colour phase 6 sets from the constant's alpha where the draw
asks for it. */
static uint32_t blend_factor(DWORD factor, BOOL alpha)
{
	static BOOL unknown_logged;

	switch (factor)
	{
	case D3DBLEND_ZERO: return D3D11_BACKEND_BLEND_ZERO;
	case D3DBLEND_ONE: return D3D11_BACKEND_BLEND_ONE;
	case D3DBLEND_SRCCOLOR: return alpha ? D3D11_BACKEND_BLEND_SRC_ALPHA : D3D11_BACKEND_BLEND_SRC_COLOR;
	case D3DBLEND_INVSRCCOLOR: return alpha ? D3D11_BACKEND_BLEND_INV_SRC_ALPHA : D3D11_BACKEND_BLEND_INV_SRC_COLOR;
	case D3DBLEND_SRCALPHA: return D3D11_BACKEND_BLEND_SRC_ALPHA;
	case D3DBLEND_INVSRCALPHA: return D3D11_BACKEND_BLEND_INV_SRC_ALPHA;
	case D3DBLEND_DESTALPHA: return D3D11_BACKEND_BLEND_DEST_ALPHA;
	case D3DBLEND_INVDESTALPHA: return D3D11_BACKEND_BLEND_INV_DEST_ALPHA;
	case D3DBLEND_DESTCOLOR: return alpha ? D3D11_BACKEND_BLEND_DEST_ALPHA : D3D11_BACKEND_BLEND_DEST_COLOR;
	case D3DBLEND_INVDESTCOLOR: return alpha ? D3D11_BACKEND_BLEND_INV_DEST_ALPHA : D3D11_BACKEND_BLEND_INV_DEST_COLOR;
	case D3DBLEND_SRCALPHASAT: return D3D11_BACKEND_BLEND_SRC_ALPHA_SAT;
	case D3DBLEND_CONSTANTCOLOR: case D3DBLEND_CONSTANTALPHA: return D3D11_BACKEND_BLEND_BLEND_FACTOR;
	case D3DBLEND_INVCONSTANTCOLOR: case D3DBLEND_INVCONSTANTALPHA: return D3D11_BACKEND_BLEND_INV_BLEND_FACTOR;
	default:
		if (!unknown_logged)
		{
			unknown_logged = TRUE;
			platform_log("Direct3D 11: a blend factor %lu is not one of Direct3D 8's: ONE in its place (logged once)",
				(unsigned long)factor);
		}
		return D3D11_BACKEND_BLEND_ONE;
	}
}

/* blend_equation (d3d8_gl.c) */
static uint32_t blend_operation(DWORD operation)
{
	switch (operation)
	{
	case D3DBLENDOP_SUBTRACT: return D3D11_BACKEND_BLEND_OP_SUBTRACT;
	case D3DBLENDOP_REVSUBTRACT:
	case D3DBLENDOP_REVSUBTRACTSIGNED: return D3D11_BACKEND_BLEND_OP_REV_SUBTRACT;
	case D3DBLENDOP_MIN: return D3D11_BACKEND_BLEND_OP_MIN;
	case D3DBLENDOP_MAX: return D3D11_BACKEND_BLEND_OP_MAX;
	default: return D3D11_BACKEND_BLEND_OP_ADD;
	}
}

/* a D3DCMPFUNC (GL's GL_NEVER to GL_ALWAYS, 512 to 519; 0 is GL_NEVER, as
apply_raster_state reads it) as D3D11_COMPARISON_NEVER (1) to _ALWAYS (8) */
static uint32_t comparison(DWORD function)
{
	if (function >= D3DCMP_NEVER && function <= D3DCMP_ALWAYS)
		return (uint32_t)(function - D3DCMP_NEVER) + D3D11_BACKEND_COMPARISON_NEVER;
	return function ? D3D11_BACKEND_COMPARISON_ALWAYS : D3D11_BACKEND_COMPARISON_NEVER;
}

/* stencil_operation (d3d8_gl.c): GL's GL_INCR and GL_DECR saturate, its
_WRAP forms are Direct3D's INCR and DECR */
static uint32_t stencil_operation(DWORD operation)
{
	switch (operation)
	{
	case D3DSTENCILOP_ZERO: return D3D11_BACKEND_STENCIL_OP_ZERO;
	case D3DSTENCILOP_REPLACE: return D3D11_BACKEND_STENCIL_OP_REPLACE;
	case D3DSTENCILOP_INCRSAT: return D3D11_BACKEND_STENCIL_OP_INCR_SAT;
	case D3DSTENCILOP_DECRSAT: return D3D11_BACKEND_STENCIL_OP_DECR_SAT;
	case D3DSTENCILOP_INVERT: return D3D11_BACKEND_STENCIL_OP_INVERT;
	case D3DSTENCILOP_INCR: return D3D11_BACKEND_STENCIL_OP_INCR;
	case D3DSTENCILOP_DECR: return D3D11_BACKEND_STENCIL_OP_DECR;
	default: return D3D11_BACKEND_STENCIL_OP_KEEP;
	}
}

/* address_mode (d3d8_gl.c) */
static uint32_t address_mode(DWORD mode)
{
	switch (mode)
	{
	case D3DTADDRESS_MIRROR: return D3D11_BACKEND_ADDRESS_MIRROR;
	case D3DTADDRESS_CLAMP: return D3D11_BACKEND_ADDRESS_CLAMP;
	case D3DTADDRESS_BORDER: return D3D11_BACKEND_ADDRESS_BORDER;
	case D3DTADDRESS_CLAMPTOEDGE: return D3D11_BACKEND_ADDRESS_CLAMP;
	default: return D3D11_BACKEND_ADDRESS_WRAP;
	}
}

/* a factor that reads the blend constant's colour, or its alpha */
static BOOL factor_constant_color(DWORD factor)
{
	return factor == D3DBLEND_CONSTANTCOLOR || factor == D3DBLEND_INVCONSTANTCOLOR;
}

static BOOL factor_constant_alpha(DWORD factor)
{
	return factor == D3DBLEND_CONSTANTALPHA || factor == D3DBLEND_INVCONSTANTALPHA;
}

/* the draw blends with the source factor the constant's alpha and the
destination factor the constant's colour (the transparent meter's blend,
rasterizer_xbox_transparent_geometry.c, the one such pair in the game): GL
read the constant's alpha in every channel for the source and the constant's
colour for the destination, and Direct3D 11 has one blend factor for both,
so the source's factor goes into the pixel shader (HLSL_BY_CONSTANT_ALPHA,
or the inverse) and the blend state's source factors are ONE; 0 otherwise */
static int constant_alpha_mode(void)
{
	DWORD *rs = D3D__RenderState;

	if (rs[D3DRS_ALPHABLENDENABLE] && factor_constant_alpha(rs[D3DRS_SRCBLEND]) &&
		factor_constant_color(rs[D3DRS_DESTBLEND]))
	{
		return rs[D3DRS_SRCBLEND] == D3DBLEND_CONSTANTALPHA ? HLSL_BY_CONSTANT_ALPHA : HLSL_BY_INVERSE_CONSTANT_ALPHA;
	}
	return HLSL_BY_CONSTANT_ALPHA_NONE;
}

/* the blend factor OMSetBlendState takes: the constant's colour (its alpha in
w) where a factor reads the constant's colour; else, where one reads the
constant's alpha alone, that alpha in every channel, as GL's CONSTANT_ALPHA
reads it */
static void blend_factor_values(float factor[4])
{
	DWORD *rs = D3D__RenderState;

	color_to_vec4(rs[D3DRS_BLENDCOLOR], factor);
	if (!factor_constant_color(rs[D3DRS_SRCBLEND]) && !factor_constant_color(rs[D3DRS_DESTBLEND]) &&
		(factor_constant_alpha(rs[D3DRS_SRCBLEND]) || factor_constant_alpha(rs[D3DRS_DESTBLEND])))
	{
		factor[0] = factor[1] = factor[2] = factor[3];
	}
}

/* apply_raster_state's blending and colour mask; with by_constant_alpha
(constant_alpha_mode) the source factors are ONE, the shader having
multiplied by the constant's alpha */
static void blend_description(struct d3d11_backend_blend *blend, int by_constant_alpha)
{
	DWORD *rs = D3D__RenderState;
	DWORD write = rs[D3DRS_COLORWRITEENABLE];
	static BOOL other_way_logged;

	memset(blend, 0, sizeof(*blend));
	if (rs[D3DRS_ALPHABLENDENABLE])
	{
		blend->enable = 1;
		blend->source = blend_factor(rs[D3DRS_SRCBLEND], FALSE);
		blend->destination = blend_factor(rs[D3DRS_DESTBLEND], FALSE);
		blend->operation = blend_operation(rs[D3DRS_BLENDOP]);
		blend->source_alpha = blend_factor(rs[D3DRS_SRCBLEND], TRUE);
		blend->destination_alpha = blend_factor(rs[D3DRS_DESTBLEND], TRUE);
		blend->operation_alpha = blend->operation;
		if (by_constant_alpha)
			blend->source = blend->source_alpha = D3D11_BACKEND_BLEND_ONE;
		/* (the other way round, a source constant colour with a destination
		constant alpha, is not in the game: the factor is the constant's
		colour, said once) */
		if (!other_way_logged && factor_constant_color(rs[D3DRS_SRCBLEND]) && factor_constant_alpha(rs[D3DRS_DESTBLEND]))
		{
			other_way_logged = TRUE;
			platform_log("Direct3D 11: a draw blends with a source constant colour and a destination constant alpha "
				"(source %lu, destination %lu): both read the constant's colour (logged once)",
				(unsigned long)rs[D3DRS_SRCBLEND], (unsigned long)rs[D3DRS_DESTBLEND]);
		}
	}
	else
	{
		blend->source = blend->source_alpha = D3D11_BACKEND_BLEND_ONE;
		blend->destination = blend->destination_alpha = D3D11_BACKEND_BLEND_ZERO;
		blend->operation = blend->operation_alpha = D3D11_BACKEND_BLEND_OP_ADD;
	}
	blend->write_mask = ((write & D3DCOLORWRITEENABLE_RED) ? D3D11_BACKEND_CLEAR_R : 0) |
		((write & D3DCOLORWRITEENABLE_GREEN) ? D3D11_BACKEND_CLEAR_G : 0) |
		((write & D3DCOLORWRITEENABLE_BLUE) ? D3D11_BACKEND_CLEAR_B : 0) |
		((write & D3DCOLORWRITEENABLE_ALPHA) ? D3D11_BACKEND_CLEAR_A : 0);
}

/* apply_raster_state's depth and stencil, with bind_targets' has_depth */
static void depth_stencil_description(struct d3d11_backend_depth_stencil *depth, BOOL has_depth)
{
	DWORD *rs = D3D__RenderState;

	memset(depth, 0, sizeof(*depth));
	depth->depth_enable = has_depth && rs[D3DRS_ZENABLE];
	depth->depth_write = depth->depth_enable && rs[D3DRS_ZWRITEENABLE];
	depth->depth_function = depth->depth_enable ? comparison(rs[D3DRS_ZFUNC]) : D3D11_BACKEND_COMPARISON_ALWAYS;
	depth->stencil_enable = has_depth && rs[D3DRS_STENCILENABLE];
	if (depth->stencil_enable)
	{
		depth->stencil_read_mask = rs[D3DRS_STENCILMASK] & 0xff;
		depth->stencil_write_mask = rs[D3DRS_STENCILWRITEMASK] & 0xff;
		depth->stencil_fail = stencil_operation(rs[D3DRS_STENCILFAIL]);
		depth->stencil_depth_fail = stencil_operation(rs[D3DRS_STENCILZFAIL]);
		depth->stencil_pass = stencil_operation(rs[D3DRS_STENCILPASS]);
		depth->stencil_function = comparison(rs[D3DRS_STENCILFUNC]);
	}
	else
	{
		depth->stencil_read_mask = depth->stencil_write_mask = 0xff;
		depth->stencil_fail = depth->stencil_depth_fail = depth->stencil_pass = D3D11_BACKEND_STENCIL_OP_KEEP;
		depth->stencil_function = D3D11_BACKEND_COMPARISON_ALWAYS;
	}
}

/* apply_raster_state's culling, fill and polygon offset (the desktop
branch's front face: no flip, "Clip space" in D3D11.md) */
static void rasterizer_description(struct d3d11_backend_rasterizer *rasterizer)
{
	DWORD *rs = D3D__RenderState;
	static BOOL point_logged;

	memset(rasterizer, 0, sizeof(*rasterizer));
	rasterizer->fill = rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME ? D3D11_BACKEND_FILL_WIREFRAME : D3D11_BACKEND_FILL_SOLID;
	if (rs[D3DRS_FILLMODE] == D3DFILL_POINT && !point_logged)
	{
		point_logged = TRUE;
		platform_log("Direct3D 11: a draw asks for point fill, which Direct3D 11 does not have: solid (logged once)");
	}
	if (rs[D3DRS_CULLMODE] == D3DCULL_NONE)
	{
		rasterizer->cull = D3D11_BACKEND_CULL_NONE;
	}
	else
	{
		rasterizer->cull = rs[D3DRS_CULLMODE] == rs[D3DRS_FRONTFACE] ? D3D11_BACKEND_CULL_FRONT : D3D11_BACKEND_CULL_BACK;
		rasterizer->front_counter_clockwise = rs[D3DRS_FRONTFACE] == D3DFRONT_CCW;
	}
	/* D3DRS_ZBIAS is expressed in these (D3DDevice_SetRenderState_ZBias):
	GL's units and Direct3D's DepthBias both count the depth format's
	smallest step, so the offset is rounded to them */
	if (rs[D3DRS_SOLIDOFFSETENABLE])
	{
		float slope = dword_float(rs[D3DRS_POLYGONOFFSETZSLOPESCALE]);
		float offset = dword_float(rs[D3DRS_POLYGONOFFSETZOFFSET]);

		if (offset == offset && offset > -1.0e9f && offset < 1.0e9f)
			rasterizer->depth_bias = (int32_t)(offset < 0.0f ? offset - 0.5f : offset + 0.5f);
		rasterizer->slope_scaled_depth_bias = slope == slope && slope > -1.0e30f && slope < 1.0e30f ? slope : 0.0f;
	}
	rasterizer->depth_clip = 1;
	rasterizer->scissor = 1;
}

/* configure_sampler's inputs (d3d8_gl.c): the stage's state, or the
high-res HUD texture's filtering */
#define SAMPLER_STATE_WORDS 11

static void sampler_inputs(int stage, BOOL mipmapped, BOOL hires, DWORD inputs[SAMPLER_STATE_WORDS])
{
	DWORD *state = D3D__TextureState[stage];

	inputs[0] = hires ? D3DTEXF_LINEAR : state[D3DTSS_MINFILTER];
	inputs[1] = hires ? D3DTEXF_LINEAR : mipmapped ? state[D3DTSS_MIPFILTER] : D3DTEXF_NONE;
	inputs[2] = hires ? D3DTEXF_LINEAR : state[D3DTSS_MAGFILTER];
	inputs[3] = state[D3DTSS_ADDRESSU];
	inputs[4] = state[D3DTSS_ADDRESSV];
	inputs[5] = state[D3DTSS_ADDRESSW];
	inputs[6] = hires ? 0 : state[D3DTSS_MIPMAPLODBIAS];
	inputs[7] = hires ? 0 : state[D3DTSS_MAXMIPLEVEL];
	inputs[8] = state[D3DTSS_MAXANISOTROPY];
	inputs[9] = state[D3DTSS_BORDERCOLOR];
	inputs[10] = hires;
}

/* sampler_parameters (d3d8_gl.c) as a description: the minification
filter point for POINT and linear otherwise, the magnification the same,
the mip filter none (NONE: level 0 alone, MinLOD and MaxLOD 0, which is what
GL's filters without mipmaps read), point (POINT) or linear; anisotropic when
the minification is ANISOTROPIC and MAXANISOTROPY above 1 (Direct3D 11's
anisotropic filter is linear in all three); the bias from MIPMAPLODBIAS's
float, held to Direct3D's range; MinLOD from MAXMIPLEVEL; the border colour
where an address mode is BORDER */
static void sampler_description(const DWORD inputs[SAMPLER_STATE_WORDS], struct d3d11_backend_sampler *sampler)
{
	DWORD min_filter = inputs[0], mip_filter = inputs[1], mag_filter = inputs[2], anisotropy = inputs[8];
	float bias = dword_float(inputs[6]);

	memset(sampler, 0, sizeof(*sampler));
	if (min_filter == D3DTEXF_ANISOTROPIC && anisotropy > 1)
	{
		sampler->filter = D3D11_BACKEND_FILTER_ANISOTROPIC;
		sampler->max_anisotropy = anisotropy < 16 ? (uint32_t)anisotropy : 16;
	}
	else
	{
		sampler->filter = D3D11_BACKEND_FILTER(min_filter != D3DTEXF_POINT, mag_filter != D3DTEXF_POINT,
			mip_filter != D3DTEXF_NONE && mip_filter != D3DTEXF_POINT);
		sampler->max_anisotropy = 1;
	}
	sampler->address_u = address_mode(inputs[3]);
	sampler->address_v = address_mode(inputs[4]);
	sampler->address_w = address_mode(inputs[5]);
	sampler->mip_lod_bias = bias == bias ? (bias < -16.0f ? -16.0f : bias > 15.99f ? 15.99f : bias) : 0.0f;
	sampler->comparison = D3D11_BACKEND_COMPARISON_NEVER;
	if (mip_filter != D3DTEXF_NONE)
	{
		sampler->min_lod = (float)inputs[7];
		sampler->max_lod = FLT_MAX;
	}
	if (sampler->address_u == D3D11_BACKEND_ADDRESS_BORDER || sampler->address_v == D3D11_BACKEND_ADDRESS_BORDER ||
		sampler->address_w == D3D11_BACKEND_ADDRESS_BORDER)
	{
		color_to_vec4(inputs[9], sampler->border_color);
	}
}

/* the last description of each kind and its handle (the device asks the
backend only for one that differs), and each stage's sampler inputs */
static struct
{
	BOOL blend_made, depth_stencil_made, rasterizer_made;
	struct d3d11_backend_blend blend;
	struct d3d11_backend_depth_stencil depth_stencil;
	struct d3d11_backend_rasterizer rasterizer;
	uint32_t blend_handle, depth_stencil_handle, rasterizer_handle;
	BOOL sampler_made[D3DTSS_MAXSTAGES];
	DWORD sampler_inputs[D3DTSS_MAXSTAGES][SAMPLER_STATE_WORDS];
	uint32_t sampler_handle[D3DTSS_MAXSTAGES];
} state_objects;

/* the draw's state objects into device.draw (has_depth: bind_targets') */
static void draw_states(const struct stage_sampling sampling[D3DTSS_MAXSTAGES], BOOL has_depth, int by_constant_alpha)
{
	struct d3d11_backend_blend blend;
	struct d3d11_backend_depth_stencil depth_stencil;
	struct d3d11_backend_rasterizer rasterizer;
	int stage;

	blend_description(&blend, by_constant_alpha);
	if (!state_objects.blend_made || !words_equal(&blend, &state_objects.blend, sizeof(blend)))
	{
		state_objects.blend = blend;
		state_objects.blend_handle = d3d11_backend_blend_make(&blend);
		state_objects.blend_made = TRUE;
	}
	depth_stencil_description(&depth_stencil, has_depth);
	if (!state_objects.depth_stencil_made || !words_equal(&depth_stencil, &state_objects.depth_stencil,
		sizeof(depth_stencil)))
	{
		state_objects.depth_stencil = depth_stencil;
		state_objects.depth_stencil_handle = d3d11_backend_depth_stencil_make(&depth_stencil);
		state_objects.depth_stencil_made = TRUE;
	}
	rasterizer_description(&rasterizer);
	if (!state_objects.rasterizer_made || !words_equal(&rasterizer, &state_objects.rasterizer, sizeof(rasterizer)))
	{
		state_objects.rasterizer = rasterizer;
		state_objects.rasterizer_handle = d3d11_backend_rasterizer_make(&rasterizer);
		state_objects.rasterizer_made = TRUE;
	}
	device.draw.blend = state_objects.blend_handle;
	device.draw.depth_stencil = state_objects.depth_stencil_handle;
	device.draw.rasterizer = state_objects.rasterizer_handle;
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		DWORD inputs[SAMPLER_STATE_WORDS];

		if (!sampling[stage].sampled)
		{
			device.draw.samplers[stage] = 0;
			continue;
		}
		sampler_inputs(stage, sampling[stage].mipmapped, sampling[stage].hires, inputs);
		if (!state_objects.sampler_made[stage] || !words_equal(inputs, state_objects.sampler_inputs[stage],
			sizeof(inputs)))
		{
			struct d3d11_backend_sampler sampler;

			sampler_description(inputs, &sampler);
			memcpy(state_objects.sampler_inputs[stage], inputs, sizeof(inputs));
			state_objects.sampler_handle[stage] = d3d11_backend_sampler_make(&sampler);
			state_objects.sampler_made[stage] = TRUE;
		}
		device.draw.samplers[stage] = state_objects.sampler_handle[stage];
	}
	backend_lost_check();
}

/* ---------- the constant buffers (prepare_draw's uniforms, d3d8_gl.c)

The vertex constants: one DYNAMIC buffer for every program, written whole
when constants_serial moved since its last write. The model lights: the lit
pixel shaders' b1, from the same registers (model_light_register), written
when any of the twelve moved since. The other uniforms: compared as
draw_uniform_inputs there (the viewport's scale and offset, the texture
scales, the combiner constants, fog, the alpha reference, UI_OFFSET, the bump
matrices and luminance; POINTSIZE and MIPMAPLODBIAS out, the HLSL having no
point size and the bias being the sampler's; BLENDCOLOR in) and, when they
differ, converted as there into hlsl_shaders.h's two structures, each
written when its own values changed. */

static unsigned long long constants_written_serial;
static unsigned long long lights_written_serial;
static BOOL lights_written;

/* the vertex constant register of each of the per-pixel lighting's
(XGPU_MODEL_LIGHT_COUNT) */
static unsigned long model_light_register(int light)
{
	return (unsigned long)(XGPU_VERTEX_CONSTANT_BIAS + (light ? -80 + light : -82));
}

static void constants_update(BOOL lit)
{
	if (constants_written_serial != constants_serial)
	{
		d3d11_backend_constants_write(D3D11_BACKEND_CONSTANTS_VERTEX, device.constants, sizeof(device.constants));
		constants_written_serial = constants_serial;
	}
	/* the lights of a draw lit for each pixel, from the same registers, when
	any of them changed since the buffer last had them */
	if (lit && (!lights_written || lights_written_serial != constants_serial))
	{
		int light = 0;

		if (lights_written)
		{
			for (light = 0; light < XGPU_MODEL_LIGHT_COUNT; light++)
			{
				if (constant_serials[model_light_register(light)] > lights_written_serial)
					break;
			}
		}
		if (light < XGPU_MODEL_LIGHT_COUNT)
		{
			struct hlsl_model_lights lights;

			for (light = 0; light < XGPU_MODEL_LIGHT_COUNT; light++)
				memcpy(lights.model_lights[light], device.constants[model_light_register(light)], sizeof(lights.model_lights[0]));
			d3d11_backend_constants_write(D3D11_BACKEND_CONSTANTS_MODEL_LIGHTS, &lights, sizeof(lights));
			lights_written = TRUE;
		}
		lights_written_serial = constants_serial;
	}
}

/* the uniforms of the latest draws, converted from these inputs */
#define DRAW_UNIFORM_INPUT_COUNT (4 + 4 + 16 + 16 + 2 + 4 + 1 + 1 + 1 + 6 * D3DTSS_MAXSTAGES)

static DWORD draw_uniform_inputs[DRAW_UNIFORM_INPUT_COUNT];
static BOOL draw_uniforms_valid;
static struct hlsl_vertex_parameters vertex_parameters_written;
static struct hlsl_pixel_parameters pixel_parameters_written;
static BOOL vertex_parameters_valid, pixel_parameters_valid;

static float dword_to_float(DWORD value)
{
	union { DWORD d; float f; } u;

	u.d = value;
	return u.f;
}

static void uniforms_update(const float texture_scale[D3DTSS_MAXSTAGES][4])
{
	DWORD inputs[DRAW_UNIFORM_INPUT_COUNT];
	unsigned long count = 0;
	struct hlsl_vertex_parameters vertex;
	struct hlsl_pixel_parameters pixel;
	int stage;

	/* the state the uniforms come from: most draws share it with the draw
	before them, and so share its uniforms */
	memcpy(&inputs[count], device.viewport_scale, sizeof(device.viewport_scale));
	count += 4;
	memcpy(&inputs[count], device.viewport_offset, sizeof(device.viewport_offset));
	count += 4;
	memcpy(&inputs[count], texture_scale, 16 * sizeof(DWORD));
	count += 16;
	for (stage = 0; stage < 8; stage++)
	{
		inputs[count++] = D3D__RenderState[D3DRS_PSCONSTANT0_0 + stage];
		inputs[count++] = D3D__RenderState[D3DRS_PSCONSTANT1_0 + stage];
	}
	inputs[count++] = D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0];
	inputs[count++] = D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1];
	inputs[count++] = D3D__RenderState[D3DRS_FOGCOLOR];
	inputs[count++] = D3D__RenderState[D3DRS_FOGSTART];
	inputs[count++] = D3D__RenderState[D3DRS_FOGEND];
	inputs[count++] = D3D__RenderState[D3DRS_FOGDENSITY];
	inputs[count++] = D3D__RenderState[D3DRS_ALPHAREF];
	inputs[count++] = (DWORD)UI_OFFSET;
	inputs[count++] = D3D__RenderState[D3DRS_BLENDCOLOR];
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		DWORD *state = D3D__TextureState[stage];

		inputs[count++] = state[D3DTSS_BUMPENVMAT00];
		inputs[count++] = state[D3DTSS_BUMPENVMAT01];
		inputs[count++] = state[D3DTSS_BUMPENVMAT10];
		inputs[count++] = state[D3DTSS_BUMPENVMAT11];
		inputs[count++] = state[D3DTSS_BUMPENVLSCALE];
		inputs[count++] = state[D3DTSS_BUMPENVLOFFSET];
	}
	if (draw_uniforms_valid && words_equal(inputs, draw_uniform_inputs, sizeof(inputs)))
		return;
	memcpy(draw_uniform_inputs, inputs, sizeof(inputs));
	draw_uniforms_valid = TRUE;

	memset(&vertex, 0, sizeof(vertex));
	memcpy(vertex.viewport_scale, device.viewport_scale, sizeof(vertex.viewport_scale));
	memcpy(vertex.viewport_offset, device.viewport_offset, sizeof(vertex.viewport_offset));
	vertex.screen[0] = (float)UI_OFFSET;
	if (!vertex_parameters_valid || !words_equal(&vertex, &vertex_parameters_written, sizeof(vertex)))
	{
		d3d11_backend_constants_write(D3D11_BACKEND_CONSTANTS_VERTEX_PARAMETERS, &vertex, sizeof(vertex));
		vertex_parameters_written = vertex;
		vertex_parameters_valid = TRUE;
	}

	memset(&pixel, 0, sizeof(pixel));
	for (stage = 0; stage < 8; stage++)
	{
		color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT0_0 + stage], pixel.ps_c0[stage]);
		color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT1_0 + stage], pixel.ps_c1[stage]);
	}
	color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0], pixel.ps_final_c0);
	color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1], pixel.ps_final_c1);
	color_to_vec4(D3D__RenderState[D3DRS_FOGCOLOR], pixel.fog_color);
	pixel.fog_parameters[0] = dword_to_float(D3D__RenderState[D3DRS_FOGSTART]);
	pixel.fog_parameters[1] = dword_to_float(D3D__RenderState[D3DRS_FOGEND]);
	pixel.fog_parameters[2] = dword_to_float(D3D__RenderState[D3DRS_FOGDENSITY]);
	pixel.fog_parameters[3] = 0.0f;
	pixel.alpha_reference[0] = (float)(D3D__RenderState[D3DRS_ALPHAREF] & 0xff);
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		DWORD *state = D3D__TextureState[stage];

		pixel.bump_matrix[stage][0] = dword_to_float(state[D3DTSS_BUMPENVMAT00]);
		pixel.bump_matrix[stage][1] = dword_to_float(state[D3DTSS_BUMPENVMAT01]);
		pixel.bump_matrix[stage][2] = dword_to_float(state[D3DTSS_BUMPENVMAT10]);
		pixel.bump_matrix[stage][3] = dword_to_float(state[D3DTSS_BUMPENVMAT11]);
		pixel.bump_luminance[stage][0] = dword_to_float(state[D3DTSS_BUMPENVLSCALE]);
		pixel.bump_luminance[stage][1] = dword_to_float(state[D3DTSS_BUMPENVLOFFSET]);
		pixel.bump_luminance[stage][2] = pixel.bump_luminance[stage][3] = 0.0f;
		memcpy(pixel.texture_scale[stage], texture_scale[stage], sizeof(pixel.texture_scale[stage]));
	}
	color_to_vec4(D3D__RenderState[D3DRS_BLENDCOLOR], pixel.blend_constant);
	if (!pixel_parameters_valid || !words_equal(&pixel, &pixel_parameters_written, sizeof(pixel)))
	{
		d3d11_backend_constants_write(D3D11_BACKEND_CONSTANTS_PIXEL_PARAMETERS, &pixel, sizeof(pixel));
		pixel_parameters_written = pixel;
		pixel_parameters_valid = TRUE;
	}
}

/* ---------- the draw's preparation (prepare_draw, d3d8_gl.c)

A draw past draw_ready, in prepare_draw's order with the backend's calls
where the GL was: its key and its textures (the texture cache: step 2), the
targets after the textures (bind_targets), its vertex shader's variant and
the two shaders (made now if this run has not made them; a lit pair that
fails sets lighting_failed and logs prepare_draw's line, and the draw takes
the unlit variant with the key's per_pixel_lighting 0, as there), its
layout, its state objects, the constant buffers, and the record's dynamic
values and attribute slot. FALSE if the draw is skipped (counted): no
target, a shader that failed, a layout that could not be made. */
static BOOL draw_prepare(BOOL immediate)
{
	struct vertex_shader_object *program = current_program();
	struct nv2a_pixel_shader_key key;
	struct stage_sampling sampling[D3DTSS_MAXSTAGES];
	float texture_scale[D3DTSS_MAXSTAGES][4];
	struct vertex_variant *variant = NULL;
	uint32_t vertex = 0, pixel = 0;
	BOOL has_depth = FALSE;
	int by_constant_alpha = constant_alpha_mode();
	int stage;

	pixel_key_make(&key, sampling, texture_scale);
	/* the targets after the textures: a render target the draw samples has
	its multisampled pixels resolved first, and the back buffer can be both
	sampled and drawn into (the backend samples a copy then) */
	if (!bind_targets(&has_depth))
	{
		stats.skipped_no_target++;
		return FALSE;
	}
	if (program->lighting.lights && !program->lighting_failed && per_pixel_lighting())
	{
		key.per_pixel_lighting = (unsigned char)program->lighting.lights;
		variant = vertex_variant_get(program, immediate, TRUE);
		vertex = vertex_shader_handle(variant);
		pixel = pixel_shader_handle(pixel_entry_get(&key, by_constant_alpha));
		if (!vertex || !pixel)
		{
			/* drawn as the vertex shader lights it instead (not at all,
			were it to fail too) */
			platform_log("GPU: vertex shader %lu cannot be lit for each pixel here (refer to the shader log above): "
				"lit for each vertex", program->id);
			program->lighting_failed = TRUE;
			key.per_pixel_lighting = 0;
		}
	}
	if (!vertex || !pixel)
	{
		variant = vertex_variant_get(program, immediate, FALSE);
		vertex = vertex_shader_handle(variant);
		pixel = pixel_shader_handle(pixel_entry_get(&key, by_constant_alpha));
	}
	if (!vertex || !pixel)
	{
		shaders_met.skipped_no_shader++;
		return FALSE;
	}
	device.draw.vertex_shader = vertex;
	device.draw.pixel_shader = pixel;
	device.draw.layout = draw_layout(variant, vertex, immediate);
	if (!device.draw.layout)
	{
		shaders_met.skipped_no_layout++;
		return FALSE;
	}
	draw_states(sampling, has_depth, by_constant_alpha);
	constants_update(key.per_pixel_lighting != 0);
	uniforms_update(texture_scale);
	/* the dynamic values: the blend factor, the stencil reference, the
	viewport in the targets' pixels (apply_raster_state's, row 0 at the top:
	no flip) and its depth range; the scissor is the backend's, the same
	rectangle */
	blend_factor_values(device.draw.blend_factor);
	device.draw.stencil_reference = D3D__RenderState[D3DRS_STENCILREF];
	device.draw.viewport[0] = target_pixel((float)device.viewport.X, 0);
	device.draw.viewport[1] = target_pixel((float)device.viewport.Y, 1);
	device.draw.viewport[2] = target_pixel((float)(device.viewport.X + device.viewport.Width), 0) - device.draw.viewport[0];
	device.draw.viewport[3] = target_pixel((float)(device.viewport.Y + device.viewport.Height), 1) - device.draw.viewport[1];
	device.draw.min_depth = device.viewport.MinZ;
	device.draw.max_depth = device.viewport.MaxZ;
	/* the attribute slot: the input registers' current values, read by the
	inputs the layout's elements leave out; a packed register's the integer
	zero, as GL gave it (setup_streams: state_attribute_value(index, NULL)) */
	memcpy(device.draw.attributes, device.attributes, sizeof(device.draw.attributes));
	if (!immediate)
	{
		for (stage = 0; stage < XGPU_VERTEX_ATTRIBUTE_COUNT; stage++)
		{
			if (device.vertex_shader->packed_mask & (1UL << stage))
				memset(device.draw.attributes[stage], 0, sizeof(device.draw.attributes[stage]));
		}
	}
	device.draw.query_slot = 0;
	return TRUE;
}

/* the record drawn by the backend, where GL drew (glDrawArrays,
glDrawElementsBaseVertex): counted as a draw when it was, else as not drawn
(the backend logged why once) */
static void draw_submit(BOOL immediate)
{
	if (d3d11_backend_draw(&device.draw))
	{
		if (immediate)
			stats.immediate_draws++;
		else
			stats.draws++;
	}
	else
	{
		stats.skipped_not_drawn++;
		backend_lost_check();
	}
}

/* ---------- tracing (debug.gpu_trace_frame): d3d8_gl.c's trace_draw */

static void trace_draw(const char *kind, D3DPRIMITIVETYPE type, unsigned long count, const float *first_vertex)
{
	struct vertex_shader_object *program = current_program();
	DWORD *rs = D3D__RenderState;

	if (!trace_frame())
		return;
	platform_log("%s type %d count %lu vs %lu (decl %lu) vp %lu,%lu %lux%lu z%.2f-%.2f zen %lu zw %lu zf %lx blend %lu %lx/%lx cull %lx cw %08lx tm %05lx cc %lx fin %08lx/%08lx at %lu/%lx",
		kind, type, count, program ? program->id : 0, device.vertex_shader ? device.vertex_shader->id : 0,
		device.viewport.X, device.viewport.Y, device.viewport.Width, device.viewport.Height,
		device.viewport.MinZ, device.viewport.MaxZ, rs[D3DRS_ZENABLE], rs[D3DRS_ZWRITEENABLE], rs[D3DRS_ZFUNC],
		rs[D3DRS_ALPHABLENDENABLE], rs[D3DRS_SRCBLEND], rs[D3DRS_DESTBLEND], rs[D3DRS_CULLMODE],
		rs[D3DRS_COLORWRITEENABLE], rs[D3DRS_PSTEXTUREMODES], rs[D3DRS_PSCOMBINERCOUNT],
		rs[D3DRS_PSFINALCOMBINERINPUTSABCD], rs[D3DRS_PSFINALCOMBINERINPUTSEFG],
		rs[D3DRS_ALPHATESTENABLE], rs[D3DRS_ALPHAFUNC]);
	{
		int stage;

		for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
		{
			D3DBaseTexture *texture = device.textures[stage];
			struct xgpu_texture_description description;

			if (!texture || !((D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f))
				continue;
			xgpu_texture_describe(texture->Format, texture->Size, &description);
			platform_log("    t%d: data %08lx format %08lx size %08lx -> fmt %02lx %lux%lux%lu levels %lu linear %d cube %d rt %d min %lu mip %lu bias %g maxmip %lu",
				stage, texture->Data, texture->Format, texture->Size, description.format, description.width,
				description.height, description.depth, description.levels, description.linear, description.cube_map,
				render_target_find(texture->Data) != NULL, D3D__TextureState[stage][D3DTSS_MINFILTER],
				D3D__TextureState[stage][D3DTSS_MIPFILTER], dword_to_float(D3D__TextureState[stage][D3DTSS_MIPMAPLODBIAS]),
				D3D__TextureState[stage][D3DTSS_MAXMIPLEVEL]);
		}
	}
	platform_log("    offset enable %lu slope %g offset %g zbias %ld stencil %lu func %lx ref %lx mask %lx write %lx ops %lx/%lx/%lx",
		rs[D3DRS_SOLIDOFFSETENABLE], dword_to_float(rs[D3DRS_POLYGONOFFSETZSLOPESCALE]),
		dword_to_float(rs[D3DRS_POLYGONOFFSETZOFFSET]), (long)rs[D3DRS_ZBIAS], rs[D3DRS_STENCILENABLE],
		rs[D3DRS_STENCILFUNC], rs[D3DRS_STENCILREF], rs[D3DRS_STENCILMASK], rs[D3DRS_STENCILWRITEMASK],
		rs[D3DRS_STENCILFAIL], rs[D3DRS_STENCILZFAIL], rs[D3DRS_STENCILPASS]);
	if (config_boolean("debug.gpu_trace_constants"))
	{
		int constant;

		for (constant = 0; constant < XGPU_VERTEX_CONSTANT_COUNT; constant++)
		{
			const float *value = device.constants[constant];

			if (value[0] || value[1] || value[2] || value[3])
				platform_log("    c[%d] = %g %g %g %g", constant, value[0], value[1], value[2], value[3]);
		}
	}
	if (device.vertex_shader)
	{
		unsigned long index;

		for (index = 0; index < device.vertex_shader->element_count; index++)
		{
			const struct vertex_element *element = &device.vertex_shader->elements[index];

			platform_log("    decl v%lu: stream %lu offset %lu type %02lx", (unsigned long)element->reg,
				(unsigned long)element->stream, (unsigned long)element->offset, (unsigned long)element->type);
		}
		for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
		{
			const float *value = device.attributes[index];

			if (value[0] || value[1] || value[2] || value[3] != 1.0f)
				platform_log("    current v%lu = %g %g %g %g", index, value[0], value[1], value[2], value[3]);
		}
	}
	if (first_vertex)
	{
		int reg;

		for (reg = 0; reg < XGPU_VERTEX_ATTRIBUTE_COUNT; reg++)
		{
			const float *v = first_vertex + reg * 4;

			if (v[0] || v[1] || v[2] || v[3] != 1.0f)
				platform_log("    v%d = %g %g %g %g", reg, v[0], v[1], v[2], v[3]);
		}
	}
}

/* the dump's line, every 600 frames when more came */
static void dump_say(void)
{
	if (!debug_settings.dump_shaders || device.frame % 600 ||
		(shaders_met.vertex_dumped == shaders_met.vertex_dumped_said &&
		shaders_met.pixel_dumped == shaders_met.pixel_dumped_said))
	{
		return;
	}
	shaders_met.vertex_dumped_said = shaders_met.vertex_dumped;
	shaders_met.pixel_dumped_said = shaders_met.pixel_dumped;
	platform_log("shader dump: %lu vertex shaders and %lu pixel shaders written", shaders_met.vertex_dumped,
		shaders_met.pixel_dumped);
}

/* ---------- the generator self-test (debug.d3d11_self_test, third line)

Synthetic shaders for what the game's corpus lacks or has little of (fog,
colour sign, alpha kill on stages 1 to 3, the texture modes and sampler
types, the point threshold, the combiners' mappings, the alpha tests, the
multisampled alpha test's SV_Coverage at ps_4_1, the lit pair with one and
two lights, every mask bit of a vertex shader, every operation), each
generated and compiled by the backend (d3d11_backend_shader_compiles). */

struct self_test
{
	int count, failed;
	char failures[256];
};

static void self_test_result(struct self_test *test, const char *name, BOOL ok)
{
	test->count++;
	if (ok)
		return;
	test->failed++;
	if (strlen(test->failures) + strlen(name) + 3 < sizeof(test->failures))
	{
		if (*test->failures)
			strcat(test->failures, ", ");
		strcat(test->failures, name);
	}
}

static void self_test_pixel(struct self_test *test, const struct nv2a_pixel_shader_key *key, const char *name,
	const char *wanted_target)
{
	char target[8];
	char *text = nv2a_hlsl_pixel_shader(key, HLSL_BY_CONSTANT_ALPHA_NONE, target);
	BOOL ok = text && !strcmp(target, wanted_target) &&
		d3d11_backend_shader_compiles(text, strlen(text), name, HLSL_ENTRY, target);

	if (text && strcmp(target, wanted_target))
		platform_log("d3d11: shader self-test: %s is for %s, not %s", name, target, wanted_target);
	free(text);
	self_test_result(test, name, ok);
}

static void self_test_vertex(struct self_test *test, const uint32_t *instructions, uint32_t count, uint32_t packed,
	uint32_t integer, uint32_t three, const struct nv2a_vertex_lighting *lighting, const char *name)
{
	char *text = nv2a_hlsl_vertex_shader(instructions, count, packed, integer, three, lighting);
	BOOL ok = text && d3d11_backend_shader_compiles(text, strlen(text), name, HLSL_ENTRY, HLSL_VERTEX_TARGET);

	free(text);
	self_test_result(test, name, ok);
}

/* a texture mode for each stage (nv2a_psh.c's _mode_*) */
static DWORD self_test_modes(int stage0, int stage1, int stage2, int stage3)
{
	return (DWORD)(stage0 | stage1 << 5 | stage2 << 10 | stage3 << 15);
}

/* a key that reads every register in its one combiner stage and its final
combiner: t0 to t3, v0, v1, fog, r0, r1, c0, c1 */
static void self_test_key(struct nv2a_pixel_shader_key *key, DWORD modes, int sampler0, int sampler1, int sampler2,
	int sampler3)
{
	DWORD *state = key->combiner_state;

	memset(key, 0, sizeof(*key));
	key->texture_modes = modes;
	key->sampler_type[0] = (unsigned char)sampler0;
	key->sampler_type[1] = (unsigned char)sampler1;
	key->sampler_type[2] = (unsigned char)sampler2;
	key->sampler_type[3] = (unsigned char)sampler3;
	state[D3DRS_PSCOMBINERCOUNT] = 1;
	/* stage 0: A t0, B t1, C t2, D t3 (rgb); A v0, B fog, C c0, D r0
	(alpha); AB to r0, CD to r1, sum to v1 */
	state[D3DRS_PSRGBINPUTS0] = 0x08090a0b;
	state[D3DRS_PSALPHAINPUTS0] = 0x1413111c;
	state[D3DRS_PSRGBOUTPUTS0] = 0x5cd;
	state[D3DRS_PSALPHAOUTPUTS0] = 0x5cd;
	/* the final combiner: A r0, B v1, C c1, D the EF product; E t0, F t1, G
	r1's alpha */
	state[D3DRS_PSFINALCOMBINERINPUTSABCD] = 0x0c05020f;
	state[D3DRS_PSFINALCOMBINERINPUTSEFG] = 0x08091d00;
	state[D3DRS_PSDOTMAPPING] = 0x321;
	state[D3DRS_PSINPUTTEXTURE] = (1 << 16) | (2 << 20);
}

/* a vertex program instruction (nv2a_vsh.c's fields): the operations, the
three operands by register file (1 temporary, 2 input, 3 constant) and
index, the input register and constant they share, the masks and the
destinations; every swizzle xyzw */
static void self_test_instruction(uint32_t *words, unsigned mac, unsigned ilu, unsigned a_mux, unsigned a_index,
	unsigned c_mux, unsigned c_index, unsigned input, unsigned constant, unsigned mac_mask, unsigned temporary,
	unsigned ilu_mask, unsigned output_mask, unsigned output_address, unsigned from_ilu, unsigned relative)
{
	/* (B is the constant, or temporary 3 when A is a constant) */
	unsigned b_mux = a_mux == 3 ? 1 : 3, b_index = 3;

	words[0] = 0;
	words[1] = ilu << 25 | mac << 21 | constant << 13 | input << 9 | 0x1b;
	words[2] = a_index << 28 | a_mux << 26 | 0x1bu << 17 | b_index << 13 | b_mux << 11 | 0x1bu << 2 |
		((c_index >> 2) & 3);
	words[3] = (c_index & 3) << 30 | c_mux << 28 | mac_mask << 24 | temporary << 20 | ilu_mask << 16 |
		output_mask << 12 | (output_mask ? 1u << 11 : 0) | output_address << 3 | from_ilu << 2 | relative << 1;
}

#define SELF_TEST_INSTRUCTIONS 48

/* a short program: every input added into r2, every MAC operation into r4 to
r11 and out to every output register, arl and a relative constant, every ILU
operation (rcc of r12.w, which keeps the clip position), one with both units
running, then the end */
static uint32_t self_test_program(uint32_t words[SELF_TEST_INSTRUCTIONS][4])
{
	static const unsigned macs[] = { 2, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
	static const unsigned outputs[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
	static const unsigned ilus[] = { 1, 2, 4, 5, 6, 7 };
	uint32_t count = 0;
	unsigned index;

	/* r2 += v0 ... v15 */
	for (index = 0; index < 16; index++)
		self_test_instruction(words[count++], 3, 0, 2, 0, 1, 2, index, 0, 0xf, 2, 0, 0, 0, 0, 0);
	/* oPos = r2 */
	self_test_instruction(words[count++], 1, 0, 1, 2, 1, 2, 0, 0, 0, 0, 0, 0xf, 0, 0, 0);
	/* mul, mad, dp3, dph, dp4, dst, min, max, slt, sge: r4 to r11 (r12:
	oPos), out to oD0 ... oT3 */
	for (index = 0; index < sizeof(macs) / sizeof(macs[0]); index++)
	{
		self_test_instruction(words[count++], macs[index], 0, 1, 2, 1, 2 + index % 3, 0, 5 + index, 0xf,
			4 + index % 8, 0, 0xf, outputs[index], 0, 0);
	}
	/* arl a0 = r2.x; r5 = c[a0 + 7] */
	self_test_instruction(words[count++], 13, 0, 1, 2, 1, 2, 0, 0, 0x8, 0, 0, 0, 0, 0, 0);
	self_test_instruction(words[count++], 1, 0, 3, 0, 1, 2, 0, 7, 0xf, 5, 0, 0, 0, 0, 1);
	/* rcc of r12.w (the screen conversion's), then mov, rcp, rsq, exp,
	log, lit of r4 into r6, out to oT0 from the ILU */
	self_test_instruction(words[count++], 0, 3, 1, 2, 1, 12, 0, 0, 0, 1, 0x1, 0, 0, 0, 0);
	for (index = 0; index < sizeof(ilus) / sizeof(ilus[0]); index++)
		self_test_instruction(words[count++], 0, ilus[index], 1, 2, 1, 4, 0, 0, 0, 6, 0xf, 0xf, 9, 1, 0);
	/* both units: mad into r7, rsq into r1 */
	self_test_instruction(words[count++], 4, 4, 1, 2, 1, 4, 0, 9, 0xf, 7, 0x8, 0xf, 10, 0, 0);
	/* the end */
	self_test_instruction(words[count++], 1, 0, 1, 7, 1, 7, 0, 0, 0, 0, 0, 0xf, 12, 0, 0);
	words[count - 1][3] |= 1;
	return count;
}

static void shader_self_test(char *line, size_t line_size)
{
	static const struct
	{
		DWORD function;
		const char *name;
	} alpha_tests[] =
	{
		{ D3DCMP_NEVER, "the alpha test never" }, { D3DCMP_LESS, "the alpha test less" },
		{ D3DCMP_EQUAL, "the alpha test equal" }, { D3DCMP_LESSEQUAL, "the alpha test less or equal" },
		{ D3DCMP_GREATER, "the alpha test greater" }, { D3DCMP_NOTEQUAL, "the alpha test not equal" },
		{ D3DCMP_GREATEREQUAL, "the alpha test greater or equal" }, { D3DCMP_ALWAYS, "the alpha test always" },
	};
	static const struct
	{
		int modes[4];
		int samplers[4];
		const char *name;
	} texture_cases[] =
	{
		/* project2d, bump environment, with luminance, dependent AR */
		{ { 0x01, 0x06, 0x07, 0x0f }, { 1, 1, 1, 1 }, "the bump and dependent modes" },
		/* dependent GB, clip plane, pass-through */
		{ { 0x01, 0x10, 0x05, 0x04 }, { 1, 1, 0, 0 }, "dependent GB, clip plane and pass-through" },
		/* dot product, dot ST, dot ZW */
		{ { 0x04, 0x11, 0x09, 0x0a }, { 0, 1, 1, 0 }, "dot ST and dot ZW" },
		/* dot reflect diffuse, then specular */
		{ { 0x01, 0x11, 0x0b, 0x0c }, { 1, 1, 3, 3 }, "dot reflect diffuse and specular" },
		{ { 0x01, 0x11, 0x11, 0x0d }, { 1, 1, 1, 2 }, "dot STR into a volume" },
		{ { 0x01, 0x11, 0x11, 0x0e }, { 1, 1, 1, 3 }, "dot STR into a cube" },
		{ { 0x01, 0x11, 0x11, 0x12 }, { 1, 1, 1, 3 }, "dot reflect specular constant" },
		/* a cube map, a volume (project3d), BRDF (the default) */
		{ { 0x03, 0x02, 0x08, 0x00 }, { 3, 2, 1, 0 }, "a cube map, a volume and BRDF" },
	};
	static const DWORD fog_modes[] = { D3DFOG_NONE, D3DFOG_EXP, D3DFOG_EXP2, D3DFOG_LINEAR };
	static const char *const fog_names[] = { "fog by the vertex", "fog exp", "fog exp2", "fog linear" };
	uint32_t program[SELF_TEST_INSTRUCTIONS][4];
	struct nv2a_pixel_shader_key key;
	struct nv2a_vertex_lighting lighting;
	struct self_test test;
	uint32_t count;
	int index, stage;

	memset(&test, 0, sizeof(test));
	/* fog, in each table mode, into the combiner's fog register */
	for (index = 0; index < 4; index++)
	{
		self_test_key(&key, self_test_modes(1, 1, 1, 1), 1, 1, 1, 1);
		key.fog_enable = 1;
		key.fog_table_mode = (unsigned char)fog_modes[index];
		self_test_pixel(&test, &key, fog_names[index], HLSL_PIXEL_TARGET);
	}
	/* colour sign and alpha kill on every stage */
	self_test_key(&key, self_test_modes(1, 1, 1, 1), 1, 1, 1, 1);
	for (stage = 0; stage < 4; stage++)
		key.color_sign[stage] = (unsigned char)(stage == 3 ? 0xf : 1 << stage);
	self_test_pixel(&test, &key, "colour sign", HLSL_PIXEL_TARGET);
	self_test_key(&key, self_test_modes(1, 1, 1, 1), 1, 1, 1, 1);
	for (stage = 0; stage < 4; stage++)
		key.alpha_kill[stage] = 1;
	self_test_pixel(&test, &key, "alpha kill on every stage", HLSL_PIXEL_TARGET);
	/* the texture modes, each sampler type */
	for (index = 0; index < (int)(sizeof(texture_cases) / sizeof(texture_cases[0])); index++)
	{
		self_test_key(&key, self_test_modes(texture_cases[index].modes[0], texture_cases[index].modes[1],
			texture_cases[index].modes[2], texture_cases[index].modes[3]), texture_cases[index].samplers[0],
			texture_cases[index].samplers[1], texture_cases[index].samplers[2], texture_cases[index].samplers[3]);
		self_test_pixel(&test, &key, texture_cases[index].name, HLSL_PIXEL_TARGET);
	}
	/* the high-res meters' coverage and point threshold */
	self_test_key(&key, self_test_modes(1, 0, 0, 0), 1, 0, 0, 0);
	key.coverage_alpha = 1;
	key.point_threshold = 1;
	self_test_pixel(&test, &key, "the point threshold", HLSL_PIXEL_TARGET);
	/* eight combiner stages: every input mapping, the dot products, the mux
	both ways, every output mapping, the blue into alpha, unique constants */
	self_test_key(&key, self_test_modes(1, 1, 1, 1), 1, 1, 1, 1);
	key.combiner_state[D3DRS_PSCOMBINERCOUNT] = 8 | 0x1000 | 0x10000;
	for (stage = 0; stage < 8; stage++)
	{
		DWORD mapping = (DWORD)(stage << 5);

		key.combiner_state[D3DRS_PSRGBINPUTS0 + stage] = (0x08 | mapping) << 24 | (0x19 | mapping) << 16 |
			(0x0c | mapping) << 8 | (0x01 | mapping);
		key.combiner_state[D3DRS_PSALPHAINPUTS0 + stage] = (0x1c | mapping) << 24 | (0x12 | mapping) << 16 |
			(0x14 | mapping) << 8 | (0x1d | mapping);
		key.combiner_state[D3DRS_PSRGBOUTPUTS0 + stage] = (DWORD)(((stage & 1 ? 0x83 : 0x44) | (stage % 7) << 3) << 12) |
			0xc0d;
		key.combiner_state[D3DRS_PSALPHAOUTPUTS0 + stage] = (DWORD)(((stage % 7) << 3 | 0x04) << 12) | 0xdc0;
	}
	key.combiner_state[D3DRS_PSFINALCOMBINERINPUTSEFG] = 0x08091de0;
	self_test_pixel(&test, &key, "the combiners", HLSL_PIXEL_TARGET);
	key.combiner_state[D3DRS_PSCOMBINERCOUNT] = 8 | 0x100;
	self_test_pixel(&test, &key, "the combiners' other mux", HLSL_PIXEL_TARGET);
	/* every alpha test, then multisampled: SV_Coverage, at ps_4_1, where it
	compares by order */
	for (index = 0; index < (int)(sizeof(alpha_tests) / sizeof(alpha_tests[0])); index++)
	{
		self_test_key(&key, self_test_modes(1, 1, 1, 1), 1, 1, 1, 1);
		key.alpha_test_function = alpha_tests[index].function;
		self_test_pixel(&test, &key, alpha_tests[index].name, HLSL_PIXEL_TARGET);
	}
	self_test_key(&key, self_test_modes(1, 1, 1, 1), 1, 1, 1, 1);
	key.alpha_test_function = D3DCMP_GREATEREQUAL;
	key.alpha_test_samples = 4;
	self_test_pixel(&test, &key, "SV_Coverage greater or equal", HLSL_PIXEL_COVERAGE_TARGET);
	key.alpha_test_function = D3DCMP_LESS;
	key.alpha_test_samples = 8;
	self_test_pixel(&test, &key, "SV_Coverage less", HLSL_PIXEL_COVERAGE_TARGET);
	key.alpha_test_function = D3DCMP_NOTEQUAL;
	self_test_pixel(&test, &key, "multisampled not equal", HLSL_PIXEL_TARGET);
	/* the lit pair, with one light and with two: the pixel shaders, then the
	vertex shaders from the program below */
	self_test_key(&key, self_test_modes(1, 1, 1, 1), 1, 1, 1, 1);
	key.per_pixel_lighting = 1;
	self_test_pixel(&test, &key, "a pixel shader lit by one light", HLSL_PIXEL_TARGET);
	key.per_pixel_lighting = 2;
	self_test_pixel(&test, &key, "a pixel shader lit by two", HLSL_PIXEL_TARGET);
	/* every mask bit of a vertex shader: all packed; all integers with three
	components; a mixture; none (immediate mode) */
	count = self_test_program(program);
	self_test_vertex(&test, &program[0][0], count, 0xffff, 0, 0, NULL, "a vertex shader of packed inputs");
	self_test_vertex(&test, &program[0][0], count, 0, 0xffff, 0xffff, NULL,
		"a vertex shader of integer inputs of three components");
	self_test_vertex(&test, &program[0][0], count, 0x00ff, 0xff00, 0xf0f0, NULL, "a vertex shader of mixed inputs");
	self_test_vertex(&test, &program[0][0], count, 0, 0, 0, NULL, "an immediate vertex shader");
	memset(&lighting, 0, sizeof(lighting));
	lighting.lights = 1;
	lighting.normal_instruction = 17;
	lighting.normal_register = 2;
	self_test_vertex(&test, &program[0][0], count, 0, 0, 0, &lighting, "a vertex shader lit by one light");
	lighting.lights = 2;
	lighting.position_instruction = 18;
	lighting.position_register = 4;
	self_test_vertex(&test, &program[0][0], count, 0, 0, 0, &lighting, "a vertex shader lit by two");
	if (test.failed)
	{
		snprintf(line, line_size, "d3d11: shader self-test: FAILED (%d of %d synthetic shaders: %s)", test.failed,
			test.count, test.failures);
	}
	else
	{
		snprintf(line, line_size, "d3d11: shader self-test: ok (%d synthetic shaders compile: fog in each mode, "
			"colour sign, alpha kill on every stage, every texture mode and sampler type, the point threshold, the "
			"combiners' mappings, every alpha test, the lit pair with one and two lights, every vertex mask bit and "
			"operation, SV_Coverage at ps_4_1)", test.count);
	}
}

/* ---------- the shader service's self-test (debug.d3d11_self_test, fourth
line)

The backend's cases (d3d11_backend_shaders_self_test: the cache, a failed
compile, a damaged file, a layout, a state object of each kind), then a
layout of every vertex type of the format table (attribute_format) over a
vertex shader the generator makes for it with its masks: sixteen types in one
declaration, the other three in a second with its thirteen other inputs from
the attribute slot (vs_4_0 has sixteen inputs). */

static void shader_service_self_test(char *line, size_t line_size)
{
	static const unsigned char types[] =
	{
		D3DVSDT_FLOAT1, D3DVSDT_FLOAT2, D3DVSDT_FLOAT3, D3DVSDT_FLOAT4, D3DVSDT_FLOAT2H, D3DVSDT_D3DCOLOR,
		D3DVSDT_SHORT1, D3DVSDT_SHORT2, D3DVSDT_SHORT3, D3DVSDT_SHORT4, D3DVSDT_NORMSHORT1, D3DVSDT_NORMSHORT2,
		D3DVSDT_NORMSHORT3, D3DVSDT_NORMSHORT4, D3DVSDT_PBYTE1, D3DVSDT_PBYTE2, D3DVSDT_PBYTE3, D3DVSDT_PBYTE4,
		D3DVSDT_NORMPACKED3,
	};
	uint32_t program[SELF_TEST_INSTRUCTIONS][4];
	uint32_t instruction_count, before, after, statistics[D3D11_BACKEND_STATISTICS];
	char backend[512];
	const char *failed = NULL;
	unsigned long first;
	int part = 0;
	size_t length;

	if (!d3d11_backend_shaders_self_test(backend, sizeof(backend)))
	{
		snprintf(line, line_size, "%s", backend);
		return;
	}
	instruction_count = self_test_program(program);
	for (first = 0; !failed && first < sizeof(types); first += XGPU_VERTEX_ATTRIBUTE_COUNT, part++)
	{
		struct vertex_shader_object declaration;
		struct d3d11_backend_layout_element elements[XGPU_VERTEX_ATTRIBUTE_COUNT];
		unsigned char element_types[XGPU_VERTEX_ATTRIBUTE_COUNT];
		uint32_t count, vertex, layout;
		unsigned long index;
		char *text;

		memset(&declaration, 0, sizeof(declaration));
		for (index = first; index < sizeof(types) && index < first + XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
		{
			struct vertex_element *element = &declaration.elements[declaration.element_count++];

			element->reg = (unsigned char)(index - first);
			element->stream = (unsigned char)(index % 2);
			element->type = types[index];
			element->bytes = (unsigned char)vertex_type_bytes(element->type);
			/* (each at a multiple of 16: the alignment is not what is tried) */
			element->offset = (unsigned short)(element->reg / 2 * 16);
			element_masks(element, &declaration.packed_mask, &declaration.integer_mask,
				&declaration.three_component_mask);
		}
		text = nv2a_hlsl_vertex_shader(&program[0][0], instruction_count, (uint32_t)declaration.packed_mask,
			(uint32_t)declaration.integer_mask, (uint32_t)declaration.three_component_mask, NULL);
		if (!text)
		{
			failed = "a vertex shader of every type was not generated";
			break;
		}
		{
			uint64_t hash = shader_hash(SHADER_HASH_START, text, strlen(text));

			vertex = d3d11_backend_shader_find(D3D11_BACKEND_STAGE_VERTEX, hash);
			if (!vertex)
				vertex = d3d11_backend_shader_make(D3D11_BACKEND_STAGE_VERTEX, hash, text, strlen(text), HLSL_ENTRY,
					HLSL_VERTEX_TARGET);
		}
		free(text);
		count = layout_elements(&declaration, 0x3, elements, element_types);
		d3d11_backend_shaders_statistics(statistics);
		before = statistics[D3D11_BACKEND_STATISTIC_UNFED_LAYOUTS];
		layout = vertex ? d3d11_backend_layout_make(elements, count, vertex) : 0;
		d3d11_backend_shaders_statistics(statistics);
		after = statistics[D3D11_BACKEND_STATISTIC_UNFED_LAYOUTS];
		if (!vertex)
			failed = part ? "the second vertex shader of every type did not compile" :
				"the first vertex shader of every type did not compile";
		else if (!layout)
			failed = part ? "the second layout of every type was not made" : "the first layout of every type was not made";
		else if (after != before + (part ? 1 : 0))
			failed = "the second layout's inputs from the attribute slot were not counted";
	}
	if (failed)
	{
		snprintf(line, line_size, "d3d11: shader service self-test: FAILED (%s)", failed);
		return;
	}
	/* (the backend's line, its parenthesis closed after this part) */
	snprintf(line, line_size, "%s", backend);
	length = strlen(line);
	if (length && line[length - 1] == ')')
		line[--length] = 0;
	snprintf(line + length, line_size - length, "; layouts of all %u vertex types of the format table over generated "
		"vertex shaders, in two streams, the second with %u inputs from the attribute slot)", (unsigned)sizeof(types),
		(unsigned)(XGPU_VERTEX_ATTRIBUTE_COUNT - (sizeof(types) - XGPU_VERTEX_ATTRIBUTE_COUNT)));
}

/* prepare_draw's guards (d3d8_gl.c): a draw with no program, no vertex
shader or no instructions is skipped, as is one debug.gpu_skip_vertex_shaders
names; and here, a draw with no rings to copy its data into. The rest of
prepare_draw is draw_prepare's. */
static BOOL draw_ready(void)
{
	struct vertex_shader_object *program = current_program();

	if (!device.backend_ready || !program || !device.vertex_shader || !program->instructions)
	{
		stats.skipped_no_program++;
		return FALSE;
	}
	{
		const char *skip = debug_settings.skip_vertex_shaders;

		while (skip && *skip)
		{
			if ((unsigned long)atol(skip) == program->id)
				return FALSE;
			skip = strchr(skip, ',');
			if (skip)
				skip++;
		}
	}
	if (!device.buffers_ready)
	{
		stats.skipped_no_buffer++;
		return FALSE;
	}
	return TRUE;
}

void WINAPI D3DDevice_SetStreamSource(UINT stream_number, D3DVertexBuffer *stream_data, UINT stride)
{
	if (stream_number >= 16)
		return;
	device.streams[stream_number].data = stream_data ? stream_data->Data : 0;
	device.streams[stream_number].stride = stride;
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *index_data, UINT base_vertex_index)
{
	device.base_vertex_index = base_vertex_index;
	/* an index buffer's Data is a window address, not an offset within one:
	from a map file it is the address the window was linked at, and from
	CreateIndexBuffer ordinary memory, which the move leaves alone */
	D3D__IndexData = index_data ? (WORD *)PORT_WINDOW_REBASE(index_data->Data) : NULL;
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive_type, UINT start_vertex, UINT vertex_count)
{
	if (!vertex_count || !draw_ready() || !draw_prepare(FALSE))
		return;
	trace_draw("draw", primitive_type, vertex_count, NULL);
	stream_data(start_vertex, vertex_count);
	device.draw.topology = primitive_topology(primitive_type);
	if (primitive_made(primitive_type))
	{
		unsigned long count;
		WORD *indices = made_indices(primitive_type, NULL, vertex_count, &count);

		draw_indices_at(D3D11_BACKEND_DATA_RING, 0, index_upload(indices, count * sizeof(WORD)), count, 0);
		free(indices);
	}
	else
	{
		draw_indices_none(vertex_count);
	}
	draw_submit(FALSE);
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type, UINT vertex_count, CONST WORD *index_data)
{
	unsigned long minimum, maximum, count, generation = 0, index_offset = 0;
	WORD *indices = NULL;
	const WORD *source = index_data;
	uint32_t index_segment = 0;
	BOOL mirrored;

	if (!vertex_count || !index_data || !draw_ready() || !draw_prepare(FALSE))
		return;
	/* quads, fans, polygons and loops are drawn from indices made for the
	draw */
	mirrored = !primitive_made(primitive_type) &&
		mirror_range((unsigned long)index_data, vertex_count * sizeof(WORD), &index_segment, &index_offset, &generation);
	index_extent(index_data, vertex_count, generation, mirrored, &minimum, &maximum);
	trace_draw("indexed", primitive_type, vertex_count, NULL);
	/* (the streams from the base vertex on: index i is vertex base + i) */
	stream_data(device.base_vertex_index + minimum, maximum - minimum + 1);
	device.draw.topology = primitive_topology(primitive_type);
	if (mirrored)
	{
		/* the attributes start at vertex minimum */
		draw_indices_at(D3D11_BACKEND_DATA_SEGMENT, index_segment, index_offset, vertex_count, -(long)minimum);
		draw_submit(FALSE);
		return;
	}
	stats.streamed_bytes += vertex_count * sizeof(WORD);
	count = vertex_count;
	if (primitive_made(primitive_type))
	{
		indices = made_indices(primitive_type, index_data, vertex_count, &count);
		source = indices;
	}
	draw_indices_at(D3D11_BACKEND_DATA_RING, 0, index_upload(source, count * sizeof(WORD)), count, -(long)minimum);
	free(indices);
	draw_submit(FALSE);
}

/* ---------- immediate mode */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type)
{
	device.immediate_active = TRUE;
	device.immediate_type = primitive_type;
	device.immediate_count = 0;
}

static void immediate_emit(void)
{
	unsigned long floats = XGPU_VERTEX_ATTRIBUTE_COUNT * 4;

	if (device.immediate_count == device.immediate_capacity)
	{
		device.immediate_capacity = device.immediate_capacity ? device.immediate_capacity * 2 : 256;
		device.immediate_vertices = realloc(device.immediate_vertices,
			device.immediate_capacity * floats * sizeof(float));
	}
	memcpy(device.immediate_vertices + device.immediate_count * floats, device.attributes, floats * sizeof(float));
	device.immediate_count++;
}

void WINAPI D3DDevice_End(void)
{
	unsigned long stride = XGPU_VERTEX_ATTRIBUTE_COUNT * 4 * sizeof(float);
	unsigned long offset, count = device.immediate_count;
	D3DPRIMITIVETYPE type = device.immediate_type;

	device.immediate_active = FALSE;
	if (!count || !draw_ready() || !draw_prepare(TRUE))
		return;
	trace_draw("immediate", type, count, device.immediate_vertices);
	/* the vertices streamed: every attribute four floats, one after another,
	in slot 0 at the ring's offset (the variant's immediate layout) */
	offset = stream_upload(device.immediate_vertices, count * stride);
	memset(device.draw.streams, 0, sizeof(device.draw.streams));
	device.draw.streams[0].kind = D3D11_BACKEND_DATA_RING;
	device.draw.streams[0].offset = (uint32_t)offset;
	device.draw.streams[0].stride = (uint32_t)stride;
	device.draw.topology = primitive_topology(type);
	if (primitive_made(type))
	{
		unsigned long index_count;
		WORD *indices = made_indices(type, NULL, count, &index_count);

		draw_indices_at(D3D11_BACKEND_DATA_RING, 0, index_upload(indices, index_count * sizeof(WORD)), index_count, 0);
		free(indices);
	}
	else
	{
		draw_indices_none(count);
	}
	draw_submit(TRUE);
}

static void set_attribute(INT reg, float a, float b, float c, float d)
{
	BOOL emit = FALSE;

	if (reg == D3DVSDE_VERTEX)
	{
		reg = 0;
		emit = TRUE;
	}
	if (reg < 0 || reg >= XGPU_VERTEX_ATTRIBUTE_COUNT)
		return;
	device.attributes[reg][0] = a;
	device.attributes[reg][1] = b;
	device.attributes[reg][2] = c;
	device.attributes[reg][3] = d;
	/* like the hardware, writing register 0 completes a vertex */
	if (device.immediate_active && (emit || reg == 0))
		immediate_emit();
}

void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b)
{
	set_attribute(reg, a, b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d)
{
	set_attribute(reg, a, b, c, d);
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	set_attribute(reg, (float)a, (float)b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	set_attribute(reg, a / 255.0f, b / 255.0f, c / 255.0f, d / 255.0f);
}

void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	float value[4];

	color_to_vec4(color, value);
	set_attribute(reg, value[0], value[1], value[2], value[3]);
}

/* ---------- clearing */

/* the most rectangles one clear passes to the backend at a time (the game
gives one or none) */
#define CLEAR_RECTANGLES 16

static BOOL first_drawn_clear_logged;

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	float rgba[4];
	int32_t pixels[CLEAR_RECTANGLES][4];
	uint32_t channels = 0, made = 0, result;
	BOOL has_depth = FALSE;
	DWORD index;

	if (trace_frame())
		platform_log("clear flags %lx color %08lx z %g count %lu target %08lx depth %08lx", (unsigned long)flags,
			(unsigned long)color, z, (unsigned long)count,
			device.render_target ? (unsigned long)device.render_target->Data : 0,
			device.depth_stencil ? (unsigned long)device.depth_stencil->Data : 0);
	stats.clears++;
	if (!device.backend_ready || !bind_targets(&has_depth))
		return;
	color_to_vec4(color, rgba);
	/* the Xbox clears the channels named (D3DCLEAR_TARGET_R, _G, _B, _A):
	the fog screen clears only alpha, leaving the picture under the fog */
	if (flags & D3DCLEAR_TARGET_R)
		channels |= D3D11_BACKEND_CLEAR_R;
	if (flags & D3DCLEAR_TARGET_G)
		channels |= D3D11_BACKEND_CLEAR_G;
	if (flags & D3DCLEAR_TARGET_B)
		channels |= D3D11_BACKEND_CLEAR_B;
	if (flags & D3DCLEAR_TARGET_A)
		channels |= D3D11_BACKEND_CLEAR_A;
	if (has_depth && (flags & D3DCLEAR_ZBUFFER))
		channels |= D3D11_BACKEND_CLEAR_DEPTH;
	if (has_depth && (flags & D3DCLEAR_STENCIL))
		channels |= D3D11_BACKEND_CLEAR_STENCIL;
	if (!channels)
		return;
	/* the rectangles in the targets' pixels, row 0 at the top (Direct3D's
	way up, so no flip) */
	if (!count || !rectangles)
	{
		/* the NV2A clips a viewport-less clear to the viewport, which is what
		keeps a split-screen window's clear from wiping the other window */
		GLint x0 = target_pixel((float)device.viewport.X, 0);
		GLint y0 = target_pixel((float)device.viewport.Y, 1);

		pixels[0][0] = x0;
		pixels[0][1] = y0;
		pixels[0][2] = target_pixel((float)(device.viewport.X + device.viewport.Width), 0) - x0;
		pixels[0][3] = target_pixel((float)(device.viewport.Y + device.viewport.Height), 1) - y0;
		made = 1;
	}
	for (index = 0; rectangles && index < count; index++)
	{
		INT left = rectangles[index].x1 > device.viewport.X ? rectangles[index].x1 : device.viewport.X;
		INT top = rectangles[index].y1 > device.viewport.Y ? rectangles[index].y1 : device.viewport.Y;
		INT right = rectangles[index].x2 < device.viewport.X + device.viewport.Width ?
			rectangles[index].x2 : device.viewport.X + device.viewport.Width;
		INT bottom = rectangles[index].y2 < device.viewport.Y + device.viewport.Height ?
			rectangles[index].y2 : device.viewport.Y + device.viewport.Height;
		GLint x0, y0;

		if (left >= right || top >= bottom)
			continue;
		x0 = target_pixel((float)(left + UI_OFFSET), 0);
		y0 = target_pixel((float)top, 1);
		pixels[made][0] = x0;
		pixels[made][1] = y0;
		pixels[made][2] = target_pixel((float)(right + UI_OFFSET), 0) - x0;
		pixels[made][3] = target_pixel((float)bottom, 1) - y0;
		/* (a clear of more rectangles than that goes in parts) */
		if (++made == CLEAR_RECTANGLES)
		{
			result = d3d11_backend_clear(channels, rgba, z, stencil, &pixels[0][0], made);
			stats.clears_by_calls += result == 1;
			stats.clears_drawn += result == 2;
			made = 0;
		}
	}
	if (!made)
		return;
	result = d3d11_backend_clear(channels, rgba, z, stencil, &pixels[0][0], made);
	stats.clears_by_calls += result == 1;
	stats.clears_drawn += result == 2;
	/* (the first clear that is drawn, said once: the statistics count them a
	frame, and one now and then is lost in the average) */
	if (result == 2 && !first_drawn_clear_logged)
	{
		first_drawn_clear_logged = TRUE;
		platform_log("Direct3D 11: the first clear drawn, at frame %lu: flags %lx, colour %08lx, %lu rectangles, the "
			"first %ldx%ld at %ld,%ld", device.frame, (unsigned long)flags, (unsigned long)color, (unsigned long)made,
			(long)pixels[0][2], (long)pixels[0][3], (long)pixels[0][0], (long)pixels[0][1]);
	}
}

/* ---------- the anti-aliasing passes */

/* display.anti_aliasing's pass over a window's 3D view, before the HUD and
menus are drawn over it (source/render/render.c): phase 6's, with nothing
drawn yet */
void halo_screen_anti_alias(short x0, short y0, short x1, short y1)
{
	(void)x0;
	(void)y0;
	(void)x1;
	(void)y1;
}

/* ---------- presentation */

static void write_screenshot(struct render_target_entry *target)
{
	const char *directory = *config_string("debug.screenshot_directory") ?
		config_string("debug.screenshot_directory") : NULL;
	unsigned long width = target->target.gl_width, height = target->target.gl_height;
	unsigned char *pixels;
	char path[512];
	FILE *file;
	unsigned long row;
	unsigned char header[54] = { 'B', 'M' };
	unsigned long image_size = width * height * 4;

	if (!directory)
		return;
	pixels = malloc(image_size);
	/* (BGRA rows from the top, as the BMP below takes them) */
	if (!pixels || !d3d11_backend_target_read(target->backend_target, pixels, image_size))
	{
		free(pixels);
		return;
	}
	/* the display ignores destination alpha, which the game uses as scratch;
	image viewers would show it as transparency */
	for (row = 0; row < width * height; row++)
	{
		pixels[row * 4 + 3] = 0xff;
	}
	snprintf(path, sizeof(path), "%s/frame%05lu.bmp", directory, device.frame);
	file = fopen(path, "wb");
	if (file)
	{
		*(unsigned int *)(header + 2) = (unsigned int)(54 + image_size);
		*(unsigned int *)(header + 10) = 54;
		*(unsigned int *)(header + 14) = 40;
		*(int *)(header + 18) = (int)width;
		*(int *)(header + 22) = -(int)height; /* rows from the top, as read */
		*(unsigned short *)(header + 26) = 1;
		*(unsigned short *)(header + 28) = 32;
		*(unsigned int *)(header + 34) = (unsigned int)image_size;
		fwrite(header, 1, sizeof(header), file);
		for (row = 0; row < height; row++)
			fwrite(pixels + row * width * 4, 1, width * 4, file);
		fclose(file);
	}
	free(pixels);
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle,
	void *unused, void *unused2)
{
	static long screenshot_every = -1;
	int presented = 0;

	(void)source_rectangle;
	(void)destination_rectangle;
	(void)unused;
	(void)unused2;
	if (screenshot_every < 0)
		screenshot_every = config_integer("debug.screenshot_every");

	/* display.vsync, the present's sync interval, and the marker, read again
	when a setting changes (as frame_interval_ns reads them) */
	if (device.backend_ready && device.settings_read_at != config_changes())
	{
		device.settings_read_at = config_changes();
		device.vsync = config_boolean("display.vsync");
		d3d11_backend_present_marker(config_boolean("debug.d3d11_present_marker"));
	}
	/* the back buffer, drawn letterboxed into the window and presented; its
	target is made now if nothing was cleared into it yet, so a frame with no
	clear presents black */
	if (device.backend_ready)
	{
		struct render_target_entry *back_buffer = render_target_get(&device.back_buffer);
		int window_width = 0, window_height = 0;

		if (back_buffer && back_buffer->backend_target)
		{
			if (trace_frame())
				platform_log("present back buffer %08lx target %lu", (unsigned long)device.back_buffer.Data,
					(unsigned long)back_buffer->backend_target);
			if (screenshot_every > 0 && device.frame % (unsigned long)screenshot_every == 0)
				write_screenshot(back_buffer);
			platform_video_drawable_size(&window_width, &window_height);
			presented = d3d11_backend_present(back_buffer->backend_target,
				(uint32_t)(window_width > 0 ? window_width : 0), (uint32_t)(window_height > 0 ? window_height : 0),
				device.vsync ? 1 : 0);
			xgpu_d3d11_texture_cache_begin_frame();
		}
		stats.debug_messages += d3d11_backend_drain_debug();
		backend_lost_check();
	}
	if (presented == 1)
		stats.presented++;
	else
		stats.not_presented++;
	/* with vsync, a presented frame was paced by the present's interval;
	every other frame is paced by the platform layer (display.max_fps, or the
	display's refresh with vsync) */
	if (device.window_ready && (presented != 1 || !device.vsync))
		platform_video_swap();
	device.frame++;
	stats.presents++;
	if (debug_settings.statistics && device.frame % 60 == 0)
	{
		/* (draws, clears and target changes a frame, as d3d8_gl.c's line has
		them, and its skipped draws over the 60 frames; the targets alive; the
		KB mirrored and streamed a frame, as d3d8_gl.c's line has them, the KB
		written into segments a frame, the segments alive and the rings'
		discards since the last line; the pixel shader keys and vertex shader
		variants met since the start; the presents and resizes since the last
		line; the stub calls since the start, which phase 6 brings to 0; the
		layer's messages since the last line, when debug.d3d11_debug asks for
		the layer) */
		char layer[64] = "";
		uint32_t resizes = d3d11_backend_resizes(), discards = d3d11_backend_ring_discards();
		uint32_t made[D3D11_BACKEND_STATISTICS], drawn[D3D11_BACKEND_DRAW_STATISTICS];
		int index;

		if (config_boolean("debug.d3d11_debug"))
			snprintf(layer, sizeof(layer), "; %lu debug layer messages", stats.debug_messages);
		d3d11_backend_draw_statistics(drawn);
		/* (the shader service's totals since the self-tests: shaders read
		from the cache and compiled, with the time each took, failed; layouts,
		those with an input the shader reads from the attribute slot; state
		objects; and the draws skipped since the start for a shader or a
		layout) */
		d3d11_backend_shaders_statistics(made);
		for (index = 0; index < D3D11_BACKEND_STATISTICS; index++)
			made[index] -= device.shader_statistics_before[index];
		platform_log("frame %lu: shaders %lu cached in %.1f ms, %lu compiled in %.1f ms, %lu failed; %lu layouts (%lu "
			"with inputs from the attribute slot), %lu/%lu/%lu/%lu blend/depth-stencil/rasterizer/sampler states; skipped "
			"since the start: %lu no shader, %lu no layout", device.frame,
			(unsigned long)made[D3D11_BACKEND_STATISTIC_CACHED],
			made[D3D11_BACKEND_STATISTIC_CACHED_MICROSECONDS] / 1000.0,
			(unsigned long)made[D3D11_BACKEND_STATISTIC_COMPILED],
			made[D3D11_BACKEND_STATISTIC_COMPILE_MICROSECONDS] / 1000.0,
			(unsigned long)made[D3D11_BACKEND_STATISTIC_FAILED], (unsigned long)made[D3D11_BACKEND_STATISTIC_LAYOUTS],
			(unsigned long)made[D3D11_BACKEND_STATISTIC_UNFED_LAYOUTS], (unsigned long)made[D3D11_BACKEND_STATISTIC_BLEND],
			(unsigned long)made[D3D11_BACKEND_STATISTIC_DEPTH_STENCIL],
			(unsigned long)made[D3D11_BACKEND_STATISTIC_RASTERIZER], (unsigned long)made[D3D11_BACKEND_STATISTIC_SAMPLERS],
			shaders_met.skipped_no_shader, shaders_met.skipped_no_layout);
		platform_log("frame %lu: Direct3D 11; %lu draws, %lu immediate; skipped in 60 frames: %lu no "
			"program, %lu no buffer, %lu no target, %lu not drawn; %lu clears (%lu by clear calls, %lu drawn), "
			"%lu target changes, %lu targets; %lu KB mirrored, %lu KB streamed, %lu KB uploaded to %lu segments, %lu ring "
			"discards; %lu binds skipped, %lu self-sampling copies; %lu textures (%lu KB), %lu texture writes (%lu KB); "
			"%lu pixel keys, %lu vertex variants met; %lu presented, "
			"%lu not; %lu swap chain resizes; %lu GL stub calls%s",
			device.frame, stats.draws / stats.presents, stats.immediate_draws / stats.presents,
			stats.skipped_no_program, stats.skipped_no_buffer, stats.skipped_no_target,
			stats.skipped_not_drawn,
			stats.clears / stats.presents, stats.clears_by_calls / stats.presents, stats.clears_drawn / stats.presents,
			stats.target_changes / stats.presents, (unsigned long)d3d11_backend_targets_alive(),
			stats.mirrored_bytes / stats.presents / 1024, stats.streamed_bytes / stats.presents / 1024,
			stats.uploaded_bytes / stats.presents / 1024, (unsigned long)d3d11_backend_segments_alive(),
			(unsigned long)(discards - device.discards_before),
			(unsigned long)(drawn[D3D11_BACKEND_DRAW_STATISTIC_BINDS_SKIPPED] -
				device.draw_statistics_before[D3D11_BACKEND_DRAW_STATISTIC_BINDS_SKIPPED]),
			(unsigned long)(drawn[D3D11_BACKEND_DRAW_STATISTIC_SELF_SAMPLING_COPIES] -
				device.draw_statistics_before[D3D11_BACKEND_DRAW_STATISTIC_SELF_SAMPLING_COPIES]),
			(unsigned long)drawn[D3D11_BACKEND_DRAW_STATISTIC_TEXTURES_ALIVE],
			(unsigned long)drawn[D3D11_BACKEND_DRAW_STATISTIC_TEXTURE_BYTES] / 1024,
			(unsigned long)(drawn[D3D11_BACKEND_DRAW_STATISTIC_TEXTURE_WRITES] -
				device.draw_statistics_before[D3D11_BACKEND_DRAW_STATISTIC_TEXTURE_WRITES]),
			(unsigned long)(drawn[D3D11_BACKEND_DRAW_STATISTIC_TEXTURE_WRITE_BYTES] -
				device.draw_statistics_before[D3D11_BACKEND_DRAW_STATISTIC_TEXTURE_WRITE_BYTES]) / 1024,
			shaders_met.pixel_keys, shaders_met.vertex_variants,
			stats.presented,
			stats.not_presented, (unsigned long)(resizes - device.resizes_before),
			gl_stubs_calls() - device.gl_stub_calls_before, layer);
		device.resizes_before = resizes;
		device.discards_before = discards;
		memcpy(device.draw_statistics_before, drawn, sizeof(drawn));
		memset(&stats, 0, sizeof(stats));
	}
	dump_say();
	layouts_say();
	platform_pump_events();

	pthread_mutex_lock(&vertical_blank_lock);
	/* the Xbox keeps at most two frames queued behind its 60 Hz display;
	with interpolation, frames come at the real display's rate instead,
	paced by vsync (platform_video_swap) */
	if (halo_interpolation_enabled())
	{
		flip_count++;
	}
	else
	{
		while (pending_flips >= 2)
			pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
		pending_flips++;
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

HRESULT WINAPI D3DDevice_PersistDisplay(void)
{
	return S_OK;
}
