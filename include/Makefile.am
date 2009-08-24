
includedir = @includedir@/ofono

include_HEADERS = log.h plugin.h history.h dbus.h modem.h \
			types.h call-barring.h call-forwarding.h \
			call-meter.h call-settings.h phonebook.h \
			ssn.h ussd.h sms.h sim.h message-waiting.h \
			netreg.h voicecall.h devinfo.h

nodist_include_HEADERS = version.h

MAINTAINERCLEANFILES = Makefile.in

all-local:
	@test -d ofono || mkdir ofono
	@list='$(include_HEADERS)'; for i in $$list; do \
	    test -f ofono/$$i || \
	    $(LN_S) $(abs_top_srcdir)/include/$$i ofono/$$i; \
	done
	@list='$(nodist_include_HEADERS)'; for i in $$list; do \
	    test -f ofono/$$i || \
	    $(LN_S) $(abs_top_builddir)/include/$$i ofono/$$i; \
	done

clean-local:
	@rm -rf ofono
