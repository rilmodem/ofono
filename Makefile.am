
AM_MAKEFLAGS = --no-print-directory

includedir = @includedir@/ofono

include_HEADERS = include/log.h include/plugin.h include/history.h \
			include/dbus.h include/modem.h include/types.h \
			include/call-barring.h include/call-forwarding.h \
			include/call-meter.h include/call-settings.h \
			include/phonebook.h include/ssn.h include/ussd.h \
			include/sms.h include/sim.h include/message-waiting.h \
			include/netreg.h include/voicecall.h include/devinfo.h \
			include/cbs.h include/call-volume.h \
			include/gprs.h include/gprs-context.h \
			include/radio-settings.h include/stk.h \
			include/nettime.h

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

gisi_sources = gisi/modem.h gisi/modem.c \
			gisi/netlink.h gisi/netlink.c \
			gisi/socket.h gisi/socket.c \
			gisi/client.h gisi/client.c \
			gisi/server.h gisi/server.c \
			gisi/pep.h gisi/pep.c \
			gisi/pipe.h gisi/pipe.c \
			gisi/iter.h gisi/iter.c \
			gisi/verify.c gisi/phonet.h

gatchat_sources = gatchat/gatchat.h gatchat/gatchat.c \
				gatchat/gatresult.h gatchat/gatresult.c \
				gatchat/gatsyntax.h gatchat/gatsyntax.c \
				gatchat/ringbuffer.h gatchat/ringbuffer.c \
				gatchat/gatio.h	gatchat/gatio.c \
				gatchat/crc-ccitt.h gatchat/crc-ccitt.c \
				gatchat/gatmux.h gatchat/gatmux.c \
				gatchat/gsm0710.h gatchat/gsm0710.c \
				gatchat/gattty.h gatchat/gattty.c \
				gatchat/gatutil.h gatchat/gatutil.c \
				gatchat/gat.h \
				gatchat/gatserver.h gatchat/gatserver.c \
				gatchat/gathdlc.c gatchat/gathdlc.h \
				gatchat/gatppp.c gatchat/gatppp.h \
				gatchat/ppp.h gatchat/ppp_cp.h \
				gatchat/ppp_cp.c gatchat/ppp_lcp.c \
				gatchat/ppp_auth.c gatchat/ppp_net.c \
				gatchat/ppp_ipcp.c

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

builtin_modules += caif
builtin_sources += plugins/caif.c

if ISIMODEM
builtin_modules += isimodem
builtin_sources += $(gisi_sources) \
				drivers/isimodem/isimodem.h \
				drivers/isimodem/isimodem.c \
				drivers/isimodem/mtc.h \
				drivers/isimodem/debug.h \
				drivers/isimodem/isiutil.h \
				drivers/isimodem/debug.c \
				drivers/isimodem/phonebook.c \
				drivers/isimodem/devinfo.c \
				drivers/isimodem/info.h \
				drivers/isimodem/network-registration.c \
				drivers/isimodem/network.h \
				drivers/isimodem/infoserver.h \
				drivers/isimodem/infoserver.c \
				drivers/isimodem/voicecall.c \
				drivers/isimodem/call.h \
				drivers/isimodem/sms.c \
				drivers/isimodem/sms.h \
				drivers/isimodem/cbs.c \
				drivers/isimodem/sim.c \
				drivers/isimodem/sim.h \
				drivers/isimodem/ssn.c \
				drivers/isimodem/ussd.c \
				drivers/isimodem/call-forwarding.c \
				drivers/isimodem/call-settings.c \
				drivers/isimodem/call-barring.c \
				drivers/isimodem/call-meter.c \
				drivers/isimodem/ss.h \
				drivers/isimodem/radio-settings.c \
				drivers/isimodem/gss.h \
				drivers/isimodem/gprs.c \
				drivers/isimodem/gprs-context.c \
				drivers/isimodem/gpds.h

builtin_modules += usbpnmodem
builtin_sources += plugins/usbpnmodem.c

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
				drivers/atmodem/stk.c \
				drivers/atmodem/sim-poll.c \
				drivers/atmodem/sim-poll.h \
				drivers/atmodem/ussd.c \
				drivers/atmodem/voicecall.c \
				drivers/atmodem/call-barring.c \
				drivers/atmodem/phonebook.c \
				drivers/atmodem/ssn.c \
				drivers/atmodem/devinfo.c \
				drivers/atmodem/call-volume.c \
				drivers/atmodem/vendor.h \
				drivers/atmodem/atutil.h \
				drivers/atmodem/atutil.c \
				drivers/atmodem/gprs.c \
				drivers/atmodem/gprs-context.c

builtin_modules += nwmodem
builtin_sources += drivers/atmodem/atutil.h \
			drivers/nwmodem/nwmodem.h \
			drivers/nwmodem/nwmodem.c \
			drivers/nwmodem/radio-settings.c

builtin_modules += calypsomodem
builtin_sources += drivers/atmodem/atutil.h \
			drivers/calypsomodem/calypsomodem.h \
			drivers/calypsomodem/calypsomodem.c \
			drivers/calypsomodem/voicecall.c

builtin_modules += hfpmodem
builtin_sources += drivers/atmodem/atutil.h \
			drivers/hfpmodem/hfpmodem.h \
			drivers/hfpmodem/hfpmodem.c \
			drivers/hfpmodem/voicecall.c \
			drivers/hfpmodem/network-registration.c \
			drivers/hfpmodem/call-volume.c

builtin_modules += mbmmodem
builtin_sources += drivers/atmodem/atutil.h \
			drivers/mbmmodem/mbmmodem.h \
			drivers/mbmmodem/mbmmodem.c \
			drivers/mbmmodem/gprs-context.c \
			drivers/mbmmodem/stk.c

builtin_modules += hsomodem
builtin_sources += drivers/atmodem/atutil.h \
			drivers/hsomodem/hsomodem.h \
			drivers/hsomodem/hsomodem.c \
			drivers/hsomodem/gprs-context.c \
			drivers/hsomodem/radio-settings.c

builtin_modules += stemodem
builtin_sources += drivers/atmodem/atutil.h \
			drivers/stemodem/stemodem.h \
			drivers/stemodem/stemodem.c \
			drivers/stemodem/voicecall.c \
			drivers/stemodem/gprs-context.c \
			drivers/stemodem/caif_socket.h \
			drivers/stemodem/if_caif.h

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

builtin_modules += wavecom
builtin_sources += plugins/wavecom.c

builtin_modules += calypso
builtin_sources += plugins/calypso.c

builtin_modules += mbm
builtin_sources += plugins/mbm.c

builtin_modules += hso
builtin_sources += plugins/hso.c

builtin_modules += huawei
builtin_sources += plugins/huawei.c

builtin_modules += em770
builtin_sources += plugins/em770.c

builtin_modules += novatel
builtin_sources += plugins/novatel.c

builtin_modules += hfp
builtin_sources += plugins/hfp.c

builtin_modules += palmpre
builtin_sources += plugins/palmpre.c

builtin_modules += ste
builtin_sources += plugins/ste.c

endif

if MAINTAINER_MODE
builtin_modules += example_history example_nettime
builtin_sources += plugins/example_history.c plugins/example_nettime.c
endif

sbin_PROGRAMS = src/ofonod

src_ofonod_SOURCES = $(gdbus_sources) $(builtin_sources) \
			src/main.c src/ofono.h src/log.c src/plugin.c \
			src/modem.c src/common.h src/common.c \
			src/manager.c src/dbus.c src/util.h src/util.c \
			src/network.c src/voicecall.c src/ussd.c src/sms.c \
			src/call-settings.c src/call-forwarding.c \
			src/call-meter.c src/smsutil.h src/smsutil.c \
			src/ssn.c src/call-barring.c src/sim.c src/stk.c \
			src/phonebook.c src/history.c src/message-waiting.c \
			src/simutil.h src/simutil.c src/storage.h \
			src/storage.c src/cbs.c src/watch.c src/call-volume.c \
			src/gprs.c src/idmap.h src/idmap.c \
			src/radio-settings.c src/stkutil.h src/stkutil.c \
			src/nettime.c

src_ofonod_LDADD = $(builtin_libadd) @GLIB_LIBS@ @DBUS_LIBS@ @CAPNG_LIBS@ -ldl

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

AM_CFLAGS = @DBUS_CFLAGS@ @GLIB_CFLAGS@ @CAPNG_CFLAGS@ $(builtin_cflags) \
					-DOFONO_PLUGIN_BUILTIN \
					-DPLUGINDIR=\""$(build_plugindir)"\"

INCLUDES = -I$(builddir)/include -I$(builddir)/src -I$(srcdir)/src \
			-I$(srcdir)/gdbus -I$(srcdir)/gisi -I$(srcdir)/gatchat

doc_files = doc/overview.txt doc/ofono-paper.txt \
		doc/manager-api.txt doc/modem-api.txt doc/network-api.txt \
			doc/voicecallmanager-api.txt doc/voicecall-api.txt \
			doc/call-forwarding-api.txt doc/call-settings-api.txt \
			doc/call-meter-api.txt \
			doc/dataconnectionmanager-api.txt

test_scripts = test/activate-context \
		test/create-context \
		test/deactivate-context \
		test/dial-number \
		test/disable-modem \
		test/enable-modem \
		test/enter-pin \
		test/hangup-all \
		test/list-contexts \
		test/list-modems \
		test/list-operators \
		test/monitor-ofono \
		test/process-context-settings \
		test/receive-sms \
		test/remove-contexts \
		test/send-sms \
		test/set-mic-volume \
		test/set-speaker-volume \
		test/test-advice-of-charge \
		test/test-call-barring \
		test/test-call-forwarding \
		test/test-call-settings \
		test/test-manager \
		test/test-modem \
		test/test-network-registration \
		test/test-phonebook \
		test/test-ss-control-cb \
		test/test-ss-control-cf \
		test/test-ss-control-cs \
		test/test-ussd \
		test/test-voicecall \
		test/offline-modem \
		test/online-modem \
		test/get-tech-preference \
		test/set-tech-preference

if TEST
testdir = $(pkglibdir)/test
test_SCRIPTS = $(test_scripts)
endif

conf_files = src/ofono.conf plugins/modem.conf

EXTRA_DIST = src/genbuiltin plugins/example_history.c $(doc_files) \
				$(test_scripts) $(conf_files) $(udev_files)

dist_man_MANS = doc/ofonod.8


unit_objects =

noinst_PROGRAMS = unit/test-common unit/test-util unit/test-idmap \
					unit/test-sms unit/test-simutil \
					unit/test-mux unit/test-caif \
					unit/test-stkutil

unit_test_common_SOURCES = unit/test-common.c src/common.c
unit_test_common_LDADD = @GLIB_LIBS@
unit_objects += $(unit_test_common_OBJECTS)

unit_test_util_SOURCES = unit/test-util.c src/util.c
unit_test_util_LDADD = @GLIB_LIBS@
unit_objects += $(unit_test_utils_OBJECTS)

unit_test_idmap_SOURCES = unit/test-idmap.c src/idmap.c
unit_test_idmap_LDADD = @GLIB_LIBS@
unit_objects += $(unit_test_idmap_OBJECTS)

unit_test_sms_SOURCES = unit/test-sms.c src/util.c src/smsutil.c src/storage.c
unit_test_sms_LDADD = @GLIB_LIBS@
unit_objects += $(unit_test_sms_OBJECTS)

unit_test_simutil_SOURCES = unit/test-simutil.c src/util.c \
				src/simutil.c src/smsutil.c src/storage.c
unit_test_simutil_LDADD = @GLIB_LIBS@
unit_objects += $(unit_test_simutil_OBJECTS)

unit_test_stkutil_SOURCES = unit/test-stkutil.c src/util.c \
				src/storage.c src/smsutil.c \
				src/simutil.c src/stkutil.c
unit_test_stkutil_LDADD = @GLIB_LIBS@
unit_objects += $(unit_test_stkutil_OBJECTS)

unit_test_mux_SOURCES = unit/test-mux.c $(gatchat_sources)
unit_test_mux_LDADD = @GLIB_LIBS@
unit_objects += $(unit_test_mux_OBJECTS)

unit_test_caif_SOURCES = unit/test-caif.c $(gatchat_sources) \
					drivers/stemodem/caif_socket.h \
					drivers/stemodem/if_caif.h 
unit_test_caif_LDADD = @GLIB_LIBS@
unit_objects += $(unit_test_caif_OBJECTS)

noinst_PROGRAMS += gatchat/gsmdial gatchat/test-server gatchat/test-qcdm

gatchat_gsmdial_SOURCES = gatchat/gsmdial.c $(gatchat_sources)
gatchat_gsmdial_LDADD = @GLIB_LIBS@

gatchat_test_server_SOURCES = gatchat/test-server.c $(gatchat_sources)
gatchat_test_server_LDADD = @GLIB_LIBS@ -lutil

gatchat_test_qcdm_SOURCES = gatchat/test-qcdm.c $(gatchat_sources)
gatchat_test_qcdm_LDADD = @GLIB_LIBS@


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
	$(AM_V_GEN)cp $(srcdir)/$(subst 97-,,$@) $@

$(src_ofonod_OBJECTS) $(unit_objects): $(local_headers)

include/ofono/version.h: include/version.h
	$(AM_V_at)$(MKDIR_P) include/ofono
	$(AM_V_GEN)$(LN_S) $(abs_top_builddir)/$< $@

include/ofono/%.h: include/%.h
	$(AM_V_at)$(MKDIR_P) include/ofono
	$(AM_V_GEN)$(LN_S) $(abs_top_srcdir)/$< $@

clean-local:
	@$(RM) -rf include/ofono
