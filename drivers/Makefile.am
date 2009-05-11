
builtin_modules =
builtin_sources =
builtin_cflags =

builtin_modules += atmodem
builtin_sources += atmodem/main.c

noinst_LTLIBRARIES = libbuiltin.la
 
libbuiltin_la_SOURCES = $(builtin_sources)
libbuiltin_la_LDFLAGS =
libbuiltin_la_CFLAGS = $(AM_CFLAGS) $(builtin_cflags) -DOFONO_PLUGIN_BUILTIN

BUILT_SOURCES = builtin.h

nodist_libbuiltin_la_SOURCES = $(BUILT_SOURCES)

AM_CFLAGS = -fvisibility=hidden @GLIB_CFLAGS@ @GDBUS_CFLAGS@ @GATCHAT_CFLAGS@

INCLUDES = -I$(top_builddir)/include

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
