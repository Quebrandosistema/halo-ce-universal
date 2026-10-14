/*
NV2A_PSH_HLSL.C

Translation of Xbox pixel shaders - the NV2A texture shader stages and
register combiners, as held in the pixel shader render states - into HLSL,
for the Windows build's Direct3D 11 renderer (port/windows/D3D11.md, phase
4). A copy of port/linux/src/nv2a_psh.c at commit 99902271 (the last to
change it) in which only what is GLSL's grammar is HLSL's: the prologue (the
constant buffers, the interface's structure and statics from
hlsl_shaders.h), the types, constructors and functions, the textures and
their samplers, the multisampled alpha test's coverage (SV_Coverage, at
ps_4_1), the colour returned and (phase 6) the multiply by the blend
constant's alpha for the draw that blends by it. The texture stages, the combiners, fog,
the alpha test, alpha kill, colour sign, coverage_alpha, the per-pixel
lighting and the debug.gpu_debug_* settings are the original's, line for
line; the Android branches (the LOD bias uniform, the atomic counter) do not
come (the copy is the desktop's). Changes to nv2a_psh.c are not followed
automatically. tools/d3d11_shader_check.py proves each dumped shader's text
is the original's with these changes and no other.

A pixel shader runs in three parts:
- texture stages 0-3 (PSTextureModes) fetch t0-t3, some of them using the
  result of an earlier stage (dot product and bump environment modes);
- up to eight general combiner stages (PSRGBInputs/Outputs and
  PSAlphaInputs/Outputs) that each compute A*B, C*D and their sum or mux
  on the registers and write them back;
- the final combiner (PSFinalCombinerInputsABCD/EFG):
  rgb = A*B + (1-A)*C + D, alpha = G.
Register values are clamped to [-1, 1] between stages, as on the hardware.
*/

#include "xgpu.h"
#include "port_config.h"
#include "hlsl_shaders.h"

#include <stdio.h>
#include <stdlib.h>

enum
{
	_register_zero = 0,
	_register_c0 = 1,
	_register_c1 = 2,
	_register_fog = 3,
	_register_v0 = 4,
	_register_v1 = 5,
	_register_t0 = 8,
	_register_t1 = 9,
	_register_t2 = 10,
	_register_t3 = 11,
	_register_r0 = 12,
	_register_r1 = 13,
	_register_v1r0_sum = 14,
	_register_ef_product = 15,
};

enum
{
	_mode_none = 0x00,
	_mode_project2d = 0x01,
	_mode_project3d = 0x02,
	_mode_cubemap = 0x03,
	_mode_passthru = 0x04,
	_mode_clipplane = 0x05,
	_mode_bumpenvmap = 0x06,
	_mode_bumpenvmap_luminance = 0x07,
	_mode_brdf = 0x08,
	_mode_dot_st = 0x09,
	_mode_dot_zw = 0x0a,
	_mode_dot_reflect_diffuse = 0x0b,
	_mode_dot_reflect_specular = 0x0c,
	_mode_dot_str_3d = 0x0d,
	_mode_dot_str_cube = 0x0e,
	_mode_dependent_ar = 0x0f,
	_mode_dependent_gb = 0x10,
	_mode_dot_product = 0x11,
	_mode_dot_reflect_specular_constant = 0x12,
};

/* ---------- combiner inputs */

static const char *register_expression(unsigned long reg, int stage, BOOL unique_c0, BOOL unique_c1)
{
	static char buffer[4][32];
	static int next = 0;
	char *result = buffer[next++ & 3];

	switch (reg)
	{
	case _register_c0:
		if (stage < 0)
			return "ps_final_c0";
		snprintf(result, sizeof(buffer[0]), "ps_c0[%d]", unique_c0 ? stage : 0);
		return result;
	case _register_c1:
		if (stage < 0)
			return "ps_final_c1";
		snprintf(result, sizeof(buffer[0]), "ps_c1[%d]", unique_c1 ? stage : 0);
		return result;
	case _register_fog: return "fog";
	case _register_v0: return "v0";
	case _register_v1: return "v1";
	case _register_t0: return "t0";
	case _register_t1: return "t1";
	case _register_t2: return "t2";
	case _register_t3: return "t3";
	case _register_r0: return "r0";
	case _register_r1: return "r1";
	case _register_v1r0_sum: return stage < 0 ? "v1r0_sum" : "((float4)(0.0))";
	case _register_ef_product: return stage < 0 ? "ef_product" : "((float4)(0.0))";
	default: return "((float4)(0.0))";
	}
}

/* one combiner input byte as a float3 (rgb) or float (alpha) expression */
static void combiner_input(struct xgpu_text *text, unsigned long input, BOOL alpha_portion, int stage,
	BOOL unique_c0, BOOL unique_c1)
{
	unsigned long reg = input & 0x0f;
	BOOL alpha_channel = (input & 0x10) != 0;
	unsigned long mapping = input & 0xe0;
	const char *source = register_expression(reg, stage, unique_c0, unique_c1);
	char value[64];

	if (alpha_portion)
		snprintf(value, sizeof(value), "%s.%s", source, alpha_channel ? "a" : "b");
	else if (alpha_channel)
		snprintf(value, sizeof(value), "((float3)(%s.a))", source);
	else
		snprintf(value, sizeof(value), "%s.rgb", source);

	switch (mapping)
	{
	case 0x00: xgpu_text_append(text, "max(%s, 0.0)", value); break;
	case 0x20: xgpu_text_append(text, "(1.0 - clamp(%s, 0.0, 1.0))", value); break;
	case 0x40: xgpu_text_append(text, "(2.0 * max(%s, 0.0) - 1.0)", value); break;
	case 0x60: xgpu_text_append(text, "(1.0 - 2.0 * max(%s, 0.0))", value); break;
	case 0x80: xgpu_text_append(text, "(max(%s, 0.0) - 0.5)", value); break;
	case 0xa0: xgpu_text_append(text, "(0.5 - max(%s, 0.0))", value); break;
	case 0xc0: xgpu_text_append(text, "(%s)", value); break;
	default: xgpu_text_append(text, "(-%s)", value); break;
	}
}

/* final combiner inputs only have the unsigned identity and invert mappings */
static void final_input(struct xgpu_text *text, unsigned long input, BOOL alpha_portion)
{
	unsigned long reg = input & 0x0f;
	BOOL alpha_channel = (input & 0x10) != 0;
	const char *source = register_expression(reg, -1, FALSE, FALSE);
	char value[64];

	if (alpha_portion)
		snprintf(value, sizeof(value), "%s.%s", source, alpha_channel ? "a" : "b");
	else if (alpha_channel)
		snprintf(value, sizeof(value), "((float3)(%s.a))", source);
	else
		snprintf(value, sizeof(value), "%s.rgb", source);
	if (input & 0x20)
		xgpu_text_append(text, "(1.0 - clamp(%s, 0.0, 1.0))", value);
	else
		xgpu_text_append(text, "clamp(%s, 0.0, 1.0)", value);
}

static const char *destination_name(unsigned long reg)
{
	switch (reg)
	{
	case _register_v0: return "v0";
	case _register_v1: return "v1";
	case _register_t0: return "t0";
	case _register_t1: return "t1";
	case _register_t2: return "t2";
	case _register_t3: return "t3";
	case _register_r0: return "r0";
	case _register_r1: return "r1";
	default: return NULL;
	}
}

static const char *output_mapping(unsigned long flags)
{
	switch (flags & 0x38)
	{
	case 0x08: return "(%s - 0.5)";
	case 0x10: return "(%s * 2.0)";
	case 0x18: return "((%s - 0.5) * 2.0)";
	case 0x20: return "(%s * 4.0)";
	case 0x30: return "(%s * 0.5)";
	default: return "(%s)";
	}
}

/* ---------- one general combiner stage */

static void combiner_stage(struct xgpu_text *text, const DWORD *state, int stage)
{
	DWORD combiner_count = state[D3DRS_PSCOMBINERCOUNT];
	BOOL unique_c0 = (combiner_count & 0x1000) != 0;
	BOOL unique_c1 = (combiner_count & 0x10000) != 0;
	BOOL mux_msb = (combiner_count & 0x100) != 0;
	int portion;

	xgpu_text_append(text, "\t/* combiner stage %d */\n\t{\n", stage);
	for (portion = 0; portion < 2; portion++)
	{
		BOOL alpha = portion == 1;
		DWORD inputs = alpha ? state[D3DRS_PSALPHAINPUTS0 + stage] : state[D3DRS_PSRGBINPUTS0 + stage];
		DWORD outputs = alpha ? state[D3DRS_PSALPHAOUTPUTS0 + stage] : state[D3DRS_PSRGBOUTPUTS0 + stage];
		unsigned long flags = outputs >> 12;
		const char *type = alpha ? "float" : "float3";
		const char *prefix = alpha ? "a" : "c";
		const char *mapping = output_mapping(flags);
		char mapped[64];

		xgpu_text_append(text, "\t\t%s %sA = ", type, prefix);
		combiner_input(text, (inputs >> 24) & 0xff, alpha, stage, unique_c0, unique_c1);
		xgpu_text_append(text, ";\n\t\t%s %sB = ", type, prefix);
		combiner_input(text, (inputs >> 16) & 0xff, alpha, stage, unique_c0, unique_c1);
		xgpu_text_append(text, ";\n\t\t%s %sC = ", type, prefix);
		combiner_input(text, (inputs >> 8) & 0xff, alpha, stage, unique_c0, unique_c1);
		xgpu_text_append(text, ";\n\t\t%s %sD = ", type, prefix);
		combiner_input(text, inputs & 0xff, alpha, stage, unique_c0, unique_c1);
		xgpu_text_append(text, ";\n");

		if (!alpha && (flags & 0x02))
			xgpu_text_append(text, "\t\tfloat3 cAB = ((float3)(dot(cA, cB)));\n");
		else
			xgpu_text_append(text, "\t\t%s %sAB = %sA * %sB;\n", type, prefix, prefix, prefix);
		if (!alpha && (flags & 0x01))
			xgpu_text_append(text, "\t\tfloat3 cCD = ((float3)(dot(cC, cD)));\n");
		else
			xgpu_text_append(text, "\t\t%s %sCD = %sC * %sD;\n", type, prefix, prefix, prefix);
		if (flags & 0x04)
		{
			if (mux_msb)
				xgpu_text_append(text, "\t\t%s %sSUM = r0.a >= 0.5 ? %sCD : %sAB;\n", type, prefix, prefix, prefix);
			else
				xgpu_text_append(text, "\t\t%s %sSUM = ((int)(r0.a * 255.0 + 0.5) & 1) != 0 ? %sCD : %sAB;\n",
					type, prefix, prefix, prefix);
		}
		else
		{
			xgpu_text_append(text, "\t\t%s %sSUM = %sAB + %sCD;\n", type, prefix, prefix, prefix);
		}
		snprintf(mapped, sizeof(mapped), mapping, "%sAB");
		xgpu_text_append(text, "\t\t%sAB = clamp(", prefix);
		xgpu_text_append(text, mapped, prefix);
		xgpu_text_append(text, ", -1.0, 1.0);\n");
		snprintf(mapped, sizeof(mapped), mapping, "%sCD");
		xgpu_text_append(text, "\t\t%sCD = clamp(", prefix);
		xgpu_text_append(text, mapped, prefix);
		xgpu_text_append(text, ", -1.0, 1.0);\n");
		snprintf(mapped, sizeof(mapped), mapping, "%sSUM");
		xgpu_text_append(text, "\t\t%sSUM = clamp(", prefix);
		xgpu_text_append(text, mapped, prefix);
		xgpu_text_append(text, ", -1.0, 1.0);\n");
	}

	/* write back only after both portions have read their inputs */
	for (portion = 0; portion < 2; portion++)
	{
		BOOL alpha = portion == 1;
		DWORD outputs = alpha ? state[D3DRS_PSALPHAOUTPUTS0 + stage] : state[D3DRS_PSRGBOUTPUTS0 + stage];
		unsigned long flags = outputs >> 12;
		const char *prefix = alpha ? "a" : "c";
		const char *component = alpha ? "a" : "rgb";
		const char *ab = destination_name((outputs >> 4) & 0x0f);
		const char *cd = destination_name(outputs & 0x0f);
		const char *sum = destination_name((outputs >> 8) & 0x0f);

		if (ab)
		{
			xgpu_text_append(text, "\t\t%s.%s = %sAB;\n", ab, component, prefix);
			if (!alpha && (flags & 0x80))
				xgpu_text_append(text, "\t\t%s.a = cAB.b;\n", ab);
		}
		if (cd)
		{
			xgpu_text_append(text, "\t\t%s.%s = %sCD;\n", cd, component, prefix);
			if (!alpha && (flags & 0x40))
				xgpu_text_append(text, "\t\t%s.a = cCD.b;\n", cd);
		}
		if (sum)
			xgpu_text_append(text, "\t\t%s.%s = %sSUM;\n", sum, component, prefix);
	}
	xgpu_text_append(text, "\t}\n");
}

/* ---------- texture stages */

static const char *sampler_declaration(unsigned char type)
{
	switch (type)
	{
	case _xgpu_sampler_3d: return "Texture3D";
	case _xgpu_sampler_cube: return "TextureCube";
	default: return "Texture2D";
	}
}

static unsigned long stage_mode(const struct nv2a_pixel_shader_key *key, int stage)
{
	return (key->texture_modes >> (5 * stage)) & 0x1f;
}

static int stage_input(const DWORD *state, int stage)
{
	switch (stage)
	{
	case 2: return (state[D3DRS_PSINPUTTEXTURE] >> 16) & 1;
	case 3: return (state[D3DRS_PSINPUTTEXTURE] >> 20) & 3;
	default: return 0;
	}
}

/* the input texel of a dot product stage, mapped per PSDotMapping */
static void dot_input(struct xgpu_text *text, const DWORD *state, int stage)
{
	unsigned long mapping = (state[D3DRS_PSDOTMAPPING] >> ((stage - 1) * 4)) & 7;
	int input = stage_input(state, stage);

	switch (mapping)
	{
	case 0: xgpu_text_append(text, "t%d.rgb", input); break;
	case 1: xgpu_text_append(text, "((t%d.rgb * 255.0 - 128.0) / 127.0)", input); break;
	case 3: xgpu_text_append(text, "signed_bytes(t%d.rgb)", input); break;
	default: xgpu_text_append(text, "(t%d.rgb * 2.0 - 1.0)", input); break;
	}
}

/* (the LOD bias is the sampler's, as on the desktop) */
#define SAMPLE_BIAS ""

static void sample(struct xgpu_text *text, const struct nv2a_pixel_shader_key *key, int stage, const char *coordinates)
{
	switch (key->sampler_type[stage])
	{
	case _xgpu_sampler_3d:
		xgpu_text_append(text, "tex%d.Sample(tex%d_sampler, (%s).xyz" SAMPLE_BIAS ")", stage, stage, coordinates
			);
		break;
	case _xgpu_sampler_cube:
		xgpu_text_append(text, "tex%d.Sample(tex%d_sampler, (%s).xyz" SAMPLE_BIAS ")", stage, stage, coordinates
			);
		break;
	default:
		xgpu_text_append(text, "tex%d.Sample(tex%d_sampler, (%s).xy * texture_scale[%d].xy" SAMPLE_BIAS ")", stage,
			stage, coordinates, stage
			);
		break;
	}
}

static void texture_stage(struct xgpu_text *text, const struct nv2a_pixel_shader_key *key, int stage)
{
	const DWORD *state = key->combiner_state;
	unsigned long mode = stage_mode(key, stage);
	char coordinates[96];

	xgpu_text_append(text, "\t/* texture stage %d, mode %lu */\n", stage, mode);
	if (key->sampler_type[stage] == _xgpu_sampler_none &&
		mode != _mode_passthru && mode != _mode_clipplane && mode != _mode_dot_product && mode != _mode_dot_zw)
	{
		mode = _mode_none;
	}
	switch (mode)
	{
	case _mode_project2d:
	case _mode_project3d:
		snprintf(coordinates, sizeof(coordinates), "float4(xT%d.xyz / (xT%d.w != 0.0 ? xT%d.w : 1.0), 1.0)", stage, stage, stage);
		xgpu_text_append(text, "\tt%d = ", stage);
		sample(text, key, stage, coordinates);
		xgpu_text_append(text, ";\n");
		if (stage == 0 && key->point_threshold && key->sampler_type[stage] == _xgpu_sampler_2d)
		{
			/* IDs must not be averaged with another segment or corrupted by
			low-alpha artwork. Zero marks the atlas's continuous meters. */
			xgpu_text_append(text,
				"\t{\n\t\tfloat2 uv = (%s).xy * texture_scale[0].xy;\n"
				"\t\tint2 size; tex0.GetDimensions(size.x, size.y);\n"
				"\t\tint2 pixel = clamp(((int2)(floor(uv * ((float2)(size))))), ((int2)(0)), size - 1);\n"
				"\t\tfloat threshold = tex0.Load(int3(pixel, 0)).r;\n"
				"\t\tif (threshold > 0.0) t0.rb = ((float2)(threshold));\n\t}\n", coordinates);
		}
		break;
	case _mode_cubemap:
		snprintf(coordinates, sizeof(coordinates), "xT%d", stage);
		xgpu_text_append(text, "\tt%d = ", stage);
		sample(text, key, stage, coordinates);
		xgpu_text_append(text, ";\n");
		break;
	case _mode_passthru:
		xgpu_text_append(text, "\tt%d = clamp(xT%d, 0.0, 1.0);\n", stage, stage);
		break;
	case _mode_clipplane:
	{
		unsigned long compare = (state[D3DRS_PSCOMPAREMODE] >> (4 * stage)) & 0xf;
		static const char components[] = "xyzw";
		int component;

		for (component = 0; component < 4; component++)
		{
			xgpu_text_append(text, "\tif (xT%d.%c %s 0.0) discard;\n", stage, components[component],
				(compare & (1 << component)) ? ">=" : "<");
		}
		xgpu_text_append(text, "\tt%d = ((float4)(0.0));\n", stage);
		break;
	}
	case _mode_bumpenvmap:
	case _mode_bumpenvmap_luminance:
	{
		int input = stage - 1;

		xgpu_text_append(text, "\t{\n\t\tfloat2 d = signed_bytes(t%d.rgb).rg;\n", input);
		xgpu_text_append(text, "\t\tfloat2 coordinates = xT%d.xy + float2(bump_matrix[%d].x * d.x + bump_matrix[%d].z * d.y,"
			" bump_matrix[%d].y * d.x + bump_matrix[%d].w * d.y);\n", stage, stage, stage, stage, stage);
		xgpu_text_append(text, "\t\tt%d = ", stage);
		sample(text, key, stage, "float4(coordinates, 0.0, 1.0)");
		xgpu_text_append(text, ";\n");
		if (mode == _mode_bumpenvmap_luminance)
		{
			xgpu_text_append(text, "\t\tt%d.rgb *= clamp(bump_luminance[%d].x * t%d.b + bump_luminance[%d].y, 0.0, 1.0);\n",
				stage, stage, input, stage);
		}
		xgpu_text_append(text, "\t}\n");
		break;
	}
	case _mode_dot_product:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, state, stage);
		xgpu_text_append(text, ");\n\tt%d = ((float4)(0.0));\n", stage);
		break;
	case _mode_dot_st:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, state, stage);
		xgpu_text_append(text, ");\n\tt%d = ", stage);
		snprintf(coordinates, sizeof(coordinates), "float4(dot%d, dot%d, 0.0, 1.0)", stage - 1, stage);
		sample(text, key, stage, coordinates);
		xgpu_text_append(text, ";\n");
		break;
	case _mode_dot_zw:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, state, stage);
		xgpu_text_append(text, ");\n\tt%d = ((float4)(0.0));\n", stage);
		break;
	case _mode_dot_reflect_diffuse:
		/* the normal takes its third component from stage 3's dot product */
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, state, stage);
		xgpu_text_append(text, ");\n\tdot3 = dot(xT3.xyz, ");
		dot_input(text, state, 3);
		xgpu_text_append(text, ");\n\tt%d = ", stage);
		sample(text, key, stage, "float4(dot1, dot2, dot3, 1.0)");
		xgpu_text_append(text, ";\n");
		break;
	case _mode_dot_reflect_specular:
	case _mode_dot_reflect_specular_constant:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, state, stage);
		xgpu_text_append(text, ");\n\t{\n\t\tfloat3 n = float3(dot1, dot2, dot3);\n");
		if (mode == _mode_dot_reflect_specular)
			xgpu_text_append(text, "\t\tfloat3 e = float3(xT1.w, xT2.w, xT3.w);\n");
		else
			xgpu_text_append(text, "\t\tfloat3 e = ps_c0[0].xyz;\n");
		xgpu_text_append(text, "\t\tfloat3 r = 2.0 * n * dot(n, e) / max(dot(n, n), 1.0e-20) - e;\n\t\tt%d = ", stage);
		sample(text, key, stage, "float4(r, 1.0)");
		xgpu_text_append(text, ";\n\t}\n");
		break;
	case _mode_dot_str_3d:
	case _mode_dot_str_cube:
		xgpu_text_append(text, "\tdot%d = dot(xT%d.xyz, ", stage, stage);
		dot_input(text, state, stage);
		xgpu_text_append(text, ");\n\tt%d = ", stage);
		sample(text, key, stage, "float4(dot1, dot2, dot3, 1.0)");
		xgpu_text_append(text, ";\n");
		break;
	case _mode_dependent_ar:
		snprintf(coordinates, sizeof(coordinates), "float4(t%d.a, t%d.r, 0.0, 1.0)", stage_input(state, stage), stage_input(state, stage));
		xgpu_text_append(text, "\tt%d = ", stage);
		sample(text, key, stage, coordinates);
		xgpu_text_append(text, ";\n");
		break;
	case _mode_dependent_gb:
		snprintf(coordinates, sizeof(coordinates), "float4(t%d.g, t%d.b, 0.0, 1.0)", stage_input(state, stage), stage_input(state, stage));
		xgpu_text_append(text, "\tt%d = ", stage);
		sample(text, key, stage, coordinates);
		xgpu_text_append(text, ";\n");
		break;
	default:
		xgpu_text_append(text, "\tt%d = ((float4)(0.0));\n", stage);
		break;
	}

	/* channels the application marked signed (D3DTSS_COLORSIGN) */
	if (key->color_sign[stage] && mode != _mode_none)
	{
		static const char channels[] = "argb";
		int bit;

		for (bit = 0; bit < 4; bit++)
		{
			if (key->color_sign[stage] & (1 << bit))
				xgpu_text_append(text, "\tt%d.%c = signed_byte(t%d.%c);\n", stage, channels[bit], stage, channels[bit]);
		}
	}
	if (key->alpha_kill[stage] && mode != _mode_none)
		xgpu_text_append(text, "\tif (t%d.a == 0.0) discard;\n", stage);
}

/* ---------- the whole shader */

static const char *comparison_operator(unsigned long function)
{
	switch (function)
	{
	case D3DCMP_NEVER: return NULL;
	case D3DCMP_LESS: return "<";
	case D3DCMP_EQUAL: return "==";
	case D3DCMP_LESSEQUAL: return "<=";
	case D3DCMP_GREATER: return ">";
	case D3DCMP_NOTEQUAL: return "!=";
	case D3DCMP_GREATEREQUAL: return ">=";
	default: return "";
	}
}

/* ---------- per-pixel model lighting

The game lights its models for each vertex: its model lighting programs
(nv2a_vsh.c nv2a_vertex_shader_lighting) sum the ambient light, two distant
lights and two point lights into the diffuse color v0. Halo's models have
few vertices, so the light across a curved surface comes out in facets, and
a point light that passes close lights only the vertices it reaches.
model_lighting computes the same sum for each pixel, from the normal and
world position the vertex program hands on and the same constants
(XGPU_MODEL_LIGHT_COUNT): the same falloff, facing, cone and translucency
terms, clamped where the programs clamp them. The diffuse alpha, which the
programs write apart (0, or planar fog's), is kept. */

static void model_lighting(struct xgpu_text *text, BOOL point_lights)
{
	int light;

	xgpu_text_append(text,
		HLSL_MODEL_LIGHTS_TEXT
		"static float4 xWorldNormal;\n"
		"%s"
		"float3 model_lighting()\n"
		"{\n"
		/* the normal's direction between the vertices, at the length the
		vertices' normals have, as the programs light by it unnormalized
		(shorter than 1 on some models: the first-person weapons) */
		"\tfloat3 n = xWorldNormal.xyz * (xWorldNormal.w * rsqrt(max(dot(xWorldNormal.xyz, xWorldNormal.xyz), 1.0e-24)));\n"
		/* the ambient light, the first distant light, which also lights the
		back by the translucency, and the second */
		"\tfloat facing = dot(n, -model_lights[7].xyz);\n"
		"\tfloat3 light = model_lights[11].xyz + max(max(facing, -facing * model_lights[0].z), 0.0) * model_lights[8].xyz +\n"
		"\t\tmax(dot(n, -model_lights[9].xyz), 0.0) * model_lights[10].xyz;\n",
		point_lights ? "static float3 xWorldPosition;\n" : "");
	/* each point light: its position and 1 / radius squared, its cone's axis
	and falloff scale, its color and falloff offset */
	for (light = 0; point_lights && light < 2; light++)
	{
		int first = 1 + light * 3;

		xgpu_text_append(text,
			"\t{\n"
			"\t\tfloat3 to_light = model_lights[%d].xyz - xWorldPosition;\n"
			"\t\tfloat distance_squared = dot(to_light, to_light);\n"
			"\t\tfloat3 l = to_light * rsqrt(max(distance_squared, 1.0e-12));\n"
			"\t\tlight += max(1.0 - distance_squared * model_lights[%d].w, 0.0) * clamp(dot(l, n), 0.0, 1.0) *\n"
			"\t\t\tclamp(dot(l, -model_lights[%d].xyz) * model_lights[%d].w + model_lights[%d].w, 0.0, 1.0) *\n"
			"\t\t\tmodel_lights[%d].xyz;\n"
			"\t}\n",
			first, first, first + 1, first + 1, first + 2, first + 2);
	}
	xgpu_text_append(text, "\treturn clamp(light, 0.0, 1.0);\n}\n");
}

char *nv2a_hlsl_pixel_shader(const struct nv2a_pixel_shader_key *key, int by_constant_alpha, char target[8])
{
	const DWORD *state = key->combiner_state;
	struct xgpu_text text = { 0 };
	unsigned long combiner_count = state[D3DRS_PSCOMBINERCOUNT] & 0xff;
	DWORD final_abcd = state[D3DRS_PSFINALCOMBINERINPUTSABCD];
	DWORD final_efg = state[D3DRS_PSFINALCOMBINERINPUTSEFG];
	int stage;
	/* the multisampled alpha test writes SV_Coverage, shader model 4.1's
	(the alpha test's conditions below) */
	const char *comparison_test = key->alpha_test_function ? comparison_operator(key->alpha_test_function) : NULL;
	BOOL coverage = comparison_test && *comparison_test && key->alpha_test_samples && comparison_test[0] != '=' &&
		comparison_test[0] != '!';

	if (combiner_count > 8)
		combiner_count = 8;
	snprintf(target, 8, "%s", coverage ? HLSL_PIXEL_COVERAGE_TARGET : HLSL_PIXEL_TARGET);

	/* the constant buffer; the interface's structure, the lit members
	after the nine, and its statics; the coverage's output */
	xgpu_text_append(&text, "%s%s", HLSL_PIXEL_PARAMETERS_TEXT, HLSL_INTERFACE_BEGIN);
	if (key->per_pixel_lighting)
		xgpu_text_append(&text, "%s", HLSL_INTERFACE_NORMAL_MEMBER);
	if (key->per_pixel_lighting == 2)
		xgpu_text_append(&text, "%s", HLSL_INTERFACE_POSITION_MEMBER);
	xgpu_text_append(&text, "%s%s%s", HLSL_INTERFACE_END, coverage ? HLSL_PIXEL_COVERAGE_OUTPUT : "",
		HLSL_INTERFACE_STATICS);
	for (stage = 0; stage < 4; stage++)
	{
		xgpu_text_append(&text, "%s tex%d : register(t%d);\nSamplerState tex%d_sampler : register(s%d);\n",
			sampler_declaration(key->sampler_type[stage]), stage, HLSL_TEXTURE_SLOT_FIRST + stage, stage,
			HLSL_SAMPLER_SLOT_FIRST + stage);
	}
	if (key->per_pixel_lighting)
		model_lighting(&text, key->per_pixel_lighting == 2);
	xgpu_text_append(&text,
		"float signed_byte(float x)\n"
		"{\n"
		"	float b = floor(x * 255.0 + 0.5);\n"
		"	return (b >= 128.0 ? b - 256.0 : b) / 127.0;\n"
		"}\n"
		"float3 signed_bytes(float3 x)\n"
		"{\n"
		"	return float3(signed_byte(x.r), signed_byte(x.g), signed_byte(x.b));\n"
		"}\n"
		"%s main(vs_out input)%s\n"
		"{\n"
		HLSL_INTERFACE_INPUT
		"%s%s%s"
		"\tfloat4 v0 = xD0;\n"
		"\tfloat4 v1 = xD1;\n"
		"\tfloat4 t0 = ((float4)(0.0)), t1 = ((float4)(0.0)), t2 = ((float4)(0.0)), t3 = ((float4)(0.0));\n"
		"\tfloat dot0 = 0.0, dot1 = 0.0, dot2 = 0.0, dot3 = 0.0;\n",
		coverage ? "ps_out" : "float4", coverage ? "" : " : SV_Target",
		key->per_pixel_lighting ? HLSL_INTERFACE_NORMAL_INPUT : "",
		key->per_pixel_lighting == 2 ? HLSL_INTERFACE_POSITION_INPUT : "",
		coverage ? "\tps_out output;\n" : "");
	if (key->per_pixel_lighting)
		xgpu_text_append(&text, "\tv0.rgb = model_lighting();\n");

	for (stage = 0; stage < 4; stage++)
		texture_stage(&text, key, stage);

	/* the fog register: rgb is the fog color, alpha the fog factor */
	if (key->fog_enable)
	{
		switch (key->fog_table_mode)
		{
		case D3DFOG_EXP:
			xgpu_text_append(&text, "\tfloat fog_factor = exp(-fog_parameters.z * xFog);\n");
			break;
		case D3DFOG_EXP2:
			xgpu_text_append(&text, "\tfloat fog_factor = exp(-(fog_parameters.z * xFog) * (fog_parameters.z * xFog));\n");
			break;
		case D3DFOG_LINEAR:
			xgpu_text_append(&text, "\tfloat fog_factor = (fog_parameters.y - xFog) / max(fog_parameters.y - fog_parameters.x, 1.0e-6);\n");
			break;
		default:
			xgpu_text_append(&text, "\tfloat fog_factor = xFog;\n");
			break;
		}
	}
	else
	{
		xgpu_text_append(&text, "\tfloat fog_factor = 1.0;\n");
	}
	xgpu_text_append(&text,
		"\tfloat4 fog = float4(fog_color.rgb, clamp(fog_factor, 0.0, 1.0));\n"
		"\tfloat4 r0 = float4(0.0, 0.0, 0.0, t0.a);\n"
		"\tfloat4 r1 = ((float4)(0.0));\n");

	for (stage = 0; stage < (int)combiner_count; stage++)
		combiner_stage(&text, state, stage);

	if (final_abcd == 0 && final_efg == 0)
	{
		xgpu_text_append(&text, "\tfloat4 result = r0;\n");
	}
	else
	{
		unsigned long settings = final_efg & 0xff;

		xgpu_text_append(&text, "\tfloat4 ef_product = float4(");
		final_input(&text, (final_efg >> 24) & 0xff, FALSE);
		xgpu_text_append(&text, " * ");
		final_input(&text, (final_efg >> 16) & 0xff, FALSE);
		xgpu_text_append(&text, ", 0.0);\n");
		xgpu_text_append(&text, "\tfloat4 v1r0_sum = float4(%s + %s, 0.0);\n",
			(settings & 0x40) ? "(1.0 - clamp(v1.rgb, 0.0, 1.0))" : "clamp(v1.rgb, 0.0, 1.0)",
			(settings & 0x20) ? "(1.0 - clamp(r0.rgb, 0.0, 1.0))" : "clamp(r0.rgb, 0.0, 1.0)");
		if (settings & 0x80)
			xgpu_text_append(&text, "\tv1r0_sum = clamp(v1r0_sum, 0.0, 1.0);\n");
		xgpu_text_append(&text, "\tfloat3 fA = ");
		final_input(&text, (final_abcd >> 24) & 0xff, FALSE);
		xgpu_text_append(&text, ";\n\tfloat3 fB = ");
		final_input(&text, (final_abcd >> 16) & 0xff, FALSE);
		xgpu_text_append(&text, ";\n\tfloat3 fC = ");
		final_input(&text, (final_abcd >> 8) & 0xff, FALSE);
		xgpu_text_append(&text, ";\n\tfloat3 fD = ");
		final_input(&text, final_abcd & 0xff, FALSE);
		xgpu_text_append(&text, ";\n\tfloat fG = ");
		final_input(&text, (final_efg >> 8) & 0xff, TRUE);
		xgpu_text_append(&text, ";\n\tfloat4 result = float4(fA * fB + (1.0 - fA) * fC + fD, fG);\n");
	}

	if (key->coverage_alpha)
		xgpu_text_append(&text, "\tresult.a = lerp(1.0, result.a, t0.g);\n");
	if (key->alpha_test_function)
	{
		const char *comparison = comparison_operator(key->alpha_test_function);

		if (!comparison)
		{
			xgpu_text_append(&text, "\tdiscard;\n");
		}
		else if (*comparison && key->alpha_test_samples && comparison[0] != '=' && comparison[0] != '!')
		{
			/* multisampled: the samples covered as alpha passes the reference
			over the pixel (its change across the pixel from fwidth), the rest
			left as they are; alpha itself is kept, the game keeping values of
			its own in destination alpha */
			xgpu_text_append(&text,
				"\tfloat test_alpha = clamp(result.a, 0.0, 1.0) * 255.0;\n"
				"\tfloat test_coverage = clamp(%s(test_alpha - alpha_reference.x) / max(fwidth(test_alpha), 1.0) + 0.5, 0.0, 1.0);\n"
				"\tint test_samples = (int)(test_coverage * %d.0 + 0.5);\n"
				"\tif (test_samples == 0) discard;\n"
				"\toutput.coverage = (uint)((1 << test_samples) - 1);\n",
				comparison[0] == '<' ? "-" : "", (int)key->alpha_test_samples);
		}
		else if (*comparison)
		{
			xgpu_text_append(&text, "\tif (!(floor(clamp(result.a, 0.0, 1.0) * 255.0 + 0.5) %s alpha_reference.x)) discard;\n", comparison);
		}
	}
	if (*config_string("debug.gpu_debug_expression"))
		xgpu_text_append(&text, "\tresult = float4(((float3)(%s)), 1.0);\n", config_string("debug.gpu_debug_expression"));
	if (config_boolean("debug.gpu_debug_texture0"))
		xgpu_text_append(&text, "\tresult = float4(t0.rgb, 1.0);\n");
	if (config_boolean("debug.gpu_debug_flat"))
		xgpu_text_append(&text, "\tresult = xD0.a > 0.0 ? float4(xD0.rgb, 1.0) : float4(1.0, 0.0, 1.0, 1.0);\n");
	/* the source blend factor the constant's alpha (with the destination's
	the constant's colour: the transparent meter), taken into the colour, all
	four channels, after the alpha test (which reads the shader's alpha) and
	before the return (blending reads the product); the blend state's source
	factors are ONE (d3d8_d3d11.c) */
	if (by_constant_alpha == HLSL_BY_CONSTANT_ALPHA)
		xgpu_text_append(&text, "\tresult *= blend_constant.w;\n");
	else if (by_constant_alpha == HLSL_BY_INVERSE_CONSTANT_ALPHA)
		xgpu_text_append(&text, "\tresult *= 1.0 - blend_constant.w;\n");
	if (coverage)
		xgpu_text_append(&text, "\toutput.color = clamp(result, 0.0, 1.0);\n\treturn output;\n}\n");
	else
		xgpu_text_append(&text, "\treturn clamp(result, 0.0, 1.0);\n}\n");
	return text.buffer;
}
