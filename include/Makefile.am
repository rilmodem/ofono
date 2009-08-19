
includedir = @includedir@/ofono

include_HEADERS = log.h plugin.h history.h dbus.h modem.h \
			types.h call-barring.h call-forwarding.h \
			call-meter.h call-settings.h phonebook.h \
			ssn.h ussd.h sms.h sim.h message-waiting.h \
			netreg.h

nodist_include_HEADERS = version.h

MAINTAINERCLEANFILES = Makefile.in

all-local:
	@if [ ! -e ofono ]; then \
	  mkdir ofono; \
	  list='$(include_HEADERS)'; for i in $$list; \
	    do $(LN_S) $(abs_top_srcdir)/include/$$i ofono/$$i; done; \
	  list='$(nodist_include_HEADERS)'; for i in $$list; \
	    do $(LN_S) $(abs_top_builddir)/include/$$i ofono/$$i; done; \
	fi

clean-local:
	@rm -rf ofono
