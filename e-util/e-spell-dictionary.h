<<<<<<< HEAD
/*
 * e-spell-dictionary.h
=======
/* e-spell-dictionary.h
>>>>>>> Import classes for spell checking
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

<<<<<<< HEAD
=======
/* Based on Marco Barisione's GSpellLanguage. */

>>>>>>> Import classes for spell checking
#if !defined (__E_UTIL_H_INSIDE__) && !defined (LIBEUTIL_COMPILATION)
#error "Only <e-util/e-util.h> should be included directly."
#endif

#ifndef E_SPELL_DICTIONARY_H
#define E_SPELL_DICTIONARY_H

#include <glib-object.h>
<<<<<<< HEAD
#include <enchant/enchant.h>

/* Standard GObject macros */
#define E_TYPE_SPELL_DICTIONARY \
	(e_spell_dictionary_get_type ())
#define E_SPELL_DICTIONARY(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_SPELL_DICTIONARY, ESpellDictionary))
#define E_SPELL_DICTIONARY_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_SPELL_DICTIONARY, ESpellDictionaryClass))
#define E_IS_SPELL_DICTIONARY(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_SPELL_DICTIONARY))
#define E_IS_SPELL_DICTIONARY_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_SPELL_DICTIONARY))
#define E_SPELL_DICTIONARY_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_SPELL_DICTIONARY, ESpellDictionaryClass))
=======
>>>>>>> Import classes for spell checking

G_BEGIN_DECLS

typedef struct _ESpellDictionary ESpellDictionary;
<<<<<<< HEAD
typedef struct _ESpellDictionaryPrivate ESpellDictionaryPrivate;
typedef struct _ESpellDictionaryClass ESpellDictionaryClass;
typedef struct _ESpellChecker ESpellChecker;

struct _ESpellDictionary {
	GObject parent;
	ESpellDictionaryPrivate *priv;
};

struct _ESpellDictionaryClass {
	GObjectClass parent_class;
};

GType		e_spell_dictionary_get_type	(void) G_GNUC_CONST;
ESpellDictionary *
		e_spell_dictionary_new		(ESpellChecker *spell_checker,
						 EnchantDict *enchant_dict);
guint		e_spell_dictionary_hash		(ESpellDictionary *dictionary);
gboolean	e_spell_dictionary_equal	(ESpellDictionary *dictionary1,
						 ESpellDictionary *dictionary2);
gint		e_spell_dictionary_compare	(ESpellDictionary *dictionary1,
						 ESpellDictionary *dictionary2);
const gchar *	e_spell_dictionary_get_name	(ESpellDictionary *dictionary);
const gchar *	e_spell_dictionary_get_code	(ESpellDictionary *dictionary);
ESpellChecker *	e_spell_dictionary_ref_spell_checker
						(ESpellDictionary *dictionary);
gboolean	e_spell_dictionary_check_word	(ESpellDictionary *dictionary,
						 const gchar *word,
						 gsize length);
void		e_spell_dictionary_learn_word	(ESpellDictionary *dictionary,
						 const gchar *word,
						 gsize length);
void		e_spell_dictionary_ignore_word	(ESpellDictionary *dictionary,
						 const gchar *word,
						 gsize length);
GList *		e_spell_dictionary_get_suggestions
						(ESpellDictionary *dictionary,
						 const gchar *word,
						 gsize length);
void		e_spell_dictionary_store_correction
						(ESpellDictionary *dictionary,
						 const gchar *misspelled,
						 gsize misspelled_length,
						 const gchar *correction,
						 gsize correction_length);
=======

#define E_TYPE_SPELL_DICTIONARY \
	(e_spell_dictionary_get_type ())


GType		e_spell_dictionary_get_type		(void);
const GList *	e_spell_dictionary_get_available	(void);

const ESpellDictionary *
		e_spell_dictionary_lookup
					(const gchar *language_code);
const gchar *	e_spell_dictionary_get_language_code
					(const ESpellDictionary *dictionary);
const gchar *	e_spell_dictionary_get_name
					(const ESpellDictionary *dictionary);
gint		e_spell_dictionary_compare
					(const ESpellDictionary *dict_a,
					 const ESpellDictionary *dict_b);
>>>>>>> Import classes for spell checking

G_END_DECLS

#endif /* E_SPELL_DICTIONARY_H */
