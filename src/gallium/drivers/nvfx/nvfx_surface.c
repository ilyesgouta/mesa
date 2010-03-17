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
nvfx_region_init(struct nv04_region* rgn, struct nvfx_surface* surf, unsigned x, unsigned y)
{
	rgn->bo = ((struct nvfx_miptree*)surf->base.texture)->base.bo;
	rgn->offset = surf->base.offset;
	rgn->x = x;
	rgn->y = y;
	rgn->z = 0;

	unsigned bits = util_format_get_blocksizebits(surf->base.texture->format);
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
		assert(surf->base.texture->flags & NVFX_RESOURCE_FLAG_LINEAR);

		rgn->x = util_format_get_nblocksx(surf->base.format, x) << shift;
		rgn->y = util_format_get_nblocksy(surf->base.format, y);
	}

        if(!(surf->base.texture->flags & NVFX_RESOURCE_FLAG_LINEAR))
        {
		unsigned depth = u_minify(surf->base.texture->depth0, surf->base.level);

		// TODO: move this code to surface creation?
		if((depth <= 1) && (surf->base.height <= 1 || surf->base.width <= 2))
			rgn->pitch = surf->base.width << rgn->bpps;
		else if(depth > 1 && surf->base.height <= 2 && surf->base.width <= 2)
		{
			rgn->pitch = surf->base.width << rgn->bpps;
			rgn->offset += (surf->base.zslice * surf->base.width * surf->base.height) << rgn->bpps;
		}
		else
		{
			rgn->pitch = 0;
			rgn->z = surf->base.zslice;
			rgn->w = surf->base.width;
			rgn->h = surf->base.height;
			rgn->d = depth;
		}
	}
	else
	{
		rgn->pitch = surf->pitch;
		//rgn->w = rgn->h = rgn->d = rgn->z = 0; // undefined for non-swizzled
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

	nvfx_surface_flush(dsts);
	nvfx_surface_flush(srcs);

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

	nvfx_region_init(&dst, (struct nvfx_surface*)dsts, dx, dy);
	nvfx_region_init(&src, (struct nvfx_surface*)srcs, sx, sy);
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
	 * If the user wanted the surface data on the CPU, he would have cleared with memset */

	nvfx_surface_flush(dsts);

	// we don't care about interior pixel order since we set all them to the same value
	nvfx_region_init(&dst, (struct nvfx_surface*)dsts, dx, dy);
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

void
nvfx_surface_copy_render_temp(struct pipe_surface* surf, int dir)
{
	struct nv04_2d_context* ctx = nvfx_screen(surf->texture->screen)->eng2d;
	struct nvfx_surface* ns = (struct nvfx_surface*)surf;
	unsigned w = ns->base.width;
	unsigned h = ns->base.height;
	struct nv04_region surfrgn;
	struct nv04_region render;
	nvfx_region_init(&surfrgn, ns, 0, 0);
	render.bo = ns->render;
	render.offset = 0;
	render.pitch = align(util_format_get_stride(ns->base.format, ns->base.width), 64);
	render.bpps = surfrgn.bpps;
	render.x = render.y = render.z = 0;
	struct nv04_region* dst = dir ? &render : &surfrgn;
	struct nv04_region* src = dir ? &surfrgn : &render;
	if(nv04_region_copy_2d(ctx, dst, src, w, h,
			nvfx_surface_format(ns->base.format), nv04_scaled_image_format(ns->base.format),
			1, 1))
		nv04_region_copy_cpu(dst, src, w, h);
}

void
nvfx_surface_do_use_render_temp(struct pipe_surface* surf, struct list_head* render_cache)
{
	struct nvfx_surface* ns = (struct nvfx_surface*)surf;
	if(!ns->render_cache)
		nvfx_surface_copy_render_temp(surf, 1);
	else
	{
		/* We can keep the surface only on one cache, and the user expects that flushing the old cache
		 * flushes this surface. Thus, we have no choice except flushing now.
		 */
		nvfx_surface_copy_render_temp(surf, 0);
		LIST_DEL(&ns->render_list);
	}

	ns->render_cache = render_cache;
	LIST_ADDTAIL(&ns->render_list, render_cache);
}

void
nvfx_surface_do_flush(struct pipe_surface* surf)
{
	struct nvfx_surface* ns = (struct nvfx_surface*)surf;
	//printf("flushing bo %i %ix%i\n", ns->render->handle, ns->base.width, ns->base.height);

	nvfx_surface_copy_render_temp(surf, 0);

	LIST_DEL(&ns->render_list);
	ns->render_cache = 0;
}

static inline
int nvfx_surface_renderable(struct nvfx_surface* ns, int all_swizzled)
{
	if(all_swizzled)
		assert(!(ns->base.texture->flags & NVFX_RESOURCE_FLAG_LINEAR));

	if(ns->base.texture->flags & NVFX_RESOURCE_FLAG_LINEAR)
		return !(ns->base.offset & 63) && !(ns->pitch & 63);
	else
		return all_swizzled;
}

void nvfx_surface_get_render_target(struct pipe_surface* surf, int all_swizzled, struct nvfx_render_target* target)
{
	struct nvfx_surface* ns = (struct nvfx_surface*)surf;
	if(nvfx_surface_renderable(ns, all_swizzled))
	{
		nvfx_surface_flush(surf);

		target->bo = ((struct nvfx_miptree*)ns->base.texture)->base.bo;
		target->offset = ns->base.offset;
		target->pitch = align(ns->pitch, 64);
	}
	else
	{
		if(!ns->render)
		{
			unsigned size = util_format_get_2d_size(ns->base.format, align(ns->pitch, 64), ns->base.height);
			// TODO: 256 alignment is probably too much, 128 or 64 should be correct
			// TODO: should we put _GART here? seems we shouldn't
			nouveau_bo_new(nouveau_screen(surf->texture->screen)->device, NOUVEAU_BO_MAP | NOUVEAU_BO_VRAM, 256, size, &ns->render);
//			printf("render temp!\n");
		}

		target->bo = ns->render;
		target->offset = 0;
		target->pitch = align(ns->pitch, 64);
	}
}

void
nvfx_surface_do_flush_render_cache(struct list_head* list)
{
	struct list_head* cur = list->next;
	struct list_head* next;
	for(cur = list->next; cur != list; cur = next)
	{
		struct nvfx_surface* ns = LIST_ENTRY(struct nvfx_surface, cur, render_list);
		next = cur->next;
		nvfx_surface_flush(&ns->base);
	}
}
