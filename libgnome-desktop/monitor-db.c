/* monitor-db.c
 *
 * Copyright 2007, 2008, Red Hat, Inc.
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
 * Author: Soren Sandmann <sandmann@redhat.com>
 */

#define GNOME_DESKTOP_USE_UNSTABLE_API

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "libgnomeui/monitor-db.h"
#include "edid.h"

#define CONFIG_BASENAME "monitors.xml"

/* In version 0 of the config file format, we had several <configuration> toplevel elements
 * and no explicit version number.  So, the filed looked like
 *
 *   <configuration>
 *     ...
 *   </configuration>
 *   <configuration>
 *     ...
 *   </configuration>
 *
 * Since version 1 of the config file, the file has a toplevel <monitors> element to group
 * all the configurations.  That element has a "version" attribute which is an integer.
 * So, the file looks like this:
 *
 *   <monitors>
 *     <configuration>
 *       ...
 *     </configuration>
 *     <configuration>
 *       ...
 *     </configuration>
 *   </monitors>
 */

/* A helper wrapper around the GMarkup parser stuff */
static gboolean parse_file_gmarkup (const gchar *file,
				    const GMarkupParser *parser,
				    gpointer data,
				    GError **err);

typedef struct CrtcAssignment CrtcAssignment;

static void            crtc_assignment_apply (CrtcAssignment *assign);
static CrtcAssignment *crtc_assignment_new   (RWScreen       *screen,
					      Output        **outputs);
static void            crtc_assignment_free  (CrtcAssignment *assign);
static void            output_free           (Output         *output);
static Output *	       output_copy	     (Output         *output);

static gchar *get_old_config_filename (void);
static gchar *get_config_filename (void);

typedef struct Parser Parser;

/* Parser for monitor configurations */
struct Parser
{
    int config_file_version;
    Output *output;
    Configuration *configuration;
    GPtrArray *outputs;
    GPtrArray *configurations;
    GQueue *stack;
};

static int
parse_int (const char *text)
{
    return strtol (text, NULL, 0);
}

static guint
parse_uint (const char *text)
{
    return strtoul (text, NULL, 0);
}

static gboolean
stack_is (Parser *parser,
	  const char *s1,
	  ...)
{
    GList *stack = NULL;
    const char *s;
    GList *l1, *l2;
    va_list args;
    
    stack = g_list_prepend (stack, (gpointer)s1);
    
    va_start (args, s1);
    
    s = va_arg (args, const char *);
    while (s)
    {
	stack = g_list_prepend (stack, (gpointer)s);
	s = va_arg (args, const char *);
    }
	
    l1 = stack;
    l2 = parser->stack->head;
    
    while (l1 && l2)
    {
	if (strcmp (l1->data, l2->data) != 0)
	{
	    g_list_free (stack);
	    return FALSE;
	}
	
	l1 = l1->next;
	l2 = l2->next;
    }
    
    g_list_free (stack);
    
    return (!l1 && !l2);
}

static void
handle_start_element (GMarkupParseContext *context,
		      const gchar         *name,
		      const gchar        **attr_names,
		      const gchar        **attr_values,
		      gpointer             user_data,
		      GError             **err)
{
    Parser *parser = user_data;

    if (strcmp (name, "output") == 0)
    {
	int i;
	g_assert (parser->output == NULL);

	parser->output = g_new0 (Output, 1);
	parser->output->rotation = 0;
	
	for (i = 0; attr_names[i] != NULL; ++i)
	{
	    if (strcmp (attr_names[i], "name") == 0)
	    {
		parser->output->name = g_strdup (attr_values[i]);
		break;
	    }
	}

	if (!parser->output->name)
	{
	    /* This really shouldn't happen, but it's better to make
	     * something up than to crash later.
	     */
	    g_warning ("Malformed monitor configuration file");
	    
	    parser->output->name = g_strdup ("default");
	}	
	parser->output->connected = FALSE;
	parser->output->on = FALSE;
    }
    else if (strcmp (name, "configuration") == 0)
    {
	g_assert (parser->configuration == NULL);
	
	parser->configuration = g_new0 (Configuration, 1);
	parser->configuration->clone = FALSE;
	parser->configuration->outputs = g_new0 (Output *, 1);
    }
    else if (strcmp (name, "monitors") == 0)
    {
	int i;

	for (i = 0; attr_names[i] != NULL; i++)
	{
	    if (strcmp (attr_names[i], "version") == 0)
	    {
		parser->config_file_version = parse_int (attr_values[i]);
		break;
	    }
	}
    }

    g_queue_push_tail (parser->stack, g_strdup (name));
}

static void
handle_end_element (GMarkupParseContext *context,
		    const gchar         *name,
		    gpointer             user_data,
		    GError             **err)
{
    Parser *parser = user_data;
    
    if (strcmp (name, "output") == 0)
    {
	/* If no rotation properties were set, just use RW_ROTATION_0 */
	if (parser->output->rotation == 0)
	    parser->output->rotation = RW_ROTATION_0;
	
	g_ptr_array_add (parser->outputs, parser->output);

	parser->output = NULL;
    }
    else if (strcmp (name, "configuration") == 0)
    {
	g_ptr_array_add (parser->outputs, NULL);
	parser->configuration->outputs =
	    (Output **)g_ptr_array_free (parser->outputs, FALSE);
	parser->outputs = g_ptr_array_new ();
	g_ptr_array_add (parser->configurations, parser->configuration);
	parser->configuration = NULL;
    }
    
    g_free (g_queue_pop_tail (parser->stack));
}

#define TOPLEVEL_ELEMENT (parser->config_file_version > 0 ? "monitors" : NULL)

static void
handle_text (GMarkupParseContext *context,
	     const gchar         *text,
	     gsize                text_len,
	     gpointer             user_data,
	     GError             **err)
{
    Parser *parser = user_data;
    
    if (stack_is (parser, "vendor", "output", "configuration", TOPLEVEL_ELEMENT, NULL))
    {
	parser->output->connected = TRUE;
	
	strncpy (parser->output->vendor, text, 3);
	parser->output->vendor[3] = 0;
    }
    else if (stack_is (parser, "clone", "configuration", TOPLEVEL_ELEMENT, NULL))
    {
	if (strcmp (text, "yes") == 0)
	    parser->configuration->clone = TRUE;
    }
    else if (stack_is (parser, "product", "output", "configuration", TOPLEVEL_ELEMENT, NULL))
    {
	parser->output->connected = TRUE;

	parser->output->product = parse_int (text);
    }
    else if (stack_is (parser, "serial", "output", "configuration", TOPLEVEL_ELEMENT, NULL))
    {
	parser->output->connected = TRUE;

	parser->output->serial = parse_uint (text);
    }
    else if (stack_is (parser, "width", "output", "configuration", TOPLEVEL_ELEMENT, NULL))
    {
	parser->output->on = TRUE;

	parser->output->width = parse_int (text);
    }
    else if (stack_is (parser, "x", "output", "configuration", TOPLEVEL_ELEMENT, NULL))
    {
	parser->output->on = TRUE;

	parser->output->x = parse_int (text);
    }
    else if (stack_is (parser, "y", "output", "configuration", TOPLEVEL_ELEMENT, NULL))
    {
	parser->output->on = TRUE;

	parser->output->y = parse_int (text);
    }
    else if (stack_is (parser, "height", "output", "configuration", TOPLEVEL_ELEMENT, NULL))
    {
	parser->output->on = TRUE;

	parser->output->height = parse_int (text);
    }
    else if (stack_is (parser, "rate", "output", "configuration", TOPLEVEL_ELEMENT, NULL))
    {
	parser->output->on = TRUE;

	parser->output->rate = parse_int (text);
    }
    else if (stack_is (parser, "rotation", "output", "configuration", TOPLEVEL_ELEMENT, NULL))
    {
	if (strcmp (text, "normal") == 0)
	{
	    parser->output->rotation |= RW_ROTATION_0;
	}
	else if (strcmp (text, "left") == 0)
	{
	    parser->output->rotation |= RW_ROTATION_90;
	}
	else if (strcmp (text, "upside_down") == 0)
	{
	    parser->output->rotation |= RW_ROTATION_180;
	}
	else if (strcmp (text, "right") == 0)
	{
	    parser->output->rotation |= RW_ROTATION_270;
	}
    }
    else if (stack_is (parser, "reflect_x", "output", "configuration", TOPLEVEL_ELEMENT, NULL))
    {
	if (strcmp (text, "yes") == 0)
	{
	    parser->output->rotation |= RW_REFLECT_X;
	}
    }
    else if (stack_is (parser, "reflect_y", "output", "configuration", TOPLEVEL_ELEMENT, NULL))
    {
	if (strcmp (text, "yes") == 0)
	{
	    parser->output->rotation |= RW_REFLECT_Y;
	}
    }
    else
    {
	/* Ignore other properties so we can expand the format in the future */
    }
}

static void
parser_free (Parser *parser)
{
    int i;
    GList *list;

    g_assert (parser != NULL);

    if (parser->output)
	output_free (parser->output);

    if (parser->configuration)
	configuration_free (parser->configuration);

    for (i = 0; i < parser->outputs->len; ++i)
    {
	Output *output = parser->outputs->pdata[i];

	output_free (output);
    }

    g_ptr_array_free (parser->outputs, TRUE);

    for (i = 0; i < parser->configurations->len; ++i)
    {
	Configuration *config = parser->configurations->pdata[i];

	configuration_free (config);
    }

    g_ptr_array_free (parser->configurations, TRUE);

    for (list = parser->stack->head; list; list = list->next)
	g_free (list->data);
    g_queue_free (parser->stack);
    
    g_free (parser);
}

static Configuration **
configurations_read_from_file (const gchar *filename, GError **error)
{
    Parser *parser = g_new0 (Parser, 1);
    Configuration **result;
    GMarkupParser callbacks = {
	handle_start_element,
	handle_end_element,
	handle_text,
	NULL, /* passthrough */
	NULL, /* error */
    };

    parser->config_file_version = 0;
    parser->configurations = g_ptr_array_new ();
    parser->outputs = g_ptr_array_new ();
    parser->stack = g_queue_new ();
    
    if (!parse_file_gmarkup (filename, &callbacks, parser, error))
    {
	result = NULL;
	
	g_assert (parser->outputs);
	goto out;
    }

    g_assert (parser->outputs);
    
    g_ptr_array_add (parser->configurations, NULL);
    result = (Configuration **)g_ptr_array_free (parser->configurations, FALSE);
    parser->configurations = g_ptr_array_new ();
    
    g_assert (parser->outputs);
out:
    parser_free (parser);

    return result;
}

static Configuration **
configurations_read (GError **error)
{
    char *filename;
    Configuration **configs;
    GError *err;

    /* Try the new configuration file... */

    filename = get_config_filename ();

    err = NULL;
    configs = configurations_read_from_file (filename, &err);

    g_free (filename);

    if (g_error_matches (err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    {
	g_error_free (err);

	/* Okay, so try the old configuration file */
	filename = get_old_config_filename ();
	configs = configurations_read_from_file (filename, error);
	g_free (filename);

	return configs;
    }

    g_propagate_error (error, err);
    return configs;
}

Configuration *
configuration_new_current (RWScreen *screen)
{
    Configuration *config = g_new0 (Configuration, 1);
    GPtrArray *a = g_ptr_array_new ();
    RWOutput **rw_outputs;
    int i;
    int clone_width = -1;
    int clone_height = -1;

    g_return_val_if_fail (screen != NULL, NULL);

    rw_outputs = rw_screen_list_outputs (screen);

    config->clone = FALSE;
    
    for (i = 0; rw_outputs[i] != NULL; ++i)
    {
	RWOutput *rw_output = rw_outputs[i];
	Output *output = g_new0 (Output, 1);
	RWMode *mode = NULL;
	const guint8 *edid_data = rw_output_get_edid_data (rw_output);
	RWCrtc *crtc;

	output->name = g_strdup (rw_output_get_name (rw_output));
	output->connected = rw_output_is_connected (rw_output);

	if (!output->connected)
	{
	    output->x = -1;
	    output->y = -1;
	    output->width = -1;
	    output->height = -1;
	    output->rate = -1;
	    output->rotation = RW_ROTATION_0;
	}
	else
	{
	    MonitorInfo *info = NULL;

	    if (edid_data)
		info = decode_edid (edid_data);

	    if (info)
	    {
		memcpy (output->vendor, info->manufacturer_code,
			sizeof (output->vendor));
		
		output->product = info->product_code;
		output->serial = info->serial_number;
		output->aspect = info->aspect_ratio;
	    }
	    else
	    {
		strcpy (output->vendor, "???");
		output->product = 0;
		output->serial = 0;
	    }
	    
	    output->display_name = make_display_name (
		rw_output_get_name (rw_output), info);
		
	    g_free (info);
		
	    crtc = rw_output_get_crtc (rw_output);
	    mode = crtc? rw_crtc_get_current_mode (crtc) : NULL;
	    
	    if (crtc && mode)
	    {
		output->on = TRUE;
		
		rw_crtc_get_position (crtc, &output->x, &output->y);
		output->width = rw_mode_get_width (mode);
		output->height = rw_mode_get_height (mode);
		output->rate = rw_mode_get_freq (mode);
		output->rotation = rw_crtc_get_current_rotation (crtc);

		if (output->x == 0 && output->y == 0) {
			if (clone_width == -1) {
				clone_width = output->width;
				clone_height = output->height;
			} else if (clone_width == output->width &&
					clone_height == output->height) {
				config->clone = TRUE;
			}
		}
	    }
	    else
	    {
		output->on = FALSE;
		config->clone = FALSE;
	    }

	    /* Get preferred size for the monitor */
	    mode = rw_output_get_preferred_mode (rw_output);
	    
	    if (!mode)
	    {
		RWMode **modes = rw_output_list_modes (rw_output);
		
		/* FIXME: we should pick the "best" mode here, where best is
		 * sorted wrt
		 *
		 * - closest aspect ratio
		 * - mode area
		 * - refresh rate
		 * - We may want to extend randrwrap so that get_preferred
		 *   returns that - although that could also depend on
		 *   the crtc.
		 */
		if (modes[0])
		    mode = modes[0];
	    }
	    
	    if (mode)
	    {
		output->pref_width = rw_mode_get_width (mode);
		output->pref_height = rw_mode_get_height (mode);
	    }
	    else
	    {
		/* Pick some random numbers. This should basically never happen */
		output->pref_width = 1024;
		output->pref_height = 768;
	    }
	}
 
	g_ptr_array_add (a, output);
    }

    g_ptr_array_add (a, NULL);
    
    config->outputs = (Output **)g_ptr_array_free (a, FALSE);

    g_assert (configuration_match (config, config));
    
    return config;
}

static void
output_free (Output *output)
{
    if (output->display_name)
	g_free (output->display_name);

    if (output->name)
	g_free (output->name);
    
    g_free (output);
}

static Output *
output_copy (Output *output)
{
    Output *copy = g_new0 (Output, 1);

    *copy = *output;

    copy->name = g_strdup (output->name);
    copy->display_name = g_strdup (output->display_name);

    return copy;
}

static void
outputs_free (Output **outputs)
{
    int i;

    g_assert (outputs != NULL);

    for (i = 0; outputs[i] != NULL; ++i)
	output_free (outputs[i]);
}

void
configuration_free (Configuration *config)
{
    g_return_if_fail (config != NULL);
    outputs_free (config->outputs);
    
    g_free (config);
}

static void
configurations_free (Configuration **configurations)
{
    int i;

    g_assert (configurations != NULL);

    for (i = 0; configurations[i] != NULL; ++i)
	configuration_free (configurations[i]);

    g_free (configurations);
}

static gboolean
parse_file_gmarkup (const gchar *filename,
		    const GMarkupParser *parser,
		    gpointer data,
		    GError **err)
{
    GMarkupParseContext *context = NULL;
    gchar *contents = NULL;
    gboolean result = TRUE;
    gsize len;

    if (!g_file_get_contents (filename, &contents, &len, err))
    {
	result = FALSE;
	goto out;
    }
    
    context = g_markup_parse_context_new (parser, 0, data, NULL);

    if (!g_markup_parse_context_parse (context, contents, len, err))
    {
	result = FALSE;
	goto out;
    }

    if (!g_markup_parse_context_end_parse (context, err))
    {
	result = FALSE;
	goto out;
    }

out:
    if (contents)
	g_free (contents);

    if (context)
	g_markup_parse_context_free (context);

    return result;
}

static gboolean
output_match (Output *output1, Output *output2)
{
    g_assert (output1 != NULL);
    g_assert (output2 != NULL);

    if (strcmp (output1->name, output2->name) != 0)
	return FALSE;

    if (strcmp (output1->vendor, output2->vendor) != 0)
	return FALSE;

    if (output1->product != output2->product)
	return FALSE;

    if (output1->serial != output2->serial)
	return FALSE;

    if (output1->connected != output2->connected)
	return FALSE;
    
    return TRUE;
}

static Output *
find_output (Configuration *config, const char *name)
{
    int i;

    for (i = 0; config->outputs[i] != NULL; ++i)
    {
	Output *output = config->outputs[i];
	
	if (strcmp (name, output->name) == 0)
	    return output;
    }

    return NULL;
}

gboolean
configuration_match (Configuration *c1, Configuration *c2)
{
    int i;

    for (i = 0; c1->outputs[i] != NULL; ++i)
    {
	Output *output1 = c1->outputs[i];
	Output *output2;

	output2 = find_output (c2, output1->name);
	if (!output2 || !output_match (output1, output2))
	    return FALSE;
    }
    
    return TRUE;
}

static Output **
make_outputs (Configuration *config)
{
    GPtrArray *outputs;
    Output *first_on;;
    int i;

    outputs = g_ptr_array_new ();

    first_on = NULL;
    
    for (i = 0; config->outputs[i] != NULL; ++i)
    {
	Output *old = config->outputs[i];
	Output *new = output_copy (old);

	if (old->on && !first_on)
	    first_on = old;
	
	if (config->clone && new->on)
	{
	    g_assert (first_on);

	    new->width = first_on->width;
	    new->height = first_on->height;
	    new->rotation = first_on->rotation;
	    new->x = 0;
	    new->y = 0;
	}

	g_ptr_array_add (outputs, new);
    }

    g_ptr_array_add (outputs, NULL);

    return (Output **)g_ptr_array_free (outputs, FALSE);
}

gboolean
configuration_applicable (Configuration  *configuration,
			  RWScreen       *screen)
{
    Output **outputs = make_outputs (configuration);
    CrtcAssignment *assign = crtc_assignment_new (screen, outputs);
    gboolean result;

    if (assign)
    {
	result = TRUE;
	crtc_assignment_free (assign);
    }
    else
    {
	result = FALSE;
    }

    outputs_free (outputs);

    return result;
}

static Configuration *
configuration_find (Configuration **haystack,
		    Configuration  *needle)
{
    int i;

    for (i = 0; haystack[i] != NULL; ++i)
    {
	if (configuration_match (haystack[i], needle))
	    return haystack[i];
    }

    return NULL;
}

/* Database management */

static gchar *
get_old_config_filename (void)
{
    return g_build_filename (g_get_home_dir(), ".gnome2", CONFIG_BASENAME, NULL);
}

static gchar *
get_config_filename (void)
{
    return g_build_filename (g_get_user_config_dir (), CONFIG_BASENAME, NULL);
}

static const char *
get_rotation_name (RWRotation r)
{
    if (r & RW_ROTATION_0)
	return "normal";
    if (r & RW_ROTATION_90)
	return "left";
    if (r & RW_ROTATION_180)
	return "upside_down";
    if (r & RW_ROTATION_270)
	return "right";

    return "normal";
}

static const char *
yes_no (int x)
{
    return x? "yes" : "no";
}

static const char *
get_reflect_x (RWRotation r)
{
    return yes_no (r & RW_REFLECT_X);
}

static const char *
get_reflect_y (RWRotation r)
{
    return yes_no (r & RW_REFLECT_Y);
}

static void
emit_configuration (Configuration *config,
		    GString *string)
{
    int j;

    g_string_append_printf (string, "  <configuration>\n");

    g_string_append_printf (string, "      <clone>%s</clone>\n", yes_no (config->clone));
    
    for (j = 0; config->outputs[j] != NULL; ++j)
    {
	Output *output = config->outputs[j];
	
	g_string_append_printf (
	    string, "      <output name=\"%s\">\n", output->name);
	
	if (output->connected && *output->vendor != '\0')
	{
	    g_string_append_printf (
		string, "          <vendor>%s</vendor>\n", output->vendor);
	    g_string_append_printf (
		string, "          <product>0x%04x</product>\n", output->product);
	    g_string_append_printf (
		string, "          <serial>0x%08x</serial>\n", output->serial);
	}
	
	/* An unconnected output which is on does not make sense */
	if (output->connected && output->on)
	{
	    g_string_append_printf (
		string, "          <width>%d</width>\n", output->width);
	    g_string_append_printf (
		string, "          <height>%d</height>\n", output->height);
	    g_string_append_printf (
		string, "          <rate>%d</rate>\n", output->rate);
	    g_string_append_printf (
		string, "          <x>%d</x>\n", output->x);
	    g_string_append_printf (
		string, "          <y>%d</y>\n", output->y);
	    g_string_append_printf (
		string, "          <rotation>%s</rotation>\n", get_rotation_name (output->rotation));
	    g_string_append_printf (
		string, "          <reflect_x>%s</reflect_x>\n", get_reflect_x (output->rotation));
	    g_string_append_printf (
		string, "          <reflect_y>%s</reflect_y>\n", get_reflect_y (output->rotation));
	}
	
	g_string_append_printf (string, "      </output>\n");
    }
    
    g_string_append_printf (string, "  </configuration>\n");
}

void
configuration_sanitize (Configuration *config)
{
    int i;
    int x_offset, y_offset;

    /* Offset everything by the top/left-most coordinate to
     * make sure the configuration starts at (0, 0)
     */
    x_offset = y_offset = G_MAXINT;
    for (i = 0; config->outputs[i]; ++i)
    {
	Output *output = config->outputs[i];

	if (output->on)
	{
	    x_offset = MIN (x_offset, output->x);
	    y_offset = MIN (y_offset, output->y);
	}
    }

    for (i = 0; config->outputs[i]; ++i)
    {
	Output *output = config->outputs[i];
	
	if (output->on)
	{
	    output->x -= x_offset;
	    output->y -= y_offset;
	}
    }
}


gboolean
configuration_save (Configuration *configuration, GError **err)
{
    Configuration **configurations;
    GString *output = g_string_new("");
    int i;
    gchar *filename;
    gboolean result;

    configurations = configurations_read (NULL); /* NULL-GError */
    
    g_string_append_printf (output, "<monitors version=\"1\">\n");

    if (configurations)
    {
	for (i = 0; configurations[i] != NULL; ++i)
	{
	    if (!configuration_match (configurations[i], configuration))
		emit_configuration (configurations[i], output);
	}

	configurations_free (configurations);
    }

    emit_configuration (configuration, output);

    g_string_append_printf (output, "</monitors>\n");

    filename = get_config_filename ();
    result = g_file_set_contents (filename, output->str, -1, err);
    g_free (filename);

    if (result)
    {
	/* Only remove the old config file if we were successful in saving the new one */

	filename = get_old_config_filename ();
	if (g_file_test (filename, G_FILE_TEST_EXISTS))
	    g_unlink (filename);

	g_free (filename);
    }

    return result;
}

static gboolean
apply_configuration (Configuration *conf, RWScreen *screen)
{
    CrtcAssignment *assignment;
    Output **outputs;

    outputs = make_outputs (conf);

    assignment = crtc_assignment_new (screen, outputs);

    outputs_free (outputs);
    
    if (assignment)
    {
	crtc_assignment_apply (assignment);
	    
	crtc_assignment_free (assignment);

	return TRUE;
    }

    return FALSE;
}

gboolean
configuration_apply_stored (RWScreen *screen)
{
    Configuration **configs = configurations_read (NULL); /* NULL-GError */
    Configuration *current;
    Configuration *found;
    gboolean result;

    if (!screen)
	return FALSE;
    
    rw_screen_refresh (screen);
    
    current = configuration_new_current (screen);
    if (configs)
    {
	if ((found = configuration_find (configs, current)))
	{
	    apply_configuration (found, screen);
	    result = TRUE;
	}
	else
	{
	    result = FALSE;
	}
	
	configurations_free (configs);
    }
	
    configuration_free (current);

    return result;
}


/*
 * CRTC assignment
 */
typedef struct CrtcInfo CrtcInfo;

struct CrtcInfo
{
    RWMode    *mode;
    int        x;
    int        y;
    RWRotation rotation;
    GPtrArray *outputs;
};

struct CrtcAssignment
{
    RWScreen *screen;
    GHashTable *info;
};

static gboolean
can_clone (CrtcInfo *info,
	   RWOutput *output)
{
    int i;

    for (i = 0; i < info->outputs->len; ++i)
    {
	RWOutput *clone = info->outputs->pdata[i];

	if (!rw_output_can_clone (clone, output))
	    return FALSE;
    }

    return TRUE;
}

static gboolean
crtc_assignment_assign (CrtcAssignment *assign,
			RWCrtc         *crtc,
			RWMode         *mode,
			int             x,
			int             y,
			RWRotation      rotation,
			RWOutput       *output)
{
    /* FIXME: We should reject stuff that is outside the screen ranges */
    
    CrtcInfo *info = g_hash_table_lookup (assign->info, crtc);

    if (!rw_crtc_can_drive_output (crtc, output) ||
	!rw_output_supports_mode (output, mode)  ||
	!rw_crtc_supports_rotation (crtc, rotation))
    {
	return FALSE;
    }

    if (info)
    {
	if (info->mode == mode		&&
	    info->x == x		&&
	    info->y == y		&&
	    info->rotation == rotation  &&
	    can_clone (info, output))
	{
	    g_ptr_array_add (info->outputs, output);

	    return TRUE;
	}
    }
    else
    {	
	CrtcInfo *info = g_new0 (CrtcInfo, 1);
	
	info->mode = mode;
	info->x = x;
	info->y = y;
	info->rotation = rotation;
	info->outputs = g_ptr_array_new ();
	
	g_ptr_array_add (info->outputs, output);
	
	g_hash_table_insert (assign->info, crtc, info);
	    
	return TRUE;
    }
    
    return FALSE;
}

static void
crtc_assignment_unassign (CrtcAssignment *assign,
			  RWCrtc         *crtc,
			  RWOutput       *output)
{
    CrtcInfo *info = g_hash_table_lookup (assign->info, crtc);

    if (info)
    {
	g_ptr_array_remove (info->outputs, output);

	if (info->outputs->len == 0)
	    g_hash_table_remove (assign->info, crtc);
    }
}

static void
crtc_assignment_free (CrtcAssignment *assign)
{
    g_hash_table_destroy (assign->info);

    g_free (assign);
}

static void
configure_crtc (gpointer key,
		gpointer value,
		gpointer data)
{
    RWCrtc *crtc = key;
    CrtcInfo *info = value;

    rw_crtc_set_config (crtc,
			info->x, info->y,
			info->mode,
			info->rotation,
			(RWOutput **)info->outputs->pdata,
			info->outputs->len);
}

static gboolean
mode_is_rotated (CrtcInfo *info)
{
    if ((info->rotation & RW_ROTATION_270)		||
	(info->rotation & RW_ROTATION_90))
    {
	return TRUE;
    }
    return FALSE;
}

static gboolean
crtc_is_rotated (RWCrtc *crtc)
{
    RWRotation r = rw_crtc_get_current_rotation (crtc);

    if ((r & RW_ROTATION_270)		||
	(r & RW_ROTATION_90))
    {
	return TRUE;
    }

    return FALSE;
}

static void
crtc_assignment_apply (CrtcAssignment *assign)
{
    GList *active_crtcs = g_hash_table_get_keys (assign->info);
    RWCrtc **all_crtcs = rw_screen_list_crtcs (assign->screen);
    GList *list;
    int width, height;
    int i;
    int min_width, max_width, min_height, max_height;
    int width_mm, height_mm;

    /* Compute size of the screen */
    width = height = 1;
    for (list = active_crtcs; list != NULL; list = list->next)
    {
	RWCrtc *crtc = list->data;
	CrtcInfo *info = g_hash_table_lookup (assign->info, crtc);
	int w, h;

	w = rw_mode_get_width (info->mode);
	h = rw_mode_get_height (info->mode);
	
	if (mode_is_rotated (info))
	{
	    int tmp = h;
	    h = w;
	    w = tmp;
	}
	
	width = MAX (width, info->x + w);
	height = MAX (height, info->y + h);
    }
    g_list_free (active_crtcs);

    rw_screen_get_ranges (
	assign->screen, &min_width, &max_width, &min_height, &max_height);

    width = MAX (min_width, width);
    width = MIN (max_width, width);
    height = MAX (min_height, height);
    height = MIN (max_height, height);
    
    /* Turn off all crtcs currently displaying outside the new screen */
    for (i = 0; all_crtcs[i] != NULL; ++i)
    {
	RWCrtc *crtc = all_crtcs[i];
	RWMode *mode = rw_crtc_get_current_mode (crtc);
	int x, y;

	if (mode)
	{
	    int w, h;
	    rw_crtc_get_position (crtc, &x, &y);

	    w = rw_mode_get_width (mode);
	    h = rw_mode_get_height (mode);
	    
	    if (crtc_is_rotated (crtc))
	    {
		int tmp = h;
		h = w;
		w = tmp;
	    }
	    
	    if (x + w > width || y + h > height)
		rw_crtc_set_config (crtc, 0, 0, NULL, RW_ROTATION_0, NULL, 0);
	}
    }

    /* Turn off all CRTC's that are not in the assignment */
    for (i = 0; all_crtcs[i] != NULL; ++i)
    {
	RWCrtc *crtc = all_crtcs[i];
	
	if (!g_hash_table_lookup (assign->info, crtc))
	    rw_crtc_set_config (crtc, 0, 0, NULL, RW_ROTATION_0, NULL, 0);
    }

    /* The 'physical size' of an X screen is meaningless if that screen
     * can consist of many monitors. So just pick a size that make the
     * dpi 96.
     *
     * Firefox and Evince apparently believe what X tells them.
     */
    width_mm = (width / 96.0) * 25.4 + 0.5;
    height_mm = (height / 96.0) * 25.4 + 0.5;
    
    rw_screen_set_size (assign->screen, width, height, width_mm, height_mm);

    g_hash_table_foreach (assign->info, configure_crtc, NULL);
}

/* Check whether the given set of settings can be used
 * at the same time -- ie. whether there is an assignment
 * of CRTC's to outputs.
 *
 * Brute force - the number of objects involved is small
 * enough that it doesn't matter.
 */
static gboolean
real_assign_crtcs (RWScreen *screen,
		   Output **outputs,
		   CrtcAssignment *assignment)
{
    RWCrtc **crtcs = rw_screen_list_crtcs (screen);
    Output *output;
    int i;

    output = *outputs;
    if (!output)
	return TRUE;

    /* It is always allowed for an output to be turned off */
    if (!output->on)
    {
	return real_assign_crtcs (screen, outputs + 1, assignment);
    }

    for (i = 0; crtcs[i] != NULL; ++i)
    {
	int pass;

	/* Make two passses, one where frequencies must match, then
	 * one where they don't have to
	 */
	for (pass = 0; pass < 2; ++pass)
	{
	    RWCrtc *crtc = crtcs[i];
	    RWOutput *rw_output = rw_screen_get_output_by_name (screen, output->name);
	    RWMode **modes = rw_output_list_modes (rw_output);
	    int j;
	
	    for (j = 0; modes[j] != NULL; ++j)
	    {
		RWMode *mode = modes[j];
		
		if (rw_mode_get_width (mode) == output->width	&&
		    rw_mode_get_height (mode) == output->height &&
		    (pass == 1 || rw_mode_get_freq (mode) == output->rate))
		{
		    if (crtc_assignment_assign (
			    assignment, crtc, modes[j],
			    output->x, output->y,
			    output->rotation,
			    rw_output))
		    {
			if (real_assign_crtcs (screen, outputs + 1, assignment))
			    return TRUE;
			
			crtc_assignment_unassign (assignment, crtc, rw_output);
		    }
		}
	    }
	}
    }

    return FALSE;
}

static void
crtc_info_free (CrtcInfo *info)
{
    g_ptr_array_free (info->outputs, TRUE);
    g_free (info);
}

static CrtcAssignment *
crtc_assignment_new (RWScreen *screen, Output **outputs)
{
    CrtcAssignment *assignment = g_new0 (CrtcAssignment, 1);

    assignment->info = g_hash_table_new_full (
	g_direct_hash, g_direct_equal, NULL, (GFreeFunc)crtc_info_free);

    if (real_assign_crtcs (screen, outputs, assignment))
    {
	assignment->screen = screen;
	
	return assignment;
    }
    else
    {
	crtc_assignment_free (assignment);
    
	return NULL;
    }
}
