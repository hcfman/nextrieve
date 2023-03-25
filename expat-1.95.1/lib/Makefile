# Generated automatically from Makefile.in by configure.
################################################################
# Process this file with top-level configure script to produce Makefile
#
# Copyright 2000 Clark Cooper
#
#  This file is part of EXPAT.
#
#  EXPAT is free software; you can redistribute it and/or modify it
#  under the terms of the License (based on the MIT/X license) contained
#  in the file COPYING that comes with this distribution.
#
# EXPAT IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
# CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
# TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN EXPAT.
#


SHELL = /bin/sh

srcdir = .
top_srcdir = ..
prefix = /usr/local
exec_prefix = ${prefix}

bindir = ${exec_prefix}/bin
sbindir = ${exec_prefix}/sbin
libexecdir = ${exec_prefix}/libexec
datadir = ${prefix}/share
sysconfdir = ${prefix}/etc
sharedstatedir = ${prefix}/com
localstatedir = ${prefix}/var
libdir = ${exec_prefix}/lib
infodir = ${prefix}/info
mandir = ${prefix}/man
includedir = ${prefix}/include
oldincludedir = /usr/include

subdir = lib

DESTDIR =

top_builddir = ..

INSTALL = /usr/bin/install -c
INSTALL_PROGRAM = ${INSTALL}
INSTALL_SCRIPT = ${INSTALL_PROGRAM}
INSTALL_DATA = ${INSTALL} -m 644

host_alias = i686-pc-linux
host_triplet = i686-pc-linux-gnu
AS = 
CC = gcc
DLLTOOL = 
LIBTOOL = $(SHELL) $(top_builddir)/libtool
LN_S = ln -s
OBJDUMP = 
PACKAGE = expat
RANLIB = ranlib
VERSION = 1.95.1

LIBRARY = libexpat.la
SOURCES = xmlparse.c xmltok.c xmlrole.c
OBJECTS = $(SOURCES:.c=.o)
LTOBJECTS = $(SOURCES:.c=.lo)

TEMPLATES = xmltok_impl.c xmltok_ns.c
APIHEADER = expat.h
HEADERS = ascii.h iasciitab.h utf8tab.h xmltok.h asciitab.h latin1tab.h \
          nametab.h xmldef.h xmlrole.h xmltok_impl.h

mkinstalldirs = $(SHELL) $(top_srcdir)/conftools/mkinstalldirs
CONFIG_HEADER = ../config.h
CONFIG_CLEAN_FILES = 

INCLUDES = -I$(srcdir) -I..
DEFS = -DHAVE_CONFIG_H -DPACKAGE='"$(PACKAGE)"' -DVERSION='"$(PACKAGE)_$(VERSION)"'
CPPFLAGS = 
LDFLAGS = 
LIBS = 
CFLAGS = -g -O2

LIBREVISION = 1
LIBCURRENT  = 0
LIBAGE      = 0

COMPILE = $(CC) $(DEFS) $(INCLUDES) $(CPPFLAGS) $(CFLAGS)
LTCOMPILE = $(LIBTOOL) --mode=compile $(CC) $(DEFS) $(INCLUDES) $(CPPFLAGS) $(CFLAGS)
CCLD = $(CC)
LINK = $(LIBTOOL) --mode=link  $(CCLD) -version-info $(LIBCURRENT):$(LIBREVISION):$(LIBAGE) $(CFLAGS) $(LDFLAGS) -o $@
DIST_COMMON =  Makefile.in


DISTFILES = $(DIST_COMMON) $(SOURCES) $(TEMPLATES) $(APIHEADER) $(HEADERS) 

TAR = gtar
GZIP_ENV = --best

all: $(LIBRARY)

.SUFFIXES: .c .lo .o
.PHONY: all clean distclean maintainer-clean

.c.o:
	@echo '$(COMPILE) -c $<'; \
	$(COMPILE) -Wp,-MD,.deps/$(*F).pp -c $<
	@-cp .deps/$(*F).pp .deps/$(*F).P; \
	tr ' ' '\012' < .deps/$(*F).pp \
	  | sed -e 's/^\\$$//' -e '/^$$/ d' -e '/:$$/ d' -e 's/$$/ :/' \
	    >> .deps/$(*F).P; \
	rm .deps/$(*F).pp

.c.lo:
	@echo '$(LTCOMPILE) -c $<'; \
	test -d .deps || mkdir .deps ; \
	$(LTCOMPILE) -Wp,-MD,.deps/$(*F).pp -c $<
	@-sed -e 's/^\([^:]*\)\.o[ 	]*:/\1.lo \1.o :/' \
	  < .deps/$(*F).pp > .deps/$(*F).P; \
	tr ' ' '\012' < .deps/$(*F).pp \
	  | sed -e 's/^\\$$//' -e '/^$$/ d' -e '/:$$/ d' -e 's/$$/ :/' \
	    >> .deps/$(*F).P; \
	rm -f .deps/$(*F).pp

Makefile: $(srcdir)/Makefile.in  $(top_builddir)/config.status
	cd $(top_builddir) \
	  && CONFIG_FILES=$(subdir)/$@ CONFIG_HEADERS= $(SHELL) ./config.status

$(top_builddir)/config.status: $(top_builddir)/configure
	cd $(top_builddir) && $(MAKE) config.status

$(top_builddir)/config.h: $(top_builddir)/config.hin
	cd $(top_builddir) && $(MAKE) config.h

clean:
	rm -f $(LIBRARY) *.o *.lo *~
	rm -rf .libs _libs .deps

distclean: clean
	rm -f Makefile

maintainer-clean: distclean

install: $(LIBRARY) $(APIHEADER)
	$(mkinstalldirs) $(DESTDIR)$(libdir) $(DESTDIR)$(includedir)
	$(LIBTOOL)  --mode=install $(INSTALL) $(LIBRARY) $(DESTDIR)$(libdir)/$(LIBRARY)
	$(INSTALL_DATA) $(APIHEADER) $(DESTDIR)$(includedir)

uninstall:
	$(LIBTOOL)  --mode=uninstall rm -f $(DESTDIR)$(libdir)/$(LIBRARY);
	rm -f $(DESTDIR)$(libdir)/$(APIHEADER)

$(LIBRARY): $(LTOBJECTS)
	$(LINK) -rpath $(libdir) $(LDFLAGS) $(LTOBJECTS)

xmlparse.o \
xmlparse.lo: xmlparse.c expat.h xmlrole.h xmltok.h $(top_builddir)/config.h

xmlrole.o \
xmlrole.lo: xmlrole.c ascii.h xmlrole.h $(top_builddir)/config.h

xmltok.o \
xmltok.lo: xmltok.c xmltok_impl.c xmltok_ns.c \
          ascii.h asciitab.h iasciitab.h latin1tab.h nametab.h utf8tab.h \
          xmltok.h xmltok_impl.h $(top_builddir)/config.h
