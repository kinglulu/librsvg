/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*-

   test-rsvg.c: Command line utility for exercising rsvg.
 
   Copyright (C) 2000 Eazel, Inc.
  
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

#include "config.h"
#include "rsvg.h"

#include <popt.h>
#include <stdio.h>
#include <stdlib.h>

int
main (int argc, const char **argv)
{
	poptContext popt_context;
	char *x_zoom_str = "1.0";
	char *y_zoom_str = "1.0";
	struct poptOption options_table[] = {
		{ "x-zoom", 'x', POPT_ARG_STRING, &x_zoom_str, 0, NULL, "zoom factor" },
		{ "y-zoom", 'y', POPT_ARG_STRING, &y_zoom_str, 0, NULL, "zoom factor" },
		POPT_AUTOHELP
		{ NULL, 0, 0, NULL, 0 }
	};
	char c;
	const char * const *args;
	GdkPixbuf *pixbuf;

	g_type_init ();

	popt_context = poptGetContext ("test-rsvg", argc, argv, options_table, 0);

	c = poptGetNextOpt (popt_context);
	args = poptGetArgs (popt_context);

	pixbuf = rsvg_pixbuf_from_file_at_zoom (args[0],
						atof (x_zoom_str),
						atof (y_zoom_str),
						NULL);
	if (pixbuf) {
		if (args[1] != NULL)
			gdk_pixbuf_save (pixbuf, args[1], "png", NULL, NULL);
	} else {
		fprintf (stderr, "Error loading SVG file.\n");
		return 1;
	}
	return 0;
}
