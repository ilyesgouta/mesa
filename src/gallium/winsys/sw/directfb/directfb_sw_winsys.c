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
 * @file
 * DirectFB software rasterizer winsys.
 */

#include <stdio.h>
#include <assert.h>

#include "pipe/p_format.h"
#include "util/u_memory.h"
#include "util/u_debug.h"
#include "state_tracker/sw_winsys.h"

#include "directfb_sw_winsys.h"

struct directfb_sw_winsys
{
   struct sw_winsys base;

   IDirectFB *pDfb;
};

struct directfb_sw_displaytarget
{
   IDirectFBSurface *pSurface;
   IDirectFBSurface *pSubSurface;

   enum pipe_format fmt;
   DFBSurfacePixelFormat format;

   unsigned width;
   unsigned height;
   unsigned stride;
   void *data;
};

static INLINE struct directfb_sw_winsys *
directfb_sw_winsys(struct sw_winsys *ws)
{
   return (struct directfb_sw_winsys *)ws;
}

static INLINE struct directfb_sw_displaytarget *
directfb_sw_displaytarget(struct sw_displaytarget *dt)
{
   return (struct directfb_sw_displaytarget *)dt;
}

static boolean
directfb_sw_is_displaytarget_format_supported(struct sw_winsys *ws,
                                              unsigned tex_usage,
                                              enum pipe_format format )
{
   debug_checkpoint_full();

   switch (format) {
   case PIPE_FORMAT_B5G6R5_UNORM: /* 16bit RGB */
   case PIPE_FORMAT_B8G8R8A8_UNORM: /* 32bit ARGB */
      return TRUE;
   default:
      return FALSE;
   }
}

static DFBSurfacePixelFormat
dfb_pixelformat(enum pipe_format format )
{
   switch (format) {
   case PIPE_FORMAT_B5G6R5_UNORM: /* 16bit RGB */
      return DSPF_RGB16;
   case PIPE_FORMAT_B8G8R8A8_UNORM: /* 32bit ARGB */
      return DSPF_ARGB;
   default:
      return DSPF_UNKNOWN;
   }
}

static void *
directfb_sw_displaytarget_map(struct sw_winsys *ws,
                              struct sw_displaytarget *dt,
                              unsigned flags )
{
   struct directfb_sw_displaytarget *target;

   int pitch = 0;

   DFBResult ret = DFB_OK;

   debug_checkpoint_full();

   target = directfb_sw_displaytarget(dt);

   if (!target->data) {
      IDirectFBSurface *pSurface = target->pSurface;

      ret = pSurface->GetSubSurface(pSurface, NULL, &target->pSubSurface);
      if (ret)
         return NULL;

      pSurface = target->pSubSurface;

      ret = pSurface->Lock(pSurface, DSLF_READ | DSLF_WRITE, &target->data, &pitch);
      if (ret) {
         pSurface->Release(pSurface);
         return NULL;
      }

      assert(target->data);
   }

   return target->data;
}

static void
directfb_sw_displaytarget_unmap(struct sw_winsys *ws,
                                struct sw_displaytarget *dt )
{
   struct directfb_sw_displaytarget *target;

   debug_checkpoint_full();

   target = directfb_sw_displaytarget(dt);

   if (target->data) {
      IDirectFBSurface *pSurface = target->pSubSurface;

      pSurface->Unlock(pSurface);
      target->data = NULL;

      pSurface->Release(pSurface);
   }
}

static void
directfb_sw_displaytarget_destroy(struct sw_winsys *ws,
                                  struct sw_displaytarget *dt)
{
   struct directfb_sw_displaytarget *target;

   debug_checkpoint_full();

   target = directfb_sw_displaytarget(dt);

   IDirectFBSurface *pSurface = target->pSurface;

   pSurface->Release(pSurface);

   FREE(target);
}

static struct sw_displaytarget *
directfb_sw_displaytarget_create(struct sw_winsys *ws,
                                 unsigned tex_usage,
                                 enum pipe_format format,
                                 unsigned width,
                                 unsigned height,
                                 unsigned alignment,
                                 unsigned *stride)
{
   struct directfb_sw_winsys *winsys;
   struct directfb_sw_displaytarget *target;

   debug_printf("%s: %dx%d surface\n", __FUNCTION__, width, height);

   winsys = directfb_sw_winsys(ws);

   target = CALLOC_STRUCT(directfb_sw_displaytarget);
   if (!target)
      return NULL;

   DFBSurfaceDescription desc;
   DFBResult ret;

   void* data;
   int pitch;

   IDirectFB *pDfb = winsys->pDfb;
   IDirectFBSurface *pSurface;

   memset(&desc, 0, sizeof(desc));

   desc.flags = DSDESC_CAPS | DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
   desc.caps = DSCAPS_SYSTEMONLY;
   desc.width = width;
   desc.height = height;
   desc.pixelformat = dfb_pixelformat(format);

   ret = pDfb->CreateSurface(pDfb, &desc, &pSurface);

   if (ret) {
      FREE(target);
      return NULL;
   }

   target->fmt = format;
   target->format = dfb_pixelformat(format);
   target->width = width;
   target->height = height;

   target->pSurface = pSurface;

   ret = pSurface->Lock(pSurface, DSLF_WRITE, &data, &pitch);

   if (ret) {
      pSurface->Release(pSurface);
      FREE(target);
      return NULL;
   }

   *stride = pitch;

   pSurface->Unlock(pSurface);

   return (struct sw_displaytarget *)target;
}

static struct sw_displaytarget *
directfb_sw_displaytarget_from_handle(struct sw_winsys *ws,
                                      const struct pipe_resource *templet,
                                      struct winsys_handle *whandle,
                                      unsigned *stride)
{
   debug_checkpoint_full();

   return NULL;
}

static boolean
directfb_sw_displaytarget_get_handle(struct sw_winsys *ws,
                                     struct sw_displaytarget *dt,
                                     struct winsys_handle *whandle)
{
   debug_checkpoint_full();

   return FALSE;
}

static void
directfb_sw_displaytarget_display(struct sw_winsys *ws,
                                  struct sw_displaytarget *dt,
                                  void *context_private)
{
   struct directfb_sw_displaytarget *target;

   debug_checkpoint_full();

   /* Not cool! */
   IDirectFBSurface *pDest = (IDirectFBSurface*)context_private;

   target = directfb_sw_displaytarget(dt);

   IDirectFBSurface *pSurface = target->pSurface;

   pDest->StretchBlit(pDest, pSurface, NULL, NULL);
   pDest->Flip(pDest, NULL, 0);
}

static void
directfb_sw_destroy(struct sw_winsys *ws)
{
   debug_checkpoint_full();

   FREE(ws);
}

struct sw_winsys* directfb_sw_create()
{
   static struct directfb_sw_winsys *winsys;

   debug_checkpoint_full();

   winsys = CALLOC_STRUCT(directfb_sw_winsys);
   if (!winsys)
      return NULL;

   DirectFBCreate(&winsys->pDfb);

   winsys->base.destroy = directfb_sw_destroy;
   winsys->base.is_displaytarget_format_supported =
           directfb_sw_is_displaytarget_format_supported;
   winsys->base.displaytarget_create = directfb_sw_displaytarget_create;
   winsys->base.displaytarget_from_handle =
         directfb_sw_displaytarget_from_handle;
   winsys->base.displaytarget_get_handle =
         directfb_sw_displaytarget_get_handle;
   winsys->base.displaytarget_map = directfb_sw_displaytarget_map;
   winsys->base.displaytarget_unmap = directfb_sw_displaytarget_unmap;
   winsys->base.displaytarget_display = directfb_sw_displaytarget_display;
   winsys->base.displaytarget_destroy = directfb_sw_displaytarget_destroy;

   return &winsys->base;
}
