/*
NV2A_VSH_HLSL.C

Translation of NV2A vertex programs (Xbox vertex shader microcode) into
HLSL, for the Windows build's Direct3D 11 renderer (port/windows/D3D11.md,
phase 4). A copy of port/linux/src/nv2a_vsh.c at commit 3dba558e (the last
to change it) in which only what is GLSL's grammar is HLSL's: the prologue
(the constant buffers, the interface's structure and statics from
hlsl_shaders.h), the types, constructors and functions, the vertex inputs (a
structure, with the integer and three-component attributes), and the
epilogue's output structure. The instruction decoding, the operations, the
screen-space conversion undone, the half pixel and the camera-plane guard are
the original's, line for line; the Android branches do not come (the copy is
the desktop's). The model lighting's detection (nv2a_vertex_shader_lighting)
is not copied: the device calls the original. Changes to nv2a_vsh.c are not
followed automatically. tools/d3d11_shader_check.py proves each dumped
shader's text is the original's with these changes and no other.

Each instruction is four little-endian words. Word 1 to 3 hold a MAC
(vector) operation and an ILU (scalar) operation that execute in parallel
on the same three operands A, B and C, and the destinations of both; the
bit positions of every field are spelled out in operand_fields() and in
nv2a_vertex_shader_to_glsl(). When both units run, the ILU result goes to
temporary r1, whatever the instruction's temporary register field says.

Xbox vertex programs finish by converting their clip-space position to
screen space with the viewport constants c[-38] and c[-37], which Direct3D
maintains. The generated shader inverts that transform to hand OpenGL a
clip-space position again.
*/

#include "xgpu.h"
#include "hlsl_shaders.h"

#include <stdlib.h>

/* ---------- instruction fields */

static unsigned long field(const DWORD *instruction, int word, int low_bit, int bit_count)
{
	return (instruction[word] >> low_bit) & ((1UL << bit_count) - 1);
}

enum
{
	_mac_nop, _mac_mov, _mac_mul, _mac_add, _mac_mad, _mac_dp3, _mac_dph, _mac_dp4,
	_mac_dst, _mac_min, _mac_max, _mac_slt, _mac_sge, _mac_arl,
};

enum
{
	_ilu_nop, _ilu_mov, _ilu_rcp, _ilu_rcc, _ilu_rsq, _ilu_exp, _ilu_log, _ilu_lit,
};

enum
{
	_mux_unknown, _mux_temporary, _mux_input, _mux_constant,
};

/* output register addresses (o[]) */
static const char *output_name(unsigned long address)
{
	switch (address)
	{
	case 0: return "oPos";
	case 3: return "oD0";
	case 4: return "oD1";
	case 5: return "oFog";
	case 6: return "oPts";
	case 7: return "oB0";
	case 8: return "oB1";
	case 9: return "oT0";
	case 10: return "oT1";
	case 11: return "oT2";
	case 12: return "oT3";
	default: return "oUnused";
	}
}

static void write_mask(unsigned long mask, char *out)
{
	/* bit 3 is x */
	int count = 0;

	if (mask & 8) out[count++] = 'x';
	if (mask & 4) out[count++] = 'y';
	if (mask & 2) out[count++] = 'z';
	if (mask & 1) out[count++] = 'w';
	out[count] = 0;
}

struct operand_fields
{
	unsigned long negate, swizzle[4], index, mux;
};

static void operand_fields(const DWORD *instruction, char which, struct operand_fields *fields)
{
	switch (which)
	{
	case 'A':
		fields->negate = field(instruction, 1, 8, 1);
		fields->swizzle[0] = field(instruction, 1, 6, 2);
		fields->swizzle[1] = field(instruction, 1, 4, 2);
		fields->swizzle[2] = field(instruction, 1, 2, 2);
		fields->swizzle[3] = field(instruction, 1, 0, 2);
		fields->index = field(instruction, 2, 28, 4);
		fields->mux = field(instruction, 2, 26, 2);
		break;
	case 'B':
		fields->negate = field(instruction, 2, 25, 1);
		fields->swizzle[0] = field(instruction, 2, 23, 2);
		fields->swizzle[1] = field(instruction, 2, 21, 2);
		fields->swizzle[2] = field(instruction, 2, 19, 2);
		fields->swizzle[3] = field(instruction, 2, 17, 2);
		fields->index = field(instruction, 2, 13, 4);
		fields->mux = field(instruction, 2, 11, 2);
		break;
	default:
		fields->negate = field(instruction, 2, 10, 1);
		fields->swizzle[0] = field(instruction, 2, 8, 2);
		fields->swizzle[1] = field(instruction, 2, 6, 2);
		fields->swizzle[2] = field(instruction, 2, 4, 2);
		fields->swizzle[3] = field(instruction, 2, 2, 2);
		fields->index = (field(instruction, 2, 0, 2) << 2) | field(instruction, 3, 30, 2);
		fields->mux = field(instruction, 3, 28, 2);
		break;
	}
}

static void operand(struct xgpu_text *text, const DWORD *instruction, char which, int relative)
{
	static const char swizzle_names[] = "xyzw";
	struct operand_fields fields;
	unsigned long *swizzle = fields.swizzle, index, mux;

	operand_fields(instruction, which, &fields);
	index = fields.index;
	mux = fields.mux;
	xgpu_text_append(text, "%s", fields.negate ? "-" : "");
	switch (mux)
	{
	case _mux_temporary:
		/* r12 reads back the position output */
		if (index == 12)
			xgpu_text_append(text, "oPos");
		else
			xgpu_text_append(text, "r%lu", index);
		break;
	case _mux_input:
		xgpu_text_append(text, "v%lu", field(instruction, 1, 9, 4));
		break;
	case _mux_constant:
		if (relative)
			xgpu_text_append(text, "c[clamp(a0 + %lu, 0, %d)]", field(instruction, 1, 13, 8), XGPU_VERTEX_CONSTANT_COUNT - 1);
		else
			xgpu_text_append(text, "c[%lu]", field(instruction, 1, 13, 8));
		break;
	default:
		xgpu_text_append(text, "((float4)(0.0))");
		break;
	}
	xgpu_text_append(text, ".%c%c%c%c",
		swizzle_names[swizzle[0]], swizzle_names[swizzle[1]], swizzle_names[swizzle[2]], swizzle_names[swizzle[3]]);
}

/* ---------- translation */

/* (the constant buffers and the interface's structure come first, from
hlsl_shaders.h: nv2a_hlsl_vertex_shader) */
static const char shader_prologue[] =
	HLSL_INTERFACE_STATICS
	"float4 unpack_normpacked3(uint p)\n"
	"{\n"
	"	int x = (int)(p << 21) >> 21;\n"
	"	int y = (int)(p << 10) >> 21;\n"
	"	int z = (int)(p) >> 22;\n"
	"	return float4((float)(x) / 1023.0, (float)(y) / 1023.0, (float)(z) / 511.0, 1.0);\n"
	"}\n"
	"float4 nv2a_rcc(float x)\n"
	"{\n"
	"	float r = 1.0 / x;\n"
	"	if (r > 0.0) r = clamp(r, 5.42101e-20, 1.884467e+19);\n"
	"	else r = clamp(r, -1.884467e+19, -5.42101e-20);\n"
	"	return ((float4)(r));\n"
	"}\n"
	"float4 nv2a_exp(float x)\n"
	"{\n"
	"	return float4(exp2(floor(x)), frac(x), exp2(x), 1.0);\n"
	"}\n"
	"float4 nv2a_log(float x)\n"
	"{\n"
	"	x = abs(x);\n"
	"	if (x == 0.0) return float4(-1.0e30, 1.0, -1.0e30, 1.0);\n"
	"	float e = floor(log2(x));\n"
	"	return float4(e, x / exp2(e), log2(x), 1.0);\n"
	"}\n"
	"float4 nv2a_lit(float4 s)\n"
	"{\n"
	"	float specular = s.x > 0.0 ? pow(max(s.y, 0.0), clamp(s.w, -127.9961, 127.9961)) : 0.0;\n"
	"	return float4(1.0, max(s.x, 0.0), specular, 1.0);\n"
	"}\n";

char *nv2a_hlsl_vertex_shader(const uint32_t *instruction_words, uint32_t count, uint32_t packed_attribute_mask,
	uint32_t integer_mask, uint32_t three_component_mask, const struct nv2a_vertex_lighting *lighting)
{
	/* (the original's arguments, as its body names them) */
	const DWORD *instructions = (const DWORD *)instruction_words;
	unsigned long instruction_count = count;
	struct xgpu_text text = { 0 };
	unsigned long index;

	/* the constant buffers; the interface's structure, the lit members
	after the nine */
	xgpu_text_append(&text, "%s%s", HLSL_VERTEX_CONSTANTS_TEXT, HLSL_INTERFACE_BEGIN);
	if (lighting)
		xgpu_text_append(&text, "%s", HLSL_INTERFACE_NORMAL_MEMBER);
	if (lighting && lighting->lights == 2)
		xgpu_text_append(&text, "%s", HLSL_INTERFACE_POSITION_MEMBER);
	xgpu_text_append(&text, "%s", HLSL_INTERFACE_END);
	xgpu_text_append(&text, "%s", shader_prologue);
	/* (the pixel shader's model_lighting; the normal's length in w) */
	if (lighting)
		xgpu_text_append(&text, "static float4 xWorldNormal;\n");
	if (lighting && lighting->lights == 2)
		xgpu_text_append(&text, "static float3 xWorldPosition;\n");
	xgpu_text_append(&text, "struct vs_in\n{\n");
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (packed_attribute_mask & (1UL << index))
			xgpu_text_append(&text, "\tuint v%lu_packed : TEXCOORD%lu;\n", index, index);
		else if (integer_mask & (1UL << index))
			xgpu_text_append(&text, "\tint4 v%lu_int : TEXCOORD%lu;\n", index, index);
		else
			xgpu_text_append(&text, "\tfloat4 v%lu_in : TEXCOORD%lu;\n", index, index);
	}
	xgpu_text_append(&text, "};\n");

	xgpu_text_append(&text, "vs_out main(vs_in input)\n{\n");
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (packed_attribute_mask & (1UL << index))
			xgpu_text_append(&text, "\tfloat4 v%lu = unpack_normpacked3(input.v%lu_packed);\n", index, index);
		else if (integer_mask & (1UL << index))
			xgpu_text_append(&text, "\tfloat4 v%lu = ((float4)(input.v%lu_int));\n", index, index);
		else
			xgpu_text_append(&text, "\tfloat4 v%lu = input.v%lu_in;\n", index, index);
	}
	/* (the w a three-component GL format gave: the layout reads four) */
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (three_component_mask & (1UL << index))
			xgpu_text_append(&text, "\tv%lu.w = 1.0;\n", index);
	}
	xgpu_text_append(&text,
		"\tfloat4 r0 = ((float4)(0.0)), r1 = ((float4)(0.0)), r2 = ((float4)(0.0)), r3 = ((float4)(0.0));\n"
		"\tfloat4 r4 = ((float4)(0.0)), r5 = ((float4)(0.0)), r6 = ((float4)(0.0)), r7 = ((float4)(0.0));\n"
		"\tfloat4 r8 = ((float4)(0.0)), r9 = ((float4)(0.0)), r10 = ((float4)(0.0)), r11 = ((float4)(0.0));\n"
		"\tfloat4 oPos = float4(0.0, 0.0, 0.0, 1.0);\n"
		"\tfloat4 oD0 = float4(0.0, 0.0, 0.0, 1.0), oD1 = float4(0.0, 0.0, 0.0, 1.0);\n"
		"\tfloat4 oB0 = float4(0.0, 0.0, 0.0, 1.0), oB1 = float4(0.0, 0.0, 0.0, 1.0);\n"
		"\tfloat4 oT0 = float4(0.0, 0.0, 0.0, 1.0), oT1 = float4(0.0, 0.0, 0.0, 1.0);\n"
		"\tfloat4 oT2 = float4(0.0, 0.0, 0.0, 1.0), oT3 = float4(0.0, 0.0, 0.0, 1.0);\n"
		"\tfloat4 oFog = ((float4)(1.0)), oPts = ((float4)(0.0)), oUnused = ((float4)(0.0));\n"
		"\tint a0 = 0;\n"
		"\tfloat4 A, B, C, mac, ilu;\n");
	xgpu_text_append(&text, "\tfloat4 clip_position = ((float4)(0.0));\n\tbool clip_captured = false;\n");

	for (index = 0; index < instruction_count; index++)
	{
		const DWORD *instruction = instructions + index * 4;
		unsigned long mac = field(instruction, 1, 21, 4);
		unsigned long ilu = field(instruction, 1, 25, 3);
		unsigned long mac_mask = field(instruction, 3, 24, 4);
		unsigned long temporary = field(instruction, 3, 20, 4);
		unsigned long ilu_mask = field(instruction, 3, 16, 4);
		unsigned long output_mask = field(instruction, 3, 12, 4);
		unsigned long output_is_register = field(instruction, 3, 11, 1);
		unsigned long output_address = field(instruction, 3, 3, 8);
		unsigned long output_from_ilu = field(instruction, 3, 2, 1);
		int relative = (int)field(instruction, 3, 1, 1);
		char mask[5];

		xgpu_text_append(&text, "\t/* %lu */\n", index);
		/* the lighting's normal and position, before the program reuses them */
		if (lighting && index == lighting->normal_instruction)
		{
			xgpu_text_append(&text, "\txWorldNormal = float4(r%lu.xyz, length(r%lu.xyz));\n",
				lighting->normal_register, lighting->normal_register);
		}
		if (lighting && lighting->lights == 2 && index == lighting->position_instruction)
			xgpu_text_append(&text, "\txWorldPosition = r%lu.xyz;\n", lighting->position_register);
		xgpu_text_append(&text, "\tA = "); operand(&text, instruction, 'A', relative); xgpu_text_append(&text, ";\n");
		xgpu_text_append(&text, "\tB = "); operand(&text, instruction, 'B', relative); xgpu_text_append(&text, ";\n");
		xgpu_text_append(&text, "\tC = "); operand(&text, instruction, 'C', relative); xgpu_text_append(&text, ";\n");

		switch (mac)
		{
		case _mac_nop: break;
		case _mac_mov: xgpu_text_append(&text, "\tmac = A;\n"); break;
		case _mac_mul: xgpu_text_append(&text, "\tmac = A * B;\n"); break;
		case _mac_add: xgpu_text_append(&text, "\tmac = A + C;\n"); break;
		case _mac_mad: xgpu_text_append(&text, "\tmac = A * B + C;\n"); break;
		case _mac_dp3: xgpu_text_append(&text, "\tmac = ((float4)(dot(A.xyz, B.xyz)));\n"); break;
		case _mac_dph: xgpu_text_append(&text, "\tmac = ((float4)(dot(A.xyz, B.xyz) + B.w));\n"); break;
		case _mac_dp4: xgpu_text_append(&text, "\tmac = ((float4)(dot(A, B)));\n"); break;
		case _mac_dst: xgpu_text_append(&text, "\tmac = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
		case _mac_min: xgpu_text_append(&text, "\tmac = min(A, B);\n"); break;
		case _mac_max: xgpu_text_append(&text, "\tmac = max(A, B);\n"); break;
		case _mac_slt: xgpu_text_append(&text, "\tmac = ((float4)((A < B)));\n"); break;
		case _mac_sge: xgpu_text_append(&text, "\tmac = ((float4)((A >= B)));\n"); break;
		case _mac_arl: xgpu_text_append(&text, "\tmac = A;\n"); break;
		default: xgpu_text_append(&text, "\tmac = ((float4)(0.0));\n"); break;
		}
		switch (ilu)
		{
		case _ilu_nop: break;
		case _ilu_mov: xgpu_text_append(&text, "\tilu = C;\n"); break;
		case _ilu_rcp: xgpu_text_append(&text, "\tilu = ((float4)(1.0 / C.x));\n"); break;
		case _ilu_rcc: xgpu_text_append(&text, "\tilu = nv2a_rcc(C.x);\n"); break;
		case _ilu_rsq: xgpu_text_append(&text, "\tilu = ((float4)(rsqrt(abs(C.x))));\n"); break;
		case _ilu_exp: xgpu_text_append(&text, "\tilu = nv2a_exp(C.x);\n"); break;
		case _ilu_log: xgpu_text_append(&text, "\tilu = nv2a_log(C.x);\n"); break;
		case _ilu_lit: xgpu_text_append(&text, "\tilu = nv2a_lit(C);\n"); break;
		default: xgpu_text_append(&text, "\tilu = ((float4)(0.0));\n"); break;
		}
		/* the screen-space conversion takes the reciprocal of the clip-space
		position's w (rcc of r12.w); keep the position it converts */
		if (ilu == _ilu_rcc && field(instruction, 3, 28, 2) == _mux_temporary &&
			((field(instruction, 2, 0, 2) << 2) | field(instruction, 3, 30, 2)) == 12)
		{
			xgpu_text_append(&text, "\tclip_position = oPos;\n\tclip_captured = true;\n");
		}

		/* results are written only after both units have read their inputs */
		if (mac == _mac_arl)
		{
			xgpu_text_append(&text, "\ta0 = (int)(floor(mac.x + 0.001));\n");
		}
		else if (mac != _mac_nop && mac_mask)
		{
			write_mask(mac_mask, mask);
			if (temporary == 12)
				xgpu_text_append(&text, "\toPos.%s = mac.%s;\n", mask, mask);
			else
				xgpu_text_append(&text, "\tr%lu.%s = mac.%s;\n", temporary, mask, mask);
		}
		if (ilu != _ilu_nop && ilu_mask)
		{
			unsigned long ilu_temporary = mac != _mac_nop ? 1 : temporary;

			write_mask(ilu_mask, mask);
			if (ilu_temporary == 12)
				xgpu_text_append(&text, "\toPos.%s = ilu.%s;\n", mask, mask);
			else
				xgpu_text_append(&text, "\tr%lu.%s = ilu.%s;\n", ilu_temporary, mask, mask);
		}
		if (output_mask && (output_from_ilu ? ilu : mac) != 0)
		{
			const char *source = output_from_ilu ? "ilu" : "mac";

			write_mask(output_mask, mask);
			if (output_is_register)
				xgpu_text_append(&text, "\t%s.%s = %s.%s;\n", output_name(output_address), mask, source, mask);
			/* writes to constant memory are not used by Halo's shaders */
		}
		if (field(instruction, 3, 0, 1))
			break;
	}

	xgpu_text_append(&text,
		"\t/* undo the screen-space conversion done with c[-38] and c[-37] */\n"
		"\tfloat3 scale = float3(viewport_scale.x != 0.0 ? viewport_scale.x : 1.0,\n"
		"\t\tviewport_scale.y != 0.0 ? viewport_scale.y : 1.0,\n"
		"\t\tviewport_scale.z != 0.0 ? viewport_scale.z : 1.0);\n"
		/* Direct3D 8 puts pixel centres on integer screen coordinates (the
		game offsets its screen-space quads by -0.5 to match), OpenGL on
		half-integers */
		/* The conversion is screen = clip * c[-38] * rcc(w) + c[-37]; undoing
		it by multiplying by w again is lossy near the camera plane, where
		rcc clamps and 1/w rounds differently on each GPU (Mali put vertices
		of the first-person weapon at the vanishing point). Where the clip
		position was kept, the same result is computed without dividing. */
		"\tfloat4 position;\n"
		"\tif (clip_captured)\n"
		"\t\tposition = float4((clip_position.xyz * c[%d].xyz + (c[%d].xyz + float3(0.5 + screen.x, 0.5, 0.0)\n"
		"\t\t\t- viewport_offset.xyz) * clip_position.w) / scale, clip_position.w);\n"
		"\telse\n"
		"\t{\n"
		"\t\tfloat3 ndc = (float3(oPos.xy + float2(0.5 + screen.x, 0.5), oPos.z) - viewport_offset.xyz) / scale;\n"
		"\t\tposition = float4(ndc * oPos.w, oPos.w);\n"
		"\t}\n"
		/* A position whose w is zero, or is not a number, is the clip-space
		origin: the screen conversion's reciprocal is clamped rather than
		infinite, so a large position times a w of zero is exactly zero, and
		the origin is inside the frustum. Nothing then clips the triangle
		away and OpenGL divides zero by zero there: the vertex lands on the
		middle of the screen and the triangle is drawn out to it from the
		first-person weapon, whose pose follows the camera and so reaches the
		camera plane. The divide on the Xbox sends such a vertex to infinity
		and the clipper takes the triangle; put it behind the camera instead,
		which the clipper also takes. */
		"\tif (!(abs(position.w) > 0.0))\n"
		"\t\tposition = float4(0.0, 0.0, 0.0, -1.0);\n"
		/* (Direct3D 11's clip space is desktop GL's under glClipControl:
		rows from the top, depth 0..1; it has no point size) */
		"\tvs_out output;\n"
		"\toutput.position = position;\n"
		"\txD0 = clamp(oD0, 0.0, 1.0);\n"
		"\txD1 = clamp(oD1, 0.0, 1.0);\n"
		"\txB0 = clamp(oB0, 0.0, 1.0);\n"
		"\txB1 = clamp(oB1, 0.0, 1.0);\n"
		"\txT0 = oT0;\n"
		"\txT1 = oT1;\n"
		"\txT2 = oT2;\n"
		"\txT3 = oT3;\n"
		"\txFog = oFog.x;\n"
		HLSL_INTERFACE_OUTPUT,
		XGPU_VERTEX_CONSTANT_BIAS - 38, XGPU_VERTEX_CONSTANT_BIAS - 37
		);
	if (lighting)
		xgpu_text_append(&text, "%s", HLSL_INTERFACE_NORMAL_OUTPUT);
	if (lighting && lighting->lights == 2)
		xgpu_text_append(&text, "%s", HLSL_INTERFACE_POSITION_OUTPUT);
	xgpu_text_append(&text, "%s}\n", HLSL_INTERFACE_RETURN);
	return text.buffer;
}
