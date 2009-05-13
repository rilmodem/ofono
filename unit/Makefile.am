
noinst_PROGRAMS = test-common test-util test-sms

test_common_SOURCES = test-common.c $(top_builddir)/src/common.c

test_util_SOURCES = test-util.c $(top_builddir)/src/util.c

test_sms_SOURCES = test-sms.c $(top_builddir)/src/util.c \
					$(top_builddir)/src/smsutil.c

LDADD = @GLIB_LIBS@ @GTHREAD_LIBS@

AM_CFLAGS = @GTHREAD_CFLAGS@ @GLIB_CFLAGS@

INCLUDES = -I$(top_builddir)/include -I$(top_srcdir)/src

MAINTAINERCLEANFILES = Makefile.in
