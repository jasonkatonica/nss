#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

DEPTH		= ../..
CORE_DEPTH	= ../..

MODULE		= coreconf

CSRCS		= nsinstall.c pathsub.c

PROGRAM		= nsinstall

# Indicate that this directory builds build tools.
INTERNAL_TOOLS  = 1


include $(DEPTH)/coreconf/config.mk

ifeq (,$(filter-out WIN%,$(OS_TARGET)))
PROGRAM		=
TARGETS		=
else
TARGETS		= $(PROGRAM)
INSTALL		= true
endif

ifdef NATIVE_CC
CC=$(NATIVE_CC)
endif

ifdef NATIVE_FLAGS
OS_CFLAGS=$(NATIVE_FLAGS)
endif

ifdef NATIVE_LDFLAGS
LDFLAGS=$(NATIVE_LDFLAGS)
endif

include $(DEPTH)/coreconf/rules.mk

# Redefine MAKE_OBJDIR for just this directory
define MAKE_OBJDIR
if test ! -d $(@D); then mkdir -p $(@D); fi
endef

