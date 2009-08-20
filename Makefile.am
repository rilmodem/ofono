
SUBDIRS = gdbus gatchat include plugins drivers unit src doc

if WANT_ISI
  SUBDIRS += gisi
endif

AM_MAKEFLAGS = --no-print-directory

DISTCHECK_CONFIGURE_FLAGS = --disable-datafiles

MAINTAINERCLEANFILES = Makefile.in \
	aclocal.m4 configure config.h.in config.sub config.guess \
	ltmain.sh depcomp missing install-sh mkinstalldirs
