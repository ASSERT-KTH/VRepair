/*
 * file.c -- functions for dealing with file output
 *
 * Copyright (C)1999-2006 Mark Simpson <damned@theworld.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can either send email to this
 * program's maintainer or write to: The Free Software Foundation,
 * Inc.; 59 Temple Place, Suite 330; Boston, MA 02111-1307, USA.
 *
 */
#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */

#include "common.h"

#include "alloc.h"
#include "date.h"
#include "debug.h"
#include "file.h"
#include "mapi_attr.h"
#include "options.h"
#include "path.h"

#define TNEF_DEFAULT_FILENAME "tnef-tmp"

/* ask user for confirmation of the action */
static int
confirm_action (const char *prompt, ...)
{
    if (INTERACTIVE)
    {
	int confirmed = 0;
	char buf[BUFSIZ + 1];
	va_list args;
	va_start (args, prompt);

	VPRINTF(stdout, prompt, args);
	fgets (buf, BUFSIZ, stdin);
	if (buf[0] == 'y' || buf[0] == 'Y') confirmed = 1;

	va_end (args);

	return confirmed;
    }
    return 1;
}

void
file_write (File *file, const char* directory)
{
    char *path = NULL;

    assert (file);
    if (!file) return;

    if (file->name == NULL)
    {
	file->name = strdup( TNEF_DEFAULT_FILENAME );
	debug_print ("No file name specified, using default %s.\n", TNEF_DEFAULT_FILENAME);
    }

    if ( file->path == NULL )
    {
	file->path = munge_fname( file->name );

	if (file->path == NULL)
	{
	    file->path = strdup( TNEF_DEFAULT_FILENAME );
	    debug_print ("No path name available, using default %s.\n", TNEF_DEFAULT_FILENAME);
	}
    }

    path = concat_fname( directory, file->path );

    if (path == NULL)
    {
	path = strdup( TNEF_DEFAULT_FILENAME );
	debug_print ("No path generated, using default %s.\n", TNEF_DEFAULT_FILENAME);
    }

    debug_print ("%sWRITING\t|\t%s\t|\t%s\n",
		 ((LIST_ONLY==0)?"":"NOT "), file->name, path);

    if (!LIST_ONLY)
    {
	FILE *fp = NULL;

	if (!confirm_action ("extract %s?", file->name)) return;
	if (!OVERWRITE_FILES)
	{
	    if (file_exists (path))
	    {
		if (!NUMBER_FILES)
		{
		    fprintf (stderr,
			     "tnef: %s: Could not create file: File exists\n",
			     path);
		    return;
		}
		else
		{
		    char *tmp = find_free_number (path);
		    debug_print ("Renaming %s to %s\n", path, tmp);
		    XFREE (path);
		    path = tmp;
		}
	    }
	}

	fp = fopen (path, "wb");
	if (fp == NULL)
	{
	    perror (path);
	    exit (1);
	}
	if (fwrite (file->data, 1, file->len, fp) != file->len)
	{
	    perror (path);
	    exit (1);
	}
	fclose (fp);
    }

    if (LIST_ONLY || VERBOSE_ON)
    {
	if (LIST_ONLY && VERBOSE_ON)
	{
	    /* FIXME: print out date and stuff */
	    const char *date_str = date_to_str(&file->dt);
	    fprintf (stdout, "%11lu\t|\t%s\t|\t%s\t|\t%s", 
		     (unsigned long)file->len,
		     date_str+4, /* skip the day of week */
		     file->name,
		     path);
	}
	else
	{
            fprintf (stdout, "%s\t|\t%s", file->name, path);
	}
	if ( SHOW_MIME )
	{
	    fprintf (stdout, "\t|\t%s", file->mime_type ? file->mime_type : "unknown");
            fprintf (stdout, "\t|\t%s", file->content_id ? file->content_id : "");
	}
        fprintf (stdout, "\n");
    }
    XFREE(path);
}

static void
file_add_mapi_attrs (File* file, MAPI_Attr** attrs)
{
    int i;
    for (i = 0; attrs[i]; i++)
    {
	MAPI_Attr* a = attrs[i];

	if (a->num_values)
	{

	    switch (a->name)
	    {
	    case MAPI_ATTACH_LONG_FILENAME:
		assert(a->type == szMAPI_STRING);
		if (file->name) XFREE(file->name);
		file->name = strdup( (char*)a->values[0].data.buf );
		break;

	    case MAPI_ATTACH_DATA_OBJ:
		assert((a->type == szMAPI_BINARY) || (a->type == szMAPI_OBJECT));
		file->len = a->values[0].len;
		if (file->data) XFREE (file->data);
		file->data = CHECKED_XMALLOC (unsigned char, file->len);
		memmove (file->data, a->values[0].data.buf, file->len);
		break;

             case MAPI_ATTACH_MIME_TAG:
		assert(a->type == szMAPI_STRING);
		if (file->mime_type) XFREE (file->mime_type);
		file->mime_type = CHECKED_XMALLOC (char, a->values[0].len);
		memmove (file->mime_type, a->values[0].data.buf, a->values[0].len);
		break;

                case MAPI_ATTACH_CONTENT_ID:
                    assert(a->type == szMAPI_STRING);
                    if (file->content_id) XFREE(file->content_id);
                    file->content_id = CHECKED_XMALLOC (char, a->values[0].len);
                    memmove (file->content_id, a->values[0].data.buf, a->values[0].len);
                    break;

	    default:
		break;
	    }
	}
    }
}

void
file_add_attr (File* file, Attr* attr)
{
    assert (file && attr);
    if (!(file && attr)) return;

    /* we only care about some things... we will skip most attributes */
    switch (attr->name)
    {
    case attATTACHMODIFYDATE:
	copy_date_from_attr (attr, &file->dt);
	break;

    case attATTACHMENT:
    {
	MAPI_Attr **mapi_attrs = mapi_attr_read (attr->len, attr->buf);
	if (mapi_attrs)
	{
	    file_add_mapi_attrs (file, mapi_attrs);
	    mapi_attr_free_list (mapi_attrs);
	    XFREE (mapi_attrs);
	}
    }
    break;

    case attATTACHTITLE:
	file->name = strdup( (char*)attr->buf );
	break;

    case attATTACHDATA:
	file->len = attr->len;
	file->data = CHECKED_XMALLOC(unsigned char, attr->len);
	memmove (file->data, attr->buf, attr->len);
	break;

    default:
	break;
    }
}

void
file_free (File *file)
{
    if (file)
    {
	XFREE (file->name);
	XFREE (file->data);
	XFREE (file->mime_type);
        XFREE (file->content_id);
        XFREE (file->path);
	memset (file, '\0', sizeof (File));
    }
}

