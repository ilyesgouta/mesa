/**************************************************************************
 * 
 * Copyright 2007 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 * Copyright 2010 VMware, Inc.  All rights reserved.
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

#ifndef SP_TEX_SAMPLE_H
#define SP_TEX_SAMPLE_H


#include "tgsi/tgsi_exec.h"

struct sp_sampler_variant;

typedef void (*wrap_nearest_func)(float s,
                                  unsigned size,
                                  int *icoord);

typedef void (*wrap_linear_func)(float s, 
                                 unsigned size,
                                 int *icoord0,
                                 int *icoord1,
                                 float *w);

typedef float (*compute_lambda_func)(const struct sp_sampler_variant *samp,
                                     const float s[TGSI_QUAD_SIZE],
                                     const float t[TGSI_QUAD_SIZE],
                                     const float p[TGSI_QUAD_SIZE]);

typedef void (*img_filter_func)(struct sp_sampler_variant *samp,
                                float s,
                                float t,
                                float p,
                                unsigned level,
                                unsigned face_id,
                                float *rgba);

typedef void (*filter_func)(struct sp_sampler_variant *sp_samp,
                            const float s[TGSI_QUAD_SIZE],
                            const float t[TGSI_QUAD_SIZE],
                            const float p[TGSI_QUAD_SIZE],
                            const float c0[TGSI_QUAD_SIZE],
                            const float lod[TGSI_QUAD_SIZE],
                            enum tgsi_sampler_control control,
                            float rgba[TGSI_NUM_CHANNELS][TGSI_QUAD_SIZE]);


typedef void (*get_dim_func)(struct sp_sampler_variant *sp_samp,
                             int level, int dims[4]);

typedef void (*fetch_func)(struct sp_sampler_variant *sp_samp,
                           const int i[TGSI_QUAD_SIZE],
                           const int j[TGSI_QUAD_SIZE], const int k[TGSI_QUAD_SIZE],
                           const int lod[TGSI_QUAD_SIZE], const int8_t offset[3],
                           float rgba[TGSI_NUM_CHANNELS][TGSI_QUAD_SIZE]);


union sp_sampler_key {
   struct {
      unsigned target:5;
      unsigned is_pot:1;
      unsigned processor:2;
      unsigned unit:4;
      unsigned swizzle_r:3;
      unsigned swizzle_g:3;
      unsigned swizzle_b:3;
      unsigned swizzle_a:3;
      unsigned pad:8;
   } bits;
   unsigned value;
};


struct sp_sampler_variant
{
   union sp_sampler_key key;

   /* The owner of this struct:
    */
   const struct pipe_sampler_state *sampler;


   /* Currently bound texture:
    */
   const struct pipe_sampler_view *view;
   struct softpipe_tex_tile_cache *cache;

   /* For sp_get_samples_2d_linear_POT:
    */
   unsigned xpot;
   unsigned ypot;

   unsigned faces[TGSI_QUAD_SIZE];
   
   wrap_nearest_func nearest_texcoord_s;
   wrap_nearest_func nearest_texcoord_t;
   wrap_nearest_func nearest_texcoord_p;

   wrap_linear_func linear_texcoord_s;
   wrap_linear_func linear_texcoord_t;
   wrap_linear_func linear_texcoord_p;

   img_filter_func min_img_filter;
   img_filter_func mag_img_filter;

   compute_lambda_func compute_lambda;

   filter_func mip_filter;
   filter_func compare;
   filter_func sample_target;

   filter_func get_samples;
   fetch_func get_texel;
   get_dim_func get_dims;


   /* Linked list:
    */
   struct sp_sampler_variant *next;
};


/**
 * Subclass of tgsi_sampler
 */
struct sp_tgsi_sampler
{
   struct tgsi_sampler base;  /**< base class */
   struct sp_sampler_variant *sp_sampler[PIPE_MAX_SAMPLERS];

};


struct sp_sampler;

/* Create a sampler variant for a given set of non-orthogonal state.  Currently the 
 */
struct sp_sampler_variant *
sp_create_sampler_variant( const struct pipe_sampler_state *sampler,
                           const union sp_sampler_key key );

void sp_sampler_variant_bind_view( struct sp_sampler_variant *variant,
                                   struct softpipe_tex_tile_cache *tex_cache,
                                   const struct pipe_sampler_view *view );

void sp_sampler_variant_destroy( struct sp_sampler_variant * );



static INLINE struct sp_sampler_variant *
sp_sampler_variant(const struct tgsi_sampler *sampler)
{
   return (struct sp_sampler_variant *) sampler;
}

extern void
sp_get_samples(struct tgsi_sampler *tgsi_sampler,
               const float s[TGSI_QUAD_SIZE],
               const float t[TGSI_QUAD_SIZE],
               const float p[TGSI_QUAD_SIZE],
               float lodbias,
               float rgba[TGSI_NUM_CHANNELS][TGSI_QUAD_SIZE]);


struct sp_tgsi_sampler *
sp_create_tgsi_sampler(void);


#endif /* SP_TEX_SAMPLE_H */
