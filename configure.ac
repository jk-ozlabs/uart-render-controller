AC_PREREQ([2.69])
AC_INIT([obmc-uart-render-controller], 0.1, [jk@ozlabs.org])
AM_INIT_AUTOMAKE([subdir-objects -Wall -Werror foreign dist-xz])
AM_SILENT_RULES([yes])

AC_PROG_CC
AM_PROG_AR
AC_PROG_INSTALL
AC_PROG_MAKE_SET

AX_APPEND_COMPILE_FLAGS([-Wall -Werror], [CFLAGS])

PKG_CHECK_MODULES([LIBSYSTEMD], [libsystemd], [],
		[AC_MSG_ERROR(["libsystemd required and not found."])])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
