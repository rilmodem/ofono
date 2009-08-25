
builtin_modules =
builtin_sources =
builtin_cflags =

if MAINTAINER_MODE
builtin_modules += example_history
builtin_sources += example_history.c
endif

builtin_modules += generic_at
builtin_sources += generic_at.c

noinst_LTLIBRARIES = libbuiltin.la
 
libbuiltin_la_SOURCES = $(builtin_sources)
libbuiltin_la_LDFLAGS =
libbuiltin_la_CFLAGS = $(AM_CFLAGS) $(builtin_cflags) -DOFONO_PLUGIN_BUILTIN \
			-DOFONO_API_SUBJECT_TO_CHANGE

BUILT_SOURCES = builtin.h

nodist_libbuiltin_la_SOURCES = $(BUILT_SOURCES)

AM_CFLAGS = -fvisibility=hidden @GLIB_CFLAGS@ @GDBUS_CFLAGS@ @GATCHAT_CFLAGS@

INCLUDES = -I$(top_builddir)/include -I$(top_srcdir)/src

CLEANFILES = $(BUILT_SOURCES)

MAINTAINERCLEANFILES = Makefile.in

builtin.h: $(top_srcdir)/src/genbuiltin $(builtin_sources)
	$(AM_V_GEN)$(top_srcdir)/src/genbuiltin $(builtin_modules) | \
				$(SED) -e "s/builtin\[\]/plugins\[\]/" > $@
