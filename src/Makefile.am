
sbin_PROGRAMS = ofonod

ofonod_SOURCES = main.c ofono.h log.c plugin.c

ofonod_LDADD = @GDBUS_LIBS@ @GLIB_LIBS@ @GTHREAD_LIBS@ -ldl

ofonod_LDFLAGS = -Wl,--export-dynamic -Wl,--version-script=ofono.ver

ofonod_DEPENDENCIES = ofono.ver

CLEANFILES = ofono.ver ofono.exp

if MAINTAINER_MODE
plugindir = $(abs_top_srcdir)/plugins/.libs
else
plugindir = $(libdir)/ofono/plugins
endif

AM_CFLAGS = @GTHREAD_CFLAGS@ @GLIB_CFLAGS@ @GDBUS_CFLAGS@ \
					-DPLUGINDIR=\""$(plugindir)"\"

INCLUDES = -I$(top_builddir)/include

MAINTAINERCLEANFILES = Makefile.in

ofono.exp: $(ofonod_OBJECTS)
	nm -B *.o | awk '{ print $$3 }' | sort -u | grep -E -e '^ofono_' > $@

ofono.ver: ofono.exp
	echo "{ global:" > $@
	cat $< | sed -e "s/\(.*\)/\1;/" >> $@
	echo "local: *; };" >> $@
