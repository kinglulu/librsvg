/* vim: set sw=4: -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* 
   rsvg.h: SAX-based renderer for SVG files into a GdkPixbuf.
 
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

#ifndef RSVG_H
#define RSVG_H

#include <gdk-pixbuf/gdk-pixbuf.h>

G_BEGIN_DECLS

typedef enum {
	RSVG_ERROR_FAILED
} RsvgError;

#define RSVG_ERROR (rsvg_error_quark ())
GQuark rsvg_error_quark (void) G_GNUC_CONST;

typedef struct RsvgHandle RsvgHandle;

/**
 * RsvgSizeFunc ():
 * @width: Pointer to where to set/store the width
 * @height: Pointer to where to set/store the height
 * @user_data: User data pointer
 *
 * Function to let a user of the library specify the SVG's dimensions
 * @width: the ouput width the SVG should be
 * @height: the output height the SVG should be
 * @user_data: user data
 */
typedef void (* RsvgSizeFunc) (gint     *width,
							   gint     *height,
							   gpointer  user_data);


#ifndef RSVG_DISABLE_DEPRECATED
void        rsvg_set_default_dpi          (double dpi);
void        rsvg_handle_set_dpi           (RsvgHandle * handle, double dpi);
#endif

void        rsvg_set_default_dpi_x_y          (double dpi_x, double dpi_y);
void        rsvg_handle_set_dpi_x_y           (RsvgHandle * handle, double dpi_x, double dpi_y);

RsvgHandle *rsvg_handle_new               (void);
void        rsvg_handle_set_size_callback (RsvgHandle      *handle,
										   RsvgSizeFunc     size_func,
										   gpointer         user_data,
										   GDestroyNotify   user_data_destroy);
gboolean    rsvg_handle_write             (RsvgHandle      *handle,
										   const guchar    *buf,
										   gsize            count,
										   GError         **error);
gboolean    rsvg_handle_close             (RsvgHandle      *handle,
										   GError         **error);
GdkPixbuf  *rsvg_handle_get_pixbuf        (RsvgHandle      *handle);
void        rsvg_handle_free              (RsvgHandle      *handle);

/* Convenience API */

GdkPixbuf  *rsvg_pixbuf_from_file                  (const gchar  *file_name,
													GError      **error);
GdkPixbuf  *rsvg_pixbuf_from_file_at_zoom          (const gchar  *file_name,
													double        x_zoom,
													double        y_zoom,
													GError      **error);
GdkPixbuf  *rsvg_pixbuf_from_file_at_size          (const gchar  *file_name,
													gint          width,
													gint          height,
													GError      **error);
GdkPixbuf  *rsvg_pixbuf_from_file_at_max_size      (const gchar  *file_name,
													gint          max_width,
													gint          max_height,
													GError      **error);
GdkPixbuf  *rsvg_pixbuf_from_file_at_zoom_with_max (const gchar  *file_name,
													double        x_zoom,
													double        y_zoom,
													gint          max_width,
													gint          max_height,
													GError      **error);

/* Accessibility API */

G_CONST_RETURN char *rsvg_handle_get_title         (RsvgHandle *handle);
G_CONST_RETURN char *rsvg_handle_get_desc          (RsvgHandle *handle);

/* Extended Convenience API */

GdkPixbuf  * rsvg_pixbuf_from_file_at_size_ex (RsvgHandle * handle,
											   const gchar  *file_name,
											   gint          width,
											   gint          height,
											   GError      **error);
GdkPixbuf  * rsvg_pixbuf_from_file_ex (RsvgHandle * handle,
									   const gchar  *file_name,
									   GError      **error);
GdkPixbuf  * rsvg_pixbuf_from_file_at_zoom_ex (RsvgHandle * handle,
											   const gchar  *file_name,
											   double        x_zoom,
											   double        y_zoom,
											   GError      **error);
GdkPixbuf  * rsvg_pixbuf_from_file_at_max_size_ex (RsvgHandle * handle,
												   const gchar  *file_name,
												   gint          max_width,
												   gint          max_height,
												   GError      **error);
GdkPixbuf  * rsvg_pixbuf_from_file_at_zoom_with_max_ex (RsvgHandle * handle,
														const gchar  *file_name,
														double        x_zoom,
														double        y_zoom,
														gint          max_width,
														gint          max_height,
														GError      **error);

G_END_DECLS

#endif /* RSVG_H */
