/*
 * This file is part of the hildon-application-manager.
 *
 * Copyright (C) 2007 Nokia Corporation.  All Rights reserved.
 *
 * Contact: Marius Vollmer <marius.vollmer@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#include "xexp.h"

struct xexp {
  char *tag;
  xexp *rest;
  xexp *first;
  char *text;
};

xexp *
xexp_rest (xexp *x)
{
  return x->rest;
}

void
xexp_free_1 (xexp *x)
{
  g_free (x->tag);
  g_free (x->text);
  g_free (x);
}

void
xexp_free (xexp *x)
{
  while (x)
    {
      xexp *r = xexp_rest (x);
      xexp_free (x->first);
      xexp_free_1 (x);
      x = r;
    }
}

const char *
xexp_tag (xexp *x)
{
  return x->tag;
}

int
xexp_is (xexp *x, const char *tag)
{
  return strcmp (x->tag, tag) == 0;
}

xexp *
xexp_list_new (const char *tag, xexp *first, xexp *rest)
{
  g_assert (tag);

  xexp *x = g_new0 (xexp, 1);
  x->tag = g_strdup (tag);
  x->text = NULL;
  x->first = first;
  x->rest = rest;
  return x;
}

int
xexp_is_list (xexp *x)
{
  return x->text == NULL;
}

xexp *
xexp_first (xexp *x)
{
  g_assert (xexp_is_list (x));
  return x->first;
}

void
xexp_prepend (xexp *x, xexp *y)
{
  xexp **yptr;
  g_assert (xexp_is_list (x));
  
  yptr = &y->rest;
  while (*yptr)
    yptr = &(*yptr)->rest;
  *yptr = x->first;
  x->first = y;
}

void
xexp_append (xexp *x, xexp *y)
{
  xexp **yptr;
  g_assert (xexp_is_list (x));

  yptr = &x->first;
  while (*yptr)
    yptr = &(*yptr)->rest;
  *yptr = y;
}

void
xexp_del (xexp *x, xexp *z)
{
  xexp **yptr;

  g_assert (xexp_is_list (x));
  yptr = &x->first;
  while (*yptr)
    {
      xexp *y = *yptr;
      if (y == z)
	{
	  *yptr = y->rest;
	  y->rest = NULL;
	  xexp_free (y);
	  return;
	}
      yptr = &(*yptr)->rest;
    }
}

void
xexp_set_rest (xexp *x, xexp *y)
{
  xexp_free (x->rest);
  x->rest = y;
}

int
xexp_length (xexp *x)
{
  int len;
  xexp *y;

  g_assert (xexp_is_list (x));
  for (len = 0, y = xexp_first (x); y; len++, y = xexp_rest (y))
    ;
  return len;
}

xexp *
xexp_text_new (const char *tag, const char *text, xexp *rest)
{
  g_assert (tag);
  g_assert (text);

  xexp *x = g_new0 (xexp, 1);
  x->tag = g_strdup (tag);
  x->text = g_strdup (text);
  x->first = NULL;
  x->rest = rest;
  return x;
}

int
xexp_is_text (xexp *x)
{
  return x->text != NULL;
}

const char *
xexp_text (xexp *x)
{
  g_assert (xexp_is_text (x));
  return x->text;
}

int
xexp_text_as_int (xexp *x)
{
  return atoi (xexp_text (x));
}

xexp *
xexp_aref (xexp *x, const char *tag)
{
  xexp *y = xexp_first (x);
  while (y)
    {
      if (xexp_is (y, tag))
	return y;
      y = xexp_rest (y);
    }
  return NULL;
}

const char *
xexp_aref_text (xexp *x, const char *tag)
{
  xexp *y = xexp_aref (x, tag);
  if (y && xexp_is_text (y))
    return xexp_text (y);
  else
    return NULL;
}

int
xexp_aref_bool (xexp *x, const char *tag)
{
  return xexp_aref (x, tag) != NULL;
}

void
xexp_aset (xexp *x, xexp *val)
{
  xexp_adel (x, xexp_tag (val));
  xexp_prepend (x, val);
}

void
xexp_aset_bool (xexp *x, const char *tag, int val)
{
  if (val)
    xexp_aset (x, xexp_list_new (tag, NULL, NULL));
  else
    xexp_adel (x, tag);
}

void
xexp_aset_text (xexp *x, const char *tag, const char *val)
{
  if (val)
    xexp_aset (x, xexp_text_new (tag, val, NULL));
  else
    xexp_adel (x, tag);
}

void
xexp_adel (xexp *x, const char *tag)
{
  xexp **yptr;

  g_assert (xexp_is_list (x));
  yptr = &x->first;
  while (*yptr)
    {
      xexp *y = *yptr;
      if (xexp_is (y, tag))
	{
	  *yptr = y->rest;
	  y->rest = NULL;
	  xexp_free (y);
	  return;
	}
      yptr = &(*yptr)->rest;
    }
}

void
transmogrify_text_to_list (xexp *x)
{
  g_free (x->text);
  x->text = NULL;
}

void
transmogrify_list_to_text (xexp *x, const char *text)
{
  g_assert (text);
  g_assert (x->first == NULL);
  x->text = g_strdup (text);
}

/** Parsing */

typedef struct {
  xexp *result;
  GSList *stack;
} xexp_parse_context;

static void
ignore_text (const char *text, GError **error)
{
  const char *p = text;
  while (*p && isspace (*p))
    p++;

  if (*p)
    g_set_error (error, 
		 G_MARKUP_ERROR,
		 G_MARKUP_ERROR_INVALID_CONTENT,
		 "Unexpected text: %s", text);
}

static void
xexp_parser_start_element (GMarkupParseContext *context,
			   const gchar         *element_name,
			   const gchar        **attribute_names,
			   const gchar        **attribute_values,
			   gpointer             user_data,
			   GError             **error)
{
  xexp_parse_context *xp = (xexp_parse_context *)user_data;

  xexp *x = xexp_list_new (element_name, NULL, NULL);
  if (xp->stack)
    {
      /* If the current node is a text, it must be all whitespace and
	 we turn it into a list node.
      */
      xexp *y = (xexp *)xp->stack->data;
      if (xexp_is_text (y))
	{
	  ignore_text (xexp_text (y), error);
	  transmogrify_text_to_list (y);
	}

      xexp_append (y, x);
    }
  xp->stack = g_slist_prepend (xp->stack, x);
}

static void
xexp_parser_end_element (GMarkupParseContext *context,
			 const gchar         *element_name,
			 gpointer             user_data,
			 GError             **error)
{
  xexp_parse_context *xp = (xexp_parse_context *)user_data;

  GSList *top = xp->stack;
  xexp *x = (xexp *)top->data;
  xp->stack = top->next;
  g_slist_free_1 (top);

  if (xp->stack == NULL)
    xp->result = x;
}

static void
xexp_parser_text (GMarkupParseContext *context,
		  const gchar         *text,
		  gsize                text_len,
		  gpointer             user_data,
		  GError             **error)
{
  xexp_parse_context *xp = (xexp_parse_context *)user_data;

  /* If the current xexp is has already children, TEXT must be all
     whitespace and we ignore it.  Otherwise, we turn the current node
     into text.
  */
  
  if (xp->stack)
    {
      xexp *y = (xexp *)xp->stack->data;
      if (xexp_first (y))
	ignore_text (text, error);
      else
	transmogrify_list_to_text (y, text);
    }
}

static GMarkupParser xexp_markup_parser = {
  xexp_parser_start_element,
  xexp_parser_end_element,
  xexp_parser_text,
  NULL,
  NULL
};

xexp *
xexp_read (FILE *f, GError **error)
{
  xexp_parse_context xp;
  GMarkupParseContext *ctxt;
  gchar buf[1];
  size_t n;

  xp.stack = NULL;
  xp.result = NULL;
  ctxt = g_markup_parse_context_new (&xexp_markup_parser, 0, &xp, NULL);

  while (xp.result == NULL && (n = fread (buf, 1, 1, f)) > 0)
    {
      if (!g_markup_parse_context_parse (ctxt, buf, n, error))
	goto error;
    }

  if (!g_markup_parse_context_end_parse (ctxt, error))
    goto error;
    

  g_assert (xp.result && xp.stack == NULL);
		   
  return xp.result;

 error:
  if (xp.stack)
    {
      xexp *top = (xexp *) g_slist_last (xp.stack)->data;
      xexp_free (top);
    }
  else if (xp.result)
    xexp_free (xp.result);
  return NULL;
}

xexp *
xexp_read_file (const char *filename)
{
  FILE *f = fopen (filename, "r");
  if (f)
    {
      GError *error = NULL;
      xexp *x = xexp_read (f, &error);
      fclose (f);
      if (error)
	{
	  fprintf (stderr, "%s: %s\n", filename, error->message);
	  g_error_free (error);
	}
      return x;
    }
  fprintf (stderr, "%s: %s\n", filename, strerror (errno));
  return NULL;
}

/** Writing */

static void
xexp_fprintf_escaped (FILE *f, const char *fmt, ...)
{
  va_list args;
  char *str;

  va_start (args, fmt);
  str = g_markup_vprintf_escaped (fmt, args);
  va_end (args);
  fputs (str, f);
  g_free (str);
}

static void
write_blanks (FILE *f, int level)
{
  static const char blanks[] = "                ";
  if (level > sizeof(blanks)-1)
    level = sizeof(blanks)-1;
  fputs (blanks + sizeof(blanks)-1 - level, f);
}

static void
xexp_write_1 (FILE *f, xexp *x, int level)
{
  xexp *y;

  write_blanks (f, level);

  if (xexp_is_list (x))
    {
      y = xexp_first (x);
      if (y == NULL)
	xexp_fprintf_escaped (f, "<%s/>\n", xexp_tag (x));
      else 
	{
	  xexp_fprintf_escaped (f, "<%s>\n", xexp_tag (x));
	  for (; y; y = xexp_rest (y))
	    xexp_write_1 (f, y, level + 1);
	  write_blanks (f, level);
	  xexp_fprintf_escaped (f, "</%s>\n", xexp_tag (x));
	}
    }
  else
    xexp_fprintf_escaped (f, "<%s>%s</%s>\n",
			  xexp_tag (x), xexp_text (x), xexp_tag (x));
}

void
xexp_write (FILE *f, xexp *x)
{
  xexp_write_1 (f, x, 0);
}

int
xexp_write_file (const char *filename, xexp *x)
{
  char *tmp_filename = g_strdup_printf ("%s#%d", filename, getpid);
  FILE *f = fopen (tmp_filename, "w");

  if (f == NULL)
    goto error;

  xexp_write (f, x);
  if (ferror (f))
    goto error;

  if (fclose (f) < 0)
    {
      f = NULL;
      goto error;
    }
  f = NULL;

  if (rename (tmp_filename, filename) < 0)
    goto error;
  
  g_free (tmp_filename);
  return 1;

 error:
  fprintf (stderr, "%s: %s\n", filename, strerror (errno));
  if (f)
    fclose (f);
  if (tmp_filename != filename)
    g_free (tmp_filename);
  return 0;
}
