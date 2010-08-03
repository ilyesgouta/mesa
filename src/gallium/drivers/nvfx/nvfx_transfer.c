#include "pipe/p_state.h"
#include "pipe/p_defines.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_memory.h"
#include "util/u_math.h"
#include "util/u_staging.h"
#include "nvfx_context.h"
#include "nvfx_screen.h"
#include "nvfx_state.h"
#include "nvfx_resource.h"
#include "nvfx_transfer.h"

struct nvfx_staging_transfer
{
	struct util_staging_transfer base;

	unsigned offset;
};

struct pipe_transfer *
nvfx_transfer_new(struct pipe_context *pipe,
			  struct pipe_resource *pt,
			  struct pipe_subresource sr,
			  unsigned usage,
			  const struct pipe_box *box)
{
	struct nvfx_staging_transfer* tx;
	bool direct = !nvfx_resource_on_gpu(pt) && pt->flags & NVFX_RESOURCE_FLAG_LINEAR;

	tx = (struct nvfx_staging_transfer*)util_staging_transfer_new(pipe, pt, sr, usage, box, direct);

	if(pt->target == PIPE_BUFFER)
	{
		tx->base.base.slice_stride = tx->base.base.stride = ((struct nvfx_resource*)tx->base.staging_resource)->bo->size;
		if(direct)
			tx->offset = util_format_get_stride(pt->format, box->x);
		else
			tx->offset = 0;
	}
	else
	{
		if(direct)
		{
			tx->base.base.stride = nvfx_subresource_pitch(pt, sr.level);
			tx->base.base.slice_stride = tx->base.base.stride * u_minify(pt->height0, sr.level);
			tx->offset = nvfx_subresource_offset(pt, sr.face, sr.level, box->z)
				+ util_format_get_2d_size(pt->format, tx->base.base.stride, box->y)
				+ util_format_get_stride(pt->format, box->x);
		}
		else
		{
			tx->base.base.stride = ((struct nvfx_miptree*)tx->base.staging_resource)->linear_pitch;
			tx->base.base.slice_stride = tx->base.base.stride * tx->base.staging_resource->height0;
			tx->offset = 0;
		}
	}

	return &tx->base.base;
}

void *
nvfx_transfer_map(struct pipe_context *pipe, struct pipe_transfer *ptx)
{
	struct nvfx_staging_transfer *tx = (struct nvfx_staging_transfer *)ptx;
	struct nvfx_miptree *mt = (struct nvfx_miptree *)tx->base.staging_resource;

	uint8_t *map = nouveau_screen_bo_map(pipe->screen, mt->base.bo, nouveau_screen_transfer_flags(ptx->usage));

	return map + tx->offset;
}

void
nvfx_transfer_unmap(struct pipe_context *pipe, struct pipe_transfer *ptx)
{
	struct nvfx_staging_transfer *tx = (struct nvfx_staging_transfer *)ptx;
	struct nvfx_miptree *mt = (struct nvfx_miptree *)tx->base.staging_resource;

	nouveau_screen_bo_unmap(pipe->screen, mt->base.bo);
}
