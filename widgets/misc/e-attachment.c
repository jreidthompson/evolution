/*
 * e-attachment.c
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

#include "e-attachment.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <camel/camel-iconv.h>
#include <camel/camel-data-wrapper.h>
#include <camel/camel-mime-message.h>
#include <camel/camel-stream-filter.h>
#include <camel/camel-stream-mem.h>
#include <camel/camel-stream-null.h>
#include <camel/camel-stream-vfs.h>

#include "e-util/e-util.h"

#define E_ATTACHMENT_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_ATTACHMENT, EAttachmentPrivate))

/* Emblems */
#define EMBLEM_CANCELLED	"gtk-cancel"
#define EMBLEM_LOADING		"emblem-downloads"
#define EMBLEM_SAVING		"document-save"
#define EMBLEM_ENCRYPT_WEAK	"security-low"
#define EMBLEM_ENCRYPT_STRONG	"security-high"
#define EMBLEM_ENCRYPT_UNKNOWN	"security-medium"
#define EMBLEM_SIGN_BAD		"stock_signature_bad"
#define EMBLEM_SIGN_GOOD	"stock_signature-ok"
#define EMBLEM_SIGN_UNKNOWN	"stock_signature"

/* Attributes needed by EAttachmentStore, et al. */
#define ATTACHMENT_QUERY "standard::*,preview::*,thumbnail::*"

struct _EAttachmentPrivate {
	GFile *file;
	GFileInfo *file_info;
	GCancellable *cancellable;
	CamelMimePart *mime_part;
	guint emblem_timeout_id;
	gchar *disposition;
	gint percent;

	guint loading : 1;
	guint saving  : 1;

	camel_cipher_validity_encrypt_t encrypted;
	camel_cipher_validity_sign_t signed_;

	/* This is a reference to our row in an EAttachmentStore,
	 * serving as a means of broadcasting "row-changed" signals.
	 * If we are removed from the store, we lazily free the
	 * reference when it is found to be to be invalid. */
	GtkTreeRowReference *reference;
};

enum {
	PROP_0,
	PROP_DISPOSITION,
	PROP_ENCRYPTED,
	PROP_FILE,
	PROP_FILE_INFO,
	PROP_LOADING,
	PROP_MIME_PART,
	PROP_PERCENT,
	PROP_REFERENCE,
	PROP_SAVING,
	PROP_SIGNED
};

static gpointer parent_class;

static gchar *
attachment_get_default_charset (void)
{
	GConfClient *client;
	const gchar *key;
	gchar *charset;

	/* XXX This doesn't really belong here. */

	client = gconf_client_get_default ();
	key = "/apps/evolution/mail/composer/charset";
	charset = gconf_client_get_string (client, key, NULL);
	if (charset == NULL || *charset == '\0') {
		g_free (charset);
		key = "/apps/evolution/mail/format/charset";
		charset = gconf_client_get_string (client, key, NULL);
		if (charset == NULL || *charset == '\0') {
			g_free (charset);
			charset = NULL;
		}
	}
	g_object_unref (client);

	if (charset == NULL)
		charset = g_strdup (camel_iconv_locale_charset ());

	if (charset == NULL)
		charset = g_strdup ("us-ascii");

	return charset;
}

static void
attachment_notify_model (EAttachment *attachment)
{
	GtkTreeRowReference *reference;
	GtkTreeModel *model;
	GtkTreePath *path;
	GtkTreeIter iter;

	reference = e_attachment_get_reference (attachment);

	if (reference == NULL)
		return;

	model = gtk_tree_row_reference_get_model (reference);
	path = gtk_tree_row_reference_get_path (reference);

	gtk_tree_model_get_iter (model, &iter, path);
	gtk_tree_model_row_changed (model, path, &iter);

	gtk_tree_path_free (path);
}

static void
attachment_set_file_info (EAttachment *attachment,
                          GFileInfo *file_info)
{
	GtkTreeRowReference *reference;

	reference = e_attachment_get_reference (attachment);

	if (file_info != NULL)
		g_object_ref (file_info);

	if (attachment->priv->file_info != NULL)
		g_object_unref (attachment->priv->file_info);

	attachment->priv->file_info = file_info;

	g_object_notify (G_OBJECT (attachment), "file-info");

	/* Tell the EAttachmentStore its total size changed. */
	if (reference != NULL) {
		GtkTreeModel *model;
		model = gtk_tree_row_reference_get_model (reference);
		g_object_notify (G_OBJECT (model), "total-size");
	}

	attachment_notify_model (attachment);
}

static void
attachment_set_loading (EAttachment *attachment,
                        gboolean loading)
{
	GtkTreeRowReference *reference;

	reference = e_attachment_get_reference (attachment);

	attachment->priv->percent = 0;
	attachment->priv->loading = loading;

	g_object_freeze_notify (G_OBJECT (attachment));
	g_object_notify (G_OBJECT (attachment), "percent");
	g_object_notify (G_OBJECT (attachment), "loading");
	g_object_thaw_notify (G_OBJECT (attachment));

	if (reference != NULL) {
		GtkTreeModel *model;
		model = gtk_tree_row_reference_get_model (reference);
		g_object_notify (G_OBJECT (model), "num-loading");
	}

	attachment_notify_model (attachment);
}

static void
attachment_set_saving (EAttachment *attachment,
                       gboolean saving)
{
	attachment->priv->percent = 0;
	attachment->priv->saving = saving;

	g_object_freeze_notify (G_OBJECT (attachment));
	g_object_notify (G_OBJECT (attachment), "percent");
	g_object_notify (G_OBJECT (attachment), "saving");
	g_object_thaw_notify (G_OBJECT (attachment));

	attachment_notify_model (attachment);
}

static void
attachment_progress_cb (goffset current_num_bytes,
                        goffset total_num_bytes,
                        EAttachment *attachment)
{
	attachment->priv->percent =
		(current_num_bytes * 100) / total_num_bytes;

	g_object_notify (G_OBJECT (attachment), "percent");

	attachment_notify_model (attachment);
}

static gboolean
attachment_cancelled_timeout_cb (EAttachment *attachment)
{
	attachment->priv->emblem_timeout_id = 0;
	g_cancellable_reset (attachment->priv->cancellable);

	attachment_notify_model (attachment);

	return FALSE;
}

static void
attachment_cancelled_cb (EAttachment *attachment)
{
	/* Reset the GCancellable after one second.  This causes a
	 * cancel emblem to be briefly shown on the attachment icon
	 * as visual feedback that an operation was cancelled. */

	if (attachment->priv->emblem_timeout_id > 0)
		g_source_remove (attachment->priv->emblem_timeout_id);

	attachment->priv->emblem_timeout_id = g_timeout_add_seconds (
		1, (GSourceFunc) attachment_cancelled_timeout_cb, attachment);
}

static void
attachment_set_property (GObject *object,
                         guint property_id,
                         const GValue *value,
                         GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_DISPOSITION:
			e_attachment_set_disposition (
				E_ATTACHMENT (object),
				g_value_get_string (value));
			return;

		case PROP_ENCRYPTED:
			e_attachment_set_encrypted (
				E_ATTACHMENT (object),
				g_value_get_int (value));
			return;

		case PROP_FILE:
			e_attachment_set_file (
				E_ATTACHMENT (object),
				g_value_get_object (value));
			return;

		case PROP_MIME_PART:
			e_attachment_set_mime_part (
				E_ATTACHMENT (object),
				g_value_get_boxed (value));
			return;

		case PROP_REFERENCE:
			e_attachment_set_reference (
				E_ATTACHMENT (object),
				g_value_get_boxed (value));
			return;

		case PROP_SIGNED:
			e_attachment_set_signed (
				E_ATTACHMENT (object),
				g_value_get_int (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
attachment_get_property (GObject *object,
                         guint property_id,
                         GValue *value,
                         GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_DISPOSITION:
			g_value_set_string (
				value, e_attachment_get_disposition (
				E_ATTACHMENT (object)));
			return;

		case PROP_ENCRYPTED:
			g_value_set_int (
				value, e_attachment_get_encrypted (
				E_ATTACHMENT (object)));
			return;

		case PROP_FILE:
			g_value_set_object (
				value, e_attachment_get_file (
				E_ATTACHMENT (object)));
			return;

		case PROP_FILE_INFO:
			g_value_set_object (
				value, e_attachment_get_file_info (
				E_ATTACHMENT (object)));
			return;

		case PROP_LOADING:
			g_value_set_boolean (
				value, e_attachment_get_loading (
				E_ATTACHMENT (object)));
			return;

		case PROP_MIME_PART:
			g_value_set_boxed (
				value, e_attachment_get_mime_part (
				E_ATTACHMENT (object)));
			return;

		case PROP_PERCENT:
			g_value_set_int (
				value, e_attachment_get_percent (
				E_ATTACHMENT (object)));
			return;

		case PROP_REFERENCE:
			g_value_set_boxed (
				value, e_attachment_get_reference (
				E_ATTACHMENT (object)));
			return;

		case PROP_SAVING:
			g_value_set_boolean (
				value, e_attachment_get_saving (
				E_ATTACHMENT (object)));
			return;

		case PROP_SIGNED:
			g_value_set_int (
				value, e_attachment_get_signed (
				E_ATTACHMENT (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
attachment_dispose (GObject *object)
{
	EAttachmentPrivate *priv;

	priv = E_ATTACHMENT_GET_PRIVATE (object);

	if (priv->file != NULL) {
		g_object_unref (priv->file);
		priv->file = NULL;
	}

	if (priv->file_info != NULL) {
		g_object_unref (priv->file_info);
		priv->file_info = NULL;
	}

	if (priv->cancellable != NULL) {
		g_object_unref (priv->cancellable);
		priv->cancellable = NULL;
	}

	if (priv->mime_part != NULL) {
		camel_object_unref (priv->mime_part);
		priv->mime_part = NULL;
	}

	if (priv->emblem_timeout_id > 0) {
		g_source_remove (priv->emblem_timeout_id);
		priv->emblem_timeout_id = 0;
	}

	/* This accepts NULL arguments. */
	gtk_tree_row_reference_free (priv->reference);
	priv->reference = NULL;

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
attachment_finalize (GObject *object)
{
	EAttachmentPrivate *priv;

	priv = E_ATTACHMENT_GET_PRIVATE (object);

	g_free (priv->disposition);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
attachment_class_init (EAttachmentClass *class)
{
	GObjectClass *object_class;

	parent_class = g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (EAttachmentPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = attachment_set_property;
	object_class->get_property = attachment_get_property;
	object_class->dispose = attachment_dispose;
	object_class->finalize = attachment_finalize;

	g_object_class_install_property (
		object_class,
		PROP_DISPOSITION,
		g_param_spec_string (
			"disposition",
			"Disposition",
			NULL,
			"attachment",
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	/* FIXME Define a GEnumClass for this. */
	g_object_class_install_property (
		object_class,
		PROP_ENCRYPTED,
		g_param_spec_int (
			"encrypted",
			"Encrypted",
			NULL,
			CAMEL_CIPHER_VALIDITY_ENCRYPT_NONE,
			CAMEL_CIPHER_VALIDITY_ENCRYPT_STRONG,
			CAMEL_CIPHER_VALIDITY_ENCRYPT_NONE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	g_object_class_install_property (
		object_class,
		PROP_FILE,
		g_param_spec_object (
			"file",
			"File",
			NULL,
			G_TYPE_FILE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));

	g_object_class_install_property (
		object_class,
		PROP_FILE_INFO,
		g_param_spec_object (
			"file-info",
			"File Info",
			NULL,
			G_TYPE_FILE_INFO,
			G_PARAM_READABLE));

	g_object_class_install_property (
		object_class,
		PROP_LOADING,
		g_param_spec_boolean (
			"loading",
			"Loading",
			NULL,
			FALSE,
			G_PARAM_READABLE));

	g_object_class_install_property (
		object_class,
		PROP_MIME_PART,
		g_param_spec_boxed (
			"mime-part",
			"MIME Part",
			NULL,
			E_TYPE_CAMEL_OBJECT,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_PERCENT,
		g_param_spec_int (
			"percent",
			"Percent",
			NULL,
			0,
			100,
			0,
			G_PARAM_READABLE));

	g_object_class_install_property (
		object_class,
		PROP_REFERENCE,
		g_param_spec_boxed (
			"reference",
			"Reference",
			NULL,
			GTK_TYPE_TREE_ROW_REFERENCE,
			G_PARAM_READWRITE));

	g_object_class_install_property (
		object_class,
		PROP_SAVING,
		g_param_spec_boolean (
			"saving",
			"Saving",
			NULL,
			FALSE,
			G_PARAM_READABLE));

	/* FIXME Define a GEnumClass for this. */
	g_object_class_install_property (
		object_class,
		PROP_SIGNED,
		g_param_spec_int (
			"signed",
			"Signed",
			NULL,
			CAMEL_CIPHER_VALIDITY_SIGN_NONE,
			CAMEL_CIPHER_VALIDITY_SIGN_NEED_PUBLIC_KEY,
			CAMEL_CIPHER_VALIDITY_SIGN_NONE,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT));
}

static void
attachment_init (EAttachment *attachment)
{
	attachment->priv = E_ATTACHMENT_GET_PRIVATE (attachment);
	attachment->priv->cancellable = g_cancellable_new ();
	attachment->priv->encrypted = CAMEL_CIPHER_VALIDITY_ENCRYPT_NONE;
	attachment->priv->signed_ = CAMEL_CIPHER_VALIDITY_SIGN_NONE;

	g_signal_connect_swapped (
		attachment->priv->cancellable, "cancelled",
		G_CALLBACK (attachment_cancelled_cb), attachment);
}

GType
e_attachment_get_type (void)
{
	static GType type = 0;

	if (G_UNLIKELY (type == 0)) {
		static const GTypeInfo type_info = {
			sizeof (EAttachmentClass),
			(GBaseInitFunc) NULL,
			(GBaseFinalizeFunc) NULL,
			(GClassInitFunc) attachment_class_init,
			(GClassFinalizeFunc) NULL,
			NULL,  /* class_data */
			sizeof (EAttachment),
			0,     /* n_preallocs */
			(GInstanceInitFunc) attachment_init,
			NULL   /* value_table */
		};

		type = g_type_register_static (
			G_TYPE_OBJECT, "EAttachment", &type_info, 0);
	}

	return type;
}

EAttachment *
e_attachment_new (void)
{
	return g_object_new (E_TYPE_ATTACHMENT, NULL);
}

EAttachment *
e_attachment_new_for_path (const gchar *path)
{
	EAttachment *attachment;
	GFile *file;

	g_return_val_if_fail (path != NULL, NULL);

	file = g_file_new_for_path (path);
	attachment = g_object_new (E_TYPE_ATTACHMENT, "file", file, NULL);
	g_object_unref (file);

	return attachment;
}

EAttachment *
e_attachment_new_for_uri (const gchar *uri)
{
	EAttachment *attachment;
	GFile *file;

	g_return_val_if_fail (uri != NULL, NULL);

	file = g_file_new_for_uri (uri);
	attachment = g_object_new (E_TYPE_ATTACHMENT, "file", file, NULL);
	g_object_unref (file);

	return attachment;
}

EAttachment *
e_attachment_new_for_message (CamelMimeMessage *message)
{
	CamelDataWrapper *wrapper;
	CamelMimePart *mime_part;
	EAttachment *attachment;
	GString *description;
	const gchar *subject;

	g_return_val_if_fail (CAMEL_IS_MIME_MESSAGE (message), NULL);

	mime_part = camel_mime_part_new ();
	camel_mime_part_set_disposition (mime_part, "inline");
	subject = camel_mime_message_get_subject (message);

	description = g_string_new (_("Attached message"));
	if (subject != NULL)
		g_string_append_printf (description, " - %s", subject);
	camel_mime_part_set_description (mime_part, description->str);
	g_string_free (description, TRUE);

	wrapper = CAMEL_DATA_WRAPPER (message);
	camel_medium_set_content_object (CAMEL_MEDIUM (mime_part), wrapper);
	camel_mime_part_set_content_type (mime_part, "message/rfc822");

	attachment = e_attachment_new ();
	e_attachment_set_mime_part (attachment, mime_part);
	camel_object_unref (mime_part);

	return attachment;
}

void
e_attachment_add_to_multipart (EAttachment *attachment,
                               CamelMultipart *multipart,
                               const gchar *default_charset)
{
	CamelContentType *content_type;
	CamelDataWrapper *wrapper;
	CamelMimePart *mime_part;

	/* XXX EMsgComposer might be a better place for this function. */

	g_return_if_fail (E_IS_ATTACHMENT (attachment));
	g_return_if_fail (CAMEL_IS_MULTIPART (multipart));

	/* Still loading?  Too bad. */
	mime_part = e_attachment_get_mime_part (attachment);
	if (mime_part == NULL)
		return;

	content_type = camel_mime_part_get_content_type (mime_part);
	wrapper = camel_medium_get_content_object (CAMEL_MEDIUM (mime_part));

	if (CAMEL_IS_MULTIPART (wrapper))
		goto exit;

	/* For text content, determine the best encoding and character set. */
	if (camel_content_type_is (content_type, "text", "*")) {
		CamelTransferEncoding encoding;
		CamelStreamFilter *filtered_stream;
		CamelMimeFilterBestenc *filter;
		CamelStream *stream;
		const gchar *charset;

		charset = camel_content_type_param (content_type, "charset");

		/* Determine the best encoding by writing the MIME
		 * part to a NULL stream with a "bestenc" filter. */
		stream = camel_stream_null_new ();
		filtered_stream = camel_stream_filter_new_with_stream (stream);
		filter = camel_mime_filter_bestenc_new (
			CAMEL_BESTENC_GET_ENCODING);
		camel_stream_filter_add (
			filtered_stream, CAMEL_MIME_FILTER (filter));
		camel_data_wrapper_decode_to_stream (
			wrapper, CAMEL_STREAM (filtered_stream));
		camel_object_unref (filtered_stream);
		camel_object_unref (stream);

		/* Retrieve the best encoding from the filter. */
		encoding = camel_mime_filter_bestenc_get_best_encoding (
			filter, CAMEL_BESTENC_8BIT);
		camel_mime_part_set_encoding (mime_part, encoding);
		camel_object_unref (filter);

		if (encoding == CAMEL_TRANSFER_ENCODING_7BIT) {
			/* The text fits within us-ascii, so this is safe.
			 * FIXME Check that this isn't iso-2022-jp? */
			default_charset = "us-ascii";

		} else if (charset == NULL && default_charset == NULL) {
			default_charset = attachment_get_default_charset ();
			/* FIXME Check that this fits within the
			 *       default_charset and if not, find one
			 *       that does and/or allow the user to
			 *       specify. */
		}

		if (charset == NULL) {
			gchar *type;

			camel_content_type_set_param (
				content_type, "charset", default_charset);
			type = camel_content_type_format (content_type);
			camel_mime_part_set_content_type (mime_part, type);
			g_free (type);
		}

	/* Otherwise, unless it's a message/rfc822, Base64 encode it. */
	} else if (!CAMEL_IS_MIME_MESSAGE (wrapper))
		camel_mime_part_set_encoding (
			mime_part, CAMEL_TRANSFER_ENCODING_BASE64);

exit:
	camel_multipart_add_part (multipart, mime_part);
}

void
e_attachment_cancel (EAttachment *attachment)
{
	g_return_if_fail (E_IS_ATTACHMENT (attachment));

	g_cancellable_cancel (attachment->priv->cancellable);
}

const gchar *
e_attachment_get_disposition (EAttachment *attachment)
{
	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), NULL);

	return attachment->priv->disposition;
}

void
e_attachment_set_disposition (EAttachment *attachment,
                              const gchar *disposition)
{
	g_return_if_fail (E_IS_ATTACHMENT (attachment));

	g_free (attachment->priv->disposition);
	attachment->priv->disposition = g_strdup (disposition);

	g_object_notify (G_OBJECT (attachment), "disposition");
}

GFile *
e_attachment_get_file (EAttachment *attachment)
{
	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), NULL);

	return attachment->priv->file;
}

void
e_attachment_set_file (EAttachment *attachment,
                       GFile *file)
{
	g_return_if_fail (E_IS_ATTACHMENT (attachment));

	if (file != NULL) {
		g_return_if_fail (G_IS_FILE (file));
		g_object_ref (file);
	}

	if (attachment->priv->file != NULL)
		g_object_unref (attachment->priv->file);

	attachment->priv->file = file;

	g_object_notify (G_OBJECT (attachment), "file");
}

GFileInfo *
e_attachment_get_file_info (EAttachment *attachment)
{
	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), NULL);

	return attachment->priv->file_info;
}

gboolean
e_attachment_get_loading (EAttachment *attachment)
{
	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), FALSE);

	return attachment->priv->loading;
}

CamelMimePart *
e_attachment_get_mime_part (EAttachment *attachment)
{
	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), NULL);

	return attachment->priv->mime_part;
}

void
e_attachment_set_mime_part (EAttachment *attachment,
                            CamelMimePart *mime_part)
{
	g_return_if_fail (E_IS_ATTACHMENT (attachment));

	if (mime_part != NULL) {
		g_return_if_fail (CAMEL_IS_MIME_PART (mime_part));
		camel_object_ref (mime_part);
	}

	if (attachment->priv->mime_part != NULL)
		camel_object_unref (attachment->priv->mime_part);

	attachment->priv->mime_part = mime_part;

	g_object_notify (G_OBJECT (attachment), "mime-part");
}

gint
e_attachment_get_percent (EAttachment *attachment)
{
	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), 0);

	return attachment->priv->percent;
}

GtkTreeRowReference *
e_attachment_get_reference (EAttachment *attachment)
{
	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), NULL);

	/* Don't return an invalid tree row reference. */
	if (!gtk_tree_row_reference_valid (attachment->priv->reference))
		e_attachment_set_reference (attachment, NULL);

	return attachment->priv->reference;
}

void
e_attachment_set_reference (EAttachment *attachment,
                            GtkTreeRowReference *reference)
{
	g_return_if_fail (E_IS_ATTACHMENT (attachment));

	if (reference != NULL)
		reference = gtk_tree_row_reference_copy (reference);

	gtk_tree_row_reference_free (attachment->priv->reference);
	attachment->priv->reference = reference;

	g_object_notify (G_OBJECT (attachment), "reference");
}

gboolean
e_attachment_get_saving (EAttachment *attachment)
{
	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), FALSE);

	return attachment->priv->saving;
}

camel_cipher_validity_encrypt_t
e_attachment_get_encrypted (EAttachment *attachment)
{
	g_return_val_if_fail (
		E_IS_ATTACHMENT (attachment),
		CAMEL_CIPHER_VALIDITY_ENCRYPT_NONE);

	return attachment->priv->encrypted;
}

void
e_attachment_set_encrypted (EAttachment *attachment,
                            camel_cipher_validity_encrypt_t encrypted)
{
	g_return_if_fail (E_IS_ATTACHMENT (attachment));

	attachment->priv->encrypted = encrypted;

	g_object_notify (G_OBJECT (attachment), "encrypted");
	attachment_notify_model (attachment);
}

camel_cipher_validity_sign_t
e_attachment_get_signed (EAttachment *attachment)
{
	g_return_val_if_fail (
		E_IS_ATTACHMENT (attachment),
		CAMEL_CIPHER_VALIDITY_SIGN_NONE);

	return attachment->priv->signed_;
}

void
e_attachment_set_signed (EAttachment *attachment,
                         camel_cipher_validity_sign_t signed_)
{
	g_return_if_fail (E_IS_ATTACHMENT (attachment));

	attachment->priv->signed_ = signed_;

	g_object_notify (G_OBJECT (attachment), "signed");
	attachment_notify_model (attachment);
}

const gchar *
e_attachment_get_description (EAttachment *attachment)
{
	GFileInfo *file_info;
	const gchar *attribute;

	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), NULL);

	attribute = G_FILE_ATTRIBUTE_STANDARD_DESCRIPTION;
	file_info = e_attachment_get_file_info (attachment);

	if (file_info == NULL)
		return NULL;

	return g_file_info_get_attribute_string (file_info, attribute);
}

const gchar *
e_attachment_get_thumbnail_path (EAttachment *attachment)
{
	GFileInfo *file_info;
	const gchar *attribute;

	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), NULL);

	attribute = G_FILE_ATTRIBUTE_THUMBNAIL_PATH;
	file_info = e_attachment_get_file_info (attachment);

	if (file_info == NULL)
		return NULL;

	return g_file_info_get_attribute_byte_string (file_info, attribute);
}

gboolean
e_attachment_is_image (EAttachment *attachment)
{
	GFileInfo *file_info;
	const gchar *content_type;

	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), FALSE);

	file_info = e_attachment_get_file_info (attachment);
	if (file_info == NULL)
		return FALSE;

	content_type = g_file_info_get_content_type (file_info);
	if (content_type == NULL)
		return FALSE;

	return g_content_type_is_a (content_type, "image");
}

gboolean
e_attachment_is_rfc822 (EAttachment *attachment)
{
	GFileInfo *file_info;
	const gchar *content_type;

	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), FALSE);

	file_info = e_attachment_get_file_info (attachment);
	if (file_info == NULL)
		return FALSE;

	content_type = g_file_info_get_content_type (file_info);
	if (content_type == NULL)
		return FALSE;

	return g_content_type_equals (content_type, "message/rfc822");
}

GList *
e_attachment_list_apps (EAttachment *attachment)
{
	GList *app_info_list;
	GFileInfo *file_info;
	const gchar *content_type;
	const gchar *display_name;
	gchar *allocated;

	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), NULL);

	file_info = e_attachment_get_file_info (attachment);
	g_return_val_if_fail (file_info != NULL, NULL);

	content_type = g_file_info_get_content_type (file_info);
	display_name = g_file_info_get_display_name (file_info);
	g_return_val_if_fail (content_type != NULL, NULL);

	app_info_list = g_app_info_get_all_for_type (content_type);

	if (app_info_list != NULL || display_name == NULL)
		goto exit;

	if (!g_content_type_is_unknown (content_type))
		goto exit;

	allocated = g_content_type_guess (display_name, NULL, 0, NULL);
	app_info_list = g_app_info_get_all_for_type (allocated);
	g_free (allocated);

exit:
	return app_info_list;
}

GList *
e_attachment_list_emblems (EAttachment *attachment)
{
	GCancellable *cancellable;
	GList *list = NULL;
	GIcon *icon;

	g_return_val_if_fail (E_IS_ATTACHMENT (attachment), NULL);

	cancellable = attachment->priv->cancellable;

	if (g_cancellable_is_cancelled (cancellable)) {
		icon = g_themed_icon_new (EMBLEM_CANCELLED);
		list = g_list_append (list, g_emblem_new (icon));
		g_object_unref (icon);
	}

	if (e_attachment_get_loading (attachment)) {
		icon = g_themed_icon_new (EMBLEM_LOADING);
		list = g_list_append (list, g_emblem_new (icon));
		g_object_unref (icon);
	}

	if (e_attachment_get_saving (attachment)) {
		icon = g_themed_icon_new (EMBLEM_SAVING);
		list = g_list_append (list, g_emblem_new (icon));
		g_object_unref (icon);
	}

	switch (e_attachment_get_encrypted (attachment)) {
		case CAMEL_CIPHER_VALIDITY_ENCRYPT_WEAK:
			icon = g_themed_icon_new (EMBLEM_ENCRYPT_WEAK);
			list = g_list_append (list, g_emblem_new (icon));
			g_object_unref (icon);
			break;

		case CAMEL_CIPHER_VALIDITY_ENCRYPT_ENCRYPTED:
			icon = g_themed_icon_new (EMBLEM_ENCRYPT_UNKNOWN);
			list = g_list_append (list, g_emblem_new (icon));
			g_object_unref (icon);
			break;

		case CAMEL_CIPHER_VALIDITY_ENCRYPT_STRONG:
			icon = g_themed_icon_new (EMBLEM_ENCRYPT_STRONG);
			list = g_list_append (list, g_emblem_new (icon));
			g_object_unref (icon);
			break;

		default:
			break;
	}

	switch (e_attachment_get_signed (attachment)) {
		case CAMEL_CIPHER_VALIDITY_SIGN_GOOD:
			icon = g_themed_icon_new (EMBLEM_SIGN_GOOD);
			list = g_list_append (list, g_emblem_new (icon));
			g_object_unref (icon);
			break;

		case CAMEL_CIPHER_VALIDITY_SIGN_BAD:
			icon = g_themed_icon_new (EMBLEM_SIGN_BAD);
			list = g_list_append (list, g_emblem_new (icon));
			g_object_unref (icon);
			break;

		case CAMEL_CIPHER_VALIDITY_SIGN_UNKNOWN:
		case CAMEL_CIPHER_VALIDITY_SIGN_NEED_PUBLIC_KEY:
			icon = g_themed_icon_new (EMBLEM_SIGN_UNKNOWN);
			list = g_list_append (list, g_emblem_new (icon));
			g_object_unref (icon);
			break;

		default:
			break;
	}

	return list;
}

/************************* e_attachment_load_async() *************************/

typedef struct _AttachmentLoadContext AttachmentLoadContext;

struct _AttachmentLoadContext {
	EAttachment *attachment;
	GSimpleAsyncResult *simple;
	GInputStream *input_stream;
	GOutputStream *output_stream;
	GFileInfo *file_info;
	goffset total_num_bytes;
	gssize bytes_read;
	gchar buffer[4096];
};

/* Forward Declaration */
static void
attachment_load_stream_read_cb (GInputStream *input_stream,
                                GAsyncResult *result,
                                AttachmentLoadContext *load_context);

static AttachmentLoadContext *
attachment_load_context_new (EAttachment *attachment,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
	AttachmentLoadContext *load_context;
	GSimpleAsyncResult *simple;

	simple = g_simple_async_result_new (
		G_OBJECT (attachment), callback,
		user_data, e_attachment_load_async);

	load_context = g_slice_new0 (AttachmentLoadContext);
	load_context->attachment = g_object_ref (attachment);
	load_context->simple = simple;

	attachment_set_loading (load_context->attachment, TRUE);

	return load_context;
}

static void
attachment_load_context_free (AttachmentLoadContext *load_context)
{
	/* Do not free the GSimpleAsyncResult. */
	g_object_unref (load_context->attachment);

	if (load_context->input_stream != NULL)
		g_object_unref (load_context->input_stream);

	if (load_context->output_stream != NULL)
		g_object_unref (load_context->output_stream);

	if (load_context->file_info != NULL)
		g_object_unref (load_context->file_info);

	g_slice_free (AttachmentLoadContext, load_context);
}

static void
attachment_load_finish (AttachmentLoadContext *load_context)
{
	GFileInfo *file_info;
	EAttachment *attachment;
	GMemoryOutputStream *output_stream;
	GSimpleAsyncResult *simple;
	CamelDataWrapper *wrapper;
	CamelMimePart *mime_part;
	CamelStream *stream;
	const gchar *attribute;
	const gchar *content_type;
	const gchar *display_name;
	const gchar *description;
	const gchar *disposition;
	gchar *mime_type;
	gpointer data;
	gsize size;

	/* Steal the reference. */
	simple = load_context->simple;
	load_context->simple = NULL;

	file_info = load_context->file_info;
	attachment = load_context->attachment;
	output_stream = G_MEMORY_OUTPUT_STREAM (load_context->output_stream);

	if (e_attachment_is_rfc822 (attachment))
		wrapper = (CamelDataWrapper *) camel_mime_message_new ();
	else
		wrapper = camel_data_wrapper_new ();

	content_type = g_file_info_get_content_type (file_info);
	mime_type = g_content_type_get_mime_type (content_type);

	data = g_memory_output_stream_get_data (output_stream);
	size = g_memory_output_stream_get_data_size (output_stream);

	stream = camel_stream_mem_new_with_buffer (data, size);
	camel_data_wrapper_construct_from_stream (wrapper, stream);
	camel_data_wrapper_set_mime_type (wrapper, mime_type);
	camel_stream_close (stream);
	camel_object_unref (stream);

	mime_part = camel_mime_part_new ();
	camel_medium_set_content_object (CAMEL_MEDIUM (mime_part), wrapper);

	camel_object_unref (wrapper);
	g_free (mime_type);

	display_name = g_file_info_get_display_name (file_info);
	if (display_name != NULL)
		camel_mime_part_set_filename (mime_part, display_name);

	attribute = G_FILE_ATTRIBUTE_STANDARD_DESCRIPTION;
	description = g_file_info_get_attribute_string (file_info, attribute);
	if (description != NULL)
		camel_mime_part_set_description (mime_part, description);

	disposition = e_attachment_get_disposition (attachment);
	if (disposition != NULL)
		camel_mime_part_set_disposition (mime_part, disposition);

	g_simple_async_result_set_op_res_gpointer (
		simple, mime_part, (GDestroyNotify) camel_object_unref);

	g_simple_async_result_complete (simple);

	attachment_load_context_free (load_context);
}

static void
attachment_load_write_cb (GOutputStream *output_stream,
                          GAsyncResult *result,
                          AttachmentLoadContext *load_context)
{
	EAttachment *attachment;
	GCancellable *cancellable;
	GInputStream *input_stream;
	gssize bytes_written;
	GError *error = NULL;

	bytes_written = g_output_stream_write_finish (
		output_stream, result, &error);

	if (error != NULL) {
		GSimpleAsyncResult *simple;

		/* Steal the reference. */
		simple = load_context->simple;
		load_context->simple = NULL;

		g_simple_async_result_set_from_error (simple, error);
		g_simple_async_result_complete (simple);
		g_error_free (error);

		attachment_load_context_free (load_context);

		return;
	}

	attachment = load_context->attachment;
	cancellable = attachment->priv->cancellable;
	input_stream = load_context->input_stream;

	attachment_progress_cb (
		g_seekable_tell (G_SEEKABLE (output_stream)),
		load_context->total_num_bytes, attachment);

	if (bytes_written < load_context->bytes_read) {
		g_memmove (
			load_context->buffer,
			load_context->buffer + bytes_written,
			load_context->bytes_read - bytes_written);
		load_context->bytes_read -= bytes_written;

		g_output_stream_write_async (
			output_stream,
			load_context->buffer,
			load_context->bytes_read,
			G_PRIORITY_DEFAULT, cancellable,
			(GAsyncReadyCallback) attachment_load_write_cb,
			load_context);
	} else
		g_input_stream_read_async (
			input_stream,
			load_context->buffer,
			sizeof (load_context->buffer),
			G_PRIORITY_DEFAULT, cancellable,
			(GAsyncReadyCallback) attachment_load_stream_read_cb,
			load_context);
}

static void
attachment_load_stream_read_cb (GInputStream *input_stream,
                                GAsyncResult *result,
                                AttachmentLoadContext *load_context)
{
	EAttachment *attachment;
	GCancellable *cancellable;
	GOutputStream *output_stream;
	gssize bytes_read;
	GError *error = NULL;

	bytes_read = g_input_stream_read_finish (
		input_stream, result, &error);

	if (error != NULL) {
		GSimpleAsyncResult *simple;

		/* Steal the reference. */
		simple = load_context->simple;
		load_context->simple = NULL;

		g_simple_async_result_set_from_error (simple, error);
		g_simple_async_result_complete (simple);
		g_error_free (error);

		attachment_load_context_free (load_context);

		return;
	}

	if (bytes_read == 0) {
		attachment_load_finish (load_context);
		return;
	}

	attachment = load_context->attachment;
	cancellable = attachment->priv->cancellable;
	output_stream = load_context->output_stream;
	load_context->bytes_read = bytes_read;

	g_output_stream_write_async (
		output_stream,
		load_context->buffer,
		load_context->bytes_read,
		G_PRIORITY_DEFAULT, cancellable,
		(GAsyncReadyCallback) attachment_load_write_cb,
		load_context);
}

static void
attachment_load_file_read_cb (GFile *file,
                              GAsyncResult *result,
                              AttachmentLoadContext *load_context)
{
	EAttachment *attachment;
	GCancellable *cancellable;
	GFileInputStream *input_stream;
	GOutputStream *output_stream;
	GError *error = NULL;

	input_stream = g_file_read_finish (file, result, &error);
	load_context->input_stream = G_INPUT_STREAM (input_stream);

	if (error != NULL) {
		GSimpleAsyncResult *simple;

		/* Steal the reference. */
		simple = load_context->simple;
		load_context->simple = NULL;

		g_simple_async_result_set_from_error (simple, error);
		g_simple_async_result_complete (simple);
		g_error_free (error);

		attachment_load_context_free (load_context);

		return;
	}

	/* Load the contents into a GMemoryOutputStream. */
	output_stream = g_memory_output_stream_new (
		NULL, 0, g_realloc, g_free);

	attachment = load_context->attachment;
	cancellable = attachment->priv->cancellable;
	load_context->output_stream = output_stream;

	g_input_stream_read_async (
		load_context->input_stream,
		load_context->buffer,
		sizeof (load_context->buffer),
		G_PRIORITY_DEFAULT, cancellable,
		(GAsyncReadyCallback) attachment_load_stream_read_cb,
		load_context);
}

static void
attachment_load_query_info_cb (GFile *file,
                               GAsyncResult *result,
                               AttachmentLoadContext *load_context)
{
	EAttachment *attachment;
	GCancellable *cancellable;
	GFileInfo *file_info;
	GError *error = NULL;

	attachment = load_context->attachment;
	cancellable = attachment->priv->cancellable;

	file_info = g_file_query_info_finish (file, result, &error);
	attachment_set_file_info (attachment, file_info);
	load_context->file_info = file_info;

	if (error != NULL) {
		GSimpleAsyncResult *simple;

		/* Steal the reference. */
		simple = load_context->simple;
		load_context->simple = NULL;

		g_simple_async_result_set_from_error (simple, error);
		g_simple_async_result_complete (simple);
		g_error_free (error);

		attachment_load_context_free (load_context);

		return;
	}

	load_context->total_num_bytes = g_file_info_get_size (file_info);

	g_file_read_async (
		file, G_PRIORITY_DEFAULT,
		cancellable, (GAsyncReadyCallback)
		attachment_load_file_read_cb, load_context);
}

static void
attachment_load_from_mime_part (AttachmentLoadContext *load_context)
{
	GFileInfo *file_info;
	EAttachment *attachment;
	GSimpleAsyncResult *simple;
	CamelContentType *content_type;
	CamelMimePart *mime_part;
	const gchar *attribute;
	const gchar *string;
	gchar *allocated;
	goffset size;

	attachment = load_context->attachment;
	mime_part = e_attachment_get_mime_part (attachment);

	file_info = g_file_info_new ();
	load_context->file_info = file_info;

	content_type = camel_mime_part_get_content_type (mime_part);
	allocated = camel_content_type_simple (content_type);
	if (allocated != NULL) {
		GIcon *icon;
		gchar *cp;

		/* GIO expects lowercase MIME types. */
		for (cp = allocated; *cp != '\0'; cp++)
			*cp = g_ascii_tolower (*cp);

		/* Swap the MIME type for a content type. */
		cp = g_content_type_from_mime_type (allocated);
		g_free (allocated);
		allocated = cp;

		/* Use the MIME part's filename if we have to. */
		if (g_content_type_is_unknown (allocated)) {
			string = camel_mime_part_get_filename (mime_part);
			if (string != NULL) {
				g_free (allocated);
				allocated = g_content_type_guess (
					string, NULL, 0, NULL);
			}
		}

		g_file_info_set_content_type (file_info, allocated);

		icon = g_content_type_get_icon (allocated);
		if (icon != NULL) {
			g_file_info_set_icon (file_info, icon);
			g_object_unref (icon);
		}
	}
	g_free (allocated);

	string = camel_mime_part_get_filename (mime_part);
	if (string != NULL)
		g_file_info_set_display_name (file_info, string);

	attribute = G_FILE_ATTRIBUTE_STANDARD_DESCRIPTION;
	string = camel_mime_part_get_description (mime_part);
	if (string != NULL)
		g_file_info_set_attribute_string (
			file_info, attribute, string);

	size = (goffset) camel_mime_part_get_content_size (mime_part);
	g_file_info_set_size (file_info, size);

	string = camel_mime_part_get_disposition (mime_part);
	e_attachment_set_disposition (attachment, string);

	attachment_set_file_info (attachment, file_info);

	/* Steal the reference. */
	simple = load_context->simple;
	load_context->simple = NULL;

	camel_object_ref (mime_part);
	g_simple_async_result_set_op_res_gpointer (
		simple, mime_part,
		(GDestroyNotify) camel_object_unref);
	g_simple_async_result_complete_in_idle (simple);

	attachment_load_context_free (load_context);
}

void
e_attachment_load_async (EAttachment *attachment,
                         GAsyncReadyCallback callback,
                         gpointer user_data)
{
	AttachmentLoadContext *load_context;
	GCancellable *cancellable;
	CamelMimePart *mime_part;
	GFile *file;

	g_return_if_fail (E_IS_ATTACHMENT (attachment));
	g_return_if_fail (callback != NULL);

	g_return_if_fail (!e_attachment_get_loading (attachment));
	g_return_if_fail (!e_attachment_get_saving (attachment));

	file = e_attachment_get_file (attachment);
	mime_part = e_attachment_get_mime_part (attachment);
	g_return_if_fail (file != NULL || mime_part != NULL);

	load_context = attachment_load_context_new (
		attachment, callback, user_data);

	cancellable = attachment->priv->cancellable;
	g_cancellable_reset (cancellable);

	/* Handle the trivial case first. */
	if (mime_part != NULL)
		attachment_load_from_mime_part (load_context);

	else if (file != NULL)
		g_file_query_info_async (
			file, ATTACHMENT_QUERY,
			G_FILE_QUERY_INFO_NONE,G_PRIORITY_DEFAULT,
			cancellable, (GAsyncReadyCallback)
			attachment_load_query_info_cb, load_context);
}

gboolean
e_attachment_load_finish (EAttachment *attachment,
                          GAsyncResult *result,
                          GError **error)
{
	GSimpleAsyncResult *simple;
	CamelMimePart *mime_part;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (result,
		G_OBJECT (attachment), e_attachment_load_async), FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	mime_part = g_simple_async_result_get_op_res_gpointer (simple);
	if (mime_part != NULL)
		e_attachment_set_mime_part (attachment, mime_part);
	g_simple_async_result_propagate_error (simple, error);
	g_object_unref (simple);

	attachment_set_loading (attachment, FALSE);

	return (mime_part != NULL);
}

void
e_attachment_load_handle_error (EAttachment *attachment,
                                GAsyncResult *result,
                                GtkWindow *parent)
{
	GtkWidget *dialog;
	GFileInfo *file_info;
	const gchar *display_name;
	const gchar *primary_text;
	GError *error = NULL;

	g_return_if_fail (E_IS_ATTACHMENT (attachment));
	g_return_if_fail (G_IS_ASYNC_RESULT (result));
	g_return_if_fail (GTK_IS_WINDOW (parent));

	if (e_attachment_load_finish (attachment, result, &error))
		return;

	/* Ignore cancellations. */
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	file_info = e_attachment_get_file_info (attachment);

	if (file_info != NULL)
		display_name = g_file_info_get_display_name (file_info);
	else
		display_name = NULL;

	if (display_name != NULL)
		primary_text = g_strdup_printf (
			_("Could not load '%s'"), display_name);
	else
		primary_text = g_strdup_printf (
			_("Could not load the attachment"));

	dialog = gtk_message_dialog_new_with_markup (
		parent, GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
		"<big><b>%s</b></big>", primary_text);

	gtk_message_dialog_format_secondary_text (
		GTK_MESSAGE_DIALOG (dialog), "%s", error->message);

	gtk_dialog_run (GTK_DIALOG (dialog));

	gtk_widget_destroy (dialog);
	g_error_free (error);
}

/************************* e_attachment_open_async() *************************/

typedef struct _AttachmentOpenContext AttachmentOpenContext;

struct _AttachmentOpenContext {
	EAttachment *attachment;
	GSimpleAsyncResult *simple;
	GAppInfo *app_info;
	GFile *file;
};

static AttachmentOpenContext *
attachment_open_context_new (EAttachment *attachment,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
	AttachmentOpenContext *open_context;
	GSimpleAsyncResult *simple;

	simple = g_simple_async_result_new (
		G_OBJECT (attachment), callback,
		user_data, e_attachment_open_async);

	open_context = g_slice_new0 (AttachmentOpenContext);
	open_context->attachment = g_object_ref (attachment);
	open_context->simple = simple;

	return open_context;
}

static void
attachment_open_context_free (AttachmentOpenContext *open_context)
{
	/* Do not free the GSimpleAsyncResult. */
	g_object_unref (open_context->attachment);

	if (open_context->app_info != NULL)
		g_object_unref (open_context->app_info);

	if (open_context->file != NULL)
		g_object_unref (open_context->file);

	g_slice_free (AttachmentOpenContext, open_context);
}

static void
attachment_open_file (AttachmentOpenContext *open_context)
{
	GdkAppLaunchContext *context;
	GSimpleAsyncResult *simple;
	GList *file_list;
	gboolean success;
	GError *error = NULL;

	/* Steal the reference. */
	simple = open_context->simple;
	open_context->simple = NULL;

	if (open_context->app_info == NULL)
		open_context->app_info = g_file_query_default_handler (
			open_context->file, NULL, &error);

	if (open_context->app_info == NULL)
		goto exit;

	context = gdk_app_launch_context_new ();
	file_list = g_list_prepend (NULL, open_context->file);

	success = g_app_info_launch (
		open_context->app_info, file_list,
		G_APP_LAUNCH_CONTEXT (context), &error);

	g_simple_async_result_set_op_res_gboolean (simple, success);

	g_list_free (file_list);
	g_object_unref (context);

exit:
	if (error != NULL) {
		g_simple_async_result_set_from_error (simple, error);
		g_error_free (error);
	}

	g_simple_async_result_complete (simple);
	attachment_open_context_free (open_context);
}

static void
attachment_open_save_finished_cb (EAttachment *attachment,
                                  GAsyncResult *result,
                                  AttachmentOpenContext *open_context)
{
	GError *error = NULL;

	if (e_attachment_save_finish (attachment, result, &error))
		attachment_open_file (open_context);
	else {
		GSimpleAsyncResult *simple;

		/* Steal the reference. */
		simple = open_context->simple;
		open_context->simple = NULL;

		g_simple_async_result_set_from_error (simple, error);
		g_simple_async_result_complete (simple);
		g_error_free (error);

		attachment_open_context_free (open_context);
	}
}

static void
attachment_open_save_temporary (AttachmentOpenContext *open_context)
{
	gchar *path;
	gint fd;
	GError *error = NULL;

	fd = e_file_open_tmp (&path, &error);
	if (error != NULL) {
		GSimpleAsyncResult *simple;

		/* Steal the reference. */
		simple = open_context->simple;
		open_context->simple = NULL;

		g_simple_async_result_set_from_error (simple, error);
		g_simple_async_result_complete (simple);
		g_error_free (error);

		attachment_open_context_free (open_context);
		return;
	}

	close (fd);

	open_context->file = g_file_new_for_path (path);

	e_attachment_save_async (
		open_context->attachment, open_context->file,
		(GAsyncReadyCallback) attachment_open_save_finished_cb,
		open_context);
}

void
e_attachment_open_async (EAttachment *attachment,
                         GAppInfo *app_info,
                         GAsyncReadyCallback callback,
                         gpointer user_data)
{
	AttachmentOpenContext *open_context;
	CamelMimePart *mime_part;
	GFile *file;

	g_return_if_fail (E_IS_ATTACHMENT (attachment));
	g_return_if_fail (callback != NULL);

	g_return_if_fail (!e_attachment_get_loading (attachment));
	g_return_if_fail (!e_attachment_get_saving (attachment));

	file = e_attachment_get_file (attachment);
	mime_part = e_attachment_get_mime_part (attachment);
	g_return_if_fail (file != NULL || mime_part != NULL);

	open_context = attachment_open_context_new (
		attachment, callback, user_data);

	if (G_IS_APP_INFO (app_info))
		open_context->app_info = g_object_ref (app_info);

	/* If the attachment already references a GFile, we can launch
	 * the application directly.  Otherwise we have to save the MIME
	 * part to a temporary file and launch the application from that. */
	if (file != NULL) {
		open_context->file = g_object_ref (file);
		attachment_open_file (open_context);

	} else if (mime_part != NULL)
		attachment_open_save_temporary (open_context);
}

gboolean
e_attachment_open_finish (EAttachment *attachment,
                          GAsyncResult *result,
                          GError **error)
{
	GSimpleAsyncResult *simple;
	gboolean success;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (result,
		G_OBJECT (attachment), e_attachment_open_async), FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	success = g_simple_async_result_get_op_res_gboolean (simple);
	g_simple_async_result_propagate_error (simple, error);
	g_object_unref (simple);

	return success;
}

void
e_attachment_open_handle_error (EAttachment *attachment,
                                GAsyncResult *result,
                                GtkWindow *parent)
{
	GtkWidget *dialog;
	GFileInfo *file_info;
	const gchar *display_name;
	const gchar *primary_text;
	GError *error = NULL;

	g_return_if_fail (E_IS_ATTACHMENT (attachment));
	g_return_if_fail (G_IS_ASYNC_RESULT (result));
	g_return_if_fail (GTK_IS_WINDOW (parent));

	if (e_attachment_open_finish (attachment, result, &error))
		return;

	/* Ignore cancellations. */
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	file_info = e_attachment_get_file_info (attachment);

	if (file_info != NULL)
		display_name = g_file_info_get_display_name (file_info);
	else
		display_name = NULL;

	if (display_name != NULL)
		primary_text = g_strdup_printf (
			_("Could not open '%s'"), display_name);
	else
		primary_text = g_strdup_printf (
			_("Could not open the attachment"));

	dialog = gtk_message_dialog_new_with_markup (
		parent, GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
		"<big><b>%s</b></big>", primary_text);

	gtk_message_dialog_format_secondary_text (
		GTK_MESSAGE_DIALOG (dialog), "%s", error->message);

	gtk_dialog_run (GTK_DIALOG (dialog));

	gtk_widget_destroy (dialog);
	g_error_free (error);
}

/************************* e_attachment_save_async() *************************/

typedef struct _AttachmentSaveContext AttachmentSaveContext;

struct _AttachmentSaveContext {
	EAttachment *attachment;
	GSimpleAsyncResult *simple;
	GInputStream *input_stream;
	GOutputStream *output_stream;
	goffset total_num_bytes;
	gssize bytes_read;
	gchar buffer[4096];
};

/* Forward Declaration */
static void
attachment_save_read_cb (GInputStream *input_stream,
                         GAsyncResult *result,
                         AttachmentSaveContext *save_context);

static AttachmentSaveContext *
attachment_save_context_new (EAttachment *attachment,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
	AttachmentSaveContext *save_context;
	GSimpleAsyncResult *simple;

	simple = g_simple_async_result_new (
		G_OBJECT (attachment), callback,
		user_data, e_attachment_save_async);

	save_context = g_slice_new0 (AttachmentSaveContext);
	save_context->attachment = g_object_ref (attachment);
	save_context->simple = simple;

	attachment_set_saving (save_context->attachment, TRUE);

	return save_context;
}

static void
attachment_save_context_free (AttachmentSaveContext *save_context)
{
	/* Do not free the GSimpleAsyncResult. */
	g_object_unref (save_context->attachment);

	if (save_context->input_stream != NULL)
		g_object_unref (save_context->input_stream);

	if (save_context->output_stream != NULL)
		g_object_unref (save_context->output_stream);

	g_slice_free (AttachmentSaveContext, save_context);
}

static void
attachment_save_file_cb (GFile *source,
                         GAsyncResult *result,
                         AttachmentSaveContext *save_context)
{
	GSimpleAsyncResult *simple;
	gboolean success;
	GError *error = NULL;

	/* Steal the reference. */
	simple = save_context->simple;
	save_context->simple = NULL;

	success = g_file_copy_finish (source, result, &error);
	g_simple_async_result_set_op_res_gboolean (simple, success);

	if (error != NULL) {
		g_simple_async_result_set_from_error (simple, error);
		g_error_free (error);
	}

	g_simple_async_result_complete (simple);
	attachment_save_context_free (save_context);
}

static void
attachment_save_write_cb (GOutputStream *output_stream,
                          GAsyncResult *result,
                          AttachmentSaveContext *save_context)
{
	EAttachment *attachment;
	GCancellable *cancellable;
	GInputStream *input_stream;
	gssize bytes_written;
	GError *error = NULL;

	bytes_written = g_output_stream_write_finish (
		output_stream, result, &error);

	if (error != NULL) {
		GSimpleAsyncResult *simple;

		/* Steal the reference. */
		simple = save_context->simple;
		save_context->simple = NULL;

		g_simple_async_result_set_from_error (simple, error);
		g_simple_async_result_complete (simple);
		g_error_free (error);

		attachment_save_context_free (save_context);

		return;
	}

	attachment = save_context->attachment;
	cancellable = attachment->priv->cancellable;
	input_stream = save_context->input_stream;

	if (bytes_written < save_context->bytes_read) {
		g_memmove (
			save_context->buffer,
			save_context->buffer + bytes_written,
			save_context->bytes_read - bytes_written);
		save_context->bytes_read -= bytes_written;

		g_output_stream_write_async (
			output_stream,
			save_context->buffer,
			save_context->bytes_read,
			G_PRIORITY_DEFAULT, cancellable,
			(GAsyncReadyCallback) attachment_save_write_cb,
			save_context);
	} else
		g_input_stream_read_async (
			input_stream,
			save_context->buffer,
			sizeof (save_context->buffer),
			G_PRIORITY_DEFAULT, cancellable,
			(GAsyncReadyCallback) attachment_save_read_cb,
			save_context);
}

static void
attachment_save_read_cb (GInputStream *input_stream,
                         GAsyncResult *result,
                         AttachmentSaveContext *save_context)
{
	EAttachment *attachment;
	GCancellable *cancellable;
	GOutputStream *output_stream;
	gssize bytes_read;
	GError *error = NULL;

	bytes_read = g_input_stream_read_finish (
		input_stream, result, &error);

	if (error != NULL) {
		GSimpleAsyncResult *simple;

		/* Steal the reference. */
		simple = save_context->simple;
		save_context->simple = NULL;

		g_simple_async_result_set_from_error (simple, error);
		g_simple_async_result_complete (simple);
		g_error_free (error);

		attachment_save_context_free (save_context);

		return;
	}

	if (bytes_read == 0) {
		GSimpleAsyncResult *simple;

		/* Steal the reference. */
		simple = save_context->simple;
		save_context->simple = NULL;

		g_simple_async_result_set_op_res_gboolean (simple, TRUE);
		g_simple_async_result_complete (simple);

		attachment_save_context_free (save_context);

		return;
	}

	attachment = save_context->attachment;
	cancellable = attachment->priv->cancellable;
	output_stream = save_context->output_stream;
	save_context->bytes_read = bytes_read;

	attachment_progress_cb (
		g_seekable_tell (G_SEEKABLE (input_stream)),
		save_context->total_num_bytes, attachment);

	g_output_stream_write_async (
		output_stream,
		save_context->buffer,
		save_context->bytes_read,
		G_PRIORITY_DEFAULT, cancellable,
		(GAsyncReadyCallback) attachment_save_write_cb,
		save_context);
}

static void
attachment_save_replace_cb (GFile *destination,
                            GAsyncResult *result,
                            AttachmentSaveContext *save_context)
{
	GCancellable *cancellable;
	GInputStream *input_stream;
	GFileOutputStream *output_stream;
	CamelDataWrapper *wrapper;
	CamelMimePart *mime_part;
	CamelStream *stream;
	EAttachment *attachment;
	GByteArray *buffer;
	GError *error = NULL;

	output_stream = g_file_replace_finish (destination, result, &error);
	save_context->output_stream = G_OUTPUT_STREAM (output_stream);

	if (error != NULL) {
		GSimpleAsyncResult *simple;

		/* Steal the reference. */
		simple = save_context->simple;
		save_context->simple = NULL;

		g_simple_async_result_set_from_error (simple, error);
		g_simple_async_result_complete (simple);
		g_error_free (error);

		attachment_save_context_free (save_context);

		return;
	}

	attachment = save_context->attachment;
	cancellable = attachment->priv->cancellable;
	mime_part = e_attachment_get_mime_part (attachment);

	/* Decode the MIME part to an in-memory buffer.  We have to do
	 * this because CamelStream is synchronous-only, and using threads
	 * is dangerous because CamelDataWrapper is not reentrant. */
	buffer = g_byte_array_new ();
	stream = camel_stream_mem_new_with_byte_array (buffer);
	wrapper = camel_medium_get_content_object (CAMEL_MEDIUM (mime_part));
	camel_data_wrapper_decode_to_stream (wrapper, stream);
	camel_object_unref (stream);

	/* Load the buffer into a GMemoryInputStream. */
	input_stream = g_memory_input_stream_new_from_data (
		buffer->data, (gssize) buffer->len,
		(GDestroyNotify) g_free);
	save_context->input_stream = input_stream;
	save_context->total_num_bytes = (goffset) buffer->len;
	g_byte_array_free (buffer, FALSE);

	g_input_stream_read_async (
		input_stream,
		save_context->buffer,
		sizeof (save_context->buffer),
		G_PRIORITY_DEFAULT, cancellable,
		(GAsyncReadyCallback) attachment_save_read_cb,
		save_context);
}

void
e_attachment_save_async (EAttachment *attachment,
                         GFile *destination,
                         GAsyncReadyCallback callback,
                         gpointer user_data)
{
	AttachmentSaveContext *save_context;
	GCancellable *cancellable;
	CamelMimePart *mime_part;
	GFile *source;

	g_return_if_fail (E_IS_ATTACHMENT (attachment));
	g_return_if_fail (G_IS_FILE (destination));
	g_return_if_fail (callback != NULL);

	g_return_if_fail (!e_attachment_get_loading (attachment));
	g_return_if_fail (!e_attachment_get_saving (attachment));

	/* The attachment content is either a GFile (on disk) or a
	 * CamelMimePart (in memory).  Each is saved differently. */

	source = e_attachment_get_file (attachment);
	mime_part = e_attachment_get_mime_part (attachment);
	g_return_if_fail (source != NULL || mime_part != NULL);

	save_context = attachment_save_context_new (
		attachment, callback, user_data);

	cancellable = attachment->priv->cancellable;
	g_cancellable_reset (cancellable);

	/* GFile is the easier, but probably less common case.  The
	 * attachment already references an on-disk file, so we can
	 * just use GIO to copy it asynchronously.
	 *
	 * We use G_FILE_COPY_OVERWRITE because the user should have
	 * already confirmed the overwrite through the save dialog. */
	if (G_IS_FILE (source))
		g_file_copy_async (
			source, destination,
			G_FILE_COPY_OVERWRITE,
			G_PRIORITY_DEFAULT, cancellable,
			(GFileProgressCallback) attachment_progress_cb,
			attachment,
			(GAsyncReadyCallback) attachment_save_file_cb,
			save_context);

	/* CamelMimePart can only be decoded to a file synchronously, so
	 * we do this in two stages.  Stage one asynchronously opens the
	 * destination file for writing.  Stage two spawns a thread that
	 * decodes the MIME part to the destination file.  This stage is
	 * not cancellable, unfortunately. */
	else if (CAMEL_IS_MIME_PART (mime_part))
		g_file_replace_async (
			destination, NULL, FALSE,
			G_FILE_CREATE_REPLACE_DESTINATION,
			G_PRIORITY_DEFAULT, cancellable,
			(GAsyncReadyCallback) attachment_save_replace_cb,
			save_context);
}

gboolean
e_attachment_save_finish (EAttachment *attachment,
                          GAsyncResult *result,
                          GError **error)
{
	GSimpleAsyncResult *simple;
	gboolean success;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (result,
		G_OBJECT (attachment), e_attachment_save_async), FALSE);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	success = g_simple_async_result_get_op_res_gboolean (simple);
	g_simple_async_result_propagate_error (simple, error);
	g_object_unref (simple);

	attachment_set_saving (attachment, FALSE);

	return success;
}

void
e_attachment_save_handle_error (EAttachment *attachment,
                                GAsyncResult *result,
                                GtkWindow *parent)
{
	GtkWidget *dialog;
	GFileInfo *file_info;
	const gchar *display_name;
	const gchar *primary_text;
	GError *error = NULL;

	g_return_if_fail (E_IS_ATTACHMENT (attachment));
	g_return_if_fail (G_IS_ASYNC_RESULT (result));
	g_return_if_fail (GTK_IS_WINDOW (parent));

	if (e_attachment_save_finish (attachment, result, &error))
		return;

	/* Ignore cancellations. */
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;

	file_info = e_attachment_get_file_info (attachment);

	if (file_info != NULL)
		display_name = g_file_info_get_display_name (file_info);
	else
		display_name = NULL;

	if (display_name != NULL)
		primary_text = g_strdup_printf (
			_("Could not save '%s'"), display_name);
	else
		primary_text = g_strdup_printf (
			_("Could not save the attachment"));

	dialog = gtk_message_dialog_new_with_markup (
		parent, GTK_DIALOG_DESTROY_WITH_PARENT,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
		"<big><b>%s</b></big>", primary_text);

	gtk_message_dialog_format_secondary_text (
		GTK_MESSAGE_DIALOG (dialog), "%s", error->message);

	gtk_dialog_run (GTK_DIALOG (dialog));

	gtk_widget_destroy (dialog);
	g_error_free (error);
}
