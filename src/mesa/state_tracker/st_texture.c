/**************************************************************************
 * 
 * Copyright 2007 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/

#include "st_context.h"
#include "st_format.h"
#include "st_texture.h"
#include "st_cb_fbo.h"
#include "st_inlines.h"
#include "main/enums.h"

#undef Elements  /* fix re-defined macro warning */

#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_rect.h"
#include "util/u_math.h"


#define DBG if(0) printf

#if 0
static GLenum
target_to_target(GLenum target)
{
   switch (target) {
   case GL_TEXTURE_CUBE_MAP_POSITIVE_X_ARB:
   case GL_TEXTURE_CUBE_MAP_NEGATIVE_X_ARB:
   case GL_TEXTURE_CUBE_MAP_POSITIVE_Y_ARB:
   case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_ARB:
   case GL_TEXTURE_CUBE_MAP_POSITIVE_Z_ARB:
   case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_ARB:
      return GL_TEXTURE_CUBE_MAP_ARB;
   default:
      return target;
   }
}
#endif


/**
 * Allocate a new pipe_resource object
 * width0, height0, depth0 are the dimensions of the level 0 image
 * (the highest resolution).  last_level indicates how many mipmap levels
 * to allocate storage for.  For non-mipmapped textures, this will be zero.
 */
struct pipe_resource *
st_texture_create(struct st_context *st,
                  enum pipe_texture_target target,
		  enum pipe_format format,
		  GLuint last_level,
		  GLuint width0,
		  GLuint height0,
		  GLuint depth0,
                  GLuint bind,
                  GLuint flags)
{
   struct pipe_resource pt, *newtex;
   struct pipe_screen *screen = st->pipe->screen;

   assert(target <= PIPE_TEXTURE_CUBE);

   DBG("%s target %s format %s last_level %d\n", __FUNCTION__,
       _mesa_lookup_enum_by_nr(target),
       _mesa_lookup_enum_by_nr(format), last_level);

   assert(format);
   assert(screen->is_format_supported(screen, format, target, 
                                      PIPE_BIND_SAMPLER_VIEW, 0));

   memset(&pt, 0, sizeof(pt));
   pt.target = target;
   pt.format = format;
   pt.last_level = last_level;
   pt.width0 = width0;
   pt.height0 = height0;
   pt.depth0 = depth0;
   pt._usage = PIPE_USAGE_DEFAULT;
   pt.bind = bind;
   pt.flags = flags;

   newtex = screen->resource_create(screen, &pt);

   assert(!newtex || pipe_is_referenced(&newtex->reference));

   return newtex;
}


/**
 * Check if a texture image can be pulled into a unified mipmap texture.
 */
GLboolean
st_texture_match_image(const struct pipe_resource *pt,
                       const struct gl_texture_image *image,
                       GLuint face, GLuint level)
{
   /* Images with borders are never pulled into mipmap textures. 
    */
   if (image->Border) 
      return GL_FALSE;

   /* Check if this image's format matches the established texture's format.
    */
   if (st_mesa_format_to_pipe_format(image->TexFormat) != pt->format)
      return GL_FALSE;

   /* Test if this image's size matches what's expected in the
    * established texture.
    */
   if (image->Width != u_minify(pt->width0, level) ||
       image->Height != u_minify(pt->height0, level) ||
       image->Depth != u_minify(pt->depth0, level))
      return GL_FALSE;

   return GL_TRUE;
}


#if 000
/* Although we use the image_offset[] array to store relative offsets
 * to cube faces, Mesa doesn't know anything about this and expects
 * each cube face to be treated as a separate image.
 *
 * These functions present that view to mesa:
 */
const GLuint *
st_texture_depth_offsets(struct pipe_resource *pt, GLuint level)
{
   static const GLuint zero = 0;

   if (pt->target != PIPE_TEXTURE_3D || pt->level[level].nr_images == 1)
      return &zero;
   else
      return pt->level[level].image_offset;
}


/**
 * Return the offset to the given mipmap texture image within the
 * texture memory buffer, in bytes.
 */
GLuint
st_texture_image_offset(const struct pipe_resource * pt,
                        GLuint face, GLuint level)
{
   if (pt->target == PIPE_TEXTURE_CUBE)
      return (pt->level[level].level_offset +
              pt->level[level].image_offset[face] * pt->cpp);
   else
      return pt->level[level].level_offset;
}
#endif


/**
 * Map a teximage in a mipmap texture.
 * \param row_stride  returns row stride in bytes
 * \param image_stride  returns image stride in bytes (for 3D textures).
 * \return address of mapping
 */
GLubyte *
st_texture_image_map(struct st_context *st, struct st_texture_image *stImage,
		     GLuint zoffset, enum pipe_transfer_usage usage,
                     GLuint x, GLuint y, GLuint w, GLuint h)
{
   struct pipe_context *pipe = st->pipe;
   struct pipe_resource *pt = stImage->pt;

   DBG("%s \n", __FUNCTION__);

   stImage->transfer = st_no_flush_get_tex_transfer(st, pt, stImage->face,
						    stImage->level, zoffset,
						    usage, x, y, w, h);

   if (stImage->transfer)
      return pipe_transfer_map(pipe, stImage->transfer);
   else
      return NULL;
}


void
st_texture_image_unmap(struct st_context *st,
                       struct st_texture_image *stImage)
{
   struct pipe_context *pipe = st->pipe;

   DBG("%s\n", __FUNCTION__);

   pipe_transfer_unmap(pipe, stImage->transfer);

   pipe->transfer_destroy(pipe, stImage->transfer);
}



/**
 * Upload data to a rectangular sub-region.  Lots of choices how to do this:
 *
 * - memcpy by span to current destination
 * - upload data as new buffer and blit
 *
 * Currently always memcpy.
 */
static void
st_surface_data(struct pipe_context *pipe,
		struct pipe_transfer *dst,
		unsigned dstx, unsigned dsty,
		const void *src, unsigned src_stride,
		unsigned srcx, unsigned srcy, unsigned width, unsigned height)
{
   void *map = pipe_transfer_map(pipe, dst);

   assert(dst->resource);
   util_copy_rect(map,
                  dst->resource->format,
                  dst->stride,
                  dstx, dsty, 
                  width, height, 
                  src, src_stride, 
                  srcx, srcy);

   pipe_transfer_unmap(pipe, dst);
}


/* Upload data for a particular image.
 */
void
st_texture_image_data(struct st_context *st,
                      struct pipe_resource *dst,
                      GLuint face,
                      GLuint level,
                      void *src,
                      GLuint src_row_stride, GLuint src_image_stride)
{
   struct pipe_context *pipe = st->pipe;
   GLuint depth = u_minify(dst->depth0, level);
   GLuint i;
   const GLubyte *srcUB = src;
   struct pipe_transfer *dst_transfer;

   DBG("%s\n", __FUNCTION__);

   for (i = 0; i < depth; i++) {
      dst_transfer = st_no_flush_get_tex_transfer(st, dst, face, level, i,
						  PIPE_TRANSFER_WRITE, 0, 0,
						  u_minify(dst->width0, level),
                                                  u_minify(dst->height0, level));

      st_surface_data(pipe, dst_transfer,
		      0, 0,                             /* dstx, dsty */
		      srcUB,
		      src_row_stride,
		      0, 0,                             /* source x, y */
		      u_minify(dst->width0, level),
                      u_minify(dst->height0, level));      /* width, height */

      pipe->transfer_destroy(pipe, dst_transfer);

      srcUB += src_image_stride;
   }
}


/* Copy mipmap image between textures
 */
void
st_texture_image_copy(struct pipe_context *pipe,
                      struct pipe_resource *dst, GLuint dstLevel,
                      struct pipe_resource *src,
                      GLuint face)
{
   struct pipe_screen *screen = pipe->screen;
   GLuint width = u_minify(dst->width0, dstLevel); 
   GLuint height = u_minify(dst->height0, dstLevel); 
   GLuint depth = u_minify(dst->depth0, dstLevel); 
   struct pipe_surface *src_surface;
   struct pipe_surface *dst_surface;
   GLuint i;

   for (i = 0; i < depth; i++) {
      GLuint srcLevel;

      /* find src texture level of needed size */
      for (srcLevel = 0; srcLevel <= src->last_level; srcLevel++) {
         if (u_minify(src->width0, srcLevel) == width &&
             u_minify(src->height0, srcLevel) == height) {
            break;
         }
      }
      assert(u_minify(src->width0, srcLevel) == width);
      assert(u_minify(src->height0, srcLevel) == height);

#if 0
      {
         src_surface = screen->get_tex_surface(screen, src, face, srcLevel, i,
                                               PIPE_BUFFER_USAGE_CPU_READ);
         ubyte *map = screen->surface_map(screen, src_surface, PIPE_BUFFER_USAGE_CPU_READ);
         map += src_surface->width * src_surface->height * 4 / 2;
         printf("%s center pixel: %d %d %d %d (pt %p[%d] -> %p[%d])\n",
                __FUNCTION__,
                map[0], map[1], map[2], map[3],
                src, srcLevel, dst, dstLevel);

         screen->surface_unmap(screen, src_surface);
         pipe_surface_reference(&src_surface, NULL);
      }
#endif

      dst_surface = screen->get_tex_surface(screen, dst, face, dstLevel, i,
                                            PIPE_BIND_BLIT_DESTINATION);

      src_surface = screen->get_tex_surface(screen, src, face, srcLevel, i,
                                            PIPE_BIND_BLIT_SOURCE);

      pipe->surface_copy(pipe,
                         dst_surface,
                         0, 0, /* destX, Y */
                         src_surface,
                         0, 0, /* srcX, Y */
                         width, height);

      pipe_surface_reference(&src_surface, NULL);
      pipe_surface_reference(&dst_surface, NULL);
   }
}


void
st_teximage_flush_before_map(struct st_context *st,
			     struct pipe_resource *pt,
			     unsigned int face,
			     unsigned int level,
			     enum pipe_transfer_usage usage)
{
   struct pipe_context *pipe = st->pipe;
   unsigned referenced =
      pipe->is_resource_referenced(pipe, pt, face, level);

   if (referenced && ((referenced & PIPE_REFERENCED_FOR_WRITE) ||
		      (usage & PIPE_TRANSFER_WRITE)))
      st->pipe->flush(st->pipe, PIPE_FLUSH_RENDER_CACHE, NULL);
}
