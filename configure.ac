#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.63])
AC_INIT([cmf2imf], [1.0], [malvineous@shikadi.net])
AM_INIT_AUTOMAKE([cmf2imf], [1.0])
AC_CONFIG_SRCDIR([src/main.cpp])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_LANG(C++)

# Overarching version of all libraries (used for installation dir)
AC_SUBST(cmf2imf_release, cmf2imf-1.0)

# Checks for programs.
AC_PROG_CXX

# Checks for libraries.
AC_PROG_LIBTOOL

BOOST_REQUIRE([1.37])
BOOST_PROGRAM_OPTIONS

PKG_CHECK_MODULES([libgamecommon], [libgamecommon])

# Checks for header files.

# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.

AC_OUTPUT(Makefile src/Makefile)
