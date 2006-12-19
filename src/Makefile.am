bin_PROGRAMS=afuse
afuse_SOURCES=afuse.c afuse.h fd_list.c fd_list.h dir_list.c dir_list.h utils.c utils.h

if FUSE_OPT_COMPAT
afuse_LDADD = ../compat/libcompat.a
endif
