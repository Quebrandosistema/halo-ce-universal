/*
WIN32_D3D11_TEXTURES.C

The Direct3D 11 backend's textures (port/windows/D3D11.md, phase 6, step 2):
the game's textures made and written through the device's copy of the
texture cache (xbox_textures_d3d11.c), their shader resource views, the mip
composites. Step 1 has what the draw unit links of it: no texture exists
yet, and a stage of kind cache or composite reads zero.
*/

#include <string.h>

#include "win32_d3d11.h"

ID3D11ShaderResourceView *d3d11_backend_texture_view(uint32_t handle)
{
	(void)handle;
	return NULL;
}

ID3D11ShaderResourceView *d3d11_backend_composite_view(uint32_t handle)
{
	(void)handle;
	return NULL;
}

void d3d11_backend_textures_statistics(uint32_t counts[D3D11_BACKEND_DRAW_STATISTICS])
{
	(void)counts;
}

void d3d11_backend_textures_release(void)
{
}
