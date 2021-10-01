/* The MIT License (MIT)
 *
 * Copyright (c) 2015 Main Street Softworks, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "m_config.h"

#include <mstdlib/mstdlib.h>
#include "fs/m_fs_int.h"
#include "platform/m_platform.h"

#ifndef _WIN32
#  include <errno.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static char M_fs_path_sep_win = '\\';
static char M_fs_path_sep_unix = '/';

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void M_fs_path_split(const char *path, char **dir, char **name, M_fs_system_t sys_type)
{
	M_list_str_t *parts;
	char         *temp;

	if (dir == NULL && name == NULL)
		return;

	if (dir != NULL)
		*dir = NULL;
	if (name != NULL)
		*name = NULL;

	if (path == NULL || *path == '\0') {
		if (dir != NULL) {
			*dir = M_strdup(".");
		}
		return;
	}

	sys_type = M_fs_path_get_system_type(sys_type);
	parts    = M_fs_path_componentize_path(path, sys_type);
	temp     = M_list_str_take_at(parts, M_list_str_len(parts)-1);

	if (M_list_str_len(parts) == 0 && M_fs_path_isabs(path, sys_type)) {
		M_list_str_insert(parts, temp);
		M_free(temp);
		temp = NULL;
	}

	if (temp != NULL && *temp == '\0') {
		M_free(temp);
		temp = NULL;
	}

	if (name != NULL) {
		*name = temp;
	} else {
		M_free(temp);
	}

	if (dir != NULL) {
		*dir = M_fs_path_join_parts(parts, sys_type);
		if (*dir == NULL) {
			*dir = M_strdup(".");
		}
	}

	M_list_str_destroy(parts);
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* Figure out what system type to use for logic. */
M_fs_system_t M_fs_path_get_system_type(M_fs_system_t sys_type)
{
	if (sys_type == M_FS_SYSTEM_AUTO) {
#ifdef _WIN32
		return M_FS_SYSTEM_WINDOWS;
#else
		return M_FS_SYSTEM_UNIX;
#endif
	}
	return sys_type;
}

/* Get the appropriate separator for the system type. */
char M_fs_path_get_system_sep(M_fs_system_t sys_type)
{
	if (M_fs_path_get_system_type(sys_type) == M_FS_SYSTEM_WINDOWS) {
		return M_fs_path_sep_win;
	}
	return M_fs_path_sep_unix;
}


/* Determine the max path length for the system. */
size_t M_fs_path_get_path_max(M_fs_system_t sys_type)
{
	long path_max = 0;

	/* Set some defaults based on the system type in case the path length isn't
	 * actually defined anywhere. */
	sys_type = M_fs_path_get_system_type(sys_type);

	/* Try to determine the path length */
#ifdef PATH_MAX
	path_max = PATH_MAX;
#elif !defined(_WIN32)
	path_max = pathconf("/", _PC_PATH_MAX);
#endif

	/* Ensure we didn't get an unreasonably long path. */
	if (path_max <= 0 || path_max > 65536) {
		if (sys_type == M_FS_SYSTEM_WINDOWS) {
			path_max = 260;
		} else {
			path_max = 4096;
		}
	}

	return (size_t)path_max;
}

/* Check if a path is an absolute path. A path is absolute if it's unix and
 * starrts with /. Or windows and starts with \\\\ (UNC) or a drive letter
 * followed by :. E.g. X:. */
M_bool M_fs_path_isabs(const char *p, M_fs_system_t sys_type)
{
	size_t len;

	if (p == NULL) {
		return M_FALSE;
	}

	len = M_str_len(p);
	if (len == 0) {
		return M_FALSE;
	}

	sys_type = M_fs_path_get_system_type(sys_type);
	if ((sys_type == M_FS_SYSTEM_WINDOWS && (M_fs_path_isunc(p) ||
			(len >= 2 && (p[1] == ':' || (p[0] == '\\' && p[1] == '\\'))))) ||
		(sys_type == M_FS_SYSTEM_UNIX && *p == '/'))
	{
		return M_TRUE;
	}

	return M_FALSE;
}

M_bool M_fs_path_isunc(const char *p)
{
	if (p == NULL) {
		return M_FALSE;
	}

	if (M_str_len(p) >= 2 && p[0] == '\\' && p[1] == '\\') {
		return M_TRUE;
	}

	return M_FALSE;
}

/* Take a path and split it into components. This will remove empty parts. An
 * absolute path starting with / will have the / replaced with an empty to
 * start the list. An empty at the start of the path list should be treated as
 * an abolute path. */
M_list_str_t *M_fs_path_componentize_path(const char *path, M_fs_system_t sys_type)
{
	M_list_str_t *list1;
	M_list_str_t *list2;
	M_list_str_t *list3;
	const char   *const_temp;
	const char   *const_temp2;
	size_t len;
	size_t len2;
	size_t i;
	size_t j;

	sys_type = M_fs_path_get_system_type(sys_type);

	list1 = M_list_str_split('/', path, M_LIST_STR_NONE, M_FALSE);
	list3 = M_list_str_create(M_LIST_STR_NONE);
	len = M_list_str_len(list1);
	for (i=0; i<len; i++) {
		const_temp = M_list_str_at(list1, i);
		if (const_temp == NULL || *const_temp == '\0') {
			continue;
		}
		list2 = M_list_str_split('\\', const_temp, M_LIST_STR_NONE, M_FALSE);
		len2 = M_list_str_len(list2);
		for (j=0; j<len2; j++) {
			const_temp2 = M_list_str_at(list2, j);
			if (const_temp2 == NULL || *const_temp2 == '\0') {
				continue;
			}
			M_list_str_insert(list3, const_temp2);
		}
		M_list_str_destroy(list2);
	}
	M_list_str_destroy(list1);

	if ((sys_type == M_FS_SYSTEM_UNIX && M_fs_path_isabs(path, sys_type)) ||
		(sys_type == M_FS_SYSTEM_WINDOWS && M_fs_path_isunc(path)))
	{
		M_list_str_insert_at(list3, "", 0);
	}

	return list3;
}

/* Take a list of path components and join them into a string separated by the
 * system path separator. */
char *M_fs_path_join_parts(const M_list_str_t *path, M_fs_system_t sys_type)
{
	M_list_str_t *parts;
	const char   *part;
	char         *out;
	size_t        len;
	size_t        i;
	size_t        count;

	if (path == NULL) {
		return NULL;
	}
	len = M_list_str_len(path);
	if (len == 0) {
		return NULL;
	}

	sys_type = M_fs_path_get_system_type(sys_type);

	/* Remove any empty parts (except for the first part which denotes an abs path on Unix
 	 * or a UNC path on Windows). */
	parts = M_list_str_duplicate(path);
	for (i=len-1; i>0; i--) {
		part = M_list_str_at(parts, i);
		if (part == NULL || *part == '\0') {
			M_list_str_remove_at(parts, i);
		}
	}

	len = M_list_str_len(parts);

	/* Join puts the sep between items. If there are no items then the sep
	 * won't be written. */
	part = M_list_str_at(parts, 0);
	if (len == 1 && (part == NULL || *part == '\0')) {
		M_list_str_destroy(parts);
		if (sys_type == M_FS_SYSTEM_WINDOWS) {
			return M_strdup("\\\\");
		}
		return M_strdup("/");
	}

	/* Handle windows abs path because they need two separators. */
	if (sys_type == M_FS_SYSTEM_WINDOWS && len > 0) {
		part  = M_list_str_at(parts, 0);
		/* If we have 1 item we need to add two empties so we get the second separator. */
		count = (len == 1) ? 2 : 1;
		/* If we're dealing with a unc path add the second sep so we get two separators for the UNC base. */
		if (part != NULL && *part == '\0') {
			for (i=0; i<count; i++) {
				M_list_str_insert_at(parts, "", 0);
			}
		} else if (M_fs_path_isabs(part, sys_type) && len == 1) {
			/* We need to add an empty so we get a separator after the drive. */
			M_list_str_insert_at(parts, "", 1);
		}
	}

	out = M_list_str_join(parts, (unsigned char)M_fs_path_get_system_sep(sys_type));
	M_list_str_destroy(parts);
	return out;
}

char *M_fs_path_join_vparts(M_fs_system_t sys_type, size_t num, ...)
{
	M_list_str_t *parts;
	char         *out;
	va_list       ap;
	size_t        i;

	parts = M_list_str_create(M_LIST_STR_NONE);
	va_start(ap, num);
	for (i=0; i<num; i++) {
		M_list_str_insert(parts, va_arg(ap, const char *));
	}
	va_end(ap);

	out = M_fs_path_join_parts(parts, sys_type);
	M_list_str_destroy(parts);
	return out;
}

/* Combine two parts of a path into one. We don't use the M_fs_path_join_parts function because we are working exclusively
 * with relative paths. We don't want an empty p1 to put a dir sep for example. */
char *M_fs_path_join(const char *p1, const char *p2, M_fs_system_t sys_type)
{
	M_buf_t *buf;
	char     sep;

	sys_type = M_fs_path_get_system_type(sys_type);

	/* If p2 is an absolute path we can't properly join it to another path... */
	if (M_fs_path_isabs(p2, sys_type))
		return M_strdup(p2);

	buf = M_buf_create();
	sep = M_fs_path_get_system_sep(sys_type);

	/* Don't add nothing if we have nothing. */
	if (p1 != NULL && *p1 != '\0')
		M_buf_add_str(buf, p1);
	/* Only put a sep if we have two parts and we really need the sep (p1 doesn't end with a sep). */
	if (p1 != NULL && *p1 != '\0' && p2 != NULL && *p2 != '\0' && p1[M_str_len(p1)-1] != sep)
		M_buf_add_byte(buf, (unsigned char)sep);
	/* Don't add nothing if we have nothing. */
	if (p2 != NULL && *p2 != '\0')
		M_buf_add_str(buf, p2);

	return M_buf_finish_str(buf, NULL);
}

char *M_fs_path_join_resolved(const char *path, const char *part, const char *resolved_name, M_fs_system_t sys_type)
{
	char *full_path;
	char *dir;
	char *rpath;

	if ((path == NULL || *path == '\0') && (part == NULL || *part == '\0') && (resolved_name == NULL || *resolved_name == '\0'))
		return NULL;

	sys_type = M_fs_path_get_system_type(sys_type);

	/* If the resolved path is absolute we don't need to modify it. */
	if (M_fs_path_isabs(resolved_name, sys_type))
		return M_strdup(resolved_name);

	full_path = M_fs_path_join(path, part, sys_type);
	M_fs_path_split(full_path, &dir, NULL, sys_type);
	M_free(full_path);

	rpath = M_fs_path_join(dir, resolved_name, sys_type);
	M_free(dir);

	return rpath;
}

#ifdef _WIN32
M_fs_error_t M_fs_path_readlink_int(char **out, const char *path, M_bool last, M_fs_path_norm_t flags, M_fs_system_t sys_type)
{
	(void)path;
	(void)last;
	(void)flags;
	(void)sys_type;
	*out = NULL;
	return M_FS_ERROR_SUCCESS;
}
#else
#include <string.h>
M_fs_error_t M_fs_path_readlink_int(char **out, const char *path, M_bool last, M_fs_path_norm_t flags, M_fs_system_t sys_type)
{
	char        *temp;
	ssize_t      read_len;
	size_t       path_max;
	int          errsv;
	M_fs_info_t *info;

	if (out == NULL || path == NULL) {
		return M_FS_ERROR_INVALID;
	}
	*out = NULL;

	/* Check if this is actually a symlink */
	if (M_fs_info(&info, path, M_FS_PATH_INFO_FLAGS_BASIC) != M_FS_ERROR_SUCCESS) {
		/* Must not be a real path so it's not a symlink. */
		return M_FS_ERROR_SUCCESS;
	}
	if (M_fs_info_get_type(info) != M_FS_TYPE_SYMLINK) {
		/* Real path but it's not a symlink. */
		M_fs_info_destroy(info);
		return M_FS_ERROR_SUCCESS;
	}
	M_fs_info_destroy(info);

	path_max = M_fs_path_get_path_max(sys_type);
	temp     = M_malloc_zero(path_max);

	/* Try to follow the path as a symlink. */
	read_len = readlink(path, temp, path_max-1);
	if (read_len == -1) {
		errsv = errno;
		M_free(temp);
		/* Not a symlink. */
		if (errsv == EINVAL) {
			return M_FS_ERROR_SUCCESS;
		}
		/* The location pointed to by the link does not exist. */
		if (errsv == ENOENT) {
			if ((flags & M_FS_PATH_NORM_SYMLINKS_FAILDNE && !last) || (flags & M_FS_PATH_NORM_SYMLINKS_FAILDNELAST && last)) {
				return M_FS_ERROR_DNE;
			} else {
				return M_FS_ERROR_SUCCESS;
			}
		}
		return M_fs_error_from_syserr(errsv);
	} else {
		/* Appease coverity even though it is null termed on allocation */
		temp[read_len] = '\0';
	}
	/* this way out isn't massive if the path is small. */
	*out = M_strdup(temp);
	M_free(temp);
	return M_FS_ERROR_SUCCESS;
}
#endif

M_fs_error_t M_fs_path_readlink(char **out, const char *path)
{
	return M_fs_path_readlink_int(out, path, M_TRUE, M_FS_PATH_NORM_SYMLINKS_FAILDNELAST, M_FS_SYSTEM_AUTO);
}


M_fs_error_t M_fs_path_get_cwd(char **cwd)
{
	char   *temp;
	size_t  path_max;
#ifdef _WIN32
	DWORD   dpath_max;
#endif

	if (cwd == NULL)
		return M_FS_ERROR_INVALID;
	*cwd = NULL;

	path_max = M_fs_path_get_path_max(M_FS_SYSTEM_AUTO)+1;
	temp     = M_malloc(path_max);

#ifdef _WIN32
	if (!M_win32_size_t_to_dword(path_max, &dpath_max))
		return M_FS_ERROR_INVALID;
	if (GetCurrentDirectory(dpath_max, temp) == 0) {
		M_free(temp);
		return M_fs_error_from_syserr(GetLastError());
	}
#else
	if (getcwd(temp, path_max) == NULL) {
		M_free(temp);
		return M_fs_error_from_syserr(errno);
	}
#endif

	*cwd = M_strdup(temp);
	M_free(temp);
	return M_FS_ERROR_SUCCESS;
}

M_fs_error_t M_fs_path_set_cwd(const char *path)
{
	if (path == NULL || *path == '\0')
		return M_FS_ERROR_INVALID;

#ifdef _WIN32
	if (SetCurrentDirectory(path) == 0) {
		return M_fs_error_from_syserr(GetLastError());
	}
#else
	if (chdir(path) != 0) {
		return M_fs_error_from_syserr(errno);
	}
#endif

	return M_FS_ERROR_SUCCESS;
}

#ifdef _WIN32
M_bool M_fs_path_ishidden(const char *path, M_fs_info_t *info)
{
	M_bool have_info;
	M_bool ret;

	if ((path == NULL || *path == '\0') && info == NULL) {
		return M_FALSE;
	}

	have_info = (info == NULL) ? M_FALSE : M_TRUE;
	if (!have_info) {
		if (M_fs_info(&info, path, M_FS_PATH_INFO_FLAGS_BASIC) != M_FS_ERROR_SUCCESS) {
			return M_FALSE;
		}
	}

	ret = M_fs_info_get_ishidden(info);

	if (!have_info) {
		M_fs_info_destroy(info);
	}

	return ret;
}
#else
M_bool M_fs_path_ishidden(const char *path, M_fs_info_t *info)
{
	M_list_str_t *path_parts;
	size_t        len;
	M_bool        ret        = M_FALSE;

	(void)info;

	if (path == NULL || *path == '\0') {
		return M_FALSE;
	}

	/* Hidden. Check if the first character of the last part of the path. Either the file or directory name itself
 	 * starts with a '.'. */
	path_parts = M_fs_path_componentize_path(path, M_FS_SYSTEM_UNIX);
	len = M_list_str_len(path_parts);
	if (len > 0) {
		if (*M_list_str_at(path_parts, len-1) == '.') {
			ret = M_TRUE;
		}
	}
	M_list_str_destroy(path_parts);

	return ret;
}
#endif

char *M_fs_path_dirname(const char *path, M_fs_system_t sys_type)
{
	char *out;

	M_fs_path_split(path, &out, NULL, sys_type);
	return out;
}

char *M_fs_path_basename(const char *path, M_fs_system_t sys_type)
{
	char *out;

	M_fs_path_split(path, NULL, &out, sys_type);
	return out;
}

char *M_fs_path_user_confdir(M_fs_system_t sys_type)
{
	char         *out;
	M_fs_error_t  res;

#ifdef _WIN32
	res = M_fs_path_norm(&out, "%APPDATA%", M_FS_PATH_NORM_NONE, sys_type);
#elif defined(__APPLE__)
	res = M_fs_path_norm(&out, "~/Library/Application Support/", M_FS_PATH_NORM_HOME, sys_type);
#else
	res = M_fs_path_norm(&out, "~/.config", M_FS_PATH_NORM_HOME, sys_type);
#endif
	if (res != M_FS_ERROR_SUCCESS)
		return NULL;
	return out;
}

char *M_fs_path_tmpdir(M_fs_system_t sys_type)
{
	char         *d   = NULL;
	char         *out = NULL;
	M_fs_error_t  res;

#ifdef _WIN32
	size_t len = M_fs_path_get_path_max(M_FS_SYSTEM_WINDOWS)+1;
	d = M_malloc_zero(len);
	/* Return is length without NULL. */
	if (GetTempPath((DWORD)len, d) >= len) {
		M_free(d);
		d = NULL;
	}
#elif defined(__APPLE__)
	d = M_fs_path_mac_tmpdir();
#else
	const char *const_temp;
	/* Try Unix env var. */
#  ifdef HAVE_SECURE_GETENV
	const_temp = secure_getenv("TMPDIR");
#  else
	const_temp = getenv("TMPDIR");
#  endif
	if (!M_str_isempty(const_temp) && M_fs_perms_can_access(const_temp, M_FS_FILE_MODE_READ|M_FS_FILE_MODE_WRITE) == M_FS_ERROR_SUCCESS) {
		d = M_strdup(const_temp);
	}
	/* Fallback to some "standard" system paths. */
	if (d == NULL) {
		const_temp = "/tmp";
		if (!M_str_isempty(const_temp) && M_fs_perms_can_access(const_temp, M_FS_FILE_MODE_READ|M_FS_FILE_MODE_WRITE) == M_FS_ERROR_SUCCESS) {
			d = M_strdup(const_temp);
		}
	}
	if (d == NULL) {
		const_temp = "/var/tmp";
		if (!M_str_isempty(const_temp) && M_fs_perms_can_access(const_temp, M_FS_FILE_MODE_READ|M_FS_FILE_MODE_WRITE) == M_FS_ERROR_SUCCESS) {
			d = M_strdup(const_temp);
		}
	}
#endif

	if (d != NULL) {
		res = M_fs_path_norm(&out, d, M_FS_PATH_NORM_ABSOLUTE, sys_type);
		if (res != M_FS_ERROR_SUCCESS) {
			out = NULL;
		}
	}
	M_free(d);

	return out;
}
