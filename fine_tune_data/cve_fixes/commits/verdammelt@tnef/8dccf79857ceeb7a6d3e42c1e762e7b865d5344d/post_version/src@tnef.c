/*
 * tnef.c -- extract files from microsoft TNEF format
 *
 * Copyright (C)1999-2006 Mark Simpson <damned@theworld.com>
 * Copyright (C)1997 Thomas Boll  <tb@boll.ch>	[ORIGINAL AUTHOR]
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
 * Commentary:
 *       scans tnef file and extracts all attachments
 *       attachments are written to their original file-names if possible
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include "common.h"

#include "tnef.h"

#include "alloc.h"
#include "attr.h"
#include "debug.h"
#include "file.h"
#include "mapi_attr.h"
#include "options.h"
#include "path.h"
#include "rtf.h"
#include "util.h"

typedef struct
{
    VarLenData **text_body;
    VarLenData **html_bodies;
    VarLenData **rtf_bodies;
} MessageBody;

typedef enum
{
    TEXT = 't',
    HTML = 'h',
    RTF = 'r'
} MessageBodyTypes;

/* Reads and decodes a object from the stream */

static Attr*
read_object (FILE *in)
{
    Attr *attr = NULL;

    /* peek to see if there is more to read from this stream */
    int tmp_char = fgetc(in);
    if (tmp_char == -1) return NULL;
    ungetc(tmp_char, in);

    attr = attr_read (in);

    return attr;
}

static void 
free_bodies(VarLenData **bodies, int len)
{
    while (len--)
    {
        XFREE(bodies[len]->data);
        XFREE(bodies[len]);
    }
}

static File**
get_body_files (const char* filename,
		const char pref,
		const MessageBody* body)
{
    File **files = NULL;
    VarLenData **data;
    char *ext = "";
    char *type = "unknown";
    int i;

    switch (pref)
    {
    case 'r':
	data = body->rtf_bodies;
	ext = ".rtf";
            type = "text/rtf";
	break;
    case 'h':
	data = body->html_bodies;
	ext = ".html";
            type = "text/html";
	break;
    case 't':
	data = body->text_body;
	ext = ".txt";
            type = "text/plain";
	break;
    default:
	data = NULL;
	break;
    }

    if (data)
    {
	int count = 0;
	char *tmp 
	    = CHECKED_XCALLOC(char, 
			      strlen(filename) + strlen(ext) + 1);
	strcpy (tmp, filename);
	strcat (tmp, ext);

        char *mime = CHECKED_XCALLOC(char, strlen(type) + 1);
        strcpy (mime, type);

	/* first get a count */
	while (data[count++]);

	files = (File**)XCALLOC(File*, count + 1);
	for (i = 0; data[i]; i++)
	{
	    files[i] = (File*)XCALLOC(File, 1);
	    files[i]->name = tmp;
            files[i]->mime_type = mime;
	    files[i]->len = data[i]->len;
	    files[i]->data 
		= CHECKED_XMALLOC(unsigned char, data[i]->len);
	    memmove (files[i]->data, data[i]->data, data[i]->len);
	}
    }
    return files;
}

static VarLenData**
get_text_data (Attr *attr)
{
    VarLenData **body = XCALLOC(VarLenData*, 2);

    body[0] = XCALLOC(VarLenData, 1);
    body[0]->len = attr->len;
    body[0]->data = CHECKED_XCALLOC(unsigned char, attr->len);
    memmove (body[0]->data, attr->buf, attr->len);
    return body;
}

static VarLenData**
get_html_data (MAPI_Attr *a)
{
    VarLenData **body = XCALLOC(VarLenData*, a->num_values + 1);

    int j;
    for (j = 0; j < a->num_values; j++)
    {
        if (a->type == szMAPI_BINARY) {
 	    body[j] = XMALLOC(VarLenData, 1);
	    body[j]->len = a->values[j].len;
	    body[j]->data = CHECKED_XCALLOC(unsigned char, a->values[j].len);
	    memmove (body[j]->data, a->values[j].data.buf, body[j]->len);
        }
    }
    return body;
}

int
data_left (FILE* input_file)
{
    int retval = 1;
    
    if (feof(input_file)) retval = 0;
    else if (input_file != stdin)
    {
	/* check if there is enough data left */
	struct stat statbuf;
	size_t pos, data_left;
	fstat (fileno(input_file), &statbuf);
	pos = ftell(input_file);
	data_left = (statbuf.st_size - pos);

	if (data_left > 0 && data_left < MINIMUM_ATTR_LENGTH) 
	{
	    if ( CRUFT_SKIP )
	    {
		/* look for specific flavor of cruft -- trailing "\r\n" */

		if ( data_left == 2 )
		{
		    int c = fgetc( input_file );

		    if ( c < 0 )	/* this should never happen */
		    {
			fprintf( stderr, "ERROR: confused beyond all redemption.\n" );
			exit (1);
		    }

		    ungetc( c, input_file );

		    if ( c == 0x0d )		/* test for "\r" part of "\r\n" */
		    {
			/* "trust" that next char is 0x0a and ignore this cruft */

			if ( VERBOSE_ON )
			    fprintf( stderr, "WARNING: garbage at end of file (ignored)\n" );

			if ( DEBUG_ON )
			    debug_print( "!!garbage at end of file (ignored)\n" );
		    }
		    else
		    {
			fprintf( stderr, "ERROR: garbage at end of file.\n" );
		    }
		}
		else
		{
		    fprintf (stderr, "ERROR: garbage at end of file.\n");
		}
	    }
	    else
	    {
		fprintf (stderr, "ERROR: garbage at end of file.\n");
	    }

	    retval = 0;
	}
    }
    return retval;
}


/* The entry point into this module.  This parses an entire TNEF file. */
int
parse_file (FILE* input_file, char* directory, 
	    char *body_filename, char *body_pref,
	    int flags)
{
    uint32 d;
    uint16 key;
    Attr *attr = NULL;
    File *file = NULL;
    int rtf_size = 0, html_size = 0;
    MessageBody body;
    memset (&body, '\0', sizeof (MessageBody));

    /* store the program options in our file global variables */
    g_flags = flags;

    /* check that this is in fact a TNEF file */
    d = geti32(input_file);
    if (d != TNEF_SIGNATURE)
    {
	fprintf (stdout, "Seems not to be a TNEF file\n");
	return 1;
    }

    /* Get the key */
    key = geti16(input_file);
    debug_print ("TNEF Key: %hx\n", key);

    /* The rest of the file is a series of 'messages' and 'attachments' */
    while ( data_left( input_file ) )
    {
	attr = read_object( input_file );

	if ( attr == NULL ) break;

	/* This signals the beginning of a file */
	if (attr->name == attATTACHRENDDATA)
	{
	    if (file)
	    {
		file_write (file, directory);
		file_free (file);
	    }
	    else
	    {
		file = CHECKED_XCALLOC (File, 1);
	    }
	}

	/* Add the data to our lists. */
	switch (attr->lvl_type)
	{
	case LVL_MESSAGE:
	    if (attr->name == attBODY)
	    {
		body.text_body = get_text_data (attr);
	    }
	    else if (attr->name == attMAPIPROPS) 
	    { 
		MAPI_Attr **mapi_attrs 
		    = mapi_attr_read (attr->len, attr->buf); 
		if (mapi_attrs)
		{ 
		    int i;
		    for (i = 0; mapi_attrs[i]; i++)
		    {
			MAPI_Attr *a = mapi_attrs[i];
		
			if (a->type == szMAPI_BINARY && a->name == MAPI_BODY_HTML)
			{
			    body.html_bodies = get_html_data (a);
                                html_size = a->num_values;
			}
			else if (a->type == szMAPI_BINARY && a->name == MAPI_RTF_COMPRESSED)
			{
			    body.rtf_bodies = get_rtf_data (a);
                                rtf_size = a->num_values;
			}
		    }
		    /* cannot save attributes to file, since they
		     * are not attachment attributes */ 
		    /* file_add_mapi_attrs (file, mapi_attrs); */
		    mapi_attr_free_list (mapi_attrs); 
		    XFREE (mapi_attrs); 
		}
	    }
	    break;
	case LVL_ATTACHMENT:
	    file_add_attr (file, attr);
	    break;
	default:
	    fprintf (stderr, "Invalid lvl type on attribute: %d\n",
		     attr->lvl_type);
	    return 1;
	    break;
	}
	attr_free (attr);
	XFREE (attr);
    }

    if (file)
    {
	file_write (file, directory);
	file_free (file);
	XFREE (file);
    }
    
    /* Write the message body */
    if (flags & SAVEBODY)
    {
	int i = 0;
	int all_flag = 0;
	if (strcmp (body_pref, "all") == 0) 
	{
	    all_flag = 1;
	    body_pref = "rht";
	}

	for (; i < 3; i++)
	{
	    File **files
		= get_body_files (body_filename, body_pref[i], &body);
	    if (files)
	    {
		int j = 0; 
		for (; files[j]; j++)
		{
		    file_write(files[j], directory);
		    file_free (files[j]);
                    XFREE(files[j]);
		}
		XFREE(files);
		if (!all_flag) break;
	    }
	}
    }

    if (body.text_body)
    {
        free_bodies(body.text_body, 1);
        XFREE(body.text_body);
    }
    if (rtf_size > 0)
    {
        free_bodies(body.rtf_bodies, rtf_size);
        XFREE(body.rtf_bodies);
    }
    if (html_size > 0)
    {
        free_bodies(body.html_bodies, html_size);
        XFREE(body.html_bodies);
    }
    return 0;
}

