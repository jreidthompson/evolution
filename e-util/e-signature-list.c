/*
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
 * Authors:
 *		Jeffrey Stedfast <fejj@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#include "e-signature-list.h"

#include <config.h>
#include <string.h>

#include <libedataserver/e-uid.h>

struct _ESignatureListPrivate {
	GConfClient *gconf;
	guint notify_id;
	gboolean resave;
};

enum {
	SIGNATURE_ADDED,
	SIGNATURE_CHANGED,
	SIGNATURE_REMOVED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (
	ESignatureList,
	e_signature_list,
	E_TYPE_LIST)

static void
e_signature_list_dispose (GObject *object)
{
	ESignatureList *list = (ESignatureList *) object;

	if (list->priv->gconf) {
		if (list->priv->notify_id != 0)
			gconf_client_notify_remove (
				list->priv->gconf, list->priv->notify_id);
		g_object_unref (list->priv->gconf);
		list->priv->gconf = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_signature_list_parent_class)->dispose (object);
}

static void
e_signature_list_class_init (ESignatureListClass *class)
{
	GObjectClass *object_class;

	g_type_class_add_private (class, sizeof (ESignatureListPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->dispose = e_signature_list_dispose;

	signals[SIGNATURE_ADDED] = g_signal_new (
		"signature-added",
		G_OBJECT_CLASS_TYPE (object_class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (ESignatureListClass, signature_added),
		NULL, NULL,
		g_cclosure_marshal_VOID__OBJECT,
		G_TYPE_NONE, 1,
		E_TYPE_SIGNATURE);

	signals[SIGNATURE_CHANGED] = g_signal_new (
		"signature-changed",
		G_OBJECT_CLASS_TYPE (object_class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (ESignatureListClass, signature_changed),
		NULL, NULL,
		g_cclosure_marshal_VOID__OBJECT,
		G_TYPE_NONE, 1,
		E_TYPE_SIGNATURE);

	signals[SIGNATURE_REMOVED] = g_signal_new (
		"signature-removed",
		G_OBJECT_CLASS_TYPE (object_class),
		G_SIGNAL_RUN_LAST,
		G_STRUCT_OFFSET (ESignatureListClass, signature_removed),
		NULL, NULL,
		g_cclosure_marshal_VOID__OBJECT,
		G_TYPE_NONE, 1,
		E_TYPE_SIGNATURE);
}

static void
e_signature_list_init (ESignatureList *signature_list)
{
	signature_list->priv = G_TYPE_INSTANCE_GET_PRIVATE (signature_list, E_TYPE_SIGNATURE_LIST, ESignatureListPrivate);
}

static GSList *
add_autogen (ESignatureList *list, GSList *new_sigs)
{
	ESignature *autogen;

	autogen = e_signature_new ();
	e_signature_set_autogenerated (autogen, TRUE);

	e_list_append (E_LIST (list), autogen);

	return g_slist_prepend (new_sigs, autogen);
}

static void
gconf_signatures_changed (GConfClient *client,
                          guint cnxn_id,
                          GConfEntry *entry,
                          gpointer user_data)
{
	ESignatureList *signature_list = user_data;
	GSList *list, *l, *n, *new_sigs = NULL;
	gboolean have_autogen = FALSE;
	gboolean resave = FALSE;
	ESignature *signature;
	EList *old_sigs;
	EIterator *iter;
	gboolean found;
	gchar *uid;

	old_sigs = e_list_duplicate (E_LIST (signature_list));

	list = gconf_client_get_list (
		client, "/apps/evolution/mail/signatures",
		GCONF_VALUE_STRING, NULL);
	for (l = list; l; l = l->next) {
		found = FALSE;
		if ((uid = e_signature_uid_from_xml (l->data))) {
			/* See if this is an existing signature */
			iter = e_list_get_iterator (old_sigs);
			while (e_iterator_is_valid (iter)) {
				const gchar *signature_uid;

				signature = (ESignature *) e_iterator_get (iter);
				signature_uid = e_signature_get_uid (signature);
				if (!strcmp (signature_uid, uid)) {
					/* The signature still exists, so remove
					 * it from "old_sigs" and update it. */
					found = TRUE;
					e_iterator_delete (iter);
					if (e_signature_set_from_xml (signature, l->data))
						g_signal_emit (
							signature_list,
							signals[SIGNATURE_CHANGED],
							0, signature);

					have_autogen |= e_signature_get_autogenerated (signature);

					break;
				}

				e_iterator_next (iter);
			}

			g_object_unref (iter);
		}

		if (!found) {
			resave = TRUE;

			/* Must be a new signature */
			signature = e_signature_new_from_xml (l->data);
			if (signature) {
				have_autogen |= e_signature_get_autogenerated (signature);

				e_list_append (E_LIST (signature_list), signature);
				new_sigs = g_slist_prepend (new_sigs, signature);
			}
		}

		g_free (uid);
	}

	if (!have_autogen) {
		new_sigs = add_autogen (signature_list, new_sigs);
		resave = TRUE;
	}

	if (new_sigs != NULL) {
		/* Now emit signals for each added signature. */
		l = g_slist_reverse (new_sigs);
		while (l != NULL) {
			n = l->next;
			signature = l->data;
			g_signal_emit (signature_list, signals[SIGNATURE_ADDED], 0, signature);
			g_object_unref (signature);
			g_slist_free_1 (l);
			l = n;
		}
	}

	/* Anything left in old_sigs must have been deleted */
	iter = e_list_get_iterator (old_sigs);
	while (e_iterator_is_valid (iter)) {
		signature = (ESignature *) e_iterator_get (iter);
		e_list_remove (E_LIST (signature_list), signature);
		g_signal_emit (
			signature_list, signals[SIGNATURE_REMOVED], 0,
			signature);
		e_iterator_next (iter);
	}

	g_object_unref (iter);
	g_object_unref (old_sigs);

	signature_list->priv->resave = resave;
}

static gpointer
copy_func (gconstpointer data, gpointer closure)
{
	GObject *object = (GObject *)data;

	g_object_ref (object);

	return object;
}

static void
free_func (gpointer data, gpointer closure)
{
	g_object_unref (data);
}

/**
 * e_signature_list_new:
 * @gconf: a #GConfClient
 *
 * Reads the list of signaturess from @gconf and listens for changes.
 * Will emit #signature_added, #signature_changed, and #signature_removed
 * signals according to notifications from GConf.
 *
 * You can modify the list using e_list_append(), e_list_remove(), and
 * e_iterator_delete(). After adding, removing, or changing accounts,
 * you must call e_signature_list_save() to push the changes back to
 * GConf.
 *
 * Return value: the list of signatures
 **/
ESignatureList *
e_signature_list_new (GConfClient *gconf)
{
	ESignatureList *signature_list;

	g_return_val_if_fail (GCONF_IS_CLIENT (gconf), NULL);

	signature_list = g_object_new (E_TYPE_SIGNATURE_LIST, NULL);
	e_signature_list_construct (signature_list, gconf);

	return signature_list;
}

void
e_signature_list_construct (ESignatureList *signature_list, GConfClient *gconf)
{
	g_return_if_fail (GCONF_IS_CLIENT (gconf));

	e_list_construct (E_LIST (signature_list), copy_func, free_func, NULL);
	signature_list->priv->gconf = gconf;
	g_object_ref (gconf);

	gconf_client_add_dir (signature_list->priv->gconf,
			      "/apps/evolution/mail/signatures",
			      GCONF_CLIENT_PRELOAD_ONELEVEL, NULL);

	signature_list->priv->notify_id =
		gconf_client_notify_add (signature_list->priv->gconf,
					 "/apps/evolution/mail/signatures",
					 gconf_signatures_changed, signature_list,
					 NULL, NULL);

	gconf_signatures_changed (signature_list->priv->gconf,
				  signature_list->priv->notify_id,
				  NULL, signature_list);

	if (signature_list->priv->resave) {
		e_signature_list_save (signature_list);
		signature_list->priv->resave = FALSE;
	}
}

/**
 * e_signature_list_save:
 * @signature_list: an #ESignatureList
 *
 * Saves @signature_list to GConf. Signals will be emitted for changes.
 **/
void
e_signature_list_save (ESignatureList *signature_list)
{
	GSList *list = NULL;
	ESignature *signature;
	EIterator *iter;
	gchar *xmlbuf;

	for (iter = e_list_get_iterator (E_LIST (signature_list));
	     e_iterator_is_valid (iter);
	     e_iterator_next (iter)) {
		signature = (ESignature *) e_iterator_get (iter);

		if ((xmlbuf = e_signature_to_xml (signature)))
			list = g_slist_append (list, xmlbuf);
	}

	g_object_unref (iter);

	gconf_client_set_list (signature_list->priv->gconf,
			       "/apps/evolution/mail/signatures",
			       GCONF_VALUE_STRING, list, NULL);

	while (list) {
		g_free (list->data);
		list = g_slist_remove (list, list->data);
	}

	gconf_client_suggest_sync (signature_list->priv->gconf, NULL);
}

/**
 * e_signature_list_add:
 * @signature_list: signature list
 * @signature: signature to add
 *
 * Add an signature to the signature list.  Will emit the signature-changed
 * event.
 **/
void
e_signature_list_add (ESignatureList *signature_list, ESignature *signature)
{
	e_list_append ((EList *) signature_list, signature);
	g_signal_emit (signature_list, signals[SIGNATURE_ADDED], 0, signature);
}

/**
 * e_signature_list_change:
 * @signature_list: signature list
 * @signature: signature to change
 *
 * Signal that the details of an signature have changed.
 **/
void
e_signature_list_change (ESignatureList *signature_list, ESignature *signature)
{
	/* maybe the signature should do this itself ... */
	g_signal_emit (signature_list, signals[SIGNATURE_CHANGED], 0, signature);
}

/**
 * e_signature_list_remove:
 * @signature_list: signature list
 * @signature: signature
 *
 * Remove an signature from the signature list, and emit the
 * signature-removed signal.  If the signature was the default signature,
 * then reset the default to the first signature.
 **/
void
e_signature_list_remove (ESignatureList *signature_list, ESignature *signature)
{
	/* not sure if need to ref but no harm */
	g_object_ref (signature);
	e_list_remove ((EList *) signature_list, signature);
	g_signal_emit (signature_list, signals[SIGNATURE_REMOVED], 0, signature);
	g_object_unref (signature);
}

/**
 * e_signature_list_find_by_name:
 * @signature_list: an #ESignatureList
 * @name: the signature name to find
 *
 * Searches @signature_list for the given signature name.
 *
 * Returns: the matching signature or %NULL if it doesn't exist
 **/
ESignature *
e_signature_list_find_by_name (ESignatureList *signature_list,
                               const gchar *signature_name)
{
	ESignature *signature = NULL;
	EIterator *it;

	g_return_val_if_fail (E_IS_SIGNATURE_LIST (signature_list), NULL);

	/* this could use a callback for more flexibility ...
	   ... but this makes the common cases easier */

	if (signature_name == NULL)
		return NULL;

	for (it = e_list_get_iterator (E_LIST (signature_list));
	     e_iterator_is_valid (it);
	     e_iterator_next (it)) {
		const gchar *value;

		/* XXX EIterator misuses const. */
		signature = (ESignature *) e_iterator_get (it);
		value = e_signature_get_name (signature);

		if (g_strcmp0 (value, signature_name) == 0)
			break;

		signature = NULL;
	}

	g_object_unref (it);

	return signature;
}

/**
 * e_signature_list_find_by_uid:
 * @signature_list: an #ESignatureList
 * @name: the signature UID to find
 *
 * Searches @signature_list for the given signature UID.
 *
 * Returns: the matching signature or %NULL if it doesn't exist
 **/
ESignature *
e_signature_list_find_by_uid (ESignatureList *signature_list,
                              const gchar *signature_uid)
{
	ESignature *signature = NULL;
	EIterator *it;

	g_return_val_if_fail (E_IS_SIGNATURE_LIST (signature_list), NULL);

	/* this could use a callback for more flexibility ...
	   ... but this makes the common cases easier */

	if (signature_uid == NULL)
		return NULL;

	for (it = e_list_get_iterator (E_LIST (signature_list));
	     e_iterator_is_valid (it);
	     e_iterator_next (it)) {
		const gchar *value = NULL;

		/* XXX EIterator misuses const. */
		signature = (ESignature *) e_iterator_get (it);
		value = e_signature_get_uid (signature);

		if (g_strcmp0 (value, signature_uid) == 0)
			break;

		signature = NULL;
	}

	g_object_unref (it);

	return signature;
}
