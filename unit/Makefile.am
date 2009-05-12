
noinst_PROGRAMS = test-util test-common test-sms

test_util_SOURCES = test-util.c ../src/util.c

test_util_LDADD = @GDBUS_LIBS@ @GLIB_LIBS@ @GTHREAD_LIBS@ -ldl

test_util_LDFLAGS = -Wl,--export-dynamic

test_common_SOURCES = test-common.c ../src/common.c

test_common_LDADD = @GDBUS_LIBS@ @GLIB_LIBS@ @GTHREAD_LIBS@ -ldl

test_common_LDFLAGS = -Wl,--export-dynamic

test_sms_SOURCES = test-sms.c ../src/util.c ../src/smsutil.c

test_sms_LDADD = @GDBUS_LIBS@ @GLIB_LIBS@ @GTHREAD_LIBS@ -ldl

test_sms_LDFLAGS = -Wl,--export-dynamic

AM_CFLAGS = @GTHREAD_CFLAGS@ @GLIB_CFLAGS@ @GDBUS_CFLAGS@ \
		-DOFONO_API_SUBJECT_TO_CHANGE

INCLUDES = -I$(top_builddir)/include -I$(top_builddir) -I$(top_builddir)/src

MAINTAINERCLEANFILES = Makefile.in
