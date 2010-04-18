#include <errno.h>
#include "pipe/p_context.h"
#include "pipe/p_format.h"
#include "util/u_format.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_rect.h"
#include "util/u_blitter.h"

#include "nouveau/nouveau_winsys.h"
#include "nouveau/nouveau_util.h"
#include "nouveau/nouveau_screen.h"
#include "nvfx_context.h"
#include "nvfx_screen.h"
#include "nvfx_resource.h"
#include "nv04_2d.h"

static INLINE void
nvfx_surface_get_region(struct nv04_region* rgn, struct nvfx_surface* ns, unsigned x, unsigned y, boolean for_write)
{
	struct pipe_surface* surf = &ns->base.base;
	rgn->x = x;
	rgn->y = y;
	rgn->z = 0;

	unsigned bits = util_format_get_blocksizebits(surf->texture->format);
	switch(bits)
	{
	case 8:
		rgn->bpps = 0;
		break;
	case 16:
		rgn->bpps = 1;
		break;
	case 32:
		rgn->bpps = 2;
		break;
	default:
		assert(util_is_pot(bits));
		int shift = log2i(bits) - 3;
		assert(shift >= 2);
		rgn->bpps = 2;
		shift -= 2;
		assert(surf->texture->flags & NVFX_RESOURCE_FLAG_LINEAR);

		rgn->x = util_format_get_nblocksx(surf->format, x) << shift;
		rgn->y = util_format_get_nblocksy(surf->format, y);
	}

	if(ns->temp) {
		rgn->bo = ns->temp;
		rgn->offset = 0;
		rgn->pitch = ns->pitch;

		if(for_write)
			util_dirty_surface_set_dirty(nvfx_surface_get_dirty_surfaces(surf), &ns->base);
	} else {
		rgn->bo = ((struct nvfx_miptree*)surf->texture)->base.bo;
		rgn->offset = surf->offset;

		if(!(surf->texture->flags & NVFX_RESOURCE_FLAG_LINEAR))
		{
			unsigned depth = u_minify(surf->texture->depth0, surf->level);

			// TODO: move this code to surface creation?
			if((depth <= 1) && (surf->height <= 1 || surf->width <= 2))
				rgn->pitch = surf->width << rgn->bpps;
			else if(depth > 1 && surf->height <= 2 && surf->width <= 2)
			{
				rgn->pitch = surf->width << rgn->bpps;
				rgn->offset += (surf->zslice * surf->width * surf->height) << rgn->bpps;
			}
			else
			{
				rgn->pitch = 0;
				rgn->z = surf->zslice;
				rgn->w = surf->width;
				rgn->h = surf->height;
				rgn->d = depth;
			}
		}
		else
		{
			rgn->pitch = ns->pitch;
			//rgn->w = rgn->h = rgn->d = rgn->z = 0; // undefined for non-swizzled
		}
	}
}

// TODO: actually test this for all formats, it's probably wrong for some...

static INLINE int
nvfx_surface_format(enum pipe_format format)
{
	switch(util_format_get_blocksize(format)) {
	case 1:
		return NV04_CONTEXT_SURFACES_2D_FORMAT_Y8;
	case 2:
		//return NV04_CONTEXT_SURFACES_2D_FORMAT_Y16;
		return NV04_CONTEXT_SURFACES_2D_FORMAT_R5G6B5;
	case 4:
		//if(format == PIPE_FORMAT_B8G8R8X8_UNORM || format == PIPE_FORMAT_B8G8R8A8_UNORM)
			return NV04_CONTEXT_SURFACES_2D_FORMAT_A8R8G8B8;
		//else
		//	return NV04_CONTEXT_SURFACES_2D_FORMAT_Y32;
	default:
		return -1;
	}
}

static INLINE int
nv04_scaled_image_format(enum pipe_format format)
{
	switch(util_format_get_blocksize(format)) {
	case 1:
		return NV03_SCALED_IMAGE_FROM_MEMORY_COLOR_FORMAT_Y8;
	case 2:
		//if(format == PIPE_FORMAT_B5G5R5A1_UNORM)
		//	return NV03_SCALED_IMAGE_FROM_MEMORY_COLOR_FORMAT_A1R5G5B5;
		//else
			return NV03_SCALED_IMAGE_FROM_MEMORY_COLOR_FORMAT_R5G6B5;
	case 4:
		if(format == PIPE_FORMAT_B8G8R8X8_UNORM)
			return NV03_SCALED_IMAGE_FROM_MEMORY_COLOR_FORMAT_X8R8G8B8;
		else
			return NV03_SCALED_IMAGE_FROM_MEMORY_COLOR_FORMAT_A8R8G8B8;
	default:
		return -1;
	}
}

static struct blitter_context*
nvfx_get_blitter(struct pipe_context* pipe, int copy)
{
	struct nvfx_context* nvfx = nvfx_context(pipe);

	struct blitter_context* blitter = nvfx->blitter;
	if(!blitter)
		nvfx->blitter = blitter = util_blitter_create(pipe);

	util_blitter_save_blend(blitter, nvfx->blend);
	util_blitter_save_depth_stencil_alpha(blitter, nvfx->zsa);
	util_blitter_save_stencil_ref(blitter, &nvfx->stencil_ref);
	util_blitter_save_rasterizer(blitter, nvfx->rasterizer);
	util_blitter_save_fragment_shader(blitter, nvfx->fragprog);
	util_blitter_save_vertex_shader(blitter, nvfx->vertprog);
	util_blitter_save_viewport(blitter, &nvfx->viewport);
	util_blitter_save_framebuffer(blitter, &nvfx->framebuffer);
	util_blitter_save_clip(blitter, &nvfx->clip);
	util_blitter_save_vertex_elements(blitter, nvfx->vtxelt);
	util_blitter_save_vertex_buffers(blitter, nvfx->vtxbuf_nr, nvfx->vtxbuf);
	
	if(copy)
	{
		util_blitter_save_fragment_sampler_states(blitter, nvfx->nr_samplers, (void**)nvfx->tex_sampler);
		util_blitter_save_fragment_sampler_views(blitter, nvfx->nr_textures, nvfx->fragment_sampler_views);
	}

	return blitter;
}

static void
nvfx_surface_copy(struct pipe_context* pipe, struct pipe_surface *dsts,
		  unsigned dx, unsigned dy, struct pipe_surface *srcs, unsigned sx, unsigned sy,
		  unsigned w, unsigned h)
{
	struct nv04_2d_context *ctx = nvfx_screen(pipe->screen)->eng2d;
	struct nv04_region dst, src;

	if(!w || !h)
		return;

	static int copy_threshold = -1;
	if(copy_threshold < 0)
	{
		copy_threshold = debug_get_num_option("NOUVEAU_COPY_THRESHOLD", 0);
		if(copy_threshold < 0)
			copy_threshold = 0;
	}

	int dst_to_gpu = !(dsts->texture->_usage & PIPE_USAGE_DYNAMIC);;
	int src_on_gpu = nouveau_resource_on_gpu(srcs->texture);

	//printf("%i %ix%i\n", dsts->format, w, h);

	nvfx_surface_get_region(&dst, (struct nvfx_surface*)dsts, dx, dy, TRUE);
	nvfx_surface_get_region(&src, (struct nvfx_surface*)srcs, sx, sy, FALSE);
	w = util_format_get_stride(dsts->format, w) >> dst.bpps;
	h = util_format_get_nblocksy(dsts->format, h);

	//printf("%i %ix%i\n", dsts->format, w, h);

	int ret;
	if((!dst_to_gpu || !src_on_gpu) && (w * h <= copy_threshold))
		ret = -1; /* use the CPU */
	else
		ret = nv04_region_copy_2d(ctx, &dst, &src, w, h,
			nvfx_surface_format(dsts->texture->format), nv04_scaled_image_format(dsts->texture->format),
			dst_to_gpu, src_on_gpu);
	if(!ret)
	{}
	else if(ret > 0 && dsts->texture->bind & PIPE_BIND_RENDER_TARGET
			&& srcs->texture->bind & PIPE_BIND_SAMPLER_VIEW
			)
	{
		struct blitter_context* blitter = nvfx_get_blitter(pipe, 1);
		util_blitter_copy(blitter, dsts, dx, dy, srcs, sx, sy, w, h, TRUE);
	}
	else
		nv04_region_copy_cpu(&dst, &src, w, h);
}

static void
nvfx_surface_fill(struct pipe_context* pipe, struct pipe_surface *dsts,
		  unsigned dx, unsigned dy, unsigned w, unsigned h, unsigned value)
{
	struct nv04_2d_context *ctx = nvfx_screen(pipe->screen)->eng2d;
	struct nv04_region dst;
	/* Always try to use the GPU right now, if possible
	 * If the user wanted the surface data on the CPU, he would have cleared with memset (hopefully) */

	// we don't care about interior pixel order since we set all them to the same value
	nvfx_surface_get_region(&dst, (struct nvfx_surface*)dsts, dx, dy, TRUE);
	w = util_format_get_stride(dsts->format, w) >> dst.bpps;
	h = util_format_get_nblocksy(dsts->format, h);

	int ret = nv04_region_fill_2d(ctx, &dst, w, h, value);
	if(!ret)
		return;
	else if(ret > 0 && dsts->texture->bind & PIPE_BIND_RENDER_TARGET)
	{
		struct blitter_context* blitter = nvfx_get_blitter(pipe, 0);
		util_blitter_fill(blitter, dsts, dx, dy, w, h, value);
	}
	else
		nv04_region_fill_cpu(&dst, w, h, value);
}

void
nvfx_screen_surface_takedown(struct pipe_screen *pscreen)
{
	nv04_2d_context_takedown(nvfx_screen(pscreen)->eng2d);
	nvfx_screen(pscreen)->eng2d = 0;
}

int
nvfx_screen_surface_init(struct pipe_screen *pscreen)
{
	struct nv04_2d_context* ctx = nv04_2d_context_init(nouveau_screen(pscreen)->channel);
	if(!ctx)
		return -1;
	nvfx_screen(pscreen)->eng2d = ctx;
	return 0;
}

void
nvfx_init_surface_functions(struct nvfx_context* nvfx)
{
	nvfx->pipe.surface_copy = nvfx_surface_copy;
	nvfx->pipe.surface_fill = nvfx_surface_fill;
}

static void
nvfx_surface_copy_temp(struct pipe_context* pipe, struct pipe_surface* surf, int to_temp)
{
	struct nv04_2d_context* ctx = nvfx_screen(surf->texture->screen)->eng2d;
	struct nvfx_surface* ns = (struct nvfx_surface*)surf;
	unsigned w = surf->width;
	unsigned h = surf->height;
	struct nv04_region surfrgn;
	struct nv04_region temp;
	temp.bo = ns->temp;
	/* zero it out temporarily, so we can get the original region */
	ns->temp = 0;
	nvfx_surface_get_region(&surfrgn, ns, 0, 0, !to_temp);
	ns->temp = temp.bo;

	temp.offset = 0;
	temp.pitch = align(util_format_get_stride(surf->format, surf->width), 64);
	temp.bpps = surfrgn.bpps;
	temp.x = temp.y = temp.z = 0;

	struct nv04_region* dst = to_temp ? &temp : &surfrgn;
	struct nv04_region* src = to_temp ? &surfrgn : &temp;
	int ret = nv04_region_copy_2d(ctx, dst, src, w, h,
			nvfx_surface_format(surf->format), nv04_scaled_image_format(surf->format),
			1, 1);
	if(!ret)
	{}
	else if(ret > 0) {
		// TODO: use ad-hoc 3D code here, this is horribly inefficient
		struct pipe_resource* tempt = nvfx_miptree_from_region(surf->texture->screen, &temp, surf->texture->format, w, h);
		struct pipe_surface* temps = nvfx_miptree_surface_new(tempt->screen, tempt, 0, 0, 0, 0);
		struct pipe_surface* dsts = to_temp ? temps : surf;
		struct pipe_surface* srcs = to_temp ? surf : temps;

		/* TODO: check this earlier! */
		if(dsts->texture->bind & PIPE_BIND_RENDER_TARGET
			&& srcs->texture->bind & PIPE_BIND_SAMPLER_VIEW)
		{
			struct blitter_context* blitter = nvfx_get_blitter(pipe, 1);
			struct nvfx_context* nvfx = (struct nvfx_context*)pipe;
			void* ib = nvfx->idxbuf;
			unsigned fmt = nvfx->idxbuf_format;
			util_blitter_copy(blitter, dsts, dst->x, dst->y, srcs, src->x, src->y, w, h, TRUE);
			nvfx->idxbuf = ib;
			nvfx->idxbuf_format = fmt;
			// velem restore already sets NVFX_NEW_ARRAYS
		}
		else
			nv04_region_copy_cpu(dst, src, w, h);

		pipe_surface_reference(&temps, 0);
		pipe_resource_reference(&tempt, 0);
	}
	else
		nv04_region_copy_cpu(dst, src, w, h);
}

void
nvfx_surface_create_temp(struct pipe_context* pipe, struct pipe_surface* surf)
{
	struct nvfx_surface* ns = (struct nvfx_surface*)surf;
	unsigned size = util_format_get_2d_size(surf->format, align(ns->pitch, 64), surf->height);
	// TODO: 256 alignment is probably too much, 128 or 64 should be correct
	// TODO: should we put _GART here? seems we shouldn't
	nouveau_bo_new(nouveau_screen(surf->texture->screen)->device, NOUVEAU_BO_MAP | NOUVEAU_BO_VRAM, 256, size, &ns->temp);
	nvfx_surface_copy_temp(pipe, surf, 1);
}

void
nvfx_surface_flush(struct pipe_context* pipe, struct pipe_surface* surf)
{
	struct nvfx_surface* ns = (struct nvfx_surface*)surf;

	nvfx_surface_copy_temp(pipe, surf, 0);
	nouveau_bo_ref(0, &ns->temp);

	util_dirty_surface_set_clean(nvfx_surface_get_dirty_surfaces(surf), &ns->base);
}

