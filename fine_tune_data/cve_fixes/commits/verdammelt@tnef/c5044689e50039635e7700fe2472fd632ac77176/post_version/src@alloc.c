/*
 * alloc.c -- Useful allocation function/defintions
 *
 * Copyright (C)1999-2006 Mark Simpson <damned@world.std.com>
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

static size_t alloc_limit = 0;

void
set_alloc_limit (size_t size)
{
    alloc_limit = size;
}

size_t
get_alloc_limit()
{
    return alloc_limit;
}

size_t
check_mul_overflow(size_t a, size_t b, size_t* res)
{
    size_t tmp = a * b;
    if (a != 0 && tmp / a != b) return 1;
    *res = tmp;
    return 0;
}

static void
alloc_limit_failure (char *fn_name, size_t size)
{
    fprintf (stderr,
             "%s: Maximum allocation size exceeded "
             "(maxsize = %lu; size = %lu).\n",
             fn_name,
             (unsigned long)alloc_limit,
             (unsigned long)size);
}

void
alloc_limit_assert (char *fn_name, size_t size)
{
    if (alloc_limit && size > alloc_limit)
    {
        alloc_limit_failure (fn_name, size);
        exit (-1);
    }
}

/* attempts to malloc memory, if fails print error and call abort */
void*
xmalloc (size_t num, size_t size)
{
    size_t res;
    if (check_mul_overflow(num, size, &res))
        abort();

    void *ptr = malloc (res);
    if (!ptr
        && (size != 0))         /* some libc don't like size == 0 */
    {
        perror ("xmalloc: Memory allocation failure");
        abort();
    }
    return ptr;
}

/* Allocates memory but only up to a limit */
void*
checked_xmalloc (size_t num, size_t size)
{
    size_t res;
    if (check_mul_overflow(num, size, &res))
        abort();

    alloc_limit_assert ("checked_xmalloc", res);
    return xmalloc (num, size);
}

/* xmallocs memory and clears it out */
void*
xcalloc (size_t num, size_t size)
{
    size_t res;
    if (check_mul_overflow(num, size, &res))
        abort();

    void *ptr;
    ptr = malloc(res);
    if (ptr)
    {
        memset (ptr, '\0', (res));
    }
    return ptr;
}

/* xcallocs memory but only up to a limit */
void*
checked_xcalloc (size_t num, size_t size)
{
    size_t res;
    if (check_mul_overflow(num, size, &res))
        abort();

    alloc_limit_assert ("checked_xcalloc", (res));
    return xcalloc (num, size);
}
