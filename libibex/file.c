/*
	Copyright 2000 Helix Code Inc.
*/

/* file.c: index file read/write ops */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ibex_internal.h"

static unsigned long read_number(FILE *f);
static void write_number(FILE *f, unsigned long n);
static char *get_compressed_word(FILE *f, char **lastword);

static gint free_file(gpointer key, gpointer value, gpointer data);
static void free_word(gpointer key, gpointer value, gpointer data);

/* The file format is:
 *
 * version string (currently "ibex1")
 * file count
 * list of compressed filenames, separated by \0
 * word count
 * list of compressed words, each followed by \0, a count, and that
 *   many references.
 *
 * All numbers are stored 7-bit big-endian, with the high bit telling
 * whether or not the number continues to the next byte.
 *
 * compressed text consists of a byte telling how many characters the
 * line has in common with the line before it, followed by the rest of
 * the string. Obviously this only really works if the lists are sorted.
 */
ibex *
ibex_open(char *file, gboolean create)
{
  ibex *ib;
  FILE *f;
  char vbuf[sizeof(IBEX_VERSION) - 1];
  char *word, *lastword;
  unsigned long nfiles, nwords, nrefs, ref;
  ibex_file **ibfs = NULL;
  int i;
  GPtrArray *refs;

  f = fopen(file, "r");
  if (!f && (errno != ENOENT || !create))
    {
      if (errno == 0)
	errno = ENOMEM;
      return NULL;
    }

  ib = g_malloc(sizeof(ibex));
  ib->dirty = FALSE;
  ib->path = g_strdup(file);
  ib->files = g_tree_new(strcmp);
  ib->words = g_hash_table_new(g_str_hash, g_str_equal);
  ib->oldfiles = g_ptr_array_new();

  if (!f)
    return ib;

  /* Check version. */
  if (fread(vbuf, 1, sizeof(vbuf), f) != sizeof(vbuf))
    {
      if (feof(f))
	errno = EINVAL;
      goto errout;
    }
  if (strncmp(vbuf, IBEX_VERSION, sizeof(vbuf) != 0))
    {
      errno = EINVAL;
      goto errout;
    }

  /* Read list of files. */
  nfiles = read_number(f);
  ibfs = g_malloc(nfiles * sizeof(ibex_file *));
  lastword = NULL;
  for (i = 0; i < nfiles; i++)
    {
      ibfs[i] = g_malloc(sizeof(ibex_file));
      ibfs[i]->name = get_compressed_word(f, &lastword);
      if (!ibfs[i]->name)
	goto errout;
      ibfs[i]->index = 0;
      g_tree_insert(ib->files, ibfs[i]->name, ibfs[i]);
    }

  /* Read list of words. */
  nwords = read_number(f);
  lastword = NULL;
  for (i = 0; i < nwords; i++)
    {
      word = get_compressed_word(f, &lastword);
      if (!word)
	goto errout;

      nrefs = read_number(f);
      refs = g_ptr_array_new();
      g_ptr_array_set_size(refs, nrefs);
      while (nrefs--)
	{
	  ref = read_number(f);
	  if (ref >= nfiles)
	    goto errout;
	  refs->pdata[nrefs] = ibfs[ref];
	}

      g_hash_table_insert(ib->words, word, refs);
    }

  g_free(ibfs);
  fclose(f);
  return ib;

errout:
  fclose(f);
  g_tree_traverse(ib->files, free_file, G_IN_ORDER, NULL);
  g_tree_destroy(ib->files);
  g_hash_table_foreach(ib->words, free_word, NULL);
  g_hash_table_destroy(ib->words);
  g_ptr_array_free(ib->oldfiles, TRUE);
  if (ibfs)
    g_free(ibfs);
  g_free(ib->path);
  g_free(ib);

  return NULL;
}

struct ibex_write_data {
  unsigned long index;
  FILE *f;
  char *lastname;
};

static int
get_prefix(struct ibex_write_data *iwd, char *name)
{
  int i = 0;
  if (iwd->lastname)
    {
      while (!strncmp(iwd->lastname, name, i + 1))
	i++;
    }
  iwd->lastname = name;
  return i;
}

static gint
write_file(gpointer key, gpointer value, gpointer data)
{
  char *file = key;
  ibex_file *ibf = value;
  struct ibex_write_data *iwd = data;
  int prefix;

  ibf->index = iwd->index++;
  prefix = get_prefix(iwd, file);
  fprintf(iwd->f, "%c%s", prefix, file + prefix);
  fputc(0, iwd->f);
  return FALSE;
}

static void
store_word(gpointer key, gpointer value, gpointer data)
{
  GTree *wtree = data;

  g_tree_insert(wtree, key, value);
}

static gint
write_word(gpointer key, gpointer value, gpointer data)
{
  char *word = key;
  GPtrArray *refs = value;
  struct ibex_write_data *iwd = data;
  ibex_file *ibf;
  int i, ind, prefix;

  for (i = ind = 0; i < refs->len; i++)
    {
      ibf = g_ptr_array_index(refs, i);
      if (ibf->index == -1)
	{
	  g_ptr_array_remove_index_fast(refs, i);
	  i--;
	}
      else
	ind++;
    }

  if (ind != 0)
    {
      prefix = get_prefix(iwd, word);
      fprintf(iwd->f, "%c%s", prefix, word + prefix);
      fputc(0, iwd->f);

      write_number(iwd->f, ind);

      for (i = 0; i < refs->len; i++)
	{
	  ibf = g_ptr_array_index(refs, i);
	  write_number(iwd->f, ibf->index);
	}
    }
  return FALSE;
}      

int
ibex_write(ibex *ib)
{
  struct ibex_write_data iwd;
  GTree *wtree;
  char *tmpfile;

  tmpfile = g_strdup_printf("%s~", ib->path);
  iwd.f = fopen(tmpfile, "w");
  if (!iwd.f)
    {
      if (errno == 0)
	errno = ENOMEM;
      g_free(tmpfile);
      return -1;
    }

  fputs(IBEX_VERSION, iwd.f);
  if (ferror(iwd.f))
    goto lose;

  iwd.index = 0;
  iwd.lastname = NULL;
  write_number(iwd.f, g_tree_nnodes(ib->files));
  if (ferror(iwd.f))
    goto lose;
  g_tree_traverse(ib->files, write_file, G_IN_ORDER, &iwd);
  if (ferror(iwd.f))
    goto lose;

  iwd.lastname = NULL;
  write_number(iwd.f, g_hash_table_size(ib->words));
  if (ferror(iwd.f))
    goto lose;
  wtree = g_tree_new(strcmp);
  g_hash_table_foreach(ib->words, store_word, wtree);
  g_tree_traverse(wtree, write_word, G_IN_ORDER, &iwd);
  g_tree_destroy(wtree);
  if (ferror(iwd.f))
    goto lose;

  if (fclose(iwd.f) == 0 && rename(tmpfile, ib->path) == 0)
    {
      g_free(tmpfile);
      ib->dirty = FALSE;
      return 0;
    }

lose:
  unlink(tmpfile);
  g_free(tmpfile);
  return -1;
}

int
ibex_close(ibex *ib)
{
  ibex_file *ibf;

  if (ib->dirty && ibex_write(ib) == -1)
    return -1;

  g_tree_traverse(ib->files, free_file, G_IN_ORDER, NULL);
  g_tree_destroy(ib->files);
  g_hash_table_foreach(ib->words, free_word, NULL);
  g_hash_table_destroy(ib->words);

  while (ib->oldfiles->len)
    {
      ibf = g_ptr_array_remove_index(ib->oldfiles, 0);
      g_free(ibf->name);
      g_free(ibf);
    }
  g_ptr_array_free(ib->oldfiles, TRUE);
  g_free(ib->path);
  g_free(ib);

  return 0;
}

static gint
free_file(gpointer key, gpointer value, gpointer data)
{
  ibex_file *ibf = value;

  g_free(ibf->name);
  g_free(ibf);
  return FALSE;
}

static void
free_word(gpointer key, gpointer value, gpointer data)
{
  g_free(key);
  g_ptr_array_free(value, TRUE);
}

static char *
get_compressed_word(FILE *f, char **lastword)
{
  char *buf, *p;
  int c, size;

  c = getc(f);
  if (c == EOF)
    return NULL;

  size = c + 10;
  buf = g_malloc(size);
  if (*lastword)
    strncpy(buf, *lastword, c);
  p = buf + c;
  do
    {
      c = getc(f);
      if (c == EOF)
	return NULL;
      if (p == buf + size)
	{
	  buf = g_realloc(buf, size + 10);
	  p = buf + size;
	  size += 10;
	}
      *p++ = c;
    }
  while (c != 0);

  *lastword = buf;
  return buf;
}

static void
write_number(FILE *f, unsigned long number)
{
  int i, flag = 0;
  char buf[4];

  i = 4;
  do
    {
      buf[--i] = (number & 0x7F) | flag;
      number = number >> 7;
      flag = 0x80;
    }
  while (number != 0);

  fwrite(buf + i, 1, 4 - i, f);
}

static unsigned long
read_number(FILE *f)
{
  int byte;
  unsigned long num;

  num = 0;
  do
    {
      byte = getc(f);
      num = num << 7 | (byte & 0x7F);
    }
  while (byte & 0x80);
  
  return num;
}

