/* vim: set sw=4: -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
   rsvg-paint-server.h : RSVG colors

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

#ifndef RSVG_PAINT_SERVER_H
#define RSVG_PAINT_SERVER_H

#include <glib/gtypes.h>
#include "rsvg-defs.h"

G_BEGIN_DECLS

typedef struct _RsvgGradientStop RsvgGradientStop;
typedef struct _RsvgGradientStops RsvgGradientStops;
typedef struct _RsvgLinearGradient RsvgLinearGradient;
typedef struct _RsvgRadialGradient RsvgRadialGradient;
typedef struct _RsvgPattern RsvgPattern;
typedef struct _RsvgSolidColour RsvgSolidColour;

typedef struct _RsvgPaintServer RsvgPaintServer;

typedef struct _RsvgPSCtx RsvgPSCtx;

typedef enum {
  RSVG_GRADIENT_PAD,
  RSVG_GRADIENT_REFLECT,
  RSVG_GRADIENT_REPEAT
} RsvgGradientSpread;

struct _RsvgPSCtx {
	double x0;
	double y0;
	double x1;
	double y1;

	guint32 color;
	double affine[6];
	RsvgDrawingCtx *ctx;
};

struct _RsvgGradientStop {
	RsvgNode super;
	double offset;
	gboolean is_current_color;
	guint32 rgba;
};

struct _RsvgLinearGradient {
	RsvgNode super;
	gboolean obj_bbox;
	double affine[6]; /* user space to actual at time of gradient def */
	RsvgGradientSpread spread;
	double x1, y1;
	double x2, y2;
	guint32 current_color;
	gboolean has_current_color;
	int hasx1 : 1;
	int hasy1 : 1;
	int hasx2 : 1;
	int hasy2 : 1;
	int hastransform : 1;
	int hasbbox : 1;
	int hasspread : 1;
	RsvgNode * fallback;
};

struct _RsvgRadialGradient {
	RsvgNode super;
	gboolean obj_bbox;
	double affine[6]; /* user space to actual at time of gradient def */
	RsvgGradientSpread spread;
	double cx, cy;
	double r;
	double fx, fy;
	guint32 current_color;
	gboolean has_current_color;
	int hascx : 1;
	int hascy : 1;
	int hasfx : 1;
	int hasfy : 1;
	int hasr : 1;
	int hasspread : 1;
	int hastransform : 1;
	int hasbbox : 1;
	RsvgNode * fallback;
};

struct _RsvgPattern {
	RsvgNode super;
	gboolean obj_cbbox;
	gboolean obj_bbox;
	gboolean vbox;
	double affine[6]; /* user space to actual at time of gradient def */
	double x, y, width, height;
	double vbx, vby, vbh, vbw;
	unsigned int preserve_aspect_ratio;
	int hasx : 1;
	int hasy : 1;
	int haswidth : 1;
	int hasheight : 1;
	int hasvbox : 1;
	int hasaspect : 1;
	int hastransform : 1;
	int hascbox : 1;
	int hasbbox : 1;
	RsvgPattern * fallback;
};

struct _RsvgSolidColour {
	gboolean currentcolour;	
	guint32 rgb;
};

typedef struct _RsvgSolidColour RsvgPaintServerColour;
typedef enum _RsvgPaintServerType RsvgPaintServerType;
typedef union _RsvgPaintServerCore RsvgPaintServerCore;

union _RsvgPaintServerCore {
	RsvgLinearGradient *lingrad;
	RsvgRadialGradient *radgrad;
	RsvgSolidColour *colour;
	RsvgPattern *pattern;
};

enum _RsvgPaintServerType {
	RSVG_PAINT_SERVER_RAD_GRAD, 
	RSVG_PAINT_SERVER_LIN_GRAD, 
	RSVG_PAINT_SERVER_SOLID,
	RSVG_PAINT_SERVER_PATTERN
};

struct _RsvgPaintServer {
	int refcnt;
	RsvgPaintServerType type;
	RsvgPaintServerCore core;
};

/* Create a new paint server based on a specification string. */
RsvgPaintServer *
rsvg_paint_server_parse (gboolean * inherit, const RsvgDefs *defs, const char *str,
						 guint32 current_color);

void
rsvg_paint_server_ref (RsvgPaintServer *ps);

void
rsvg_paint_server_unref (RsvgPaintServer *ps);

RsvgRadialGradient *
rsvg_clone_radial_gradient (const RsvgRadialGradient *grad, gboolean * shallow_cloned);

RsvgLinearGradient *
rsvg_clone_linear_gradient (const RsvgLinearGradient *grad, gboolean * shallow_cloned);

RsvgNode *
rsvg_new_linear_gradient (void);

RsvgNode *
rsvg_new_radial_gradient (void);

RsvgNode *
rsvg_new_stop(void);

RsvgNode *
rsvg_new_pattern (void);

void
rsvg_pattern_fix_fallback(RsvgPattern * pattern);

void
rsvg_linear_gradient_fix_fallback(RsvgLinearGradient * grad);

void
rsvg_radial_gradient_fix_fallback(RsvgRadialGradient * grad);

G_END_DECLS

#endif
