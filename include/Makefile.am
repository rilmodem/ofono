
includedir = @includedir@/ofono

include_HEADERS = log.h

MAINTAINERCLEANFILES = Makefile.in

all-local:
	@if [ ! -e ofono ]; then \
	  mkdir ofono; \
	  list='$(include_HEADERS)'; for i in $$list; \
	    do $(LN_S) $(abs_top_srcdir)/include/$$i ofono/$$i; done; \
	fi

clean-local:
	@rm -rf ofono
