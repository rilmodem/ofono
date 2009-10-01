
AM_MAKEFLAGS = --no-print-directory

includedir = @includedir@/ofono

include_HEADERS = include/log.h include/plugin.h include/history.h \
			include/dbus.h include/modem.h include/types.h \
			include/call-barring.h include/call-forwarding.h \
			include/call-meter.h include/call-settings.h \
			include/phonebook.h include/ssn.h include/ussd.h \
			include/sms.h include/sim.h include/message-waiting.h \
			include/netreg.h include/voicecall.h include/devinfo.h \
			include/cbs.h include/call-volume.h

nodist_include_HEADERS = include/version.h

local_headers = $(foreach file,$(include_HEADERS) $(nodist_include_HEADERS), \
					include/ofono/$(notdir $(file)))


if DATAFILES
dbusdir = @DBUS_DATADIR@

dbus_DATA = src/ofono.conf

confdir = $(sysconfdir)/ofono

conf_DATA =

statedir = $(localstatedir)/lib/ofono

state_DATA =
endif

builtin_modules =
builtin_sources =
builtin_libadd =
builtin_cflags =

gdbus_sources = gdbus/gdbus.h gdbus/mainloop.c gdbus/object.c gdbus/watch.c

gisi_sources = gisi/phonet.h gisi/modem.h gisi/netlink.h gisi/netlink.c \
		gisi/socket.h gisi/socket.c gisi/client.h gisi/client.c \
		gisi/pep.h gisi/pep.c gisi/pipe.h gisi/pipe.c gisi/iter.h \
		gisi/iter.c gisi/verify.c

gatchat_sources = gatchat/gatchat.h gatchat/gatchat.c \
				gatchat/gatresult.h gatchat/gatresult.c \
				gatchat/gatsyntax.h gatchat/gatsyntax.c \
				gatchat/ringbuffer.h gatchat/ringbuffer.c \
				gatchat/gatmux.h gatchat/gatmux.c \
				gatchat/gsm0710.h gatchat/gsm0710.c \
				gatchat/gattty.h gatchat/gattty.c

udev_files = plugins/ofono.rules

if UDEV
builtin_modules += udev
builtin_sources += plugins/udev.c
builtin_cflags += @UDEV_CFLAGS@
builtin_libadd += @UDEV_LIBS@

if DATAFILES
rulesdir = @UDEV_DATADIR@

rules_DATA = $(foreach file,$(udev_files), plugins/97-$(notdir $(file)))
endif
endif

if ISIMODEM
builtin_modules += isimodem
builtin_sources += $(gisi_sources) \
				drivers/isimodem/isi.h \
				drivers/isimodem/isimodem.c \
				drivers/isimodem/phonebook.c \
				drivers/isimodem/devinfo.c \
				drivers/isimodem/network-registration.c \
				drivers/isimodem/voicecall.c \
				drivers/isimodem/sms.c \
				drivers/isimodem/cbs.c \
				drivers/isimodem/sim.c \
				drivers/isimodem/ssn.c \
				drivers/isimodem/ussd.c \
				drivers/isimodem/call-forwarding.c \
				drivers/isimodem/call-settings.c \
				drivers/isimodem/call-barring.c \
				drivers/isimodem/call-meter.c
endif

if ATMODEM
builtin_modules += atmodem
builtin_sources += $(gatchat_sources) \
				drivers/atmodem/atmodem.h \
				drivers/atmodem/atmodem.c \
				drivers/atmodem/call-settings.c \
				drivers/atmodem/sms.c \
				drivers/atmodem/cbs.c \
				drivers/atmodem/call-forwarding.c \
				drivers/atmodem/call-meter.c \
				drivers/atmodem/network-registration.c \
				drivers/atmodem/sim.c \
				drivers/atmodem/ussd.c \
				drivers/atmodem/voicecall.c \
				drivers/atmodem/call-barring.c \
				drivers/atmodem/phonebook.c \
				drivers/atmodem/ssn.c \
				drivers/atmodem/devinfo.c \
				drivers/atmodem/call-volume.c \
				drivers/atmodem/vendor.h \
				drivers/atmodem/atutil.h \
				drivers/atmodem/atutil.c

builtin_modules += calypsomodem
builtin_sources += drivers/atmodem/atutil.h \
			drivers/calypsomodem/calypsomodem.h \
			drivers/calypsomodem/calypsomodem.c \
			drivers/calypsomodem/voicecall.c

builtin_modules += modemconf
builtin_sources += plugins/modemconf.c

if DATAFILES
conf_DATA += plugins/modem.conf
endif

builtin_modules += phonesim
builtin_sources += plugins/phonesim.c

builtin_modules += atgen
builtin_sources += plugins/atgen.c

builtin_modules += g1
builtin_sources += plugins/g1.c

builtin_modules += calypso
builtin_sources += plugins/calypso.c

builtin_modules += mbm
builtin_sources += plugins/mbm.c

builtin_modules += hso
builtin_sources += plugins/hso.c

builtin_modules += huawei
builtin_sources += plugins/huawei.c

builtin_modules += novatel
builtin_sources += plugins/novatel.c
endif

if MAINTAINER_MODE
builtin_modules += example_history
builtin_sources += plugins/example_history.c
endif

sbin_PROGRAMS = src/ofonod

src_ofonod_SOURCES = $(gdbus_sources) $(builtin_sources) \
			src/main.c src/ofono.h src/log.c src/plugin.c \
			src/modem.c src/common.h src/common.c \
			src/manager.c src/dbus.c src/util.h src/util.c \
			src/network.c src/voicecall.c src/ussd.c src/sms.c \
			src/call-settings.c src/call-forwarding.c \
			src/call-meter.c src/smsutil.h src/smsutil.c \
			src/ssn.c src/call-barring.c src/sim.c \
			src/phonebook.c src/history.c src/message-waiting.c \
			src/simutil.h src/simutil.c src/storage.h \
			src/storage.c src/cbs.c src/watch.c src/call-volume.c

src_ofonod_LDADD = $(builtin_libadd) \
			@GLIB_LIBS@ @GTHREAD_LIBS@ @DBUS_LIBS@ -ldl

src_ofonod_LDFLAGS = -Wl,--export-dynamic -Wl,--version-script=src/ofono.ver

src_ofonod_DEPENDENCIES = src/ofono.ver

CLEANFILES = src/ofono.ver src/ofono.exp src/builtin.h \
					$(local_headers) $(rules_DATA)

plugindir = $(libdir)/ofono/plugins

if MAINTAINER_MODE
build_plugindir = $(abs_top_srcdir)/plugins/.libs
else
build_plugindir = $(plugindir)
endif

AM_CFLAGS = @DBUS_CFLAGS@ @GLIB_CFLAGS@ @GTHREAD_CFLAGS@ \
					$(builtin_cflags) \
					-DOFONO_PLUGIN_BUILTIN \
					-DPLUGINDIR=\""$(build_plugindir)"\"

INCLUDES = -I$(builddir)/include -I$(builddir)/src -I$(srcdir)/src \
			-I$(srcdir)/gdbus -I$(srcdir)/gisi -I$(srcdir)/gatchat

doc_files = doc/overview.txt doc/ofono-paper.txt \
		doc/manager-api.txt doc/modem-api.txt doc/network-api.txt \
			doc/voicecallmanager-api.txt doc/voicecall-api.txt \
			doc/call-forwarding-api.txt doc/call-settings-api.txt \
			doc/call-meter-api.txt

test_files = test/test-manager test/test-modem test/test-voicecall \
		test/test-network-registration test/test-phonebook \
		test/test-advice-of-charge test/test-call-settings \
		test/test-call-forwarding test/test-call-barring \
		test/test-ss-control-cb test/test-ss-control-cf \
		test/test-ss-control-cs \
		test/monitor-ofono test/list-modems test/enable-modem \
		test/list-operators test/dial-number test/hangup-all \
		test/receive-sms test/send-sms

conf_files = src/ofono.conf plugins/modem.conf

EXTRA_DIST = src/genbuiltin plugins/example_history.c $(doc_files) \
				$(test_files) $(conf_files) $(udev_files)

dist_man_MANS = doc/ofonod.8


unit_objects =

noinst_PROGRAMS = unit/test-common unit/test-util \
			unit/test-sms unit/test-simutil unit/test-mux

unit_test_common_SOURCES = unit/test-common.c src/common.c
unit_test_common_LDADD = @GLIB_LIBS@
unit_objects += $(unit_test_common_OBJECTS)

unit_test_util_SOURCES = unit/test-util.c src/util.c
unit_test_util_LDADD = @GLIB_LIBS@
unit_objects += $(unit_test_utils_OBJECTS)

unit_test_sms_SOURCES = unit/test-sms.c src/util.c src/smsutil.c src/storage.c
unit_test_sms_LDADD = @GLIB_LIBS@
unit_objects += $(unit_test_sms_OBJECTS)

unit_test_simutil_SOURCES = unit/test-simutil.c src/util.c \
						src/simutil.c src/smsutil.c \
						src/storage.c
unit_test_simutil_LDADD = @GLIB_LIBS@
unit_objects += $(unit_test_simutil_OBJECTS)

unit_test_mux_SOURCES = unit/test-mux.c $(gatchat_sources)
unit_test_mux_LDADD = @GLIB_LIBS@
unit_objects += $(unit_test_mux_OBJECTS)


noinst_PROGRAMS += gatchat/gsmdial

gatchat_gsmdial_SOURCES = gatchat/gsmdial.c $(gatchat_sources)

gatchat_gsmdial_LDADD = @GLIB_LIBS@


DISTCHECK_CONFIGURE_FLAGS = --disable-datafiles

MAINTAINERCLEANFILES = Makefile.in \
	aclocal.m4 configure config.h.in config.sub config.guess \
	ltmain.sh depcomp compile missing install-sh mkinstalldirs


src/plugin.$(OBJEXT): src/builtin.h

src/builtin.h: src/genbuiltin $(builtin_sources)
	$(AM_V_GEN)$(srcdir)/src/genbuiltin $(builtin_modules) > $@

src/ofono.exp: $(src_ofonod_OBJECTS)
	$(AM_V_GEN)$(NM) $^ | $(AWK) '{ print $$3 }' | sort -u | \
				$(EGREP) -e '^ofono_' -e '^g_dbus_' > $@

src/ofono.ver: src/ofono.exp
	$(AM_V_at)echo "{ global:" > $@
	$(AM_V_GEN)$(SED) -e "s/\(.*\)/\1;/" $< >> $@
	$(AM_V_at)echo "local: *; };" >> $@

plugins/%.rules:
	$(AM_V_GEN)cp $(subst 97-,,$@) $@

$(src_ofonod_OBJECTS) $(unit_objects): $(local_headers)

include/ofono/version.h: include/version.h
	$(AM_V_at)$(MKDIR_P) include/ofono
	$(AM_V_GEN)$(LN_S) $(abs_top_builddir)/$< $@

include/ofono/%.h: include/%.h
	$(AM_V_at)$(MKDIR_P) include/ofono
	$(AM_V_GEN)$(LN_S) $(abs_top_srcdir)/$< $@

clean-local: include/ofono
	@$(RM) -r $<
