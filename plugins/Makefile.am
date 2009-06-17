
builtin_modules =
builtin_sources =
builtin_cflags =

if MAINTAINER_MODE
builtin_modules += example_history
builtin_sources += example_history.c
endif

noinst_LTLIBRARIES = libbuiltin.la
 
libbuiltin_la_SOURCES = $(builtin_sources)
libbuiltin_la_LDFLAGS =
libbuiltin_la_CFLAGS = $(AM_CFLAGS) $(builtin_cflags) -DOFONO_PLUGIN_BUILTIN \
			-DOFONO_API_SUBJECT_TO_CHANGE

BUILT_SOURCES = builtin.h

nodist_libbuiltin_la_SOURCES = $(BUILT_SOURCES)

AM_CFLAGS = -fvisibility=hidden @GLIB_CFLAGS@ @GDBUS_CFLAGS@

INCLUDES = -I$(top_builddir)/include -I$(top_srcdir)/src

CLEANFILES = $(BUILT_SOURCES)

MAINTAINERCLEANFILES = Makefile.in

builtin.h:
	echo "" > $@
	list='$(builtin_modules)'; for i in $$list; \
	  do echo "extern struct ofono_plugin_desc __ofono_builtin_$$i;" >> $@; done
	echo "" >> $@
	echo "static struct ofono_plugin_desc *__ofono_plugins[] = {" >> $@
	list='$(builtin_modules)'; for i in $$list; \
	  do echo "&__ofono_builtin_$$i," >> $@; done
	echo "NULL };" >> $@
