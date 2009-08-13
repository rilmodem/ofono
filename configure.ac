AC_PREREQ(2.60)
AC_INIT()

AM_INIT_AUTOMAKE(ofono, 0.3)
AM_CONFIG_HEADER(config.h)

m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AM_MAINTAINER_MODE

AC_LANG_C

AC_PROG_CC
AC_PROG_CC_PIE
AC_PROG_INSTALL
AC_C_RESTRICT

m4_define([_LT_AC_TAGCONFIG], [])
m4_ifdef([AC_LIBTOOL_TAGS], [AC_LIBTOOL_TAGS([])])

AC_DISABLE_STATIC
AC_PROG_LIBTOOL

AC_ARG_ENABLE(optimization, AC_HELP_STRING([--disable-optimization],
			[disable code optimization through compiler]), [
	if (test "${enableval}" = "no"); then
		CFLAGS="$CFLAGS -O0"
	fi
])

AC_ARG_ENABLE(debug, AC_HELP_STRING([--enable-debug],
			[enable compiling with debugging information]), [
	if (test "${enableval}" = "yes" &&
				test "${ac_cv_prog_cc_g}" = "yes"); then
		CFLAGS="$CFLAGS -g"
	fi
])

AC_ARG_ENABLE(pie, AC_HELP_STRING([--enable-pie],
			[enable position independent executables flag]), [
	if (test "${enableval}" = "yes" &&
				test "${ac_cv_prog_cc_pie}" = "yes"); then
		CFLAGS="$CFLAGS -fPIE"
		LDFLAGS="$LDFLAGS -pie"
	fi
])

AC_ARG_ENABLE(threads, AC_HELP_STRING([--enable-threads],
		[enable threading support]), [enable_threads=${enableval}])

AC_CHECK_LIB(dl, dlopen, dummy=yes,
			AC_MSG_ERROR(dynamic linking loader is required))

PKG_CHECK_MODULES(GLIB, glib-2.0 >= 2.16, dummy=yes,
				AC_MSG_ERROR(GLib >= 2.16 is required))
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

if (test "${enable_threads}" = "yes"); then
	AC_DEFINE(NEED_THREADS, 1, [Define if threading support is required])
	PKG_CHECK_MODULES(GTHREAD, gthread-2.0 >= 2.16, dummy=yes,
				AC_MSG_ERROR(GThread >= 2.16 is required))
	GLIB_CFLAGS="$GLIB_CFLAGS $GTHREAD_CFLAGS"
	GLIB_LIBS="$GLIB_LIBS $GTHREAD_LIBS"
fi

PKG_CHECK_MODULES(DBUS, dbus-1 >= 1.0, dummy=yes,
				AC_MSG_ERROR(D-Bus >= 1.0 is required))
AC_CHECK_LIB(dbus-1, dbus_watch_get_unix_fd, dummy=yes,
	AC_DEFINE(NEED_DBUS_WATCH_GET_UNIX_FD, 1,
		[Define to 1 if you need the dbus_watch_get_unix_fd() function.]))
AC_SUBST(DBUS_CFLAGS)
AC_SUBST(DBUS_LIBS)
DBUS_DATADIR="`$PKG_CONFIG --variable=sysconfdir dbus-1`"
if (test -z "{DBUS_DATADIR}"); then
	DBUS_DATADIR="${sysconfdir}/dbus-1/system.d"
else
	DBUS_DATADIR="$DBUS_DATADIR/dbus-1/system.d"
fi
AC_SUBST(DBUS_DATADIR)

AC_SUBST([GDBUS_CFLAGS], ['$(DBUS_CFLAGS) -I$(top_srcdir)/gdbus'])
AC_SUBST([GDBUS_LIBS], ['$(top_builddir)/gdbus/libgdbus.la $(DBUS_LIBS)'])

AC_SUBST([GATCHAT_CFLAGS], ['-I$(top_srcdir)/gatchat'])
AC_SUBST([GATCHAT_LIBS], ['$(top_builddir)/gatchat/libgatchat.la'])

AC_SUBST([GISI_CFLAGS], ['-I$(top_srcdir)'])
AC_SUBST([GISI_LIBS], ['$(top_builddir)/gisi/libgisi.la'])

AC_ARG_ENABLE(datafiles, AC_HELP_STRING([--disable-datafiles],
			[don't install configuration and data files]),
					[enable_datafiles=${enableval}])

AM_CONDITIONAL(DATAFILES, test "${enable_datafiles}" != "no")

AC_PREFIX_DEFAULT(/usr/local)

if (test "${prefix}" = "NONE"); then
	dnl no prefix and no localstatedir, so default to /var
	if (test "$localstatedir" = '${prefix}/var'); then
		AC_SUBST([localstatedir], ['/var'])
	fi

	prefix="${ac_default_prefix}"
fi

if (test "$localstatedir" = '${prefix}/var'); then
	storagedir="${prefix}/var/lib/ofono"
else
	storagedir="${localstatedir}/lib/ofono"
fi

AC_DEFINE_UNQUOTED(STORAGEDIR, "${storagedir}",
			[Directory for the storage files])

COMPILER_FLAGS

AC_OUTPUT(Makefile gdbus/Makefile gatchat/Makefile gisi/Makefile
			include/Makefile include/version.h src/Makefile
			plugins/Makefile drivers/Makefile unit/Makefile
								doc/Makefile)
