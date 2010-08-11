#include "util/u_format.h"

#include "nvfx_context.h"
#include "nouveau/nouveau_util.h"
#include "nvfx_tex.h"
#include "nvfx_resource.h"

void
nv30_sampler_state_init(struct pipe_context *pipe,
			  struct nvfx_sampler_state *ps,
			  const struct pipe_sampler_state *cso)
{
	if (cso->max_anisotropy >= 8) {
		ps->en |= NV34TCL_TX_ENABLE_ANISO_8X;
	} else
	if (cso->max_anisotropy >= 4) {
		ps->en |= NV34TCL_TX_ENABLE_ANISO_4X;
	} else
	if (cso->max_anisotropy >= 2) {
		ps->en |= NV34TCL_TX_ENABLE_ANISO_2X;
	}

	{
		float limit;

		limit = CLAMP(cso->lod_bias, -16.0, 15.0);
		ps->filt |= (int)(cso->lod_bias * 256.0) & 0x1fff;

		limit = CLAMP(cso->max_lod, 0.0, 15.0);
		ps->en |= (int)(limit) << 14 /*NV34TCL_TX_ENABLE_MIPMAP_MAX_LOD_SHIFT*/;

		limit = CLAMP(cso->min_lod, 0.0, 15.0);
		ps->en |= (int)(limit) << 26 /*NV34TCL_TX_ENABLE_MIPMAP_MIN_LOD_SHIFT*/;
	}
}

#define FMT_FLAG_DEPTH 1

#define _(m,tf,ts0x,ts0y,ts0z,ts0w,ts1x,ts1y,ts1z,ts1w,f)                        \
[PIPE_FORMAT_##m] = {                                                                              \
  NV34TCL_TX_FORMAT_FORMAT_##tf,                                               \
  NV34TCL_TX_FORMAT_FORMAT_##tf##_RECT,                                               \
  (NV34TCL_TX_SWIZZLE_S0_X_##ts0x | NV34TCL_TX_SWIZZLE_S0_Y_##ts0y |           \
   NV34TCL_TX_SWIZZLE_S0_Z_##ts0z | NV34TCL_TX_SWIZZLE_S0_W_##ts0w |           \
   NV34TCL_TX_SWIZZLE_S1_X_##ts1x | NV34TCL_TX_SWIZZLE_S1_Y_##ts1y |           \
   NV34TCL_TX_SWIZZLE_S1_Z_##ts1z | NV34TCL_TX_SWIZZLE_S1_W_##ts1w),           \
   f \
}

struct nv30_texture_format {
	int     format;
	int     rect_format;
	int     swizzle;
	unsigned flags;
};

#define NV34TCL_TX_FORMAT_FORMAT_DXT1_RECT NV34TCL_TX_FORMAT_FORMAT_DXT1
#define NV34TCL_TX_FORMAT_FORMAT_DXT3_RECT NV34TCL_TX_FORMAT_FORMAT_DXT3
#define NV34TCL_TX_FORMAT_FORMAT_DXT5_RECT NV34TCL_TX_FORMAT_FORMAT_DXT5

#define NV34TCL_TX_FORMAT_FORMAT_Z24 0x2a00
#define NV34TCL_TX_FORMAT_FORMAT_Z16 0x2c00

// XXX: this is an untested guess, find the right values
#define NV34TCL_TX_FORMAT_FORMAT_Z24_RECT 0x2b00
#define NV34TCL_TX_FORMAT_FORMAT_Z16_RECT 0x2d00

static struct nv30_texture_format
nv30_texture_formats[PIPE_FORMAT_COUNT] = {
	[0 ... PIPE_FORMAT_COUNT - 1] = {-1, 0, 0},
	_(B8G8R8X8_UNORM, A8R8G8B8,   S1,   S1,   S1,  ONE, X, Y, Z, W, 0),
	_(B8G8R8A8_UNORM, A8R8G8B8,   S1,   S1,   S1,   S1, X, Y, Z, W, 0),
	_(B5G5R5A1_UNORM, A1R5G5B5,   S1,   S1,   S1,   S1, X, Y, Z, W, 0),
	_(B4G4R4A4_UNORM, A4R4G4B4,   S1,   S1,   S1,   S1, X, Y, Z, W, 0),
	_(B5G6R5_UNORM  , R5G6B5  ,   S1,   S1,   S1,  ONE, X, Y, Z, W, 0),
	_(L8_UNORM      , L8      ,   S1,   S1,   S1,  ONE, X, X, X, X, 0),
	_(A8_UNORM      , L8      , ZERO, ZERO, ZERO,   S1, X, X, X, X, 0),
	_(I8_UNORM      , L8      ,   S1,   S1,   S1,   S1, X, X, X, X, 0),
	_(L8A8_UNORM    , A8L8    ,   S1,   S1,   S1,   S1, X, X, X, Y, 0),
	_(Z16_UNORM     , Z16     ,   S1,   S1,   S1,  ONE, W, W, W, W, FMT_FLAG_DEPTH),
	_(S8_USCALED_Z24_UNORM,Z24,   S1,   S1,   S1,  ONE, W, W, W, W, FMT_FLAG_DEPTH),
	_(DXT1_RGB      , DXT1    ,   S1,   S1,   S1,  ONE, X, Y, Z, W, 0),
	_(DXT1_RGBA     , DXT1    ,   S1,   S1,   S1,   S1, X, Y, Z, W, 0),
	_(DXT3_RGBA     , DXT3    ,   S1,   S1,   S1,   S1, X, Y, Z, W, 0),
	_(DXT5_RGBA     , DXT5    ,   S1,   S1,   S1,   S1, X, Y, Z, W, 0),
	{},
};

void
nv30_fragtex_set(struct nvfx_context *nvfx, int unit)
{
	struct nvfx_sampler_state *ps = nvfx->tex_sampler[unit];
	struct nvfx_miptree *mt = (struct nvfx_miptree *)nvfx->fragment_sampler_views[unit]->texture;
	struct pipe_resource *pt = &mt->base.base;
	struct nouveau_bo *bo = mt->base.bo;
	struct nv30_texture_format *tf;
	struct nouveau_channel* chan = nvfx->screen->base.channel;
	uint32_t txf, txs;
	unsigned tex_flags = NOUVEAU_BO_VRAM | NOUVEAU_BO_GART | NOUVEAU_BO_RD;
	unsigned use_rect;

	tf = &nv30_texture_formats[pt->format];
	assert(tf->format >= 0);

	if(pt->height0 <= 1 || util_format_is_compressed(pt->format))
	{
		/* in the case of compressed or 1D textures, we can get away with this,
		 * since the layout is the same
		 */
		use_rect = ps->fmt;
	}
	else
	{
		static int warned = 0;
		if(!warned && !ps->fmt != !(pt->flags & NVFX_RESOURCE_FLAG_LINEAR)) {
			warned = 1;
			fprintf(stderr,
					"Unimplemented: coordinate normalization mismatch. Possible reasons:\n"
					"1. ARB_texture_non_power_of_two is being used despite the fact it isn't supported\n"
					"2. The state tracker is not using the appropriate coordinate normalization\n");
		}

		use_rect  = pt->flags & NVFX_RESOURCE_FLAG_LINEAR;
	}

	txf = use_rect ? tf->rect_format : tf->format;
	txs = tf->swizzle;
	if((tf->flags & FMT_FLAG_DEPTH) && !ps->compare)
	{
		/* This works by reading the depth value most significant 8/16 bits.
		 * We are losing precision, but nVidia loses even more by using A8R8G8B8 instead of HILO16
		 * There is no 32-bit integer texture support, so other things are infeasible.
		 *
		 * TODO: is it possible to read 16 bits for Z16? A16 doesn't seem to work, either due to normalization or endianness issues
		 */
		switch(txf)
		{
		case NV34TCL_TX_FORMAT_FORMAT_Z24:
			txf = NV34TCL_TX_FORMAT_FORMAT_HILO16;
			break;
		case NV34TCL_TX_FORMAT_FORMAT_Z24_RECT:
			txf = NV34TCL_TX_FORMAT_FORMAT_HILO16_RECT;
			break;
		case NV34TCL_TX_FORMAT_FORMAT_Z16:
			txf = NV34TCL_TX_FORMAT_FORMAT_A8L8;
			break;
		case NV34TCL_TX_FORMAT_FORMAT_Z16_RECT:
			txf = NV34TCL_TX_FORMAT_FORMAT_A8L8_RECT;
			break;
		default:
			assert(0);
		}
	}
	txf |= ((pt->last_level>0) ? NV34TCL_TX_FORMAT_MIPMAP : 0);
	txf |= log2i(pt->width0) << NV34TCL_TX_FORMAT_BASE_SIZE_U_SHIFT;
	txf |= log2i(pt->height0) << NV34TCL_TX_FORMAT_BASE_SIZE_V_SHIFT;
	txf |= log2i(pt->depth0) << NV34TCL_TX_FORMAT_BASE_SIZE_W_SHIFT;
	txf |= NV34TCL_TX_FORMAT_NO_BORDER | 0x10000;

	switch (pt->target) {
	case PIPE_TEXTURE_CUBE:
		txf |= NV34TCL_TX_FORMAT_CUBIC;
		/* fall-through */
	case PIPE_TEXTURE_2D:
		txf |= NV34TCL_TX_FORMAT_DIMS_2D;
		break;
	case PIPE_TEXTURE_3D:
		txf |= NV34TCL_TX_FORMAT_DIMS_3D;
		break;
	case PIPE_TEXTURE_1D:
		txf |= NV34TCL_TX_FORMAT_DIMS_1D;
		break;
	default:
		NOUVEAU_ERR("Unknown target %d\n", pt->target);
		return;
	}

	if(use_rect)
		txs |= nvfx_subresource_pitch(&mt->base, 0) << NV34TCL_TX_SWIZZLE_RECT_PITCH_SHIFT;

	MARK_RING(chan, 9, 2);
	OUT_RING(chan, RING_3D(NV34TCL_TX_OFFSET(unit), 8));
	OUT_RELOC(chan, bo, 0, tex_flags | NOUVEAU_BO_LOW, 0, 0);
	OUT_RELOC(chan, bo, txf, tex_flags | NOUVEAU_BO_OR,
		      NV34TCL_TX_FORMAT_DMA0, NV34TCL_TX_FORMAT_DMA1);
	OUT_RING(chan, ps->wrap);
	OUT_RING(chan, NV34TCL_TX_ENABLE_ENABLE | ps->en);
	OUT_RING(chan, txs);
	OUT_RING(chan, ps->filt | 0x2000 /*voodoo*/);
	OUT_RING(chan, (pt->width0 << NV34TCL_TX_NPOT_SIZE_W_SHIFT) |
		       pt->height0);
	OUT_RING(chan, ps->bcol);

	nvfx->hw_txf[unit] = txf;
	nvfx->hw_samplers |= (1 << unit);
}
