/* The trivial pair's vertex shader (written for the Direct3D 11 probe's compile step, port/windows/D3D11.md,
phase 0, step 3): the compiler's fixed cost. */

struct vertex_in
{
	float4 position : TEXCOORD0;
	float4 colour : TEXCOORD1;
};

struct stage_link
{
	float4 position : SV_Position;
	float4 xD0 : TEXCOORD0;
};

stage_link main(vertex_in input)
{
	stage_link link;

	link.position = input.position;
	link.xD0 = input.colour;
	return link;
}
