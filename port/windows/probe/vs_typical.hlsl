/* vs004_1.glsl from the dump of build d071462e (debug.gpu_dump_shaders), converted to HLSL for the
Direct3D 11 probe's compile step (port/windows/D3D11.md, phase 0, step 3): only what HLSL needs is
changed, the body is kept. */

cbuffer vertex_constants : register(b0)
{
	float4 c[192];
	float4 viewport_scale;
	float4 viewport_offset;
	float4 loose_floats; /* point_size, screen_offset */
};
#define point_size loose_floats.x
#define screen_offset loose_floats.y
struct stage_link
{
	float4 position : SV_Position;
	float4 xD0 : TEXCOORD0;
	float4 xD1 : TEXCOORD1;
	float4 xB0 : TEXCOORD2;
	float4 xB1 : TEXCOORD3;
	float4 xT0 : TEXCOORD4;
	float4 xT1 : TEXCOORD5;
	float4 xT2 : TEXCOORD6;
	float4 xT3 : TEXCOORD7;
	float xFog : TEXCOORD8;
};
struct vertex_in
{
	float4 v0_in : TEXCOORD0;
	float4 v1_in : TEXCOORD1;
	float4 v2_in : TEXCOORD2;
	float4 v3_in : TEXCOORD3;
	float4 v4_in : TEXCOORD4;
	float4 v5_in : TEXCOORD5;
	float4 v6_in : TEXCOORD6;
	float4 v7_in : TEXCOORD7;
	float4 v8_in : TEXCOORD8;
	float4 v9_in : TEXCOORD9;
	float4 v10_in : TEXCOORD10;
	float4 v11_in : TEXCOORD11;
	float4 v12_in : TEXCOORD12;
	float4 v13_in : TEXCOORD13;
	float4 v14_in : TEXCOORD14;
	float4 v15_in : TEXCOORD15;
};
static float4 v0_in;
static float4 v1_in;
static float4 v2_in;
static float4 v3_in;
static float4 v4_in;
static float4 v5_in;
static float4 v6_in;
static float4 v7_in;
static float4 v8_in;
static float4 v9_in;
static float4 v10_in;
static float4 v11_in;
static float4 v12_in;
static float4 v13_in;
static float4 v14_in;
static float4 v15_in;
static float4 xD0;
static float4 xD1;
static float4 xB0;
static float4 xB1;
static float4 xT0;
static float4 xT1;
static float4 xT2;
static float4 xT3;
static float xFog;
static float4 position_out;
float4 unpack_normpacked3(uint p)
{
	int x = int(p << 21) >> 21;
	int y = int(p << 10) >> 21;
	int z = int(p) >> 22;
	return float4(float(x) / 1023.0, float(y) / 1023.0, float(z) / 511.0, 1.0);
}
float4 nv2a_rcc(float x)
{
	float r = 1.0 / x;
	if (r > 0.0) r = clamp(r, 5.42101e-20, 1.884467e+19);
	else r = clamp(r, -1.884467e+19, -5.42101e-20);
	return ((float4)(r));
}
float4 nv2a_exp(float x)
{
	return float4(exp2(floor(x)), frac(x), exp2(x), 1.0);
}
float4 nv2a_log(float x)
{
	x = abs(x);
	if (x == 0.0) return float4(-1.0e30, 1.0, -1.0e30, 1.0);
	float e = floor(log2(x));
	return float4(e, x / exp2(e), log2(x), 1.0);
}
float4 nv2a_lit(float4 s)
{
	float specular = s.x > 0.0 ? pow(max(s.y, 0.0), clamp(s.w, -127.9961, 127.9961)) : 0.0;
	return float4(1.0, max(s.x, 0.0), specular, 1.0);
}
void glsl_main()
{
	float4 v0 = v0_in;
	float4 v1 = v1_in;
	float4 v2 = v2_in;
	float4 v3 = v3_in;
	float4 v4 = v4_in;
	float4 v5 = v5_in;
	float4 v6 = v6_in;
	float4 v7 = v7_in;
	float4 v8 = v8_in;
	float4 v9 = v9_in;
	float4 v10 = v10_in;
	float4 v11 = v11_in;
	float4 v12 = v12_in;
	float4 v13 = v13_in;
	float4 v14 = v14_in;
	float4 v15 = v15_in;
	float4 r0 = ((float4)(0.0)), r1 = ((float4)(0.0)), r2 = ((float4)(0.0)), r3 = ((float4)(0.0));
	float4 r4 = ((float4)(0.0)), r5 = ((float4)(0.0)), r6 = ((float4)(0.0)), r7 = ((float4)(0.0));
	float4 r8 = ((float4)(0.0)), r9 = ((float4)(0.0)), r10 = ((float4)(0.0)), r11 = ((float4)(0.0));
	float4 oPos = float4(0.0, 0.0, 0.0, 1.0);
	float4 oD0 = float4(0.0, 0.0, 0.0, 1.0), oD1 = float4(0.0, 0.0, 0.0, 1.0);
	float4 oB0 = float4(0.0, 0.0, 0.0, 1.0), oB1 = float4(0.0, 0.0, 0.0, 1.0);
	float4 oT0 = float4(0.0, 0.0, 0.0, 1.0), oT1 = float4(0.0, 0.0, 0.0, 1.0);
	float4 oT2 = float4(0.0, 0.0, 0.0, 1.0), oT3 = float4(0.0, 0.0, 0.0, 1.0);
	float4 oFog = ((float4)(1.0)), oPts = ((float4)(point_size)), oUnused = ((float4)(0.0));
	int a0 = 0;
	float4 A, B, C, mac, ilu;
	float4 clip_position = ((float4)(0.0));
	bool clip_captured = false;
	/* 0 */
	A = v0.xyzw;
	B = c[28].xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	oPos.x = mac.x;
	/* 1 */
	A = v0.xyzw;
	B = c[29].xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	oPos.y = mac.y;
	/* 2 */
	A = v0.xyzw;
	B = c[30].xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	oPos.z = mac.z;
	/* 3 */
	A = v0.xyzw;
	B = c[31].xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	oPos.w = mac.w;
	/* 4 */
	A = v9.xyzw;
	B = v9.xyzw;
	C = v9.xyzw;
	mac = A;
	oD0.xyzw = mac.xyzw;
	/* 5 */
	A = c[34].xxxx;
	B = v0.xyyy;
	C = v0.xyzw;
	mac = A * B;
	r10.xy = mac.xy;
	/* 6 */
	A = v4.xyzw;
	B = c[34].yyyy;
	C = r10.xyyy;
	mac = A * B + C;
	r10.xy = mac.xy;
	/* 7 */
	A = oPos.xyzw;
	B = c[58].xyzw;
	C = oPos.wwww;
	mac = A * B;
	ilu = nv2a_rcc(C.x);
	clip_position = oPos;
	clip_captured = true;
	r1.x = ilu.x;
	oPos.xyz = mac.xyz;
	/* 8 */
	A = r10.xyyy;
	B = c[37].xyyy;
	C = v0.xyzw;
	mac = A * B;
	r10.xy = mac.xy;
	/* 9 */
	A = r10.xyyy;
	B = v0.xyzw;
	C = c[35].zwww;
	mac = A + C;
	r10.xy = mac.xy;
	/* 10 */
	A = oPos.xyzw;
	B = r1.xxxx;
	C = c[59].xyzw;
	mac = A * B + C;
	oPos.xyz = mac.xyz;
	/* 11 */
	A = r10.xyyy;
	B = c[32].xyyy;
	C = v0.xyzw;
	mac = A * B;
	oT0.xy = mac.xy;
	/* 12 */
	A = c[34].zzzz;
	B = v0.xyyy;
	C = v0.xyzw;
	mac = A * B;
	r10.xy = mac.xy;
	/* 13 */
	A = v4.xyzw;
	B = c[34].wwww;
	C = r10.xyyy;
	mac = A * B + C;
	r10.xy = mac.xy;
	/* 14 */
	A = r10.xyyy;
	B = c[37].zwww;
	C = v0.xyzw;
	mac = A * B;
	r10.xy = mac.xy;
	/* 15 */
	A = r10.xyyy;
	B = v0.xyzw;
	C = c[36].xyyy;
	mac = A + C;
	r10.xy = mac.xy;
	/* 16 */
	A = r10.xyyy;
	B = c[33].xyyy;
	C = v0.xyzw;
	mac = A * B;
	oT1.xy = mac.xy;
	/* 17 */
	A = c[35].xxxx;
	B = v0.xyyy;
	C = v0.xyzw;
	mac = A * B;
	r10.xy = mac.xy;
	/* 18 */
	A = v4.xyzw;
	B = c[35].yyyy;
	C = r10.xyyy;
	mac = A * B + C;
	r10.xy = mac.xy;
	/* 19 */
	A = r10.xyyy;
	B = c[38].xyyy;
	C = v0.xyzw;
	mac = A * B;
	r10.xy = mac.xy;
	/* 20 */
	A = r10.xyyy;
	B = v0.xyzw;
	C = c[36].zwww;
	mac = A + C;
	r10.xy = mac.xy;
	/* 21 */
	A = r10.xyyy;
	B = c[33].zwww;
	C = v0.xyzw;
	mac = A * B;
	oT2.xy = mac.xy;
	/* undo the screen-space conversion done with c[-38] and c[-37] */
	float3 scale = float3(viewport_scale.x != 0.0 ? viewport_scale.x : 1.0,
		viewport_scale.y != 0.0 ? viewport_scale.y : 1.0,
		viewport_scale.z != 0.0 ? viewport_scale.z : 1.0);
	float4 position;
	if (clip_captured)
		position = float4((clip_position.xyz * c[58].xyz + (c[59].xyz + float3(0.5 + screen_offset, 0.5, 0.0)
			- viewport_offset.xyz) * clip_position.w) / scale, clip_position.w);
	else
	{
		float3 ndc = (float3(oPos.xy + float2(0.5 + screen_offset, 0.5), oPos.z) - viewport_offset.xyz) / scale;
		position = float4(ndc * oPos.w, oPos.w);
	}
	if (!(abs(position.w) > 0.0))
		position = float4(0.0, 0.0, 0.0, -1.0);
	position_out = position;
		xD0 = clamp(oD0, 0.0, 1.0);
	xD1 = clamp(oD1, 0.0, 1.0);
	xB0 = clamp(oB0, 0.0, 1.0);
	xB1 = clamp(oB1, 0.0, 1.0);
	xT0 = oT0;
	xT1 = oT1;
	xT2 = oT2;
	xT3 = oT3;
	xFog = oFog.x;
}

stage_link main(vertex_in input)
{
	stage_link link;
	v0_in = input.v0_in;
	v1_in = input.v1_in;
	v2_in = input.v2_in;
	v3_in = input.v3_in;
	v4_in = input.v4_in;
	v5_in = input.v5_in;
	v6_in = input.v6_in;
	v7_in = input.v7_in;
	v8_in = input.v8_in;
	v9_in = input.v9_in;
	v10_in = input.v10_in;
	v11_in = input.v11_in;
	v12_in = input.v12_in;
	v13_in = input.v13_in;
	v14_in = input.v14_in;
	v15_in = input.v15_in;
	glsl_main();
	link.position = position_out;
	link.xD0 = xD0;
	link.xD1 = xD1;
	link.xB0 = xB0;
	link.xB1 = xB1;
	link.xT0 = xT0;
	link.xT1 = xT1;
	link.xT2 = xT2;
	link.xT3 = xT3;
	link.xFog = xFog;
	return link;
}
