/* 
   rsvg.h: SAX-based renderer for SVG files into a GdkPixbuf.
 
   Copyright (C) 2000 Eazel, Inc.
  
   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.
  
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
  
   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the
   Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
  
   Author: Raph Levien <raph@artofcode.com>
*/

#ifndef RSVG_H
#define RSVG_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

void       rsvg_set_fonts_dir (const char *fonts_dir);
GdkPixbuf *rsvg_render_file   (FILE       *f,
			       double      zoom);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
