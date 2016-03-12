/*
	afuse - An automounter using FUSE
	Copyright (C) 2008-2013 Jacob Bower <jacob.bower@gmail.com>

	Portions of this program derive from examples provided with
	FUSE-2.5.2.

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.
*/

#include <config.h>

#ifdef linux
// For pread()/pwrite()
#define _XOPEN_SOURCE 500
// For getline()
#define _GNU_SOURCE
#endif

#include <fuse.h>
#include <fuse_opt.h>
#ifndef __USE_BSD
// for mkdtemp
#define __USE_BSD
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <alloca.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <fnmatch.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>

#if !HAVE_GETLINE && !HAVE_FGETLN
#error Need getline or fgetln
#endif

#ifdef XATTR_NOFOLLOW
#define lgetxattr(p, n, v, s) \
         getxattr(p, n, v, s, 0, XATTR_NOFOLLOW)
#define lsetxattr(p, n, v, s, f) \
         setxattr(p, n, v, s, 0, f | XATTR_NOFOLLOW)
#define llistxattr(p, list, s) \
         listxattr(p, list, s, XATTR_NOFOLLOW)
#define lremovexattr(p, n) \
         removexattr(p, n, XATTR_NOFOLLOW)
#endif				/* XATTR_NOFOLLOW */
#endif				/* HAVE_SETXATTR */

#include "fd_list.h"
#include "dir_list.h"
#include "string_sorted_list.h"
#include "utils.h"

#include "variable_pairing_heap.h"

#define TMP_DIR_TEMPLATE "/tmp/afuse-XXXXXX"
static char *mount_point_directory;
static dev_t mount_point_dev;

// Data structure filled in when parsing command line args
struct user_options_t {
	char *mount_command_template;
	char *unmount_command_template;
	char *populate_root_command;
	char *filter_file;
	bool flush_writes;
	bool exact_getattr;
	uint64_t auto_unmount_delay;
} user_options = {
NULL, NULL, NULL, NULL, false, false, UINT64_MAX};

typedef struct _mount_list_t {
	struct _mount_list_t *next;
	struct _mount_list_t *prev;

	char *root_name;
	char *mount_point;
	fd_list_t *fd_list;
	dir_list_t *dir_list;

	 PH_NEW_LINK(struct _mount_list_t) auto_unmount_ph_node;
	/* This is the sort key for the auto_unmount_ph heap.  It will
	   equal UINT64_MAX if this node is not in the heap. */
	int64_t auto_unmount_time;
} mount_list_t;

typedef struct _mount_filter_list_t {
	struct _mount_filter_list_t *next;

	char *pattern;
} mount_filter_list_t;

PH_DECLARE_TYPE(auto_unmount_ph, mount_list_t)
    PH_DEFINE_TYPE(auto_unmount_ph, mount_list_t, auto_unmount_ph_node,
	       auto_unmount_time)

static mount_filter_list_t *mount_filter_list = NULL;

#define BLOCK_SIGALRM \
	sigset_t block_sigalrm_oldset, block_sigalrm_set;	\
	sigemptyset(&block_sigalrm_set); \
	sigaddset(&block_sigalrm_set, SIGALRM); \
	sigprocmask(SIG_BLOCK, &block_sigalrm_set, &block_sigalrm_oldset)

#define UNBLOCK_SIGALRM \
	sigprocmask(SIG_SETMASK, &block_sigalrm_oldset, NULL)

#define DEFAULT_CASE_INVALID_ENUM  \
		fprintf(stderr, "Unexpected switch value in %s:%s:%d\n", \
			__FILE__, __func__, __LINE__);  \
		exit(1);

static auto_unmount_ph_t auto_unmount_ph;
static int64_t auto_unmount_next_timeout = INT64_MAX;

static void add_mount_filter(const char *glob)
{
	mount_filter_list_t *new_entry;

	new_entry = my_malloc(sizeof(mount_filter_list_t));
	new_entry->pattern = my_strdup(glob);
	new_entry->next = mount_filter_list;

	mount_filter_list = new_entry;
}

static int is_mount_filtered(const char *mount_point)
{
	mount_filter_list_t *current_filter;

	current_filter = mount_filter_list;

	while (current_filter) {
		if (!fnmatch(current_filter->pattern, mount_point, 0))
			return 1;

		current_filter = current_filter->next;
	}

	return 0;
}

static void load_mount_filter_file(const char *filename)
{
	FILE *filter_file;
	if ((filter_file = fopen(filename, "r")) == NULL) {
		fprintf(stderr, "Failed to open filter file '%s'\n", filename);
		exit(1);
	}

	char *line = NULL;
	ssize_t llen;
	size_t lsize;
#ifdef HAVE_GETLINE
	while ((llen = getline(&line, &lsize, filter_file)) != -1)
#else				// HAVE_FGETLN
	while (line = fgetln(filter_file, &llen))
#endif
	{
		if (llen >= 1) {
			if (line[0] == '#')
				continue;

			if (line[llen - 1] == '\n') {
				line[llen - 1] = '\0';
				llen--;
			}
		}

		if (llen > 0)
			add_mount_filter(line);
	}

	free(line);

	fclose(filter_file);
}

static int64_t from_timeval(const struct timeval *tv)
{
	return (int64_t) tv->tv_sec * 1000000 + ((int64_t) tv->tv_usec);
}

static void to_timeval(struct timeval *tv, int64_t usec)
{
	tv->tv_sec = usec / 1000000;
	tv->tv_usec = usec % 1000000;
}

static int get_retval(int res)
{
	if (res == -1)
		return -errno;
	return 0;
}

static int check_mount(mount_list_t * mount)
{
	struct stat buf;

	if (lstat(mount->mount_point, &buf) == -1)
		return 0;
	if (buf.st_dev == mount_point_dev)
		return 0;
	return 1;
}

static void update_auto_unmount(mount_list_t * mount)
{
	if (user_options.auto_unmount_delay == UINT64_MAX)
		return;

	/* Get the current time */
	struct timeval tv;
	mount_list_t *min_mount;
	int64_t cur_time, next_time;
	gettimeofday(&tv, NULL);
	cur_time = from_timeval(&tv);

	if (mount) {
		/* Always remove first */
		if (mount->auto_unmount_time != INT64_MAX)
			auto_unmount_ph_remove(&auto_unmount_ph, mount);

		if (!mount->fd_list && !mount->dir_list) {
			mount->auto_unmount_time =
			    cur_time + user_options.auto_unmount_delay;
			auto_unmount_ph_insert(&auto_unmount_ph, mount);
		} else {
			mount->auto_unmount_time = INT64_MAX;
		}
	}
	min_mount = auto_unmount_ph_min(&auto_unmount_ph);
	next_time = min_mount ? min_mount->auto_unmount_time : INT64_MAX;

	if (next_time != auto_unmount_next_timeout) {
		struct itimerval itv;
		auto_unmount_next_timeout = next_time;

		if (next_time != INT64_MAX) {
			if (next_time > cur_time)
				to_timeval(&itv.it_value, next_time - cur_time);
			else	/* Timer is set to expire immediately --- set it to 1 instead */
				to_timeval(&itv.it_value, 1);
		} else {
			/* Disable the timer */
			to_timeval(&itv.it_value, 0);
		}
		to_timeval(&itv.it_interval, 0);
		if (setitimer(ITIMER_REAL, &itv, NULL) != 0) {
			perror("Error setting timer");
		}
	}
}

int do_umount(mount_list_t * mount);

static void handle_auto_unmount_timer(int x)
{
	(void)x;		/* Ignored */
	/* Get the current time */
	struct timeval tv;
	int64_t cur_time;
	mount_list_t *mount;
	gettimeofday(&tv, NULL);
	cur_time = from_timeval(&tv);

	while ((mount = auto_unmount_ph_min(&auto_unmount_ph)) != NULL &&
	       mount->auto_unmount_time <= cur_time) {
		do_umount(mount);
	}

	update_auto_unmount(NULL);
}

mount_list_t *mount_list = NULL;

mount_list_t *find_mount(const char *root_name)
{
	mount_list_t *current_mount = mount_list;

	while (current_mount) {
		if (strcmp(root_name, current_mount->root_name) == 0)
			return current_mount;

		current_mount = current_mount->next;
	}

	return NULL;
}

int is_mount(const char *root_name)
{
	return find_mount(root_name) ? 1 : 0;
}

mount_list_t *add_mount(const char *root_name, char *mount_point)
{
	mount_list_t *new_mount;

	new_mount = (mount_list_t *) my_malloc(sizeof(mount_list_t));
	new_mount->root_name = my_strdup(root_name);
	new_mount->mount_point = mount_point;

	new_mount->next = mount_list;
	new_mount->prev = NULL;
	new_mount->fd_list = NULL;
	new_mount->dir_list = NULL;
	new_mount->auto_unmount_time = INT64_MAX;
	if (mount_list)
		mount_list->prev = new_mount;

	mount_list = new_mount;

	update_auto_unmount(new_mount);

	return new_mount;
}

void remove_mount(mount_list_t * current_mount)
{
	if (current_mount->auto_unmount_time != INT64_MAX)
		auto_unmount_ph_remove(&auto_unmount_ph, current_mount);

	free(current_mount->root_name);
	free(current_mount->mount_point);
	if (current_mount->prev)
		current_mount->prev->next = current_mount->next;
	else
		mount_list = current_mount->next;
	if (current_mount->next)
		current_mount->next->prev = current_mount->prev;
	free(current_mount);
	update_auto_unmount(NULL);
}

char *make_mount_point(const char *root_name)
{
	char *dir_tmp;

	// Create the mount point
	dir_tmp =
	    my_malloc(strlen(mount_point_directory) + 2 + strlen(root_name));
	strcpy(dir_tmp, mount_point_directory);
	strcat(dir_tmp, "/");
	strcat(dir_tmp, root_name);

	if (mkdir(dir_tmp, 0700) == -1 && errno != EEXIST) {
		fprintf(stderr, "Cannot create directory: %s (%s)\n",
			dir_tmp, strerror(errno));
		free(dir_tmp);
		return NULL;
	}
	return dir_tmp;
}

// Note: this method strips out quotes and applies them itself as should be appropriate
bool run_template(const char *template, const char *mount_point,
		  const char *root_name)
{
	int len = 0;
	int nargs = 1;
	int i;
	char *buf;
	char *p;
	char **args;
	char **arg;
	bool quote = false;
	pid_t pid;
	int status;

	// calculate length
	for (i = 0; template[i]; i++)
		if (template[i] == '%') {
			switch (template[i + 1]) {
			case 'm':
				len += strlen(mount_point);
				i++;
				break;
			case 'r':
				len += strlen(root_name);
				i++;
				break;
			case '%':
				len++;
				i++;
				break;
			}
		} else if (template[i] == ' ' && !quote) {
			len++;
			nargs++;
		} else if (template[i] == '"')
			quote = !quote;
		else if (template[i] == '\\' && template[i + 1])
			len++, i++;
		else
			len++;

	buf = my_malloc(len + 1);
	args = my_malloc((nargs + 1) * sizeof(*args));

	p = buf;
	arg = args;
	*arg++ = p;

	for (i = 0; template[i]; i++)
		if (template[i] == '%') {
			switch (template[i + 1]) {
			case 'm':
				strcpy(p, mount_point);
				p += strlen(mount_point);
				i++;
				break;
			case 'r':
				strcpy(p, root_name);
				p += strlen(root_name);
				i++;
				break;
			case '%':
				*p++ = '%';
				i++;
				break;
			}
		} else if (template[i] == ' ' && !quote) {
			*p++ = '\0';
			*arg++ = p;
		} else if (template[i] == '"')
			quote = !quote;
		else if (template[i] == '\\' && template[i + 1])
			*p++ = template[++i];
		else
			*p++ = template[i];

	*p = '\0';
	*arg = NULL;

	pid = fork();
	if (pid == -1) {
		fprintf(stderr, "Failed to fork (%s)\n", strerror(errno));
		free(args);
		free(buf);
		return false;
	}
	if (pid == 0) {
		execvp(args[0], args);
		abort();
	}
	pid = waitpid(pid, &status, 0);
	if (pid == -1) {
		fprintf(stderr, "Failed to waitpid (%s)\n", strerror(errno));
		free(args);
		free(buf);
		return false;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "Failed to invoke command: %s\n", args[0]);
		free(args);
		free(buf);
		return false;
	}
	free(args);
	free(buf);
	return true;
}

mount_list_t *do_mount(const char *root_name)
{
	char *mount_point;
	mount_list_t *mount;

	fprintf(stderr, "Mounting: %s\n", root_name);

	if (!(mount_point = make_mount_point(root_name))) {
		fprintf(stderr,
			"Failed to create mount point directory: %s/%s\n",
			mount_point_directory, root_name);
		return NULL;
	}

	if (!run_template(user_options.mount_command_template,
			  mount_point, root_name)) {
		// remove the now unused directory
		if (rmdir(mount_point) == -1)
			fprintf(stderr,
				"Failed to remove mount point dir: %s (%s)",
				mount_point, strerror(errno));

		free(mount_point);
		return NULL;
	}

	mount = add_mount(root_name, mount_point);
	return mount;
}

int do_umount(mount_list_t * mount)
{
	fprintf(stderr, "Unmounting: %s\n", mount->root_name);

	run_template(user_options.unmount_command_template,
		     mount->mount_point, mount->root_name);
	/* Still unmount anyway */

	if (rmdir(mount->mount_point) == -1)
		fprintf(stderr, "Failed to remove mount point dir: %s (%s)",
			mount->mount_point, strerror(errno));
	remove_mount(mount);
	return 1;
}

void unmount_all(void)
{
	fprintf(stderr, "Attempting to unmount all filesystems:\n");

	while (mount_list) {
		fprintf(stderr, "\tUnmounting: %s\n", mount_list->root_name);

		do_umount(mount_list);
	}

	fprintf(stderr, "done.\n");
}

void shutdown(void)
{
	BLOCK_SIGALRM;

	unmount_all();

	UNBLOCK_SIGALRM;

	if (rmdir(mount_point_directory) == -1)
		fprintf(stderr,
			"Failed to remove temporary mount point directory: %s (%s)\n",
			mount_point_directory, strerror(errno));
}

int max_path_out_len(const char *path_in)
{
	return strlen(mount_point_directory) + strlen(path_in) + 2;
}

// returns true if path is a child directory of a root node
// e.g. /a/b is a child, /a is not.
int extract_root_name(const char *path, char *root_name)
{
	int i;

	for (i = 1; path[i] && path[i] != '/'; i++)
		root_name[i - 1] = path[i];
	root_name[i - 1] = '\0';

	return strlen(&path[i]);
}

typedef enum {
	PROC_PATH_FAILED,
	PROC_PATH_ROOT_DIR,
	PROC_PATH_ROOT_SUBDIR,
	PROC_PATH_PROXY_DIR
} proc_result_t;

proc_result_t process_path(const char *path_in, char *path_out, char *root_name,
			   int attempt_mount, mount_list_t ** out_mount)
{
	char *path_out_base;
	int is_child;
	int len;
	mount_list_t *mount = NULL;

	*out_mount = NULL;

	fprintf(stderr, "Path in: %s\n", path_in);
	is_child = extract_root_name(path_in, root_name);
	fprintf(stderr, "root_name is: %s\n", root_name);

	if (is_mount_filtered(root_name))
		return PROC_PATH_FAILED;

	// Mount filesystem if necessary
	// the combination of is_child and attempt_mount prevent inappropriate
	// mounting of a filesystem for example if the user tries to mknod
	// in the afuse root this should cause an error not a mount.
	// !!FIXME!! this is broken on FUSE < 2.5 (?) because a getattr
	// on the root node seems to occur with every single access.
	if ((is_child || attempt_mount) &&
	    strlen(root_name) > 0 &&
	    !(mount = find_mount(root_name)) && !(mount = do_mount(root_name)))
		return PROC_PATH_FAILED;

	if (mount && !check_mount(mount)) {
		do_umount(mount);
		mount = do_mount(root_name);
		if (!mount)
			return PROC_PATH_FAILED;
	}
	// construct path_out (mount_point_directory + '/' + path_in + '\0')
	path_out_base = path_out;
	len = strlen(mount_point_directory);
	memcpy(path_out, mount_point_directory, len);
	path_out += len;
	*path_out++ = '/';
	len = strlen(path_in) - 1;
	memcpy(path_out, path_in + 1, len);
	path_out += len;
	*path_out = '\0';
	fprintf(stderr, "Path out: %s\n", path_out_base);

	*out_mount = mount;

	if (is_child)
		return PROC_PATH_PROXY_DIR;
	else if (strlen(root_name))
		return PROC_PATH_ROOT_SUBDIR;
	else
		return PROC_PATH_ROOT_DIR;
}

static int afuse_getattr(const char *path, struct stat *stbuf)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	int retval;
	mount_list_t *mount;
	BLOCK_SIGALRM;

	fprintf(stderr, "> GetAttr\n");

	switch (process_path(path, real_path, root_name, 0, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;

	case PROC_PATH_ROOT_DIR:
		fprintf(stderr, "Getattr on: (%s) - %s\n", path, root_name);
		stbuf->st_mode = S_IFDIR | 0700;
		stbuf->st_nlink = 1;
		stbuf->st_uid = getuid();
		stbuf->st_gid = getgid();
		stbuf->st_size = 0;
		stbuf->st_blksize = 0;
		stbuf->st_blocks = 0;
		stbuf->st_atime = 0;
		stbuf->st_mtime = 0;
		stbuf->st_ctime = 0;
		retval = 0;
		break;
	case PROC_PATH_ROOT_SUBDIR:
		if (user_options.exact_getattr)
			/* try to mount it */
			process_path(path, real_path, root_name, 1, &mount);
		if (!mount) {
			stbuf->st_mode = S_IFDIR | 0000;
			if (!user_options.exact_getattr)
				stbuf->st_mode = S_IFDIR | 0750;
			stbuf->st_nlink = 1;
			stbuf->st_uid = getuid();
			stbuf->st_gid = getgid();
			stbuf->st_size = 0;
			stbuf->st_blksize = 0;
			stbuf->st_blocks = 0;
			stbuf->st_atime = 0;
			stbuf->st_mtime = 0;
			stbuf->st_ctime = 0;
			retval = 0;
			break;
		}

	case PROC_PATH_PROXY_DIR:
		retval = get_retval(lstat(real_path, stbuf));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_readlink(const char *path, char *buf, size_t size)
{
	int res;
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	int retval;
	mount_list_t *mount;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 1, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
		retval = -ENOENT;
		break;
	case PROC_PATH_ROOT_SUBDIR:
		if (!mount) {
			retval = -ENOENT;
			break;
		}
	case PROC_PATH_PROXY_DIR:
		res = readlink(real_path, buf, size - 1);
		if (res == -1) {
			retval = -errno;
			break;
		}
		buf[res] = '\0';
		retval = 0;
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_opendir(const char *path, struct fuse_file_info *fi)
{
	DIR *dp;
	char *root_name = alloca(strlen(path));
	mount_list_t *mount;
	char *real_path = alloca(max_path_out_len(path));
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 1, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
		retval = 0;
		break;
	case PROC_PATH_ROOT_SUBDIR:
		if (!mount) {
			retval = -EACCES;
			fi->fh = 0lu;
			break;
		}
	case PROC_PATH_PROXY_DIR:
		dp = opendir(real_path);
		if (dp == NULL) {
			retval = -errno;
			break;
		}
		fi->fh = (unsigned long)dp;
		if (mount)
			dir_list_add(&mount->dir_list, dp);
		retval = 0;
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static inline DIR *get_dirp(struct fuse_file_info *fi)
{
	return (DIR *) (uintptr_t) fi->fh;
}

int populate_root_dir(char *pop_cmd, struct list_t **dir_entry_listptr,
		      fuse_fill_dir_t filler, void *buf)
{
	FILE *browser;
	size_t hsize = 0;
	ssize_t hlen;
	char *dir_entry = NULL;

	if (!pop_cmd)
		return -1;

	if ((browser = popen(pop_cmd, "r")) == NULL) {
		fprintf(stderr, "Failed to execute populate_root_command=%s\n",
			pop_cmd);
		return -errno;
	}

	int loop_error = 0;
#ifdef HAVE_GETLINE
	while ((hlen = getline(&dir_entry, &hsize, browser)) != -1)
#else				// HAVE_FGETLN
	while (dir_entry = fgetln(browser, &hsize))
#endif
	{
		if (hlen >= 1 && dir_entry[hlen - 1] == '\n')
			dir_entry[hlen - 1] = '\0';

		fprintf(stderr, "Got entry \"%s\"\n", dir_entry);

		int insert_err =
		    insert_sorted_if_unique(dir_entry_listptr, dir_entry);
		if (insert_err == 1)	// already in list
			continue;
		else if (insert_err) {
			fprintf(stderr,
				"populate_root_command: failed on inserting new entry into sorted list.\n");
			loop_error = 1;
		}

		if (strlen(dir_entry) != 0)
			filler(buf, dir_entry, NULL, 0);
	}

	free(dir_entry);

	int pclose_err = pclose(browser);
	if (pclose_err) {
		int pclose_errno = errno;
		fprintf(stderr,
			"populate_root_command: pclose failed, ret %d, status %d, errno %d (%s)\n",
			pclose_errno, WEXITSTATUS(pclose_errno), pclose_errno,
			strerror(pclose_errno));
	}

	return loop_error || pclose_err;
}

static int afuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	DIR *dp = get_dirp(fi);
	struct dirent *de;
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	struct list_t *dir_entry_list = NULL;
	mount_list_t *mount, *next;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 1, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;

	case PROC_PATH_ROOT_DIR:
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);
		insert_sorted_if_unique(&dir_entry_list, ".");
		insert_sorted_if_unique(&dir_entry_list, "..");
		for (mount = mount_list; mount; mount = next) {
			next = mount->next;
			/* Check for dead mounts. */
			if (!check_mount(mount)) {
				do_umount(mount);
			} else {
				if (insert_sorted_if_unique
				    (&dir_entry_list, mount->root_name))
					retval = -1;
				filler(buf, mount->root_name, NULL, 0);
			}
		}
		populate_root_dir(user_options.populate_root_command,
				  &dir_entry_list, filler, buf);
		destroy_list(&dir_entry_list);
		mount = NULL;
		retval = 0;
		break;

	case PROC_PATH_ROOT_SUBDIR:
		if (!mount) {
			retval = (!dp) ? -EBADF : -EACCES;
			break;
		}
	case PROC_PATH_PROXY_DIR:
		seekdir(dp, offset);
		while ((de = readdir(dp)) != NULL) {
			struct stat st;
			memset(&st, 0, sizeof(st));
			st.st_ino = de->d_ino;
			st.st_mode = de->d_type << 12;
			if (filler(buf, de->d_name, &st, telldir(dp)))
				break;
		}
		retval = 0;
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_releasedir(const char *path, struct fuse_file_info *fi)
{
	DIR *dp = get_dirp(fi);
	mount_list_t *mount;
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	int retval;

	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 1, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;

	case PROC_PATH_ROOT_DIR:
		retval = 0;
		break;

	case PROC_PATH_ROOT_SUBDIR:
	case PROC_PATH_PROXY_DIR:
		if (mount)
			dir_list_remove(&mount->dir_list, dp);
		if (dp)
			closedir(dp);
		retval = 0;
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_mknod(const char *path, mode_t mode, dev_t rdev)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;
	fprintf(stderr, "> Mknod\n");

	switch (process_path(path, real_path, root_name, 0, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;

	case PROC_PATH_ROOT_DIR:
	case PROC_PATH_ROOT_SUBDIR:
		retval = -ENOTSUP;
		break;

	case PROC_PATH_PROXY_DIR:
		if (S_ISFIFO(mode))
			retval = get_retval(mkfifo(real_path, mode));
		else
			retval = get_retval(mknod(real_path, mode, rdev));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_mkdir(const char *path, mode_t mode)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	int retval;
	mount_list_t *mount;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 0, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
	case PROC_PATH_ROOT_SUBDIR:
		retval = -ENOTSUP;
		break;
	case PROC_PATH_PROXY_DIR:
		retval = get_retval(mkdir(real_path, mode));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_unlink(const char *path)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 0, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
	case PROC_PATH_ROOT_SUBDIR:
		retval = -ENOTSUP;
		break;
	case PROC_PATH_PROXY_DIR:
		retval = get_retval(unlink(real_path));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_rmdir(const char *path)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 0, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
		retval = -ENOTSUP;
		break;
	case PROC_PATH_ROOT_SUBDIR:
		if (mount) {
			/* Unmount */
			if (mount->dir_list || mount->fd_list)
				retval = -EBUSY;
			else {
				do_umount(mount);
				mount = NULL;
				retval = 0;
			}
		} else
			retval = -ENOTSUP;
		break;
	case PROC_PATH_PROXY_DIR:
		retval = get_retval(rmdir(real_path));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_symlink(const char *from, const char *to)
{
	char *root_name_to = alloca(strlen(to));
	char *real_to_path = alloca(max_path_out_len(to));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(to, real_to_path, root_name_to, 0, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
	case PROC_PATH_ROOT_SUBDIR:
		retval = -ENOTSUP;
		break;
	case PROC_PATH_PROXY_DIR:
		retval = get_retval(symlink(from, real_to_path));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_rename(const char *from, const char *to)
{
	char *root_name_from = alloca(strlen(from));
	char *root_name_to = alloca(strlen(to));
	char *real_from_path = alloca(max_path_out_len(from));
	char *real_to_path = alloca(max_path_out_len(to));
	mount_list_t *mount_from, *mount_to = NULL;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path
		(from, real_from_path, root_name_from, 0, &mount_from)) {

	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;

	case PROC_PATH_ROOT_DIR:
	case PROC_PATH_ROOT_SUBDIR:
		retval = -ENOTSUP;
		break;

	case PROC_PATH_PROXY_DIR:
		switch (process_path
			(to, real_to_path, root_name_to, 0, &mount_to)) {

		case PROC_PATH_FAILED:
			retval = -ENXIO;
			break;

		case PROC_PATH_ROOT_DIR:
		case PROC_PATH_ROOT_SUBDIR:
			retval = -ENOTSUP;
			break;

		case PROC_PATH_PROXY_DIR:
			retval =
			    get_retval(rename(real_from_path, real_to_path));
			break;

		default:
			DEFAULT_CASE_INVALID_ENUM;
		}
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount_to)
		update_auto_unmount(mount_to);
	if (mount_from && mount_from != mount_to)
		update_auto_unmount(mount_from);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_link(const char *from, const char *to)
{
	char *root_name_from = alloca(strlen(from));
	char *root_name_to = alloca(strlen(to));
	char *real_from_path = alloca(max_path_out_len(from));
	char *real_to_path = alloca(max_path_out_len(to));
	mount_list_t *mount_to = NULL, *mount_from;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path
		(from, real_from_path, root_name_from, 0, &mount_from)) {

	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
	case PROC_PATH_ROOT_SUBDIR:
		retval = -ENOTSUP;
		break;
	case PROC_PATH_PROXY_DIR:
		switch (process_path
			(to, real_to_path, root_name_to, 0, &mount_to)) {

		case PROC_PATH_FAILED:
			retval = -ENXIO;
			break;
		case PROC_PATH_ROOT_DIR:
		case PROC_PATH_ROOT_SUBDIR:
			retval = -ENOTSUP;
			break;
		case PROC_PATH_PROXY_DIR:
			retval = get_retval(link(real_from_path, real_to_path));
			break;

		default:
			DEFAULT_CASE_INVALID_ENUM;
		}
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount_to)
		update_auto_unmount(mount_to);
	if (mount_from && mount_from != mount_to)
		update_auto_unmount(mount_from);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_chmod(const char *path, mode_t mode)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 0, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
	case PROC_PATH_ROOT_SUBDIR:
		retval = -ENOTSUP;
		break;
	case PROC_PATH_PROXY_DIR:
		retval = get_retval(chmod(real_path, mode));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_chown(const char *path, uid_t uid, gid_t gid)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 0, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
	case PROC_PATH_ROOT_SUBDIR:
		retval = -ENOTSUP;
		break;
	case PROC_PATH_PROXY_DIR:
		retval = get_retval(lchown(real_path, uid, gid));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_truncate(const char *path, off_t size)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 0, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
	case PROC_PATH_ROOT_SUBDIR:
		retval = -ENOTSUP;
		break;
	case PROC_PATH_PROXY_DIR:
		retval = get_retval(truncate(real_path, size));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_utime(const char *path, struct utimbuf *buf)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 0, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
		retval = -ENOTSUP;
		break;
	case PROC_PATH_ROOT_SUBDIR:
		if (!mount) {
			retval = -ENOTSUP;
			break;
		}
	case PROC_PATH_PROXY_DIR:
		retval = get_retval(utime(real_path, buf));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_open(const char *path, struct fuse_file_info *fi)
{
	int fd;
	char *root_name = alloca(strlen(path));
	mount_list_t *mount;
	char *real_path = alloca(max_path_out_len(path));
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 1, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
	case PROC_PATH_ROOT_SUBDIR:
		retval = -ENOENT;
		break;
	case PROC_PATH_PROXY_DIR:
		fd = open(real_path, fi->flags);
		if (fd == -1) {
			retval = -errno;
			break;
		}

		fi->fh = fd;
		if (mount)
			fd_list_add(&mount->fd_list, fd);
		retval = 0;
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	int res;

	(void)path;
	res = pread(fi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;

	return res;
}

static int afuse_write(const char *path, const char *buf, size_t size,
		       off_t offset, struct fuse_file_info *fi)
{
	int res;

	(void)path;
	res = pwrite(fi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;

	if (user_options.flush_writes)
		fsync(fi->fh);

	return res;
}

static int afuse_release(const char *path, struct fuse_file_info *fi)
{
	char *root_name = alloca(strlen(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	extract_root_name(path, root_name);
	mount = find_mount(root_name);
	retval = get_retval(close(fi->fh));

	if (mount) {
		fd_list_remove(&mount->fd_list, fi->fh);
		update_auto_unmount(mount);
	}

	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_fsync(const char *path, int isdatasync,
		       struct fuse_file_info *fi)
{
	int res;
	(void)path;

#ifndef HAVE_FDATASYNC
	(void)isdatasync;
#else
	if (isdatasync)
		res = fdatasync(fi->fh);
	else
#endif
		res = fsync(fi->fh);
	return get_retval(res);
}

#if FUSE_VERSION >= 25
static int afuse_access(const char *path, int mask)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 1, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
	case PROC_PATH_PROXY_DIR:
		retval = get_retval(access(real_path, mask));
		break;
	case PROC_PATH_ROOT_SUBDIR:
		if (mount)
			retval = get_retval(access(real_path, mask));
		else
			retval = -EACCES;
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_ftruncate(const char *path, off_t size,
			   struct fuse_file_info *fi)
{
	(void)path;
	return get_retval(ftruncate(fi->fh, size));
}

static int afuse_create(const char *path, mode_t mode,
			struct fuse_file_info *fi)
{
	int fd;
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 0, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
	case PROC_PATH_ROOT_SUBDIR:
		retval = -ENOTSUP;
		break;
	case PROC_PATH_PROXY_DIR:
		fd = open(real_path, fi->flags, mode);
		if (fd == -1) {
			retval = -errno;
			break;
		}
		fi->fh = fd;
		retval = 0;
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_fgetattr(const char *path, struct stat *stbuf,
			  struct fuse_file_info *fi)
{
	(void)path;

	return get_retval(fstat(fi->fh, stbuf));
}
#endif

#if FUSE_VERSION >= 25
static int afuse_statfs(const char *path, struct statvfs *stbuf)
#else
static int afuse_statfs(const char *path, struct statfs *stbuf)
#endif
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 1, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;

	case PROC_PATH_ROOT_DIR:
#if FUSE_VERSION >= 25
		stbuf->f_namemax = 0x7fffffff;
		stbuf->f_frsize = 512;
#else
		stbuf->f_namelen = 0x7fffffff;
#endif
		stbuf->f_bsize = 1024;
		stbuf->f_blocks = 0;
		stbuf->f_bfree = 0;
		stbuf->f_bavail = 0;
		stbuf->f_files = 0;
		stbuf->f_ffree = 0;
		retval = 0;
		break;

	case PROC_PATH_ROOT_SUBDIR:
		if (!mount) {
			retval = -EACCES;
			break;
		}
	case PROC_PATH_PROXY_DIR:
		retval = get_retval(statvfs(real_path, stbuf));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

void afuse_destroy(void *p)
{
	(void)p;		/* Unused */
	shutdown();
}

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int afuse_setxattr(const char *path, const char *name, const char *value,
			  size_t size, int flags)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 0, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
		retval = -ENOENT;
		break;
	case PROC_PATH_ROOT_SUBDIR:
		if (!mount) {
			retval = -ENOTSUP;
			break;
		}
	case PROC_PATH_PROXY_DIR:
		retval =
		    get_retval(lsetxattr(real_path, name, value, size, flags));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_getxattr(const char *path, const char *name, char *value,
			  size_t size)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 1, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
		retval = -ENOTSUP;
		break;
	case PROC_PATH_ROOT_SUBDIR:
		if (!mount) {
			retval = -ENOTSUP;
			break;
		}
	case PROC_PATH_PROXY_DIR:
		retval = get_retval(lgetxattr(real_path, name, value, size));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_listxattr(const char *path, char *list, size_t size)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 1, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
		retval = -ENOTSUP;
		break;
	case PROC_PATH_ROOT_SUBDIR:
		if (!mount) {
			retval = -ENOTSUP;
			break;
		}
	case PROC_PATH_PROXY_DIR:
		retval = get_retval(llistxattr(real_path, list, size));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}

static int afuse_removexattr(const char *path, const char *name)
{
	char *root_name = alloca(strlen(path));
	char *real_path = alloca(max_path_out_len(path));
	mount_list_t *mount;
	int retval;
	BLOCK_SIGALRM;

	switch (process_path(path, real_path, root_name, 0, &mount)) {
	case PROC_PATH_FAILED:
		retval = -ENXIO;
		break;
	case PROC_PATH_ROOT_DIR:
		retval = -ENOTSUP;
		break;
	case PROC_PATH_ROOT_SUBDIR:
		if (!mount) {
			retval = -ENOTSUP;
			break;
		}
	case PROC_PATH_PROXY_DIR:
		retval = get_retval(lremovexattr(real_path, name));
		break;

	default:
		DEFAULT_CASE_INVALID_ENUM;
	}
	if (mount)
		update_auto_unmount(mount);
	UNBLOCK_SIGALRM;
	return retval;
}
#endif				/* HAVE_SETXATTR */

static struct fuse_operations afuse_oper = {
	.getattr = afuse_getattr,
	.readlink = afuse_readlink,
	.opendir = afuse_opendir,
	.readdir = afuse_readdir,
	.releasedir = afuse_releasedir,
	.mknod = afuse_mknod,
	.mkdir = afuse_mkdir,
	.symlink = afuse_symlink,
	.unlink = afuse_unlink,
	.rmdir = afuse_rmdir,
	.rename = afuse_rename,
	.link = afuse_link,
	.chmod = afuse_chmod,
	.chown = afuse_chown,
	.truncate = afuse_truncate,
	.utime = afuse_utime,
	.open = afuse_open,
	.read = afuse_read,
	.write = afuse_write,
	.release = afuse_release,
	.fsync = afuse_fsync,
	.statfs = afuse_statfs,
#if FUSE_VERSION >= 25
	.access = afuse_access,
	.create = afuse_create,
	.ftruncate = afuse_ftruncate,
	.fgetattr = afuse_fgetattr,
#endif
	.destroy = afuse_destroy,
#ifdef HAVE_SETXATTR
	.setxattr = afuse_setxattr,
	.getxattr = afuse_getxattr,
	.listxattr = afuse_listxattr,
	.removexattr = afuse_removexattr,
#endif
};

enum {
	KEY_HELP,
	KEY_FLUSHWRITES,
	KEY_EXACT_GETATTR
};

#define AFUSE_OPT(t, p, v) { t, offsetof(struct user_options_t, p), v }

static struct fuse_opt afuse_opts[] = {
	AFUSE_OPT("mount_template=%s", mount_command_template, 0),
	AFUSE_OPT("unmount_template=%s", unmount_command_template, 0),
	AFUSE_OPT("populate_root_command=%s", populate_root_command, 0),
	AFUSE_OPT("filter_file=%s", filter_file, 0),

	AFUSE_OPT("timeout=%llu", auto_unmount_delay, 0),

	FUSE_OPT_KEY("exact_getattr", KEY_EXACT_GETATTR),
	FUSE_OPT_KEY("flushwrites", KEY_FLUSHWRITES),
	FUSE_OPT_KEY("-h", KEY_HELP),
	FUSE_OPT_KEY("--help", KEY_HELP),

	FUSE_OPT_END
};

static void usage(const char *progname)
{
	fprintf(stderr,
		"Usage: %s mountpoint [options]\n"
		"\n"
		"    -o opt,[opt...]        mount options\n"
		"    -h   --help            print help\n"
		"    -V   --version         print FUSE version information\n"
		"\n"
		"afuse options:\n"
		"    -o mount_template=CMD         template for CMD to execute to mount (1)\n"
		"    -o unmount_template=CMD       template for CMD to execute to unmount (1) (2)\n"
		"    -o populate_root_command=CMD  CMD to execute providing root directory list (3)\n"
		"    -o filter_file=FILE           FILE listing ignore filters for mount points (4)\n"
		"    -o timeout=TIMEOUT            automatically unmount after TIMEOUT seconds\n"
		"    -o flushwrites                flushes data to disk for all file writes\n"
		"    -o exact_getattr              allows getattr calls to cause a mount\n" 
		"\n\n"
		" (1) - When executed, %%r is expanded to the directory name inside the\n"
		"       afuse mount, and %%m is expanded to the actual directory to mount\n"
		"       onto. Both templates are REQUIRED.\n"
		"\n"
		" (2) - The unmount command must perform a lazy unmount operation. E.g. the\n"
		"       -u -z options to fusermount, or -l for regular mount.\n"
		"\n"
		" (3) - The populate_root command should output one dir entry per line,\n"
		"       and return immediately. It is run for each directory listing request.\n"
		"\n"
		" (4) - Each line of the filter file is a shell wildcard filter (glob). A '#'\n"
		"       as the first character on a line ignores a filter.\n"
		"\n"
		" The following filter patterns are hard-coded:"
		"\n", progname);

	mount_filter_list_t *cur = mount_filter_list;
	while (cur) {
		fprintf(stderr, "    %s\n", cur->pattern);
		cur = cur->next;
	}

	fprintf(stderr, "\n");
}

static int afuse_opt_proc(void *data, const char *arg, int key,
			  struct fuse_args *outargs)
{
	/* Unused */
	(void)arg;
	(void)data;

	switch (key) {
	case KEY_HELP:
		usage(outargs->argv[0]);
		fuse_opt_add_arg(outargs, "-ho");
		fuse_main(outargs->argc, outargs->argv, &afuse_oper);
		exit(1);

	case KEY_FLUSHWRITES:
		user_options.flush_writes = true;
		return 0;

	case KEY_EXACT_GETATTR:
		user_options.exact_getattr = true;
		return 0;

	default:
		return 1;
	}
}

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	char *temp_dir_name = my_malloc(strlen(TMP_DIR_TEMPLATE));
	strcpy(temp_dir_name, TMP_DIR_TEMPLATE);

	if (fuse_opt_parse(&args, &user_options, afuse_opts, afuse_opt_proc) ==
	    -1)
		return 1;

	// !!FIXME!! force single-threading for now as data structures are not locked
	fuse_opt_add_arg(&args, "-s");

	// Adjust user specified timeout from seconds to microseconds as required
	if (user_options.auto_unmount_delay != UINT64_MAX)
		user_options.auto_unmount_delay *= 1000000;

	auto_unmount_ph_init(&auto_unmount_ph);

	/**
	 * Install the SIGALRM signal handler.
	 */
	{
		struct sigaction act;
		act.sa_handler = &handle_auto_unmount_timer;
		sigemptyset(&act.sa_mask);
		act.sa_flags = 0;
		while (sigaction(SIGALRM, &act, NULL) == -1 && errno == EINTR)
			continue;
	}

	// Check for required parameters
	if (!user_options.mount_command_template
	    || !user_options.unmount_command_template) {
		fprintf(stderr, "(Un)Mount command templates missing.\n\n");
		usage(argv[0]);
		fuse_opt_add_arg(&args, "-ho");
		fuse_main(args.argc, args.argv, &afuse_oper);

		return 1;
	}

	if (user_options.filter_file)
		load_mount_filter_file(user_options.filter_file);

	if (!(mount_point_directory = mkdtemp(temp_dir_name))) {
		fprintf(stderr,
			"Failed to create temporary mount point dir.\n");
		return 1;
	}

	{
		struct stat buf;
		if (lstat(mount_point_directory, &buf) == -1) {
			fprintf(stderr,
				"Failed to stat temporary mount point dir.\n");
			return 1;
		}
		mount_point_dev = buf.st_dev;
	}

	umask(0);

	// !!FIXME!! death by signal doesn't unmount fs
	return fuse_main(args.argc, args.argv, &afuse_oper);
}
