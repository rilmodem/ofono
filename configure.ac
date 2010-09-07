AC_PREREQ(2.60)
AC_INIT(ofono, 0.27)

AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_CONFIG_HEADER(config.h)

m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AM_MAINTAINER_MODE

AC_PREFIX_DEFAULT(/usr/local)

PKG_PROG_PKG_CONFIG

COMPILER_FLAGS

AC_LANG_C

AC_C_RESTRICT

AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_CC_PIE
AC_PROG_INSTALL
AC_PROG_SED
AC_PROG_AWK
AM_PROG_MKDIR_P

m4_define([_LT_AC_TAGCONFIG], [])
m4_ifdef([AC_LIBTOOL_TAGS], [AC_LIBTOOL_TAGS([])])

AC_DISABLE_STATIC
AC_PROG_LIBTOOL
AC_PROG_NM

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

AC_ARG_ENABLE(test, AC_HELP_STRING([--enable-test],
		[enable test/example scripts]), [enable_test=${enableval}])
AM_CONDITIONAL(TEST, test "${enable_test}" = "yes")

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

AC_CHECK_LIB(c, signalfd, dummy=yes,
			AC_MSG_ERROR(signalfd support is required))

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
AC_CHECK_LIB(dbus-1, dbus_connection_can_send_type, dummy=yes,
	AC_DEFINE(NEED_DBUS_CONNECTION_CAN_SEND_TYPE, 1,
		[Define to 1 if you need the dbus_connection_can_send_type() function.]
))
AC_SUBST(DBUS_CFLAGS)
AC_SUBST(DBUS_LIBS)

AC_ARG_WITH(dbusconf, AC_HELP_STRING([--with-dbusconf=PATH],
	[path to D-Bus config directory]), [path_dbusconf=${withval}],
		[path_dbusconf="`$PKG_CONFIG --variable=sysconfdir dbus-1`"])
if (test -z "${path_dbusconf}"); then
	DBUS_DATADIR="${sysconfdir}/dbus-1/system.d"
else
	DBUS_DATADIR="${path_dbusconf}/dbus-1/system.d"
fi
AC_SUBST(DBUS_DATADIR)

AC_ARG_ENABLE(udev, AC_HELP_STRING([--disable-udev],
			[don't use udev support even if available]),
						[enable_udev=${enableval}])
if (test "${enable_udev}" != "no"); then
	PKG_CHECK_MODULES(UDEV, libudev >= 143,
				[enable_udev="yes"], [enable_udev="no"])
fi
if (test "${enable_udev}" = "yes"); then
	UDEV_DATADIR="`$PKG_CONFIG --variable=udevdir udev`"
	if (test -z "${UDEV_DATADIR}"); then
		UDEV_DATADIR="${sysconfdir}/udev/rules.d"
	else
		UDEV_DATADIR="${UDEV_DATADIR}/rules.d"
	fi
	AC_SUBST(UDEV_DATADIR)
fi
AC_SUBST(UDEV_CFLAGS)
AC_SUBST(UDEV_LIBS)
AM_CONDITIONAL(UDEV, test "${enable_udev}" = "yes")

AC_ARG_ENABLE(capng, AC_HELP_STRING([--enable-capng],
		[enable capabilities support]), [enable_capng=${enableval}])
if (test "${enable_capng}" = "yes"); then
	PKG_CHECK_MODULES(CAPNG, libcap-ng, dummy=yes,
				AC_MSG_ERROR(Capabilities library is required))
	AC_SUBST(CAPNG_CFLAGS)
	AC_SUBST(CAPNG_LIBS)
	AC_DEFINE(HAVE_CAPNG, 1, [Define to 1 if you have capabilities library.])
fi

AC_ARG_ENABLE(isimodem, AC_HELP_STRING([--disable-isimodem],
				[disable PhoNet/ISI modem support]),
					[enable_isimodem=${enableval}])
AM_CONDITIONAL(ISIMODEM, test "${enable_isimodem}" != "no")

AC_ARG_ENABLE(atmodem, AC_HELP_STRING([--disable-atmodem],
				[disable ETSI AT modem support]),
					[enable_atmodem=${enableval}])
AM_CONDITIONAL(ATMODEM, test "${enable_atmodem}" != "no")

AC_ARG_ENABLE(datafiles, AC_HELP_STRING([--disable-datafiles],
			[don't install configuration and data files]),
					[enable_datafiles=${enableval}])

AM_CONDITIONAL(DATAFILES, test "${enable_datafiles}" != "no")

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

if (test "$sysconfdir" = '${prefix}/etc'); then
	configdir="${prefix}/etc/ofono"
else
	configdir="${sysconfdir}/ofono"
fi

AC_DEFINE_UNQUOTED(CONFIGDIR, "${configdir}",
			[Directory for the configuration files])

AC_OUTPUT(Makefile include/version.h)
