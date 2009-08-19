AM_CFLAGS = @GLIB_CFLAGS@ -I$(top_srcdir)

noinst_LTLIBRARIES = libgisi.la
MAINTAINERCLEANFILES = Makefile.in

libgisi_la_SOURCES = \
	phonet.h isicommon.h \
	netlink.h netlink.c \
	socket.h socket.c \
	client.h client.c \
	pep.h pep.c \
	pipe.h pipe.c
libgisi_la_LIBADD = @GLIB_LIBS@
