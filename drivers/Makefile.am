
builtin_modules =
builtin_sources =
builtin_cflags =

builtin_modules += atmodem
builtin_sources += atmodem/atmodem.c atmodem/at.h \
			atmodem/session.h atmodem/session.c \
			atmodem/call-settings.c atmodem/sms.c \
			atmodem/call-forwarding.c atmodem/call-meter.c \
			atmodem/network-registration.c atmodem/sim.c \
			atmodem/ussd.c atmodem/voicecall.c \
			atmodem/call-barring.c atmodem/phonebook.c

builtin_modules += isimodem
builtin_sources += isimodem/isimodem.c isimodem/isi.h

noinst_LTLIBRARIES = libbuiltin.la

libbuiltin_la_SOURCES = $(builtin_sources)
libbuiltin_la_LDFLAGS =
libbuiltin_la_CFLAGS = $(AM_CFLAGS) $(builtin_cflags) \
			-DOFONO_PLUGIN_BUILTIN -DOFONO_API_SUBJECT_TO_CHANGE

BUILT_SOURCES = builtin.h

nodist_libbuiltin_la_SOURCES = $(BUILT_SOURCES)

AM_CFLAGS = -fvisibility=hidden \
		@GLIB_CFLAGS@ @GDBUS_CFLAGS@ @GATCHAT_CFLAGS@ @GISI_CFLAGS@

INCLUDES = -I$(top_builddir)/include -I$(top_srcdir)/src

CLEANFILES = $(BUILT_SOURCES)

MAINTAINERCLEANFILES = Makefile.in

builtin.h:
	echo "" > $@
	list='$(builtin_modules)'; for i in $$list; \
	  do echo "extern struct ofono_plugin_desc __ofono_builtin_$$i;" >> $@; done
	echo "" >> $@
	echo "static struct ofono_plugin_desc *__ofono_drivers[] = {" >> $@
	list='$(builtin_modules)'; for i in $$list; \
	  do echo "&__ofono_builtin_$$i," >> $@; done
	echo "NULL };" >> $@
