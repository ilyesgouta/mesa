#ifndef __NV04_SURFACE_2D_H__
#define __NV04_SURFACE_2D_H__

struct nvfx_surface {
	struct pipe_surface base;
	unsigned pitch;
};

struct nvfx_surface_2d {
	struct nouveau_notifier *ntfy;
	struct nouveau_grobj *surf2d;
	struct nouveau_grobj *swzsurf;
	struct nouveau_grobj *m2mf;
	struct nouveau_grobj *rect;
	struct nouveau_grobj *blit;
	struct nouveau_grobj *sifm;
};

#define NVFX_RESOURCE_FLAG_LINEAR (PIPE_RESOURCE_FLAG_DRV_PRIV << 0)

#endif
