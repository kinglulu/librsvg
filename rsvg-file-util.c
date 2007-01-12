/* vim: set sw=4 sts=4: -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
   rsvg-file-util.c: SAX-based renderer for SVG files into a GdkPixbuf.

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

#include "config.h"
#include "rsvg.h"
#include "rsvg-private.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define SVG_BUFFER_SIZE (1024 * 8)

void
_rsvg_size_callback (int *width, int *height, gpointer data)
{
    struct RsvgSizeCallbackData *real_data = (struct RsvgSizeCallbackData *) data;
    double zoomx, zoomy, zoom;

    int in_width, in_height;

    in_width = *width;
    in_height = *height;

    switch (real_data->type) {
    case RSVG_SIZE_ZOOM:
        if (*width < 0 || *height < 0)
            return;

        *width = floor (real_data->x_zoom * *width + 0.5);
        *height = floor (real_data->y_zoom * *height + 0.5);
        break;

    case RSVG_SIZE_ZOOM_MAX:
        if (*width < 0 || *height < 0)
            return;

        *width = floor (real_data->x_zoom * *width + 0.5);
        *height = floor (real_data->y_zoom * *height + 0.5);

        if (*width > real_data->width || *height > real_data->height) {
            zoomx = (double) real_data->width / *width;
            zoomy = (double) real_data->height / *height;
            zoom = MIN (zoomx, zoomy);

            *width = floor (zoom * *width + 0.5);
            *height = floor (zoom * *height + 0.5);
        }
        break;

    case RSVG_SIZE_WH_MAX:
        if (*width < 0 || *height < 0)
            return;

        zoomx = (double) real_data->width / *width;
        zoomy = (double) real_data->height / *height;
        if (zoomx < 0)
            zoom = zoomy;
        else if (zoomy < 0)
            zoom = zoomx;
        else
            zoom = MIN (zoomx, zoomy);

        *width = floor (zoom * *width + 0.5);
        *height = floor (zoom * *height + 0.5);
        break;

    case RSVG_SIZE_WH:

        if (real_data->width != -1)
            *width = real_data->width;
        if (real_data->height != -1)
            *height = real_data->height;
        break;

    default:
        g_assert_not_reached ();
    }

    if (real_data->keep_aspect_ratio) {
        int out_min = MIN (*width, *height);

        if (out_min == *width) {
            *height = in_height * ((double) *width / (double) in_width);
        } else {
            *width = in_width * ((double) *height / (double) in_height);
        }
    }
}

/* private */
GdkPixbuf *
rsvg_pixbuf_from_data_with_size_data (const guchar * buff,
                                      size_t len,
                                      struct RsvgSizeCallbackData *data,
                                      const char *base_uri, GError ** error)
{
    RsvgHandle *handle;
    GdkPixbuf *retval;

    handle = rsvg_handle_new ();

    if (!handle) {
        g_set_error (error, rsvg_error_quark (), 0, _("Error creating SVG reader"));
        return NULL;
    }

    rsvg_handle_set_size_callback (handle, _rsvg_size_callback, data, NULL);
    rsvg_handle_set_base_uri (handle, base_uri);

    if (!rsvg_handle_write (handle, buff, len, error)) {
        g_object_unref (G_OBJECT (handle));
        return NULL;
    }

    if (!rsvg_handle_close (handle, error)) {
        g_object_unref (G_OBJECT (handle));
        return NULL;
    }

    retval = rsvg_handle_get_pixbuf (handle);
    g_object_unref (G_OBJECT (handle));

    return retval;
}

static GdkPixbuf *
rsvg_pixbuf_from_stdio_file_with_size_data (GByteArray * f,
                                            struct RsvgSizeCallbackData *data,
                                            gchar * base_uri, GError ** error)
{
    RsvgHandle *handle;
    GdkPixbuf *retval;

    handle = rsvg_handle_new ();

    if (!handle) {
        g_set_error (error, rsvg_error_quark (), 0, _("Error creating SVG reader"));
        return NULL;
    }

    rsvg_handle_set_size_callback (handle, _rsvg_size_callback, data, NULL);
    rsvg_handle_set_base_uri (handle, base_uri);

    if (!rsvg_handle_write (handle, f->data, f->len, error)) {
        g_object_unref (G_OBJECT (handle));
        return NULL;
    }

    if (!rsvg_handle_close (handle, error)) {
        g_object_unref (G_OBJECT (handle));
        return NULL;
    }

    retval = rsvg_handle_get_pixbuf (handle);
    g_object_unref (G_OBJECT (handle));

    return retval;
}

static GdkPixbuf *
rsvg_pixbuf_from_file_with_size_data (const gchar * file_name,
                                      struct RsvgSizeCallbackData *data, GError ** error)
{
    GdkPixbuf *pixbuf;
    GByteArray *f;
    GString *base_uri = g_string_new (file_name);

    f = _rsvg_acquire_xlink_href_resource (file_name, base_uri->str, error);

    if (f) {
        pixbuf = rsvg_pixbuf_from_stdio_file_with_size_data (f, data, base_uri->str, error);
        g_byte_array_free (f, TRUE);
    } else {
        pixbuf = NULL;
    }

    g_string_free (base_uri, TRUE);

    return pixbuf;
}

/**
 * rsvg_pixbuf_from_file:
 * @file_name: A file name
 * @error: return location for errors
 * 
 * Loads a new #GdkPixbuf from @file_name and returns it.  The caller must
 * assume the reference to the reurned pixbuf. If an error occurred, @error is
 * set and %NULL is returned.
 * 
 * Return value: A newly allocated #GdkPixbuf, or %NULL
 * Deprecated: Set up a cairo matrix and use rsvg_handle_new_from_file() + rsvg_handle_render_cairo() instead.
 **/
GdkPixbuf *
rsvg_pixbuf_from_file (const gchar * file_name, GError ** error)
{
    return rsvg_pixbuf_from_file_at_size (file_name, -1, -1, error);
}

/**
 * rsvg_pixbuf_from_file_at_zoom:
 * @file_name: A file name
 * @x_zoom: The horizontal zoom factor
 * @y_zoom: The vertical zoom factor
 * @error: return location for errors
 * 
 * Loads a new #GdkPixbuf from @file_name and returns it.  This pixbuf is scaled
 * from the size indicated by the file by a factor of @x_zoom and @y_zoom.  The
 * caller must assume the reference to the returned pixbuf. If an error
 * occurred, @error is set and %NULL is returned.
 * 
 * Return value: A newly allocated #GdkPixbuf, or %NULL
 * Deprecated: Set up a cairo matrix and use rsvg_handle_new_from_file() + rsvg_handle_render_cairo() instead.
 **/
GdkPixbuf *
rsvg_pixbuf_from_file_at_zoom (const gchar * file_name,
                               double x_zoom, double y_zoom, GError ** error)
{
    struct RsvgSizeCallbackData data;

    g_return_val_if_fail (file_name != NULL, NULL);
    g_return_val_if_fail (x_zoom > 0.0 && y_zoom > 0.0, NULL);

    data.type = RSVG_SIZE_ZOOM;
    data.x_zoom = x_zoom;
    data.y_zoom = y_zoom;
    data.keep_aspect_ratio = FALSE;

    return rsvg_pixbuf_from_file_with_size_data (file_name, &data, error);
}

/**
 * rsvg_pixbuf_from_file_at_zoom_with_max:
 * @file_name: A file name
 * @x_zoom: The horizontal zoom factor
 * @y_zoom: The vertical zoom factor
 * @max_width: The requested max width
 * @max_height: The requested max heigh
 * @error: return location for errors
 * 
 * Loads a new #GdkPixbuf from @file_name and returns it.  This pixbuf is scaled
 * from the size indicated by the file by a factor of @x_zoom and @y_zoom. If the
 * resulting pixbuf would be larger than max_width/max_heigh it is uniformly scaled
 * down to fit in that rectangle. The caller must assume the reference to the
 * returned pixbuf. If an error occurred, @error is set and %NULL is returned.
 * 
 * Return value: A newly allocated #GdkPixbuf, or %NULL
 * Deprecated: Set up a cairo matrix and use rsvg_handle_new_from_file() + rsvg_handle_render_cairo() instead.
 **/
GdkPixbuf *
rsvg_pixbuf_from_file_at_zoom_with_max (const gchar * file_name,
                                        double x_zoom,
                                        double y_zoom,
                                        gint max_width, gint max_height, GError ** error)
{
    struct RsvgSizeCallbackData data;

    g_return_val_if_fail (file_name != NULL, NULL);
    g_return_val_if_fail (x_zoom > 0.0 && y_zoom > 0.0, NULL);

    data.type = RSVG_SIZE_ZOOM_MAX;
    data.x_zoom = x_zoom;
    data.y_zoom = y_zoom;
    data.width = max_width;
    data.height = max_height;
    data.keep_aspect_ratio = FALSE;

    return rsvg_pixbuf_from_file_with_size_data (file_name, &data, error);
}

/**
 * rsvg_pixbuf_from_file_at_size:
 * @file_name: A file name
 * @width: The new width, or -1
 * @height: The new height, or -1
 * @error: return location for errors
 * 
 * Loads a new #GdkPixbuf from @file_name and returns it.  This pixbuf is scaled
 * from the size indicated to the new size indicated by @width and @height.  If
 * either of these are -1, then the default size of the image being loaded is
 * used.  The caller must assume the reference to the returned pixbuf. If an
 * error occurred, @error is set and %NULL is returned.
 * 
 * Return value: A newly allocated #GdkPixbuf, or %NULL
 * Deprecated: Set up a cairo matrix and use rsvg_handle_new_from_file() + rsvg_handle_render_cairo() instead.
 **/
GdkPixbuf *
rsvg_pixbuf_from_file_at_size (const gchar * file_name, gint width, gint height, GError ** error)
{
    struct RsvgSizeCallbackData data;

    data.type = RSVG_SIZE_WH;
    data.width = width;
    data.height = height;
    data.keep_aspect_ratio = FALSE;

    return rsvg_pixbuf_from_file_with_size_data (file_name, &data, error);
}

/**
 * rsvg_pixbuf_from_file_at_max_size:
 * @file_name: A file name
 * @max_width: The requested max width
 * @max_height: The requested max heigh
 * @error: return location for errors
 * 
 * Loads a new #GdkPixbuf from @file_name and returns it.  This pixbuf is uniformly
 * scaled so that the it fits into a rectangle of size max_width * max_height. The
 * caller must assume the reference to the returned pixbuf. If an error occurred,
 * @error is set and %NULL is returned.
 * 
 * Return value: A newly allocated #GdkPixbuf, or %NULL
 * Deprecated: Set up a cairo matrix and use rsvg_handle_new_from_file() + rsvg_handle_render_cairo() instead.
 **/
GdkPixbuf *
rsvg_pixbuf_from_file_at_max_size (const gchar * file_name,
                                   gint max_width, gint max_height, GError ** error)
{
    struct RsvgSizeCallbackData data;

    data.type = RSVG_SIZE_WH_MAX;
    data.width = max_width;
    data.height = max_height;
    data.keep_aspect_ratio = FALSE;

    return rsvg_pixbuf_from_file_with_size_data (file_name, &data, error);
}
