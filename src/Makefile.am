
if DATAFILES
dbusdir = @DBUS_DATADIR@

dbus_DATA = ofono.conf
endif

sbin_PROGRAMS = ofonod

ofonod_SOURCES = main.c ofono.h log.c plugin.c \
		driver.h modem.c common.h common.c \
		manager.c dbus.c util.h util.c \
		network.c voicecall.c ussd.h ussd.c sms.c \
		call-settings.c call-forwarding.c call-meter.c \
		smsutil.h smsutil.c cssn.h cssn.c call-barring.c sim.h sim.c \
		phonebook.c history.c simutil.h simutil.c \
		message-waiting.c

ofonod_LDADD = $(top_builddir)/plugins/libbuiltin.la \
		$(top_builddir)/drivers/libbuiltin.la \
		@GATCHAT_LIBS@ @GDBUS_LIBS@ @GLIB_LIBS@ \
		@GISI_LIBS@ @GTHREAD_LIBS@ -ldl

ofonod_LDFLAGS = -Wl,--export-dynamic -Wl,--version-script=ofono.ver

ofonod_DEPENDENCIES = ofono.ver \
	@GATCHAT_LIBS@ @GISI_LIBS@ ../gdbus/libgdbus.la \
	../drivers/libbuiltin.la ../plugins/libbuiltin.la

CLEANFILES = ofono.ver ofono.exp

if MAINTAINER_MODE
plugindir = $(abs_top_srcdir)/plugins/.libs
else
plugindir = $(libdir)/ofono/plugins
endif

AM_CFLAGS = @GTHREAD_CFLAGS@ @GLIB_CFLAGS@ @GDBUS_CFLAGS@ @GATCHAT_CFLAGS@ \
					@GISI_CFLAGS@ -DPLUGINDIR=\""$(plugindir)"\"

INCLUDES = -I$(top_builddir)/include -I$(top_builddir)

EXTRA_DIST = ofono.conf

MAINTAINERCLEANFILES = Makefile.in

ofono.exp: $(ofonod_OBJECTS)
	nm -B *.o | awk '{ print $$3 }' | sort -u | grep -E -e '^ofono_' > $@

ofono.ver: ofono.exp
	echo "{ global:" > $@
	cat $< | sed -e "s/\(.*\)/\1;/" >> $@
	echo "local: *; };" >> $@
