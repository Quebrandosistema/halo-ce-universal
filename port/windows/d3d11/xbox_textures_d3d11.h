/*
XBOX_TEXTURES_D3D11.H

What the Direct3D 11 renderer's device (d3d8_d3d11.c) calls of its copy of
the texture cache (xbox_textures_d3d11.c, port/windows/D3D11.md, phase 6):
the two functions d3d8_gl.c called of xbox_textures.c, with the backend's
handle in place of the GL name. The Xbox SDK's side: the exports every build
shares (xgpu_texture_describe and the rest) stay declared in xgpu.h.
*/

#ifndef XBOX_TEXTURES_D3D11_H
#define XBOX_TEXTURES_D3D11_H

#include "xgpu.h"

#include <stdint.h>

/* what xgpu_d3d11_texture_get answers for an Xbox texture header: the
backend's texture (d3d11_backend.h; 0 if it could not be made, which the
draw samples as zero), its kind (D3D11_BACKEND_TEXTURE_KIND_2D, _3D, _CUBE)
and its description, with the high-res flags where a high-res texture is
drawn in its place (hud_hires.h) */
struct xgpu_d3d11_texture
{
	uint32_t handle;
	uint32_t kind;
	struct xgpu_texture_description description;
};

/* the texture for an Xbox texture header, uploaded or refreshed from the
game's memory as needed (xgpu_texture_get's) */
void xgpu_d3d11_texture_get(const DWORD *resource, const D3DCOLOR *palette, struct xgpu_d3d11_texture *result);
/* once a frame: the textures not used for a while dropped
(xgpu_texture_cache_begin_frame's) */
void xgpu_d3d11_texture_cache_begin_frame(void);

#endif
