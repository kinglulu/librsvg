/*
   rsvg.c: SAX-based renderer for SVG files into a GdkPixbuf.

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

#include <math.h>
#include <string.h>
#include <stdarg.h>

#include <libart_lgpl/art_affine.h>
#include <libart_lgpl/art_vpath_bpath.h>
#include <libart_lgpl/art_vpath_dash.h>
#include <libart_lgpl/art_svp_vpath_stroke.h>
#include <libart_lgpl/art_svp_vpath.h>
#include <libart_lgpl/art_svp_intersect.h>
#include <libart_lgpl/art_render_mask.h>
#include <libart_lgpl/art_render_svp.h>
#include <libart_lgpl/art_rgba.h>
#include <libart_lgpl/art_rgb_affine.h>
#include <libart_lgpl/art_rgb_rgba_affine.h>

#include <libxml/SAX.h>
#include <libxml/xmlmemory.h>

#include <pango/pangoft2.h>

#if ENABLE_GNOME_VFS
#include <libgnomevfs/gnome-vfs.h>
#endif

#include "rsvg-bpath-util.h"
#include "rsvg-path.h"
#include "rsvg-css.h"
#include "rsvg-paint-server.h"

#define SVG_BUFFER_SIZE (1024 * 8)

/* 4/3 * (1-cos 45)/sin 45 = 4/3 * sqrt(2) - 1 */
#define RSVG_ARC_MAGIC ((double) 0.5522847498)

/*
 * This is configurable at runtime
 */
#define RSVG_DEFAULT_DPI 90.0
static double internal_dpi = RSVG_DEFAULT_DPI;

typedef struct {
  double affine[6];

  gint opacity; /* 0..255 */

  RsvgPaintServer *fill;
  gint fill_opacity; /* 0..255 */

  RsvgPaintServer *stroke;
  gint stroke_opacity; /* 0..255 */
  double stroke_width;
  double miter_limit;

  ArtPathStrokeCapType cap;
  ArtPathStrokeJoinType join;

  double font_size;
  char *font_family;
  guint text_offset;

  guint32 stop_color; /* rgb */
  gint stop_opacity; /* 0..255 */

  ArtVpathDash dash;

  GdkPixbuf *save_pixbuf;
} RsvgState;

typedef struct RsvgSaxHandler RsvgSaxHandler;

struct RsvgSaxHandler {
  void (*free) (RsvgSaxHandler *self);
  void (*start_element) (RsvgSaxHandler *self, const xmlChar *name, const xmlChar **atts);
  void (*end_element) (RsvgSaxHandler *self, const xmlChar *name);
  void (*characters) (RsvgSaxHandler *self, const xmlChar *ch, int len);
};

struct RsvgHandle {
  RsvgSizeFunc size_func;
  gpointer user_data;
  GDestroyNotify user_data_destroy;
  GdkPixbuf *pixbuf;

  /* stack; there is a state for each element */
  RsvgState *state;
  int n_state;
  int n_state_max;

  RsvgDefs *defs;
  GHashTable *css_props;

  /* not a handler stack. each nested handler keeps
   * track of its parent
   */
  RsvgSaxHandler *handler;
  int handler_nest;

  GHashTable *entities; /* g_malloc'd string -> xmlEntityPtr */

  PangoContext *pango_context;
  xmlParserCtxtPtr ctxt;
  GError **error;

  int width;
  int height;
  double dpi;
};

static gdouble
rsvg_viewport_percentage (gdouble width, gdouble height)
{
  return ((width * width) + (height * height)) / M_SQRT2;
}

static void
rsvg_state_init (RsvgState *state)
{
  memset (state, 0, sizeof (*state));

  art_affine_identity (state->affine);

  state->opacity = 0xff;
  state->fill = rsvg_paint_server_parse (NULL, "#000");
  state->fill_opacity = 0xff;
  state->stroke_opacity = 0xff;
  state->stroke_width = 1;
  state->miter_limit = 4;
  state->cap = ART_PATH_STROKE_CAP_BUTT;
  state->join = ART_PATH_STROKE_JOIN_MITER;
  state->stop_opacity = 0xff;
}

static void
rsvg_state_clone (RsvgState *dst, const RsvgState *src)
{
  gint i;

  *dst = *src;
  dst->font_family = g_strdup (src->font_family);
  rsvg_paint_server_ref (dst->fill);
  rsvg_paint_server_ref (dst->stroke);
  dst->save_pixbuf = NULL;

  if (src->dash.n_dash > 0)
    {
      dst->dash.dash = g_new (gdouble, src->dash.n_dash);
      for (i = 0; i < src->dash.n_dash; i++)
	dst->dash.dash[i] = src->dash.dash[i];
    }
}

static void
rsvg_state_finalize (RsvgState *state)
{
  g_free (state->font_family);
  rsvg_paint_server_unref (state->fill);
  rsvg_paint_server_unref (state->stroke);

  if (state->dash.n_dash != 0)
    g_free (state->dash.dash);
}

static void
rsvg_ctx_free_helper (gpointer key, gpointer value, gpointer user_data)
{
  xmlEntityPtr entval = (xmlEntityPtr)value;

  /* key == entval->name, so it's implicitly freed below */

  g_free ((char *) entval->name);
  g_free ((char *) entval->ExternalID);
  g_free ((char *) entval->SystemID);
  xmlFree (entval->content);
  xmlFree (entval->orig);
  g_free (entval);
}

static void
rsvg_pixmap_destroy (guchar *pixels, gpointer data)
{
  g_free (pixels);
}

static void
rsvg_start_svg (RsvgHandle *ctx, const xmlChar **atts)
{
  int i;
  int width = -1, height = -1, x = -1, y = -1;
  int rowstride;
  art_u8 *pixels;
  gint percent, em, ex;
  RsvgState *state;
  gboolean has_alpha = TRUE;
  gint new_width, new_height;
  double x_zoom = 1.;
  double y_zoom = 1.;

  double vbox_x = 0, vbox_y = 0, vbox_w = 0, vbox_h = 0;
  gboolean has_vbox = TRUE;

  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  /* x & y should be ignored since we should always be the outermost SVG,
	     at least for now, but i'll include them here anyway */
	  if (!strcmp ((char *)atts[i], "width"))
	    width = rsvg_css_parse_length ((char *)atts[i + 1], ctx->dpi, &percent, &em, &ex);
	  else if (!strcmp ((char *)atts[i], "height"))	    
	    height = rsvg_css_parse_length ((char *)atts[i + 1], ctx->dpi, &percent, &em, &ex);
	  else if (!strcmp ((char *)atts[i], "x"))	    
	    x = rsvg_css_parse_length ((char *)atts[i + 1], ctx->dpi, &percent, &em, &ex);
	  else if (!strcmp ((char *)atts[i], "y"))	    
	    y = rsvg_css_parse_length ((char *)atts[i + 1], ctx->dpi, &percent, &em, &ex);
	  else if (!strcmp ((char *)atts[i], "viewBox"))
	    {
	      /* todo: viewbox can have whitespace and/or comma but we're only likely to see
		 these 2 combinations */
	      if (4 == sscanf ((char *)atts[i + 1], " %lf %lf %lf %lf ", &vbox_x, &vbox_y, &vbox_w, &vbox_h) ||
		  4 == sscanf ((char *)atts[i + 1], " %lf , %lf , %lf , %lf ", &vbox_x, &vbox_y, &vbox_w, &vbox_h))
		has_vbox = TRUE;
	    }
	}

      if (has_vbox && vbox_w > 0. && vbox_h > 0.)
	{
	  new_width  = (int)floor (vbox_w);
	  new_height = (int)floor (vbox_h);

	  /* apply the sizing function on the *original* width and height
	     to acquire our real destination size. we'll scale it against
	     the viewBox's coordinates later */
	  if (ctx->size_func)
	    (* ctx->size_func) (&width, &height, ctx->user_data);
	}
      else
	{
	  new_width  = width;
	  new_height = height;

	  /* apply the sizing function to acquire our new width and height.
	     we'll scale this against the old values later */
	  if (ctx->size_func)
	    (* ctx->size_func) (&new_width, &new_height, ctx->user_data);
	}

      /* set these here because % are relative to viewbox */
      ctx->width = new_width;
      ctx->height = new_height;

      if (!has_vbox)
	{
	  x_zoom = (width < 0 || new_width < 0) ? 1 : (double) new_width / width;
	  y_zoom = (height < 0 || new_height < 0) ? 1 : (double) new_height / height;
	}
      else
	{
#if 1
	  x_zoom = (width < 0 || new_width < 0) ? 1 : (double) width / new_width;
	  y_zoom = (height < 0 || new_height < 0) ? 1 : (double) height / new_height;
#else
	  x_zoom = (width < 0 || new_width < 0) ? 1 : (double) new_width / width;
	  y_zoom = (height < 0 || new_height < 0) ? 1 : (double) new_height / height;	  
#endif

	  /* reset these so that we get a properly sized SVG and not a huge one */
	  new_width  = (width == -1 ? new_width : width);
	  new_height = (height == -1 ? new_height : height);
	}

      /* Scale size of target pixbuf */
      state = &ctx->state[ctx->n_state - 1];
      art_affine_scale (state->affine, x_zoom, y_zoom);

#if 0
      if (vbox_x != 0. || vbox_y != 0.)
	{
	  double affine[6];
	  art_affine_translate (affine, vbox_x, vbox_y);
	  art_affine_multiply (state->affine, affine, state->affine);
	}
#endif

      if (new_width < 0 || new_height < 0)
        {
          g_warning ("rsvg_start_svg: width and height not specified in the SVG, nor supplied by the size callback");
          if (new_width < 0) new_width = 500;
          if (new_height < 0) new_height = 500;
        }

      if (new_width >= INT_MAX / 4)
        {
          /* FIXME: GError here? */
          g_warning ("rsvg_start_svg: width too large");
	  return;
        }
      rowstride = (new_width * (has_alpha ? 4 : 3) + 3) & ~3;
      if (rowstride > INT_MAX / new_height)
        {
          /* FIXME: GError here? */
          g_warning ("rsvg_start_svg: width too large");
	  return;
        }

      /* FIXME: Add GError here if size is too big. */

      pixels = g_try_malloc (rowstride * new_height);
      if (pixels == NULL)
        {
          /* FIXME: GError here? */
          g_warning ("rsvg_start_svg: dimensions too large");
	  return;
        }
      memset (pixels, has_alpha ? 0 : 255, rowstride * new_height);
      ctx->pixbuf = gdk_pixbuf_new_from_data (pixels,
					      GDK_COLORSPACE_RGB,
					      has_alpha, 8,
					      new_width, new_height,
					      rowstride,
					      rsvg_pixmap_destroy,
					      NULL);
    }
}

/* Parse a CSS2 style argument, setting the SVG context attributes. */
static void
rsvg_parse_style_arg (RsvgHandle *ctx, RsvgState *state, const char *str)
{
  int arg_off;

  arg_off = rsvg_css_param_arg_offset (str);
  if (rsvg_css_param_match (str, "opacity"))
    {
      state->opacity = rsvg_css_parse_opacity (str + arg_off);
    }
  else if (rsvg_css_param_match (str, "fill"))
    {
      rsvg_paint_server_unref (state->fill);
      state->fill = rsvg_paint_server_parse (ctx->defs, str + arg_off);
    }
  else if (rsvg_css_param_match (str, "fill-opacity"))
    {
      state->fill_opacity = rsvg_css_parse_opacity (str + arg_off);
    }
  else if (rsvg_css_param_match (str, "stroke"))
    {
      rsvg_paint_server_unref (state->stroke);
      state->stroke = rsvg_paint_server_parse (ctx->defs, str + arg_off);
    }
  else if (rsvg_css_param_match (str, "stroke-width"))
    {
      state->stroke_width = rsvg_css_parse_normalized_length (str + arg_off, ctx->dpi, 
							      (gdouble)ctx->height, state->font_size, 0.);
    }
  else if (rsvg_css_param_match (str, "stroke-linecap"))
    {
      if (!strcmp (str + arg_off, "butt"))
	state->cap = ART_PATH_STROKE_CAP_BUTT;
      else if (!strcmp (str + arg_off, "round"))
	state->cap = ART_PATH_STROKE_CAP_ROUND;
      else if (!strcmp (str + arg_off, "square"))
	state->cap = ART_PATH_STROKE_CAP_SQUARE;
      else
	g_warning ("unknown line cap style %s", str + arg_off);
    }
  else if (rsvg_css_param_match (str, "stroke-opacity"))
    {
      state->stroke_opacity = rsvg_css_parse_opacity (str + arg_off);
    }
  else if (rsvg_css_param_match (str, "stroke-linejoin"))
    {
      if (!strcmp (str + arg_off, "miter"))
	state->join = ART_PATH_STROKE_JOIN_MITER;
      else if (!strcmp (str + arg_off, "round"))
	state->join = ART_PATH_STROKE_JOIN_ROUND;
      else if (!strcmp (str + arg_off, "bevel"))
	state->join = ART_PATH_STROKE_JOIN_BEVEL;
      else
	g_warning ("unknown line join style %s", str + arg_off);
    }
  else if (rsvg_css_param_match (str, "font-size"))
    {
      state->font_size = rsvg_css_parse_normalized_length (str + arg_off, ctx->dpi, 
							   (gdouble)ctx->height, state->font_size, 0.);
    }
  else if (rsvg_css_param_match (str, "font-family"))
    {
      g_free (state->font_family);
      state->font_family = g_strdup (str + arg_off);
    }
  else if (rsvg_css_param_match (str, "stop-color"))
    {
      state->stop_color = rsvg_css_parse_color (str + arg_off);
    }
  else if (rsvg_css_param_match (str, "stop-opacity"))
    {
      state->stop_opacity = rsvg_css_parse_opacity (str + arg_off);
    }
  else if (rsvg_css_param_match (str, "stroke-miterlimit"))
    {
      state->miter_limit = g_ascii_strtod (str + arg_off, NULL);
    }
  else if (rsvg_css_param_match (str, "stroke-dashoffset"))
    {
      state->dash.offset = rsvg_css_parse_normalized_length (str + arg_off, ctx->dpi, 
							     rsvg_viewport_percentage((gdouble)ctx->width, (gdouble)ctx->height), state->font_size, 0.);
      if (state->dash.offset < 0.)
	state->dash.offset = 0.;
    }
  else if (rsvg_css_param_match (str, "stroke-dasharray"))
    {
      if(!strcmp(str + arg_off, "none"))
	{
            if (state->dash.n_dash != 0)
            {
                /* free any cloned dash data */
                g_free (state->dash.dash);
                state->dash.n_dash = 0; 
            }
	}
      else
	{
	  gchar ** dashes = g_strsplit (str + arg_off, ",", -1);
	  if (NULL != dashes)
	    {
	      gint n_dashes, i;
	      gboolean is_even = FALSE ;

	      /* count the #dashes */
	      for (n_dashes = 0; dashes[n_dashes] != NULL; n_dashes++)
		;

	      is_even = (n_dashes % 2 == 0);
	      state->dash.n_dash = (is_even ? n_dashes : n_dashes * 2);
	      state->dash.dash = g_new (double, state->dash.n_dash);

	      /* TODO: handle negative value == error case */

	      /* the even and base case */
	      for (i = 0; i < n_dashes; i++)
		state->dash.dash[i] = g_ascii_strtod (dashes[i], NULL);

	      /* if an odd number of dashes is found, it gets repeated */
	      if (!is_even)
		for (; i < state->dash.n_dash; i++)
		  state->dash.dash[i] = g_ascii_strtod (dashes[i - n_dashes], NULL);

	      g_strfreev (dashes) ;
	    }
	}
    }
}

/* tell whether @str is a supported style argument 
   whenever something gets added to parse_arg, please
   remember to add it here too
*/
static gboolean
rsvg_is_style_arg(const char *str)
{
  static GHashTable *styles = NULL;
  if (!styles)
    {
      styles = g_hash_table_new (g_str_hash, g_str_equal);
      
      g_hash_table_insert (styles, "fill",              GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "fill-opacity",      GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "font-family",       GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "font-size",         GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "opacity",           GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "stop-color",        GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "stop-opacity",      GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "stroke",            GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "stroke-dasharray",  GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "stroke-dashoffset", GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "stroke-linecap",    GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "stroke-linejoin",   GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "stroke-miterlimit", GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "stroke-opacity",    GINT_TO_POINTER (TRUE));
      g_hash_table_insert (styles, "stroke-width",      GINT_TO_POINTER (TRUE));
    }
  
  /* this will default to 0 (FALSE) on a failed lookup */
  return GPOINTER_TO_INT (g_hash_table_lookup (styles, str)); 
}

/* take a pair of the form (fill="#ff00ff") and parse it as a style */
static void
rsvg_parse_style_pair (RsvgHandle *ctx, RsvgState *state, 
		       const char *key, const char *val)
{
  gchar * str = g_strdup_printf ("%s:%s", key, val);
  rsvg_parse_style_arg (ctx, state, str);
  g_free (str);
}

/* Split a CSS2 style into individual style arguments, setting attributes
   in the SVG context.

   It's known that this is _way_ out of spec. A more complete CSS2
   implementation will happen later.
*/
static void
rsvg_parse_style (RsvgHandle *ctx, RsvgState *state, const char *str)
{
  int start, end;
  char *arg;

  start = 0;
  while (str[start] != '\0')
    {
      for (end = start; str[end] != '\0' && str[end] != ';'; end++);
      arg = g_new (char, 1 + end - start);
      memcpy (arg, str + start, end - start);
      arg[end - start] = '\0';
      rsvg_parse_style_arg (ctx, state, arg);
      g_free (arg);
      start = end;
      if (str[start] == ';') start++;
      while (str[start] == ' ') start++;
    }
}

/*
 * Extremely poor man's CSS parser. Not robust. Not compliant.
 * Should work well enough for our needs ;-)
 */
static void
rsvg_parse_cssbuffer (RsvgHandle *ctx, const char * buff, size_t buflen)
{
  size_t loc = 0;
  
  while (loc < buflen)
    {
      GString * style_name = g_string_new (NULL);
      GString * style_props = g_string_new (NULL);

      /* advance to the style's name */
      while (loc < buflen && g_ascii_isspace (buff[loc]))
	loc++;

      while (loc < buflen && !g_ascii_isspace (buff[loc]))
	g_string_append_c (style_name, buff[loc++]);

      /* advance to the first { that defines the style's properties */
      while (loc < buflen && buff[loc++] != '{' )
	;

      while (loc < buflen && g_ascii_isspace (buff[loc]))
	loc++;

      while (buff[loc] != '}')
	{
	  /* suck in and append our property */
	  while (loc < buflen && buff[loc] != ';' && buff[loc] != '}' )
	    g_string_append_c (style_props, buff[loc++]);

	  if (buff[loc] == '}')
	    break;
	  else
	    {
	      g_string_append_c (style_props, ';');
	      
	      /* advance to the next property */
	      loc++;
	      while (g_ascii_isspace (buff[loc]) && loc < buflen)
		loc++;
	    }
	}

      /* push name/style pair into HT */
      g_hash_table_insert (ctx->css_props, style_name->str, style_props->str);
      
      g_string_free (style_name, FALSE);
      g_string_free (style_props, FALSE);

      loc++;
      while (g_ascii_isspace (buff[loc]) && loc < buflen)
	loc++;
    }
}

/* Parse an SVG transform string into an affine matrix. Reference: SVG
   working draft dated 1999-07-06, section 8.5. Return TRUE on
   success. */
static gboolean
rsvg_parse_transform (double dst[6], const char *src)
{
  int idx;
  char keyword[32];
  double args[6];
  int n_args;
  guint key_len;
  double tmp_affine[6];

  art_affine_identity (dst);

  idx = 0;
  while (src[idx])
    {
      /* skip initial whitespace */
      while (g_ascii_isspace (src[idx]))
	idx++;

      /* parse keyword */
      for (key_len = 0; key_len < sizeof (keyword); key_len++)
	{
	  char c;

	  c = src[idx];
	  if (g_ascii_isalpha (c) || c == '-')
	    keyword[key_len] = src[idx++];
	  else
	    break;
	}
      if (key_len >= sizeof (keyword))
	return FALSE;
      keyword[key_len] = '\0';

      /* skip whitespace */
      while (g_ascii_isspace (src[idx]))
	idx++;

      if (src[idx] != '(')
	return FALSE;
      idx++;

      for (n_args = 0; ; n_args++)
	{
	  char c;
	  char *end_ptr;

	  /* skip whitespace */
	  while (g_ascii_isspace (src[idx]))
	    idx++;
	  c = src[idx];
	  if (g_ascii_isdigit (c) || c == '+' || c == '-' || c == '.')
	    {
	      if (n_args == sizeof(args) / sizeof(args[0]))
		return FALSE; /* too many args */
	      args[n_args] = g_ascii_strtod (src + idx, &end_ptr);
	      idx = end_ptr - src;

	      while (g_ascii_isspace (src[idx]))
		idx++;

	      /* skip optional comma */
	      if (src[idx] == ',')
		idx++;
	    }
	  else if (c == ')')
	    break;
	  else
	    return FALSE;
	}
      idx++;

      /* ok, have parsed keyword and args, now modify the transform */
      if (!strcmp (keyword, "matrix"))
	{
	  if (n_args != 6)
	    return FALSE;
	  art_affine_multiply (dst, args, dst);
	}
      else if (!strcmp (keyword, "translate"))
	{
	  if (n_args == 1)
	    args[1] = 0;
	  else if (n_args != 2)
	    return FALSE;
	  art_affine_translate (tmp_affine, args[0], args[1]);
	  art_affine_multiply (dst, tmp_affine, dst);
	}
      else if (!strcmp (keyword, "scale"))
	{
	  if (n_args == 1)
	    args[1] = args[0];
	  else if (n_args != 2)
	    return FALSE;
	  art_affine_scale (tmp_affine, args[0], args[1]);
	  art_affine_multiply (dst, tmp_affine, dst);
	}
      else if (!strcmp (keyword, "rotate"))
	{
	  if (n_args != 1)
	    return FALSE;
	  art_affine_rotate (tmp_affine, args[0]);
	  art_affine_multiply (dst, tmp_affine, dst);
	}
      else if (!strcmp (keyword, "skewX"))
	{
	  if (n_args != 1)
	    return FALSE;
	  art_affine_shear (tmp_affine, args[0]);
	  art_affine_multiply (dst, tmp_affine, dst);
	}
      else if (!strcmp (keyword, "skewY"))
	{
	  if (n_args != 1)
	    return FALSE;
	  art_affine_shear (tmp_affine, args[0]);
	  /* transpose the affine, given that we know [1] is zero */
	  tmp_affine[1] = tmp_affine[2];
	  tmp_affine[2] = 0;
	  art_affine_multiply (dst, tmp_affine, dst);
	}
      else
	return FALSE; /* unknown keyword */
    }
  return TRUE;
}

/**
 * rsvg_parse_transform_attr: Parse transform attribute and apply to state.
 * @ctx: Rsvg context.
 * @state: State in which to apply the transform.
 * @str: String containing transform.
 *
 * Parses the transform attribute in @str and applies it to @state.
 **/
static void
rsvg_parse_transform_attr (RsvgHandle *ctx, RsvgState *state, const char *str)
{
  double affine[6];

  if (rsvg_parse_transform (affine, str))
    {
      art_affine_multiply (state->affine, affine, state->affine);
    }
  else
    {
      /* parse error for transform attribute. todo: report */
    }
}

static gboolean
rsvg_lookup_apply_css_style (RsvgHandle *ctx, const char * target)
{
  const char * value = (const char *)g_hash_table_lookup (ctx->css_props, target);

  if (value != NULL)
    {
      rsvg_parse_style (ctx, &ctx->state[ctx->n_state - 1],
			value);
      return TRUE;
    }
  return FALSE;
}

/**
 * rsvg_parse_style_attrs: Parse style attribute.
 * @ctx: Rsvg context.
 * @tag: The SVG tag we're processing (eg: circle, ellipse), optionally %NULL
 * @klazz: The space delimited class list, optionally %NULL
 * @atts: Attributes in SAX style.
 *
 * Parses style and transform attributes and modifies state at top of
 * stack.
 **/
static void
rsvg_parse_style_attrs (RsvgHandle *ctx, 
			const char * tag,
			const char * klazz,
			const xmlChar **atts)
{
  int i = 0, j = 0;
  char * target = NULL;
  gboolean found = FALSE;
  GString * klazz_list = NULL;

  if (tag != NULL && klazz != NULL)
    {
      target = g_strdup_printf ("%s.%s", tag, klazz);
      found = rsvg_lookup_apply_css_style (ctx, target);
      g_free (target);
    }
  
  if (found == FALSE)
    {
      if (tag != NULL)
	rsvg_lookup_apply_css_style (ctx, tag);

      if (klazz != NULL)
	{
	  i = strlen (klazz);
	  while (j < i)
	    {
	      klazz_list = g_string_new (".");

	      while (j < i && g_ascii_isspace(klazz[j]))
		j++;

	      while (j < i && !g_ascii_isspace(klazz[j]))
		g_string_append_c (klazz_list, klazz[j++]);

	      rsvg_lookup_apply_css_style (ctx, klazz_list->str);
	      g_string_free (klazz_list, TRUE);
	    }
	}
    }

  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  if (!strcmp ((char *)atts[i], "style"))
	    rsvg_parse_style (ctx, &ctx->state[ctx->n_state - 1],
			      (char *)atts[i + 1]);
	  else if (!strcmp ((char *)atts[i], "transform"))
	    rsvg_parse_transform_attr (ctx, &ctx->state[ctx->n_state - 1],
				       (char *)atts[i + 1]);
	  else if (rsvg_is_style_arg ((char *)atts[i]))
	    rsvg_parse_style_pair (ctx, &ctx->state[ctx->n_state - 1],
				   (char *)atts[i], (char *)atts[i + 1]);
	}
    }
}

/**
 * rsvg_push_opacity_group: Begin a new transparency group.
 * @ctx: Context in which to push.
 *
 * Pushes a new transparency group onto the stack. The top of the stack
 * is stored in the context, while the "saved" value is in the state
 * stack.
 **/
static void
rsvg_push_opacity_group (RsvgHandle *ctx)
{
  RsvgState *state;
  GdkPixbuf *pixbuf;
  art_u8 *pixels;
  int width, height, rowstride;

  state = &ctx->state[ctx->n_state - 1];
  pixbuf = ctx->pixbuf;

  state->save_pixbuf = pixbuf;

  if (pixbuf == NULL)
    {
      /* FIXME: What warning/GError here? */
      return;
    }

  if (!gdk_pixbuf_get_has_alpha (pixbuf))
    {
      g_warning ("push/pop transparency group on non-alpha buffer nyi");
      return;
    }

  width = gdk_pixbuf_get_width (pixbuf);
  height = gdk_pixbuf_get_height (pixbuf);
  rowstride = gdk_pixbuf_get_rowstride (pixbuf);
  pixels = g_new (art_u8, rowstride * height);
  memset (pixels, 0, rowstride * height);

  pixbuf = gdk_pixbuf_new_from_data (pixels,
				     GDK_COLORSPACE_RGB,
				     TRUE,
				     gdk_pixbuf_get_bits_per_sample (pixbuf),
				     width,
				     height,
				     rowstride,
				     rsvg_pixmap_destroy,
				     NULL);
  ctx->pixbuf = pixbuf;
}

/**
 * rsvg_pop_opacity_group: End a transparency group.
 * @ctx: Context in which to push.
 * @opacity: Opacity for blending (0..255).
 *
 * Pops a new transparency group from the stack, recompositing with the
 * next on stack.
 **/
static void
rsvg_pop_opacity_group (RsvgHandle *ctx, int opacity)
{
  RsvgState *state = &ctx->state[ctx->n_state - 1];
  GdkPixbuf *tos, *nos;
  art_u8 *tos_pixels, *nos_pixels;
  int width;
  int height;
  int rowstride;
  int x, y;
  int tmp;

  tos = ctx->pixbuf;
  nos = state->save_pixbuf;

  if (tos == NULL || nos == NULL)
    {
      /* FIXME: What warning/GError here? */
      return;
    }

  if (!gdk_pixbuf_get_has_alpha (nos))
    {
      g_warning ("push/pop transparency group on non-alpha buffer nyi");
      return;
    }

  width = gdk_pixbuf_get_width (tos);
  height = gdk_pixbuf_get_height (tos);
  rowstride = gdk_pixbuf_get_rowstride (tos);

  tos_pixels = gdk_pixbuf_get_pixels (tos);
  nos_pixels = gdk_pixbuf_get_pixels (nos);

  for (y = 0; y < height; y++)
    {
      for (x = 0; x < width; x++)
	{
	  art_u8 r, g, b, a;
	  a = tos_pixels[4 * x + 3];
	  if (a)
	    {
	      r = tos_pixels[4 * x];
	      g = tos_pixels[4 * x + 1];
	      b = tos_pixels[4 * x + 2];
	      tmp = a * opacity + 0x80;
	      a = (tmp + (tmp >> 8)) >> 8;
	      art_rgba_run_alpha (nos_pixels + 4 * x, r, g, b, a, 1);
	    }
	}
      tos_pixels += rowstride;
      nos_pixels += rowstride;
    }

  g_object_unref (tos);
  ctx->pixbuf = nos;
}

static void
rsvg_start_g (RsvgHandle *ctx, const xmlChar **atts)
{
  RsvgState *state = &ctx->state[ctx->n_state - 1];
  const char * klazz = NULL;
  int i;

  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  if (!strcmp ((char *)atts[i], "class"))
	    klazz = (const char *)atts[i + 1];
	}
    }

  rsvg_parse_style_attrs (ctx, "g", klazz, atts);
  if (state->opacity != 0xff)
    rsvg_push_opacity_group (ctx);
}

static void
rsvg_end_g (RsvgHandle *ctx)
{
  RsvgState *state = &ctx->state[ctx->n_state - 1];

  if (state->opacity != 0xff)
    rsvg_pop_opacity_group (ctx, state->opacity);
}

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
rsvg_render_svp (RsvgHandle *ctx, const ArtSVP *svp,
		 RsvgPaintServer *ps, int opacity)
{
  GdkPixbuf *pixbuf;
  ArtRender *render;
  gboolean has_alpha;

  pixbuf = ctx->pixbuf;
  if (pixbuf == NULL)
    {
      /* FIXME: What warning/GError here? */
      return;
    }

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

  art_render_svp (render, svp);
  art_render_mask_solid (render, (opacity << 8) + opacity + (opacity >> 7));
  rsvg_render_paint_server (render, ps, NULL); /* todo: paint server ctx */
  art_render_invoke (render);
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

  state = &ctx->state[ctx->n_state - 1];
  affine_bpath = art_bpath_affine_transform (bpath,
					     state->affine);

  vpath = art_bez_path_to_vec (affine_bpath, 0.25);
  art_free (affine_bpath);

  need_tmpbuf = (state->fill != NULL) && (state->stroke != NULL) &&
    state->opacity != 0xff;

  if (need_tmpbuf)
    rsvg_push_opacity_group (ctx);

  if (state->fill != NULL)
    {
      ArtVpath *closed_vpath;
      ArtSVP *svp2;
      ArtSvpWriter *swr;

      closed_vpath = rsvg_close_vpath (vpath);
      svp = art_svp_from_vpath (closed_vpath);
      g_free (closed_vpath);
      
      swr = art_svp_writer_rewind_new (ART_WIND_RULE_NONZERO);
      art_svp_intersector (svp, swr);

      svp2 = art_svp_writer_rewind_reap (swr);
      art_svp_free (svp);

      opacity = state->fill_opacity;
      if (!need_tmpbuf && state->opacity != 0xff)
	{
	  tmp = opacity * state->opacity + 0x80;
	  opacity = (tmp + (tmp >> 8)) >> 8;
	}
      rsvg_render_svp (ctx, svp2, state->fill, opacity);
      art_svp_free (svp2);
    }

  if (state->stroke != NULL)
    {
      /* todo: libart doesn't yet implement anamorphic scaling of strokes */
      double stroke_width = state->stroke_width *
	art_affine_expansion (state->affine);

      if (stroke_width < 0.25)
	stroke_width = 0.25;

      /* if the path is dashed, stroke it */
      if (state->dash.n_dash > 0) 
	{
	  ArtVpath * dashed_vpath = art_vpath_dash (vpath, &state->dash);
	  art_free (vpath);
	  vpath = dashed_vpath;
	}

      svp = art_svp_vpath_stroke (vpath, state->join, state->cap,
				  stroke_width, state->miter_limit, 0.25);
      opacity = state->stroke_opacity;
      if (!need_tmpbuf && state->opacity != 0xff)
	{
	  tmp = opacity * state->opacity + 0x80;
	  opacity = (tmp + (tmp >> 8)) >> 8;
	}
      rsvg_render_svp (ctx, svp, state->stroke, opacity);
      art_svp_free (svp);
    }

  if (need_tmpbuf)
    rsvg_pop_opacity_group (ctx, state->opacity);

  art_free (vpath);
}

static void
rsvg_render_path(RsvgHandle *ctx, const char *d)
{
  RsvgBpathDef *bpath_def;
  
  bpath_def = rsvg_parse_path (d);
  rsvg_bpath_def_art_finish (bpath_def);
  
  rsvg_render_bpath (ctx, bpath_def->bpath);
  
  rsvg_bpath_def_free (bpath_def);
}

static void
rsvg_start_path (RsvgHandle *ctx, const xmlChar **atts)
{
  int i;
  char *d = NULL;
  const char * klazz = NULL;

  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  if (!strcmp ((char *)atts[i], "d"))
	    d = (char *)atts[i + 1];
	  else if (!strcmp ((char *)atts[i], "class"))
	    klazz = (char *)atts[i + 1];
	}
    }

  if (d == NULL)
    return;

  rsvg_parse_style_attrs (ctx, "path", klazz, atts);
  rsvg_render_path (ctx, d);
}

/* begin text - this should likely get split into its own .c file */

static char *
make_valid_utf8 (const char *str)
{
  GString *string;
  const char *remainder, *invalid;
  int remaining_bytes, valid_bytes;
  
  string = NULL;
  remainder = str;
  remaining_bytes = strlen (str);
  
  while (remaining_bytes != 0)
    {
      if (g_utf8_validate (remainder, remaining_bytes, &invalid))
	break;
      valid_bytes = invalid - remainder;
      
      if (string == NULL) 
	string = g_string_sized_new (remaining_bytes);
      
      g_string_append_len (string, remainder, valid_bytes);
      g_string_append_c (string, '?');

      remaining_bytes -= valid_bytes + 1;
      remainder = invalid + 1;
    }

  if (string == NULL) 
    return g_strdup (str);
  
  g_string_append (string, remainder);
	
  return g_string_free (string, FALSE);
}


typedef struct _RsvgSaxHandlerText {
  RsvgSaxHandler super;
  RsvgSaxHandler *parent;
  RsvgHandle *ctx;
} RsvgSaxHandlerText;

static void
rsvg_text_handler_free (RsvgSaxHandler *self)
{
  g_free (self);
}

static void
rsvg_text_handler_characters (RsvgSaxHandler *self, const xmlChar *ch, int len)
{
  RsvgSaxHandlerText *z = (RsvgSaxHandlerText *)self;
  RsvgHandle *ctx = z->ctx;
  char *string, *tmp;
  int beg, end;
  RsvgState *state;
  ArtRender *render;
  GdkPixbuf *pixbuf;
  gboolean has_alpha;
  int opacity;
  PangoLayout *layout;
  PangoFontDescription *font;
  PangoLayoutLine *line;
  PangoRectangle ink_rect, line_ink_rect;
  FT_Bitmap bitmap;

  state = &ctx->state[ctx->n_state - 1];
  if (state->fill == NULL && state->font_size <= 0)
    {
      return;
    }

  pixbuf = ctx->pixbuf;
  if (pixbuf == NULL)
    {
      /* FIXME: What warning/GError here? */
      return;
    }

  /* Copy ch into string, chopping off leading and trailing whitespace */
  for (beg = 0; beg < len; beg++)
    if (!g_ascii_isspace (ch[beg]))
      break;
  
  for (end = len; end > beg; end--)
    if (!g_ascii_isspace (ch[end - 1]))
      break;
  
  if (end - beg == 0)
    {
      /* TODO: be smarter with some "last was space" logic */
      end = 1; beg = 0;
      string = g_strdup (" ");
    }
  else
    {
      string = g_malloc (end - beg + 1);
      memcpy (string, ch + beg, end - beg);
      string[end - beg] = 0;
    }

  if (!g_utf8_validate (string, -1, NULL))
    {
      tmp = make_valid_utf8 (string);
      g_free (string);
      string = tmp;
    }
  
  if (ctx->pango_context == NULL)
    ctx->pango_context = pango_ft2_get_context ((guint)ctx->dpi, (guint)ctx->dpi);

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
  
  layout = pango_layout_new (ctx->pango_context);
  pango_layout_set_text (layout, string, end - beg);
  font = pango_font_description_copy (pango_context_get_font_description (ctx->pango_context));
  if (state->font_family)
    pango_font_description_set_family_static (font, state->font_family);

  /* we need to resize the font by our X or Y scale (ideally could stretch in both directions...)
     which, though? Y for now */
  pango_font_description_set_size (font, state->font_size * PANGO_SCALE * state->affine[3]);
  pango_layout_set_font_description (layout, font);
  pango_font_description_free (font);
  
  pango_layout_get_pixel_extents (layout, &ink_rect, NULL);
  
  line = pango_layout_get_line (layout, 0);
  if (line == NULL)
    line_ink_rect = ink_rect; /* nothing to draw anyway */
  else
    pango_layout_line_get_pixel_extents (line, &line_ink_rect, NULL);
  
  bitmap.rows = ink_rect.height;
  bitmap.width = ink_rect.width;
  bitmap.pitch = (bitmap.width + 3) & ~3;
  bitmap.buffer = g_malloc0 (bitmap.rows * bitmap.pitch);
  bitmap.num_grays = 0x100;
  bitmap.pixel_mode = ft_pixel_mode_grays;
  
  pango_ft2_render_layout (&bitmap, layout, -ink_rect.x, -ink_rect.y);
  
  g_object_unref (layout);
  
  rsvg_render_paint_server (render, state->fill, NULL); /* todo: paint server ctx */
  opacity = state->fill_opacity * state->opacity;
  opacity = opacity + (opacity >> 7) + (opacity >> 14);

  art_render_mask_solid (render, opacity);
  art_render_mask (render,
		   state->affine[4] + line_ink_rect.x + state->text_offset,
		   state->affine[5] + line_ink_rect.y,
		   state->affine[4] + line_ink_rect.x + bitmap.width + state->text_offset,
		   state->affine[5] + line_ink_rect.y + bitmap.rows,
		   bitmap.buffer, bitmap.pitch);
  art_render_invoke (render);

  g_free (bitmap.buffer);
  g_free (string);

  state->text_offset += line_ink_rect.width;
}

static void
rsvg_start_tspan (RsvgHandle *ctx, const xmlChar **atts)
{
  int i;
  double affine[6] ;
  double x, y, dx, dy;
  RsvgState *state;
  const char * klazz = NULL;
  x = y = dx = dy = 0.;

  state = &ctx->state[ctx->n_state - 1];

  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  if (!strcmp ((char *)atts[i], "x"))
	    x = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "y"))
	    y = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "dx"))
	    dx = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "dy"))
	    dy = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "class"))
	    klazz = (const char *)atts[i + 1];
	}
    }
  
  /* todo: transform() is illegal here */
  x += dx ;
  y += dy ;
  
  if (x > 0 && y > 0)
    {
      art_affine_translate (affine, x, y);
      art_affine_multiply (state->affine, affine, state->affine);
    }
  rsvg_parse_style_attrs (ctx, "tspan", klazz, atts);
}

static void
rsvg_text_handler_start (RsvgSaxHandler *self, const xmlChar *name,
			 const xmlChar **atts)
{
  RsvgSaxHandlerText *z = (RsvgSaxHandlerText *)self;
  RsvgHandle *ctx = z->ctx;

  /* push the state stack */
  if (ctx->n_state == ctx->n_state_max)
    ctx->state = g_renew (RsvgState, ctx->state, ctx->n_state_max <<= 1);
  if (ctx->n_state)
    rsvg_state_clone (&ctx->state[ctx->n_state],
		      &ctx->state[ctx->n_state - 1]);
  else
    rsvg_state_init (ctx->state);
  ctx->n_state++;
  
  /* this should be the only thing starting inside of text */
  if (!strcmp ((char *)name, "tspan"))
    rsvg_start_tspan (ctx, atts);
}

static void
rsvg_text_handler_end (RsvgSaxHandler *self, const xmlChar *name)
{
  RsvgSaxHandlerText *z = (RsvgSaxHandlerText *)self;
  RsvgHandle *ctx = z->ctx;

  if (!strcmp ((char *)name, "tspan"))
    {
      /* advance the text offset */
      RsvgState *tspan = &ctx->state[ctx->n_state - 1];
      RsvgState *text  = &ctx->state[ctx->n_state - 2];
      text->text_offset += (tspan->text_offset - text->text_offset);
    }
  else if (!strcmp ((char *)name, "text"))
    {
      if (ctx->handler != NULL)
	{
	  ctx->handler->free (ctx->handler);
	  ctx->handler = z->parent;
	}
    } 

  /* pop the state stack */
  ctx->n_state--;
  rsvg_state_finalize (&ctx->state[ctx->n_state]);
}

static void
rsvg_start_text (RsvgHandle *ctx, const xmlChar **atts)
{
  int i;
  double affine[6] ;
  double x, y, dx, dy;
  const char * klazz = NULL;
  RsvgState *state;

  RsvgSaxHandlerText *handler = g_new0 (RsvgSaxHandlerText, 1);
  handler->super.free = rsvg_text_handler_free;
  handler->super.characters = rsvg_text_handler_characters;
  handler->super.start_element = rsvg_text_handler_start;
  handler->super.end_element   = rsvg_text_handler_end;
  handler->ctx = ctx;

  x = y = dx = dy = 0.;

  state = &ctx->state[ctx->n_state - 1];

  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  if (!strcmp ((char *)atts[i], "x"))
	    x = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "y"))
	    y = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "dx"))
	    dx = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "dy"))
	    dy = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "class"))
	    klazz = (const char *)atts[i + 1];
	}
    }

  x += dx ;
  y += dy ;
  
  art_affine_translate (affine, x, y);
  art_affine_multiply (state->affine, affine, state->affine);
  
  rsvg_parse_style_attrs (ctx, "text", klazz, atts);

  handler->parent = ctx->handler;
  ctx->handler = &handler->super;
}

/* end text */

typedef struct _RsvgSaxHandlerDefs {
  RsvgSaxHandler super;
  RsvgHandle *ctx;
} RsvgSaxHandlerDefs;

typedef struct _RsvgSaxHandlerStyle {
  RsvgSaxHandler super;
  RsvgSaxHandlerDefs *parent;
  RsvgHandle *ctx;
  GString *style;
} RsvgSaxHandlerStyle;

typedef struct _RsvgSaxHandlerGstops {
  RsvgSaxHandler super;
  RsvgSaxHandlerDefs *parent;
  RsvgHandle *ctx;
  RsvgGradientStops *stops;
  const char * parent_tag;
} RsvgSaxHandlerGstops;

static void
rsvg_gradient_stop_handler_free (RsvgSaxHandler *self)
{
  g_free (self);
}

static void
rsvg_gradient_stop_handler_start (RsvgSaxHandler *self, const xmlChar *name,
				  const xmlChar **atts)
{
  RsvgSaxHandlerGstops *z = (RsvgSaxHandlerGstops *)self;
  RsvgGradientStops *stops = z->stops;
  int i;
  double offset = 0;
  gboolean got_offset = FALSE;
  RsvgState state;
  int n_stop;

  if (strcmp ((char *)name, "stop"))
    {
      g_warning ("unexpected <%s> element in gradient\n", name);
      return;
    }

  rsvg_state_init (&state);

  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  if (!strcmp ((char *)atts[i], "offset"))
	    {
	      /* either a number [0,1] or a percentage */
	      offset = rsvg_css_parse_normalized_length ((char *)atts[i + 1], z->ctx->dpi, 1., 0., 0.);

	      if (offset < 0.)
		offset = 0.;
	      else if (offset > 1.)
		offset = 1.;

	      got_offset = TRUE;
	    }
	  else if (!strcmp ((char *)atts[i], "style"))
	    rsvg_parse_style (z->ctx, &state, (char *)atts[i + 1]);
	  else if (rsvg_is_style_arg ((char *)atts[i]))
	    rsvg_parse_style_pair (z->ctx, &state,
				   (char *)atts[i], (char *)atts[i + 1]);
	}
    }

  rsvg_state_finalize (&state);

  if (!got_offset)
    {
      g_warning ("gradient stop must specify offset\n");
      return;
    }

  n_stop = stops->n_stop++;
  if (n_stop == 0)
    stops->stop = g_new (RsvgGradientStop, 1);
  else if (!(n_stop & (n_stop - 1)))
    /* double the allocation if size is a power of two */
    stops->stop = g_renew (RsvgGradientStop, stops->stop, n_stop << 1);
  stops->stop[n_stop].offset = offset;
  stops->stop[n_stop].rgba = (state.stop_color << 8) | state.stop_opacity;
}

static void
rsvg_gradient_stop_handler_end (RsvgSaxHandler *self, const xmlChar *name)
{
  RsvgSaxHandlerGstops *z = (RsvgSaxHandlerGstops *)self;
  RsvgHandle *ctx = z->ctx;

  if (!strcmp((char *)name, z->parent_tag))
    {
      if (ctx->handler != NULL)
	{
	  ctx->handler->free (ctx->handler);
	  ctx->handler = &z->parent->super;
	}
    }
}

static RsvgSaxHandler *
rsvg_gradient_stop_handler_new_clone (RsvgHandle *ctx, RsvgGradientStops *stops, 
				      const char * parent)
{
  RsvgSaxHandlerGstops *gstops = g_new0 (RsvgSaxHandlerGstops, 1);

  gstops->super.free = rsvg_gradient_stop_handler_free;
  gstops->super.start_element = rsvg_gradient_stop_handler_start;
  gstops->super.end_element = rsvg_gradient_stop_handler_end;
  gstops->ctx = ctx;
  gstops->stops = stops;
  gstops->parent_tag = parent;

  gstops->parent = (RsvgSaxHandlerDefs*)ctx->handler;
  return &gstops->super;
}

static RsvgSaxHandler *
rsvg_gradient_stop_handler_new (RsvgHandle *ctx, RsvgGradientStops **p_stops,
				const char * parent)
{
  RsvgSaxHandlerGstops *gstops = g_new0 (RsvgSaxHandlerGstops, 1);
  RsvgGradientStops *stops = g_new (RsvgGradientStops, 1);

  gstops->super.free = rsvg_gradient_stop_handler_free;
  gstops->super.start_element = rsvg_gradient_stop_handler_start;
  gstops->super.end_element = rsvg_gradient_stop_handler_end;
  gstops->ctx = ctx;
  gstops->stops = stops;
  gstops->parent_tag = parent;

  stops->n_stop = 0;
  stops->stop = NULL;

  gstops->parent = (RsvgSaxHandlerDefs*)ctx->handler;
  *p_stops = stops;
  return &gstops->super;
}

static void
rsvg_linear_gradient_free (RsvgDefVal *self)
{
  RsvgLinearGradient *z = (RsvgLinearGradient *)self;

  g_free (z->stops->stop);
  g_free (z->stops);
  g_free (self);
}

static void
rsvg_start_linear_gradient (RsvgHandle *ctx, const xmlChar **atts)
{
  RsvgState *state = &ctx->state[ctx->n_state - 1];
  RsvgLinearGradient *grad = NULL;
  int i;
  char *id = NULL;
  double x1 = 0., y1 = 0., x2 = 0., y2 = 0.;
  ArtGradientSpread spread = ART_GRADIENT_PAD;
  const char * xlink_href = NULL;
  gboolean got_x1, got_x2, got_y1, got_y2, got_spread, cloned;

  got_x1 = got_x2 = got_y1 = got_y2 = got_spread = cloned = FALSE;

  /* 100% is the default */
  x2 = rsvg_css_parse_normalized_length ("100%", ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);

  /* todo: only handles numeric coordinates in gradientUnits = userSpace */
  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  if (!strcmp ((char *)atts[i], "id"))
	    id = (char *)atts[i + 1];
	  else if (!strcmp ((char *)atts[i], "x1"))
	    x1 = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "y1"))
	    y1 = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "x2"))
	    x2 = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "y2"))
	    y2 = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "spreadMethod"))
	    {
	      if (!strcmp ((char *)atts[i + 1], "pad"))
		spread = ART_GRADIENT_PAD;
	      else if (!strcmp ((char *)atts[i + 1], "reflect"))
		spread = ART_GRADIENT_REFLECT;
	      else if (!strcmp ((char *)atts[i + 1], "repeat"))
		spread = ART_GRADIENT_REPEAT;
	    }
	  else if (!strcmp ((char *)atts[i], "xlink:href"))
	    xlink_href = (const char *)atts[i + 1];
	}
    }
  
  if (xlink_href != NULL)
    {
      RsvgLinearGradient * parent = (RsvgLinearGradient*)rsvg_defs_lookup (ctx->defs, xlink_href+1);
      if (parent != NULL)
	{
	  cloned = TRUE;
	  grad = rsvg_clone_linear_gradient (parent); 
	  ctx->handler = rsvg_gradient_stop_handler_new_clone (ctx, grad->stops, "linearGradient");
	}
    }

  if (!cloned)
    {
      grad = g_new (RsvgLinearGradient, 1);
      grad->super.type = RSVG_DEF_LINGRAD;
      grad->super.free = rsvg_linear_gradient_free;
      ctx->handler = rsvg_gradient_stop_handler_new (ctx, &grad->stops, "linearGradient");
    }

  rsvg_defs_set (ctx->defs, id, &grad->super);

  for (i = 0; i < 6; i++)
    grad->affine[i] = state->affine[i];

  /* state inherits parent/cloned information unless it's explicity gotten */
  grad->x1 = (cloned && !got_x1) ? grad->x1 : x1;
  grad->y1 = (cloned && !got_y1) ? grad->y1 : y1;
  grad->x2 = (cloned && !got_x2) ? grad->x2 : x2;
  grad->y2 = (cloned && !got_y2) ? grad->y1 : y2;
  grad->spread = (cloned && !got_spread) ? grad->spread : spread;
}

static void
rsvg_radial_gradient_free (RsvgDefVal *self)
{
  RsvgRadialGradient *z = (RsvgRadialGradient *)self;

  g_free (z->stops->stop);
  g_free (z->stops);
  g_free (self);
}

static void
rsvg_start_radial_gradient (RsvgHandle *ctx, const xmlChar **atts)
{
  RsvgState *state = &ctx->state[ctx->n_state - 1];
  RsvgRadialGradient *grad = NULL;
  int i;
  char *id = NULL;
  double cx = 0., cy = 0., r = 0., fx = 0., fy = 0.;  
  const char * xlink_href = NULL;
  gboolean got_cx, got_cy, got_r, got_fx, got_fy, cloned;

  got_cx = got_cy = got_r = got_fx = got_fy = cloned = FALSE;

  /* setup defaults */
  cx = rsvg_css_parse_normalized_length ("50%", ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
  cy = rsvg_css_parse_normalized_length ("50%", ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
  r  = rsvg_css_parse_normalized_length ("50%", ctx->dpi, rsvg_viewport_percentage((gdouble)ctx->width, (gdouble)ctx->height), state->font_size, 0.);

  /* todo: only handles numeric coordinates in gradientUnits = userSpace */
  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  if (!strcmp ((char *)atts[i], "id"))
	    id = (char *)atts[i + 1];
	  else if (!strcmp ((char *)atts[i], "cx")) {
	    cx = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	    got_cx = TRUE;
	  }
	  else if (!strcmp ((char *)atts[i], "cy")) {
	    cy = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	    got_cy = TRUE;
	  }
	  else if (!strcmp ((char *)atts[i], "r")) {
	    r = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, 
						  rsvg_viewport_percentage((gdouble)ctx->width, (gdouble)ctx->height), 
						  state->font_size, 0.);
	    got_r = TRUE;
	  }
	  else if (!strcmp ((char *)atts[i], "fx")) {
	    fx = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	    got_fx = TRUE;
	  }
	  else if (!strcmp ((char *)atts[i], "fy")) {
	    fy = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	    got_fy = TRUE;
	  }
	  else if (!strcmp ((char *)atts[i], "xlink:href"))
	    xlink_href = (const char *)atts[i + 1];
	}
    }

  if (xlink_href != NULL)
    {
      RsvgRadialGradient * parent = (RsvgRadialGradient*)rsvg_defs_lookup (ctx->defs, xlink_href+1);
      if (parent != NULL)
	{
	  cloned = TRUE;
	  grad = rsvg_clone_radial_gradient (parent); 
	  ctx->handler = rsvg_gradient_stop_handler_new_clone (ctx, grad->stops, "radialGradient");
	}
    }
  if (!cloned)
    {
      grad = g_new (RsvgRadialGradient, 1);
      grad->super.type = RSVG_DEF_RADGRAD;
      grad->super.free = rsvg_radial_gradient_free;
      ctx->handler = rsvg_gradient_stop_handler_new (ctx, &grad->stops, "radialGradient");

      if (!got_fx)
	fx = cx;
      if (!got_fy)
	fy = cy;
    }

  rsvg_defs_set (ctx->defs, id, &grad->super);

  for (i = 0; i < 6; i++)
    grad->affine[i] = state->affine[i];

  /* state inherits parent/cloned information unless it's explicity gotten */
  grad->cx = (cloned && !got_cx) ? grad->cx: cx;
  grad->cy = (cloned && !got_cy) ? grad->cy: cy;
  grad->r =  (cloned && !got_r) ? grad->r : r;
  grad->fx = (cloned && !got_fx) ? grad->fx : fx;
  grad->fy = (cloned && !got_fy) ? grad->fy : fy;
}

/* end gradients */

static void
rsvg_style_handler_free (RsvgSaxHandler *self)
{
  RsvgSaxHandlerStyle *z = (RsvgSaxHandlerStyle *)self;
  RsvgHandle *ctx = z->ctx;

  rsvg_parse_cssbuffer (ctx, z->style->str, z->style->len);

  g_string_free (z->style, TRUE);
  g_free (z);
}

static void
rsvg_style_handler_characters (RsvgSaxHandler *self, const xmlChar *ch, int len)
{
  RsvgSaxHandlerStyle *z = (RsvgSaxHandlerStyle *)self;
  g_string_append_len (z->style, (const char *)ch, len);
}

static void
rsvg_style_handler_start (RsvgSaxHandler *self, const xmlChar *name,
			  const xmlChar **atts)
{
}

static void
rsvg_style_handler_end (RsvgSaxHandler *self, const xmlChar *name)
{
  RsvgSaxHandlerStyle *z = (RsvgSaxHandlerStyle *)self;
  RsvgHandle *ctx = z->ctx;
  
  if (!strcmp ((char *)name, "style"))
  {
    if (ctx->handler != NULL)
      {
	ctx->handler->free (ctx->handler);
	ctx->handler = &z->parent->super;
      }
  }
}

static void
rsvg_start_style (RsvgHandle *ctx, const xmlChar **atts)
{
  RsvgSaxHandlerStyle *handler = g_new0 (RsvgSaxHandlerStyle, 1);

  handler->super.free = rsvg_style_handler_free;
  handler->super.characters = rsvg_style_handler_characters;
  handler->super.start_element = rsvg_style_handler_start;
  handler->super.end_element   = rsvg_style_handler_end;
  handler->ctx = ctx;

  handler->style = g_string_new (NULL);

  handler->parent = (RsvgSaxHandlerDefs*)ctx->handler;
  ctx->handler = &handler->super;
}

/* */

static void
rsvg_defs_handler_free (RsvgSaxHandler *self)
{
  g_free (self);
}

static void
rsvg_defs_handler_characters (RsvgSaxHandler *self, const xmlChar *ch, int len)
{
}

static void
rsvg_defs_handler_start (RsvgSaxHandler *self, const xmlChar *name,
			 const xmlChar **atts)
{
  RsvgSaxHandlerDefs *z = (RsvgSaxHandlerDefs *)self;
  RsvgHandle *ctx = z->ctx;

  /* push the state stack */
  if (ctx->n_state == ctx->n_state_max)
    ctx->state = g_renew (RsvgState, ctx->state, ctx->n_state_max <<= 1);
  if (ctx->n_state)
    rsvg_state_clone (&ctx->state[ctx->n_state],
		      &ctx->state[ctx->n_state - 1]);
  else
    rsvg_state_init (ctx->state);
  ctx->n_state++;
  
  if (!strcmp ((char *)name, "linearGradient"))
    rsvg_start_linear_gradient (ctx, atts);
  else if (!strcmp ((char *)name, "radialGradient"))
    rsvg_start_radial_gradient (ctx, atts);
  else if (!strcmp ((char *)name, "style"))
    rsvg_start_style (ctx, atts);
}

static void
rsvg_defs_handler_end (RsvgSaxHandler *self, const xmlChar *name)
{
  RsvgSaxHandlerDefs *z = (RsvgSaxHandlerDefs *)self;
  RsvgHandle *ctx = z->ctx;

  if (!strcmp((char *)name, "defs"))
    {
      if (ctx->handler != NULL)
	{
	  ctx->handler->free (ctx->handler);
	  ctx->handler = NULL;
	}
    }

  /* pop the state stack */
  ctx->n_state--;
  rsvg_state_finalize (&ctx->state[ctx->n_state]);
}

static void
rsvg_start_defs (RsvgHandle *ctx, const xmlChar **atts)
{
  RsvgSaxHandlerDefs *handler = g_new0 (RsvgSaxHandlerDefs, 1);

  handler->super.free = rsvg_defs_handler_free;
  handler->super.characters = rsvg_defs_handler_characters;
  handler->super.start_element = rsvg_defs_handler_start;
  handler->super.end_element   = rsvg_defs_handler_end;
  handler->ctx = ctx;

  ctx->handler = &handler->super;
}

/* end defs */

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
rsvg_start_any_poly(RsvgHandle *ctx, const xmlChar **atts, gboolean is_polyline)
{
  /* the only difference i'm making between polygon and polyline is
     that a polyline closes the path */

  int i;
  const char * verts = (const char *)NULL;
  GString * g = NULL;
  gchar ** pointlist = NULL;
  const char * klazz = NULL;

  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  /* support for svg < 1.0 which used verts */
	  if (!strcmp ((char *)atts[i], "verts") || !strcmp ((char *)atts[i], "points"))
	    verts = (const char *)atts[i + 1];
	  else if (!strcmp ((char *)atts[i], "class"))
	    klazz = (const char *)atts[i + 1];
	}
    }

  if (!verts)
    return;

  rsvg_parse_style_attrs (ctx, (is_polyline ? "polyline" : "polygon"), klazz, atts);

  /* todo: make the following more memory and CPU friendly */
  g = rsvg_make_poly_point_list (verts);
  pointlist = g_strsplit (g->str, " ", -1);
  g_string_free (g, TRUE);

  /* represent as a "moveto, lineto*, close" path */  
  if (pointlist)
    {
      GString * d = g_string_sized_new (strlen(verts));
      g_string_append_printf (d, "M %s %s ", pointlist[0], pointlist[1] );

      for (i = 2; pointlist[i] != NULL && pointlist[i][0] != '\0'; i += 2)
	  g_string_append_printf (d, "L %s %s ", pointlist[i], pointlist[i+1]);

      if (!is_polyline)
	g_string_append (d, "Z");

      rsvg_render_path (ctx, d->str);
      g_string_free (d, TRUE);
      g_strfreev(pointlist);
    }
}

static void
rsvg_start_polygon (RsvgHandle *ctx, const xmlChar **atts)
{
  rsvg_start_any_poly (ctx, atts, FALSE);
}

static void
rsvg_start_polyline (RsvgHandle *ctx, const xmlChar **atts)
{
  rsvg_start_any_poly (ctx, atts, TRUE);
}

/* TODO 1: issue with affining alpha images - this is gdkpixbuf's fault...
 * TODO 2: issue with rotating images - do we want to rotate the whole
 *         canvas 2x to get this right, only to have #1 bite us?
 */
static void
rsvg_start_image (RsvgHandle *ctx, const xmlChar **atts)
{
  int i;
  double x = 0., y = 0., w = -1., h = -1.;
  const char * href = NULL;
  const char * klazz = NULL;

  GdkPixbuf *img;
  GError *err = NULL;

  gboolean has_alpha;
  guchar *rgb = NULL;
  int dest_rowstride;
  double tmp_affine[6];
  RsvgState *state = &ctx->state[ctx->n_state - 1];

  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  if (!strcmp ((char *)atts[i], "x"))
	    x = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "y"))
	    y = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "width"))
	    w = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "height"))
	    h = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  /* path is used by some older adobe illustrator versions */
	  else if (!strcmp ((char *)atts[i], "path") || !strcmp((char *)atts[i], "xlink:href"))
	    href = (const char *)atts[i + 1];
	  else if (!strcmp ((char *)atts[i], "class"))
	    klazz = (const char *)atts[i + 1];
	}
    }

  if (!href || x < 0. || y < 0. || w <= 0. || h <= 0.)
    return;
  
  rsvg_parse_style_attrs (ctx, "image", klazz, atts);

  img = gdk_pixbuf_new_from_file (href, &err);
  
  if (!img)
    {
      if (err)
	{
	  g_warning ("Couldn't load pixbuf (%s): %s\n", href, err->message);
	  g_error_free (err);
	}
      return;
    }

  /* scale/resize the dest image */
  art_affine_scale (tmp_affine, (double)w / (double)gdk_pixbuf_get_width (img),
		    (double)h / (double)gdk_pixbuf_get_height (img));
  art_affine_multiply (state->affine, tmp_affine, state->affine);

  has_alpha = gdk_pixbuf_get_has_alpha (img);
  dest_rowstride = (int)(w * (has_alpha ? 4 : 3) + 3) & ~3;
  rgb = g_new (guchar, h * dest_rowstride);

  if(has_alpha)
    art_rgb_rgba_affine (rgb, 0, 0, w, h, dest_rowstride,
			 gdk_pixbuf_get_pixels (img),
			 gdk_pixbuf_get_width (img),
			 gdk_pixbuf_get_height (img),
			 gdk_pixbuf_get_rowstride (img),
			 state->affine,
			 ART_FILTER_NEAREST,
			 NULL);
  else
    art_rgb_affine (rgb, 0, 0, w, h, dest_rowstride,
		    gdk_pixbuf_get_pixels (img),
		    gdk_pixbuf_get_width (img),
		    gdk_pixbuf_get_height (img),
		    gdk_pixbuf_get_rowstride (img),
		    state->affine,
		    ART_FILTER_NEAREST,
		    NULL);

  g_object_unref (G_OBJECT (img));
  img = gdk_pixbuf_new_from_data (rgb, GDK_COLORSPACE_RGB, has_alpha, 8, w, h, dest_rowstride, NULL, NULL);

  if (!img)
    {
      g_free (rgb);
      return;
    }

  gdk_pixbuf_copy_area (img, 0, 0,
			gdk_pixbuf_get_width (img) * state->affine[0],
			gdk_pixbuf_get_height (img) * state->affine[3],
			ctx->pixbuf, 
			state->affine[4] + x,
			state->affine[5] + y);
  
  g_object_unref (G_OBJECT (img));
  g_free (rgb);
}

static void
rsvg_start_line (RsvgHandle *ctx, const xmlChar **atts)
{
  int i;
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  char * d = NULL;
  const char * klazz = NULL;
  RsvgState *state = &ctx->state[ctx->n_state - 1];

  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  if (!strcmp ((char *)atts[i], "x1"))
	    x1 = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "y1"))
	    y1 = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  if (!strcmp ((char *)atts[i], "x2"))
	    x2 = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "y2"))
	    y2 = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "class"))
	    klazz = (const char *)atts[i + 1];
	}      
    }
  rsvg_parse_style_attrs (ctx, "line", klazz, atts);

  /* emulate a line using a path */
  d = g_strdup_printf ("M %f %f L %f %f", x1, y1, x2, y2);

  rsvg_render_path (ctx, d);
  g_free (d);
}

static void
rsvg_start_rect (RsvgHandle *ctx, const xmlChar **atts)
{
  int i;
  double x = -1, y = -1, w = -1, h = -1, rx = 0, ry = 0;
  char * d = NULL;
  const char * klazz = NULL;
  RsvgState *state = &ctx->state[ctx->n_state - 1];
  
  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  if (!strcmp ((char *)atts[i], "x"))
	    x = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "y"))
	    y = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "width"))
	    w = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "height"))
	    h = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "rx"))
	    rx = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "ry"))
	    ry = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "class"))
	    klazz = (const char *)atts[i + 1];
	}
    }

  if (x < 0. || y < 0. || w < 0. || h < 0. || rx < 0. || ry < 0.)
    return;

  rsvg_parse_style_attrs (ctx, "rect", klazz, atts);

  /* incrementing y by 1 properly draws borders. this is a HACK */
  y++;

  /* emulate a rect using a path */
  d = g_strdup_printf ("M %f %f "
		       "H %f "
		       "A %f,%f %f,%f %f %f,%f "
		       "V %f "
		       "A %f,%f %f,%f %f %f,%f "
		       "H %f "
		       "A %f,%f %f,%f %f %f,%f "
		       "V %f "
		       "A %f,%f %f,%f %f %f,%f",
		       x + rx, y,
		       x + w - rx,
		       rx, ry, 0., 0., 1., x + w, y + ry,
		       y + h - ry,
		       rx, ry, 0., 0., 1., x + w - rx, y + h,
		       x + rx,
		       rx, ry, 0., 0., 1., x, y + h - ry,
		       y + ry,
		       rx, ry, 0., 0., 1., x + rx, y);

  rsvg_render_path (ctx, d);
  g_free (d);
}

static void
rsvg_start_circle (RsvgHandle *ctx, const xmlChar **atts)
{
  int i;
  double cx = 0, cy = 0, r = 0;
  char * d = NULL;
  const char * klazz = NULL;
  RsvgState *state = &ctx->state[ctx->n_state - 1];
  
  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  if (!strcmp ((char *)atts[i], "cx"))
	    cx = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "cy"))
	    cy = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "r"))
	    r = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, 
						  rsvg_viewport_percentage((gdouble)ctx->width, (gdouble)ctx->height), 
						  state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "class"))
	    klazz = (const char *)atts[i + 1];
	}
    }

  if (cx < 0. || cy < 0. || r <= 0.)
    return;

  rsvg_parse_style_attrs (ctx, "circle", klazz, atts);

  /* approximate a circle using 4 bezier curves */
  d = g_strdup_printf ("M %f %f "
		       "C %f %f %f %f %f %f "
		       "C %f %f %f %f %f %f "
		       "C %f %f %f %f %f %f "
		       "C %f %f %f %f %f %f "
		       "Z",
		       cx + r, cy,
		       cx + r, cy + r * RSVG_ARC_MAGIC, cx + r * RSVG_ARC_MAGIC, cy + r, cx, cy + r,
		       cx - r * RSVG_ARC_MAGIC, cy + r, cx - r, cy + r * RSVG_ARC_MAGIC, cx - r, cy,
		       cx - r, cy - r * RSVG_ARC_MAGIC, cx - r * RSVG_ARC_MAGIC, cy - r, cx, cy - r,
		       cx + r * RSVG_ARC_MAGIC, cy - r, cx + r, cy - r * RSVG_ARC_MAGIC, cx + r, cy
		       );

  rsvg_render_path (ctx, d);
  g_free (d);
}

static void
rsvg_start_ellipse (RsvgHandle *ctx, const xmlChar **atts)
{
  int i;
  double cx = 0, cy = 0, rx = 0, ry = 0;
  char * d = NULL;
  const char * klazz = NULL;
  RsvgState *state = &ctx->state[ctx->n_state - 1];

  if (atts != NULL)
    {
      for (i = 0; atts[i] != NULL; i += 2)
	{
	  if (!strcmp ((char *)atts[i], "cx"))
	    cx = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "cy"))
	    cy = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "rx"))
	    rx = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->width, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "ry"))
	    ry = rsvg_css_parse_normalized_length ((char *)atts[i + 1], ctx->dpi, (gdouble)ctx->height, state->font_size, 0.);
	  else if (!strcmp ((char *)atts[i], "class"))
	    klazz = (const char *)atts[i + 1];
	}
    }

  if (cx < 0. || cy < 0. || rx <= 0. || ry <= 0.)
    return;

  rsvg_parse_style_attrs (ctx, "ellipse", klazz, atts);

  /* approximate an ellipse using 4 bezier curves */
  d = g_strdup_printf ("M %f %f "
		       "C %f %f %f %f %f %f "
		       "C %f %f %f %f %f %f "
		       "C %f %f %f %f %f %f "
		       "C %f %f %f %f %f %f "
		       "Z",
		       cx + rx, cy,
		       cx + rx, cy - RSVG_ARC_MAGIC * ry, cx + RSVG_ARC_MAGIC * rx, cy - ry, cx, cy - ry,
		       cx - RSVG_ARC_MAGIC * rx, cy - ry, cx - rx, cy - RSVG_ARC_MAGIC * ry, cx - rx, cy,
		       cx - rx, cy + RSVG_ARC_MAGIC * ry, cx - RSVG_ARC_MAGIC * rx, cy + ry, cx, cy + ry,
		       cx + RSVG_ARC_MAGIC * rx, cy + ry, cx + rx, cy + RSVG_ARC_MAGIC * ry, cx + rx, cy
		       );

  rsvg_render_path (ctx, d);
  g_free (d);

  return;
}

static void
rsvg_start_element (void *data, const xmlChar *name, const xmlChar **atts)
{
  RsvgHandle *ctx = (RsvgHandle *)data;

  if (ctx->handler)
    {
      ctx->handler_nest++;
      if (ctx->handler->start_element != NULL)
	ctx->handler->start_element (ctx->handler, name, atts);
    }
  else
    {
      /* push the state stack */
      if (ctx->n_state == ctx->n_state_max)
	ctx->state = g_renew (RsvgState, ctx->state, ctx->n_state_max <<= 1);
      if (ctx->n_state)
	rsvg_state_clone (&ctx->state[ctx->n_state],
			  &ctx->state[ctx->n_state - 1]);
      else
	rsvg_state_init (ctx->state);
      ctx->n_state++;

      if (!strcmp ((char *)name, "svg"))
	rsvg_start_svg (ctx, atts);
      else if (!strcmp ((char *)name, "g"))
	rsvg_start_g (ctx, atts);
      else if (!strcmp ((char *)name, "path"))
	rsvg_start_path (ctx, atts);
      else if (!strcmp ((char *)name, "text"))
	rsvg_start_text (ctx, atts);
      else if (!strcmp ((char *)name, "image"))
	rsvg_start_image (ctx, atts);
      else if (!strcmp ((char *)name, "line"))
	rsvg_start_line (ctx, atts);
      else if (!strcmp ((char *)name, "rect"))
	rsvg_start_rect (ctx, atts);
      else if (!strcmp ((char *)name, "circle"))
	rsvg_start_circle (ctx, atts);
      else if (!strcmp ((char *)name, "ellipse"))
	rsvg_start_ellipse (ctx, atts);
      else if (!strcmp ((char *)name, "defs"))
	rsvg_start_defs (ctx, atts);
      else if (!strcmp ((char *)name, "polygon"))
	rsvg_start_polygon (ctx, atts);
      else if (!strcmp ((char *)name, "polyline"))
	rsvg_start_polyline (ctx, atts);

      /* */
      else if (!strcmp ((char *)name, "linearGradient"))
	rsvg_start_linear_gradient (ctx, atts);
      else if (!strcmp ((char *)name, "radialGradient"))
	rsvg_start_radial_gradient (ctx, atts);
    }
}

static void
rsvg_end_element (void *data, const xmlChar *name)
{
  RsvgHandle *ctx = (RsvgHandle *)data;

  if (ctx->handler_nest > 0)
    {
      if (ctx->handler->end_element != NULL)
	ctx->handler->end_element (ctx->handler, name);
      ctx->handler_nest--;
    }
  else
    {
      if (ctx->handler != NULL)
	{
	  ctx->handler->free (ctx->handler);
	  ctx->handler = NULL;
	}

      if (!strcmp ((char *)name, "g"))
	rsvg_end_g (ctx);

      /* pop the state stack */
      ctx->n_state--;
      rsvg_state_finalize (&ctx->state[ctx->n_state]);
    }
}

static void
rsvg_characters (void *data, const xmlChar *ch, int len)
{
  RsvgHandle *ctx = (RsvgHandle *)data;

  if (ctx->handler && ctx->handler->characters != NULL)
    ctx->handler->characters (ctx->handler, ch, len);
}

static xmlEntityPtr
rsvg_get_entity (void *data, const xmlChar *name)
{
  RsvgHandle *ctx = (RsvgHandle *)data;

  return (xmlEntityPtr)g_hash_table_lookup (ctx->entities, name);
}

static void
rsvg_entity_decl (void *data, const xmlChar *name, int type,
		  const xmlChar *publicId, const xmlChar *systemId, xmlChar *content)
{
  RsvgHandle *ctx = (RsvgHandle *)data;
  GHashTable *entities = ctx->entities;
  xmlEntityPtr entity;
  char *dupname;

  entity = g_new0 (xmlEntity, 1);
  entity->type = type;
  entity->length = strlen (name);
  dupname = g_strdup (name);
  entity->name = dupname;
  entity->ExternalID = g_strdup (publicId);
  entity->SystemID = g_strdup (systemId);
  if (content)
    {
      entity->content = xmlMemStrdup (content);
      entity->length = strlen (content);
    }
  g_hash_table_insert (entities, dupname, entity);
}

static void
rsvg_error_cb (void *data, const char *msg, ...)
{
	va_list args;
	
	va_start (args, msg);
	vfprintf (stderr, msg, args);
	va_end (args);
}

static xmlSAXHandler rsvgSAXHandlerStruct = {
    NULL, /* internalSubset */
    NULL, /* isStandalone */
    NULL, /* hasInternalSubset */
    NULL, /* hasExternalSubset */
    NULL, /* resolveEntity */
    rsvg_get_entity, /* getEntity */
    rsvg_entity_decl, /* entityDecl */
    NULL, /* notationDecl */
    NULL, /* attributeDecl */
    NULL, /* elementDecl */
    NULL, /* unparsedEntityDecl */
    NULL, /* setDocumentLocator */
    NULL, /* startDocument */
    NULL, /* endDocument */
    rsvg_start_element, /* startElement */
    rsvg_end_element, /* endElement */
    NULL, /* reference */
    rsvg_characters, /* characters */
    NULL, /* ignorableWhitespace */
    NULL, /* processingInstruction */
    NULL, /* comment */
    NULL, /* xmlParserWarning */
    rsvg_error_cb, /* xmlParserError */
    rsvg_error_cb, /* xmlParserFatalError */
    NULL, /* getParameterEntity */
    rsvg_characters, /* cdataCallback */
    NULL /* */
};

GQuark
rsvg_error_quark (void)
{
  static GQuark q = 0;
  if (q == 0)
    q = g_quark_from_static_string ("rsvg-error-quark");

  return q;
}

/**
 * rsvg_handle_new:
 * @void:
 *
 * Returns a new rsvg handle.  Must be freed with @rsvg_handle_free.  This
 * handle can be used for dynamically loading an image.  You need to feed it
 * data using @rsvg_handle_write, then call @rsvg_handle_close when done.  No
 * more than one image can be loaded with one handle.
 *
 * Return value: A new #RsvgHandle
 **/
RsvgHandle *
rsvg_handle_new (void)
{
  RsvgHandle *handle;

  handle = g_new0 (RsvgHandle, 1);
  handle->n_state = 0;
  handle->n_state_max = 16;
  handle->state = g_new (RsvgState, handle->n_state_max);
  handle->defs = rsvg_defs_new ();
  handle->handler_nest = 0;
  handle->entities = g_hash_table_new (g_str_hash, g_str_equal);
  handle->dpi = internal_dpi;

  handle->css_props = g_hash_table_new_full (g_str_hash, g_str_equal,
					     g_free, g_free);

  handle->ctxt = NULL;

  return handle;
}

/**
 * rsvg_set_default_dpi
 * @dpi: Dots Per Inch (aka Pixels Per Inch)
 *
 * Sets the DPI for the all future outgoing pixbufs. Common values are
 * 72, 90, and 300 DPI. Passing a number <= 0 to #dpi will 
 * reset the DPI to whatever the default value happens to be.
 */
void
rsvg_set_default_dpi (double dpi)
{
  if (dpi <= 0.)
    internal_dpi = RSVG_DEFAULT_DPI;
  else
    internal_dpi = dpi;
}

/**
 * rsvg_handle_set_dpi
 * @handle: An #RsvgHandle
 * @dpi: Dots Per Inch (aka Pixels Per Inch)
 *
 * Sets the DPI for the outgoing pixbuf. Common values are
 * 72, 90, and 300 DPI. Passing a number <= 0 to #dpi will 
 * reset the DPI to whatever the default value happens to be.
 */
void
rsvg_handle_set_dpi (RsvgHandle * handle, double dpi)
{
  g_return_if_fail (handle != NULL);

    if (dpi <= 0.)
        handle->dpi = internal_dpi;
    else
        handle->dpi = dpi;
}

/**
 * rsvg_handle_set_size_callback:
 * @handle: An #RsvgHandle
 * @size_func: A sizing function, or %NULL
 * @user_data: User data to pass to @size_func, or %NULL
 * @user_data_destroy: Destroy function for @user_data, or %NULL
 *
 * Sets the sizing function for the @handle.  This function is called right
 * after the size of the image has been loaded.  The size of the image is passed
 * in to the function, which may then modify these values to set the real size
 * of the generated pixbuf.  If the image has no associated size, then the size
 * arguments are set to -1.
 **/
void
rsvg_handle_set_size_callback (RsvgHandle     *handle,
			       RsvgSizeFunc    size_func,
			       gpointer        user_data,
			       GDestroyNotify  user_data_destroy)
{
  g_return_if_fail (handle != NULL);

  if (handle->user_data_destroy)
    (* handle->user_data_destroy) (handle->user_data);

  handle->size_func = size_func;
  handle->user_data = user_data;
  handle->user_data_destroy = user_data_destroy;
}

/**
 * rsvg_handle_write:
 * @handle: An #RsvgHandle
 * @buf: Pointer to svg data
 * @count: length of the @buf buffer in bytes
 * @error: return location for errors
 *
 * Loads the next @count bytes of the image.  This will return #TRUE if the data
 * was loaded successful, and #FALSE if an error occurred.  In the latter case,
 * the loader will be closed, and will not accept further writes. If FALSE is
 * returned, @error will be set to an error from the #RSVG_ERROR domain.
 *
 * Return value: #TRUE if the write was successful, or #FALSE if there was an
 * error.
 **/
gboolean
rsvg_handle_write (RsvgHandle    *handle,
		   const guchar  *buf,
		   gsize          count,
		   GError       **error)
{
  GError *real_error;
  g_return_val_if_fail (handle != NULL, FALSE);

  handle->error = &real_error;
  if (handle->ctxt == NULL)
    {
      handle->ctxt = xmlCreatePushParserCtxt (
	      &rsvgSAXHandlerStruct, handle, NULL, 0, NULL);
      handle->ctxt->replaceEntities = TRUE;
    }

  xmlParseChunk (handle->ctxt, buf, count, 0);

  handle->error = NULL;
  /* FIXME: Error handling not implemented. */
  /*  if (*real_error != NULL)
    {
      g_propagate_error (error, real_error);
      return FALSE;
      }*/
  return TRUE;
}

/**
 * rsvg_handle_close:
 * @handle: An #RsvgHandle
 *
 * Closes @handle, to indicate that loading the image is complete.  This will
 * return #TRUE if the loader closed successfully.  Note that @handle isn't
 * freed until @rsvg_handle_free is called.
 *
 * Return value: #TRUE if the loader closed successfully, or #FALSE if there was
 * an error.
 **/
gboolean
rsvg_handle_close (RsvgHandle  *handle,
		   GError     **error)
{
  gchar chars[1] = { '\0' };
  GError *real_error;

  handle->error = &real_error;

  if (handle->ctxt != NULL)
  {
    xmlParseChunk (handle->ctxt, chars, 1, TRUE);
    xmlFreeParserCtxt (handle->ctxt);
  }
  
  /* FIXME: Error handling not implemented. */
  /*
  if (real_error != NULL)
    {
      g_propagate_error (error, real_error);
      return FALSE;
      }*/
  return TRUE;
}

/**
 * rsvg_handle_get_pixbuf:
 * @handle: An #RsvgHandle
 *
 * Returns the pixbuf loaded by #handle.  The pixbuf returned will be reffed, so
 * the caller of this function must assume that ref.  If insufficient data has
 * been read to create the pixbuf, or an error occurred in loading, then %NULL
 * will be returned.  Note that the pixbuf may not be complete until
 * @rsvg_handle_close has been called.
 *
 * Return value: the pixbuf loaded by #handle, or %NULL.
 **/
GdkPixbuf *
rsvg_handle_get_pixbuf (RsvgHandle *handle)
{
  g_return_val_if_fail (handle != NULL, NULL);

  if (handle->pixbuf)
    return g_object_ref (handle->pixbuf);

  return NULL;
}

/**
 * rsvg_handle_free:
 * @handle: An #RsvgHandle
 *
 * Frees #handle.
 **/
void
rsvg_handle_free (RsvgHandle *handle)
{
  int i;

  if (handle->pango_context != NULL)
    g_object_unref (handle->pango_context);
  rsvg_defs_free (handle->defs);

  for (i = 0; i < handle->n_state; i++)
    rsvg_state_finalize (&handle->state[i]);
  g_free (handle->state);

  g_hash_table_foreach (handle->entities, rsvg_ctx_free_helper, NULL);
  g_hash_table_destroy (handle->entities);

  g_hash_table_destroy (handle->css_props);

  if (handle->user_data_destroy)
    (* handle->user_data_destroy) (handle->user_data);
  if (handle->pixbuf)
    g_object_unref (handle->pixbuf);
  g_free (handle);
}

typedef enum {
  RSVG_SIZE_ZOOM,
  RSVG_SIZE_WH,
  RSVG_SIZE_WH_MAX,
  RSVG_SIZE_ZOOM_MAX
} RsvgSizeType;

struct RsvgSizeCallbackData
{
  RsvgSizeType type;
  double x_zoom;
  double y_zoom;
  gint width;
  gint height;
};

static void
rsvg_size_callback (int *width,
		    int *height,
		    gpointer  data)
{
  struct RsvgSizeCallbackData *real_data = (struct RsvgSizeCallbackData *) data;
  double zoomx, zoomy, zoom;

  switch (real_data->type) {
  case RSVG_SIZE_ZOOM:
    if (*width < 0 || *height < 0)
      return;

    *width = floor (real_data->x_zoom * *width + 0.5);
    *height = floor (real_data->y_zoom * *height + 0.5);
    return;

  case RSVG_SIZE_ZOOM_MAX:
    if (*width < 0 || *height < 0)
      return;

    *width = floor (real_data->x_zoom * *width + 0.5);
    *height = floor (real_data->y_zoom * *height + 0.5);
    
    if (*width > real_data->width || *height > real_data->height)
      {
	zoomx = (double) real_data->width / *width;
	zoomy = (double) real_data->height / *height;
	zoom = MIN (zoomx, zoomy);
	
	*width = floor (zoom * *width + 0.5);
	*height = floor (zoom * *height + 0.5);
      }
    return;

  case RSVG_SIZE_WH_MAX:
    if (*width < 0 || *height < 0)
      return;

    zoomx = (double) real_data->width / *width;
    zoomy = (double) real_data->height / *height;
    zoom = MIN (zoomx, zoomy);
    
    *width = floor (zoom * *width + 0.5);
    *height = floor (zoom * *height + 0.5);
    return;

  case RSVG_SIZE_WH:

    if (real_data->width != -1)
      *width = real_data->width;
    if (real_data->height != -1)
      *height = real_data->height;
    return;
  }

  g_assert_not_reached ();
}

static GdkPixbuf *
rsvg_pixbuf_from_file_with_size_data (const gchar * file_name,
				      struct RsvgSizeCallbackData * data,
				      GError ** error)
{
  char chars[SVG_BUFFER_SIZE];
  gint result;
  GdkPixbuf *retval;
  RsvgHandle *handle;

#if ENABLE_GNOME_VFS
  GnomeVFSHandle * f = NULL;
  if (GNOME_VFS_OK != gnome_vfs_open (&handle, file_name, GNOME_VFS_OPEN_READ))
    {
      /* FIXME: Set up error. */
      return NULL;
    }
#else
  FILE *f = fopen (file_name, "r");
  if (!f)
    {
      /* FIXME: Set up error. */
      return NULL;
    }
#endif

  handle = rsvg_handle_new ();

  rsvg_handle_set_size_callback (handle, rsvg_size_callback, data, NULL);

#if ENABLE_GNOME_VFS
  while (GNOME_VFS_OK == gnome_vfs_read (f,chars, SVG_BUFFER_SIZE, &result))
    rsvg_handle_write (handle, chars, result, error);
#else
  while ((result = fread (chars, 1, SVG_BUFFER_SIZE, f)) > 0)
    rsvg_handle_write (handle, chars, result, error);
#endif

  rsvg_handle_close (handle, error);
  retval = rsvg_handle_get_pixbuf (handle);

#if ENABLE_GNOME_VFS
  gnome_vfs_close (f);
#else
  fclose (f);
#endif

  rsvg_handle_free (handle);

  return retval;
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
 **/
GdkPixbuf *
rsvg_pixbuf_from_file (const gchar *file_name,
		       GError     **error)
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
 **/
GdkPixbuf *
rsvg_pixbuf_from_file_at_zoom (const gchar *file_name,
			       double       x_zoom,
			       double       y_zoom,
			       GError     **error)
{
  struct RsvgSizeCallbackData data;

  g_return_val_if_fail (file_name != NULL, NULL);
  g_return_val_if_fail (x_zoom > 0.0 && y_zoom > 0.0, NULL);

  data.type = RSVG_SIZE_ZOOM;
  data.x_zoom = x_zoom;
  data.y_zoom = y_zoom;

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
 **/
GdkPixbuf  *
rsvg_pixbuf_from_file_at_zoom_with_max (const gchar  *file_name,
					double        x_zoom,
					double        y_zoom,
					gint          max_width,
					gint          max_height,
					GError      **error)
{
  struct RsvgSizeCallbackData data;

  g_return_val_if_fail (file_name != NULL, NULL);
  g_return_val_if_fail (x_zoom > 0.0 && y_zoom > 0.0, NULL);

  data.type = RSVG_SIZE_ZOOM_MAX;
  data.x_zoom = x_zoom;
  data.y_zoom = y_zoom;
  data.width = max_width;
  data.height = max_height;

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
 **/
GdkPixbuf *
rsvg_pixbuf_from_file_at_size (const gchar *file_name,
			       gint         width,
			       gint         height,
			       GError     **error)
{
  struct RsvgSizeCallbackData data;

  data.type = RSVG_SIZE_WH;
  data.width = width;
  data.height = height;

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
 **/
GdkPixbuf  *
rsvg_pixbuf_from_file_at_max_size (const gchar     *file_name,
				   gint             max_width,
				   gint             max_height,
				   GError         **error)
{
  struct RsvgSizeCallbackData data;

  data.type = RSVG_SIZE_WH_MAX;
  data.width = max_width;
  data.height = max_height;

  return rsvg_pixbuf_from_file_with_size_data (file_name, &data, error);
}
