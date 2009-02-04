#
# Copyright (C) 2006 Greg Kroah-Hartman <greg@kroah.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#
#

VERSION = 013

PROGRAM = bti

CORE_OBJS = \
	bti.o

GEN_HEADERS = \
	bti_version.h

MAN_PAGES = \
	bti.1

CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)gcc
AR = $(CROSS_COMPILE)ar

override CFLAGS	+= -g -Wall -pipe -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -O2

WARNINGS	= -Wstrict-prototypes -Wsign-compare -Wshadow \
		  -Wchar-subscripts -Wmissing-declarations -Wnested-externs \
		  -Wpointer-arith -Wcast-align -Wsign-compare -Wmissing-prototypes
CFLAGS		+= $(WARNINGS)
LDFLAGS		+= -Wl,-warn-common,--as-needed


ifeq ($(strip $(V)),)
	E = @echo
	Q = @
else
	E = @\#
	Q =
endif
export E Q


# We need -lcurl for the curl stuff
# We need -lsocket and -lnsl when on Solaris
# We need -lreadline for readline support
# We need -lssl and -lcrypto when using libcurl with SSL support
# We need -lpthread for the pthread example
#LIB_OBJS = -lcurl -lnsl -lssl -lcrypto
LIB_OBJS = -lcurl -lnsl -lreadline

all:	$(PROGRAM) $(MAN_PAGES)

# "Static Pattern Rule" to build all programs
bti: %: $(HEADERS) $(GEN_HEADERS) $(CORE_OBJS)
	$(E) "  LD      " $@
	$(Q) $(LD) $(LDFLAGS) $(CORE_OBJS) -o $@ $(LIB_OBJS)


# build the objects
%.o: %.c $(HEADERS) $(GEN_HEADERS)
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(CFLAGS) $< -o $@


bti_version.h: Makefile
	$(E) "  GENHDR  " $@
	$(Q) echo "/* Generated by make. */" > $@
	$(Q) echo \#define BTI_VERSION	\"$(VERSION)\" >> $@


# man pages
%.1: %.xml
	$(E) "  XMLTO   " $@
	$(Q) xmlto man $?
.PRECIOUS: %.3

clean:
	$(E) "  CLEAN   "
	$(Q) - find . -type f -name '*.orig' -print0 | xargs -0r rm -f
	$(Q) - find . -type f -name '*.rej' -print0 | xargs -0r rm -f
	$(Q) - find . -type f -name '*~' -print0 | xargs -0r rm -f
	$(Q) - find . -type f -name '*.[oas]' -print0 | xargs -0r rm -f
	$(Q) - find . -type f -name "*.gcno" -print0 | xargs -0r rm -f
	$(Q) - find . -type f -name "*.gcda" -print0 | xargs -0r rm -f
	$(Q) - find . -type f -name "*.gcov" -print0 | xargs -0r rm -f
	$(Q) - rm -f core $(PROGRAM) $(GEN_HEADERS)
.PHONY: clean

release:
	$(Q) - rm -f bti-$(VERSION).tar.gz
	head -1 ChangeLog | grep -q "to v$(VERSION)"
	head -1 RELEASE-NOTES | grep -q "bti $(VERSION)"
	git commit -a -m "release $(VERSION)"
	cat .git/refs/heads/master > .git/refs/tags/$(VERSION)
	@ echo
	git archive --format=tar --prefix=bti-$(VERSION)/ HEAD | gzip -9v > bti-$(VERSION).tar.gz
.PHONY: release

