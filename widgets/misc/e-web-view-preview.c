/*
 * e-web-view-preview.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#include "e-web-view-preview.h"

#include <config.h>
#include <string.h>
#include <glib/gi18n-lib.h>

struct _EWebViewPreviewPrivate {
	gboolean escape_values;
	GString *updating_content; /* is NULL when not between begin_update/end_update */
};

enum {
	PROP_0,
	PROP_TREE_VIEW,
	PROP_PREVIEW_WIDGET,
	PROP_ESCAPE_VALUES
};

G_DEFINE_TYPE (
	EWebViewPreview,
	e_web_view_preview,
	GTK_TYPE_VPANED);

static void
web_view_preview_set_property (GObject *object,
                       guint property_id,
                       const GValue *value,
                       GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_ESCAPE_VALUES:
			e_web_view_preview_set_escape_values (
				E_WEB_VIEW_PREVIEW (object),
				g_value_get_boolean (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
web_view_preview_get_property (GObject *object,
                       guint property_id,
                       GValue *value,
                       GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_TREE_VIEW:
			g_value_set_object (
				value, e_web_view_preview_get_tree_view (
				E_WEB_VIEW_PREVIEW (object)));
			return;

		case PROP_PREVIEW_WIDGET:
			g_value_set_object (
				value, e_web_view_preview_get_preview (
				E_WEB_VIEW_PREVIEW (object)));
			return;

		case PROP_ESCAPE_VALUES:
			g_value_set_boolean (
				value, e_web_view_preview_get_escape_values (
				E_WEB_VIEW_PREVIEW (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
web_view_preview_dispose (GObject *object)
{
	EWebViewPreviewPrivate *priv;

	priv = E_WEB_VIEW_PREVIEW (object)->priv;

	if (priv->updating_content != NULL) {
		g_string_free (priv->updating_content, TRUE);
		priv->updating_content = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_web_view_preview_parent_class)->dispose (object);
}

static void
e_web_view_preview_class_init (EWebViewPreviewClass *klass)
{
	GObjectClass *object_class;

	g_type_class_add_private (klass, sizeof (EWebViewPreviewPrivate));

	object_class = G_OBJECT_CLASS (klass);
	object_class->set_property = web_view_preview_set_property;
	object_class->get_property = web_view_preview_get_property;
	object_class->dispose = web_view_preview_dispose;

	g_object_class_install_property (
		object_class,
		PROP_TREE_VIEW,
		g_param_spec_object (
			"tree-view",
			"Tree View",
			NULL,
			GTK_TYPE_TREE_VIEW,
			G_PARAM_READABLE));

	g_object_class_install_property (
		object_class,
		PROP_PREVIEW_WIDGET,
		g_param_spec_object (
			"preview-widget",
			"Preview Widget",
			NULL,
			GTK_TYPE_WIDGET,
			G_PARAM_READABLE));

	g_object_class_install_property (
		object_class,
		PROP_ESCAPE_VALUES,
		g_param_spec_boolean (
			"escape-values",
			"Whether escaping values automatically, when inserting",
			NULL,
			TRUE,
			G_PARAM_READWRITE));
}

static GtkWidget *
in_scrolled_window (GtkWidget *widget)
{
	GtkWidget *sw;

	g_return_val_if_fail (widget != NULL, NULL);

	sw = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add (GTK_CONTAINER (sw), widget);

	gtk_widget_show (widget);
	gtk_widget_show (sw);

	return sw;
}

static void
e_web_view_preview_init (EWebViewPreview *preview)
{
	GtkWidget *tree_view_sw, *web_view_sw;

	preview->priv = G_TYPE_INSTANCE_GET_PRIVATE (
		preview, E_TYPE_WEB_VIEW_PREVIEW, EWebViewPreviewPrivate);
	preview->priv->escape_values = TRUE;

	tree_view_sw = in_scrolled_window (gtk_tree_view_new ());
	web_view_sw = in_scrolled_window (e_web_view_new ());

	gtk_widget_hide (tree_view_sw);
	gtk_widget_show (web_view_sw);

	gtk_paned_pack1 (GTK_PANED (preview), tree_view_sw, FALSE, TRUE);
	gtk_paned_pack2 (GTK_PANED (preview), web_view_sw, TRUE, TRUE);

	/* rawly 3 lines of a text plus a little bit more */
	if (gtk_paned_get_position (GTK_PANED (preview)) < 85)
		gtk_paned_set_position (GTK_PANED (preview), 85);
}

GtkWidget *
e_web_view_preview_new (void)
{
	return g_object_new (E_TYPE_WEB_VIEW_PREVIEW, NULL);
}

GtkTreeView *
e_web_view_preview_get_tree_view (EWebViewPreview *preview)
{
	g_return_val_if_fail (preview != NULL, NULL);
	g_return_val_if_fail (E_IS_WEB_VIEW_PREVIEW (preview), NULL);

	return GTK_TREE_VIEW (gtk_bin_get_child (GTK_BIN (gtk_paned_get_child1 (GTK_PANED (preview)))));
}

GtkWidget *
e_web_view_preview_get_preview	(EWebViewPreview *preview)
{
	g_return_val_if_fail (preview != NULL, NULL);
	g_return_val_if_fail (E_IS_WEB_VIEW_PREVIEW (preview), NULL);

	return gtk_bin_get_child (GTK_BIN (gtk_paned_get_child2 (GTK_PANED (preview))));
}

void
e_web_view_preview_set_preview	(EWebViewPreview *preview, GtkWidget *preview_widget)
{
	GtkWidget *old_child;

	g_return_if_fail (preview != NULL);
	g_return_if_fail (E_IS_WEB_VIEW_PREVIEW (preview));
	g_return_if_fail (preview_widget != NULL);
	g_return_if_fail (GTK_IS_WIDGET (preview_widget));

	old_child = gtk_bin_get_child (GTK_BIN (gtk_paned_get_child2 (GTK_PANED (preview))));
	if (old_child) {
		g_return_if_fail (old_child != preview_widget);
		gtk_widget_destroy (old_child);
	}

	gtk_container_add (GTK_CONTAINER (gtk_paned_get_child2 (GTK_PANED (preview))), preview_widget);
}

void
e_web_view_preview_show_tree_view (EWebViewPreview *preview)
{
	g_return_if_fail (preview != NULL);
	g_return_if_fail (E_IS_WEB_VIEW_PREVIEW (preview));

	gtk_widget_show (gtk_paned_get_child1 (GTK_PANED (preview)));
}

void
e_web_view_preview_hide_tree_view (EWebViewPreview *preview)
{
	g_return_if_fail (preview != NULL);
	g_return_if_fail (E_IS_WEB_VIEW_PREVIEW (preview));

	gtk_widget_hide (gtk_paned_get_child1 (GTK_PANED (preview)));
}

void
e_web_view_preview_set_escape_values (EWebViewPreview *preview, gboolean escape)
{
	g_return_if_fail (preview != NULL);
	g_return_if_fail (E_IS_WEB_VIEW_PREVIEW (preview));
	g_return_if_fail (preview->priv != NULL);

	preview->priv->escape_values = escape;
}

gboolean
e_web_view_preview_get_escape_values (EWebViewPreview *preview)
{
	g_return_val_if_fail (preview != NULL, FALSE);
	g_return_val_if_fail (E_IS_WEB_VIEW_PREVIEW (preview), FALSE);
	g_return_val_if_fail (preview->priv != NULL, FALSE);

	return preview->priv->escape_values;
}

void
e_web_view_preview_begin_update (EWebViewPreview *preview)
{
	g_return_if_fail (preview != NULL);
	g_return_if_fail (E_IS_WEB_VIEW_PREVIEW (preview));
	g_return_if_fail (preview->priv != NULL);

	if (preview->priv->updating_content) {
		g_warning ("%s: Previous content update isn't finished with e_web_view_preview_end_update()", G_STRFUNC);
		g_string_free (preview->priv->updating_content, TRUE);
	}

	preview->priv->updating_content = g_string_new ("<TABLE width=\"100%\" border=\"0\" cols=\"2\">");
}

void
e_web_view_preview_end_update (EWebViewPreview *preview)
{
	GtkWidget *web_view;

	g_return_if_fail (preview != NULL);
	g_return_if_fail (E_IS_WEB_VIEW_PREVIEW (preview));
	g_return_if_fail (preview->priv != NULL);
	g_return_if_fail (preview->priv->updating_content != NULL);

	g_string_append (preview->priv->updating_content, "</TABLE>");

	web_view = e_web_view_preview_get_preview (preview);
	if (E_IS_WEB_VIEW (web_view))
		e_web_view_load_string (E_WEB_VIEW (web_view), preview->priv->updating_content->str);

	g_string_free (preview->priv->updating_content, TRUE);
	preview->priv->updating_content = NULL;
}

static gchar *
replace_string (const gchar *text, const gchar *find, const gchar *replace)
{
	const gchar *p, *next;
	GString *str;
	gint find_len;

	g_return_val_if_fail (text != NULL, NULL);
	g_return_val_if_fail (find != NULL, NULL);
	g_return_val_if_fail (*find, NULL);

	find_len = strlen (find);
	str = g_string_new ("");

	p = text;
	while (next = strstr (p, find), next) {
		if (p + 1 < next)
			g_string_append_len (str, p, next - p);

		if (replace && *replace)
			g_string_append (str, replace);

		p = next + find_len;
	}

	g_string_append (str, p);

	return g_string_free (str, FALSE);
}

static gchar *
web_view_preview_escape_text (EWebViewPreview *preview, const gchar *text)
{
	gchar *utf8_valid, *res, *end;

	if (!e_web_view_preview_get_escape_values (preview))
		return NULL;

	g_return_val_if_fail (text != NULL, NULL);

	if (g_utf8_validate (text, -1, NULL)) {
		res = g_markup_escape_text (text, -1);
	} else {
		utf8_valid = g_strdup (text);
		while (end = NULL, !g_utf8_validate (utf8_valid, -1, (const gchar **) &end) && end && *end)
			*end = '?';

		res = g_markup_escape_text (utf8_valid, -1);

		g_free (utf8_valid);
	}

	if (res && strchr (res, '\n')) {
		/* replace line breaks with <BR> */
		if (strchr (res, '\r')) {
			end = replace_string (res, "\r", "");
			g_free (res);
			res = end;
		}

		end = replace_string (res, "\n", "<BR>");
		g_free (res);
		res = end;
	}

	return res;
}

void
e_web_view_preview_add_header (EWebViewPreview *preview, gint index, const gchar *header)
{
	gchar *escaped;

	g_return_if_fail (preview != NULL);
	g_return_if_fail (E_IS_WEB_VIEW_PREVIEW (preview));
	g_return_if_fail (preview->priv != NULL);
	g_return_if_fail (preview->priv->updating_content != NULL);
	g_return_if_fail (header != NULL);

	if (index < 1)
		index = 1;
	else if (index > 6)
		index = 6;

	escaped = web_view_preview_escape_text (preview, header);
	if (escaped)
		header = escaped;

	g_string_append_printf (preview->priv->updating_content, "<TR><TD colspan=2><H%d>%s</H%d></TD></TR>", index, header, index);

	g_free (escaped);
}

void
e_web_view_preview_add_text (EWebViewPreview *preview, const gchar *text)
{
	gchar *escaped;

	g_return_if_fail (preview != NULL);
	g_return_if_fail (E_IS_WEB_VIEW_PREVIEW (preview));
	g_return_if_fail (preview->priv != NULL);
	g_return_if_fail (preview->priv->updating_content != NULL);
	g_return_if_fail (text != NULL);

	escaped = web_view_preview_escape_text (preview, text);
	if (escaped)
		text = escaped;

	g_string_append_printf (preview->priv->updating_content, "<TR><TD colspan=2><FONT size=\"3\">%s</FONT></TD></TR>", text);

	g_free (escaped);
}

void
e_web_view_preview_add_raw_html (EWebViewPreview *preview, const gchar *raw_html)
{
	g_return_if_fail (preview != NULL);
	g_return_if_fail (E_IS_WEB_VIEW_PREVIEW (preview));
	g_return_if_fail (preview->priv != NULL);
	g_return_if_fail (preview->priv->updating_content != NULL);
	g_return_if_fail (raw_html != NULL);

	g_string_append_printf (preview->priv->updating_content, "<TR><TD colspan=2>%s</TD></TR>", raw_html);
}

void
e_web_view_preview_add_separator (EWebViewPreview *preview)
{
	g_return_if_fail (preview != NULL);
	g_return_if_fail (E_IS_WEB_VIEW_PREVIEW (preview));
	g_return_if_fail (preview->priv != NULL);
	g_return_if_fail (preview->priv->updating_content != NULL);

	g_string_append (preview->priv->updating_content, "<TR><TD colspan=2><HR></TD></TR>");
}

void
e_web_view_preview_add_empty_line (EWebViewPreview *preview)
{
	g_return_if_fail (preview != NULL);
	g_return_if_fail (E_IS_WEB_VIEW_PREVIEW (preview));
	g_return_if_fail (preview->priv != NULL);
	g_return_if_fail (preview->priv->updating_content != NULL);

	g_string_append (preview->priv->updating_content, "<TR><TD colspan=2>&nbsp;</TD></TR>");
}

/* section can be NULL, but value cannot */
void
e_web_view_preview_add_section (EWebViewPreview *preview, const gchar *section, const gchar *value)
{
	gchar *escaped_section = NULL, *escaped_value;

	g_return_if_fail (preview != NULL);
	g_return_if_fail (E_IS_WEB_VIEW_PREVIEW (preview));
	g_return_if_fail (preview->priv != NULL);
	g_return_if_fail (preview->priv->updating_content != NULL);
	g_return_if_fail (value != NULL);

	if (section) {
		escaped_section = web_view_preview_escape_text (preview, section);
		if (escaped_section)
			section = escaped_section;
	}

	escaped_value = web_view_preview_escape_text (preview, value);
	if (escaped_value)
		value = escaped_value;

	g_string_append_printf (preview->priv->updating_content, "<TR><TD width=\"10%%\" valign=\"top\" nowrap><FONT size=\"3\"><B>%s</B></FONT></TD><TD width=\"90%%\"><FONT size=\"3\">%s</FONT></TD></TR>", section ? section : "", value);

	g_free (escaped_section);
	g_free (escaped_value);
}
