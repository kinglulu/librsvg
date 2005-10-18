/* vim: set sw=4: -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
   rsvg-shapes.c: Draw shapes with cairo

   Copyright (C) 2005 Dom Lachowicz <cinamod@hotmail.com>
   Copyright (C) 2005 Caleb Moore <c.moore@student.unsw.edu.au>
   Copyright (C) 2005 Red Hat, Inc.

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

   Authors: Dom Lachowicz <cinamod@hotmail.com>, 
            Caleb Moore <c.moore@student.unsw.edu.au>
            Carl Worth <cworth@cworth.org>
*/

#include "rsvg-cairo-draw.h"
#include "rsvg-cairo-render.h"
#include "rsvg-cairo-clip.h"
#include "rsvg-styles.h"
#include "rsvg-bpath-util.h"
#include "rsvg-path.h"
#include "rsvg-filter.h"
#include "rsvg-structure.h"
#include "rsvg-image.h"

#include <math.h>
#include <string.h>


static void
rsvg_pixmap_destroy (gchar *pixels, gpointer data)
{
  g_free (pixels);
}

static void
_pattern_add_rsvg_color_stops (cairo_pattern_t *pattern,
							   GPtrArray       *stops,
							   guint32          current_color_rgb,
							   guint8           opacity)
{
	gsize i;
	RsvgGradientStop *stop;
	RsvgNode *node;
	guint32 rgba;

	for (i=0; i < stops->len; i++) {
		node = (RsvgNode*) g_ptr_array_index (stops, i);
		if (node->type != RSVG_NODE_STOP)
			continue;
		stop = (RsvgGradientStop*) node;
		rgba = stop->rgba;
		cairo_pattern_add_color_stop_rgba (pattern, stop->offset,
										   ((rgba >> 24) & 0xff) / 255.0,
										   ((rgba >> 16) & 0xff) / 255.0,
										   ((rgba >>  8) & 0xff) / 255.0,
										   (((rgba >>  0) & 0xff) * opacity)/255.0/255.0);
	}
}

static void
_set_source_rsvg_linear_gradient (cairo_t            *cr,
								  RsvgLinearGradient *linear,
								  guint32             current_color_rgb,
								  guint8              opacity,
								  RsvgCairoBbox       bbox)
{
	cairo_pattern_t *pattern;
	cairo_matrix_t matrix;
	RsvgLinearGradient statlinear;
	statlinear = *linear;
	linear = &statlinear;
	rsvg_linear_gradient_fix_fallback(linear);

	if (linear->has_current_color)
		current_color_rgb = linear->current_color;

	pattern = cairo_pattern_create_linear (linear->x1, linear->y1,
										   linear->x2, linear->y2);

	cairo_matrix_init (&matrix,
					   linear->affine[0], linear->affine[1],
					   linear->affine[2], linear->affine[3],
					   linear->affine[4], linear->affine[5]);
	if (linear->obj_bbox){
		cairo_matrix_t bboxmatrix;
		cairo_matrix_init (&bboxmatrix,
						   bbox.w, 0, 0, bbox.h, bbox.x, bbox.y);
		cairo_matrix_multiply(&matrix, &matrix, &bboxmatrix);
	}
	cairo_matrix_invert (&matrix);
	cairo_pattern_set_matrix (pattern, &matrix);

	if (linear->spread == RSVG_GRADIENT_REFLECT)
		cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REFLECT);
	else if (linear->spread == RSVG_GRADIENT_REPEAT)
		cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);

	_pattern_add_rsvg_color_stops (pattern, linear->super.children,
								   current_color_rgb, opacity);

	cairo_set_source (cr, pattern);
	cairo_pattern_destroy (pattern);
}

static void
_set_source_rsvg_radial_gradient (cairo_t            *cr,
								  RsvgRadialGradient *radial,
								  guint32             current_color_rgb,
								  guint8              opacity,
								  RsvgCairoBbox       bbox)
{
	cairo_pattern_t *pattern;
	cairo_matrix_t matrix;
	RsvgRadialGradient statradial;
	statradial = *radial;
	radial = &statradial;
	rsvg_radial_gradient_fix_fallback(radial);

	if (radial->has_current_color)
		current_color_rgb = radial->current_color;

	pattern = cairo_pattern_create_radial (radial->fx, radial->fy, 0.0,
										   radial->cx, radial->cy, radial->r);

	cairo_matrix_init (&matrix,
					   radial->affine[0], radial->affine[1],
					   radial->affine[2], radial->affine[3],
					   radial->affine[4], radial->affine[5]);
	if (radial->obj_bbox){
		cairo_matrix_t bboxmatrix;
		cairo_matrix_init (&bboxmatrix,
						   bbox.w, 0, 0, bbox.h, bbox.x, bbox.y);
		cairo_matrix_multiply(&matrix, &matrix, &bboxmatrix);
	}

	cairo_matrix_invert (&matrix);
	cairo_pattern_set_matrix (pattern, &matrix);

	if (radial->spread == RSVG_GRADIENT_REFLECT)
		cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REFLECT);
	else if (radial->spread == RSVG_GRADIENT_REPEAT)
		cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);

	_pattern_add_rsvg_color_stops (pattern, radial->super.children,
								   current_color_rgb, opacity);

	cairo_set_source (cr, pattern);
	cairo_pattern_destroy (pattern);
}

static void
_set_source_rsvg_solid_colour (cairo_t         *cr,
							   RsvgSolidColour *colour,
							   guint8           opacity,
							   guint32          current_colour)
{
	guint32 rgb = colour->rgb;
	if (colour->currentcolour)
		rgb = current_colour;
	double r = ((rgb >> 16) & 0xff) / 255.0;
	double g = ((rgb >>  8) & 0xff) / 255.0;
	double b = ((rgb >>  0) & 0xff) / 255.0;

	if (opacity == 0xff)
		cairo_set_source_rgb (cr, r, g, b);
	else
		cairo_set_source_rgba (cr, r, g, b,
							   opacity / 255.0);
}

static void
_set_source_rsvg_pattern (RsvgDrawingCtx *ctx,
						  RsvgPattern    *rsvg_pattern,
						  guint8          opacity,
						  RsvgCairoBbox   bbox)
{
	RsvgCairoRender *render = (RsvgCairoRender *)ctx->render;
	RsvgPattern local_pattern = *rsvg_pattern;
	cairo_t *cr_render, *cr_pattern;
	cairo_pattern_t *pattern;
	cairo_surface_t *surface;
	cairo_matrix_t matrix;
	int i;
	double affine[6], caffine[6], bbwscale, bbhscale, scwscale, schscale;

	rsvg_pattern = &local_pattern;
	rsvg_pattern_fix_fallback(rsvg_pattern);
	cr_render = render->cr;

	/* Work out the size of the rectangle so it takes into account the object bounding box */

	
	if (rsvg_pattern->obj_bbox){
		bbwscale = bbox.w;
		bbhscale = bbox.h;
	} else {
		bbwscale = 1.0;
		bbhscale = 1.0;
	}

	_rsvg_affine_multiply(affine, rsvg_pattern->affine, 
						  rsvg_state_current(ctx)->affine);

	scwscale = sqrt(affine[0] * affine[0] + affine[2] * affine[2]);
	schscale = sqrt(affine[1] * affine[1] + affine[3] * affine[3]);

	scwscale = (double)((int)(rsvg_pattern->width * bbwscale *
							  scwscale)) / (rsvg_pattern->width * bbwscale);
	schscale = (double)((int)(rsvg_pattern->height * bbhscale *
							  schscale)) / (rsvg_pattern->height * bbhscale);

	surface = cairo_surface_create_similar(cairo_get_target (cr_render),
										   CAIRO_CONTENT_COLOR_ALPHA,
										   rsvg_pattern->width * bbwscale *
										   scwscale, 
										   rsvg_pattern->height * bbhscale *
										   schscale);
	cr_pattern = cairo_create(surface);

	
	affine[0] = 1;
	affine[1] = 0.;		
	affine[2] = 0.;
	affine[3] = 1;
	/* Create the pattern coordinate system */
	if (rsvg_pattern->obj_bbox) {
		/* subtract the pattern origin */
		affine[4] = bbox.x + rsvg_pattern->x * bbox.w;
		affine[5] = bbox.y + rsvg_pattern->y * bbox.h;
	} else {
		/* subtract the pattern origin */
		affine[4] = rsvg_pattern->x;
		affine[5] = rsvg_pattern->y;
	}
	/* Apply the pattern transform */
	_rsvg_affine_multiply(affine, affine, rsvg_pattern->affine);

	/* Create the pattern contents coordinate system */
	if (rsvg_pattern->vbox) {
		/* If there is a vbox, use that */
		double w, h, x, y;
		w = rsvg_pattern->width * bbwscale;
		h = rsvg_pattern->height * bbhscale;
		x = 0;
		y = 0;
		rsvg_preserve_aspect_ratio(rsvg_pattern->preserve_aspect_ratio,
								   rsvg_pattern->vbw, rsvg_pattern->vbh, 
								   &w, &h, &x, &y);

		x -= rsvg_pattern->vbx * w / rsvg_pattern->vbw;
		y -= rsvg_pattern->vby * h / rsvg_pattern->vbh;

		caffine[0] = w / rsvg_pattern->vbw;
		caffine[1] = 0.;		
		caffine[2] = 0.;
		caffine[3] = h / rsvg_pattern->vbh;
		caffine[4] = x;		
		caffine[5] = y;
	}
	else if (rsvg_pattern->obj_cbbox) {
		/* If coords are in terms of the bounding box, use them */
		caffine[0] = bbox.w;
		caffine[1] = 0.;		
		caffine[2] = 0.;
		caffine[3] = bbox.h;
		caffine[4] = 0;		
		caffine[5] = 0;
	} else {
		/* Otherwise default to an identity matrix */
		caffine[0] = 1;
		caffine[1] = 0.;		
		caffine[2] = 0.;
		caffine[3] = 1;
		caffine[4] = 0;		
		caffine[5] = 0;
	}
	
	if (scwscale != 1.0 || schscale != 1.0)
		{
			double scalematrix[6];
			_rsvg_affine_scale(scalematrix, scwscale, schscale);
			_rsvg_affine_multiply(caffine, caffine, scalematrix);
			_rsvg_affine_scale(scalematrix, 1. / scwscale, 1. / schscale);
			_rsvg_affine_multiply(affine, scalematrix, affine);
		}

	/* Draw to another surface */
	render->cr = cr_pattern;
	
	/* Set up transformations to be determined by the contents units */
	rsvg_state_push(ctx);
	for (i = 0; i < 6; i++)
		rsvg_state_current(ctx)->personal_affine[i] =
			rsvg_state_current(ctx)->affine[i] = caffine[i];

	/* Draw everything */
	_rsvg_node_draw_children ((RsvgNode *)rsvg_pattern, ctx, 2);

	/* Return to the original coordinate system */
	rsvg_state_pop(ctx);

	/* Set the render to draw where it used to */
	render->cr = cr_render;

	pattern = cairo_pattern_create_for_surface (surface);
	cairo_pattern_set_extend (pattern, CAIRO_EXTEND_REPEAT);

	cairo_matrix_init (&matrix,
					   affine[0], affine[1],
					   affine[2], affine[3],
					   affine[4], affine[5]);


	cairo_matrix_invert (&matrix);
	cairo_pattern_set_matrix (pattern, &matrix);
	cairo_pattern_set_filter(pattern, CAIRO_FILTER_BEST);

	cairo_set_source (cr_render, pattern);

	cairo_pattern_destroy (pattern);
	cairo_destroy (cr_pattern);
	cairo_surface_destroy (surface);
}

static void
_set_source_rsvg_paint_server (RsvgDrawingCtx  *ctx,
							   guint32          current_color_rgb,
							   RsvgPaintServer *ps,
							   guint8           opacity,
							   RsvgCairoBbox    bbox,
							   guint32          current_colour)
{
	RsvgCairoRender *render = (RsvgCairoRender *)ctx->render;
	cairo_t *cr = render->cr;

	switch (ps->type) {
	case RSVG_PAINT_SERVER_LIN_GRAD:
		_set_source_rsvg_linear_gradient (cr, ps->core.lingrad,
										  current_color_rgb, opacity,
										  bbox);
		break;
	case RSVG_PAINT_SERVER_RAD_GRAD:
		_set_source_rsvg_radial_gradient (cr, ps->core.radgrad,
										  current_color_rgb, opacity,
										  bbox);
		break;
	case RSVG_PAINT_SERVER_SOLID:
		_set_source_rsvg_solid_colour (cr, ps->core.colour, opacity, 
									   current_colour);
		break;
	case RSVG_PAINT_SERVER_PATTERN:
		_set_source_rsvg_pattern (ctx, ps->core.pattern, opacity,
								  bbox);
		break;
	}
}

static void
_set_rsvg_affine (cairo_t *cr, const double affine[6])
{
	cairo_matrix_t matrix;

	cairo_matrix_init (&matrix,
					   affine[0], affine[1],
					   affine[2], affine[3],
					   affine[4], affine[5]);
	cairo_set_matrix (cr, &matrix);
}

void
rsvg_cairo_render_path (RsvgDrawingCtx *ctx, const RsvgBpathDef *bpath_def)
{
	RsvgCairoRender *render = (RsvgCairoRender *)ctx->render;
	RsvgState *state = rsvg_state_current (ctx);
	cairo_t *cr;
	RsvgBpath *bpath;
	int i;
	gdouble xmin = 0, ymin = 0, xmax = 0, ymax = 0;
	int virgin = 1, need_tmpbuf = 0;
	RsvgCairoBbox bbox;

	if (state->fill == NULL && state->stroke == NULL)
		return;

	need_tmpbuf = ((state->fill != NULL) && (state->stroke != NULL) &&
				   state->opacity != 0xff) 
		|| state->clip_path_ref || state->mask || state->filter;

	if (need_tmpbuf)
		rsvg_cairo_push_discrete_layer (ctx);

	cr = render->cr;

	cairo_save (cr);

	_set_rsvg_affine (cr, state->affine);

	cairo_set_line_width (cr, state->stroke_width);
	cairo_set_miter_limit (cr, state->miter_limit);
	cairo_set_line_cap (cr, (cairo_line_cap_t)state->cap);
	cairo_set_line_join (cr, (cairo_line_join_t)state->join);
	cairo_set_dash (cr, state->dash.dash, state->dash.n_dash, state->dash.offset);

	for (i=0; i < bpath_def->n_bpath; i++) {
		bpath = &bpath_def->bpath[i];

		if (bpath->code == RSVG_MOVETO || 
			bpath->code == RSVG_MOVETO_OPEN || 
			bpath->code == RSVG_CURVETO ||
			bpath->code == RSVG_LINETO){
			if (bpath->x3 < xmin || virgin) xmin = bpath->x3;
			if (bpath->x3 > xmax || virgin) xmax = bpath->x3;
			if (bpath->y3 < ymin || virgin) ymin = bpath->y3;
			if (bpath->y3 > ymax || virgin) ymax = bpath->y3;
			virgin = 0;
		}

		switch (bpath->code) {
		case RSVG_MOVETO:
			cairo_close_path (cr);
			/* fall-through */
		case RSVG_MOVETO_OPEN:
			cairo_move_to (cr, bpath->x3, bpath->y3);
			break;
		case RSVG_CURVETO:
			cairo_curve_to (cr,
							bpath->x1, bpath->y1,
							bpath->x2, bpath->y2,
							bpath->x3, bpath->y3);
			break;
		case RSVG_LINETO:
			cairo_line_to (cr, bpath->x3, bpath->y3);
			break;
		case RSVG_END:
			break;
		}
	}

	rsvg_cairo_bbox_init(&bbox, state->affine);
	bbox.x = xmin;
	bbox.y = ymin;
	bbox.w = xmax - xmin;
	bbox.h = ymax - ymin;
	bbox.virgin = 0;

	rsvg_cairo_bbox_insert(&render->bbox, &bbox);

	if (state->fill != NULL) {
		int opacity;
		if (state->fill_rule == FILL_RULE_EVENODD)
			cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);
		else /* state->fill_rule == FILL_RULE_NONZERO */
			cairo_set_fill_rule (cr, CAIRO_FILL_RULE_WINDING);

		if (!need_tmpbuf)
			opacity = (state->fill_opacity * state->opacity) / 255;
		else
			opacity = state->fill_opacity;

		_set_source_rsvg_paint_server (ctx,
									   state->current_color,
									   state->fill,
									   opacity,
									   bbox,
									   rsvg_state_current(ctx)->current_color);

		if (state->stroke != NULL)
			cairo_fill_preserve (cr);
		else
			cairo_fill (cr);
	}

	if (state->stroke != NULL) {
		int opacity;
		if (!need_tmpbuf)
			opacity = (state->stroke_opacity * state->opacity) / 255;
		else
			opacity = state->stroke_opacity;

		_set_source_rsvg_paint_server (ctx,
									   state->current_color,
									   state->stroke,
									   opacity,
									   bbox,
									   rsvg_state_current(ctx)->current_color);

		cairo_stroke (cr);
	}
	cairo_restore (cr);

	if (need_tmpbuf)
		rsvg_cairo_pop_discrete_layer (ctx);
}

void rsvg_cairo_render_image (RsvgDrawingCtx *ctx, const GdkPixbuf * pixbuf, 
							  double pixbuf_x, double pixbuf_y, double w, double h)
{
	RsvgCairoRender *render = (RsvgCairoRender *)ctx->render;
	RsvgState *state = rsvg_state_current(ctx);
	
	gint width = gdk_pixbuf_get_width (pixbuf);
	gint height = gdk_pixbuf_get_height (pixbuf);
	guchar *gdk_pixels = gdk_pixbuf_get_pixels (pixbuf);
	int gdk_rowstride = gdk_pixbuf_get_rowstride (pixbuf);
	int n_channels = gdk_pixbuf_get_n_channels (pixbuf);
	guchar *cairo_pixels;
	cairo_format_t format;
	cairo_surface_t *surface;
	static const cairo_user_data_key_t key;
	int j;
	RsvgCairoBbox bbox;

	if (pixbuf == NULL)
		return;

    cairo_save (render->cr);
	_set_rsvg_affine (render->cr, state->affine);
    cairo_scale (render->cr, w / width, h / height);
	pixbuf_x *= width / w;
	pixbuf_y *= height / h;

	if (n_channels == 3)
		format = CAIRO_FORMAT_RGB24;
	else
		format = CAIRO_FORMAT_ARGB32;
	
	cairo_pixels = g_malloc (4 * width * height);
	surface = cairo_image_surface_create_for_data ((unsigned char *)cairo_pixels,
												   format,
												   width, height, 4 * width);
	cairo_surface_set_user_data (surface, &key,
								 cairo_pixels, (cairo_destroy_func_t)g_free);
	
	for (j = height; j; j--)
		{
			guchar *p = gdk_pixels;
			guchar *q = cairo_pixels;
			
			if (n_channels == 3)
				{
					guchar *end = p + 3 * width;
					
					while (p < end)
						{
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
							q[0] = p[2];
							q[1] = p[1];
							q[2] = p[0];
#else	  
							q[1] = p[0];
							q[2] = p[1];
							q[3] = p[2];
#endif
							p += 3;
							q += 4;
						}
				}
			else
				{
					guchar *end = p + 4 * width;
					guint t1,t2,t3;
					
#define MULT(d,c,a,t) G_STMT_START { t = c * a; d = ((t >> 8) + t) >> 8; } G_STMT_END
					
					while (p < end)
						{
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
							MULT(q[0], p[2], p[3], t1);
							MULT(q[1], p[1], p[3], t2);
							MULT(q[2], p[0], p[3], t3);
							q[3] = p[3];
#else	  
							q[0] = p[3];
							MULT(q[1], p[0], p[3], t1);
							MULT(q[2], p[1], p[3], t2);
							MULT(q[3], p[2], p[3], t3);
#endif
							
							p += 4;
							q += 4;
						}
					
#undef MULT
				}
			
			gdk_pixels += gdk_rowstride;
			cairo_pixels += 4 * width;
		}

	rsvg_cairo_push_discrete_layer (ctx);

	cairo_set_source_surface (render->cr, surface, pixbuf_x, pixbuf_y);
	cairo_paint (render->cr);
	cairo_surface_destroy (surface);

	rsvg_cairo_pop_discrete_layer (ctx);

    cairo_restore (render->cr);

	rsvg_cairo_bbox_init(&bbox, state->affine);
	bbox.x = pixbuf_x;
	bbox.y = pixbuf_y;
	bbox.w = w;
	bbox.h = h;
	bbox.virgin = 0;

	rsvg_cairo_bbox_insert(&render->bbox, &bbox);
}

static cairo_surface_t *
rsvg_cairo_generate_mask(RsvgMask * self, RsvgDrawingCtx *ctx, 
						 RsvgCairoBbox * bbox)
{
	cairo_surface_t *surface;
	cairo_t *mask_cr, *save_cr;
	RsvgCairoRender *render = (RsvgCairoRender *)ctx->render;
	RsvgState *state = rsvg_state_current(ctx);
	guint8 * pixels;
	guint32 width = render->width, height = render->height; 
	guint32 rowstride = width * 4, row, i;
	double affinesave[6];

	pixels = g_new(guint8, height * rowstride);
	surface = cairo_image_surface_create_for_data (pixels,
												   CAIRO_FORMAT_ARGB32,
												   width, height,
												   rowstride);

	mask_cr = cairo_create (surface);
	save_cr = render->cr;
	render->cr = mask_cr;

	if (self->maskunits == objectBoundingBox)
		rsvg_cairo_add_clipping_rect (ctx, 
									  self->x * bbox->w + bbox->x, 
									  self->y * bbox->h + bbox->y,
									  self->width * bbox->w, 
									  self->height * bbox->h);
	else 
		rsvg_cairo_add_clipping_rect (ctx, self->x, self->y,
									  self->width, self->height);

	/* Horribly dirty hack to have the bbox premultiplied to everything */
	if (self->contentunits == objectBoundingBox)
		{
			double bbtransform[6];
			bbtransform[0] = bbox->w;
			bbtransform[1] = 0.;
			bbtransform[2] = 0.;
			bbtransform[3] = bbox->h;
			bbtransform[4] = bbox->x;
			bbtransform[5] = bbox->y;
			for (i = 0; i < 6; i++)
				affinesave[i] = self->super.state->affine[i];
			_rsvg_affine_multiply(self->super.state->affine,
								  bbtransform, 
								  self->super.state->affine);
		}

	rsvg_state_push(ctx);
	_rsvg_node_draw_children (&self->super, ctx, 0);
	rsvg_state_pop(ctx);

	if (self->contentunits == objectBoundingBox)
		for (i = 0; i < 6; i++)
			self->super.state->affine[i] = affinesave[i];


	render->cr = save_cr;
	
	for(row = 0; row < height; row++) {
		guint8 *row_data = (pixels + (row * rowstride));
		for(i = 0; i < width; i++) {
			guint32 *pixel = (guint32 *)row_data + i;
			*pixel = ((((*pixel & 0x00ff0000) >> 16) * 13817 +
					   ((*pixel & 0x0000ff00) >>  8) * 46518 +
					   ((*pixel & 0x000000ff)      ) * 4688) * 
					  state->opacity);
		}
	}

	cairo_destroy (mask_cr);
	return surface;
}

static void
rsvg_cairo_push_early_clips(RsvgDrawingCtx *ctx)
{
	cairo_save(((RsvgCairoRender *)ctx->render)->cr);
	if (rsvg_state_current(ctx)->clip_path_ref)
		if (((RsvgClipPath *)rsvg_state_current(ctx)->clip_path_ref)->units ==
			userSpaceOnUse)
			rsvg_cairo_clip(ctx, rsvg_state_current(ctx)->clip_path_ref, NULL);

}

static void
rsvg_cairo_push_render_stack (RsvgDrawingCtx *ctx)
{
	/* XXX: Untested, probably needs help wrt filters */

	RsvgCairoRender *render = (RsvgCairoRender *)ctx->render;
	cairo_surface_t *surface;
	cairo_t *child_cr;
	RsvgCairoBbox *bbox;
	RsvgState *state = rsvg_state_current(ctx);	
	gboolean lateclip = FALSE;

	if (rsvg_state_current(ctx)->clip_path_ref)
		if (((RsvgClipPath *)rsvg_state_current(ctx)->clip_path_ref)->units ==
			objectBoundingBox)
			lateclip = TRUE;

	if (state->opacity == 0xFF && !state->filter && !state->mask && !lateclip){
		return;
	}
	if (!state->filter)
		surface = cairo_surface_create_similar (cairo_get_target (render->cr),
												CAIRO_CONTENT_COLOR_ALPHA,
												render->width, render->height);
	else
		{
			guchar * pixels; 
			int rowstride = render->width * 4;
			pixels = g_new0(guint8, render->width * render->height * 4);

			surface = cairo_image_surface_create_for_data (pixels,
														   CAIRO_FORMAT_ARGB32,
														   render->width, 
														   render->height,
														   rowstride);
			render->pixbuf_stack = 
				g_list_prepend(render->pixbuf_stack, 
							   gdk_pixbuf_new_from_data (pixels,
														 GDK_COLORSPACE_RGB,
														 TRUE,
														 8,
														 render->width,
														 render->height,
														 rowstride,
														 (GdkPixbufDestroyNotify)rsvg_pixmap_destroy,
														 NULL));
		}
	child_cr = cairo_create (surface);
	cairo_surface_destroy (surface);
	
	render->cr_stack = g_list_prepend(render->cr_stack, render->cr);
	render->cr = child_cr;

	bbox = g_new(RsvgCairoBbox, 1);
	*bbox = render->bbox;
	render->bb_stack = g_list_prepend(render->bb_stack, bbox);
	rsvg_cairo_bbox_init(&render->bbox,state->affine);
}

void
rsvg_cairo_push_discrete_layer (RsvgDrawingCtx *ctx)
{
	rsvg_cairo_push_render_stack(ctx);
	rsvg_cairo_push_early_clips(ctx);
}

static void
rsvg_cairo_pop_render_stack (RsvgDrawingCtx *ctx)
{
	RsvgCairoRender *render = (RsvgCairoRender *)ctx->render;
	cairo_t *child_cr = render->cr;
	RsvgState *state;
	state = rsvg_state_current(ctx);
	gboolean lateclip = FALSE;
	GdkPixbuf * output = NULL;
	cairo_surface_t *surface = NULL;

	if (rsvg_state_current(ctx)->clip_path_ref)
		if (((RsvgClipPath *)rsvg_state_current(ctx)->clip_path_ref)->units ==
			objectBoundingBox)
			lateclip = TRUE;

	if (state->opacity == 0xFF && !state->filter && !state->mask && !lateclip)
		return;

	render->cr = (cairo_t *)render->cr_stack->data;
	render->cr_stack = g_list_remove_link (render->cr_stack, render->cr_stack);

	if (state->filter)
		{
			GdkPixbuf * pixbuf = render->pixbuf_stack->data;
			RsvgIRect bounds;
			RsvgCairoBbox bbox;
			double affine[6];
			_rsvg_affine_identity(affine);
			rsvg_cairo_bbox_init(&bbox, affine);
			rsvg_cairo_bbox_insert(&bbox, &render->bbox);
			bounds.x0 = bbox.x;
			bounds.y0 = bbox.y;
			bounds.x1 = bbox.w + bbox.x;
			bounds.y1 = bbox.h + bbox.y;	
			render->pixbuf_stack = g_list_remove_link (render->pixbuf_stack,
													   render->pixbuf_stack);


			rsvg_cairo_to_pixbuf(gdk_pixbuf_get_pixels(pixbuf),
								 gdk_pixbuf_get_rowstride(pixbuf),
								 gdk_pixbuf_get_height(pixbuf));
			output = rsvg_filter_render (state->filter, pixbuf, pixbuf, 
										 ctx, &bounds);
			gdk_pixbuf_unref(pixbuf);
			rsvg_pixbuf_to_cairo(gdk_pixbuf_get_pixels(output),
								 gdk_pixbuf_get_rowstride(output),
								 gdk_pixbuf_get_height(output));

			surface = cairo_image_surface_create_for_data (gdk_pixbuf_get_pixels(output),
														   CAIRO_FORMAT_ARGB32,
														   gdk_pixbuf_get_width(output), 
														   gdk_pixbuf_get_height(output),
														   gdk_pixbuf_get_rowstride(output));
			cairo_set_source_surface (render->cr,
									  surface,
									  0, 0);
		}
	else
		cairo_set_source_surface (render->cr,
								  cairo_get_target (child_cr),
								  0, 0);
	if (lateclip)
		{
			cairo_save(render->cr);
			rsvg_cairo_clip(ctx, rsvg_state_current(ctx)->clip_path_ref, 
							&render->bbox);
		}
	if (state->mask)
		{
			cairo_surface_t * mask = 
				rsvg_cairo_generate_mask(state->mask, ctx, &render->bbox);
			cairo_mask_surface (render->cr, mask, 0,0);
			cairo_surface_destroy(mask);
		}
	else if (state->opacity != 0xFF)
		cairo_paint_with_alpha (render->cr, (double)state->opacity / 255.0);
	else
		cairo_paint (render->cr);	
	cairo_destroy (child_cr);


	rsvg_cairo_bbox_insert((RsvgCairoBbox *)render->bb_stack->data, 
						   &render->bbox);
	
	render->bbox = *((RsvgCairoBbox *)render->bb_stack->data);

	g_free(render->bb_stack->data);
	render->bb_stack = g_list_remove_link (render->bb_stack, render->bb_stack);
	if (lateclip)
		cairo_restore(render->cr);

	if (state->filter)
		{
			gdk_pixbuf_unref(output);
			cairo_surface_destroy(surface);
		}
}

void
rsvg_cairo_pop_discrete_layer (RsvgDrawingCtx *ctx)
{
	cairo_restore(((RsvgCairoRender *)ctx->render)->cr);
	rsvg_cairo_pop_render_stack(ctx);
}

void 
rsvg_cairo_add_clipping_rect (RsvgDrawingCtx *ctx,
							  double x, double y,
							  double w, double h)
{
	RsvgCairoRender *render = (RsvgCairoRender *)ctx->render;
	cairo_t *cr = render->cr;
	cairo_matrix_t save;

	cairo_get_matrix (cr, &save);
	_set_rsvg_affine (cr, rsvg_state_current(ctx)->affine);

	cairo_rectangle (cr, x, y, w, h);
	cairo_clip (cr);

	cairo_set_matrix (cr, &save);
}

GdkPixbuf * 
rsvg_cairo_get_image_of_node (RsvgDrawingCtx *ctx,
							  RsvgNode       *drawable,
							  double          width,
							  double          height)
{
	/* XXX: untested, should work */

	GdkPixbuf *img = NULL;
	cairo_surface_t * surface;
	cairo_t * cr;
	guint8 *pixels;
	int rowstride;

	RsvgCairoRender *save_render = (RsvgCairoRender *)ctx->render;
	RsvgCairoRender *render;

	rowstride = width * 4;
	pixels = g_new(guint8, width * height * 4);
	surface = cairo_image_surface_create_for_data (pixels,
												   CAIRO_FORMAT_ARGB32,
												   width, height,
												   rowstride);
	cr = cairo_create (surface);
	cairo_surface_destroy (surface);

	rsvg_cairo_to_pixbuf(pixels, rowstride, height);

	render = rsvg_cairo_render_new(cr, width, height);
	ctx->render = (RsvgRender *)render;

	rsvg_state_push(ctx);	
	rsvg_node_draw (drawable, ctx, 0);	
	rsvg_state_pop(ctx);

	img = gdk_pixbuf_new_from_data (pixels,
									GDK_COLORSPACE_RGB,
									TRUE,
									8,
									width,
									height,
									rowstride,
									(GdkPixbufDestroyNotify)rsvg_pixmap_destroy,
									NULL);

	cairo_destroy (cr);
	ctx->render = (RsvgRender *)save_render;

	return img;
}

void rsvg_cairo_to_pixbuf(guint8 *pixels, int rowstride, int height)
{
	int row;
	/* un-premultiply data */
	for(row = 0; row < height; row++) {
		guint8 *row_data = (pixels + (row * rowstride));
		int i;

		for(i = 0; i < rowstride; i += 4) {
			guint8 *b = &row_data[i];
			guint32 pixel;
			guint8 alpha;

			memcpy(&pixel, b, sizeof(guint32));
			alpha = (pixel & 0xff000000) >> 24;
			if(alpha == 0) {
				b[0] = b[1] = b[2] = b[3] = 0;
			} else {
				b[0] = (((pixel & 0xff0000) >> 16) * 255 + alpha / 2) / alpha;
				b[1] = (((pixel & 0x00ff00) >>  8) * 255 + alpha / 2) / alpha;
				b[2] = (((pixel & 0x0000ff) >>  0) * 255 + alpha / 2) / alpha;
				b[3] = alpha;
			}
		}
	}
}

void rsvg_pixbuf_to_cairo(guint8 *pixels, int rowstride, int height)
{
	int row;
	/* un-premultiply data */
	for(row = 0; row < height; row++) {
		guint8 *row_data = (pixels + (row * rowstride));
		int i;

		for(i = 0; i < rowstride; i += 4) {
			guint32 *b = (guint32 *)&row_data[i];
			guint8 pixel[4];
			int alpha;

			memcpy(&pixel, b, sizeof(guint32));
			alpha = pixel[3];
			if(alpha == 0)
				*b = 0;
			else
				*b = alpha << 24 | 
					(int)pixel[0] * alpha / 255 << 16 |
					(int)pixel[1] * alpha / 255 << 8 |
					(int)pixel[2] * alpha / 255;
		}
	}
}
