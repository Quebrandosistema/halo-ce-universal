/*
HLSL_SHADERS.H

What the Direct3D 11 renderer's HLSL generators (nv2a_vsh_hlsl.c,
nv2a_psh_hlsl.c, copies of port/linux/src/nv2a_vsh.c and nv2a_psh.c) and the
device that will feed their shaders agree on (port/windows/D3D11.md, phase
4): the constant buffers' registers, sizes and every member's place, the
interface between the two stages with its semantics, the generators'
version, and their prototypes.

Every member of a constant buffer is a float4 or an array of them, and has
its place written in the shader with packoffset, from the _REGISTER defines
below (the byte offset over 16, which the shader's text is made from without
arithmetic); the C structures the device fills are checked against the same
defines at compile time, so the shader, this header and the structures
cannot disagree without the build failing. tools/d3d11_shader_check.py reads
the defines and the strings below to say what each dumped shader must be.

<stdint.h> and <stddef.h> only, so that both halves of the renderer (the
device, against the Xbox SDK, and the backend, against the Windows SDK) can
include it; the key and the lighting are named by pointer only (xgpu.h).
*/

#ifndef HLSL_SHADERS_H
#define HLSL_SHADERS_H

#include <stddef.h>
#include <stdint.h>

/* in every shader's identity (phase 5's cache): a change to the generators'
text is a new version */
#define HLSL_SHADER_GENERATOR_VERSION 1

#define HLSL_STRING(x) #x
#define HLSL_REGISTER_TEXT(x) HLSL_STRING(x)

/* ---------- the vertex stage's constant buffers */

/* c[192], the program's constants (XGPU_VERTEX_CONSTANT_COUNT) */
#define HLSL_VERTEX_CONSTANTS_SLOT 0
#define HLSL_VERTEX_CONSTANT_COUNT 192
#define HLSL_VERTEX_CONSTANTS_C 0
#define HLSL_VERTEX_CONSTANTS_C_REGISTER 0
#define HLSL_VERTEX_CONSTANTS_SIZE 3072

/* the viewport's transform (Direct3D's c[-38] and c[-37]) and, in screen.x,
the columns the menus shift by (d3d8_gl.c's screen_offset) */
#define HLSL_VERTEX_PARAMETERS_SLOT 1
#define HLSL_VERTEX_PARAMETERS_VIEWPORT_SCALE 0
#define HLSL_VERTEX_PARAMETERS_VIEWPORT_SCALE_REGISTER 0
#define HLSL_VERTEX_PARAMETERS_VIEWPORT_OFFSET 16
#define HLSL_VERTEX_PARAMETERS_VIEWPORT_OFFSET_REGISTER 1
#define HLSL_VERTEX_PARAMETERS_SCREEN 32
#define HLSL_VERTEX_PARAMETERS_SCREEN_REGISTER 2
#define HLSL_VERTEX_PARAMETERS_SIZE 48

#define HLSL_VERTEX_CONSTANTS_TEXT \
	"cbuffer vertex_constants : register(b" HLSL_REGISTER_TEXT(HLSL_VERTEX_CONSTANTS_SLOT) ")\n" \
	"{\n" \
	"\tfloat4 c[" HLSL_REGISTER_TEXT(HLSL_VERTEX_CONSTANT_COUNT) "] : packoffset(c" \
		HLSL_REGISTER_TEXT(HLSL_VERTEX_CONSTANTS_C_REGISTER) ");\n" \
	"};\n" \
	"cbuffer vertex_parameters : register(b" HLSL_REGISTER_TEXT(HLSL_VERTEX_PARAMETERS_SLOT) ")\n" \
	"{\n" \
	"\tfloat4 viewport_scale : packoffset(c" HLSL_REGISTER_TEXT(HLSL_VERTEX_PARAMETERS_VIEWPORT_SCALE_REGISTER) ");\n" \
	"\tfloat4 viewport_offset : packoffset(c" HLSL_REGISTER_TEXT(HLSL_VERTEX_PARAMETERS_VIEWPORT_OFFSET_REGISTER) ");\n" \
	"\tfloat4 screen : packoffset(c" HLSL_REGISTER_TEXT(HLSL_VERTEX_PARAMETERS_SCREEN_REGISTER) ");\n" \
	"};\n"

struct hlsl_vertex_constants
{
	float c[HLSL_VERTEX_CONSTANT_COUNT][4];
};

struct hlsl_vertex_parameters
{
	float viewport_scale[4];
	float viewport_offset[4];
	/* x: the screen offset; y, z, w: 0 */
	float screen[4];
};

/* ---------- the pixel stage's constant buffers */

/* the combiners' constants, fog, the alpha test's reference (in x), the
bump environment and the linear textures' scale (xgpu.h XGPU_PIXEL_UNIFORMS,
in its order) */
#define HLSL_PIXEL_PARAMETERS_SLOT 0
#define HLSL_PIXEL_PARAMETERS_PS_C0 0
#define HLSL_PIXEL_PARAMETERS_PS_C0_REGISTER 0
#define HLSL_PIXEL_PARAMETERS_PS_C1 128
#define HLSL_PIXEL_PARAMETERS_PS_C1_REGISTER 8
#define HLSL_PIXEL_PARAMETERS_PS_FINAL_C0 256
#define HLSL_PIXEL_PARAMETERS_PS_FINAL_C0_REGISTER 16
#define HLSL_PIXEL_PARAMETERS_PS_FINAL_C1 272
#define HLSL_PIXEL_PARAMETERS_PS_FINAL_C1_REGISTER 17
#define HLSL_PIXEL_PARAMETERS_FOG_COLOR 288
#define HLSL_PIXEL_PARAMETERS_FOG_COLOR_REGISTER 18
#define HLSL_PIXEL_PARAMETERS_FOG_PARAMETERS 304
#define HLSL_PIXEL_PARAMETERS_FOG_PARAMETERS_REGISTER 19
#define HLSL_PIXEL_PARAMETERS_ALPHA_REFERENCE 320
#define HLSL_PIXEL_PARAMETERS_ALPHA_REFERENCE_REGISTER 20
#define HLSL_PIXEL_PARAMETERS_BUMP_MATRIX 336
#define HLSL_PIXEL_PARAMETERS_BUMP_MATRIX_REGISTER 21
#define HLSL_PIXEL_PARAMETERS_BUMP_LUMINANCE 400
#define HLSL_PIXEL_PARAMETERS_BUMP_LUMINANCE_REGISTER 25
#define HLSL_PIXEL_PARAMETERS_TEXTURE_SCALE 464
#define HLSL_PIXEL_PARAMETERS_TEXTURE_SCALE_REGISTER 29
#define HLSL_PIXEL_PARAMETERS_SIZE 528

/* the per-pixel model lighting's constants (xgpu.h XGPU_MODEL_LIGHT_COUNT),
declared by the lit pixel shaders only */
#define HLSL_MODEL_LIGHTS_SLOT 1
#define HLSL_MODEL_LIGHT_COUNT 12
#define HLSL_MODEL_LIGHTS_LIGHTS 0
#define HLSL_MODEL_LIGHTS_LIGHTS_REGISTER 0
#define HLSL_MODEL_LIGHTS_SIZE 192

#define HLSL_PIXEL_PARAMETERS_TEXT \
	"cbuffer pixel_parameters : register(b" HLSL_REGISTER_TEXT(HLSL_PIXEL_PARAMETERS_SLOT) ")\n" \
	"{\n" \
	"\tfloat4 ps_c0[8] : packoffset(c" HLSL_REGISTER_TEXT(HLSL_PIXEL_PARAMETERS_PS_C0_REGISTER) ");\n" \
	"\tfloat4 ps_c1[8] : packoffset(c" HLSL_REGISTER_TEXT(HLSL_PIXEL_PARAMETERS_PS_C1_REGISTER) ");\n" \
	"\tfloat4 ps_final_c0 : packoffset(c" HLSL_REGISTER_TEXT(HLSL_PIXEL_PARAMETERS_PS_FINAL_C0_REGISTER) ");\n" \
	"\tfloat4 ps_final_c1 : packoffset(c" HLSL_REGISTER_TEXT(HLSL_PIXEL_PARAMETERS_PS_FINAL_C1_REGISTER) ");\n" \
	"\tfloat4 fog_color : packoffset(c" HLSL_REGISTER_TEXT(HLSL_PIXEL_PARAMETERS_FOG_COLOR_REGISTER) ");\n" \
	"\tfloat4 fog_parameters : packoffset(c" HLSL_REGISTER_TEXT(HLSL_PIXEL_PARAMETERS_FOG_PARAMETERS_REGISTER) ");\n" \
	"\tfloat4 alpha_reference : packoffset(c" HLSL_REGISTER_TEXT(HLSL_PIXEL_PARAMETERS_ALPHA_REFERENCE_REGISTER) ");\n" \
	"\tfloat4 bump_matrix[4] : packoffset(c" HLSL_REGISTER_TEXT(HLSL_PIXEL_PARAMETERS_BUMP_MATRIX_REGISTER) ");\n" \
	"\tfloat4 bump_luminance[4] : packoffset(c" HLSL_REGISTER_TEXT(HLSL_PIXEL_PARAMETERS_BUMP_LUMINANCE_REGISTER) ");\n" \
	"\tfloat4 texture_scale[4] : packoffset(c" HLSL_REGISTER_TEXT(HLSL_PIXEL_PARAMETERS_TEXTURE_SCALE_REGISTER) ");\n" \
	"};\n"

#define HLSL_MODEL_LIGHTS_TEXT \
	"cbuffer model_lights : register(b" HLSL_REGISTER_TEXT(HLSL_MODEL_LIGHTS_SLOT) ")\n" \
	"{\n" \
	"\tfloat4 model_lights[" HLSL_REGISTER_TEXT(HLSL_MODEL_LIGHT_COUNT) "] : packoffset(c" \
		HLSL_REGISTER_TEXT(HLSL_MODEL_LIGHTS_LIGHTS_REGISTER) ");\n" \
	"};\n"

struct hlsl_pixel_parameters
{
	float ps_c0[8][4];
	float ps_c1[8][4];
	float ps_final_c0[4];
	float ps_final_c1[4];
	float fog_color[4];
	float fog_parameters[4];
	/* x: the reference, 0 to 255; y, z, w: 0 */
	float alpha_reference[4];
	float bump_matrix[4][4];
	float bump_luminance[4][4];
	float texture_scale[4][4];
};

struct hlsl_model_lights
{
	float model_lights[HLSL_MODEL_LIGHT_COUNT][4];
};

/* the structures against the defines */
_Static_assert(offsetof(struct hlsl_vertex_constants, c) == HLSL_VERTEX_CONSTANTS_C, "vertex_constants c");
_Static_assert(HLSL_VERTEX_CONSTANTS_C == HLSL_VERTEX_CONSTANTS_C_REGISTER * 16, "vertex_constants c register");
_Static_assert(sizeof(struct hlsl_vertex_constants) == HLSL_VERTEX_CONSTANTS_SIZE, "vertex_constants size");

_Static_assert(offsetof(struct hlsl_vertex_parameters, viewport_scale) == HLSL_VERTEX_PARAMETERS_VIEWPORT_SCALE,
	"vertex_parameters viewport_scale");
_Static_assert(offsetof(struct hlsl_vertex_parameters, viewport_offset) == HLSL_VERTEX_PARAMETERS_VIEWPORT_OFFSET,
	"vertex_parameters viewport_offset");
_Static_assert(offsetof(struct hlsl_vertex_parameters, screen) == HLSL_VERTEX_PARAMETERS_SCREEN,
	"vertex_parameters screen");
_Static_assert(HLSL_VERTEX_PARAMETERS_VIEWPORT_SCALE == HLSL_VERTEX_PARAMETERS_VIEWPORT_SCALE_REGISTER * 16,
	"vertex_parameters viewport_scale register");
_Static_assert(HLSL_VERTEX_PARAMETERS_VIEWPORT_OFFSET == HLSL_VERTEX_PARAMETERS_VIEWPORT_OFFSET_REGISTER * 16,
	"vertex_parameters viewport_offset register");
_Static_assert(HLSL_VERTEX_PARAMETERS_SCREEN == HLSL_VERTEX_PARAMETERS_SCREEN_REGISTER * 16,
	"vertex_parameters screen register");
_Static_assert(sizeof(struct hlsl_vertex_parameters) == HLSL_VERTEX_PARAMETERS_SIZE, "vertex_parameters size");

#define HLSL_PIXEL_MEMBER_ASSERT(member, MEMBER) \
	_Static_assert(offsetof(struct hlsl_pixel_parameters, member) == HLSL_PIXEL_PARAMETERS_##MEMBER, \
		"pixel_parameters " #member); \
	_Static_assert(HLSL_PIXEL_PARAMETERS_##MEMBER == HLSL_PIXEL_PARAMETERS_##MEMBER##_REGISTER * 16, \
		"pixel_parameters " #member " register")
HLSL_PIXEL_MEMBER_ASSERT(ps_c0, PS_C0);
HLSL_PIXEL_MEMBER_ASSERT(ps_c1, PS_C1);
HLSL_PIXEL_MEMBER_ASSERT(ps_final_c0, PS_FINAL_C0);
HLSL_PIXEL_MEMBER_ASSERT(ps_final_c1, PS_FINAL_C1);
HLSL_PIXEL_MEMBER_ASSERT(fog_color, FOG_COLOR);
HLSL_PIXEL_MEMBER_ASSERT(fog_parameters, FOG_PARAMETERS);
HLSL_PIXEL_MEMBER_ASSERT(alpha_reference, ALPHA_REFERENCE);
HLSL_PIXEL_MEMBER_ASSERT(bump_matrix, BUMP_MATRIX);
HLSL_PIXEL_MEMBER_ASSERT(bump_luminance, BUMP_LUMINANCE);
HLSL_PIXEL_MEMBER_ASSERT(texture_scale, TEXTURE_SCALE);
#undef HLSL_PIXEL_MEMBER_ASSERT
_Static_assert(sizeof(struct hlsl_pixel_parameters) == HLSL_PIXEL_PARAMETERS_SIZE, "pixel_parameters size");

_Static_assert(offsetof(struct hlsl_model_lights, model_lights) == HLSL_MODEL_LIGHTS_LIGHTS, "model_lights");
_Static_assert(HLSL_MODEL_LIGHTS_LIGHTS == HLSL_MODEL_LIGHTS_LIGHTS_REGISTER * 16, "model_lights register");
_Static_assert(sizeof(struct hlsl_model_lights) == HLSL_MODEL_LIGHTS_SIZE, "model_lights size");

/* ---------- textures

Stage N's texture is tN and its sampler sN: Texture2D, Texture3D or
TextureCube by the key's sampler_type */
#define HLSL_TEXTURE_SLOT_FIRST 0
#define HLSL_SAMPLER_SLOT_FIRST 0

/* ---------- the interface between the stages

The values the vertex shader hands the pixel shader, in this order, with
these semantics: every vertex shader writes all nine, every pixel shader
reads all nine, so any vertex shader goes with any pixel shader (a vertex
shader's outputs may exceed a pixel shader's inputs, never the reverse). A
vertex shader lit for each pixel adds the normal and, with the point
lights, the world position after them, and only a lit pixel shader reads
them (d3d8_gl.c's prepare_draw pairs them so). In both stages the values
are statics the generated body reads and writes as GLSL's ins and outs (the
generators' bodies are the GLSL ones), copied from the input structure at
the start of the pixel shader and into the output one at the end of the
vertex shader. */
#define HLSL_INTERFACE_MEMBERS \
	"\tfloat4 xD0 : TEXCOORD0;\n" \
	"\tfloat4 xD1 : TEXCOORD1;\n" \
	"\tfloat4 xB0 : TEXCOORD2;\n" \
	"\tfloat4 xB1 : TEXCOORD3;\n" \
	"\tfloat4 xT0 : TEXCOORD4;\n" \
	"\tfloat4 xT1 : TEXCOORD5;\n" \
	"\tfloat4 xT2 : TEXCOORD6;\n" \
	"\tfloat4 xT3 : TEXCOORD7;\n" \
	"\tfloat xFog : TEXCOORD8;\n"
#define HLSL_INTERFACE_NORMAL_MEMBER "\tfloat4 xWorldNormal : TEXCOORD9;\n"
#define HLSL_INTERFACE_POSITION_MEMBER "\tfloat3 xWorldPosition : TEXCOORD10;\n"

/* the structure both stages name vs_out: the position, then the members,
then the lit ones (HLSL_INTERFACE_NORMAL_MEMBER ...), then
HLSL_INTERFACE_END */
#define HLSL_INTERFACE_BEGIN \
	"struct vs_out\n" \
	"{\n" \
	"\tprecise float4 position : SV_Position;\n" \
	HLSL_INTERFACE_MEMBERS
#define HLSL_INTERFACE_END "};\n"

/* the statics the body reads and writes (GLSL's "out vec4 xD0;" and so on) */
#define HLSL_INTERFACE_STATICS \
	"static float4 xD0;\n" \
	"static float4 xD1;\n" \
	"static float4 xB0;\n" \
	"static float4 xB1;\n" \
	"static float4 xT0;\n" \
	"static float4 xT1;\n" \
	"static float4 xT2;\n" \
	"static float4 xT3;\n" \
	"static float xFog;\n"

/* the vertex shader's last lines: the statics into the output, then the lit
ones (HLSL_INTERFACE_NORMAL_OUTPUT ...), then HLSL_INTERFACE_RETURN */
#define HLSL_INTERFACE_OUTPUT \
	"\toutput.xD0 = xD0;\n" \
	"\toutput.xD1 = xD1;\n" \
	"\toutput.xB0 = xB0;\n" \
	"\toutput.xB1 = xB1;\n" \
	"\toutput.xT0 = xT0;\n" \
	"\toutput.xT1 = xT1;\n" \
	"\toutput.xT2 = xT2;\n" \
	"\toutput.xT3 = xT3;\n" \
	"\toutput.xFog = xFog;\n"
#define HLSL_INTERFACE_NORMAL_OUTPUT "\toutput.xWorldNormal = xWorldNormal;\n"
#define HLSL_INTERFACE_POSITION_OUTPUT "\toutput.xWorldPosition = xWorldPosition;\n"
#define HLSL_INTERFACE_RETURN "\treturn output;\n"

/* the pixel shader's first lines: the input into the statics, then the lit
ones */
#define HLSL_INTERFACE_INPUT \
	"\txD0 = input.xD0;\n" \
	"\txD1 = input.xD1;\n" \
	"\txB0 = input.xB0;\n" \
	"\txB1 = input.xB1;\n" \
	"\txT0 = input.xT0;\n" \
	"\txT1 = input.xT1;\n" \
	"\txT2 = input.xT2;\n" \
	"\txT3 = input.xT3;\n" \
	"\txFog = input.xFog;\n"
#define HLSL_INTERFACE_NORMAL_INPUT "\txWorldNormal = input.xWorldNormal;\n"
#define HLSL_INTERFACE_POSITION_INPUT "\txWorldPosition = input.xWorldPosition;\n"

/* the multisampled alpha test's output (nv2a_psh_hlsl.c): the colour and the
samples covered, at ps_4_1 */
#define HLSL_PIXEL_COVERAGE_OUTPUT \
	"struct ps_out\n" \
	"{\n" \
	"\tfloat4 color : SV_Target;\n" \
	"\tuint coverage : SV_Coverage;\n" \
	"};\n"

/* the vertex shader's inputs: v<i> at TEXCOORD<i>, as float4, as the packed
uint (D3DVSDT_NORMPACKED3) or as int4 (D3DVSDT_SHORT1 to 4) */
#define HLSL_VERTEX_INPUT_COUNT 16

/* the entry points and the targets */
#define HLSL_ENTRY "main"
#define HLSL_VERTEX_TARGET "vs_4_0"
#define HLSL_PIXEL_TARGET "ps_4_0"
#define HLSL_PIXEL_COVERAGE_TARGET "ps_4_1"

/* ---------- the generators */

struct nv2a_vertex_lighting;
struct nv2a_pixel_shader_key;

/* HLSL for an NV2A vertex program (nv2a_vertex_shader_to_glsl's, xgpu.h):
the attributes whose bits are set in packed_mask are fed as NORMPACKED3's
32-bit integers and unpacked, those in integer_mask as signed 16-bit
integers and converted, and those in three_component_mask have their w made
1 (no DXGI format has three 16-bit or 8-bit components: the layout reads
four). With lighting (else NULL), the normal and world position go to the
pixel shader too. Entry HLSL_ENTRY, target HLSL_VERTEX_TARGET. Returns a
malloc'd string. */
char *nv2a_hlsl_vertex_shader(const uint32_t *instructions, uint32_t count, uint32_t packed_mask,
	uint32_t integer_mask, uint32_t three_component_mask, const struct nv2a_vertex_lighting *lighting);

/* HLSL for a pixel shader key (nv2a_pixel_shader_to_glsl's, xgpu.h), and its
target in target: HLSL_PIXEL_TARGET, or HLSL_PIXEL_COVERAGE_TARGET for a
multisampled alpha test (alpha_test_samples), which writes SV_Coverage.
Entry HLSL_ENTRY. Returns a malloc'd string. */
char *nv2a_hlsl_pixel_shader(const struct nv2a_pixel_shader_key *key, char target[8]);

#endif
