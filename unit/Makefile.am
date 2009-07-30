
noinst_PROGRAMS = test-common test-util test-sms test-simutil

test_common_SOURCES = test-common.c $(top_srcdir)/src/common.c

test_util_SOURCES = test-util.c $(top_srcdir)/src/util.c

test_sms_SOURCES = test-sms.c $(top_srcdir)/src/util.c \
					$(top_srcdir)/src/smsutil.c

test_simutil_SOURCES = test-simutil.c $(top_srcdir)/src/util.c \
					$(top_srcdir)/src/simutil.c \
					$(top_srcdir)/src/smsutil.c

LDADD = @GLIB_LIBS@ @GTHREAD_LIBS@

AM_CFLAGS = @GTHREAD_CFLAGS@ @GLIB_CFLAGS@

INCLUDES = -I$(top_builddir)/include -I$(top_srcdir)/src

MAINTAINERCLEANFILES = Makefile.in
