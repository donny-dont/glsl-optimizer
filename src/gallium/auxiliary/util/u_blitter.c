/**************************************************************************
 *
 * Copyright 2009 Marek Olšák <maraeo@gmail.com>
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

/**
 * @file
 * Blitter utility to facilitate acceleration of the clear, surface_copy,
 * and surface_fill functions.
 *
 * @author Marek Olšák
 */

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_inlines.h"
#include "pipe/p_shader_tokens.h"
#include "pipe/p_state.h"

#include "util/u_memory.h"
#include "util/u_math.h"
#include "util/u_blitter.h"
#include "util/u_draw_quad.h"
#include "util/u_pack_color.h"
#include "util/u_rect.h"
#include "util/u_simple_shaders.h"
#include "util/u_texture.h"

struct blitter_context_priv
{
   struct blitter_context blitter;

   struct pipe_context *pipe; /**< pipe context */
   struct pipe_buffer *vbuf;  /**< quad */

   float vertices[4][2][4];   /**< {pos, color} or {pos, texcoord} */

   /* Constant state objects. */
   /* Vertex shaders. */
   void *vs_col; /**< Vertex shader which passes {pos, color} to the output */
   void *vs_tex; /**<Vertex shader which passes {pos, texcoord} to the output.*/

   /* Fragment shaders. */
   void *fs_col[8];     /**< FS which outputs colors to 1-8 color buffers */
   void *fs_texfetch_col[4];   /**< FS which outputs a color from a texture */
   void *fs_texfetch_depth[4]; /**< FS which outputs a depth from a texture,
                              where the index is PIPE_TEXTURE_* to be sampled */

   /* Blend state. */
   void *blend_write_color;   /**< blend state with writemask of RGBA */
   void *blend_keep_color;    /**< blend state with writemask of 0 */

   /* Depth stencil alpha state. */
   void *dsa_write_depth_stencil[0xff]; /**< indices are stencil clear values */
   void *dsa_write_depth_keep_stencil;
   void *dsa_keep_depth_stencil;

   /* Other state. */
   void *sampler_state[16];   /**< sampler state for clamping to a miplevel */
   void *rs_state;            /**< rasterizer state */
};

struct blitter_context *util_blitter_create(struct pipe_context *pipe)
{
   struct blitter_context_priv *ctx;
   struct pipe_blend_state blend;
   struct pipe_depth_stencil_alpha_state dsa;
   struct pipe_rasterizer_state rs_state;
   struct pipe_sampler_state sampler_state;
   unsigned i, max_render_targets;

   ctx = CALLOC_STRUCT(blitter_context_priv);
   if (!ctx)
      return NULL;

   ctx->pipe = pipe;

   /* init state objects for them to be considered invalid */
   ctx->blitter.saved_fb_state.nr_cbufs = ~0;
   ctx->blitter.saved_num_textures = ~0;
   ctx->blitter.saved_num_sampler_states = ~0;

   /* blend state objects */
   memset(&blend, 0, sizeof(blend));
   ctx->blend_keep_color = pipe->create_blend_state(pipe, &blend);

   blend.colormask = PIPE_MASK_RGBA;
   ctx->blend_write_color = pipe->create_blend_state(pipe, &blend);

   /* depth stencil alpha state objects */
   memset(&dsa, 0, sizeof(dsa));
   ctx->dsa_keep_depth_stencil =
      pipe->create_depth_stencil_alpha_state(pipe, &dsa);

   dsa.depth.enabled = 1;
   dsa.depth.writemask = 1;
   dsa.depth.func = PIPE_FUNC_ALWAYS;
   ctx->dsa_write_depth_keep_stencil =
      pipe->create_depth_stencil_alpha_state(pipe, &dsa);

   dsa.stencil[0].enabled = 1;
   dsa.stencil[0].func = PIPE_FUNC_ALWAYS;
   dsa.stencil[0].fail_op = PIPE_STENCIL_OP_REPLACE;
   dsa.stencil[0].zpass_op = PIPE_STENCIL_OP_REPLACE;
   dsa.stencil[0].zfail_op = PIPE_STENCIL_OP_REPLACE;
   dsa.stencil[0].valuemask = 0xff;
   dsa.stencil[0].writemask = 0xff;

   /* create a depth stencil alpha state for each possible stencil clear
    * value */
   for (i = 0; i < 0xff; i++) {
      dsa.stencil[0].ref_value = i;

      ctx->dsa_write_depth_stencil[i] =
         pipe->create_depth_stencil_alpha_state(pipe, &dsa);
   }

   /* sampler state */
   memset(&sampler_state, 0, sizeof(sampler_state));
   sampler_state.wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   sampler_state.wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
   sampler_state.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;

   for (i = 0; i < 16; i++) {
      sampler_state.lod_bias = i;
      sampler_state.min_lod = i;
      sampler_state.max_lod = i;

      ctx->sampler_state[i] = pipe->create_sampler_state(pipe, &sampler_state);
   }

   /* rasterizer state */
   memset(&rs_state, 0, sizeof(rs_state));
   rs_state.front_winding = PIPE_WINDING_CW;
   rs_state.cull_mode = PIPE_WINDING_NONE;
   rs_state.bypass_vs_clip_and_viewport = 1;
   rs_state.gl_rasterization_rules = 1;
   ctx->rs_state = pipe->create_rasterizer_state(pipe, &rs_state);

   /* vertex shaders */
   {
      const uint semantic_names[] = { TGSI_SEMANTIC_POSITION,
                                      TGSI_SEMANTIC_COLOR };
      const uint semantic_indices[] = { 0, 0 };
      ctx->vs_col =
         util_make_vertex_passthrough_shader(pipe, 2, semantic_names,
                                             semantic_indices);
   }
   {
      const uint semantic_names[] = { TGSI_SEMANTIC_POSITION,
                                      TGSI_SEMANTIC_GENERIC };
      const uint semantic_indices[] = { 0, 0 };
      ctx->vs_tex =
         util_make_vertex_passthrough_shader(pipe, 2, semantic_names,
                                             semantic_indices);
   }

   /* fragment shaders */
   ctx->fs_texfetch_col[PIPE_TEXTURE_1D] =
      util_make_fragment_tex_shader(pipe, TGSI_TEXTURE_1D);
   ctx->fs_texfetch_col[PIPE_TEXTURE_2D] =
      util_make_fragment_tex_shader(pipe, TGSI_TEXTURE_2D);
   ctx->fs_texfetch_col[PIPE_TEXTURE_3D] =
      util_make_fragment_tex_shader(pipe, TGSI_TEXTURE_3D);
   ctx->fs_texfetch_col[PIPE_TEXTURE_CUBE] =
      util_make_fragment_tex_shader(pipe, TGSI_TEXTURE_CUBE);

   ctx->fs_texfetch_depth[PIPE_TEXTURE_1D] =
      util_make_fragment_tex_shader_writedepth(pipe, TGSI_TEXTURE_1D);
   ctx->fs_texfetch_depth[PIPE_TEXTURE_2D] =
      util_make_fragment_tex_shader_writedepth(pipe, TGSI_TEXTURE_2D);
   ctx->fs_texfetch_depth[PIPE_TEXTURE_3D] =
      util_make_fragment_tex_shader_writedepth(pipe, TGSI_TEXTURE_3D);
   ctx->fs_texfetch_depth[PIPE_TEXTURE_CUBE] =
      util_make_fragment_tex_shader_writedepth(pipe, TGSI_TEXTURE_CUBE);

   max_render_targets = pipe->screen->get_param(pipe->screen,
                                                PIPE_CAP_MAX_RENDER_TARGETS);
   assert(max_render_targets <= 8);
   for (i = 0; i < max_render_targets; i++)
      ctx->fs_col[i] = util_make_fragment_clonecolor_shader(pipe, 1+i);

   /* set invariant vertex coordinates */
   for (i = 0; i < 4; i++)
      ctx->vertices[i][0][3] = 1; /*v.w*/

   /* create the vertex buffer */
   ctx->vbuf = pipe_buffer_create(ctx->pipe->screen,
                                  32,
                                  PIPE_BUFFER_USAGE_VERTEX,
                                  sizeof(ctx->vertices));

   return &ctx->blitter;
}

void util_blitter_destroy(struct blitter_context *blitter)
{
   struct blitter_context_priv *ctx = (struct blitter_context_priv*)blitter;
   struct pipe_context *pipe = ctx->pipe;
   int i;

   pipe->delete_blend_state(pipe, ctx->blend_write_color);
   pipe->delete_blend_state(pipe, ctx->blend_keep_color);
   pipe->delete_depth_stencil_alpha_state(pipe, ctx->dsa_keep_depth_stencil);
   pipe->delete_depth_stencil_alpha_state(pipe,
                                          ctx->dsa_write_depth_keep_stencil);

   for (i = 0; i < 0xff; i++)
      pipe->delete_depth_stencil_alpha_state(pipe,
                                             ctx->dsa_write_depth_stencil[i]);

   pipe->delete_rasterizer_state(pipe, ctx->rs_state);
   pipe->delete_vs_state(pipe, ctx->vs_col);
   pipe->delete_vs_state(pipe, ctx->vs_tex);

   for (i = 0; i < 4; i++) {
      pipe->delete_fs_state(pipe, ctx->fs_texfetch_col[i]);
      pipe->delete_fs_state(pipe, ctx->fs_texfetch_depth[i]);
   }
   for (i = 0; i < 8 && ctx->fs_col[i]; i++)
      pipe->delete_fs_state(pipe, ctx->fs_col[i]);

   pipe_buffer_reference(&ctx->vbuf, NULL);
   FREE(ctx);
}

static void blitter_check_saved_CSOs(struct blitter_context_priv *ctx)
{
   /* make sure these CSOs have been saved */
   assert(ctx->blitter.saved_blend_state &&
          ctx->blitter.saved_dsa_state &&
          ctx->blitter.saved_rs_state &&
          ctx->blitter.saved_fs &&
          ctx->blitter.saved_vs);
}

static void blitter_restore_CSOs(struct blitter_context_priv *ctx)
{
   struct pipe_context *pipe = ctx->pipe;

   /* restore the state objects which are always required to be saved */
   pipe->bind_blend_state(pipe, ctx->blitter.saved_blend_state);
   pipe->bind_depth_stencil_alpha_state(pipe, ctx->blitter.saved_dsa_state);
   pipe->bind_rasterizer_state(pipe, ctx->blitter.saved_rs_state);
   pipe->bind_fs_state(pipe, ctx->blitter.saved_fs);
   pipe->bind_vs_state(pipe, ctx->blitter.saved_vs);

   ctx->blitter.saved_blend_state = 0;
   ctx->blitter.saved_dsa_state = 0;
   ctx->blitter.saved_rs_state = 0;
   ctx->blitter.saved_fs = 0;
   ctx->blitter.saved_vs = 0;

   /* restore the state objects which are required to be saved before copy/fill
    */
   if (ctx->blitter.saved_fb_state.nr_cbufs != ~0) {
      pipe->set_framebuffer_state(pipe, &ctx->blitter.saved_fb_state);
      ctx->blitter.saved_fb_state.nr_cbufs = ~0;
   }

   if (ctx->blitter.saved_num_sampler_states != ~0) {
      pipe->bind_fragment_sampler_states(pipe,
                                         ctx->blitter.saved_num_sampler_states,
                                         ctx->blitter.saved_sampler_states);
      ctx->blitter.saved_num_sampler_states = ~0;
   }

   if (ctx->blitter.saved_num_textures != ~0) {
      pipe->set_fragment_sampler_textures(pipe,
                                          ctx->blitter.saved_num_textures,
                                          ctx->blitter.saved_textures);
      ctx->blitter.saved_num_textures = ~0;
   }
}

static void blitter_set_rectangle(struct blitter_context_priv *ctx,
                                  unsigned x1, unsigned y1,
                                  unsigned x2, unsigned y2,
                                  float depth)
{
   int i;

   /* set vertex positions */
   ctx->vertices[0][0][0] = x1; /*v0.x*/
   ctx->vertices[0][0][1] = y1; /*v0.y*/

   ctx->vertices[1][0][0] = x2; /*v1.x*/
   ctx->vertices[1][0][1] = y1; /*v1.y*/

   ctx->vertices[2][0][0] = x2; /*v2.x*/
   ctx->vertices[2][0][1] = y2; /*v2.y*/

   ctx->vertices[3][0][0] = x1; /*v3.x*/
   ctx->vertices[3][0][1] = y2; /*v3.y*/

   for (i = 0; i < 4; i++)
      ctx->vertices[i][0][2] = depth; /*z*/
}

static void blitter_set_clear_color(struct blitter_context_priv *ctx,
                                    const float *rgba)
{
   int i;

   for (i = 0; i < 4; i++) {
      ctx->vertices[i][1][0] = rgba[0];
      ctx->vertices[i][1][1] = rgba[1];
      ctx->vertices[i][1][2] = rgba[2];
      ctx->vertices[i][1][3] = rgba[3];
   }
}

static void blitter_set_texcoords_2d(struct blitter_context_priv *ctx,
                                     struct pipe_surface *surf,
                                     unsigned x1, unsigned y1,
                                     unsigned x2, unsigned y2)
{
   int i;
   float s1 = x1 / (float)surf->width;
   float t1 = y1 / (float)surf->height;
   float s2 = x2 / (float)surf->width;
   float t2 = y2 / (float)surf->height;

   ctx->vertices[0][1][0] = s1; /*t0.s*/
   ctx->vertices[0][1][1] = t1; /*t0.t*/

   ctx->vertices[1][1][0] = s2; /*t1.s*/
   ctx->vertices[1][1][1] = t1; /*t1.t*/

   ctx->vertices[2][1][0] = s2; /*t2.s*/
   ctx->vertices[2][1][1] = t2; /*t2.t*/

   ctx->vertices[3][1][0] = s1; /*t3.s*/
   ctx->vertices[3][1][1] = t2; /*t3.t*/

   for (i = 0; i < 4; i++) {
      ctx->vertices[i][1][2] = 0; /*r*/
      ctx->vertices[i][1][3] = 1; /*q*/
   }
}

static void blitter_set_texcoords_3d(struct blitter_context_priv *ctx,
                                     struct pipe_surface *surf,
                                     unsigned x1, unsigned y1,
                                     unsigned x2, unsigned y2)
{
   int i;
   float depth = u_minify(surf->texture->depth0, surf->level);
   float r = surf->zslice / depth;

   blitter_set_texcoords_2d(ctx, surf, x1, y1, x2, y2);

   for (i = 0; i < 4; i++)
      ctx->vertices[i][1][2] = r; /*r*/
}

static void blitter_set_texcoords_cube(struct blitter_context_priv *ctx,
                                       struct pipe_surface *surf,
                                       unsigned x1, unsigned y1,
                                       unsigned x2, unsigned y2)
{
   int i;
   float s1 = x1 / (float)surf->width;
   float t1 = y1 / (float)surf->height;
   float s2 = x2 / (float)surf->width;
   float t2 = y2 / (float)surf->height;
   const float st[4][2] = {
      {s1, t1}, {s2, t1}, {s2, t2}, {s1, t2}
   };

   util_map_texcoords2d_onto_cubemap(surf->face,
                                     /* pointer, stride in floats */
                                     &st[0][0], 2,
                                     &ctx->vertices[0][1][0], 8);

   for (i = 0; i < 4; i++)
      ctx->vertices[i][1][3] = 1; /*q*/
}

static void blitter_draw_quad(struct blitter_context_priv *ctx)
{
   struct blitter_context *blitter = &ctx->blitter;
   struct pipe_context *pipe = ctx->pipe;

   if (blitter->draw_quad) {
      blitter->draw_quad(pipe, &ctx->vertices[0][0][0]);
   } else {
      /* write vertices and draw them */
      pipe_buffer_write(pipe->screen, ctx->vbuf,
                        0, sizeof(ctx->vertices), ctx->vertices);

      util_draw_vertex_buffer(ctx->pipe, ctx->vbuf, 0, PIPE_PRIM_TRIANGLE_FAN,
                              4,  /* verts */
                              2); /* attribs/vert */
   }
}

void util_blitter_clear(struct blitter_context *blitter,
                        unsigned width, unsigned height,
                        unsigned num_cbufs,
                        unsigned clear_buffers,
                        const float *rgba,
                        double depth, unsigned stencil)
{
   struct blitter_context_priv *ctx = (struct blitter_context_priv*)blitter;
   struct pipe_context *pipe = ctx->pipe;

   assert(num_cbufs <= 8);

   blitter_check_saved_CSOs(ctx);

   /* bind CSOs */
   if (clear_buffers & PIPE_CLEAR_COLOR)
      pipe->bind_blend_state(pipe, ctx->blend_write_color);
   else
      pipe->bind_blend_state(pipe, ctx->blend_keep_color);

   if (clear_buffers & PIPE_CLEAR_DEPTHSTENCIL)
      pipe->bind_depth_stencil_alpha_state(pipe,
         ctx->dsa_write_depth_stencil[stencil&0xff]);
   else
      pipe->bind_depth_stencil_alpha_state(pipe, ctx->dsa_keep_depth_stencil);

   pipe->bind_rasterizer_state(pipe, ctx->rs_state);
   pipe->bind_fs_state(pipe, ctx->fs_col[num_cbufs ? num_cbufs-1 : 0]);
   pipe->bind_vs_state(pipe, ctx->vs_col);

   blitter_set_clear_color(ctx, rgba);
   blitter_set_rectangle(ctx, 0, 0, width, height, depth);
   blitter_draw_quad(ctx);
   blitter_restore_CSOs(ctx);
}

void util_blitter_copy(struct blitter_context *blitter,
                       struct pipe_surface *dst,
                       unsigned dstx, unsigned dsty,
                       struct pipe_surface *src,
                       unsigned srcx, unsigned srcy,
                       unsigned width, unsigned height,
                       boolean ignore_stencil)
{
   struct blitter_context_priv *ctx = (struct blitter_context_priv*)blitter;
   struct pipe_context *pipe = ctx->pipe;
   struct pipe_screen *screen = pipe->screen;
   struct pipe_framebuffer_state fb_state;
   boolean is_stencil, is_depth;
   unsigned dst_tex_usage;

   /* give up if textures are not set */
   assert(dst->texture && src->texture);
   if (!dst->texture || !src->texture)
      return;

   is_depth = pf_get_component_bits(src->format, PIPE_FORMAT_COMP_Z) != 0;
   is_stencil = pf_get_component_bits(src->format, PIPE_FORMAT_COMP_S) != 0;
   dst_tex_usage = is_depth || is_stencil ? PIPE_TEXTURE_USAGE_DEPTH_STENCIL :
                                            PIPE_TEXTURE_USAGE_RENDER_TARGET;

   /* check if we can sample from and render to the surfaces */
   /* (assuming copying a stencil buffer is not possible) */
   if ((!ignore_stencil && is_stencil) ||
       !screen->is_format_supported(screen, dst->format, dst->texture->target,
                                    dst_tex_usage, 0) ||
       !screen->is_format_supported(screen, src->format, src->texture->target,
                                    PIPE_TEXTURE_USAGE_SAMPLER, 0)) {
      util_surface_copy(pipe, FALSE, dst, dstx, dsty, src, srcx, srcy,
                        width, height);
      return;
   }

   /* check whether the states are properly saved */
   blitter_check_saved_CSOs(ctx);
   assert(blitter->saved_fb_state.nr_cbufs != ~0);
   assert(blitter->saved_num_textures != ~0);
   assert(blitter->saved_num_sampler_states != ~0);
   assert(src->texture->target < 4);

   /* bind CSOs */
   fb_state.width = dst->width;
   fb_state.height = dst->height;

   if (is_depth) {
      pipe->bind_blend_state(pipe, ctx->blend_keep_color);
      pipe->bind_depth_stencil_alpha_state(pipe,
                                           ctx->dsa_write_depth_keep_stencil);
      pipe->bind_fs_state(pipe, ctx->fs_texfetch_depth[src->texture->target]);

      fb_state.nr_cbufs = 0;
      fb_state.zsbuf = dst;
   } else {
      pipe->bind_blend_state(pipe, ctx->blend_write_color);
      pipe->bind_depth_stencil_alpha_state(pipe, ctx->dsa_keep_depth_stencil);
      pipe->bind_fs_state(pipe, ctx->fs_texfetch_col[src->texture->target]);

      fb_state.nr_cbufs = 1;
      fb_state.cbufs[0] = dst;
      fb_state.zsbuf = 0;
   }
   pipe->bind_rasterizer_state(pipe, ctx->rs_state);
   pipe->bind_vs_state(pipe, ctx->vs_tex);
   pipe->bind_fragment_sampler_states(pipe, 1, &ctx->sampler_state[src->level]);
   pipe->set_fragment_sampler_textures(pipe, 1, &src->texture);
   pipe->set_framebuffer_state(pipe, &fb_state);

   /* set texture coordinates */
   switch (src->texture->target) {
      case PIPE_TEXTURE_1D:
      case PIPE_TEXTURE_2D:
         blitter_set_texcoords_2d(ctx, src, srcx, srcy,
                                  srcx+width, srcy+height);
         break;
      case PIPE_TEXTURE_3D:
         blitter_set_texcoords_3d(ctx, src, srcx, srcy,
                                  srcx+width, srcy+height);
         break;
      case PIPE_TEXTURE_CUBE:
         blitter_set_texcoords_cube(ctx, src, srcx, srcy,
                                    srcx+width, srcy+height);
         break;
   }

   blitter_set_rectangle(ctx, dstx, dsty, dstx+width, dsty+height, 0);
   blitter_draw_quad(ctx);
   blitter_restore_CSOs(ctx);
}

void util_blitter_fill(struct blitter_context *blitter,
                       struct pipe_surface *dst,
                       unsigned dstx, unsigned dsty,
                       unsigned width, unsigned height,
                       unsigned value)
{
   struct blitter_context_priv *ctx = (struct blitter_context_priv*)blitter;
   struct pipe_context *pipe = ctx->pipe;
   struct pipe_screen *screen = pipe->screen;
   struct pipe_framebuffer_state fb_state;
   float rgba[4];
   ubyte ub_rgba[4] = {0};
   union util_color color;
   int i;

   assert(dst->texture);
   if (!dst->texture)
      return;

   /* check if we can render to the surface */
   if (pf_is_depth_or_stencil(dst->format) || /* unlikely, but you never know */
       !screen->is_format_supported(screen, dst->format, dst->texture->target,
                                    PIPE_TEXTURE_USAGE_RENDER_TARGET, 0)) {
      util_surface_fill(pipe, dst, dstx, dsty, width, height, value);
      return;
   }

   /* unpack the color */
   color.ui = value;
   util_unpack_color_ub(dst->format, &color,
                        ub_rgba, ub_rgba+1, ub_rgba+2, ub_rgba+3);
   for (i = 0; i < 4; i++)
      rgba[i] = ubyte_to_float(ub_rgba[i]);

   /* check the saved state */
   blitter_check_saved_CSOs(ctx);
   assert(blitter->saved_fb_state.nr_cbufs != ~0);

   /* bind CSOs */
   pipe->bind_blend_state(pipe, ctx->blend_write_color);
   pipe->bind_depth_stencil_alpha_state(pipe, ctx->dsa_keep_depth_stencil);
   pipe->bind_rasterizer_state(pipe, ctx->rs_state);
   pipe->bind_fs_state(pipe, ctx->fs_col[0]);
   pipe->bind_vs_state(pipe, ctx->vs_col);

   /* set a framebuffer state */
   fb_state.width = dst->width;
   fb_state.height = dst->height;
   fb_state.nr_cbufs = 1;
   fb_state.cbufs[0] = dst;
   fb_state.zsbuf = 0;
   pipe->set_framebuffer_state(pipe, &fb_state);

   blitter_set_clear_color(ctx, rgba);
   blitter_set_rectangle(ctx, 0, 0, width, height, 0);
   blitter_draw_quad(ctx);
   blitter_restore_CSOs(ctx);
}