#ifndef NVFX_RESOURCE_H
#define NVFX_RESOURCE_H

#include "util/u_transfer.h"
#include "util/u_double_list.h"

struct pipe_resource;
struct nouveau_bo;


/* This gets further specialized into either buffer or texture
 * structures.  In the future we'll want to remove much of that
 * distinction, but for now try to keep as close to the existing code
 * as possible and use the vtbl struct to choose between the two
 * underlying implementations.
 */
struct nvfx_resource {
	struct pipe_resource base;
	struct u_resource_vtbl *vtbl;
	struct nouveau_bo *bo;
};

#define NVFX_RESOURCE_FLAG_LINEAR (PIPE_RESOURCE_FLAG_DRV_PRIV << 0)

#define NVFX_MAX_TEXTURE_LEVELS  16

struct nvfx_miptree {
        struct nvfx_resource base;

        unsigned linear_pitch; /* for linear textures, 0 for swizzled and compressed textures with level-dependent minimal pitch */
        unsigned face_size; /* 128-byte aligned face/total size */
        unsigned level_offset[NVFX_MAX_TEXTURE_LEVELS];
};

struct nvfx_surface {
	struct pipe_surface base;
	unsigned pitch;

	struct nouveau_bo* render;
        struct list_head render_list;
        struct list_head* render_cache;
};

static INLINE 
struct nvfx_resource *nvfx_resource(struct pipe_resource *resource)
{
	return (struct nvfx_resource *)resource;
}

static INLINE struct nouveau_bo *
nvfx_surface_buffer(struct pipe_surface *surf)
{
	struct nvfx_resource *mt = nvfx_resource(surf->texture);

	return mt->bo;
}


void
nvfx_init_resource_functions(struct pipe_context *pipe);

void
nvfx_screen_init_resource_functions(struct pipe_screen *pscreen);


/* Internal:
 */

struct pipe_resource *
nvfx_miptree_create(struct pipe_screen *pscreen, const struct pipe_resource *pt);

struct pipe_resource *
nvfx_miptree_from_handle(struct pipe_screen *pscreen,
			 const struct pipe_resource *template,
			 struct winsys_handle *whandle);

struct pipe_resource *
nvfx_buffer_create(struct pipe_screen *pscreen,
		   const struct pipe_resource *template);

struct pipe_resource *
nvfx_user_buffer_create(struct pipe_screen *screen,
			void *ptr,
			unsigned bytes,
			unsigned usage);



void
nvfx_miptree_surface_del(struct pipe_surface *ps);

struct pipe_surface *
nvfx_miptree_surface_new(struct pipe_screen *pscreen, struct pipe_resource *pt,
			 unsigned face, unsigned level, unsigned zslice,
			 unsigned flags);


void
nvfx_surface_copy_render_temp(struct pipe_surface* surf, int dir);

void
nvfx_surface_do_use_render_temp(struct pipe_surface* surf, struct list_head* render_cache);

static inline void
nvfx_surface_use_render_temp(struct pipe_surface* surf, struct list_head* render_cache)
{
	if(((struct nvfx_surface*)surf)->render_cache != render_cache)
		nvfx_surface_do_use_render_temp(surf, render_cache);
}

void
nvfx_surface_do_flush(struct pipe_surface* surf);

static inline void
nvfx_surface_flush(struct pipe_surface* surf)
{
	if(((struct nvfx_surface*)surf)->render_cache)
		nvfx_surface_do_flush(surf);
}

void
nvfx_surface_do_flush_render_cache(struct list_head* list);

static inline void
nvfx_surface_flush_render_cache(struct list_head* list)
{
	if(!LIST_IS_EMPTY(list))
		nvfx_surface_do_flush_render_cache(list);
}

struct nvfx_render_target;

void
nvfx_surface_get_render_target(struct pipe_surface* surf, int all_swizzled, struct nvfx_render_target* target);

#endif
