/* vim: set sw=4: -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
   rsvg-shapes.c: Draw SVG shapes

   Copyright (C) 2000 Eazel, Inc.
   Copyright (C) 2002 Dom Lachowicz <cinamod@hotmail.com>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this program; if not, write to the
   Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.

   Author: Raph Levien <raph@artofcode.com>
*/
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdio.h>

#include "rsvg-private.h"
#include "rsvg-styles.h"
#include "rsvg-shapes.h"
#include "rsvg-css.h"
#include "rsvg-bpath-util.h"
#include "rsvg-path.h"
#include "rsvg-defs.h"
#include "rsvg-filter.h"
#include "rsvg-mask.h"

#include <libart_lgpl/art_affine.h>
#include <libart_lgpl/art_vpath_bpath.h>
#include <libart_lgpl/art_render_svp.h>
#include <libart_lgpl/art_svp_vpath.h>
#include <libart_lgpl/art_svp_intersect.h>
#include <libart_lgpl/art_svp_ops.h>
#include <libart_lgpl/art_svp_vpath.h>
#include <libart_lgpl/art_rgb_affine.h>
#include <libart_lgpl/art_rgb_rgba_affine.h>
#include <libart_lgpl/art_rgb_svp.h>

/* 4/3 * (1-cos 45)/sin 45 = 4/3 * sqrt(2) - 1 */
#define RSVG_ARC_MAGIC ((double) 0.5522847498)

/**
 * rsvg_close_vpath: Close a vector path.
 * @src: Source vector path.
 *
 * Closes any open subpaths in the vector path.
 *
 * Return value: Closed vector path, allocated with g_new.
 **/
static ArtVpath *
rsvg_close_vpath (const ArtVpath *src)
{
	ArtVpath *result;
	int n_result, n_result_max;
	int src_ix;
	double beg_x, beg_y;
	gboolean open;
	
	n_result = 0;
	n_result_max = 16;
	result = g_new (ArtVpath, n_result_max);
	
	beg_x = 0;
	beg_y = 0;
	open = FALSE;
	
	for (src_ix = 0; src[src_ix].code != ART_END; src_ix++)
		{
			if (n_result == n_result_max)
				result = g_renew (ArtVpath, result, n_result_max <<= 1);
			result[n_result].code = src[src_ix].code == ART_MOVETO_OPEN ?
				ART_MOVETO : src[src_ix].code;
			result[n_result].x = src[src_ix].x;
			result[n_result].y = src[src_ix].y;
			n_result++;
			if (src[src_ix].code == ART_MOVETO_OPEN)
				{
					beg_x = src[src_ix].x;
					beg_y = src[src_ix].y;
					open = TRUE;
				}
			else if (src[src_ix + 1].code != ART_LINETO)
				{
					if (open && (beg_x != src[src_ix].x || beg_y != src[src_ix].y))
						{
							if (n_result == n_result_max)
								result = g_renew (ArtVpath, result, n_result_max <<= 1);
							result[n_result].code = ART_LINETO;
							result[n_result].x = beg_x;
							result[n_result].y = beg_y;
							n_result++;
						}
					open = FALSE;
				}
		}
	if (n_result == n_result_max)
		result = g_renew (ArtVpath, result, n_result_max <<= 1);
	result[n_result].code = ART_END;
	result[n_result].x = 0.0;
	result[n_result].y = 0.0;
	return result;
}

/* calculates how big an svp is */
static ArtIRect
rsvg_calculate_svp_bounds (const ArtSVP *svp)
{
	int i, j;	
	int bigx, littlex, bigy, littley, assignedonce;
	ArtIRect output;

	bigx = littlex = bigy = littley = assignedonce = 0;	

	for (i = 0; i < svp->n_segs; i++)
		for (j = 0; j < svp->segs[i].n_points; j++)
			{
				if (!assignedonce)
					{
						bigx = svp->segs[i].points[j].x;
						littlex = svp->segs[i].points[j].x;
						bigy = svp->segs[i].points[j].y; 
						littley = svp->segs[i].points[j].y;
						assignedonce = 1;
					}
				if (svp->segs[i].points[j].x > bigx)
					bigx = svp->segs[i].points[j].x;
				if (svp->segs[i].points[j].x < littlex)
					littlex = svp->segs[i].points[j].x;
				if (svp->segs[i].points[j].y > bigy)
					bigy = svp->segs[i].points[j].y; 
				if (svp->segs[i].points[j].y < littley)
					littley = svp->segs[i].points[j].y;
			}
	output.x0 = littlex;
	output.y0 = littley;
	output.x1 = bigx;
	output.y1 = bigy;
	return output;
}

/**
 * rsvg_render_svp: Render an SVP.
 * @ctx: Context in which to render.
 * @svp: SVP to render.
 * @ps: Paint server for rendering.
 * @opacity: Opacity as 0..0xff.
 *
 * Renders the SVP over the pixbuf in @ctx.
 **/
static void
rsvg_render_svp (RsvgHandle *ctx, ArtSVP *svp,
				 RsvgPaintServer *ps, int opacity)
{
	GdkPixbuf *pixbuf;
	ArtRender *render;
	gboolean has_alpha;
	ArtIRect temprect;
	RsvgPSCtx gradctx;
	RsvgState *state;
	int i;	

	rsvg_state_clip_path_assure(ctx);

	pixbuf = ctx->pixbuf;
	if (pixbuf == NULL)
		{
			/* FIXME: What warning/GError here? */
			return;
		}
	
	state = rsvg_state_current(ctx);

	has_alpha = gdk_pixbuf_get_has_alpha (pixbuf);

	render = art_render_new (0, 0,
							 gdk_pixbuf_get_width (pixbuf),
							 gdk_pixbuf_get_height (pixbuf),
							 gdk_pixbuf_get_pixels (pixbuf),
							 gdk_pixbuf_get_rowstride (pixbuf),
							 gdk_pixbuf_get_n_channels (pixbuf) -
							 (has_alpha ? 1 : 0),
							 gdk_pixbuf_get_bits_per_sample (pixbuf),
							 has_alpha ? ART_ALPHA_SEPARATE : ART_ALPHA_NONE,
							 NULL);

	temprect = rsvg_calculate_svp_bounds(svp);
	
	if (state->clippath != NULL)
		{
			ArtSVP * svpx;
			svpx = art_svp_intersect(svp, state->clippath);
			svp = svpx;
		}
	
	art_render_svp (render, svp);
	art_render_mask_solid (render, (opacity << 8) + opacity + (opacity >> 7));

	art_irect_union(&ctx->bbox, &ctx->bbox, &temprect);

	gradctx.x0 = temprect.x0;
	gradctx.y0 = temprect.y0;
	gradctx.x1 = temprect.x1;
	gradctx.y1 = temprect.y1;
	gradctx.ctx = ctx;

	for (i = 0; i < 6; i++)
		gradctx.affine[i] = state->affine[i];
	
	gradctx.color = state->current_color;
	rsvg_render_paint_server (render, ps, &gradctx);
	art_render_invoke (render);

	if (state->clippath != NULL) /*we don't need svpx any more*/
		art_free(svp);
}

static ArtSVP *
rsvg_render_filling (RsvgState *state, const ArtVpath *vpath)
{
			ArtVpath *closed_vpath;
			ArtSVP *svp2, *svp;
			ArtSvpWriter *swr;
			
			closed_vpath = rsvg_close_vpath (vpath);
			svp = art_svp_from_vpath (closed_vpath);
			g_free (closed_vpath);
			
			if (state->fill_rule == FILL_RULE_EVENODD)
				swr = art_svp_writer_rewind_new (ART_WIND_RULE_ODDEVEN);
			else /* state->fill_rule == FILL_RULE_NONZERO */
				swr = art_svp_writer_rewind_new (ART_WIND_RULE_NONZERO);

			art_svp_intersector (svp, swr);
			
			svp2 = art_svp_writer_rewind_reap (swr);
			art_svp_free (svp);

			return svp2;
}

static ArtSVP *
rsvg_render_outline (RsvgState *state, ArtVpath *vpath)
{
	ArtSVP * output;

	/* todo: libart doesn't yet implement anamorphic scaling of strokes */
	double stroke_width = state->stroke_width *
		art_affine_expansion (state->affine);

	if (stroke_width < 0.25)
		stroke_width = 0.25;
	
	/* if the path is dashed, stroke it */
	if (state->dash.n_dash > 0) 
		{
			ArtVpath * dashed_vpath = art_vpath_dash (vpath, &state->dash);
			vpath = dashed_vpath;
		}
	
	output = art_svp_vpath_stroke (vpath, state->join, state->cap,
								   stroke_width, state->miter_limit, 0.25);

	if (state->dash.n_dash > 0) 
		art_free (vpath);
	return output;
}

static void
rsvg_render_bpath (RsvgHandle *ctx, const ArtBpath *bpath)
{
	RsvgState *state;
	ArtBpath *affine_bpath;
	ArtVpath *vpath;
	ArtSVP *svp;
	GdkPixbuf *pixbuf;
	gboolean need_tmpbuf;
	int opacity;
	int tmp;

	pixbuf = ctx->pixbuf;
	if (pixbuf == NULL)
		{
			/* FIXME: What warning/GError here? */
			return;
		}
	
	state = rsvg_state_current (ctx);

	/* todo: handle visibility stuff earlier for performance benefits 
	 * handles all path based shapes. will handle text and images separately
	 */
	if (!state->visible || !state->cond_true)
		return;

	affine_bpath = art_bpath_affine_transform (bpath,
											   state->affine);
	
	vpath = art_bez_path_to_vec (affine_bpath, 0.25);
	art_free (affine_bpath);
	
	need_tmpbuf = ((state->fill != NULL) && (state->stroke != NULL) &&
				   state->opacity != 0xff) || rsvg_needs_discrete_layer(state);
	
	if (need_tmpbuf)
		rsvg_push_discrete_layer (ctx);
	
	if (state->fill != NULL)
		{

			opacity = state->fill_opacity;
			if (!need_tmpbuf && state->opacity != 0xff)
				{
					tmp = opacity * state->opacity + 0x80;
					opacity = (tmp + (tmp >> 8)) >> 8;
				}
			svp = rsvg_render_filling(state, vpath);
			rsvg_render_svp (ctx, svp, state->fill, opacity);
			art_svp_free (svp);
		}
	
	if (state->stroke != NULL)
		{
			opacity = state->stroke_opacity;
			if (!need_tmpbuf && state->opacity != 0xff)
				{
					tmp = opacity * state->opacity + 0x80;
					opacity = (tmp + (tmp >> 8)) >> 8;
				}
			svp = rsvg_render_outline(state, vpath);
			rsvg_render_svp (ctx, svp, state->stroke, opacity);
			art_svp_free (svp);
		}

	if (need_tmpbuf)
		rsvg_pop_discrete_layer (ctx);	
	
	art_free (vpath);
}

static ArtSVP *
rsvg_render_bpath_into_svp (RsvgHandle *ctx, const ArtBpath *bpath)
{
	RsvgState *state;
	ArtBpath *affine_bpath;
	ArtVpath *vpath;
	ArtSVP *svp;
	
	state = rsvg_state_current (ctx);

	affine_bpath = art_bpath_affine_transform (bpath,
											   state->affine);

	vpath = art_bez_path_to_vec (affine_bpath, 0.25);
	art_free (affine_bpath);
	state->fill_rule = state->clip_rule;

	svp = rsvg_render_filling(state, vpath);

	art_free (vpath);
	return svp;
}

static void
rsvg_render_markers(RsvgBpathDef * bpath_def, RsvgHandle *ctx)
{
	int i;

	double x, y;
	double lastx, lasty;
	double nextx, nexty;	
    double linewidth;

	RsvgState * state;
	RsvgMarker * startmarker;
	RsvgMarker * middlemarker;
	RsvgMarker * endmarker;

	state = rsvg_state_current(ctx);
	
	linewidth = state->stroke_width;
	startmarker = (RsvgMarker *)state->startMarker;
	middlemarker = (RsvgMarker *)state->middleMarker;
	endmarker = (RsvgMarker *)state->endMarker;

	if (!startmarker && !middlemarker && !endmarker)
		return;

	x = 0;
	y = 0;
	nextx = state->affine[0] * bpath_def->bpath[0].x3 + 
		state->affine[2] * bpath_def->bpath[0].y3 + state->affine[4];
	nexty = state->affine[1] * bpath_def->bpath[0].x3 + 
		state->affine[3] * bpath_def->bpath[0].y3 + state->affine[5];

	for (i = 0; i < bpath_def->n_bpath - 1; i++)
		{
			lastx = x;
			lasty = y;
			x = nextx;
			y = nexty;
			nextx = state->affine[0] * bpath_def->bpath[i + 1].x3 + 
				state->affine[2] * bpath_def->bpath[i + 1].y3 + state->affine[4];
			nexty = state->affine[1] * bpath_def->bpath[i + 1].x3 + 
				state->affine[3] * bpath_def->bpath[i + 1].y3 + state->affine[5];

			
			if(bpath_def->bpath[i + 1].code == ART_MOVETO || 
					bpath_def->bpath[i + 1].code == ART_MOVETO_OPEN || 
					bpath_def->bpath[i + 1].code == ART_END)
				{
					if (endmarker)
						rsvg_marker_render (endmarker, x, y, atan2(y - lasty, x - lastx), linewidth, ctx);
				}
			else if (bpath_def->bpath[i].code == ART_MOVETO || bpath_def->bpath[i].code == ART_MOVETO_OPEN)
				{		
					if (startmarker)
						rsvg_marker_render (startmarker, x, y, atan2(nexty - y, nextx - x), linewidth, ctx);
				}
			else
				{			
					if (middlemarker)
						{
							double xdifin, ydifin, xdifout, ydifout, intot, outtot, angle;
							
							xdifin = x - lastx;
							ydifin = y - lasty;
							xdifout = nextx - x;
							ydifout = nexty - y;
							
							intot = sqrt(xdifin * xdifin + ydifin * ydifin);
							outtot = sqrt(xdifout * xdifout + ydifout * ydifout);
							
							xdifin /= intot;
							ydifin /= intot;
							xdifout /= outtot;
							ydifout /= outtot;
							
							angle = atan2((ydifin + ydifout) / 2, (xdifin + xdifout) / 2);
							rsvg_marker_render (middlemarker, x, y, angle, linewidth, ctx);
						}
				}
		}
}

void
rsvg_render_path(RsvgHandle *ctx, const char *d)
{
	RsvgBpathDef *bpath_def;
	
	bpath_def = rsvg_parse_path (d);
	rsvg_bpath_def_art_finish (bpath_def);
	
	rsvg_render_bpath (ctx, bpath_def->bpath);
	
	rsvg_render_markers(bpath_def, ctx);

	rsvg_bpath_def_free (bpath_def);
}

static ArtSVP *
rsvg_render_path_as_svp(RsvgHandle *ctx, const char *d)
{
	RsvgBpathDef *bpath_def;
	ArtSVP * output;
	
	bpath_def = rsvg_parse_path (d);
	rsvg_bpath_def_art_finish (bpath_def);
	
	output = rsvg_render_bpath_into_svp (ctx, bpath_def->bpath);

	rsvg_bpath_def_free (bpath_def);
	return output;
}

void 
rsvg_defs_drawable_draw (RsvgDefsDrawable * self, RsvgHandle *ctx,
						 int dominate)
{
	self->draw(self, ctx, dominate);
}

ArtSVP *
rsvg_defs_drawable_draw_as_svp (RsvgDefsDrawable * self, RsvgHandle *ctx,
						 int dominate)
{
	return self->draw_as_svp(self, ctx, dominate);
}

static void 
rsvg_defs_drawable_path_free (RsvgDefVal *self)
{
	RsvgDefsDrawablePath *z = (RsvgDefsDrawablePath *)self;
	rsvg_state_finalize (&z->super.state);
	g_free (z->d);
	g_free (z);
}

static void 
rsvg_defs_drawable_path_draw (RsvgDefsDrawable * self, RsvgHandle *ctx, 
							  int dominate)
{
	RsvgDefsDrawablePath *path = (RsvgDefsDrawablePath*)self;

	rsvg_state_reinherit_top(ctx, &self->state, dominate);

	rsvg_render_path (ctx, path->d);
	
}

static ArtSVP *
rsvg_defs_drawable_path_draw_as_svp (RsvgDefsDrawable * self, RsvgHandle *ctx, 
									 int dominate)
{
	RsvgDefsDrawablePath *path = (RsvgDefsDrawablePath*)self;

	rsvg_state_reinherit_top(ctx,  &self->state, dominate);

	return rsvg_render_path_as_svp (ctx, path->d);
	
}

static void 
rsvg_defs_drawable_group_free (RsvgDefVal *self)
{
	RsvgDefsDrawableGroup *z = (RsvgDefsDrawableGroup *)self;
	rsvg_state_finalize (&z->super.state);
	g_ptr_array_free(z->children, TRUE);
	g_free (z);
}

static void 
rsvg_defs_drawable_group_draw (RsvgDefsDrawable * self, RsvgHandle *ctx, 
							  int dominate)
{
	RsvgDefsDrawableGroup *group = (RsvgDefsDrawableGroup*)self;
	guint i;

	rsvg_state_reinherit_top(ctx, &self->state, dominate);

	rsvg_push_discrete_layer (ctx);

	for (i = 0; i < group->children->len; i++)
		{
			rsvg_state_push(ctx);

			rsvg_defs_drawable_draw (g_ptr_array_index(group->children, i), 
									 ctx, 0);
	
			rsvg_state_pop(ctx);
		}			

	rsvg_pop_discrete_layer (ctx);
}

static ArtSVP *
rsvg_defs_drawable_group_draw_as_svp (RsvgDefsDrawable * self, RsvgHandle *ctx, 
									  int dominate)
{
	RsvgDefsDrawableGroup *group = (RsvgDefsDrawableGroup*)self;
	guint i;
	ArtSVP *svp1, *svp2, *svp3;
	
	svp1 = NULL;

	rsvg_state_reinherit_top(ctx,  &self->state, dominate);

	for (i = 0; i < group->children->len; i++)
		{
			rsvg_state_push(ctx);

			svp2 = rsvg_defs_drawable_draw_as_svp (g_ptr_array_index(group->children, i), 
												   ctx, 0);
			if (svp1 != NULL)
				{
					svp3 = art_svp_union(svp2, svp1);
					art_free(svp1);
					svp1 = svp3;
				}
			
			rsvg_state_pop(ctx);
		}		
	return svp1;
}

static void 
rsvg_defs_drawable_use_free (RsvgDefVal *self)
{
	RsvgDefsDrawableUse *z = (RsvgDefsDrawableUse *)self;
	rsvg_state_finalize (&z->super.state);
	g_free (z);
}

static void 
rsvg_defs_drawable_use_draw (RsvgDefsDrawable * self, RsvgHandle *ctx, 
							  int dominate)
{
	RsvgState *state = rsvg_state_current (ctx);
	RsvgDefsDrawableUse *use = (RsvgDefsDrawableUse*)self;

	rsvg_state_reinherit_top(ctx,  &self->state, dominate);

	if (state->opacity != 0xff || rsvg_needs_discrete_layer(state))
		rsvg_push_discrete_layer (ctx);


	rsvg_state_push(ctx);
	
	rsvg_defs_drawable_draw (use->child, ctx, 1);

	rsvg_state_pop(ctx);	

	if (state->opacity != 0xff || rsvg_needs_discrete_layer(state))
		rsvg_pop_discrete_layer (ctx);
}	

static ArtSVP *
rsvg_defs_drawable_use_draw_as_svp (RsvgDefsDrawable * self, RsvgHandle *ctx, 
									int dominate)
{
	RsvgDefsDrawableUse *use = (RsvgDefsDrawableUse*)self;
	ArtSVP * svp;

	rsvg_state_reinherit_top(ctx,  &self->state, dominate);

	rsvg_state_push(ctx);
	
	svp = rsvg_defs_drawable_draw_as_svp (use->child, ctx, 1);

	rsvg_state_pop(ctx);
	
	return svp;
}			

static void 
rsvg_defs_drawable_group_pack (RsvgDefsDrawableGroup *self, RsvgDefsDrawable *child)
{
	RsvgDefsDrawableGroup *z;
	if (self == NULL)
		return;
	z = (RsvgDefsDrawableGroup *)self;
	g_ptr_array_add(z->children, child);
}

static RsvgDefsDrawable * 
rsvg_push_part_def_group (RsvgHandle *ctx, const char * id)
{
	RsvgDefsDrawableGroup *group;

	group = g_new (RsvgDefsDrawableGroup, 1);
	group->children = g_ptr_array_new();
	rsvg_state_clone (&group->super.state, rsvg_state_current (ctx));

	group->super.super.type = RSVG_DEF_PATH;
	group->super.super.free = rsvg_defs_drawable_group_free;
	group->super.draw = rsvg_defs_drawable_group_draw;
	group->super.draw_as_svp = rsvg_defs_drawable_group_draw_as_svp;

	rsvg_defs_set (ctx->defs, id, &group->super.super);

	group->super.parent = (RsvgDefsDrawable *)ctx->current_defs_group;

	ctx->current_defs_group = group;

	return &group->super;
}

RsvgDefsDrawable * 
rsvg_push_def_group (RsvgHandle *ctx, const char * id)
{
	RsvgDefsDrawable * group;

	group = rsvg_push_part_def_group (ctx, id);

	if (group->parent != NULL)
		rsvg_defs_drawable_group_pack((RsvgDefsDrawableGroup *)group->parent, 
									  group);

	return group;
}

void
rsvg_pop_def_group (RsvgHandle *ctx)
{
	RsvgDefsDrawableGroup * group;

	group = (RsvgDefsDrawableGroup *)ctx->current_defs_group;
	if (group == NULL)
		return;
	ctx->current_defs_group = group->super.parent;

}

void
rsvg_handle_path (RsvgHandle *ctx, const char * d, const char * id)
{
	RsvgDefsDrawablePath *path;

	if (!ctx->in_defs)
		rsvg_render_path (ctx, d);

	path = g_new (RsvgDefsDrawablePath, 1);
	path->d = g_strdup(d);
	rsvg_state_clone (&path->super.state, rsvg_state_current (ctx));
	path->super.super.type = RSVG_DEF_PATH;
	path->super.super.free = rsvg_defs_drawable_path_free;
	path->super.draw = rsvg_defs_drawable_path_draw;
	path->super.draw_as_svp = rsvg_defs_drawable_path_draw_as_svp;
	rsvg_defs_set (ctx->defs, id, &path->super.super);
	
	path->super.parent = (RsvgDefsDrawable *)ctx->current_defs_group;
	if (path->super.parent != NULL)
		rsvg_defs_drawable_group_pack((RsvgDefsDrawableGroup *)path->super.parent, 
									  &path->super);
}

void
rsvg_start_path (RsvgHandle *ctx, RsvgPropertyBag *atts)
{
	const char * klazz = NULL, * id = NULL, *value, *d = NULL;
	
	if (rsvg_property_bag_size (atts))
		{
			if ((value = rsvg_property_bag_lookup (atts, "d")))
				d = value;
			if ((value = rsvg_property_bag_lookup (atts, "class")))
				klazz = value;
			if ((value = rsvg_property_bag_lookup (atts, "id")))
				id = value;

			rsvg_parse_style_attrs (ctx, rsvg_state_current (ctx), "path", klazz, id, atts);
		}
	
	if (d == NULL)
		return;
	
	rsvg_handle_path (ctx, d, id);
}

static GString *
rsvg_make_poly_point_list(const char * points)
{
	guint idx = 0, size = strlen(points);
	GString * str = g_string_sized_new (size);
	
	while (idx < size) 
		{
			/* scan for first point */
			while (!g_ascii_isdigit (points[idx]) && (points[idx] != '.') 
				   && (points[idx] != '-') && (idx < size))
				idx++;
			
			/* now build up the point list (everything until next letter!) */
			if (idx < size && points[idx] == '-')
				g_string_append_c (str, points[idx++]); /* handle leading '-' */
			while ((g_ascii_isdigit (points[idx]) || (points[idx] == '.')) && (idx < size)) 
				g_string_append_c (str, points[idx++]);
			
			g_string_append_c (str, ' ');
		}
	
	return str;
}

static void
rsvg_start_any_poly(RsvgHandle *ctx, RsvgPropertyBag *atts, gboolean is_polyline)
{
	/* the only difference between polygon and polyline is
	   that a polyline closes the path */
	
	const char * verts = (const char *)NULL;
	GString * g = NULL;
	gchar ** pointlist = NULL;
	const char * klazz = NULL, * id = NULL, *value;

	if (rsvg_property_bag_size (atts))
		{
			/* support for svg < 1.0 which used verts */
			if ((value = rsvg_property_bag_lookup (atts, "verts")) || (value = rsvg_property_bag_lookup (atts, "points")))
				verts = value;
			if ((value = rsvg_property_bag_lookup (atts, "class")))
				klazz = value;
			if ((value = rsvg_property_bag_lookup (atts, "id")))
				id = value;

			rsvg_parse_style_attrs (ctx, rsvg_state_current (ctx), (is_polyline ? "polyline" : "polygon"), klazz, id, atts);
		}
	
	if (!verts)
		return;	
	
	/* todo: make the following more memory and CPU friendly */
	g = rsvg_make_poly_point_list (verts);
	pointlist = g_strsplit (g->str, " ", -1);
	g_string_free (g, TRUE);

	/* represent as a "moveto, lineto*, close" path */  
	if (pointlist)
		{
			int i;
			GString * d = g_string_sized_new (strlen(verts));
			g_string_append_printf (d, "M %s %s ", pointlist[0], pointlist[1] );
			
			for (i = 2; pointlist[i] != NULL && pointlist[i][0] != '\0'; i += 2)
				g_string_append_printf (d, "L %s %s ", pointlist[i], pointlist[i+1]);
			
			if (!is_polyline)
				g_string_append (d, "Z");
			
			g_strfreev(pointlist);
			rsvg_handle_path (ctx, d->str, id);
			g_string_free (d, TRUE);
		}
}

void
rsvg_start_polygon (RsvgHandle *ctx, RsvgPropertyBag *atts)
{
	rsvg_start_any_poly (ctx, atts, FALSE);
}

void
rsvg_start_polyline (RsvgHandle *ctx, RsvgPropertyBag *atts)
{
	rsvg_start_any_poly (ctx, atts, TRUE);
}

void
rsvg_start_line (RsvgHandle *ctx, RsvgPropertyBag *atts)
{
	double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
	GString * d = NULL;
	const char * klazz = NULL, * id = NULL, *value;
	char buf [G_ASCII_DTOSTR_BUF_SIZE];
	double font_size;

	font_size = rsvg_state_current_font_size (ctx);

	if (rsvg_property_bag_size (atts))
		{
			if ((value = rsvg_property_bag_lookup (atts, "x1")))
				x1 = rsvg_css_parse_normalized_length (value, ctx->dpi_x, (gdouble)ctx->width, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "y1")))
				y1 = rsvg_css_parse_normalized_length (value, ctx->dpi_y, (gdouble)ctx->height, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "x2")))
				x2 = rsvg_css_parse_normalized_length (value, ctx->dpi_x, (gdouble)ctx->width, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "y2")))
				y2 = rsvg_css_parse_normalized_length (value, ctx->dpi_y, (gdouble)ctx->height, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "class")))
				klazz = value;
			if ((value = rsvg_property_bag_lookup (atts, "id")))
				id = value;

			rsvg_parse_style_attrs (ctx, rsvg_state_current (ctx), "line", klazz, id, atts);
		}
	
	/* emulate a line using a path */
	/* ("M %f %f L %f %f", x1, y1, x2, y2) */
	d = g_string_new ("M ");   

	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), x1));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), y1));
	g_string_append (d, " L ");	
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), x2));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), y2));    

	rsvg_handle_path (ctx, d->str, id);
	g_string_free (d, TRUE);
}

void
rsvg_start_rect (RsvgHandle *ctx, RsvgPropertyBag *atts)
{
	double x = 0., y = 0., w = 0, h = 0, rx = 0., ry = 0.;
	GString * d = NULL;
	const char * klazz = NULL, * id = NULL, *value;
	char buf [G_ASCII_DTOSTR_BUF_SIZE];
	gboolean got_rx = FALSE, got_ry = FALSE;
	double font_size;

	font_size = rsvg_state_current_font_size (ctx);

	if (rsvg_property_bag_size (atts))
		{
			if ((value = rsvg_property_bag_lookup (atts, "x")))
				x = rsvg_css_parse_normalized_length (value, ctx->dpi_x, (gdouble)ctx->width, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "y")))
				y = rsvg_css_parse_normalized_length (value, ctx->dpi_y, (gdouble)ctx->height, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "width")))
				w = rsvg_css_parse_normalized_length (value, ctx->dpi_x, (gdouble)ctx->width, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "height")))
				h = rsvg_css_parse_normalized_length (value, ctx->dpi_y, (gdouble)ctx->height, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "rx"))) {
				rx = rsvg_css_parse_normalized_length (value, ctx->dpi_x, (gdouble)ctx->width, font_size);
				got_rx = TRUE;
			}
			if ((value = rsvg_property_bag_lookup (atts, "ry"))) {
				ry = rsvg_css_parse_normalized_length (value, ctx->dpi_y, (gdouble)ctx->height, font_size);
				got_ry = TRUE;
			}
			if ((value = rsvg_property_bag_lookup (atts, "class")))
				klazz = value;
			if ((value = rsvg_property_bag_lookup (atts, "id")))
				id = value;

			rsvg_parse_style_attrs (ctx, rsvg_state_current (ctx), "rect", klazz, id, atts);
		}

	if (got_rx && !got_ry)
		ry = rx;
	else if (got_ry && !got_rx)
		rx = ry;	

	if (w == 0. || h == 0. || rx < 0. || ry < 0.)
		return;

	if (rx > fabs(w / 2.))
		rx = fabs(w / 2.);
	if (ry > fabs(h / 2.))
		ry = fabs(h / 2.);   
	
	/* incrementing y by 1 properly draws borders. this is a HACK */
	y += .01;
	
	/* emulate a rect using a path */
	d = g_string_new ("M ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), x + rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), y));

	g_string_append (d, " H ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), x + w - rx));

	g_string_append (d, " A");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), ry));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), 0.));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), 0.));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), 1.));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), x+w));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), y+ry));

	g_string_append (d, " V ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), y+h-ry));

	g_string_append (d, " A");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), ry));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), 0.));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), 0.));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), 1.));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), x + w - rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), y + h));

	g_string_append (d, " H ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), x + rx));

	g_string_append (d, " A");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), ry));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), 0.));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), 0.));
	g_string_append_c (d, ' ');	
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), 1.));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), x));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), y + h - ry));

	g_string_append (d, " V ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), y+ry));

	g_string_append (d, " A");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), ry));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), 0.));
	g_string_append_c (d, ' ');	
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), 0.));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), 1.));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), x+rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), y));

	g_string_append (d, " Z");

	rsvg_handle_path (ctx, d->str, id);
	g_string_free (d, TRUE);
}

void
rsvg_start_circle (RsvgHandle *ctx, RsvgPropertyBag *atts)
{
	double cx = 0, cy = 0, r = 0;
	GString * d = NULL;
	const char * klazz = NULL, * id = NULL, *value;
	char buf [G_ASCII_DTOSTR_BUF_SIZE];
	double font_size;
	
	font_size = rsvg_state_current_font_size (ctx);

	if (rsvg_property_bag_size (atts))
		{
			if ((value = rsvg_property_bag_lookup (atts, "cx")))
				cx = rsvg_css_parse_normalized_length (value, ctx->dpi_x, (gdouble)ctx->width, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "cy")))
				cy = rsvg_css_parse_normalized_length (value, ctx->dpi_y, (gdouble)ctx->height, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "r")))
				r = rsvg_css_parse_normalized_length (value, rsvg_dpi_percentage (ctx), 
													  rsvg_viewport_percentage((gdouble)ctx->width, (gdouble)ctx->height), 
													  font_size);
			if ((value = rsvg_property_bag_lookup (atts, "class")))
				klazz = value;
			if ((value = rsvg_property_bag_lookup (atts, "id")))
				id = value;

			rsvg_parse_style_attrs (ctx, rsvg_state_current (ctx), "circle", klazz, id, atts);
		}
	
	if (r <= 0.)
		return;   
	
	/* approximate a circle using 4 bezier curves */

	d = g_string_new ("M ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx+r));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy));

	g_string_append (d, " C ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx+r));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy + r * RSVG_ARC_MAGIC));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx + r * RSVG_ARC_MAGIC));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy + r));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy + r));

	g_string_append (d, " C ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx - r * RSVG_ARC_MAGIC));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy + r));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx - r));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy + r * RSVG_ARC_MAGIC));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx - r));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy));

	g_string_append (d, " C ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx - r));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy - r * RSVG_ARC_MAGIC));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx - r * RSVG_ARC_MAGIC));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy - r));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy - r));

	g_string_append (d, " C ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx + r * RSVG_ARC_MAGIC));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy - r));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx + r));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy - r * RSVG_ARC_MAGIC));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx + r));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy));

	g_string_append (d, " Z");

	rsvg_handle_path (ctx, d->str, id);
	g_string_free (d, TRUE);
}

void
rsvg_start_ellipse (RsvgHandle *ctx, RsvgPropertyBag *atts)
{
	double cx = 0, cy = 0, rx = 0, ry = 0;
	GString * d = NULL;
	const char * klazz = NULL, * id = NULL, *value;
	char buf [G_ASCII_DTOSTR_BUF_SIZE];
	double font_size;
	
	font_size = rsvg_state_current_font_size (ctx);

	if (rsvg_property_bag_size (atts))
		{
			if ((value = rsvg_property_bag_lookup (atts, "cx")))
				cx = rsvg_css_parse_normalized_length (value, ctx->dpi_x, (gdouble)ctx->width, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "cy")))
				cy = rsvg_css_parse_normalized_length (value, ctx->dpi_y, (gdouble)ctx->height, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "rx")))
				rx = rsvg_css_parse_normalized_length (value, ctx->dpi_x, (gdouble)ctx->width, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "ry")))
				ry = rsvg_css_parse_normalized_length (value, ctx->dpi_y, (gdouble)ctx->height, font_size);
			if ((value = rsvg_property_bag_lookup (atts, "class")))
				klazz = value;
			if ((value = rsvg_property_bag_lookup (atts, "id")))
						id = value;

			rsvg_parse_style_attrs (ctx, rsvg_state_current (ctx), "ellipse", klazz, id, atts);
		}
	
	if (rx <= 0. || ry <= 0.)
		return;   
	
	/* approximate an ellipse using 4 bezier curves */

	d = g_string_new ("M ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx + rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy));

	g_string_append (d, " C ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx + rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy - RSVG_ARC_MAGIC * ry));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx + RSVG_ARC_MAGIC * rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy - ry));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy - ry));

	g_string_append (d, " C ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx - RSVG_ARC_MAGIC * rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy - ry));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx - rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy - RSVG_ARC_MAGIC * ry));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx - rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy));

	g_string_append (d, " C ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx - rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy + RSVG_ARC_MAGIC * ry));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx - RSVG_ARC_MAGIC * rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy + ry));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy + ry));

	g_string_append (d, " C ");
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx + RSVG_ARC_MAGIC * rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy + ry));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx + rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy + RSVG_ARC_MAGIC * ry));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cx + rx));
	g_string_append_c (d, ' ');
	g_string_append (d, g_ascii_dtostr (buf, sizeof (buf), cy));

	g_string_append (d, " Z");

	rsvg_handle_path (ctx, d->str, id);
	g_string_free (d, TRUE);
}

static const char s_UTF8_B64Alphabet[64] = {
	0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
	0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, /* A-Z */
	0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
	0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, /* a-z */
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, /* 0-9 */
	0x2b, /* + */
	0x2f  /* / */
};
static const char utf8_b64_pad = 0x3d;

static gboolean b64_decode_char (char c, int * b64)
{
	if ((c >= 0x41) && (c <= 0x5a))
		{
			*b64 = c - 0x41;
			return TRUE;
		}
	if ((c >= 0x61) && (c <= 0x7a))
		{
			*b64 = c - (0x61 - 26);
			return TRUE;
		}
	if ((c >= 0x30) && (c <= 0x39))
		{
			*b64 = c + (52 - 0x30);
			return TRUE;
		}
	if (c == 0x2b)
		{
			*b64 = 62;
			return TRUE;
		}
	if (c == 0x2f)
		{
			*b64 = 63;
			return TRUE;
		}
	return FALSE;
}

static gboolean utf8_base64_decode(char ** binptr, size_t * binlen, const char * b64ptr, size_t b64len)
{
	gboolean decoded = TRUE;
	gboolean padding = FALSE;
	
	int i = 0;
	glong ucs4_len, j;

	unsigned char byte1 = 0;
	unsigned char byte2;
	
	gunichar ucs4, * ucs4_str;
	
	if (b64len == 0) 
		return TRUE;
	
	if ((binptr == 0) || (b64ptr == 0)) 
		return FALSE;
	
	ucs4_str = g_utf8_to_ucs4_fast(b64ptr, b64len, &ucs4_len);
	
	for(j = 0; j < ucs4_len; j++)
		{
			ucs4 = ucs4_str[j];
			if ((ucs4 & 0x7f) == ucs4)
				{
					int b64;
					char c = (char)(ucs4);

					if (b64_decode_char (c, &b64))
						{
							if (padding || (*binlen == 0))
								{
									decoded = FALSE;
									break;
								}

							switch (i)
								{
								case 0:
									byte1 = (unsigned char)(b64) << 2;
									i++;
									break;
								case 1:
									byte2 = (unsigned char)(b64);
									byte1 |= byte2 >> 4;
									*(*binptr)++ = (char)(byte1);
									(*binlen)--;
									byte1 = (byte2 & 0x0f) << 4;
									i++;
									break;
								case 2:
									byte2 = (unsigned char)(b64);
									byte1 |= byte2 >> 2;
									*(*binptr)++ = (char)(byte1);
									(*binlen)--;
									byte1 = (byte2 & 0x03) << 6;
									i++;
									break;
								default:
									byte1 |= (unsigned char)(b64);
									*(*binptr)++ = (char)(byte1);
									(*binlen)--;
									i = 0;
									break;
								}
							
							if (!decoded) 
								break;

							continue;
						}
					else if (c == utf8_b64_pad)
						{
							switch (i)
								{
								case 0:
								case 1:
									decoded = FALSE;
									break;
								case 2:
									if (*binlen == 0) 
										decoded = FALSE;
									else
										{
											*(*binptr)++ = (char)(byte1);
											(*binlen)--;
											padding = TRUE;
										}
									i++;
									break;
								default:
									if (!padding)
										{
											if (*binlen == 0) 
												decoded = FALSE;
											else
												{
													*(*binptr)++ = (char)(byte1);
													(*binlen)--;
													padding = TRUE;
												}
										}
									i = 0;
									break;
								}
							if (!decoded) 
								break;

							continue;
						}
				}
			if (g_unichar_isspace (ucs4)) 
				continue;

			decoded = FALSE;
			break;
		}

	g_free(ucs4_str);
	return decoded;
}

static GdkPixbuf *
rsvg_pixbuf_new_from_data_at_size (const char *data,
								   GError    **error)
{
	GdkPixbufLoader *loader;
	GdkPixbuf       *pixbuf;
	
	char * buffer, *bufptr;
	size_t buffer_len, buffer_max_len, data_len;

	g_return_val_if_fail (data != NULL, NULL);

	while (*data) if (*data++ == ',') break;

	data_len = strlen(data);
	
	buffer_max_len = ((data_len >> 2) + 1) * 3;
	buffer_len = buffer_max_len;
	buffer = g_new(char, buffer_max_len);
	bufptr = buffer;

	if(!utf8_base64_decode(&bufptr, &buffer_len, data, data_len)) {
		g_free(buffer);
		return NULL;
	}

	buffer_len = buffer_max_len - buffer_len;

	loader = gdk_pixbuf_loader_new ();

	if (!gdk_pixbuf_loader_write (loader, buffer, buffer_len, error)) {
		gdk_pixbuf_loader_close (loader, NULL);
		g_object_unref (loader);
		g_free(buffer);
		return NULL;
	}
	
	g_free(buffer);
	if (!gdk_pixbuf_loader_close (loader, error)) {
		g_object_unref (loader);
		return NULL;
	}
	
	pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
	
	if (!pixbuf) {
		g_object_unref (loader);
		g_set_error (error,
					 GDK_PIXBUF_ERROR,
					 GDK_PIXBUF_ERROR_FAILED,
					 _("Failed to load image: reason not known, probably a corrupt image."));
		return NULL;
	}
	
	g_object_ref (pixbuf);
	
	g_object_unref (loader);
	
	return pixbuf;
}

static gchar *
rsvg_get_file_path (const gchar * filename, const gchar *basedir)
{
	gchar *absolute_filename;

	if (g_path_is_absolute(filename)) {
		absolute_filename = g_strdup (filename);
	} else {
		gchar *tmpcdir;

		if (basedir)
			tmpcdir = g_path_get_dirname (basedir);
		else
			tmpcdir = g_get_current_dir ();

		absolute_filename = g_build_filename (tmpcdir, filename, NULL);
		g_free(tmpcdir);
	}

	return absolute_filename;
}

static GdkPixbuf *
rsvg_pixbuf_new_from_file_at_size (const char *filename,
								   const char *base_uri,
								   GError    **error)
{
	GdkPixbufLoader *loader;
	GdkPixbuf       *pixbuf;
	gchar *path;

	guchar buffer [4096];
	int length;
	FILE *f;

	g_return_val_if_fail (filename != NULL, NULL);
	
	path = rsvg_get_file_path (filename, base_uri);
	f = fopen (path, "rb");
	g_free (path);
	
	if (!f) {
		g_set_error (error,
					 G_FILE_ERROR,
					 g_file_error_from_errno (errno),
					 _("Failed to open file '%s': %s"),
					 filename, g_strerror (errno));
		return NULL;
	}
	
	loader = gdk_pixbuf_loader_new ();
	
	while (!feof (f)) {
		length = fread (buffer, 1, sizeof (buffer), f);
		if (length > 0)
			if (!gdk_pixbuf_loader_write (loader, buffer, length, error)) {
				gdk_pixbuf_loader_close (loader, NULL);
				fclose (f);
				g_object_unref (loader);
				return NULL;
			}
	}
	
	fclose (f);
	
	if (!gdk_pixbuf_loader_close (loader, error)) {
		g_object_unref (loader);
		return NULL;
	}
	
	pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);	

	if (!pixbuf) {
		g_object_unref (loader);
		g_set_error (error,
					 GDK_PIXBUF_ERROR,
					 GDK_PIXBUF_ERROR_FAILED,
					 _("Failed to load image '%s': reason not known, probably a corrupt image file"),
					 filename);
		return NULL;
	}
	
	g_object_ref (pixbuf);
	
	g_object_unref (loader);

	return pixbuf;
}

#ifdef HAVE_GNOME_VFS

#include <libgnomevfs/gnome-vfs.h>

static GdkPixbuf *
rsvg_pixbuf_new_from_vfs_at_size (const char *filename,
								  const char *base_uri,
								  GError    **error)
{
	GdkPixbufLoader *loader;
	GdkPixbuf       *pixbuf;
	
	guchar buffer [4096];
	GnomeVFSFileSize length;
	GnomeVFSHandle *f = NULL;
	GnomeVFSResult res;
	
	g_return_val_if_fail (filename != NULL, NULL);
	
	if (!gnome_vfs_initialized())
		gnome_vfs_init();

	res = gnome_vfs_open (&f, filename, GNOME_VFS_OPEN_READ);

	if (res != GNOME_VFS_OK) {
		if (base_uri) {
			GnomeVFSURI * base = gnome_vfs_uri_new (base_uri);
			if (base) {
				GnomeVFSURI * uri = gnome_vfs_uri_resolve_relative (base, filename);
				if (uri) {
					res = gnome_vfs_open_uri (&f, uri, GNOME_VFS_OPEN_READ);
					gnome_vfs_uri_unref (uri);
				}

				gnome_vfs_uri_unref (base);
			}
		}
	}

	if (res != GNOME_VFS_OK) {
		g_set_error (error, rsvg_error_quark (), (gint) res,
					 gnome_vfs_result_to_string (res));
		return NULL;
	}
	
	loader = gdk_pixbuf_loader_new ();
	
	while (TRUE) {
		res = gnome_vfs_read (f, buffer, sizeof (buffer), &length);
		if (res == GNOME_VFS_OK && length > 0) {
			if (!gdk_pixbuf_loader_write (loader, buffer, length, error)) {
				gdk_pixbuf_loader_close (loader, NULL);
				gnome_vfs_close (f);
				g_object_unref (loader);
				return NULL;
			}
		} else {
			break;
		}
	}
	
	gnome_vfs_close (f);
	
	if (!gdk_pixbuf_loader_close (loader, error)) {
		g_object_unref (loader);
		return NULL;
	}
	
	pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
	
	if (!pixbuf) {
		g_object_unref (loader);
		g_set_error (error,
					 GDK_PIXBUF_ERROR,
					 GDK_PIXBUF_ERROR_FAILED,
					 _("Failed to load image '%s': reason not known, probably a corrupt image file"),
					 filename);
		return NULL;
	}
	
	g_object_ref (pixbuf);
	
	g_object_unref (loader);

	return pixbuf;
}

#endif

GdkPixbuf *
rsvg_pixbuf_new_from_href (const char *href,
						   const char *base_uri,
						   GError    **err)
{
	GdkPixbuf * img = NULL;

	if(!strncmp(href, "data:", 5))
		img = rsvg_pixbuf_new_from_data_at_size (href, err);
	
	if(!img)
		img = rsvg_pixbuf_new_from_file_at_size (href, base_uri, err);

#ifdef HAVE_GNOME_VFS
	if(!img)
		img = rsvg_pixbuf_new_from_vfs_at_size (href, base_uri, err);
#endif

	return img;
}

void
rsvg_affine_image(GdkPixbuf *img, GdkPixbuf *intermediate, 
				  double * affine, double w, double h)
{
	gdouble tmp_affine[6];
	gdouble inv_affine[6];
	gdouble raw_inv_affine[6];
	gint intstride;
	gint basestride;	
	gint basex, basey;
	gdouble fbasex, fbasey;
	gdouble rawx, rawy;
	guchar * intpix;
	guchar * basepix;
	gint i, j, k, basebpp, ii, jj;
	gboolean has_alpha;
	gdouble pixsum[4];
	gboolean xrunnoff, yrunnoff;
	gint iwidth, iheight;
	gint width, height;

	width = gdk_pixbuf_get_width (img);
	height = gdk_pixbuf_get_height (img);
	iwidth = gdk_pixbuf_get_width (intermediate);
	iheight = gdk_pixbuf_get_height (intermediate);

	has_alpha = gdk_pixbuf_get_has_alpha (img);

	basestride = gdk_pixbuf_get_rowstride (img);
	intstride = gdk_pixbuf_get_rowstride (intermediate);
	basepix = gdk_pixbuf_get_pixels (img);
	intpix = gdk_pixbuf_get_pixels (intermediate);
	basebpp = has_alpha ? 4 : 3;

	art_affine_invert(raw_inv_affine, affine);

	/*scale to w and h*/
	tmp_affine[0] = (double)w;
	tmp_affine[3] = (double)h;
	tmp_affine[1] = tmp_affine[2] = tmp_affine[4] = tmp_affine[5] = 0;
	art_affine_multiply(tmp_affine, tmp_affine, affine);

	art_affine_invert(inv_affine, tmp_affine);


	/*apply the transformation*/
	for (i = 0; i < iwidth; i++)
		for (j = 0; j < iheight; j++)		
			{
				fbasex = (inv_affine[0] * (double)i + inv_affine[2] * (double)j + 
						  inv_affine[4]) * (double)width;
				fbasey = (inv_affine[1] * (double)i + inv_affine[3] * (double)j + 
						  inv_affine[5]) * (double)height;
				basex = floor(fbasex);
				basey = floor(fbasey);
				rawx = raw_inv_affine[0] * i + raw_inv_affine[2] * j + 
					raw_inv_affine[4];
				rawy = raw_inv_affine[1] * i + raw_inv_affine[3] * j + 
					raw_inv_affine[5];
				if (rawx < 0 || rawy < 0 || rawx >= w || 
					rawy >= h || basex < 0 || basey < 0 
					|| basex >= width || basey >= height)
					{					
						for (k = 0; k < 4; k++)
							intpix[i * 4 + j * intstride + k] = 0;
					}
				else
					{
						if (basex < 0 || basex + 1 >= width)
							xrunnoff = TRUE;
						else
							xrunnoff = FALSE;
						if (basey < 0 || basey + 1 >= height)
							yrunnoff = TRUE;
						else
							yrunnoff = FALSE;
						for (k = 0; k < basebpp; k++)
							pixsum[k] = 0;
						for (ii = 0; ii < 2; ii++)
							for (jj = 0; jj < 2; jj++)
								{
									if (basex + ii < 0 || basey + jj< 0 
										|| basex + ii >= width || basey + jj >= height)
										;
									else
										{
											for (k = 0; k < basebpp; k++)
												{
													pixsum[k] += 
														(double)basepix[basebpp * (basex + ii) + (basey + jj) * basestride + k] 
														* (xrunnoff ? 1 : fabs(fbasex - (double)(basex + (1 - ii))))
														* (yrunnoff ? 1 : fabs(fbasey - (double)(basey + (1 - jj))));
												}
										}
								}
						for (k = 0; k < basebpp; k++)
							intpix[i * 4 + j * intstride + k] = pixsum[k];
						if (!has_alpha)
							intpix[i * 4 + j * intstride + 3] = 255;
					}	

			}
}

void rsvg_clip_image(GdkPixbuf *intermediate, ArtSVP *path);

void
rsvg_clip_image(GdkPixbuf *intermediate, ArtSVP *path)
{
	gint intstride;
	gint basestride;	
	guchar * intpix;
	guchar * basepix;
	gint i, j;
	gint width, height;
	GdkPixbuf * base;

	width = gdk_pixbuf_get_width (intermediate);
	height = gdk_pixbuf_get_height (intermediate);

	intstride = gdk_pixbuf_get_rowstride (intermediate);
	intpix = gdk_pixbuf_get_pixels (intermediate);

	base = gdk_pixbuf_new (GDK_COLORSPACE_RGB, 0, 8, 
						   width, height);
	basestride = gdk_pixbuf_get_rowstride (base);
	basepix = gdk_pixbuf_get_pixels (base);
	
	art_rgb_svp_aa(path, 0, 0, width, height, 0xFFFFFF, 0x000000, basepix, basestride, NULL);

	for (i = 0; i < width; i++)
		for (j = 0; j < height; j++)		
			{
				intpix[i * 4 + j * intstride + 3] = intpix[i * 4 + j * intstride + 3] * 
					basepix[i * 3 + j * basestride] / 255;
			}
}

void
rsvg_start_image (RsvgHandle *ctx, RsvgPropertyBag *atts)
{
	double x = 0., y = 0., w = -1., h = -1.;
	const char * href = NULL;
	const char * klazz = NULL, * id = NULL, *value;
	int aspect_ratio = RSVG_ASPECT_RATIO_NONE;
	ArtIRect temprect;
	GdkPixbuf *img;
	GError *err = NULL;
	int i, j;
	double tmp_affine[6];
	double tmp_tmp_affine[6];
	RsvgState *state;
	GdkPixbuf *intermediate;
	double basex, basey;

	/* skip over defs entries for now */
	if (ctx->in_defs) return;

	state = rsvg_state_current (ctx);
	
	if (rsvg_property_bag_size (atts))
		{
			if ((value = rsvg_property_bag_lookup (atts, "x")))
				x = rsvg_css_parse_normalized_length (value, ctx->dpi_x, (gdouble)ctx->width, state->font_size);
			if ((value = rsvg_property_bag_lookup (atts, "y")))
				y = rsvg_css_parse_normalized_length (value, ctx->dpi_y, (gdouble)ctx->height, state->font_size);
			if ((value = rsvg_property_bag_lookup (atts, "width")))
				w = rsvg_css_parse_normalized_length (value, ctx->dpi_x, (gdouble)ctx->width, state->font_size);
			if ((value = rsvg_property_bag_lookup (atts, "height")))
				h = rsvg_css_parse_normalized_length (value, ctx->dpi_y, (gdouble)ctx->height, state->font_size);
			/* path is used by some older adobe illustrator versions */
			if ((value = rsvg_property_bag_lookup (atts, "path")) || (value = rsvg_property_bag_lookup (atts, "xlink:href")))
				href = value;
			if ((value = rsvg_property_bag_lookup (atts, "class")))
				klazz = value;
			if ((value = rsvg_property_bag_lookup (atts, "id")))
				id = value;
			if ((value = rsvg_property_bag_lookup (atts, "preserveAspectRatio")))
				aspect_ratio = rsvg_css_parse_aspect_ratio (value);

			rsvg_parse_style_attrs (ctx, state, "image", klazz, id, atts);
		}
	
	if (!href || w <= 0. || h <= 0.)
		return;   	

	/* figure out if image is visible or not */
	if (!state->visible || !state->cond_true)
		return;
	/*hmm, passing the error thingie into the next thing makes it screw up when using vfs*/
	img = rsvg_pixbuf_new_from_href (href, rsvg_handle_get_base_uri (ctx), NULL); 

	if (!img)
		{
			if (err)
				{
					g_warning (_("Couldn't load image: %s\n"), err->message);
					g_error_free (err);
				}
			return;
		}

	if (aspect_ratio)
		{
			if ((double)gdk_pixbuf_get_height (img) * (double)w >
				(double)gdk_pixbuf_get_width (img) * (double)h) 
				{
					w = 0.5 + (double)gdk_pixbuf_get_width (img) * (double)h 
						/ (double)gdk_pixbuf_get_height (img);
				} 
			else 
				{
					h = 0.5 + (double)gdk_pixbuf_get_height (img) * (double)w 
						/ (double)gdk_pixbuf_get_width (img);
				}
		}

	for (i = 0; i < 6; i++)
		tmp_affine[i] = state->affine[i];

	/*translate to x and y*/
	tmp_tmp_affine[0] = tmp_tmp_affine[3] = 1;
	tmp_tmp_affine[1] = tmp_tmp_affine[2] = 0;
	tmp_tmp_affine[4] = x;
	tmp_tmp_affine[5] = y;

	art_affine_multiply(tmp_affine, tmp_tmp_affine, tmp_affine);


	intermediate = gdk_pixbuf_new (GDK_COLORSPACE_RGB, 1, 8, 
								   gdk_pixbuf_get_width (ctx->pixbuf),
								   gdk_pixbuf_get_height (ctx->pixbuf));


	if (!intermediate)
		{
			g_object_unref (G_OBJECT (img));
			return;
		}

	rsvg_affine_image(img, intermediate, tmp_affine, w, h);

	g_object_unref (G_OBJECT (img));

	rsvg_push_discrete_layer(ctx);

	if (state->clippath)
		{
			rsvg_clip_image(intermediate, state->clippath);
		}

	/*slap it down*/
	rsvg_alpha_blt (intermediate, 0, 0,
					gdk_pixbuf_get_width (intermediate),
					gdk_pixbuf_get_height (intermediate),
					ctx->pixbuf, 
					0, 0);
	
	temprect.x0 = gdk_pixbuf_get_width (intermediate);
	temprect.y0 = gdk_pixbuf_get_height (intermediate);
	temprect.x1 = 0;
	temprect.y1 = 0;

	for (i = 0; i < 2; i++)
		for (j = 0; j < 2; j++)
			{
				basex = tmp_affine[0] * w * i + tmp_affine[2] * h * j + tmp_affine[4];
				basey = tmp_affine[1] * w * i + tmp_affine[3] * h * j + tmp_affine[5];
				temprect.x0 = MIN(basex, temprect.x0);
				temprect.y0 = MIN(basey, temprect.y0);
				temprect.x1 = MAX(basex, temprect.x1);
				temprect.y1 = MAX(basey, temprect.y1);
			}


	art_irect_union(&ctx->bbox, &ctx->bbox, &temprect);
	rsvg_pop_discrete_layer(ctx);

	g_object_unref (G_OBJECT (intermediate));
}

void 
rsvg_start_use (RsvgHandle *ctx, RsvgPropertyBag *atts)
{
	RsvgState *state = rsvg_state_current (ctx);
	const char * klazz = NULL, *id = NULL, *xlink_href = NULL, *value;
	double x = 0, y = 0, width = 0, height = 0;	
	gboolean got_width = FALSE, got_height = FALSE;
	double affine[6];

	if (rsvg_property_bag_size(atts))
		{
			if ((value = rsvg_property_bag_lookup (atts, "x")))
				x = rsvg_css_parse_normalized_length (value, ctx->dpi_x, (gdouble)ctx->width, state->font_size);
			if ((value = rsvg_property_bag_lookup (atts, "y")))
				y = rsvg_css_parse_normalized_length (value, ctx->dpi_y, (gdouble)ctx->height, state->font_size);
			if ((value = rsvg_property_bag_lookup (atts, "width"))) {
				width = rsvg_css_parse_normalized_length (value, ctx->dpi_x, (gdouble)ctx->height, state->font_size);
				got_width = TRUE;
			}
			if ((value = rsvg_property_bag_lookup (atts, "height"))) {
				height = rsvg_css_parse_normalized_length (value, ctx->dpi_y, (gdouble)ctx->height, state->font_size);
				got_height = TRUE;
			}
			if ((value = rsvg_property_bag_lookup (atts, "class")))
				klazz = value;
			if ((value = rsvg_property_bag_lookup (atts, "id")))
				id = value;
			if ((value = rsvg_property_bag_lookup (atts, "xlink:href")))
				xlink_href = value;
		}
	
	rsvg_parse_style_attrs (ctx, state, "use", klazz, id, atts);

	/* < 0 is an error, 0 disables rendering. TODO: handle positive values correctly */
	if (got_width || got_height)
		if (width <= 0. || height <= 0.)
			return;
	
	if (xlink_href != NULL)
		{
			RsvgDefVal * parent = rsvg_defs_lookup (ctx->defs, xlink_href+1);
			if (parent != NULL)
				switch(parent->type)
					{
					case RSVG_DEF_PATH:
						{
							RsvgDefsDrawable *drawable = (RsvgDefsDrawable*)parent;
							RsvgDefsDrawableUse * use;
							use = g_new (RsvgDefsDrawableUse, 1);
							use->child = drawable;
							rsvg_state_clone (&use->super.state, state);
							use->super.super.type = RSVG_DEF_PATH;
							use->super.super.free = rsvg_defs_drawable_use_free;
							use->super.draw = rsvg_defs_drawable_use_draw;
							use->super.draw_as_svp = rsvg_defs_drawable_use_draw_as_svp;
							art_affine_translate(affine, x, y);
							art_affine_multiply(use->super.state.affine, affine, use->super.state.affine);
							art_affine_multiply(use->super.state.personal_affine, affine, use->super.state.personal_affine);			
							
							rsvg_defs_set (ctx->defs, id, &use->super.super);
							
							use->super.parent = (RsvgDefsDrawable *)ctx->current_defs_group;
							if (use->super.parent != NULL)
								rsvg_defs_drawable_group_pack((RsvgDefsDrawableGroup *)use->super.parent, 
															  &use->super);
							
							if (!ctx->in_defs)
								rsvg_defs_drawable_draw (&use->super, ctx, 0);
							break;
						}
					default:
						g_warning (_("Unhandled defs entry/type %s %d\n"), id, 
								   parent->type);
						return;
					}
		}
}

static void
rsvg_marker_free(RsvgDefVal* self)
{
	RsvgMarker *marker;
	marker = (RsvgMarker *)self;
	g_free(self);
}

void 
rsvg_start_marker (RsvgHandle *ctx, RsvgPropertyBag *atts)
{
	const char *id = NULL, *value;
	RsvgMarker *marker;
	double font_size;
	double x = 0., y = 0., w = 0., h = 0.;
	double vbx = 0., vby = 0., vbw = 1., vbh = 1.;
	gboolean obj_bbox = TRUE;
	gboolean got_x, got_y, got_bbox, got_vbox, got_width, got_height;
	got_x = got_y = got_bbox = got_vbox = got_width = got_height = FALSE;
	
	font_size = rsvg_state_current_font_size (ctx);
	marker = g_new (RsvgMarker, 1);
		
	marker->orient = 0;
	marker->orientAuto = FALSE;
	
	if (rsvg_property_bag_size (atts))
		{
			if ((value = rsvg_property_bag_lookup (atts, "id")))
				id = value;
			if ((value = rsvg_property_bag_lookup (atts, "viewBox")))
				{
					got_vbox = rsvg_css_parse_vbox (value, &vbx, &vby,
													&vbw, &vbh);
				}
			if ((value = rsvg_property_bag_lookup (atts, "refX"))) {
				x = rsvg_css_parse_normalized_length (value, ctx->dpi_x, 1, font_size);
				got_x = TRUE;
			}
			if ((value = rsvg_property_bag_lookup (atts, "refY"))) {
				y = rsvg_css_parse_normalized_length (value, ctx->dpi_y, 1, font_size);
				got_y = TRUE;
			}
			if ((value = rsvg_property_bag_lookup (atts, "markerWidth"))) {
				w = rsvg_css_parse_normalized_length (value, ctx->dpi_x, 1, font_size);
				got_width = TRUE;
			}
			if ((value = rsvg_property_bag_lookup (atts, "markerHeight"))) {
				h = rsvg_css_parse_normalized_length (value, ctx->dpi_y, 1, font_size);
				got_height = TRUE;
			}
			if ((value = rsvg_property_bag_lookup (atts, "orient"))) {
				if (!strcmp (value, "auto"))
					marker->orientAuto = TRUE;
				else
					marker->orient = rsvg_css_parse_angle(value);
			}
			if ((value = rsvg_property_bag_lookup (atts, "markerUnits"))) {
				if (!strcmp (value, "userSpaceOnUse"))
					obj_bbox = FALSE;
				else
					obj_bbox = TRUE;					
				got_bbox = TRUE;
			}	
		}
	
	if (got_x)
		marker->refX = x;
	else
		marker->refX = 0;

	if (got_y)
		marker->refY = y;
	else
		marker->refY = 0;

	if (got_width)
		marker->width = w;
	else
		marker->width = 1;

	if (got_height)
		marker->height = h;
	else
		marker->height = 1;

	if (got_bbox)
		marker->bbox = obj_bbox;
	else
		marker->bbox = TRUE;

	if (got_vbox)
		{
			marker->vbx = vbx;
			marker->vby = vby;
			marker->vbw = vbw;
			marker->vbh = vbh;
			marker->vbox = TRUE;
		}
	else
		marker->vbox = FALSE;
	
	/* set up the defval stuff */
	marker->super.type = RSVG_DEF_MARKER;

	marker->contents =	(RsvgDefsDrawable *)rsvg_push_part_def_group(ctx, NULL);

	marker->super.free = rsvg_marker_free;

	rsvg_defs_set (ctx->defs, id, &marker->super);
}

void 
rsvg_marker_render (RsvgMarker *self, gdouble x, gdouble y, gdouble orient, gdouble linewidth, RsvgHandle *ctx)
{
	gdouble affine[6];
	gdouble taffine[6];
	int i;
	gdouble rotation;

	if (self->bbox) {
		art_affine_scale(affine,linewidth * rsvg_state_current(ctx)->affine[0], 
						 linewidth * rsvg_state_current(ctx)->affine[3]);
	} else {
		for (i = 0; i < 6; i++)
			affine[i] = rsvg_state_current(ctx)->affine[i];
	}	

	if (self->vbox) {
		taffine[0] = self->width / self->vbw;
		taffine[1] = 0.;		
		taffine[2] = 0.;
		taffine[3] = self->height / self->vbh;
		taffine[4] = - self->vbx / self->vbw;
		taffine[5] = - self->vby / self->vbh;
		art_affine_multiply(affine, taffine, affine);		
	}

	art_affine_translate(taffine, -self->refX, -self->refY);

	art_affine_multiply(affine, taffine, affine);

	if (self->orientAuto)
		rotation = orient * 180 / 3.14159265358979323;
	else
		rotation = self->orient;

	art_affine_rotate(taffine, rotation);
	
	art_affine_multiply(affine, affine, taffine);

	art_affine_translate(taffine, x, y);
	
	art_affine_multiply(affine, affine, taffine);

	rsvg_state_push(ctx);
	
	for (i = 0; i < 6; i++)
		{
			rsvg_state_current(ctx)->affine[i] = affine[i];
		}


	rsvg_defs_drawable_draw (self->contents, ctx, 2);
	
	rsvg_state_pop(ctx);
}

RsvgDefVal *
rsvg_marker_parse (const RsvgDefs * defs, const char *str)
{
	if (!strncmp (str, "url(", 4))
		{
			const char *p = str + 4;
			int ix;
			char *name;
			RsvgDefVal *val;
			
			while (g_ascii_isspace (*p))
				p++;

			if (*p == '#')
				{
				  p++;
					for (ix = 0; p[ix]; ix++)
						if (p[ix] == ')')
							break;

					if (p[ix] == ')')
						{
							name = g_strndup (p, ix);
							val = rsvg_defs_lookup (defs, name);
							g_free (name);
							
							if (val && val->type == RSVG_DEF_MARKER)
								return (RsvgDefVal *) val;
						}
				}
		}
	return NULL;
}
