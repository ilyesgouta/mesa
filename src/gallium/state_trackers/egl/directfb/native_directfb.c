/*
 * Copyright (C) 2013 Ilyes Gouta, ilyes.gouta@gmail.com.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

/**
 * For DirectFB window system,
 *
 *  - the only valid native display is EGL_DEFAULT_DISPLAY
 */

#include <stdio.h>
#include <time.h>

#include <directfb.h>

#include "util/u_memory.h"
#include "util/u_format.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "directfb/directfb_sw_winsys.h"

#include "common/native_helper.h"
#include "common/native.h"

struct directfb_display {
   struct native_display base;

   const struct native_event_handler *event_handler;

   IDirectFBSurface *pSurface;

   struct native_config *configs;
   int num_configs;
};

struct directfb_surface {
   struct native_surface base;

   enum pipe_format color_format;

   struct directfb_display *display;

   IDirectFBSurface *pSurface;

   unsigned int server_stamp;
   unsigned int client_stamp;

   struct resource_surface *rsurf;
};

static INLINE struct directfb_display *
directfb_display(const struct native_display *ndpy)
{
   return (struct directfb_display *)ndpy;
}

static INLINE struct directfb_surface *
directfb_surface(const struct native_surface *nsurf)
{
   return (struct directfb_surface *)nsurf;
}

static const struct native_config **
directfb_display_get_configs(struct native_display *ndpy, int *num_configs)
{
   struct directfb_display *display = directfb_display(ndpy);
   const struct native_config **configs;
   int i;

   debug_checkpoint_full();

   configs = MALLOC(sizeof(*configs) * display->num_configs);
   if (configs) {
      for (i = 0; i < display->num_configs; i++)
         configs[i] = &display->configs[i];
      if (num_configs)
         *num_configs = display->num_configs;
   }

   return configs;
}

static int
directfb_display_get_param(struct native_display *ndpy,
                           enum native_param_type param)
{
   int val = 0;

   debug_checkpoint_full();

   switch (param) {
   case NATIVE_PARAM_PRESERVE_BUFFER:
   case NATIVE_PARAM_USE_NATIVE_BUFFER:
      val = 1;
      break;
   case NATIVE_PARAM_MAX_SWAP_INTERVAL:
   default:
      val = 0;
      break;
   }

   return val;
}

static void
directfb_display_destroy(struct native_display *ndpy)
{
   struct directfb_display *display = directfb_display(ndpy);

   debug_checkpoint_full();

   FREE(display->configs);
   ndpy_uninit(&display->base);
   FREE(display);
}

static boolean
directfb_display_init_config(struct native_display *ndpy)
{
   debug_checkpoint_full();

   const enum pipe_format color_formats[] = {
      PIPE_FORMAT_B8G8R8A8_UNORM,
      PIPE_FORMAT_B8G8R8X8_UNORM,
      PIPE_FORMAT_B5G6R5_UNORM,
      PIPE_FORMAT_NONE
   };

   struct directfb_display *display = directfb_display(ndpy);
   int i;

   display->configs =
         CALLOC(Elements(color_formats) - 1, sizeof(*display->configs));

   if (!display->configs)
      return FALSE;

   /* add configs */
   for (i = 0; color_formats[i] != PIPE_FORMAT_NONE; i++) {
      if (display->base.screen->is_format_supported(display->base.screen,
                                                    color_formats[i],
                                                    PIPE_TEXTURE_2D,
                                                    0,
                                                    PIPE_BIND_RENDER_TARGET |
                                                    PIPE_BIND_DISPLAY_TARGET |
                                                    PIPE_BIND_SCANOUT)) {
         struct native_config *nconf = &display->configs[display->num_configs];

         nconf->color_format = color_formats[i];
         nconf->buffer_mask = (1 << NATIVE_ATTACHMENT_BACK_LEFT)
                              | (1 << NATIVE_ATTACHMENT_FRONT_LEFT);
         nconf->pixmap_bit = TRUE;
         nconf->window_bit = TRUE;
         display->num_configs++;
      }
   }

   return TRUE;
}

static boolean
directfb_display_init_screen(struct native_display *ndpy)
{
   struct directfb_display *display = directfb_display(ndpy);
   struct sw_winsys *ws;

   debug_checkpoint_full();

   ws = directfb_sw_create();
   if (!ws)
      return FALSE;

   display->base.screen =
         display->event_handler->new_sw_screen(&display->base, ws);

   if (!display->base.screen) {
      if (ws->destroy)
         ws->destroy(ws);
      return FALSE;
   }

   if (!directfb_display_init_config(&display->base)) {
      ndpy_uninit(&display->base);
      return FALSE;
   }

   return TRUE;
}

/**
 * Update the geometry of the surface.  This is a slow functions.
 */
static void
directfb_surface_update_geometry(struct native_surface *nsurf)
{
   struct directfb_surface *surf = directfb_surface(nsurf);
   int w, h;

   debug_checkpoint_full();

   surf->pSurface->GetSize(surf->pSurface, &w, &h);

   if (resource_surface_set_size(surf->rsurf, w, h))
      surf->server_stamp++;
}

/**
 * Update the buffers of the surface.
 */
static boolean
directfb_surface_update_buffers(struct native_surface *nsurf, uint buffer_mask)
{
   struct directfb_surface *surf = directfb_surface(nsurf);

   debug_checkpoint_full();

   if (surf->client_stamp != surf->server_stamp) {
      directfb_surface_update_geometry(&surf->base);
      surf->client_stamp = surf->server_stamp;
   }

   return resource_surface_add_resources(surf->rsurf, buffer_mask);
}

/**
 * Emulate an invalidate event.
 */
static void
directfb_surface_invalidate(struct native_surface *nsurf)
{
   struct directfb_surface *surf = directfb_surface(nsurf);
   struct directfb_display *display = surf->display;

   debug_checkpoint_full();

   surf->server_stamp++;

   display->event_handler->invalid_surface(&display->base,
                                           &surf->base,
                                           surf->server_stamp);
}

static boolean
directfb_surface_flush_frontbuffer(struct native_surface *nsurf)
{
   struct directfb_surface *surf = directfb_surface(nsurf);
   boolean ret;

   debug_checkpoint_full();

   ret = resource_surface_present(surf->rsurf,
         NATIVE_ATTACHMENT_FRONT_LEFT, (void *)surf);

   /* force buffers to be updated in next validation call */
   directfb_surface_invalidate(&surf->base);

   return ret;
}

static boolean
directfb_surface_swap_buffers(struct native_surface *nsurf)
{
   struct directfb_surface *surf = directfb_surface(nsurf);
   boolean ret;

   debug_checkpoint_full();

   /* surf will be flipped in directfb_sw_displaytarget_display() */
   ret = resource_surface_present(surf->rsurf,
         NATIVE_ATTACHMENT_BACK_LEFT, (void *)surf->pSurface);

   resource_surface_swap_buffers(surf->rsurf,
         NATIVE_ATTACHMENT_FRONT_LEFT, NATIVE_ATTACHMENT_BACK_LEFT, TRUE);

   /* the front/back buffers have been swapped */
   directfb_surface_invalidate(&surf->base);

   return ret;
}

static boolean
directfb_surface_present(struct native_surface *nsurf,
                         const struct native_present_control *ctrl)
{
   boolean ret;

   debug_checkpoint_full();

   if (ctrl->preserve || ctrl->swap_interval)
      return FALSE;

   switch (ctrl->natt) {
   case NATIVE_ATTACHMENT_FRONT_LEFT:
      ret = directfb_surface_flush_frontbuffer(nsurf);
      break;
   case NATIVE_ATTACHMENT_BACK_LEFT:
      ret = directfb_surface_swap_buffers(nsurf);
      break;
   default:
      ret = FALSE;
      break;
   }

   return ret;
}

static boolean
directfb_surface_validate(struct native_surface *nsurf,
                          uint attachment_mask,
                          unsigned int *seq_num,
                          struct pipe_resource **textures,
                          int *width,
                          int *height)
{
   struct directfb_surface *surf = directfb_surface(nsurf);
   uint w, h;

   debug_checkpoint_full();

   if (!directfb_surface_update_buffers(&surf->base, attachment_mask))
      return FALSE;

   if (seq_num)
      *seq_num = surf->client_stamp;

   if (textures)
      resource_surface_get_resources(surf->rsurf, textures, attachment_mask);

   resource_surface_get_size(surf->rsurf, &w, &h);
   if (width)
      *width = w;
   if (height)
      *height = h;

   return TRUE;
}

static void
directfb_surface_wait(struct native_surface *nsurf)
{
   debug_checkpoint_full();

   /* no-op */
}

static void
directfb_surface_destroy(struct native_surface *nsurf)
{
   struct directfb_surface *surf = directfb_surface(nsurf);

   debug_checkpoint_full();

   IDirectFBSurface *pSurface = surf->display->pSurface;
   pSurface->Release(pSurface);

   resource_surface_destroy(surf->rsurf);
   FREE(surf);
}

static struct native_surface *
directfb_display_create_window_surface(struct native_display *ndpy,
                                       EGLNativeWindowType win,
                                       const struct native_config *nconf)
{
    struct directfb_display *display = directfb_display(ndpy);
    struct directfb_surface *surf;

    debug_checkpoint_full();

    DFBSurfaceCapabilities caps;
    DFBResult ret;

    int w, h;

    IDirectFBSurface *pSurface = (IDirectFBSurface*)win;

    if (!pSurface)
       return NULL;

    ret = pSurface->GetSize(pSurface, &w, &h);
    if (ret)
       return NULL;
    if (w <= 0 || h <= 0)
       return NULL;

    ret = pSurface->GetCapabilities(pSurface, &caps);
    if (ret)
       return NULL;

    if (!(caps & DSCAPS_FLIPPING)
        || (caps & (DSCAPS_INTERLACED | DSCAPS_SEPARATED | DSCAPS_SUBSURFACE)))
       return NULL;

    surf = CALLOC_STRUCT(directfb_surface);
    if (!surf)
       return NULL;

    ret = pSurface->GetSubSurface(pSurface, NULL, &surf->pSurface);

    if (ret) {
       FREE(surf);
       return NULL;
    }

    /* store the original surface in the directfb_display */
    pSurface->AddRef(pSurface);
    display->pSurface = pSurface;

    surf->display = display;
    surf->color_format = nconf->color_format;

    surf->rsurf = resource_surface_create(display->base.screen,
                                          surf->color_format,
                                          PIPE_BIND_RENDER_TARGET |
                                          PIPE_BIND_DISPLAY_TARGET);
    if (!surf->rsurf) {
       FREE(surf);
       return NULL;
    }

    directfb_surface_update_geometry(&surf->base);

    surf->base.destroy = directfb_surface_destroy;
    surf->base.present = directfb_surface_present;
    surf->base.validate = directfb_surface_validate;
    surf->base.wait = directfb_surface_wait;

    return &surf->base;
}

static struct native_display *
directfb_display_create(const struct native_event_handler *event_handler)
{
   struct directfb_display *display;

   debug_checkpoint_full();

   display = CALLOC_STRUCT(directfb_display);
   if (!display)
      return NULL;

   display->event_handler = event_handler;

   display->base.init_screen = directfb_display_init_screen;
   display->base.destroy = directfb_display_destroy;
   display->base.get_param = directfb_display_get_param;
   display->base.get_configs = directfb_display_get_configs;
   display->base.create_window_surface =
         directfb_display_create_window_surface;

   return &display->base;
}

static const struct native_event_handler *directfb_event_handler;

static struct native_display*
native_create_display(void *dpy, boolean use_sw)
{
   debug_checkpoint_full();

   struct native_display *display =
         directfb_display_create(directfb_event_handler);

   return display;
}

static const struct native_platform directfb_platform = {
   "DirectFB",
   native_create_display
};

const struct native_platform *
native_get_directfb_platform(const struct native_event_handler *event_handler)
{
   directfb_event_handler = event_handler;

   debug_checkpoint_full();

   return &directfb_platform;
}
