
builtin_modules =
builtin_sources =
builtin_cflags =

builtin_modules += atmodem
builtin_sources += atmodem/atmodem.c atmodem/at.h \
			atmodem/call-settings.c atmodem/sms.c \
			atmodem/call-forwarding.c atmodem/call-meter.c \
			atmodem/network-registration.c atmodem/sim.c \
			atmodem/ussd.c atmodem/voicecall.c \
			atmodem/call-barring.c atmodem/phonebook.c \
			atmodem/ssn.c atmodem/devinfo.c

if COND_ISI
builtin_modules += isimodem
builtin_sources += isimodem/isimodem.c isimodem/isi.h isimodem/isiphonebook.c \
			isimodem/isidevinfo.c isimodem/isinetwork.c \
			isimodem/isivoicecall.c isimodem/isisms.c \
			isimodem/isisim.c isimodem/isissn.c \
			isimodem/isiussd.c isimodem/isicall-forwarding.c \
			isimodem/isicall-settings.c isimodem/isicall-barring.c \
			isimodem/isicall-meter.c
endif

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

builtin.h: $(top_srcdir)/src/genbuiltin $(builtin_sources)
	$(AM_V_GEN)$(top_srcdir)/src/genbuiltin $(builtin_modules) | \
				$(SED) -e "s/builtin\[\]/drivers\[\]/" > $@
