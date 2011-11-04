dist_bin_SCRIPTS=sshnet
bin_PROGRAMS=afuse
afuse_SOURCES=afuse.c afuse.h fd_list.c fd_list.h dir_list.c dir_list.h utils.c utils.h variable_pairing_heap.h string_sorted_list.c string_sorted_list.h

if FUSE_OPT_COMPAT
afuse_LDADD = ../compat/libcompat.a
endif
