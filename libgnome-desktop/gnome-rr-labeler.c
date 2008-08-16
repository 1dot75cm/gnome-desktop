/* gnome-rr-labeler.c - Utility to label monitors to identify them
 * while they are being configured.
 *
 * Copyright 2008, Novell, Inc.
 *
 * This file is part of the Gnome Library.
 *
 * The Gnome Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * The Gnome Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with the Gnome Library; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Federico Mena-Quintero <federico@novell.com>
 */

#define GNOME_DESKTOP_USE_UNSTABLE_API

#include <config.h>
#include "libgnomeui/gnome-rr-labeler.h"
#include <gtk/gtk.h>

struct _GnomeRRLabeler {
	GObject parent;

	GnomeRRConfig *config;

	int num_outputs;

	GdkColor *palette;
	GtkWidget **windows;
};

struct _GnomeRRLabelerClass {
	GObjectClass parent_class;
};

G_DEFINE_TYPE (GnomeRRLabeler, gnome_rr_labeler, G_TYPE_OBJECT);

static void gnome_rr_labeler_finalize (GObject *object);

static void
gnome_rr_labeler_init (GnomeRRLabeler *labeler)
{
	/* nothing */
}

static void
gnome_rr_labeler_class_init (GnomeRRLabelerClass *class)
{
	GObjectClass *object_class;

	object_class = (GObjectClass *) class;

	object_class->finalize = gnome_rr_labeler_finalize;
}

static void
gnome_rr_labeler_finalize (GObject *object)
{
	GnomeRRLabeler *labeler;

	labeler = GNOME_RR_LABELER (object);

	/* We don't destroy the labeler->config (a GnomeRRConfig*) here; let our
	 * caller do that instead.
	 */

	if (labeler->windows != NULL) {
		gnome_rr_labeler_hide (labeler);
		g_free (labeler->windows);
		labeler->windows = NULL;
	}

	g_free (labeler->palette);
	labeler->palette = NULL;

	G_OBJECT_CLASS (gnome_rr_labeler_parent_class)->finalize (object);
}

static int
count_outputs (GnomeRRConfig *config)
{
	int i;

	for (i = 0; config->outputs[i] != NULL; i++)
		;

	return i;
}

/* hsv_to_rgb() stolen from gtk+/gtk/gtkhsv.c, sigh. */

#define INTENSITY(r, g, b) ((r) * 0.30 + (g) * 0.59 + (b) * 0.11)

/* Converts from HSV to RGB */
static void
hsv_to_rgb (gdouble *h,
	    gdouble *s,
	    gdouble *v)
{
  gdouble hue, saturation, value;
  gdouble f, p, q, t;

  if (*s == 0.0)
    {
      *h = *v;
      *s = *v;
      *v = *v; /* heh */
    }
  else
    {
      hue = *h * 6.0;
      saturation = *s;
      value = *v;

      if (hue == 6.0)
	hue = 0.0;

      f = hue - (int) hue;
      p = value * (1.0 - saturation);
      q = value * (1.0 - saturation * f);
      t = value * (1.0 - saturation * (1.0 - f));

      switch ((int) hue)
	{
	case 0:
	  *h = value;
	  *s = t;
	  *v = p;
	  break;

	case 1:
	  *h = q;
	  *s = value;
	  *v = p;
	  break;

	case 2:
	  *h = p;
	  *s = value;
	  *v = t;
	  break;

	case 3:
	  *h = p;
	  *s = q;
	  *v = value;
	  break;

	case 4:
	  *h = t;
	  *s = p;
	  *v = value;
	  break;

	case 5:
	  *h = value;
	  *s = p;
	  *v = q;
	  break;

	default:
	  g_assert_not_reached ();
	}
    }
}

static void
make_palette (GnomeRRLabeler *labeler)
{
	/* The idea is that we go around an hue color wheel.  We want to start
	 * at red, go around to green/etc. and stop at blue --- because magenta
	 * is evil.  Eeeeek, no magenta, please!
	 *
	 * Purple would be nice, though.  Remember that we are watered down
	 * (i.e. low saturation), so that would be like Like berries with cream.
	 * Mmmmm, berries.
	 */
	double start_hue;
	double end_hue;
	int i;

	g_assert (labeler->num_outputs > 0);

	labeler->palette = g_new (GdkColor, labeler->num_outputs);

	start_hue = 0.0; /* red */
	end_hue   = 2.0/3; /* blue */

	for (i = 0; i < labeler->num_outputs; i++) {
		double h, s, v;

		h = start_hue + (end_hue - start_hue) / labeler->num_outputs * i;
		s = 1.0 / 3;
		v = 1.0;

		hsv_to_rgb (&h, &s, &v);

		labeler->palette[i].red   = (int) (65535 * h + 0.5);
		labeler->palette[i].green = (int) (65535 * s + 0.5);
		labeler->palette[i].blue  = (int) (65535 * v + 0.5);
	}
}

#define LABEL_WINDOW_EDGE_THICKNESS 2
#define LABEL_WINDOW_PADDING 12

static gboolean
label_window_expose_event_cb (GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
	cairo_t *cr;
	GdkColor *color;

	color = g_object_get_data (G_OBJECT (widget), "color");

	cr = gdk_cairo_create (widget->window);

	/* edge outline */

	cairo_set_source_rgb (cr, 0, 0, 0);
	cairo_rectangle (cr,
			 LABEL_WINDOW_EDGE_THICKNESS / 2.0,
			 LABEL_WINDOW_EDGE_THICKNESS / 2.0,
			 widget->allocation.width - LABEL_WINDOW_EDGE_THICKNESS,
			 widget->allocation.height - LABEL_WINDOW_EDGE_THICKNESS);
	cairo_set_line_width (cr, LABEL_WINDOW_EDGE_THICKNESS);
	cairo_stroke (cr);

	/* fill */

	gdk_cairo_set_source_color (cr, color);
	cairo_rectangle (cr,
			 LABEL_WINDOW_EDGE_THICKNESS,
			 LABEL_WINDOW_EDGE_THICKNESS,
			 widget->allocation.width - LABEL_WINDOW_EDGE_THICKNESS * 2,
			 widget->allocation.height - LABEL_WINDOW_EDGE_THICKNESS * 2);
	cairo_fill (cr);

	cairo_destroy (cr);

	return FALSE;
}

static GtkWidget *
create_label_window (GnomeRRLabeler *labeler, GnomeOutputInfo *output, GdkColor *color)
{
	GtkWidget *window;
	GtkWidget *widget;
	char *str;

	window = gtk_window_new (GTK_WINDOW_POPUP);
	GTK_WIDGET_SET_FLAGS (window, GTK_APP_PAINTABLE);

	gtk_container_set_border_width (GTK_CONTAINER (window), LABEL_WINDOW_PADDING + LABEL_WINDOW_EDGE_THICKNESS);

	/* This is semi-dangerous.  The color is part of the labeler->palette
	 * array.  Note that in gnome_rr_labeler_finalize(), we are careful to
	 * free the palette only after we free the windows.
	 */
	g_object_set_data (G_OBJECT (window), "color", color);

	g_signal_connect (window, "expose-event",
			  G_CALLBACK (label_window_expose_event_cb), labeler);

	str = g_strdup_printf ("<b>%s</b>", output->display_name);
	widget = gtk_label_new (NULL);
	gtk_label_set_markup (GTK_LABEL (widget), str);
	g_free (str);

	gtk_container_add (GTK_CONTAINER (window), widget);

	/* Should we center this at the top edge of the monitor, instead of using the upper-left corner? */
	gtk_window_move (GTK_WINDOW (window), output->x, output->y);

	gtk_widget_show_all (window);

	return window;
}

static void
create_label_windows (GnomeRRLabeler *labeler)
{
	int i;

	/* FIXME: this doesn't handle cloned outputs yet */

	labeler->windows = g_new (GtkWidget *, labeler->num_outputs);

	for (i = 0; i < labeler->num_outputs; i++) {
		if (labeler->config->outputs[i]->on) {
			labeler->windows[i] = create_label_window (labeler, labeler->config->outputs[i], labeler->palette + i);
		} else
			labeler->windows[i] = NULL;
	}
}

static void
setup_from_config (GnomeRRLabeler *labeler)
{
	labeler->num_outputs = count_outputs (labeler->config);

	make_palette (labeler);

	create_label_windows (labeler);
}

GnomeRRLabeler *
gnome_rr_labeler_new (GnomeRRConfig *config)
{
	GnomeRRLabeler *labeler;

	g_return_val_if_fail (config != NULL, NULL);

	labeler = g_object_new (GNOME_TYPE_RR_LABELER, NULL);
	labeler->config = config;

	setup_from_config (labeler);

	return labeler;
}

void
gnome_rr_labeler_hide (GnomeRRLabeler *labeler)
{
	int i;

	g_return_if_fail (GNOME_IS_RR_LABELER (labeler));

	for (i = 0; i < labeler->num_outputs; i++)
		if (labeler->windows[i] != NULL) {
			gtk_widget_destroy (labeler->windows[i]);
			labeler->windows[i] = NULL;
		}
}

void
gnome_rr_labeler_get_color_for_output (GnomeRRLabeler *labeler, GnomeOutputInfo *output, GdkColor *color_out)
{
	int i;

	g_return_if_fail (GNOME_IS_RR_LABELER (labeler));
	g_return_if_fail (output != NULL);
	g_return_if_fail (color_out != NULL);

	for (i = 0; i < labeler->num_outputs; i++)
		if (labeler->config->outputs[i] == output) {
			*color_out = labeler->palette[i];
			return;
		}

	g_warning ("trying to get the color for unknown GnomeOutputInfo %p; returning magenta!", output);

	color_out->red   = 0xffff;
	color_out->green = 0;
	color_out->blue  = 0xffff;
}
