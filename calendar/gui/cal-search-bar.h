/* Evolution calendar - Search bar widget for calendar views
 *
 * Copyright (C) 2001 Ximian, Inc.
 *
 * Author: Federico Mena-Quintero <federico@ximian.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef CAL_SEARCH_BAR_H
#define CAL_SEARCH_BAR_H

#include <libgnome/gnome-defs.h>
#include "widgets/misc/e-search-bar.h"
#include "widgets/misc/e-filter-bar.h"

BEGIN_GNOME_DECLS



#define TYPE_CAL_SEARCH_BAR            (cal_search_bar_get_type ())
#define CAL_SEARCH_BAR(obj)            (GTK_CHECK_CAST ((obj), TYPE_CAL_SEARCH_BAR, CalSearchBar))
#define CAL_SEARCH_BAR_CLASS(klass)    (GTK_CHECK_CLASS_CAST ((klass), TYPE_CAL_SEARCH_BAR,	\
					CalSearchBarClass))
#define IS_CAL_SEARCH_BAR(obj)         (GTK_CHECK_TYPE ((obj), TYPE_CAL_SEARCH_BAR))
#define IS_CAL_SEARCH_BAR_CLASS(klass) (GTK_CHECK_CLASS_TYPE ((klass), TYPE_CAL_SEARCH_BAR))

typedef struct {
	ESearchBar search_bar;
} CalSearchBar;

typedef struct {
	ESearchBarClass parent_class;

	/* Notification signals */

	void (* sexp_changed) (CalSearchBar *cal_search, const char *sexp);
} CalSearchBarClass;

GtkType cal_search_bar_get_type (void);

CalSearchBar *cal_search_bar_construct (CalSearchBar *cal_search);

GtkWidget *cal_search_bar_new (void);



END_GNOME_DECLS

#endif
