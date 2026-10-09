/* ps_71d6aefb.glsl from the dump of build d071462e (debug.gpu_dump_shaders), converted to HLSL for the
Direct3D 11 probe's compile step (port/windows/D3D11.md, phase 0, step 3): only what HLSL needs is
changed, the body is kept. */

cbuffer pixel_constants : register(b0)
{
	float4 ps_c0[8];
	float4 ps_c1[8];
	float4 ps_final_c0;
	float4 ps_final_c1;
	float4 fog_color;
	float4 fog_parameters;
	float4 bump_matrix[4];
	float4 bump_luminance[4];
	float4 texture_scale[4];
	float4 loose_floats; /* alpha_reference */
};
#define alpha_reference loose_floats.x
Texture2D tex0 : register(t0);
SamplerState tex0_sampler : register(s0);
Texture2D tex1 : register(t1);
SamplerState tex1_sampler : register(s1);
Texture2D tex2 : register(t2);
SamplerState tex2_sampler : register(s2);
Texture2D tex3 : register(t3);
SamplerState tex3_sampler : register(s3);
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
static float4 xD0;
static float4 xD1;
static float4 xB0;
static float4 xB1;
static float4 xT0;
static float4 xT1;
static float4 xT2;
static float4 xT3;
static float xFog;
static float4 fragment_color;
float signed_byte(float x)
{
	float b = floor(x * 255.0 + 0.5);
	return (b >= 128.0 ? b - 256.0 : b) / 127.0;
}
float3 signed_bytes(float3 x)
{
	return float3(signed_byte(x.r), signed_byte(x.g), signed_byte(x.b));
}
void glsl_main()
{
	float4 v0 = xD0;
	float4 v1 = xD1;
	float4 t0 = ((float4)(0.0)), t1 = ((float4)(0.0)), t2 = ((float4)(0.0)), t3 = ((float4)(0.0));
	float dot0 = 0.0, dot1 = 0.0, dot2 = 0.0, dot3 = 0.0;
	/* texture stage 0, mode 1 */
	t0 = tex0.Sample(tex0_sampler, (float4(xT0.xyz / (xT0.w != 0.0 ? xT0.w : 1.0), 1.0)).xy * texture_scale[0].xy);
	/* texture stage 1, mode 0 */
	t1 = ((float4)(0.0));
	/* texture stage 2, mode 0 */
	t2 = ((float4)(0.0));
	/* texture stage 3, mode 0 */
	t3 = ((float4)(0.0));
	float fog_factor = xFog;
	float4 fog = float4(fog_color.rgb, clamp(fog_factor, 0.0, 1.0));
	float4 r0 = float4(0.0, 0.0, 0.0, t0.a);
	float4 r1 = ((float4)(0.0));
	/* combiner stage 0 */
	{
		float3 cA = max(t0.rgb, 0.0);
		float3 cB = (1.0 - clamp(((float4)(0.0)).rgb, 0.0, 1.0));
		float3 cC = max(((float4)(0.0)).rgb, 0.0);
		float3 cD = max(((float4)(0.0)).rgb, 0.0);
		float3 cAB = cA * cB;
		float3 cCD = cC * cD;
		float3 cSUM = cAB + cCD;
		cAB = clamp((cAB), -1.0, 1.0);
		cCD = clamp((cCD), -1.0, 1.0);
		cSUM = clamp((cSUM), -1.0, 1.0);
		float aA = max(t0.a, 0.0);
		float aB = (1.0 - clamp(((float4)(0.0)).b, 0.0, 1.0));
		float aC = max(((float4)(0.0)).b, 0.0);
		float aD = max(((float4)(0.0)).b, 0.0);
		float aAB = aA * aB;
		float aCD = aC * aD;
		float aSUM = aAB + aCD;
		aAB = clamp((aAB), -1.0, 1.0);
		aCD = clamp((aCD), -1.0, 1.0);
		aSUM = clamp((aSUM), -1.0, 1.0);
		r0.rgb = cAB;
		r0.a = aAB;
	}
	/* combiner stage 1 */
	{
		float3 cA = (1.0 - clamp(((float3)(ps_c0[1].a)), 0.0, 1.0));
		float3 cB = max(r0.rgb, 0.0);
		float3 cC = max(((float3)(ps_c0[1].a)), 0.0);
		float3 cD = max(ps_c0[1].rgb, 0.0);
		float3 cAB = cA * cB;
		float3 cCD = cC * cD;
		float3 cSUM = cAB + cCD;
		cAB = clamp((cAB), -1.0, 1.0);
		cCD = clamp((cCD), -1.0, 1.0);
		cSUM = clamp((cSUM), -1.0, 1.0);
		float aA = max(((float4)(0.0)).b, 0.0);
		float aB = max(((float4)(0.0)).b, 0.0);
		float aC = max(((float4)(0.0)).b, 0.0);
		float aD = max(((float4)(0.0)).b, 0.0);
		float aAB = aA * aB;
		float aCD = aC * aD;
		float aSUM = aAB + aCD;
		aAB = clamp((aAB), -1.0, 1.0);
		aCD = clamp((aCD), -1.0, 1.0);
		aSUM = clamp((aSUM), -1.0, 1.0);
		r0.rgb = cSUM;
	}
	float4 ef_product = float4(clamp(((float4)(0.0)).rgb, 0.0, 1.0) * clamp(((float4)(0.0)).rgb, 0.0, 1.0), 0.0);
	float4 v1r0_sum = float4(clamp(v1.rgb, 0.0, 1.0) + clamp(r0.rgb, 0.0, 1.0), 0.0);
	float3 fA = clamp(((float4)(0.0)).rgb, 0.0, 1.0);
	float3 fB = clamp(((float4)(0.0)).rgb, 0.0, 1.0);
	float3 fC = clamp(((float4)(0.0)).rgb, 0.0, 1.0);
	float3 fD = clamp(r0.rgb, 0.0, 1.0);
	float fG = clamp(r0.a, 0.0, 1.0);
	float4 result = float4(fA * fB + (1.0 - fA) * fC + fD, fG);
	fragment_color = clamp(result, 0.0, 1.0);
}

float4 main(stage_link link) : SV_Target
{
	xD0 = link.xD0;
	xD1 = link.xD1;
	xB0 = link.xB0;
	xB1 = link.xB1;
	xT0 = link.xT0;
	xT1 = link.xT1;
	xT2 = link.xT2;
	xT3 = link.xT3;
	xFog = link.xFog;
	glsl_main();
	return fragment_color;
}
