/*
 * e-editor-web-extension.c
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
 */

#include "evolution-config.h"

#include <webkit2/webkit-web-extension.h>

#include "e-editor-web-extension.h"

struct _EEditorWebExtensionPrivate {
	WebKitWebExtension *wk_extension;
};

G_DEFINE_TYPE_WITH_PRIVATE (EEditorWebExtension, e_editor_web_extension, G_TYPE_OBJECT)

static void
e_editor_web_extension_dispose (GObject *object)
{
	EEditorWebExtension *extension = E_EDITOR_WEB_EXTENSION (object);

	g_clear_object (&extension->priv->wk_extension);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_editor_web_extension_parent_class)->dispose (object);
}

static void
e_editor_web_extension_class_init (EEditorWebExtensionClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);

	object_class->dispose = e_editor_web_extension_dispose;
}

static void
e_editor_web_extension_init (EEditorWebExtension *extension)
{
	extension->priv = e_editor_web_extension_get_instance_private (extension);
}

static gpointer
e_editor_web_extension_create_instance (gpointer data)
{
	return g_object_new (E_TYPE_EDITOR_WEB_EXTENSION, NULL);
}

EEditorWebExtension *
e_editor_web_extension_get_default (void)
{
	static GOnce once_init = G_ONCE_INIT;
	return E_EDITOR_WEB_EXTENSION (g_once (&once_init, e_editor_web_extension_create_instance, NULL));
}

static gboolean
web_page_send_request_cb (WebKitWebPage *web_page,
			  WebKitURIRequest *request,
			  WebKitURIResponse *redirected_response,
			  EEditorWebExtension *extension)
{
	const gchar *request_uri;
	const gchar *page_uri;

	request_uri = webkit_uri_request_get_uri (request);
	page_uri = webkit_web_page_get_uri (web_page);

	/* Always load the main resource. */
	if (g_strcmp0 (request_uri, page_uri) == 0)
		return FALSE;

	if (g_str_has_prefix (request_uri, "http:") ||
	    g_str_has_prefix (request_uri, "https:")) {
		gchar *new_uri;

		new_uri = g_strconcat ("evo-", request_uri, NULL);

		webkit_uri_request_set_uri (request, new_uri);

		g_free (new_uri);
	}

	return FALSE;
}

static gboolean
use_sources_js_file (void)
{
	static gint res = -1;

	if (res == -1)
		res = g_strcmp0 (g_getenv ("E_HTML_EDITOR_TEST_SOURCES"), "1") == 0 ? 1 : 0;

	return res;
}

static void
load_javascript_file (JSCContext *jsc_context,
		      const gchar *js_filename)
{
	JSCValue *result;
	JSCException *exception;
	gchar *content, *filename = NULL, *resource_uri;
	gsize length = 0;
	GError *error = NULL;

	g_return_if_fail (jsc_context != NULL);

	if (use_sources_js_file ()) {
		filename = g_build_filename (EVOLUTION_SOURCE_WEBKITDATADIR, js_filename, NULL);

		if (!g_file_test (filename, G_FILE_TEST_EXISTS)) {
			g_warning ("Cannot find '%s', using installed file '%s/%s' instead", filename, EVOLUTION_WEBKITDATADIR, js_filename);

			g_clear_pointer (&filename, g_free);
		}
	}

	if (!filename)
		filename = g_build_filename (EVOLUTION_WEBKITDATADIR, js_filename, NULL);

	if (!g_file_get_contents (filename, &content, &length, &error)) {
		g_warning ("Failed to load '%s': %s", filename, error ? error->message : "Unknown error");

		g_clear_error (&error);
		g_free (filename);

		return;
	}

	resource_uri = g_strconcat ("resource:///", js_filename, NULL);

	result = jsc_context_evaluate_with_source_uri (jsc_context, content, length, resource_uri, 1);

	g_free (resource_uri);

	exception = jsc_context_get_exception (jsc_context);

	if (exception) {
		g_warning ("Failed to call script '%s': %d:%d: %s",
			filename,
			jsc_exception_get_line_number (exception),
			jsc_exception_get_column_number (exception),
			jsc_exception_get_message (exception));

		jsc_context_clear_exception (jsc_context);
	}

	g_clear_object (&result);
	g_free (filename);
	g_free (content);
}

/* Returns 'null', when no match for the 'pattern' in 'text' found, otherwise
   returns an 'object { start : nnn, end : nnn };' with the first longest pattern match. */
static JSCValue *
evo_editor_jsc_find_pattern (const gchar *text,
			     const gchar *pattern,
			     JSCContext *jsc_context)
{
	JSCValue *object = NULL;
	GRegex *regex;

	if (!text || !*text || !pattern || !*pattern)
		return jsc_value_new_null (jsc_context);

	regex = g_regex_new (pattern, 0, 0, NULL);
	if (regex) {
		GMatchInfo *match_info = NULL;
		gint start = -1, end = -1;

		if (g_regex_match_all (regex, text, G_REGEX_MATCH_NOTEMPTY, &match_info) &&
		    g_match_info_fetch_pos (match_info, 0, &start, &end) &&
		    start >= 0 && end >= 0) {
			JSCValue *number;

			object = jsc_value_new_object (jsc_context, NULL, NULL);

			number = jsc_value_new_number (jsc_context, start);
			jsc_value_object_set_property (object, "start", number);
			g_clear_object (&number);

			number = jsc_value_new_number (jsc_context, end);
			jsc_value_object_set_property (object, "end", number);
			g_clear_object (&number);
		}

		if (match_info)
			g_match_info_free (match_info);
		g_regex_unref (regex);
	}

	return object ? object : jsc_value_new_null (jsc_context);
}

static void
window_object_cleared_cb (WebKitScriptWorld *world,
			  WebKitWebPage *page,
			  WebKitFrame *frame,
			  gpointer user_data)
{
	JSCContext *jsc_context;
	JSCValue *jsc_editor;

	/* Load the javascript files only to the main frame, not to the subframes */
	if (!webkit_frame_is_main_frame (frame))
		return;

	jsc_context = webkit_frame_get_js_context (frame);

	/* Read in order approximately as each other uses the previous */
	load_javascript_file (jsc_context, "e-convert.js");
	load_javascript_file (jsc_context, "e-selection.js");
	load_javascript_file (jsc_context, "e-undo-redo.js");
	load_javascript_file (jsc_context, "e-editor.js");

	jsc_editor = jsc_context_get_value (jsc_context, "EvoEditor");

	if (jsc_editor) {
		JSCValue *jsc_function;

		jsc_function = jsc_value_new_function (jsc_context, "findPattern",
			G_CALLBACK (evo_editor_jsc_find_pattern), g_object_ref (jsc_context), g_object_unref,
			JSC_TYPE_VALUE, 2, G_TYPE_STRING, G_TYPE_STRING);

		jsc_value_object_set_property (jsc_editor, "findPattern", jsc_function);

		g_clear_object (&jsc_function);
		g_clear_object (&jsc_editor);
	}

	g_clear_object (&jsc_context);
}

static void
web_page_document_loaded_cb (WebKitWebPage *web_page,
			     gpointer user_data)
{
	g_return_if_fail (WEBKIT_IS_WEB_PAGE (web_page));

	window_object_cleared_cb (NULL, web_page, webkit_web_page_get_main_frame (web_page), NULL);
}

static void
web_page_created_cb (WebKitWebExtension *wk_extension,
		     WebKitWebPage *web_page,
		     EEditorWebExtension *extension)
{
	g_return_if_fail (WEBKIT_IS_WEB_PAGE (web_page));
	g_return_if_fail (E_IS_EDITOR_WEB_EXTENSION (extension));

	window_object_cleared_cb (NULL, web_page, webkit_web_page_get_main_frame (web_page), NULL);

	g_signal_connect (
		web_page, "send-request",
		G_CALLBACK (web_page_send_request_cb), extension);

	g_signal_connect (
		web_page, "document-loaded",
		G_CALLBACK (web_page_document_loaded_cb), NULL);
}

void
e_editor_web_extension_initialize (EEditorWebExtension *extension,
				   WebKitWebExtension *wk_extension)
{
	WebKitScriptWorld *script_world;

	g_return_if_fail (E_IS_EDITOR_WEB_EXTENSION (extension));

	extension->priv->wk_extension = g_object_ref (wk_extension);

	g_signal_connect (
		wk_extension, "page-created",
		G_CALLBACK (web_page_created_cb), extension);

	script_world = webkit_script_world_get_default ();

	g_signal_connect (script_world, "window-object-cleared",
		G_CALLBACK (window_object_cleared_cb), NULL);
}
