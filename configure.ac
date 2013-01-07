#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.59)
AC_INIT([afuse], [0.2], [jacob.bower@ic.ac.uk])

AM_INIT_AUTOMAKE
AC_PROG_RANLIB

AC_CONFIG_SRCDIR([src/afuse.c])
AC_CONFIG_HEADERS([config.h])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
PKG_CHECK_MODULES([FUSE], [fuse >= 2.3])
CFLAGS="$CFLAGS $FUSE_CFLAGS -DFUSE_USE_VERSION=25"
LIBS="$FUSE_LIBS"

# Check if we need to enable compatibility code for old FUSE versions
have_fuse_opt_parse=no
AC_CHECK_FUNC([fuse_opt_parse], [have_fuse_opt_parse=yes])
if test "$have_fuse_opt_parse" = no; then
        CFLAGS="$CFLAGS -I$PWD/compat"
fi
AM_CONDITIONAL(FUSE_OPT_COMPAT, test "$have_fuse_opt_parse" = no)


AC_CHECK_FUNCS([setxattr fdatasync])

AC_CONFIG_FILES([Makefile
                 src/Makefile
                 compat/Makefile])
AC_OUTPUT
