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
#  include <unistd.h>
#endif

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* Platform specific delete functions. */
#ifdef _WIN32
static M_fs_error_t M_fs_delete_file(const char *path)
{
	if (!DeleteFile(path)) {
		return M_fs_error_from_syserr(GetLastError());
	}
	return M_FS_ERROR_SUCCESS;
}

/* Requires the dir to be empty. Will fail if empty. */
static M_fs_error_t M_fs_delete_dir(const char *path)
{
	if (!RemoveDirectory(path)) {
		return M_fs_error_from_syserr(GetLastError());
	}
	return M_FS_ERROR_SUCCESS;
}
#else
static M_fs_error_t M_fs_delete_file(const char *path)
{
	if (unlink(path) != 0) {
		return M_fs_error_from_syserr(errno);
	}
	return M_FS_ERROR_SUCCESS;
}

/* Requires the dir to be empty. Will fail if empty. */
static M_fs_error_t M_fs_delete_dir(const char *path)
{
	if (rmdir(path) != 0) {
		return M_fs_error_from_syserr(errno);
	}
	return M_FS_ERROR_SUCCESS;
}
#endif

static M_bool M_fs_isfileintodir(const char *p1, const char *p2, char **new_p2)
{
	M_fs_info_t *info1         = NULL;
	M_fs_info_t *info2         = NULL;
	char        *bname;
	M_bool       file_info_dir = M_FALSE;

	if (M_str_isempty(p1) || M_str_isempty(p2) || new_p2 == NULL)
		return M_FALSE;

	if (M_fs_info(&info1, p1, M_FS_PATH_INFO_FLAGS_BASIC) == M_FS_ERROR_SUCCESS     &&
			M_fs_info(&info2, p2, M_FS_PATH_INFO_FLAGS_BASIC) == M_FS_ERROR_SUCCESS &&
			M_fs_info_get_type(info1) != M_FS_TYPE_DIR                              &&
			M_fs_info_get_type(info2) == M_FS_TYPE_DIR)
	{
		file_info_dir = M_TRUE;
	}
	M_fs_info_destroy(info1);
	M_fs_info_destroy(info2);

	if (!file_info_dir)
		return M_FALSE;

	bname   = M_fs_path_basename(p1, M_FS_SYSTEM_AUTO);
	*new_p2 = M_fs_path_join(p2, bname, M_FS_SYSTEM_AUTO);
	M_free(bname);

	return M_TRUE;
}

static M_bool M_fs_check_overwrite_allowed(const char *p1, const char *p2, M_uint32 mode)
{
	M_fs_info_t  *info = NULL;
	char         *pold = NULL;
	char         *pnew = NULL;
	M_fs_type_t   type;
	M_bool        ret  = M_TRUE;

	if (mode & M_FS_FILE_MODE_OVERWRITE)
		return M_TRUE;

	/* If we're not overwriting we need to verify existance.
 	 *
 	 * For files we need to check if the file name exists in the
	 * directory it's being copied to.
	 *
	 * For directories we need to check if the directory name
	 * exists in the directory it's being copied to.
	 */

	if (M_fs_info(&info, p1, M_FS_PATH_INFO_FLAGS_BASIC) != M_FS_ERROR_SUCCESS)
		return M_FALSE;

	type = M_fs_info_get_type(info);
	M_fs_info_destroy(info);

	if (type != M_FS_TYPE_DIR) {
		/* File exists at path. */
		if (M_fs_perms_can_access(p2, M_FS_PERMS_MODE_NONE) == M_FS_ERROR_SUCCESS)
		{
			ret = M_FALSE;
			goto done;
		}
	}

	/* Is dir */
	pold = M_fs_path_basename(p1, M_FS_SYSTEM_AUTO);
	pnew = M_fs_path_join(p2, pnew, M_FS_SYSTEM_AUTO);
	if (M_fs_perms_can_access(pnew, M_FS_PERMS_MODE_NONE) == M_FS_ERROR_SUCCESS) {
		ret = M_FALSE;
		goto done;
	}

done:
	M_free(pnew);
	M_free(pold);
	return ret;
}

/* Moves files and dirs.
 *
 * This will overwrite dest if it exists.
 *
 * The file and dir must be on the same volume for this to succeed. Unfortunately, 
 * there isn't a good/easy way to know if the src and dest are on different volumes. The best solution
 * is to run this and check if the output fails with M_FS_ERROR_NOT_SAMEDEV and run a copy followed by
 * a delete if that is the case. */
static M_fs_error_t M_fs_move_file(const char *path_old, const char *path_new)
{
	M_fs_error_t  res;

	/* Try to move the file. This will (should) fail if the file is cross volume. */
#ifdef _WIN32
	if (MoveFileEx(path_old, path_new, MOVEFILE_REPLACE_EXISTING))
#else
	if (rename(path_old, path_new) == 0)
#endif
	{
		res = M_FS_ERROR_SUCCESS;
	} else {
#ifdef _WIN32
		res = M_fs_error_from_syserr(GetLastError());
#else
		res = M_fs_error_from_syserr(errno);
#endif
	}

	return res;
}

/* Only copies files.
 *
 * This will overwrite dest if it exists.
 *
 * Uses the following process for a copy:
 *   - Open
 *   - Loop (while we hasn't read the entire file)
 *     - Read
 *     - Write
 *   - Close
 *
 * Note:
 * Unix does not have a copy equivalent of rename so we have to use this read/write approach. Windows
 * does have a copy function but we need progress reporting. Windows does have a progress reporting
 * callback but it uses a different prototype and doesn't report all the info we want so we're not
 * going to use it. */
static M_fs_error_t M_fs_copy_file(const char *path_old, const char *path_new, M_fs_file_mode_t mode, M_fs_progress_cb_t cb, M_fs_progress_flags_t progress_flags, M_fs_progress_t *progress, const M_fs_perms_t *perms)
{
	M_fs_file_t   *fd_old;
	M_fs_file_t   *fd_new;
	M_fs_info_t   *info         = NULL;
	unsigned char  temp[M_FS_BUF_SIZE];
	size_t         read_len;
	size_t         wrote_len;
	size_t         wrote_total  = 0;
	size_t         offset;
	M_fs_error_t   res;

	/* We're going to create/open/truncate the new file, then as we read the contents from the old file we'll write it
 	 * to new file. */
	if (M_fs_perms_can_access(path_new, M_FS_PERMS_MODE_NONE) == M_FS_ERROR_SUCCESS) {
		/* Try to delete the file since we'll be overwrite it. This is so when we create the file we create it without
 		 * any permissions and to ensure that anything that has the file already open won't be able to read the new
		 * contents we're writing to the file or be able to change the perms. There is an unavoidable race condition
		 * between deleting and creating the file where someone could create the file and have access. However,
		 * depending on the OS they may have access even if the file is created with no perms... */
		res = M_fs_delete(path_new, M_FALSE, NULL, M_FS_PROGRESS_NOEXTRA);
		if (res != M_FS_ERROR_SUCCESS) {
			return res;
		}
	}
	/* Open the old file */
	res = M_fs_file_open(&fd_old, path_old, M_FS_BUF_SIZE, M_FS_FILE_MODE_READ|M_FS_FILE_MODE_NOCREATE, NULL);
	if (res != M_FS_ERROR_SUCCESS) {
		return res;
	}

	if (perms == NULL && mode & M_FS_FILE_MODE_PRESERVE_PERMS) {
		res = M_fs_info_file(&info, fd_old, M_FS_PATH_INFO_FLAGS_NONE);
		if (res != M_FS_ERROR_SUCCESS) {
			M_fs_file_close(fd_old);
			return res;
		}
		perms = M_fs_info_get_perms(info);
	}
	res = M_fs_file_open(&fd_new, path_new, M_FS_BUF_SIZE, M_FS_FILE_MODE_WRITE|M_FS_FILE_MODE_OVERWRITE, perms);
	M_fs_info_destroy(info);
	if (res != M_FS_ERROR_SUCCESS) {
		M_fs_file_close(fd_old);
		return res;
	}

	/* Copy the contents of old into new. */
	while ((res = M_fs_file_read(fd_old, temp, sizeof(temp), &read_len, M_FS_FILE_RW_NORMAL)) == M_FS_ERROR_SUCCESS && read_len != 0) {
		offset = 0;
		while (offset < read_len) {
			res          = M_fs_file_write(fd_new, temp+offset, read_len-offset, &wrote_len, M_FS_FILE_RW_NORMAL);
			offset      += wrote_len;
			wrote_total += wrote_len;

			if (cb) {
				M_fs_progress_set_result(progress, res);
				if (progress_flags & M_FS_PROGRESS_SIZE_TOTAL) {
					M_fs_progress_set_size_total_progess(progress, M_fs_progress_get_size_total_progess(progress)+wrote_len);
				}
				if (progress_flags & M_FS_PROGRESS_SIZE_CUR) {
					M_fs_progress_set_size_current_progress(progress, wrote_total);
				}
				if (progress_flags & M_FS_PROGRESS_COUNT) {
					M_fs_progress_set_count(progress, M_fs_progress_get_count(progress)+1);
				}
				if (!cb(progress)) {
					res = M_FS_ERROR_CANCELED;
				}
			}

			if (res != M_FS_ERROR_SUCCESS) {
				break;
			}
		}
		if (res != M_FS_ERROR_SUCCESS) {
			break;
		}
	}
	M_fs_file_close(fd_old);
	M_fs_file_close(fd_new);
	if (res != M_FS_ERROR_SUCCESS) {
		return res;
	}

	return M_FS_ERROR_SUCCESS;
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

M_fs_error_t M_fs_symlink(const char *target, const char *link_name)
{
	if (target == NULL || *target == '\0' || link_name == NULL || *link_name == '\0')
		return M_FS_ERROR_INVALID;
	
#ifdef _WIN32
	return M_FS_ERROR_GENERIC;
#else
	if (symlink(link_name, target) == -1) {
		return M_fs_error_from_syserr(errno);
	}
#endif

	return M_FS_ERROR_SUCCESS;
}

M_fs_error_t M_fs_move(const char *path_old, const char *path_new, M_uint32 mode, M_fs_progress_cb_t cb, M_uint32 progress_flags)
{
	char            *norm_path_old;
	char            *norm_path_new;
	char            *resolve_path;
	M_fs_info_t     *info;
	M_fs_progress_t *progress      = NULL;
	M_uint64         entry_size;
	M_fs_error_t     res;

	if (path_old == NULL || *path_old == '\0' || path_new == NULL || *path_new == '\0') {
		return M_FS_ERROR_INVALID;
	}

	/* It's okay if new path doesn't exist. */
	res = M_fs_path_norm(&norm_path_new, path_new, M_FS_PATH_NORM_RESDIR, M_FS_SYSTEM_AUTO);
	if (res != M_FS_ERROR_SUCCESS) {
		M_free(norm_path_new);
		return res;
	}

	/* If a path is a file and the destination is a directory the file should be moved 
	 * into the directory. E.g. /file.txt -> /dir = /dir/file.txt */
	if (M_fs_isfileintodir(path_old, path_new, &norm_path_old)) {
		M_free(norm_path_new);
		res = M_fs_move(path_old, norm_path_old, mode, cb, progress_flags);
		M_free(norm_path_old);
		return res;
	}

	/* Normalize the old path and do basic checks that it exists. We'll leave really checking that the old path
 	 * existing to rename because any check we perform may not be true when rename is called. */
	res = M_fs_path_norm(&norm_path_old, path_old, M_FS_PATH_NORM_RESALL, M_FS_SYSTEM_AUTO);
	if (res != M_FS_ERROR_SUCCESS) {
		M_free(norm_path_new);
		M_free(norm_path_old);
		return res;
	}

	progress = M_fs_progress_create();

	res = M_fs_info(&info, path_old, (mode & M_FS_FILE_MODE_PRESERVE_PERMS)?M_FS_PATH_INFO_FLAGS_NONE:M_FS_PATH_INFO_FLAGS_BASIC);
	if (res != M_FS_ERROR_SUCCESS) {
		M_fs_progress_destroy(progress);
		M_free(norm_path_new);
		M_free(norm_path_old);
		return res;
	}

 	/* There is a race condition where the path could not exist but be created between the exists check and calling
	 * rename to move the file but there isn't much we can do in this case. copy will delete and the file so this
	 * situation won't cause an error. */
	if (!M_fs_check_overwrite_allowed(norm_path_old, norm_path_new, mode)) {
		M_fs_progress_destroy(progress);
		M_free(norm_path_new);
		M_free(norm_path_old);
		return M_FS_ERROR_FILE_EXISTS;
	}

	if (cb) {
		entry_size = M_fs_info_get_size(info);

		M_fs_progress_set_path(progress, norm_path_new);
		M_fs_progress_set_type(progress, M_fs_info_get_type(info));
		if (progress_flags & M_FS_PROGRESS_SIZE_TOTAL) {
			M_fs_progress_set_size_total(progress, entry_size);
			M_fs_progress_set_size_total_progess(progress, entry_size);
		}
		if (progress_flags & M_FS_PROGRESS_SIZE_CUR) {
			M_fs_progress_set_size_current(progress, entry_size);
			M_fs_progress_set_size_current_progress(progress, entry_size);
		}
		/* Change the progress count to reflect the count. */
		if (progress_flags & M_FS_PROGRESS_COUNT) {
			M_fs_progress_set_count_total(progress, 1);
			M_fs_progress_set_count(progress, 1);
		}
	}

	/* Move the file. */
	if (M_fs_info_get_type(info) == M_FS_TYPE_SYMLINK) {
		res = M_fs_path_readlink(&resolve_path, norm_path_old);
		if (res == M_FS_ERROR_SUCCESS) {
			res = M_fs_symlink(norm_path_new, resolve_path);
		}
		M_free(resolve_path);
	} else {
		res = M_fs_move_file(norm_path_old, norm_path_new);
	}
	/* Failure was because we're crossing mount points. */
	if (res == M_FS_ERROR_NOT_SAMEDEV) {
		/* Can't rename so copy and delete. */
		if (M_fs_copy(norm_path_old, norm_path_new, mode, cb, progress_flags) == M_FS_ERROR_SUCCESS) {
			/* Success - Delete the original files since this is a move. */
			res = M_fs_delete(norm_path_old, M_TRUE, NULL, M_FS_PROGRESS_NOEXTRA);
		} else {
			/* Failure - Delete the new files that were copied but only if we are not overwriting. We don't
 			 * want to remove any existing files (especially if the dest is a dir). */
			if (!(mode & M_FS_FILE_MODE_OVERWRITE)) {
				M_fs_delete(norm_path_new, M_TRUE, NULL, M_FS_PROGRESS_NOEXTRA);
			}
			res = M_FS_ERROR_GENERIC;
		}
	} else {
		/* Call the cb with the result of the move whether it was a success for fail. We call the cb only if the
 		 * result of the move is not M_FS_ERROR_NOT_SAMEDEV because the copy operation will call the cb for us. */
		if (cb) {
			M_fs_progress_set_result(progress, res);
			if (!cb(progress)) {
				res = M_FS_ERROR_CANCELED;
			}
		}
	}

	M_fs_info_destroy(info);
	M_fs_progress_destroy(progress);
	M_free(norm_path_new);
	M_free(norm_path_old);

	return res;
}

M_fs_error_t M_fs_copy(const char *path_old, const char *path_new, M_uint32 mode, M_fs_progress_cb_t cb, M_uint32 progress_flags)
{
	char                   *norm_path_old;
	char                   *norm_path_new;
	char                   *join_path_old;
	char                   *join_path_new;
	M_fs_dir_entries_t     *entries;
	const M_fs_dir_entry_t *entry;
	M_fs_info_t            *info;
	M_fs_progress_t        *progress            = NULL;
	M_fs_dir_walk_filter_t  filter              = M_FS_DIR_WALK_FILTER_ALL|M_FS_DIR_WALK_FILTER_RECURSE;
	M_fs_type_t             type;
	size_t                  len;
	size_t                  i;
	M_uint64                total_count         = 0;
	M_uint64                total_size          = 0;
	M_uint64                total_size_progress = 0;
	M_uint64                entry_size;
	M_fs_error_t            res;

	if (path_old == NULL || *path_old == '\0' || path_new == NULL || *path_new == '\0') {
		return M_FS_ERROR_INVALID;
	}

	/* It's okay if new path doesn't exist. */
	res = M_fs_path_norm(&norm_path_new, path_new, M_FS_PATH_NORM_RESDIR, M_FS_SYSTEM_AUTO);
	if (res != M_FS_ERROR_SUCCESS) {
		M_free(norm_path_new);
		return res;
	}

	/* If a path is a file and the destination is a directory the file should be copied
	 * into the directory. E.g. /file.txt -> /dir = /dir/file.txt */
	if (M_fs_isfileintodir(path_old, path_new, &norm_path_old)) {
		M_free(norm_path_new);
		res = M_fs_copy(path_old, norm_path_old, mode, cb, progress_flags);
		M_free(norm_path_old);
		return res;
	}

	/* Normalize the old path and do basic checks that it exists. We'll leave really checking that the old path
 	 * existing to rename because any check we perform may not be true when rename is called. */
	res = M_fs_path_norm(&norm_path_old, path_old, M_FS_PATH_NORM_RESALL, M_FS_SYSTEM_AUTO);
	if (res != M_FS_ERROR_SUCCESS) {
		M_free(norm_path_new);
		M_free(norm_path_old);
		return res;
	}

	progress = M_fs_progress_create();

	res = M_fs_info(&info, path_old, (mode & M_FS_FILE_MODE_PRESERVE_PERMS)?M_FS_PATH_INFO_FLAGS_NONE:M_FS_PATH_INFO_FLAGS_BASIC);
	if (res != M_FS_ERROR_SUCCESS) {
		M_fs_progress_destroy(progress);
		M_free(norm_path_new);
		M_free(norm_path_old);
		return res;
	}

	type = M_fs_info_get_type(info);

 	/* There is a race condition where the path could not exist but be created between the exists check and calling
	 * rename to move the file but there isn't much we can do in this case. copy will delete and the file so this
	 * situation won't cause an error. */
	if (!M_fs_check_overwrite_allowed(norm_path_old, norm_path_new, mode)) {
		M_fs_progress_destroy(progress);
		M_free(norm_path_new);
		M_free(norm_path_old);
		return M_FS_ERROR_FILE_EXISTS;
	}

	entries = M_fs_dir_entries_create();
	/* No need to destroy info  because it's now owned by entries and will be destroyed when entries is destroyed.
 	 * M_FS_DIR_WALK_FILTER_READ_INFO_BASIC doesn't actually get the perms it's just there to ensure the info is
	 * stored in the entry. */
	M_fs_dir_entries_insert(entries, M_fs_dir_walk_fill_entry(norm_path_new, NULL, type, info, M_FS_DIR_WALK_FILTER_READ_INFO_BASIC));
	if (type == M_FS_TYPE_DIR) {
		if (mode & M_FS_FILE_MODE_PRESERVE_PERMS) {
			filter |= M_FS_DIR_WALK_FILTER_READ_INFO_FULL;
		} else if (cb && progress_flags & (M_FS_PROGRESS_SIZE_TOTAL|M_FS_PROGRESS_SIZE_CUR)) {
			filter |= M_FS_DIR_WALK_FILTER_READ_INFO_BASIC;
		}
		/* Get all the files under the dir. */
		M_fs_dir_entries_merge(&entries, M_fs_dir_walk_entries(norm_path_old, NULL, filter));
	}

	/* Put all dirs first. We need to ensure the dir(s) exist before we can copy files. */
	M_fs_dir_entries_sort(entries, M_FS_DIR_SORT_ISDIR, M_TRUE, M_FS_DIR_SORT_NAME_CASECMP, M_TRUE);

	len = M_fs_dir_entries_len(entries);
	if (cb) {
		total_size = 0;
		for (i=0; i<len; i++) {
			entry       = M_fs_dir_entries_at(entries, i);
			entry_size  = M_fs_info_get_size(M_fs_dir_entry_get_info(entry));
			total_size += entry_size;

			type = M_fs_dir_entry_get_type(entry);
			/* The total isn't the total number of files but the total number of operations. 
 			 * Making dirs and symlinks is one operation and copying a file will be split into
			 * multiple operations. Copying uses the M_FS_BUF_SIZE to read and write in
			 * chunks. We determine how many chunks will be needed to read the entire file and
			 * use that for the number of operations for the file. */
			if (type == M_FS_TYPE_DIR || type == M_FS_TYPE_SYMLINK) {
				total_count++;
			} else {
				total_count += (entry_size + M_FS_BUF_SIZE - 1) / M_FS_BUF_SIZE;
			}
		}
		/* Change the progress total size to reflect all entries. */
		if (progress_flags & M_FS_PROGRESS_SIZE_TOTAL) {
			M_fs_progress_set_size_total(progress, total_size);
		}
		/* Change the progress count to reflect the count. */
		if (progress_flags & M_FS_PROGRESS_COUNT) {
			M_fs_progress_set_count_total(progress, total_count);
		}
	}
	for (i=0; i<len; i++) {
		entry         = M_fs_dir_entries_at(entries, i);
		type          = M_fs_dir_entry_get_type(entry);
		join_path_old = M_fs_path_join(norm_path_old, M_fs_dir_entry_get_name(entry), M_FS_SYSTEM_AUTO);
		join_path_new = M_fs_path_join(norm_path_new, M_fs_dir_entry_get_name(entry), M_FS_SYSTEM_AUTO);

		entry_size           = M_fs_info_get_size(M_fs_dir_entry_get_info(entry));
		total_size_progress += entry_size;

		if (cb) {
			M_fs_progress_set_path(progress, join_path_new);
			if (progress_flags & M_FS_PROGRESS_SIZE_CUR) {
				M_fs_progress_set_size_current(progress, entry_size);
			}
		}

		/* op */
		if (type == M_FS_TYPE_DIR || type == M_FS_TYPE_SYMLINK) {
			if (type == M_FS_TYPE_DIR) {
				res = M_fs_dir_mkdir(join_path_new, M_FALSE, NULL);
			} else if (type == M_FS_TYPE_SYMLINK) {
				res = M_fs_symlink(join_path_new, M_fs_dir_entry_get_resolved_name(entry));
			} 
			if (res == M_FS_ERROR_SUCCESS && (mode & M_FS_FILE_MODE_PRESERVE_PERMS)) {
				res = M_fs_perms_set_perms(M_fs_info_get_perms(M_fs_dir_entry_get_info(entry)), join_path_new);
			}
		} else {
			res = M_fs_copy_file(join_path_old, join_path_new, mode, cb, progress_flags, progress, M_fs_info_get_perms(M_fs_dir_entry_get_info(entry)));
		}

		M_free(join_path_old);
		M_free(join_path_new);

		/* Call the callback and stop processing if requested. */
		if ((type == M_FS_TYPE_DIR || type == M_FS_TYPE_SYMLINK) && cb) {
			M_fs_progress_set_type(progress, M_fs_dir_entry_get_type(entry));
			M_fs_progress_set_result(progress, res);

			if (progress_flags & M_FS_PROGRESS_SIZE_TOTAL) {
				M_fs_progress_set_size_total_progess(progress, total_size_progress);
			}
			if (progress_flags & M_FS_PROGRESS_SIZE_CUR) {
				M_fs_progress_set_size_current_progress(progress, entry_size);
			}
			if (progress_flags & M_FS_PROGRESS_COUNT) {
				M_fs_progress_set_count(progress, M_fs_progress_get_count(progress)+1);
			}

			if (!cb(progress)) {
				res = M_FS_ERROR_CANCELED;
			}
		}
		if (res != M_FS_ERROR_SUCCESS) {
			break;
		}
	}

	/* Delete the file(s) if it could not be copied properly, but only if we are not overwriting.
 	 * If we're overwriting then there could be other files in that location (especially if it's a dir). */
	if (res != M_FS_ERROR_SUCCESS && !(mode & M_FS_FILE_MODE_OVERWRITE)) {
		M_fs_delete(path_new, M_TRUE, NULL, M_FS_PROGRESS_NOEXTRA);
	}

	M_fs_dir_entries_destroy(entries);
	M_fs_progress_destroy(progress);
	M_free(norm_path_new);
	M_free(norm_path_old);

	return res;
}

M_fs_error_t M_fs_delete(const char *path, M_bool remove_children, M_fs_progress_cb_t cb, M_uint32 progress_flags)
{
	char                   *norm_path;
	char                   *join_path;
	M_fs_dir_entries_t     *entries;
	const M_fs_dir_entry_t *entry;
	M_fs_info_t            *info;
	M_fs_progress_t        *progress            = NULL;
	M_fs_dir_walk_filter_t  filter              = M_FS_DIR_WALK_FILTER_ALL|M_FS_DIR_WALK_FILTER_RECURSE;
	M_fs_type_t             type;
	/* The result that will be returned by this function. */
	M_fs_error_t            res;
	/* The result of the delete itself. */
	M_fs_error_t            res2;
	size_t                  len;
	size_t                  i;
	M_uint64                total_size          = 0;
	M_uint64                total_size_progress = 0;
	M_uint64                entry_size;

	/* Normalize the path we are going to delete so we have a valid path to pass around. */
	res = M_fs_path_norm(&norm_path, path, M_FS_PATH_NORM_HOME, M_FS_SYSTEM_AUTO);
	if (res != M_FS_ERROR_SUCCESS) {
		M_free(norm_path);
		return res;
	}

	/* We need the info to determine if the path is valid and because we need the type. */
	res = M_fs_info(&info, norm_path, M_FS_PATH_INFO_FLAGS_BASIC);
	if (res != M_FS_ERROR_SUCCESS) {
		M_free(norm_path);
		return res;
	}

	/* We must know the type because there are different functions for deleting a file and deleting a directory. */
	type = M_fs_info_get_type(info);
	if (type == M_FS_TYPE_UNKNOWN) {
		M_fs_info_destroy(info);
		M_free(norm_path);
		return M_FS_ERROR_GENERIC;
	}

	/* Create a list of entries to store all the places we need to delete. */
	entries = M_fs_dir_entries_create();

	/* Recursive directory deletion isn't intuitive. We have to generate a list of files and delete the list.
 	 * We cannot delete as walk because not all file systems support that operation. The walk; delete; behavior
	 * is undefined in Posix and HFS is known to skip files if the directory contents is modifies as the
	 * directory is being walked. */
	if (type == M_FS_TYPE_DIR && remove_children) {
		/* We need to read the basic info if the we need to report the size totals to the cb. */
		if (cb && progress_flags & (M_FS_PROGRESS_SIZE_TOTAL|M_FS_PROGRESS_SIZE_CUR)) {
			filter |= M_FS_DIR_WALK_FILTER_READ_INFO_BASIC;
		}
		M_fs_dir_entries_merge(&entries, M_fs_dir_walk_entries(norm_path, NULL, filter));
	}

	/* Add the original path to the list of entries. This may be the only entry in the list. We need to add
 	 * it after a potential walk because we can't delete a directory that isn't empty.
	 * Note: 
	 *   - The info will be owned by the entry and destroyed when it is destroyed. 
	 *   - The basic info param doesn't get the info in this case. it's set so the info is stored in the entry. */
	M_fs_dir_entries_insert(entries, M_fs_dir_walk_fill_entry(norm_path, NULL, type, info, M_FS_DIR_WALK_FILTER_READ_INFO_BASIC));

	len = M_fs_dir_entries_len(entries);
	if (cb) {
		/* Create the progress. The same progress will be used for the entire operation. It will be updated with
 		 * new info as necessary. */
		progress = M_fs_progress_create();

		/* Get the total size of all files to be deleted if using the progress cb and size totals is set. */
		if (progress_flags & M_FS_PROGRESS_SIZE_TOTAL) {
			for (i=0; i<len; i++) {
				entry       = M_fs_dir_entries_at(entries, i);
				entry_size  = M_fs_info_get_size(M_fs_dir_entry_get_info(entry));
				total_size += entry_size;
			}
			/* Change the progress total size to reflect all entries. */
			M_fs_progress_set_size_total(progress, total_size);
		}
		/* Change the progress count to reflect the count. */
		if (progress_flags & M_FS_PROGRESS_COUNT) {
			M_fs_progress_set_count_total(progress, len);
		}
	}

	/* Assume success. Set error if there is an error. */
	res = M_FS_ERROR_SUCCESS;
	/* Loop though all entries and delete. */
	for (i=0; i<len; i++) {
		entry     = M_fs_dir_entries_at(entries, i);
		join_path = M_fs_path_join(norm_path, M_fs_dir_entry_get_name(entry), M_FS_SYSTEM_AUTO);
		/* Call the appropriate delete function. */
		if (M_fs_dir_entry_get_type(entry) == M_FS_TYPE_DIR) {
			res2 = M_fs_delete_dir(join_path);
		} else {
			res2 = M_fs_delete_file(join_path);
		}
		/* Set the return result to denote there was an error. The real error will be sent via the
		 * progress callback for the entry. */
		if (res2 != M_FS_ERROR_SUCCESS) {
			res = M_FS_ERROR_GENERIC;
		}
		/* Set the progress data for the entry. */
		if (cb) {
			entry_size           = M_fs_info_get_size(M_fs_dir_entry_get_info(entry));
			total_size_progress += entry_size;

			M_fs_progress_set_path(progress, join_path);
			M_fs_progress_set_type(progress, M_fs_dir_entry_get_type(entry));
			M_fs_progress_set_result(progress, res2);
			if (progress_flags & M_FS_PROGRESS_COUNT) {
				M_fs_progress_set_count(progress, i+1);
			}
			if (progress_flags & M_FS_PROGRESS_SIZE_TOTAL) {
				M_fs_progress_set_size_total_progess(progress, total_size_progress);
			}
			if (progress_flags & M_FS_PROGRESS_SIZE_CUR) {
				M_fs_progress_set_size_current(progress, entry_size);
				M_fs_progress_set_size_current_progress(progress, entry_size);
			}
		}
		M_free(join_path);
		/* Call the callback and stop processing if requested. */
		if (cb && !cb(progress)) {
			res = M_FS_ERROR_CANCELED;
			break;
		}
	}

	M_fs_dir_entries_destroy(entries);
	M_fs_progress_destroy(progress);
	M_free(norm_path);
	return res;
}
