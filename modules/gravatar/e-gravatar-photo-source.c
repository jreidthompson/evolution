/*
 * e-gravatar-photo-source.c
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

#include "e-gravatar-photo-source.h"

#include <libsoup/soup.h>
#include <libsoup/soup-requester.h>
#include <libsoup/soup-request-http.h>

#define E_GRAVATAR_PHOTO_SOURCE_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_GRAVATAR_PHOTO_SOURCE, EGravatarPhotoSourcePrivate))

#define AVATAR_BASE_URI "http://www.gravatar.com/avatar/"

typedef struct _AsyncContext AsyncContext;

struct _AsyncContext {
	gchar *email_address;
	GInputStream *stream;
};

/* Forward Declarations */
static void	e_gravatar_photo_source_interface_init
					(EPhotoSourceInterface *interface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (
	EGravatarPhotoSource,
	e_gravatar_photo_source,
	G_TYPE_OBJECT,
	0,
	G_IMPLEMENT_INTERFACE_DYNAMIC (
		E_TYPE_PHOTO_SOURCE,
		e_gravatar_photo_source_interface_init))

static void
async_context_free (AsyncContext *async_context)
{
	g_free (async_context->email_address);
	g_clear_object (&async_context->stream);

	g_slice_free (AsyncContext, async_context);
}

static void
gravatar_photo_source_get_photo_thread (GSimpleAsyncResult *simple,
                                        GObject *source_object,
                                        GCancellable *cancellable)
{
	AsyncContext *async_context;
	SoupRequester *requester;
	SoupRequest *request;
	SoupSession *session;
	GInputStream *stream = NULL;
	gchar *hash;
	gchar *uri;
	GError *local_error = NULL;

	async_context = g_simple_async_result_get_op_res_gpointer (simple);

	hash = e_gravatar_get_hash (async_context->email_address);
	uri = g_strdup_printf ("%s%s?d=404", AVATAR_BASE_URI, hash);

	g_debug ("Requesting avatar for %s", async_context->email_address);
	g_debug ("%s", uri);

	session = soup_session_sync_new ();

	requester = soup_requester_new ();
	soup_session_add_feature (session, SOUP_SESSION_FEATURE (requester));

	/* We control the URI so there should be no error. */
	request = soup_requester_request (requester, uri, NULL);
	g_return_if_fail (request != NULL);

	stream = soup_request_send (request, cancellable, &local_error);

	/* Sanity check. */
	g_return_if_fail (
		((stream != NULL) && (local_error == NULL)) ||
		((stream == NULL) && (local_error != NULL)));

	/* XXX soup_request_send() returns a stream on HTTP errors.
	 *     We need to check the status code on the SoupMessage
	 *     to make sure the we're not getting an error message. */
	if (stream != NULL) {
		SoupMessage *message;

		message = soup_request_http_get_message (
			SOUP_REQUEST_HTTP (request));

		if (SOUP_STATUS_IS_SUCCESSFUL (message->status_code)) {
			async_context->stream = g_object_ref (stream);

		} else if (message->status_code != SOUP_STATUS_NOT_FOUND) {
			local_error = g_error_new_literal (
				SOUP_HTTP_ERROR,
				message->status_code,
				message->reason_phrase);
		}

		g_object_unref (message);
		g_object_unref (stream);
	}

	if (local_error != NULL) {
		const gchar *domain;

		domain = g_quark_to_string (local_error->domain);
		g_debug ("Error: %s (%s)", local_error->message, domain);
		g_simple_async_result_take_error (simple, local_error);
	}

	g_debug ("Request complete");

	g_clear_object (&requester);
	g_clear_object (&request);
	g_clear_object (&session);

	g_free (hash);
	g_free (uri);
}

static void
gravatar_photo_source_get_photo (EPhotoSource *photo_source,
                                 const gchar *email_address,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data)
{
	GSimpleAsyncResult *simple;
	AsyncContext *async_context;

	async_context = g_slice_new0 (AsyncContext);
	async_context->email_address = g_strdup (email_address);

	simple = g_simple_async_result_new (
		G_OBJECT (photo_source), callback,
		user_data, gravatar_photo_source_get_photo);

	g_simple_async_result_set_check_cancellable (simple, cancellable);

	g_simple_async_result_set_op_res_gpointer (
		simple, async_context, (GDestroyNotify) async_context_free);

	g_simple_async_result_run_in_thread (
		simple, gravatar_photo_source_get_photo_thread,
		G_PRIORITY_DEFAULT, cancellable);

	g_object_unref (simple);
}

static gboolean
gravatar_photo_source_get_photo_finish (EPhotoSource *photo_source,
                                        GAsyncResult *result,
                                        GInputStream **out_stream,
                                        gint *out_priority,
                                        GError **error)
{
	GSimpleAsyncResult *simple;
	AsyncContext *async_context;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (photo_source),
		gravatar_photo_source_get_photo), FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_context = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return FALSE;

	if (async_context->stream != NULL) {
		*out_stream = g_object_ref (async_context->stream);
		if (out_priority != NULL)
			*out_priority = G_PRIORITY_DEFAULT;
	} else {
		*out_stream = NULL;
	}

	return TRUE;
}

static void
e_gravatar_photo_source_class_init (EGravatarPhotoSourceClass *class)
{
}

static void
e_gravatar_photo_source_class_finalize (EGravatarPhotoSourceClass *class)
{
}

static void
e_gravatar_photo_source_interface_init (EPhotoSourceInterface *interface)
{
	interface->get_photo = gravatar_photo_source_get_photo;
	interface->get_photo_finish = gravatar_photo_source_get_photo_finish;
}

static void
e_gravatar_photo_source_init (EGravatarPhotoSource *photo_source)
{
}

void
e_gravatar_photo_source_type_register (GTypeModule *type_module)
{
	/* XXX G_DEFINE_DYNAMIC_TYPE declares a static type registration
	 *     function, so we have to wrap it with a public function in
	 *     order to register types from a separate compilation unit. */
	e_gravatar_photo_source_register_type (type_module);
}

EPhotoSource *
e_gravatar_photo_source_new (void)
{
	return g_object_new (E_TYPE_GRAVATAR_PHOTO_SOURCE, NULL);
}

gchar *
e_gravatar_get_hash (const gchar *email_address)
{
	gchar *string;
	gchar *hash;

	g_return_val_if_fail (email_address != NULL, NULL);
	g_return_val_if_fail (g_utf8_validate (email_address, -1, NULL), NULL);

	string = g_strstrip (g_utf8_strdown (email_address, -1));
	hash = g_compute_checksum_for_string (G_CHECKSUM_MD5, string, -1);
	g_free (string);

	return hash;
}

