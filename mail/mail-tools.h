/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* 
 * Author : 
 *  Peter Williams <peterw@helixcode.com>
 *
 * Copyright 2000 Helix Code, Inc. (http://www.helixcode.com)
 *
 * This program is free software; you can redistribute it and/or 
 * modify it under the terms of the GNU General Public License as 
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */

#ifndef MAIL_TOOLS_H
#define MAIL_TOOLS_H

#include <camel/camel.h>
#include <camel/camel-filter-driver.h> /*eek*/

/* Get a CamelFolder from a root url and a foldername (uses the global session)*/
CamelFolder *
mail_tool_get_folder_from_urlname (const gchar *url, const gchar *name,
				   guint32 flags, CamelException *ex);

/* Get a useful name for a given CamelFolder (ie, not "mbox") */
const gchar *mail_tool_get_folder_name (CamelFolder *folder);

/* Get the url for the local inbox, index returns if the mailbox is indexed */
gchar *mail_tool_get_local_inbox_url (int *index);

/* Get the filename for our movemail folder or storage */
gchar *mail_tool_get_local_movemail_path (void);
gchar *mail_tool_get_local_movemail_url (void);

/* Get the CamelFolder for the local inbox */
CamelFolder *mail_tool_get_local_inbox (CamelException *ex);

/* Get the "inbox" for a url (uses global session) */
CamelFolder *mail_tool_get_inbox (const gchar *url, CamelException *ex);

/* Does a camel_movemail into the local movemail folder
 * and returns the path to the new movemail folder that was created. which shoudl be freed later */
char *
mail_tool_do_movemail (const gchar *source_url, CamelException *ex);

/* Transfers all the messages from source into dest;
 * source is emptied and synced. */
void
mail_tool_move_folder_contents (CamelFolder *source, CamelFolder *dest, gboolean use_cache, CamelException *ex);

/* Generates the subject for a message forwarding @msg */
gchar *
mail_tool_generate_forward_subject (CamelMimeMessage *msg);

/* Make a message into an attachment */
CamelMimePart *
mail_tool_make_message_attachment (CamelMimeMessage *message);

/* Parse the ui into a real CamelFolder any way we know how. */
CamelFolder *
mail_tool_uri_to_folder (const char *uri, CamelException *ex);

GHashTable *
mail_lookup_url_table (CamelMimeMessage *mime_message);

gchar *mail_tool_quote_message (CamelMimeMessage *message, const char *fmt, ...);

#endif
