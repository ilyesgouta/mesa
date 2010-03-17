#include "nvfx_context.h"
#include "nvfx_resource.h"
#include "nouveau/nouveau_util.h"
#include "util/u_format.h"

void
nvfx_state_framebuffer_validate(struct nvfx_context *nvfx)
{
	struct pipe_framebuffer_state *fb = &nvfx->framebuffer;
	struct nouveau_channel *chan = nvfx->screen->base.channel;
	uint32_t rt_enable = 0, rt_format = 0;
	int i, colour_format = 0, zeta_format = 0;
	unsigned rt_flags = NOUVEAU_BO_RDWR | NOUVEAU_BO_VRAM;
	unsigned w = fb->width;
	unsigned h = fb->height;
	int all_swizzled = 1;
	int render_temps = 0;

	if(!nvfx->is_nv4x)
		assert(fb->nr_cbufs <= 2);
	else
		assert(fb->nr_cbufs <= 4);

	for (i = 0; i < fb->nr_cbufs; i++) {
		if (colour_format)
			assert(colour_format == fb->cbufs[i]->format);
		else
			colour_format = fb->cbufs[i]->format;

		rt_enable |= (NV34TCL_RT_ENABLE_COLOR0 << i);

		if(((struct nvfx_miptree*)fb->cbufs[i]->texture)->linear_pitch
				|| (fb->cbufs[i]->texture->target == PIPE_TEXTURE_3D && u_minify(fb->cbufs[i]->texture->depth0, fb->cbufs[0]->level) > 1)
				|| (fb->cbufs[i]->offset & 63)
				|| (fb->cbufs[i]->width != fb->width)
				|| (fb->cbufs[i]->height != fb->height)
				)
			all_swizzled = 0;
	}

	for (i = 0; i < fb->nr_cbufs; i++)
	{
		nvfx_surface_get_render_target(fb->cbufs[i], all_swizzled, &nvfx->hw_rt[i]);
		if(nvfx->hw_rt[i].bo != ((struct nvfx_miptree*)fb->cbufs[i]->texture)->base.bo)
			render_temps |= (1 << i);
		assert(util_format_get_stride(fb->cbufs[i]->format, fb->width) <= nvfx->hw_rt[i].pitch);
		assert(all_swizzled || nvfx->hw_rt[i].offset + nvfx->hw_rt[i].pitch * fb->height <= nvfx->hw_rt[i].bo->size);
	}
	for(; i < 4; ++i)
		nvfx->hw_rt[i].bo = 0;

	if (rt_enable & (NV34TCL_RT_ENABLE_COLOR1 |
			 NV40TCL_RT_ENABLE_COLOR2 | NV40TCL_RT_ENABLE_COLOR3))
		rt_enable |= NV34TCL_RT_ENABLE_MRT;

	if (fb->zsbuf) {
		zeta_format = fb->zsbuf->format;
		nvfx_surface_get_render_target(fb->zsbuf, 0, &nvfx->hw_zeta);

		if(nvfx->hw_zeta.bo != ((struct nvfx_miptree*)fb->zsbuf->texture)->base.bo)
			render_temps |= 0x80;

		assert(util_format_get_stride(fb->zsbuf->format, fb->width) <= nvfx->hw_zeta.pitch);
		assert(nvfx->hw_zeta.offset + nvfx->hw_zeta.pitch * fb->height <= nvfx->hw_zeta.bo->size);
	}

	nvfx->state.render_temps = render_temps;

	if (fb->nr_cbufs && all_swizzled) {
		assert(!(fb->width & (fb->width - 1)) && !(fb->height & (fb->height - 1)));

		rt_format = NV34TCL_RT_FORMAT_TYPE_SWIZZLED |
			(log2i(fb->width) << NV34TCL_RT_FORMAT_LOG2_WIDTH_SHIFT) |
			(log2i(fb->height) << NV34TCL_RT_FORMAT_LOG2_HEIGHT_SHIFT);
	} else
		rt_format = NV34TCL_RT_FORMAT_TYPE_LINEAR;

	switch (colour_format) {
	case PIPE_FORMAT_B8G8R8X8_UNORM:
		rt_format |= NV34TCL_RT_FORMAT_COLOR_X8R8G8B8;
		break;
	case PIPE_FORMAT_B8G8R8A8_UNORM:
	case 0:
		rt_format |= NV34TCL_RT_FORMAT_COLOR_A8R8G8B8;
		break;
	case PIPE_FORMAT_B5G6R5_UNORM:
		rt_format |= NV34TCL_RT_FORMAT_COLOR_R5G6B5;
		break;
	default:
		assert(0);
	}

	switch (zeta_format) {
	case PIPE_FORMAT_Z16_UNORM:
		rt_format |= NV34TCL_RT_FORMAT_ZETA_Z16;
		break;
	case PIPE_FORMAT_S8_USCALED_Z24_UNORM:
	case PIPE_FORMAT_X8Z24_UNORM:
	case 0:
		rt_format |= NV34TCL_RT_FORMAT_ZETA_Z24S8;
		break;
	default:
		assert(0);
	}

	if ((rt_enable & NV34TCL_RT_ENABLE_COLOR0) || fb->zsbuf) {
		struct nvfx_render_target *rt0 = &nvfx->hw_rt[0];
		uint32_t pitch = rt0->pitch;

		if(!(rt_enable & NV34TCL_RT_ENABLE_COLOR0))
			rt0 = &nvfx->hw_zeta;

		if(!nvfx->is_nv4x)
		{
			if (nvfx->hw_zeta.bo)
				pitch |= (nvfx->hw_zeta.pitch << 16);
			else
				pitch |= (pitch << 16);
		}

		OUT_RING(chan, RING_3D(NV34TCL_DMA_COLOR0, 1));
		OUT_RELOC(chan, rt0->bo, 0,
			      rt_flags | NOUVEAU_BO_OR,
			      chan->vram->handle, chan->gart->handle);
		OUT_RING(chan, RING_3D(NV34TCL_COLOR0_PITCH, 2));
		OUT_RING(chan, pitch);
		OUT_RELOC(chan, rt0->bo,
			      rt0->offset, rt_flags | NOUVEAU_BO_LOW,
			      0, 0);
	}

	if (rt_enable & NV34TCL_RT_ENABLE_COLOR1) {
		OUT_RING(chan, RING_3D(NV34TCL_DMA_COLOR1, 1));
		OUT_RELOC(chan, nvfx->hw_rt[1].bo, 0,
			      rt_flags | NOUVEAU_BO_OR,
			      chan->vram->handle, chan->gart->handle);
		OUT_RING(chan, RING_3D(NV34TCL_COLOR1_OFFSET, 2));
		OUT_RELOC(chan, nvfx->hw_rt[1].bo,
				nvfx->hw_rt[1].offset, rt_flags | NOUVEAU_BO_LOW,
			      0, 0);
		OUT_RING(chan, nvfx->hw_rt[1].pitch);
	}

	if(nvfx->is_nv4x)
	{
		if (rt_enable & NV40TCL_RT_ENABLE_COLOR2) {
			OUT_RING(chan, RING_3D(NV40TCL_DMA_COLOR2, 1));
			OUT_RELOC(chan, nvfx->hw_rt[2].bo, 0,
				      rt_flags | NOUVEAU_BO_OR,
				      chan->vram->handle, chan->gart->handle);
			OUT_RING(chan, RING_3D(NV40TCL_COLOR2_OFFSET, 1));
			OUT_RELOC(chan, nvfx->hw_rt[2].bo,
				      nvfx->hw_rt[2].offset, rt_flags | NOUVEAU_BO_LOW,
				      0, 0);
			OUT_RING(chan, RING_3D(NV40TCL_COLOR2_PITCH, 1));
			OUT_RING(chan, nvfx->hw_rt[2].pitch);
		}

		if (rt_enable & NV40TCL_RT_ENABLE_COLOR3) {
			OUT_RING(chan, RING_3D(NV40TCL_DMA_COLOR3, 1));
			OUT_RELOC(chan, nvfx->hw_rt[3].bo, 0,
				      rt_flags | NOUVEAU_BO_OR,
				      chan->vram->handle, chan->gart->handle);
			OUT_RING(chan, RING_3D(NV40TCL_COLOR3_OFFSET, 1));
			OUT_RELOC(chan, nvfx->hw_rt[3].bo,
					nvfx->hw_rt[3].offset, rt_flags | NOUVEAU_BO_LOW,
				      0, 0);
			OUT_RING(chan, RING_3D(NV40TCL_COLOR3_PITCH, 1));
			OUT_RING(chan, nvfx->hw_rt[3].pitch);
		}
	}

	if (zeta_format) {
		OUT_RING(chan, RING_3D(NV34TCL_DMA_ZETA, 1));
		OUT_RELOC(chan, nvfx->hw_zeta.bo, 0,
			      rt_flags | NOUVEAU_BO_OR,
			      chan->vram->handle, chan->gart->handle);
		OUT_RING(chan, RING_3D(NV34TCL_ZETA_OFFSET, 1));
		/* TODO: reverse engineer LMA */
		OUT_RELOC(chan, nvfx->hw_zeta.bo,
			     nvfx->hw_zeta.offset, rt_flags | NOUVEAU_BO_LOW, 0, 0);
	        if(nvfx->is_nv4x) {
			OUT_RING(chan, RING_3D(NV40TCL_ZETA_PITCH, 1));
			OUT_RING(chan, nvfx->hw_zeta.pitch);
		}
	}

	OUT_RING(chan, RING_3D(NV34TCL_RT_ENABLE, 1));
	OUT_RING(chan, rt_enable);
	OUT_RING(chan, RING_3D(NV34TCL_RT_HORIZ, 3));
	OUT_RING(chan, (w << 16) | 0);
	OUT_RING(chan, (h << 16) | 0);
	OUT_RING(chan, rt_format);
	OUT_RING(chan, RING_3D(NV34TCL_VIEWPORT_HORIZ, 2));
	OUT_RING(chan, (w << 16) | 0);
	OUT_RING(chan, (h << 16) | 0);
	OUT_RING(chan, RING_3D(NV34TCL_VIEWPORT_CLIP_HORIZ(0), 2));
	OUT_RING(chan, ((w - 1) << 16) | 0);
	OUT_RING(chan, ((h - 1) << 16) | 0);
	OUT_RING(chan, RING_3D(0x1d88, 1));
	OUT_RING(chan, (1 << 12) | h);

	if(!nvfx->is_nv4x) {
		/* Wonder why this is needed, context should all be set to zero on init */
		/* TODO: we can most likely remove this, after putting it in context init */
		OUT_RING(chan, RING_3D(NV34TCL_VIEWPORT_TX_ORIGIN, 1));
		OUT_RING(chan, 0);
	}
}

void
nvfx_framebuffer_relocate(struct nvfx_context *nvfx)
{
	struct nouveau_channel *chan = nvfx->screen->base.channel;
	unsigned rt_flags = NOUVEAU_BO_RDWR | NOUVEAU_BO_VRAM;
	rt_flags |= NOUVEAU_BO_DUMMY;
	MARK_RING(chan, 20, 20);

#define DO_(var, pfx, name) \
	if(var.bo) { \
		OUT_RELOC(chan, var.bo, RING_3D(pfx##TCL_DMA_##name, 1), rt_flags, 0, 0); \
		OUT_RELOC(chan, var.bo, 0, \
			rt_flags | NOUVEAU_BO_OR, \
			chan->vram->handle, chan->gart->handle); \
		OUT_RELOC(chan, var.bo, RING_3D(pfx##TCL_##name##_OFFSET, 1), rt_flags, 0, 0); \
		OUT_RELOC(chan, var.bo, \
			var.offset, rt_flags | NOUVEAU_BO_LOW, \
			0, 0); \
	}

#define DO(pfx, num) DO_(nvfx->hw_rt[num], pfx, COLOR##num)
	DO(NV34, 0);
	DO(NV34, 1);
	DO(NV40, 2);
	DO(NV40, 3);

	DO_(nvfx->hw_zeta, NV34, ZETA);
}
