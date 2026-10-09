/* The trivial pair's pixel shader (written for the Direct3D 11 probe's compile step, port/windows/D3D11.md,
phase 0, step 3): the compiler's fixed cost. */

struct stage_link
{
	float4 position : SV_Position;
	float4 xD0 : TEXCOORD0;
};

float4 main(stage_link link) : SV_Target
{
	return link.xD0;
}
