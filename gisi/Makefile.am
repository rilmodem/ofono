AM_CFLAGS = @GLIB_CFLAGS@

noinst_LTLIBRARIES = libgisi.la
MAINTAINERCLEANFILES = Makefile.in

libgisi_la_SOURCES = \
	phonet.h \
	netlink.h netlink.c \
	socket.h socket.c \
	client.h client.c
libgisi_la_LIBADD = @GLIB_LIBS@
