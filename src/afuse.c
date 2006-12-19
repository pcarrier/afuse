/*
	afuse -  An automounter using FUSE
	Copyright (C) 2006  Jacob Bower <jacob.bower@ic.ac.uk>

	Portions of this program derive from examples provided with
	FUSE-2.5.2.

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.
*/

#include <config.h>

#ifdef linux
// For pread()/pwrite()
#define _XOPEN_SOURCE 500
#endif

#include <fuse.h>
#include <fuse_opt.h>
// for mkdtemp
#define __USE_BSD
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
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include "fd_list.h"
#include "dir_list.h"
#include "utils.h"

#define TMP_DIR_TEMPLATE "/tmp/afuse-XXXXXX"
char *mount_point_directory;

// Data structure filled in when parsing command line args
struct user_options_t {
	char *mount_command_template;
	char *unmount_command_template;
	bool mount_inuse_only;
	bool mount_inuse_only_all;
	bool flush_writes;
} user_options = {NULL, NULL, false, false, false};

typedef struct _mount_list_t {
	struct _mount_list_t *next;
	struct _mount_list_t *prev;

	char *root_name;
	fd_list_t *fd_list;
	dir_list_t *dir_list;
} mount_list_t;

mount_list_t *mount_list = NULL;

mount_list_t *find_mount(const char *root_name)
{
	mount_list_t *current_mount = mount_list;

	while(current_mount) {
		if( strcmp(root_name, current_mount->root_name) == 0)
			return current_mount;
			
		current_mount = current_mount->next;
	}

	return NULL;
}
       
int is_mount(const char *root_name)
{
	return find_mount(root_name) ? 1 : 0;
}

void add_mount(const char *root_name)
{
	mount_list_t *new_mount;
	
	new_mount = (mount_list_t *)my_malloc( sizeof(mount_list_t) );
	new_mount->root_name = my_strdup(root_name);

	new_mount->next = mount_list;
	new_mount->prev = NULL;
	new_mount->fd_list = NULL;
	new_mount->dir_list = NULL;
	if(mount_list)
		mount_list->prev = new_mount;
	
	mount_list = new_mount;
}

void remove_mount(const char *root_name)
{
	mount_list_t *current_mount = mount_list;

	while(current_mount) {
		if( strcmp(root_name, current_mount->root_name) == 0) {
			free(current_mount->root_name);
			if(current_mount->prev)
				current_mount->prev->next = current_mount->next;
			else
				mount_list = current_mount->next;
			if(current_mount->next)
				current_mount->next->prev = current_mount->prev;
			free(current_mount);

			return;
		}

		current_mount = current_mount->next;
	}
}

int make_mount_point(const char *root_name)
{
	char *dir_tmp;
	int i;
	
	// First create the mount_point_directory
	dir_tmp = my_strdup(mount_point_directory);
	for(i = 0; dir_tmp[i]; i++)
		if(dir_tmp[i] == '/' && i != 0) {
			dir_tmp[i] = '\0';
			if(mkdir(dir_tmp, 0700) == -1 && errno != EEXIST) {
				fprintf(stderr, "Cannot create directory: %s (%s)\n",
				        dir_tmp, strerror(errno));
				return 0;
			}
			dir_tmp[i] = '/';
		}
	free(dir_tmp);
	
	// Create the mount point
	dir_tmp = my_malloc(strlen(mount_point_directory) + 2 + strlen(root_name));
	strcpy(dir_tmp, mount_point_directory);
	strcat(dir_tmp, "/");
	strcat(dir_tmp, root_name);
	
	if(mkdir(dir_tmp, 0700) == -1 && errno != EEXIST) {
		fprintf(stderr, "Cannot create directory: %s (%s)\n",
			dir_tmp, strerror(errno));
		return 0;
	}
	free(dir_tmp);

	return 1;
}


// !!FIXME!! allow escaping of %'s
char *expand_template(const char *template, const char *mount_point, const char *root_name)
{
	int len = 0;
	int i;
	char *expanded_name;
	char *expanded_name_start;

	// calculate length
	for(i = 0; template[i]; i++)
		if(template[i] == '%') {
			switch(template[i + 1])
			{
				case 'm':
					len += strlen(mount_point);
					i++;
					break;
				case 'r':
					len += strlen(root_name);
					i++;
					break;
			}
		} else
			len++;
	
	expanded_name_start = expanded_name = my_malloc(len + 1);

	for(i = 0; template[i]; i++)
		if(template[i] == '%') {
			int j = 0;
			switch(template[i + 1])
			{
				case 'm':
					while(mount_point[j])
						*expanded_name++ = mount_point[j++];
					i++;
					break;
				case 'r':
					while(root_name[j])
						*expanded_name++ = root_name[j++];
					i++;
					break;
			}
		} else
			*expanded_name++ = template[i];
	
	*expanded_name = '\0';
	
	return expanded_name_start;
}

int do_mount(const char *root_name)
{
	char *mount_point;
	char *mount_command;
	mount_list_t *mount;
	int sysret;

	fprintf(stderr, "Mounting: %s\n", root_name);

	if( !make_mount_point(root_name) ) {
		fprintf(stderr, "Failed to create mount point directory: %s/%s\n",
		        mount_point_directory, root_name);
		return 0;
	}
		
	mount_point = alloca(strlen(mount_point_directory) + 2 + strlen(root_name));
	sprintf(mount_point, "%s/%s", mount_point_directory, root_name);
	
	mount_command = expand_template(user_options.mount_command_template,
	                                mount_point, root_name);
	sysret = system(mount_command);
	free(mount_command);

	fprintf(stderr, "sysret: %.8x\n", sysret);

	if(sysret) {
		fprintf(stderr, "Failed to invoke mount command: \"%s\" (%s)\n",
		        mount_command, sysret != -1 ?
			               "Error executing mount" :
				       strerror(errno));

		// remove the now unused directory
		if( rmdir(mount_point) == -1 )
			fprintf(stderr, "Failed to remove mount point dir: %s (%s)",
				mount_point, strerror(errno));

		return 0;
	}

	add_mount(root_name);

	return 1;
}

int do_umount(const char *root_name)
{
	char *mount_point;
	char *unmount_command;
	mount_list_t *mount;
	int sysret;

	fprintf(stderr, "Unmounting: %s\n", root_name);
		
	mount = find_mount(root_name);
	if(!mount) {
		fprintf(stderr, "Internal Error: tried to unmount non-existant mount point: %s\n", root_name);
		return 1;
	}
	
	mount_point = alloca(strlen(mount_point_directory) + 2 + strlen(root_name));
	sprintf(mount_point, "%s/%s", mount_point_directory, root_name);

	unmount_command = expand_template(user_options.unmount_command_template,
	                                  mount_point, root_name);
	sysret = system(unmount_command);
	free(unmount_command);
	if(sysret) {
		fprintf(stderr, "Failed to invoke unmount command: \"%s\" (%s)\n",
		        unmount_command, sysret != -1 ?
			               "Error executing mount" :
				       strerror(errno));
		return 0;
	}
	
	// tidy up after succesful unmount
	remove_mount(root_name);
	if( rmdir(mount_point) == -1 )
		fprintf(stderr, "Failed to remove mount point dir: %s (%s)",
			mount_point, strerror(errno));

	return 1;
}

void unmount_all(void)
{
	fprintf(stderr, "Attemping to unmount all filesystems:\n");
	
	while(mount_list) {
		fprintf(stderr, "\tUnmounting: %s\n", mount_list->root_name);
		
		// if unmount fails, ditch the mount anyway
		if( !do_umount(mount_list->root_name) )
			remove_mount(mount_list->root_name);
	}
	
	fprintf(stderr, "done.\n");
}

void shutdown(void)
{
	unmount_all();

	if(rmdir(mount_point_directory) == -1)
		fprintf(stderr, "Failed to remove temporary mount point directory: %s (%s)\n",
		        mount_point_directory, strerror(errno));
}

int max_path_out_len(const char *path_in)
{
	return strlen(mount_point_directory) + strlen(path_in) + 2;
}

// returns true if path is the a child directory of a root node
// e.g. /a/b is a child, /a is not.
int extract_root_name(const char *path, char *root_name)
{
	int i;
	int is_child;
	
	for(i = 1; path[i] && path[i] != '/'; i++)
		root_name[i - 1] = path[i];
	root_name[i - 1] = '\0';

	return strlen(&path[i]);
}

typedef enum {PROC_PATH_FAILED, PROC_PATH_ROOT_DIR, PROC_PATH_PROXY_DIR} proc_result_t;

proc_result_t process_path(const char *path_in, char *path_out, char *root_name, int attempt_mount)
{
	int i;
	char *path_out_base;
	int is_child;

	fprintf(stderr, "Path in: %s\n", path_in);
	is_child = extract_root_name(path_in, root_name);
	fprintf(stderr, "root_name is: %s\n", root_name);
	
	// Mount filesystem if neccessary
	// the combination of is_child and attempt_mount prevent inappropriate
	// mounting of a filesystem for example if the user tries to mknod
	// in the afuse root this should cause an error not a mount.
	// !!FIXME!! this is broken on FUSE < 2.5 (?) because a getattr 
	// on the root node seems to occur with every single access.
	if( //(is_child || attempt_mount ) && 
	    strlen(root_name) > 0        &&
	    !is_mount(root_name)         &&
	    !do_mount(root_name))
		return PROC_PATH_FAILED;

	// construct path_out (mount_point_directory + '/' + path_in + '\0')
	path_out_base = path_out;
	for(i = 0; i < strlen(mount_point_directory); i++)
		*path_out++ = mount_point_directory[i];
	*path_out++ = '/';
	for(i = 0; i < strlen(path_in) - 1; i++)
		*path_out++ = path_in[i + 1];
	*path_out = '\0';
	fprintf(stderr, "Path out: %s\n", path_out_base);
	
	return strlen(root_name) ? PROC_PATH_PROXY_DIR : PROC_PATH_ROOT_DIR;
}

// Calling this function unmounts a dir if the user has specified to umount FS'
// not inuse and if there are no file or directory handles open. The
// update_operation flag combined with the mount_inuse_only_all user option
// allows unmounting only after destructive operations.
void consider_umount(const char *root_name, bool update_operation)
{
	fprintf(stderr, "Considering unmount of: %s\n", root_name);
	if((user_options.mount_inuse_only && update_operation) ||
	    user_options.mount_inuse_only_all) {
		mount_list_t *mount = find_mount(root_name);
		if(mount &&
		   dir_list_empty(mount->dir_list) && 
		   fd_list_empty(mount->fd_list)) {
			fprintf(stderr, "Decided to umount\n", root_name);
			do_umount(root_name);
		}
	} else {
		fprintf(stderr, "No user option\n", root_name);
	}
}

static int afuse_getattr(const char *path, struct stat *stbuf)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );

	fprintf(stderr, "> GetAttr\n");

	switch( process_path(path, real_path, root_name, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			extract_root_name(path, root_name);
			fprintf(stderr, "Getattr on: (%s) - %s\n", path, root_name);
			if( is_mount(root_name) || strlen(root_name) == 0) {
				stbuf->st_mode    = S_IFDIR | 0700;
				stbuf->st_nlink   = 1;
				stbuf->st_uid     = getuid();
				stbuf->st_gid     = getgid();
				stbuf->st_size    = 0;
				stbuf->st_blksize = 0;
				stbuf->st_blocks  = 0;
				stbuf->st_atime   = 0;
				stbuf->st_mtime   = 0;
				stbuf->st_ctime   = 0;
                                                       
				return 0;
			} else
				return -ENOENT;
			
		case PROC_PATH_PROXY_DIR:
			res = lstat(real_path, stbuf);
			consider_umount(root_name, false);
			if (res == -1)
				return -errno;

			return 0;
	}
}


static int afuse_readlink(const char *path, char *buf, size_t size)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );

	switch( process_path(path, real_path, root_name, 1) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOENT;
			
		case PROC_PATH_PROXY_DIR:
			res = readlink(real_path, buf, size - 1);
			consider_umount(root_name, false);
			if (res == -1)
				return -errno;

			buf[res] = '\0';
			return 0;
	}
}

static int afuse_opendir(const char *path, struct fuse_file_info *fi)
{
	DIR *dp;
	char *root_name = alloca( strlen(path) );
	mount_list_t *mount;
	char *real_path = alloca( max_path_out_len(path) );
       
	switch( process_path(path, real_path, root_name, 1) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return 0;
			
		case PROC_PATH_PROXY_DIR:
			dp = opendir(real_path);

			if (dp == NULL) {
				consider_umount(root_name, false);
				return -errno;
			}

			fi->fh = (unsigned long) dp;
			mount = find_mount(root_name);
			if(mount)
				dir_list_add(&mount->dir_list, dp);
			return 0;
	}
}

static inline DIR *get_dirp(struct fuse_file_info *fi)
{
	return (DIR *) (uintptr_t) fi->fh;
}

static int afuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi)
{
	DIR *dp = get_dirp(fi);
	struct dirent *de;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );
	mount_list_t *mount;
       
	switch( process_path(path, real_path, root_name, 1) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			filler(buf, ".", NULL, 0);
			filler(buf, "..", NULL, 0);
			for(mount = mount_list; mount; mount = mount->next)
				filler(buf, mount->root_name, NULL, 0);
			return 0;
			
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
			consider_umount(root_name, false);

			return 0;
	}
}

static int afuse_releasedir(const char *path, struct fuse_file_info *fi)
{
	DIR *dp = get_dirp(fi);
	mount_list_t *mount;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );
       
	switch( process_path(path, real_path, root_name, 1) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return 0;
			
		case PROC_PATH_PROXY_DIR:
			extract_root_name(path, root_name);
			mount = find_mount(root_name);
			if(mount)
				dir_list_remove(&mount->dir_list, dp);
			
			closedir(dp);
	
			consider_umount(root_name, false);
			
			return 0;
	}
}

static int afuse_mknod(const char *path, mode_t mode, dev_t rdev)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );

	fprintf(stderr, "> Mknod\n");

	switch( process_path(path, real_path, root_name, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			if (S_ISFIFO(mode))
				res = mkfifo(real_path, mode);
			else
				res = mknod(real_path, mode, rdev);
			consider_umount(root_name, true);
			if (res == -1)
				return -errno;

			return 0;
	}
}

static int afuse_mkdir(const char *path, mode_t mode)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );
	
	switch( process_path(path, real_path, root_name, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			res = mkdir(real_path, mode);
			consider_umount(root_name, true);
			if (res == -1)
				return -errno;

			return 0;
	}
}

static int afuse_unlink(const char *path)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );

	switch( process_path(path, real_path, root_name, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			res = unlink(real_path);
			consider_umount(root_name, true);
			if (res == -1)
				return -errno;

			return 0;
	}
}

static int afuse_rmdir(const char *path)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );
	
	switch( process_path(path, real_path, root_name, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			res = rmdir(real_path);
			consider_umount(root_name, true);
			if (res == -1)
				return -errno;

			return 0;
	}
}

static int afuse_symlink(const char *from, const char *to)
{
	int res;
	char *root_name_to = alloca( strlen(to) );
	char *real_to_path = alloca( max_path_out_len(to) );
	
	switch( process_path(to, real_to_path, root_name_to, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			res = symlink(from, real_to_path);
			consider_umount(root_name_to, true);
			if (res == -1)
				return -errno;

			return 0;
	}
}

static int afuse_rename(const char *from, const char *to)
{
	int res;
	char *root_name_from = alloca( strlen(from) );
	char *root_name_to = alloca( strlen(to) );
	char *real_from_path = alloca( max_path_out_len(from) );
	char *real_to_path = alloca( max_path_out_len(to) );
	
	switch( process_path(from, real_from_path, root_name_from, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			switch( process_path(to, real_to_path, root_name_to, 0) )
			{
				case PROC_PATH_FAILED:
					consider_umount(root_name_from, false);
					return -ENXIO;

				case PROC_PATH_ROOT_DIR:
					consider_umount(root_name_from, false);
					return -ENOTSUP;
					
				case PROC_PATH_PROXY_DIR:
					res = rename(real_from_path, real_to_path);
					consider_umount(root_name_from, true);
					consider_umount(root_name_to, true);
					if (res == -1)
						return -errno;

					return 0;
			}
	}
}

static int afuse_link(const char *from, const char *to)
{
	int res;
	char *root_name_from = alloca( strlen(from) );
	char *root_name_to = alloca( strlen(to) );
	char *real_from_path = alloca( max_path_out_len(from) );
	char *real_to_path = alloca( max_path_out_len(to) );
	
	switch( process_path(from, real_from_path, root_name_from, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			switch( process_path(to, real_to_path, root_name_to, 0) )
			{
				case PROC_PATH_FAILED:
					consider_umount(root_name_from, false);
					return -ENXIO;

				case PROC_PATH_ROOT_DIR:
					consider_umount(root_name_from, false);
					return -ENOTSUP;
					
				case PROC_PATH_PROXY_DIR:
					res = link(real_from_path, real_to_path);
					consider_umount(root_name_from, true);
					consider_umount(root_name_to, true);
					if (res == -1)
						return -errno;

					return 0;
			}
	}
}

static int afuse_chmod(const char *path, mode_t mode)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );
	
	switch( process_path(path, real_path, root_name, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			res = chmod(real_path, mode);
			consider_umount(root_name, true);
			if (res == -1)
				return -errno;

			return 0;
	}
}

static int afuse_chown(const char *path, uid_t uid, gid_t gid)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );
	
	switch( process_path(path, real_path, root_name, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			res = lchown(real_path, uid, gid);
			consider_umount(root_name, true);
			if (res == -1)
				return -errno;

			return 0;
	}
}

static int afuse_truncate(const char *path, off_t size)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );
	
	switch( process_path(path, real_path, root_name, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			res = truncate(real_path, size);
			consider_umount(root_name, true);
			if (res == -1)
				return -errno;

			return 0;
	}
}


static int afuse_utime(const char *path, struct utimbuf *buf)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );
	
	switch( process_path(path, real_path, root_name, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			res = utime(real_path, buf);
			consider_umount(root_name, true);
			if (res == -1)
				return -errno;

			return 0;
	}
}


static int afuse_open(const char *path, struct fuse_file_info *fi)
{
	int fd;
	char *root_name = alloca( strlen(path) );
	mount_list_t *mount;
	char *real_path = alloca( max_path_out_len(path) );

	switch( process_path(path, real_path, root_name, 1) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOENT;
			
		case PROC_PATH_PROXY_DIR:
			fd = open(real_path, fi->flags);
			if (fd == -1) {
				consider_umount(root_name, true);
				return -errno;
			}

			fi->fh = fd;
			extract_root_name(path, root_name);
			mount = find_mount(root_name);
			if(mount)
				fd_list_add(&mount->fd_list, fd);
			return 0;
	}
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

	(void) path;
	res = pwrite(fi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;

	if(user_options.flush_writes)
		fsync(fi->fh);

	return res;
}


static int afuse_release(const char *path, struct fuse_file_info *fi)
{
	char *root_name = alloca( strlen(path) );
	mount_list_t *mount;
	
	extract_root_name(path, root_name);
	mount = find_mount(root_name);
	if(mount)
		fd_list_remove(&mount->fd_list, fi->fh);
    
	close(fi->fh);
	consider_umount(root_name, true);


	return 0;
}

static int afuse_fsync(const char *path, int isdatasync,
                       struct fuse_file_info *fi)
{
	int res;
	(void) path;

	#ifndef HAVE_FDATASYNC
	(void) isdatasync;
	#else
	if (isdatasync)
		res = fdatasync(fi->fh);
	else
	#endif
		res = fsync(fi->fh);
	if (res == -1)
		return -errno;

	return 0;
}

#if FUSE_VERSION >= 25
static int afuse_access(const char *path, int mask)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );

	switch( process_path(path, real_path, root_name, 1) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
		case PROC_PATH_PROXY_DIR:
			res = access(real_path, mask);
			consider_umount(root_name, false);
			if (res == -1)
				return -errno;

			return 0;
	}
}

static int afuse_ftruncate(const char *path, off_t size,
                           struct fuse_file_info *fi)
{
	int res;

	(void) path;

	res = ftruncate(fi->fh, size);
	if (res == -1)
		return -errno;

	return 0;
}

static int afuse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int fd;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );

	switch( process_path(path, real_path, root_name, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			fd = open(real_path, fi->flags, mode);
			consider_umount(root_name, true);
			if (fd == -1)
				return -errno;

			fi->fh = fd;
			return 0;
	}
}

static int afuse_fgetattr(const char *path, struct stat *stbuf,
                          struct fuse_file_info *fi)
{
	int res;

	(void) path;

	res = fstat(fi->fh, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}
#endif


#if FUSE_VERSION >= 25
static int afuse_statfs(const char *path, struct statvfs *stbuf)
#else
static int afuse_statfs(const char *path, struct statfs *stbuf)
#endif
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );

	switch( process_path(path, real_path, root_name, 1) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
#if FUSE_VERSION >= 25
			stbuf->f_namemax = 0x7fffffff;
			stbuf->f_frsize  = 512;
#else
			stbuf->f_namelen = 0x7fffffff;
#endif
			stbuf->f_bsize   = 1024;
			stbuf->f_blocks  = 0;
			stbuf->f_bfree   = 0;
			stbuf->f_bavail  = 0;
			stbuf->f_files   = 0;
			stbuf->f_ffree   = 0;
			return 0;
		
		case PROC_PATH_PROXY_DIR:
			res = statvfs(real_path, stbuf);
			consider_umount(root_name, false);
			if (res == -1)
				return -errno;

			return 0;
	}
}

void afuse_destroy(void *p)
{
	shutdown();
}

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int afuse_setxattr(const char *path, const char *name, const char *value,
                        size_t size, int flags)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );
	
	switch( process_path(path, real_path, root_name, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOENT;
			
		case PROC_PATH_PROXY_DIR:
			res = lsetxattr(real_path, name, value, size, flags);
			consider_umount(root_name, true);
			if (res == -1)
				return -errno;
			return 0;
	}
}

static int afuse_getxattr(const char *path, const char *name, char *value,
                    size_t size)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );

	switch( process_path(path, real_path, root_name, 1) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			res = lgetxattr(real_path, name, value, size);
			consider_umount(root_name, false);
			if (res == -1)
				return -errno;
			return res;
	}
}

static int afuse_listxattr(const char *path, char *list, size_t size)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );

	switch( process_path(path, real_path, root_name, 1) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			res = llistxattr(real_path, list, size);
			consider_umount(root_name, false);
			if (res == -1)
				return -errno;
			return res;
	}
}

static int afuse_removexattr(const char *path, const char *name)
{
	int res;
	char *root_name = alloca( strlen(path) );
	char *real_path = alloca( max_path_out_len(path) );
	
	switch( process_path(path, real_path, root_name, 0) )
	{
		case PROC_PATH_FAILED:
			return -ENXIO;

		case PROC_PATH_ROOT_DIR:
			return -ENOTSUP;
			
		case PROC_PATH_PROXY_DIR:
			res = lremovexattr(real_path, name);
			consider_umount(root_name, true);
			if (res == -1)
				return -errno;
			return 0;
	}
}
#endif /* HAVE_SETXATTR */

static struct fuse_operations afuse_oper = {
	.getattr     = afuse_getattr,
	.readlink    = afuse_readlink,
	.opendir     = afuse_opendir,
	.readdir     = afuse_readdir,
	.releasedir  = afuse_releasedir,
	.mknod       = afuse_mknod,
	.mkdir       = afuse_mkdir,
	.symlink     = afuse_symlink,
	.unlink      = afuse_unlink,
	.rmdir       = afuse_rmdir,
	.rename      = afuse_rename,
	.link        = afuse_link,
	.chmod       = afuse_chmod,
	.chown       = afuse_chown,
	.truncate    = afuse_truncate,
	.utime       = afuse_utime,
	.open        = afuse_open,
	.read        = afuse_read,
	.write       = afuse_write,
	.release     = afuse_release,
	.fsync       = afuse_fsync,
	.statfs      = afuse_statfs,
#if FUSE_VERSION >= 25
	.access      = afuse_access,
	.create      = afuse_create,
	.ftruncate   = afuse_ftruncate,
	.fgetattr    = afuse_fgetattr,
#endif
	.destroy     = afuse_destroy,
#ifdef HAVE_SETXATTR
	.setxattr    = afuse_setxattr,
	.getxattr    = afuse_getxattr,
	.listxattr   = afuse_listxattr,
	.removexattr = afuse_removexattr,
#endif
};


enum {
	KEY_HELP,
	KEY_INUSEONLY,
	KEY_INUSEONLYALL,
	KEY_FLUSHWRITES
};

#define AFUSE_OPT(t, p, v) { t, offsetof(struct user_options_t, p), v }

static struct fuse_opt afuse_opts[] = {
	AFUSE_OPT("mount_template=%s", mount_command_template, 0),
	AFUSE_OPT("unmount_template=%s", unmount_command_template, 0),

	FUSE_OPT_KEY("inuseonly", KEY_INUSEONLY),
	FUSE_OPT_KEY("inuseonlyall", KEY_INUSEONLYALL),
	FUSE_OPT_KEY("flushwrites", KEY_FLUSHWRITES),

	FUSE_OPT_KEY("-h",     KEY_HELP),
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
"    -o mount_template=CMD    template for CMD to execute to mount (*)\n"
"    -o unmount_template=CMD  template for CMD to execute to unmount (*) (**)\n"
"    -o inuseonly             unmount fs after files are closed or dir updates\n"
"    -o inuseonlyall          as inuseonly, but all access end in unmount\n"
"    -o flushwrites           flushes data to disk for all file writes\n"
"\n\n"
" (*) - When executed, %%r and %%m are expanded in templates to the root\n"
"       directory name for the new mount point, and the actual directory to\n"
"       mount onto respectively to mount onto. Both templates are REQUIRED.\n"
"\n"
" (**) - The unmount command must perform a lazy unmount operation. E.g. the\n"
"        -u -z options to fusermount, or -l for regular mount.\n"       
"\n", progname);
}

static int afuse_opt_proc(void *data, const char *arg, int key,
                          struct fuse_args *outargs)
{
	(void) arg;

	switch(key)
	{
		case KEY_HELP:
			usage(outargs->argv[0]);
			fuse_opt_add_arg(outargs, "-ho");
			fuse_main(outargs->argc, outargs->argv, &afuse_oper);
			exit(1);

		case KEY_INUSEONLY:
			user_options.mount_inuse_only = true;
			return 0;

		case KEY_INUSEONLYALL:
			user_options.mount_inuse_only_all = true;
			return 0;

		case KEY_FLUSHWRITES:
			user_options.flush_writes = true;
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
	
	if(fuse_opt_parse(&args, &user_options, afuse_opts, afuse_opt_proc) == -1)
		return 1;
	
	// !!FIXME!! force single-threading for now as datastructures are not locked
	fuse_opt_add_arg(&args, "-s");

	// Check for required parameters
	if(!user_options.mount_command_template || !user_options.unmount_command_template) {
		fprintf(stderr, "(Un)Mount command templates missing.\n\n");
		usage(argv[0]);
		fuse_opt_add_arg(&args, "-ho");
		fuse_main(args.argc, args.argv, &afuse_oper);
		
		return 1;
	}

	if( !(mount_point_directory = mkdtemp(temp_dir_name)) ) {
		fprintf(stderr, "Failed to create temporary mount point dir.\n");
		return 1;
	}
	
	umask(0);
	
	// Register function to tidy up on exit conditions
	if( atexit( shutdown ) ) {
		fprintf(stderr, "Failed to register exit handler.\n");
		return 1;
	}

	// !!FIXME!! death by signal doesn't unmount fs
	return fuse_main(args.argc, args.argv, &afuse_oper);
}
