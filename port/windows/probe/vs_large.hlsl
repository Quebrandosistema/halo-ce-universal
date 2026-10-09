/* vs058_0.glsl from the dump of build d071462e (debug.gpu_dump_shaders), converted to HLSL for the
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
	uint v1_packed : TEXCOORD1;
	uint v2_packed : TEXCOORD2;
	uint v3_packed : TEXCOORD3;
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
static uint v1_packed;
static uint v2_packed;
static uint v3_packed;
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
	float4 v1 = unpack_normpacked3(v1_packed);
	float4 v2 = unpack_normpacked3(v2_packed);
	float4 v3 = unpack_normpacked3(v3_packed);
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
	A = v5.xyzw;
	B = c[7].wwww;
	C = v5.xyzw;
	mac = A * B;
	r2.xy = mac.xy;
	/* 1 */
	A = r2.xxxx;
	B = v0.xyzw;
	C = v0.xyzw;
	mac = A;
	a0 = int(floor(mac.x + 0.001));
	/* 2 */
	A = v6.xxxx;
	B = c[clamp(a0 + 60, 0, 191)].xyzw;
	C = v6.xyzw;
	mac = A * B;
	r3.xyzw = mac.xyzw;
	/* 3 */
	A = v6.xxxx;
	B = c[clamp(a0 + 61, 0, 191)].xyzw;
	C = v6.xyzw;
	mac = A * B;
	r4.xyzw = mac.xyzw;
	/* 4 */
	A = v6.xxxx;
	B = c[clamp(a0 + 62, 0, 191)].xyzw;
	C = v6.xyzw;
	mac = A * B;
	r5.xyzw = mac.xyzw;
	/* 5 */
	A = r2.yyyy;
	B = v0.xyzw;
	C = v0.xyzw;
	mac = A;
	a0 = int(floor(mac.x + 0.001));
	/* 6 */
	A = v6.wwww;
	B = v6.xyzw;
	C = -v6.xxxx;
	mac = A + C;
	r6.x = mac.x;
	/* 7 */
	A = v4.xyzw;
	B = c[17].xyzw;
	C = v4.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	oT1.x = mac.x;
	/* 8 */
	A = r6.xxxx;
	B = c[clamp(a0 + 60, 0, 191)].xyzw;
	C = r3.xyzw;
	mac = A * B + C;
	r7.xyzw = mac.xyzw;
	/* 9 */
	A = r6.xxxx;
	B = c[clamp(a0 + 61, 0, 191)].xyzw;
	C = r4.xyzw;
	mac = A * B + C;
	r8.xyzw = mac.xyzw;
	/* 10 */
	A = r6.xxxx;
	B = c[clamp(a0 + 62, 0, 191)].xyzw;
	C = r5.xyzw;
	mac = A * B + C;
	r9.xyzw = mac.xyzw;
	/* 11 */
	A = v0.xyzw;
	B = r7.xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	r10.x = mac.x;
	/* 12 */
	A = v0.xyzw;
	B = r8.xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	r10.y = mac.y;
	/* 13 */
	A = v0.xyzw;
	B = r9.xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	r10.z = mac.z;
	/* 14 */
	A = v1.xyzw;
	B = r7.xyzw;
	C = v1.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz)));
	r11.x = mac.x;
	/* 15 */
	A = v1.xyzw;
	B = r8.xyzw;
	C = v1.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz)));
	r11.y = mac.y;
	/* 16 */
	A = v1.xyzw;
	B = r9.xyzw;
	C = v1.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz)));
	r11.z = mac.z;
	/* 17 */
	A = -r10.xyzw;
	B = v0.xyzw;
	C = c[4].xyzw;
	mac = A + C;
	r3.xyz = mac.xyz;
	/* 18 */
	A = r11.xyzw;
	B = r11.xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz)));
	r0.w = mac.w;
	/* 19 */
	A = r10.xyzw;
	B = c[1].xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	oPos.y = mac.y;
	/* 20 */
	A = r10.xyzw;
	B = c[0].xyzw;
	C = r0.wwww;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	ilu = ((float4)(rsqrt(abs(C.x))));
	r1.w = ilu.w;
	oPos.x = mac.x;
	/* 21 */
	A = r10.xyzw;
	B = c[2].xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	oPos.z = mac.z;
	/* 22 */
	A = r11.xyzw;
	B = r1.wwww;
	C = v0.xyzw;
	mac = A * B;
	r2.xyz = mac.xyz;
	/* 23 */
	A = r3.xyzw;
	B = r2.xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz)));
	r4.x = mac.x;
	/* 24 */
	A = r10.xyzw;
	B = c[3].xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	oPos.w = mac.w;
	/* 25 */
	A = r4.xxxx;
	B = r2.xyzw;
	C = v0.xyzw;
	mac = A * B;
	r5.xyz = mac.xyz;
	/* 26 */
	A = r5.xyzz;
	B = c[4].wwww;
	C = -r3.xyzw;
	mac = A * B + C;
	r6.xyz = mac.xyz;
	/* 27 */
	A = c[16].xyzw;
	B = v0.xyzw;
	C = v0.xyzw;
	mac = A;
	r7.xyz = mac.xyz;
	/* 28 */
	A = c[15].yzxw;
	B = r7.zxyw;
	C = v0.xyzw;
	mac = A * B;
	r8.xyz = mac.xyz;
	/* 29 */
	A = -r7.yzxw;
	B = c[15].zxyw;
	C = r8.xyzw;
	mac = A * B + C;
	r9.xyz = mac.xyz;
	/* 30 */
	A = r6.xyzw;
	B = c[15].xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz)));
	oT0.x = mac.x;
	/* 31 */
	A = r6.xyzw;
	B = c[16].xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz)));
	oT0.y = mac.y;
	/* 32 */
	A = r6.xyzw;
	B = r9.xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz)));
	oT0.z = mac.z;
	/* 33 */
	A = v4.xyzw;
	B = c[18].xyzw;
	C = v4.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	oT1.y = mac.y;
	/* 34 */
	A = v4.xyzw;
	B = c[19].xyzw;
	C = v4.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	oT2.x = mac.x;
	/* 35 */
	A = v4.xyzw;
	B = c[20].xyzw;
	C = v4.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	oT2.y = mac.y;
	/* 36 */
	A = v4.xyzw;
	B = c[21].xyzw;
	C = v4.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	oT3.x = mac.x;
	/* 37 */
	A = v4.xyzw;
	B = c[22].xyzw;
	C = v4.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	oT3.y = mac.y;
	/* 38 */
	A = r10.xyzw;
	B = c[9].xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	r11.x = mac.x;
	/* 39 */
	A = r10.xyzw;
	B = c[10].xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	r11.y = mac.y;
	/* 40 */
	A = r10.xyzw;
	B = c[8].xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz) + B.w));
	r0.z = mac.z;
	/* 41 */
	A = v4.wwww;
	B = v4.xyzw;
	C = -r11.xyzw;
	mac = A + C;
	r0.xy = mac.xy;
	/* 42 */
	A = r2.xyzw;
	B = -c[5].xyzw;
	C = v0.xyzw;
	mac = ((float4)(dot(A.xyz, B.xyz)));
	r11.x = mac.x;
	/* 43 */
	A = r0.xyzw;
	B = v4.zzzz;
	C = v4.xyzw;
	mac = max(A, B);
	r3.xyz = mac.xyz;
	/* 44 */
	A = r3.xyzw;
	B = r3.xyzw;
	C = v0.xyzw;
	mac = A * B;
	r3.xy = mac.xy;
	/* 45 */
	A = r3.xyzw;
	B = v4.wwww;
	C = v4.xyzw;
	mac = min(A, B);
	r4.xyz = mac.xyz;
	/* 46 */
	A = r4.xxxx;
	B = v0.xyzw;
	C = r4.yyyy;
	mac = A + C;
	r3.x = mac.x;
	/* 47 */
	A = r4.zzzz;
	B = c[11].xxxx;
	C = v0.xyzw;
	mac = A * B;
	r8.z = mac.z;
	/* 48 */
	A = r3.xyzw;
	B = v4.wwww;
	C = v4.xyzw;
	mac = min(A, B);
	r4.x = mac.x;
	/* 49 */
	A = v4.wwww;
	B = v4.xyzw;
	C = -r4.xyzw;
	mac = A + C;
	r5.xy = mac.xy;
	/* 50 */
	A = r11.xxxx;
	B = -r11.xxxx;
	C = v0.xyzw;
	mac = max(A, B);
	r0.x = mac.x;
	/* 51 */
	A = r5.xyzw;
	B = r5.xyzw;
	C = v0.xyzw;
	mac = A * B;
	r6.xy = mac.xy;
	/* 52 */
	A = r6.yyyy;
	B = v0.xyzw;
	C = -r6.xxxx;
	mac = A + C;
	r7.y = mac.y;
	/* 53 */
	A = v4.wwww;
	B = v4.xyzw;
	C = -r0.xxxx;
	mac = A + C;
	r0.y = mac.y;
	/* 54 */
	A = c[11].yyyy;
	B = r7.yyyy;
	C = r6.xxxx;
	mac = A * B + C;
	r7.w = mac.w;
	/* 55 */
	A = oPos.xyzw;
	B = c[58].xyzw;
	C = oPos.wwww;
	mac = A * B;
	ilu = nv2a_rcc(C.x);
	clip_position = oPos;
	clip_captured = true;
	r1.x = ilu.x;
	oPos.xyz = mac.xyz;
	/* 56 */
	A = r7.wwww;
	B = c[11].zzzz;
	C = v0.xyzw;
	mac = A * B;
	r8.w = mac.w;
	/* 57 */
	A = -r8.xyzw;
	B = v4.xyzw;
	C = v4.wwww;
	mac = A + C;
	r9.zw = mac.zw;
	/* 58 */
	A = oPos.xyzw;
	B = r1.xxxx;
	C = c[59].xyzw;
	mac = A * B + C;
	oPos.xyz = mac.xyz;
	/* 59 */
	A = r9.zzzz;
	B = r9.wwww;
	C = v0.xyzw;
	mac = A * B;
	r10.w = mac.w;
	/* 60 */
	A = r0.xyzw;
	B = r10.wwww;
	C = v0.xyzw;
	mac = A * B;
	r2.xy = mac.xy;
	/* 61 */
	A = r10.wwww;
	B = c[14].zzzz;
	C = v0.xyzw;
	mac = A * B;
	oD0.w = mac.w;
	/* 62 */
	A = r2.xxxy;
	B = c[14].zzzz;
	C = v0.xyzw;
	mac = A * B;
	oD1.xyzw = mac.xyzw;
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
	v1_packed = input.v1_packed;
	v2_packed = input.v2_packed;
	v3_packed = input.v3_packed;
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
