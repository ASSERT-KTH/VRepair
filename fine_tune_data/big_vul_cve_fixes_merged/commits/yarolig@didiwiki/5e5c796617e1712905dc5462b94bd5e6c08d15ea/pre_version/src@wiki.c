/* 
 *  DidiWiki - a small lightweight wiki engine. 
 *
 *  Copyright 2004 Matthew Allum <mallum@o-hand.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "didi.h"
#include "wikitext.h"

static char* CssData = STYLESHEET;

static char *
get_line_from_string(char **lines, int *line_len)
{
  int   i;
  char *z = *lines;

  if( z[0] == '\0' ) return NULL;

  for (i=0; z[i]; i++)
    {
      if (z[i] == '\n')
	{
	  if (i > 0 && z[i-1]=='\r')
	    { z[i-1] = '\0'; }
	  else
	    { z[i] = '\0'; }
	  i++;
	  break;
	}
    }

  /* advance lines on */
  *lines      = &z[i];
  *line_len -= i;

  return z;
}

static char*
check_for_link(char *line, int *skip_chars)
{
  char *start =  line;
  char *p     =  line;
  char *url   =  NULL;
  char *title =  NULL;
  char *result = NULL;
  int   found = 0;

  if (*p == '[') 		/* [ link [title] ] */
    {
      /* XXX TODO XXX 
       * Allow links like [the Main page] ( covert to the_main_page )
       */
      url = start+1; *p = '\0'; p++;
      while (  *p != ']' && *p != '\0' && !isspace(*p) ) p++;

      if (isspace(*p))
	{
	  *p = '\0';
	  title = ++p; 
	  while (  *p != ']' && *p != '\0' ) 
	    p++;
	}

      *p = '\0';
      p++;
    }                     
  else if (!strncasecmp(p, "http://", 7)
	   || !strncasecmp(p, "mailto://", 9)
	   || !strncasecmp(p, "file://", 7))
    {
      while ( *p != '\0' && !isspace(*p) ) p++;


      found = 1;
    }
  else if (isupper(*p))      	/* Camel-case */
    {
      int num_upper_char = 1;
      p++;
      while ( *p != '\0' && isalnum(*p) )
	{
	  if (isupper(*p))
	    { found = 1; num_upper_char++; }
	  p++;
	}

      if (num_upper_char == (p-start)) /* Dont make ALLCAPS links */
	return NULL;
    }

  if (found)  /* cant really set http/camel links in place */
    {
      url = malloc(sizeof(char) * ((p - start) + 2) );
      memset(url, 0, sizeof(char) * ((p - start) + 2));
      strncpy(url, start, p - start);
      *start = '\0';
    }

  if (url != NULL)
    {
      int len = strlen(url);

      *skip_chars = p - start;

      /* is it an image ? */
      if (!strncmp(url+len-4, ".gif", 4) || !strncmp(url+len-4, ".png", 4) 
	  || !strncmp(url+len-4, ".jpg", 4) || !strncmp(url+len-5, ".jpeg", 5))
	{
	  if (title)
	    asprintf(&result, "<a href='%s'><img src='%s' border='0'></a>",
		     title, url);
	  else
	    asprintf(&result, "<img src='%s' border='0'>", url);
	}
      else
	{
	  char *extra_attr = "";

	  if (!strncasecmp(url, "http://", 7))
	    extra_attr = " title='WWW link' ";

	  if (title)
	    asprintf(&result,"<a %s href='%s'>%s</a>", extra_attr, url, title);
	  else
	    asprintf(&result, "<a %s href='%s'>%s</a>", extra_attr, url, url);
	}

      

      return result;
    }

  return NULL;
}


static char *
file_read(char *filename)
{
  struct stat st;
  FILE*       fp;
  char*       str;
  int         len;

  /* Get the file size. */
  if (stat(filename, &st)) 
    return NULL;

  if (!(fp = fopen(filename, "rb"))) 
    return NULL;
  
  str = (char *)malloc(sizeof(char)*(st.st_size + 1));
  len = fread(str, 1, st.st_size, fp);
  if (len >= 0) str[len] = '\0';
  
  fclose(fp);

  return str;
}


static int
file_write(char *filename, char *data)
{
  FILE*       fp;
  int         bytes_written = 0;
  int         len           = strlen(data); /* clip off extra '\0' */

  if (!(fp = fopen(filename, "wb"))) 
    return -1;
 
  while ( len > 0 )
    {
      bytes_written = fwrite(data, sizeof(char), len, fp);
      len = len - bytes_written;
      data = data + bytes_written;
    }

  fclose(fp);

  return 1;
}

static int
is_wiki_format_char_or_space(char c)
{
  if (isspace(c)) return 1;
  if (strchr("/*_-", c)) return 1; 
  return 0;
}

void
wiki_print_data_as_html(HttpResponse *res, char *raw_page_data)
{
  char *p = raw_page_data;	    /* accumalates non marked up text */
  char *q = NULL, *link = NULL; /* temporary scratch stuff */
  char *line = NULL;
  int   line_len;
  int   i, j, skip_chars;

  /* flags, mainly for open tag states */
  int bold_on      = 0;
  int italic_on    = 0;
  int underline_on = 0;
  int strikethrough_on = 0;
  int open_para    = 0;
  int pre_on       = 0;
  int table_on     = 0;

#define ULIST 0
#define OLIST 1
#define NUM_LIST_TYPES 2

  struct { char ident; int  depth; char *tag; } listtypes[] = {
    { '*', 0, "ul" },
    { '#', 0, "ol" }
  };


  q = p;  /* p accumalates non marked up text, q is just a pointer
	   * to the end of the current line - used by below func. 
	   */

  while ( (line = get_line_from_string(&q, &line_len)) )
    {
      int   header_level = 0; 
      char *line_start   = line;
      int   skip_to_content = 0;
      /*
       *  process any initial wiki chars at line beginning 
       */

      if (pre_on && !isspace(*line) && *line != '\0')
	{
	  /* close any preformatting if already on*/
	  http_response_printf(res, "\n</pre>\n") ;
	  pre_on = 0;
	}

      /* Handle ordered & unordered list, code is a bit mental.. */
      for (i=0; i<NUM_LIST_TYPES; i++)
	{

	  /* extra checks avoid bolding */
	  if ( *line == listtypes[i].ident
	       && ( *(line+1) == listtypes[i].ident || isspace(*(line+1)) ) ) 
	    {                     	
	      int item_depth = 0;

	      if (listtypes[!i].depth)
		{
		  for (j=0; j<listtypes[!i].depth; j++)
		    http_response_printf(res, "</%s>\n", listtypes[!i].tag);
		  listtypes[!i].depth = 0;
		}

	      while ( *line == listtypes[i].ident ) { line++; item_depth++; }
	  
	      if (item_depth < listtypes[i].depth)
		{
		  for (j = 0; j < (listtypes[i].depth - item_depth); j++)
		    http_response_printf(res, "</%s>\n", listtypes[i].tag);
		}
	      else
		{
		  for (j = 0; j < (item_depth - listtypes[i].depth); j++)
		    http_response_printf(res, "<%s>\n", listtypes[i].tag);
		}
	      
	      http_response_printf(res, "<li>");
	      
	      listtypes[i].depth = item_depth;
	      
	      skip_to_content = 1;
	    }
	  else if (listtypes[i].depth && !listtypes[!i].depth) 
	    {
	      /* close current list */

	      for (j=0; j<listtypes[i].depth; j++)
		http_response_printf(res, "</%s>\n", listtypes[i].tag);
	      listtypes[i].depth = 0;
	    }
	}

      if (skip_to_content)
	goto line_content; /* skip parsing any more initial chars */

      /* Tables */

      if (*line == '|')
        {
	  if (table_on==0)
	    http_response_printf(res, "<table class='wikitable' cellspacing='0' cellpadding='4'>\n");
	  line++;

	    http_response_printf(res, "<tr><td>");

	  table_on = 1;
	  goto line_content;
        }
      else
        {
	  if(table_on)
	    {
	      http_response_printf(res, "</table>\n");
	      table_on = 0;
	    }
        }

      /* pre formated  */

      if ( (isspace(*line) || *line == '\0'))
	{
	  int n_spaces = 0;

	  while ( isspace(*line) ) { line++; n_spaces++; }

	  if (*line == '\0')  /* empty line - para */
	    {
	      if (pre_on)
		{
		  http_response_printf(res, "\n") ;
		  continue;
		}
	      else if (open_para)
		{
		  http_response_printf(res, "\n</p><p>\n") ;
		}
	      else
		{
		  http_response_printf(res, "\n<p>\n") ;
		  open_para = 1;
		}
	    }
	  else /* starts with space so Pre formatted, see above for close */
	    {
	      if (!pre_on)
		http_response_printf(res, "<pre>\n") ;
	      pre_on = 1;
	      line = line - ( n_spaces - 1 ); /* rewind so extra spaces
                                                 they matter to pre */
	      http_response_printf(res, "%s\n", line);
	      continue;
	    }
	}
      else if ( *line == '=' )
	{
	  while (*line == '=')
	    { header_level++; line++; }

	  http_response_printf(res, "<h%d>", header_level);
	  p = line;
	}
      else if ( *line == '-' && *(line+1) == '-' )
	{
	  /* rule */
	  http_response_printf(res, "<hr/>\n");
	  while ( *line == '-' ) line++;
	}

    line_content:

      /* 
       * now process rest of the line 
       */

      p = line;

      while ( *line != '\0' )
	{
	  if ( *line == '!' && !isspace(*(line+1))) 
	    {                	/* escape next word - skip it */
	      *line = '\0';
	      http_response_printf(res, "%s", p);
	      p = ++line;

	      while (*line != '\0' && !isspace(*line)) line++;
	      if (*line == '\0')
		continue;
	    }
	  else if ((link = check_for_link(line, &skip_chars)) != NULL)
	    {
	      http_response_printf(res, "%s", p);
	      http_response_printf(res, "%s", link); 

	      line += skip_chars;
	      p = line;

	      continue;

	    }
	  /* TODO: Below is getting bloated and messy, need rewriting more
	   *       compactly ( and efficently ).
	   */
	  else if (*line == '*')
	    {
	      /* Try and be smart about what gets bolded */
	      if (line_start != line 
		  && !is_wiki_format_char_or_space(*(line-1)) 
		  && !bold_on)
		{ line++; continue; }

	      if ((isspace(*(line+1)) && !bold_on))
		{ line++; continue; }

		/* bold */
		*line = '\0';
		http_response_printf(res, "%s%s\n", p, bold_on ? "</b>" : "<b>");
		bold_on ^= 1; /* reset flag */
		p = line+1;

	    }
	  else if (*line == '_' )
	    {
	      if (line_start != line 
		  && !is_wiki_format_char_or_space(*(line-1)) 
		  && !underline_on)
		{ line++; continue; }

	      if (isspace(*(line+1)) && !underline_on)
		{ line++; continue; }
	      /* underline */
	      *line = '\0';
	      http_response_printf(res, "%s%s\n", p, underline_on ? "</u>" : "<u>"); 
	      underline_on ^= 1; /* reset flag */
	      p = line+1;
	    }
	  else if (*line == '-')
	    {
	      if (line_start != line 
		  && !is_wiki_format_char_or_space(*(line-1)) 
		  && !strikethrough_on)
		{ line++; continue; }

	      if (isspace(*(line+1)) && !strikethrough_on)
		{ line++; continue; }
	       
	      /* strikethrough */
	      *line = '\0';
	      http_response_printf(res, "%s%s\n", p, strikethrough_on ? "</del>" : "<del>"); 
	      strikethrough_on ^= 1; /* reset flag */
	      p = line+1; 
	      

	    }
	  else if (*line == '/' )
	    {
	      if (line_start != line 
		  && !is_wiki_format_char_or_space(*(line-1)) 
		  && !italic_on)
		{ line++; continue; }

	      if (isspace(*(line+1)) && !italic_on)
		{ line++; continue; }

	      /* crude path detection */
	      if (line_start != line && isspace(*(line-1)) && !italic_on)
		{ 
		  char *tmp   = line+1;
		  int slashes = 0;

		  /* Hack to escape out file paths */
		  while (*tmp != '\0' && !isspace(*tmp))
		    { 
		      if (*tmp == '/') slashes++;
		      tmp++;
		    }

		  if (slashes > 1 || (slashes == 1 && *(tmp-1) != '/')) 
		    { line = tmp; continue; }
		}

	      if (*(line+1) == '/')
		line++; 	/* escape out common '//' - eg urls */
	      else
		{
		  /* italic */
		  *line = '\0';
		  http_response_printf(res, "%s%s\n", p, italic_on ? "</i>" : "<i>"); 
		  italic_on ^= 1; /* reset flag */
		  p = line+1; 
		}
	    }
	  else if (*line == '|' && table_on) /* table column */
	    {
	      *line = '\0';
	      http_response_printf(res, "%s", p);
	      http_response_printf(res, "</td><td>\n");
	      p = line+1;
	    }

	  line++;

	} /* next word */

      if (*p != '\0') 			/* accumalated text left over */
	http_response_printf(res, "%s", p);

      /* close any html tags that could be still open */


      if (listtypes[ULIST].depth)
	http_response_printf(res, "</li>");

      if (listtypes[OLIST].depth)
	http_response_printf(res, "</li>");

      if (table_on)
	http_response_printf(res, "</td></tr>\n");

      if (header_level)
	http_response_printf(res, "</h%d>\n", header_level);  
      else
	http_response_printf(res, "\n");


    } /* next line */

  /* clean up anything thats still open */

  if (pre_on)
    http_response_printf(res, "</pre>\n");
  
  /* close any open lists */
  for (i=0; i<listtypes[ULIST].depth; i++)
    http_response_printf(res, "</ul>\n");

  for (i=0; i<listtypes[OLIST].depth; i++)
    http_response_printf(res, "</ol>\n");
  
  /* close any open paras */
  if (open_para)
    http_response_printf(res, "</p>\n");

    /* tables */
    if (table_on)
     http_response_printf(res, "</table>\n");

}

int 
wiki_redirect(HttpResponse *res, char *location)
{
  int   header_len = strlen(location) + 14; 
  char *header = alloca(sizeof(char)*header_len);

  snprintf(header, header_len, "Location: %s\r\n", location);

  http_response_append_header(res, header);
  http_response_printf(res, "<html>\n<p>Redirect to %s</p>\n</html>\n", 
		       location);
  http_response_set_status(res, 302, "Moved Temporarily");
  http_response_send(res);

  exit(0);
}


void
wiki_show_page(HttpResponse *res, char *wikitext, char *page)
{
  char *html_clean_wikitext = NULL;

  http_response_printf_alloc_buffer(res, strlen(wikitext)*2);

  wiki_show_header(res, page, TRUE);

  html_clean_wikitext = util_htmlize(wikitext, strlen(wikitext));

  wiki_print_data_as_html(res, html_clean_wikitext);      

  wiki_show_footer(res);  

  http_response_send(res);

  exit(0);

}

void
wiki_show_edit_page(HttpResponse *res, char *wikitext, char *page)
{
  wiki_show_header(res, page, FALSE);

  if (wikitext == NULL) wikitext = "";
  http_response_printf(res, EDITFORM, page, wikitext);
		       
  wiki_show_footer(res);

  http_response_send(res);
  exit(0);
}

void
wiki_show_create_page(HttpResponse *res)
{
  wiki_show_header(res, "Create New Page", FALSE);
  http_response_printf(res, "%s", CREATEFORM);
  wiki_show_footer(res);

  http_response_send(res);
  exit(0);
}

static int 
changes_compar(const struct dirent **d1, const struct dirent **d2)
{
    struct stat st1, st2;

    stat((*d1)->d_name, &st1);

    stat((*d2)->d_name, &st2);

    if (st1.st_mtime > st2.st_mtime)
      return 1;
    else
      return -1;
}



WikiPageList**
wiki_get_pages(int  *n_pages, char *expr)
{
  WikiPageList  **pages;
  struct dirent **namelist;
  int             n, i = 0;
  struct stat     st;

  n = scandir(".", &namelist, 0, (void *)changes_compar);
  
  pages = malloc(sizeof(WikiPageList*)*n);

  while(n--) 
    {
      if ((namelist[n]->d_name)[0] == '.' 
	  || !strcmp(namelist[n]->d_name, "styles.css"))
	goto cleanup;

      if (expr != NULL) 
	{ 			/* Super Simple Search */
	  char *data = NULL;
	  if ((data = file_read(namelist[n]->d_name)) != NULL)
	    if (strstr(data, expr) == NULL)
	      if (strcmp(namelist[n]->d_name, expr) != 0) 
		goto cleanup; 
	}


      stat(namelist[n]->d_name, &st);

      /* ignore anything but regular readable files */
      if (S_ISREG(st.st_mode) && access(namelist[n]->d_name, R_OK) == 0)
	{
	  pages[i]        = malloc(sizeof(WikiPageList));
	  pages[i]->name  = strdup (namelist[n]->d_name);
	  pages[i]->mtime = st.st_mtime;

	  i++;
	}

    cleanup:
      free(namelist[n]);
    }

  *n_pages = i;

  free(namelist);

  if (i==0) return NULL;

  return pages;
}

void
wiki_show_changes_page(HttpResponse *res)
{
  WikiPageList **pages = NULL;
  int            n_pages, i;

  wiki_show_header(res, "Changes", FALSE);

  pages = wiki_get_pages(&n_pages, NULL);

  for (i=0; i<n_pages; i++)
    {
      struct tm   *pTm;
      char   datebuf[64];

      pTm = localtime(&pages[i]->mtime);
      strftime(datebuf, sizeof(datebuf), "%Y-%m-%d %H:%M", pTm);
      http_response_printf(res, "<a href='%s'>%s</a> %s<br />\n", 
			   pages[i]->name, 
			   pages[i]->name, 
			   datebuf);
    }

  wiki_show_footer(res);
  http_response_send(res);
  exit(0);
}

void
wiki_show_changes_page_rss(HttpResponse *res)
{
  WikiPageList **pages = NULL;
  int            n_pages, i;
  /*char          *html_clean_wikitext = NULL;
    char          *wikitext; */

  pages = wiki_get_pages(&n_pages, NULL);

  http_response_printf(res, "<?xml version=\"1.0\"encoding=\"ISO-8859-1\"?>\n"
                            "<rss version=\"2.0\">\n"
		            "<channel><title>DidiWiki Changes feed</title>\n");

  for (i=0; i<n_pages; i++)
    {
      struct tm   *pTm;
      char         datebuf[64];

      pTm = localtime(&pages[i]->mtime);
      strftime(datebuf, sizeof(datebuf), "%Y-%m-%d %H:%M", pTm);

      http_response_printf(res, 
			   "<item><title>%s</title>"
			   "<link>%s%s</link><description>"
			   "Modified %s\n",
                           pages[i]->name,
			   getenv("DIDIWIKI_URL_PREFIX") ? getenv("DIDIWIKI_URL_PREFIX") : "", 
                           pages[i]->name,
			   datebuf);

      /*
      wikitext = file_read(pages[i]->name);
      http_response_printf_alloc_buffer(res, strlen(wikitext)*2);
      html_clean_wikitext = util_htmlize(wikitext, strlen(wikitext));
      wiki_print_data_as_html(res, html_clean_wikitext);
      */
      http_response_printf(res, "</description></item>\n");
    }

  http_response_printf(res, "</channel>\n</rss>");

  http_response_send(res);
  exit(0);
}


void
wiki_show_search_results_page(HttpResponse *res, char *expr)
{
  WikiPageList **pages = NULL;
  int            n_pages, i;

  if (expr == NULL || strlen(expr) == 0)
    {
      wiki_show_header(res, "Search", FALSE);
      http_response_printf(res, "No Search Terms supplied");
      wiki_show_footer(res);
      http_response_send(res);
      exit(0);
    }

  pages = wiki_get_pages(&n_pages, expr);

  if (pages)
    {
      for (i=0; i<n_pages; i++)
	if (!strcmp(pages[i]->name, expr)) /* redirect on page name match */
	  wiki_redirect(res, pages[i]->name);

      wiki_show_header(res, "Search", FALSE);

      for (i=0; i<n_pages; i++)
	{
	  http_response_printf(res, "<a href='%s'>%s</a><br />\n", 
			       pages[i]->name, 
			       pages[i]->name);
	}
    }
  else 
    {
      wiki_show_header(res, "Search", FALSE);
      http_response_printf(res, "No matches");
    }

  wiki_show_footer(res);
  http_response_send(res);

  exit(0);
}

void 
wiki_show_template(HttpResponse *res, char *template_data)
{
  /* 4 templates - header.html, footer.html, 
                   header-noedit.html, footer-noedit.html

     Vars;

     $title      - page title. 
     $include()  - ?
     $pages 

  */

}

void
wiki_show_header(HttpResponse *res, char *page_title, int want_edit)
{
  http_response_printf(res, 
                       "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
                       "<html xmlns='http://www.w3.org/1999/xhtml'>\n"
		       "<head>\n"
                       "<meta http-equiv='Content-Type' content='text/html; charset=utf-8' />\n" 
		       "<link rel='SHORTCUT ICON' href='favicon.ico' />\n"
		       "<link media='all' href='styles.css' rel='stylesheet' type='text/css' />\n"
		       "<title>%s</title>\n"

		       "</head>\n"
		       "<body>\n", page_title
		       );

  http_response_printf(res, PAGEHEADER, page_title, 
		       (want_edit) ? " ( <a href='?edit' title='Edit this wiki page contents. [alt-j]' accesskey='j'>Edit</a> ) " : "" );     
}

void
wiki_show_footer(HttpResponse *res)
{
  http_response_printf(res, "%s", PAGEFOOTER);

  http_response_printf(res, 
		       "</body>\n"
		       "</html>\n"
		       );
}

void
wiki_handle_rest_call(HttpRequest  *req, 
		      HttpResponse *res,
		      char         *func)
{

  if (func != NULL && *func != '\0')
    {
      if (!strcmp(func, "page/get"))
	{
	  char *page = http_request_param_get(req, "page");

	  if (page == NULL)
	    page = http_request_get_query_string(req);

	  if (page && (access(page, R_OK) == 0)) 
	    {
	      http_response_printf(res, "%s", file_read(page));
	      http_response_send(res);
	      return;
	    }  
	}
      else if (!strcmp(func, "page/set"))
	{
	  char *wikitext = NULL, *page = NULL;
	  if( ( (wikitext = http_request_param_get(req, "text")) != NULL)
	      && ( (page = http_request_param_get(req, "page")) != NULL))
	    {
	      file_write(page, wikitext);	      
	      http_response_printf(res, "success");
	      http_response_send(res);
	      return;
	    }
	}
      else if (!strcmp(func, "page/delete"))
	{
	  char *page = http_request_param_get(req, "page");

	  if (page == NULL)
	    page = http_request_get_query_string(req);

	  if (page && (unlink(page) > 0))
	    {
	      http_response_printf(res, "success");
	      http_response_send(res);
	      return;  
	    }
	}
      else if (!strcmp(func, "page/exists"))
	{
	  char *page = http_request_param_get(req, "page");

	  if (page == NULL)
	    page = http_request_get_query_string(req);

	  if (page && (access(page, R_OK) == 0)) 
	    {
	      http_response_printf(res, "success");
	      http_response_send(res);
	      return;  
	    }
	}
      else if (!strcmp(func, "pages") || !strcmp(func, "search"))
	{
	  WikiPageList **pages = NULL;
	  int            n_pages, i;
	  char          *expr = http_request_param_get(req, "expr");

	  if (expr == NULL)
	    expr = http_request_get_query_string(req);
	  
	  pages = wiki_get_pages(&n_pages, expr);

	  if (pages)
	    {
	      for (i=0; i<n_pages; i++)
		{
		  struct tm   *pTm;
		  char   datebuf[64];
		  
		  pTm = localtime(&pages[i]->mtime);
		  strftime(datebuf, sizeof(datebuf), "%Y-%m-%d %H:%M", pTm);
		  http_response_printf(res, "%s\t%s\n", pages[i]->name, datebuf);
		}

	      http_response_send(res);
	      return;  
	    }
	}
    }

  http_response_set_status(res, 500, "Error");
  http_response_printf(res, "<html><body>Failed</body></html>\n");
  http_response_send(res);

  return;  
}



void
wiki_handle_http_request(HttpRequest *req)
{
  HttpResponse *res      = http_response_new(req);
  char         *page     = http_request_get_path_info(req); 
  char         *command  = http_request_get_query_string(req); 
  char         *wikitext = "";

  util_dehttpize(page); 	/* remove any encoding on the requested
				   page name.                           */

  if (!strcmp(page, "/"))
    {
      if (access("WikiHome", R_OK) != 0)
	wiki_redirect(res, "/WikiHome?create");
      page = "/WikiHome";
    }

  if (!strcmp(page, "/styles.css"))
    {
      /*  Return CSS page */
      http_response_set_content_type(res, "text/css");
      http_response_printf(res, "%s", CssData);
      http_response_send(res);
      exit(0);
    }

  if (!strcmp(page, "/favicon.ico"))
    {
      /*  Return favicon */
      http_response_set_content_type(res, "image/ico");
      http_response_set_data(res, FaviconData, FaviconDataLen);
      http_response_send(res);
      exit(0);
    }


  page = page + 1; 		/* skip slash */

  if (!strncmp(page, "api/", 4))
    {
      char *p;

      page += 4; 
      for (p=page; *p != '\0'; p++)
	if (*p=='?') { *p ='\0'; break; }
      
      wiki_handle_rest_call(req, res, page); 
      exit(0);
    }

  /* A little safety. issue a malformed request for any paths,
   * There shouldn't need to be any..
   */
  if (strchr(page, '/'))
    {
      http_response_set_status(res, 404, "Not Found");
      http_response_printf(res, "<html><body>404 Not Found</body></html>\n");
      http_response_send(res);
      exit(0);
    }

  if (!strcmp(page, "Changes"))
    {
      wiki_show_changes_page(res);
    }
  else if (!strcmp(page, "ChangesRss"))
    {
      wiki_show_changes_page_rss(res);
    }
  else if (!strcmp(page, "Search"))
    {
      wiki_show_search_results_page(res, http_request_param_get(req, "expr"));
    }
  else if (!strcmp(page, "Create"))
    {
      if ( (wikitext = http_request_param_get(req, "title")) != NULL)
	{
	  /* create page and redirect */
	  wiki_redirect(res, http_request_param_get(req, "title"));
	}
      else
	{
	   /* show create page form  */
	  wiki_show_create_page(res);
	}
    }
  else
    {
      /* TODO: dont blindly write wikitext data to disk */
      if ( (wikitext = http_request_param_get(req, "wikitext")) != NULL)
	{
	  file_write(page, wikitext);	      
	}

      if (access(page, R_OK) == 0) 	/* page exists */
	{
	  wikitext = file_read(page);
	  
	  if (!strcmp(command, "edit"))
	    {
	      /* print edit page */
	      wiki_show_edit_page(res, wikitext, page);
	    }
	  else
	    {
	      wiki_show_page(res, wikitext, page);
	    }
	}
      else
	{
	  if (!strcmp(command, "create"))
	    {
	      wiki_show_edit_page(res, NULL, page);
	    }
	  else
	    {
	      char buf[1024];
	      snprintf(buf, 1024, "%s?create", page);
	      wiki_redirect(res, buf);
	    }
	}
    }

}

int
wiki_init(void)
{
  char datadir[512] = { 0 };
  struct stat st;

  if (getenv("DIDIWIKIHOME"))
    {
      snprintf(datadir, 512, getenv("DIDIWIKIHOME"));
    }
  else
    {
      if (getenv("HOME") == NULL)
	{
	  fprintf(stderr, "Unable to get home directory, is HOME set?\n");
	  exit(1);
	}

      snprintf(datadir, 512, "%s/.didiwiki", getenv("HOME"));
    }  
     
  /* Check if ~/.didiwiki exists and create if not */
  if (stat(datadir, &st) != 0 )
    {
      if (mkdir(datadir, 0755) == -1)
	{
	  fprintf(stderr, "Unable to create '%s', giving up.\n", datadir);
	  exit(1);
	}
    }

  chdir(datadir);

  /* Write Default Help + Home page if it doesn't exist */

  if (access("WikiHelp", R_OK) != 0) 
    file_write("WikiHelp", HELPTEXT);

  if (access("WikiHome", R_OK) != 0) 
    file_write("WikiHome", HOMETEXT);

  /* Read in optional CSS data */

  if (access("styles.css", R_OK) == 0) 
    CssData = file_read("styles.css");
  
  return 1;
}

