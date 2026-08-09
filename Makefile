# SPDX-License-Identifier: GPL-3.0-or-later
#
# NyxBG - Build system.
#
# Copyright (C) 2026 Fernando Magalhães
#
# Contact:
#   fm4lloc@gmail.com
#   nyx-eco@proton.me
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Quick reference:
#
#     make                    release build, warnings are errors
#     make BUILD=debug        -Og -g3, source-level debugging
#     make BUILD=asan         AddressSanitizer + UndefinedBehaviorSanitizer
#     make BUILD=lto          link time optimization
#     make WERROR=0           downgrade errors back to warnings
#     make analyze            run GCC's static analyzer
#     make info               report what this build will actually use
#     make vendor             copy the system protocol XML into protocol-xml/
#     make install PREFIX=/usr

###############################################################################
# Toolchain & Utilities
###############################################################################

CC ?= cc

TARGET  := nyxbg
VERSION := 1.0.0

# GNU make splits $(call ...) arguments on literal commas, so a flag that
# contains one has to reach the split as a variable reference.
comma := ,

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
MANPREFIX  ?= $(PREFIX)/share/man
DOCDIR     ?= $(PREFIX)/share/doc/$(TARGET)
LICENSEDIR ?= $(PREFIX)/share/licenses/$(TARGET)
DESTDIR    ?=

# Overridable so packaging systems can substitute their own tools.
RM      ?= rm -f
RMDIR   ?= rm -rf
MKDIR   ?= mkdir -p
INSTALL ?= install
CP      ?= cp

PKG_CONFIG ?= pkg-config

# Targets that only delete or describe must not require the toolchain.
NOBUILD_GOALS := clean distclean uninstall help

# Probing costs one compiler invocation per candidate flag, so goals that
# only delete files skip it entirely.
REQUIRE_PROBES := $(if $(filter-out $(NOBUILD_GOALS),$(or $(MAKECMDGOALS),all)),1,)

# "info" is what you run to find out why a build fails, so it must report
# rather than be the thing that fails first.  It still probes: reporting
# flags the build would not use would defeat the purpose.
REQUIRE_TOOLCHAIN := $(if $(filter-out $(NOBUILD_GOALS) info,$(or $(MAKECMDGOALS),all)),1,)

###############################################################################
# Dependencies
###############################################################################

# Plain names only.  A pkg-config version constraint contains ">=", which
# the shell would read as a redirection when this list is expanded, so
# version floors are checked separately below.
PKGS := wayland-client libpng libjpeg

ifdef REQUIRE_TOOLCHAIN

ifeq ($(shell command -v $(PKG_CONFIG) 2>/dev/null),)
$(error pkg-config not found)
endif

ifneq ($(shell $(PKG_CONFIG) --exists $(PKGS) && echo yes),yes)
$(error missing build dependency, one of: $(PKGS))
endif

endif

# wayland-scanner is reported by the wayland-scanner module when the
# distribution ships one; the bare name is the fallback.
WAYLAND_SCANNER ?= $(or \
    $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner 2>/dev/null), \
    wayland-scanner)

ifdef REQUIRE_TOOLCHAIN
ifeq ($(shell command -v $(WAYLAND_SCANNER) 2>/dev/null),)
$(error wayland-scanner not found: install wayland-devel / libwayland-bin)
endif
endif

# Third-party headers are pulled in with -isystem rather than -I, which is
# what makes -Werror survivable: this project's own code must satisfy the
# whole warning set below, while libpng, libjpeg and libwayland are only
# required to compile.  A new compiler release that finds something to say
# about a system header must not break this build.
#
# Guarded by REQUIRE_TOOLCHAIN for the same reason as the checks above: a
# goal that only deletes or describes must not invoke pkg-config, which on
# a host without the development packages prints its own errors to stderr
# and makes "make clean" look like it failed when it did not.
PKG_CFLAGS_RAW := $(if $(REQUIRE_TOOLCHAIN),$(shell $(PKG_CONFIG) --cflags $(PKGS)),)
PKG_CPPFLAGS   := $(patsubst -I%,-isystem %,$(PKG_CFLAGS_RAW))
PKG_LDLIBS     := $(if $(REQUIRE_TOOLCHAIN),$(shell $(PKG_CONFIG) --libs $(PKGS)),)

###############################################################################
# Protocol descriptions
#
# protocol/ contains nothing but generated code and is created by this
# build; it is not tracked.  The XML it is generated from is taken from the
# system when the distribution packages it, and from the vendored copies in
# protocol-xml/ otherwise.  Both sources exist because wlr-protocols is not
# packaged everywhere -- Arch and Alpine ship it, Debian does not -- while
# using the system copy is what a distribution packager expects.
#
# xdg-shell is needed for exactly one symbol.  zwlr_layer_surface_v1 has a
# get_popup request whose argument is an xdg_popup, so the generated
# interface table references xdg_popup_interface and will not link without
# it -- even though NyxBG never creates a popup.  Only its private code is
# generated; its client header is never included.
###############################################################################

PROTOCOL   := protocol
VENDOR_XML := protocol-xml

# Also guarded: "make clean" has no business asking where the protocol
# descriptions live.  "make info" does, which is why this uses the wider
# REQUIRE_PROBES rather than REQUIRE_TOOLCHAIN.
WLR_PROTOCOLS_DIR     := $(if $(REQUIRE_PROBES),$(shell $(PKG_CONFIG) --variable=pkgdatadir wlr-protocols 2>/dev/null),)
WAYLAND_PROTOCOLS_DIR := $(if $(REQUIRE_PROBES),$(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols 2>/dev/null),)

SYSTEM_LAYER_XML := $(WLR_PROTOCOLS_DIR)/unstable/wlr-layer-shell-unstable-v1.xml
SYSTEM_XDG_XML   := $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

# The system path wins when it exists; the vendored copy is the fallback.
PROTO_LAYER_XML := $(firstword $(wildcard $(SYSTEM_LAYER_XML)) \
                               $(wildcard $(VENDOR_XML)/wlr-layer-shell-unstable-v1.xml))
PROTO_XDG_XML   := $(firstword $(wildcard $(SYSTEM_XDG_XML)) \
                               $(wildcard $(VENDOR_XML)/xdg-shell.xml))

ifdef REQUIRE_TOOLCHAIN

ifeq ($(PROTO_LAYER_XML),)
$(error cannot find wlr-layer-shell-unstable-v1.xml: install wlr-protocols, or run "make vendor" on a machine that has it)
endif
ifeq ($(PROTO_XDG_XML),)
$(error cannot find xdg-shell.xml: install wayland-protocols, or run "make vendor" on a machine that has it)
endif

endif

PROTO_HDRS := $(PROTOCOL)/wlr-layer-shell-unstable-v1-client-protocol.h
PROTO_SRCS := $(PROTOCOL)/wlr-layer-shell-unstable-v1-protocol.c \
              $(PROTOCOL)/xdg-shell-protocol.c

###############################################################################
# Flag probing
#
# Everything in the mandatory sets below is required: the build fails
# without it, which is the point.  Everything probed depends on the
# compiler version or on the target architecture, and a diagnostic or a
# hardening flag that is merely unavailable must not stop an older or
# non-x86 host -- or a Clang build -- from compiling.
#
# Each candidate is compiled against an empty translation unit with -Werror
# and kept only if it survives.  The whole list is probed in a single
# shell, because one fork per flag is what makes this pattern slow.
###############################################################################

# The probe translation unit is a defined function with a prior prototype,
# not the empty file the usual cc-option idiom uses.  An empty file is not
# a valid translation unit, so -Wpedantic rejects it and every flag probed
# alongside it would be discarded as unsupported.
CC_PROBE_TU := 'int nyx_probe(void);\nint nyx_probe(void) { return 0; }\n'

cc-filter = $(strip $(shell for f in $(1); do \
	printf $(CC_PROBE_TU) \
	    | $(CC) -Werror $$f -c -x c - -o /dev/null >/dev/null 2>&1 \
	    && printf '%s ' "$$f"; \
	done))

# Link-time options need a probe that actually links: compiling an empty
# translation unit with -Wl,... makes the driver report the argument as
# unused, which -Werror would turn into a rejection of every single one.
ld-filter = $(strip $(shell for f in $(1); do \
	printf 'int main(void){return 0;}\n' \
	    | $(CC) -Werror $$f -x c - -o /dev/null >/dev/null 2>&1 \
	    && printf '%s ' "$$f"; \
	done))

###############################################################################
# Warnings
###############################################################################

# Understood by every compiler this project supports.
WARN_BASE := \
    -Wall -Wextra -Wpedantic \
    -Wshadow -Wwrite-strings -Wpointer-arith -Wundef \
    -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations \
    -Wold-style-definition -Wmissing-field-initializers \
    -Wcast-qual -Wconversion -Wsign-conversion \
    -Wformat=2 -Wuninitialized -Winit-self \
    -Wswitch-enum -Wswitch-default -Wfloat-equal -Wdouble-promotion \
    -Wbad-function-cast -Wnested-externs -Wredundant-decls \
    -Wvla -Wempty-body -Wignored-qualifiers -Wcast-function-type \
    -Wshift-overflow -Wshift-negative-value -Wunused-macros \
    -Wmissing-noreturn -Wdeclaration-after-statement -Woverlength-strings

# Compiler- or architecture-dependent.  Absent ones are silently dropped.
WARN_PROBED_CANDIDATES := \
    -Wcast-align=strict \
    -Warray-bounds=2 \
    -Wimplicit-fallthrough=5 \
    -Wstringop-truncation \
    -Wstringop-overflow=4 \
    -Wformat-overflow=2 \
    -Wformat-signedness \
    -Wduplicated-cond \
    -Wduplicated-branches \
    -Wlogical-op \
    -Wjump-misses-init \
    -Wnull-dereference \
    -Wtrampolines \
    -Walloca \
    -Walloc-zero \
    -Warith-conversion \
    -Wmultistatement-macros \
    -Wvla-parameter \
    -Wenum-conversion \
    -Waggregate-return \
    -Wnormalized=nfc \
    -Wbidi-chars=any \
    -Wstack-protector \
    -Wstack-usage=3072 \
    -Wstrict-overflow=2 \
    -Wdisabled-optimization \
    -Wuse-after-free=3 \
    -Wdangling-pointer=2 \
    -Wsuggest-attribute=noreturn \
    -Wsuggest-attribute=format \
    -Wsuggest-attribute=malloc

###############################################################################
# Hardening
###############################################################################

# Packaging environments frequently inject their own hardening flags, and
# redefining the macro on top of theirs is a warning at best.  The explicit
# -U keeps the definition single and predictable.
#
# Level 3 needs GCC 12 with glibc 2.33 or newer; where it is unsupported
# glibc emits a #warning, which -Werror would turn into a build failure, so
# the level is probed rather than assumed.
FORTIFY_LEVEL := $(if $(REQUIRE_PROBES),$(shell \
	printf '#include <string.h>\nint main(void){return 0;}\n' \
	    | $(CC) -O2 -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 \
	            -x c - -o /dev/null >/dev/null 2>&1 && echo 3 || echo 2),2)

FORTIFY ?= -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=$(FORTIFY_LEVEL)

HARDEN_BASE := \
    -fstack-protector-strong \
    -fno-common \
    -fno-strict-aliasing \
    -fno-delete-null-pointer-checks \
    -fPIE

HARDEN_PROBED_CANDIDATES := \
    -fstack-clash-protection \
    -fcf-protection=full \
    -ftrivial-auto-var-init=zero \
    -fzero-call-used-regs=used-gpr \
    -fstrict-flex-arrays=3 \
    -fno-plt \
    -ffunction-sections \
    -fdata-sections

# -fharden-compares and -fharden-conditional-branches are deliberately
# absent.  They duplicate every comparison and every branch condition to
# survive a fault injected into the running processor, which is a threat to
# a smartcard under a laser, not to a wallpaper renderer.
#
# -fstack-protector-all is absent for the same reason: -strong already
# guards every function holding an array or taking the address of a local,
# which is where an overflow can happen.

###############################################################################
# Project Toolchain Base
###############################################################################

# _DEFAULT_SOURCE sits on top of _POSIX_C_SOURCE so that syscall() is
# declared; render.c uses it to reach memfd_create() without depending on a
# glibc-specific wrapper.  Everything else this program calls is POSIX.
#
# include/ is added with -iquote, not -I, on purpose: include/signal.h
# would otherwise shadow the system <signal.h> for every translation unit.
# With -iquote the directory is searched only for the "..." form, so
# "signal.h" resolves to ours and <signal.h> resolves to libc's.
#
# protocol/ is added with -isystem so the machine-written scanner output
# does not have to satisfy the warning set above.
PROJECT_CPPFLAGS := \
    -DNYXBG_VERSION=\"$(VERSION)\" \
    -D_POSIX_C_SOURCE=200809L \
    -D_DEFAULT_SOURCE \
    -iquote include \
    -isystem $(PROTOCOL) \
    $(PKG_CPPFLAGS)

PROBED_CFLAGS := $(if $(REQUIRE_PROBES),$(call cc-filter, \
                     $(WARN_PROBED_CANDIDATES) $(HARDEN_PROBED_CANDIDATES)))

PROJECT_CFLAGS := -std=c11 $(WARN_BASE) $(HARDEN_BASE) $(PROBED_CFLAGS)

# --gc-sections is a link time argument and only collects when the compiler
# has been told to emit one section per function and per datum, which
# -ffunction-sections and -fdata-sections above do.
PROJECT_LDFLAGS := $(if $(REQUIRE_PROBES),$(call ld-filter, \
    -pie \
    -Wl$(comma)-z$(comma)relro \
    -Wl$(comma)-z$(comma)now \
    -Wl$(comma)-z$(comma)noexecstack \
    -Wl$(comma)-z$(comma)separate-code \
    -Wl$(comma)-z$(comma)nodlopen \
    -Wl$(comma)--no-undefined \
    -Wl$(comma)--as-needed \
    -Wl$(comma)--gc-sections))

LDLIBS += $(PKG_LDLIBS) -lm

DEPFLAGS := -MMD -MP

# A warning that reaches a release build is a warning nobody will read.
# WERROR=0 exists for the packager whose compiler is newer than this tree.
WERROR ?= 1
ifeq ($(WERROR),1)
PROJECT_CFLAGS += -Werror
endif

###############################################################################
# Build Profiles
###############################################################################

BUILD ?= release

ifeq ($(BUILD),debug)

    # -Og rather than -O0: it keeps a usable debugging experience while
    # still enabling the optimizations _FORTIFY_SOURCE depends on, so the
    # hardening policy stays identical across every profile.
    PROJECT_CFLAGS += -Og -g3 -fno-omit-frame-pointer

else ifeq ($(BUILD),asan)

    # The sanitizers must be enabled at compile and at link time.  Recovery
    # is off so the first diagnostic aborts the process.
    #
    # _FORTIFY_SOURCE is dropped here on purpose: its interposed string
    # routines hide the very overflows AddressSanitizer exists to report,
    # and GCC warns about the combination.
    FORTIFY := -U_FORTIFY_SOURCE
    PROJECT_CFLAGS  += -O1 -g3 -fno-omit-frame-pointer \
                       -fsanitize=address,undefined \
                       -fno-sanitize-recover=all
    PROJECT_LDFLAGS += -fsanitize=address,undefined

else ifeq ($(BUILD),lto)

    PROJECT_CFLAGS  += -O2 -flto
    PROJECT_LDFLAGS += -flto

else ifeq ($(BUILD),release)

    PROJECT_CFLAGS += -O2

else

$(error unknown BUILD "$(BUILD)": expected release, debug, asan or lto)

endif

###############################################################################
# Sources & Objects
###############################################################################

SRCS := src/main.c    \
        src/wayland.c \
        src/layer.c   \
        src/output.c  \
        src/image.c   \
        src/scale.c   \
        src/render.c  \
        src/signal.c  \
        src/util.c

OBJS      := $(SRCS:.c=.o)
PROTO_OBJS := $(PROTO_SRCS:.c=.o)
ALL_OBJS  := $(OBJS) $(PROTO_OBJS)
DEPS      := $(ALL_OBJS:.o=.d)

###############################################################################
# Build Rules
###############################################################################

.SUFFIXES:
.DELETE_ON_ERROR:

all: $(TARGET)

# Link stage.  Only linker flags and the compiler-driver flags that LTO and
# the sanitizers require are passed here.
$(TARGET): $(ALL_OBJS)
	$(CC) $(PROJECT_LDFLAGS) $(LDFLAGS) $(ALL_OBJS) $(LDLIBS) -o $@

# The generated headers are ordinary prerequisites, not order-only ones.
# They are pulled in with -isystem, and -MMD leaves system headers out of
# the dependency files, so nothing else would notice a protocol
# regenerating.  Listing them here means editing an XML rebuilds every
# object that could be affected.
$(ALL_OBJS): $(PROTO_HDRS)

# Compilation stage.  Project invariants first, user overrides last, so a
# distribution can add flags but the warning set is not negotiable.
src/%.o: src/%.c
	$(CC) $(PROJECT_CPPFLAGS) $(FORTIFY) $(CPPFLAGS) \
	      $(PROJECT_CFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# Generated protocol glue is machine-written and does not satisfy the
# project warning set, so it is compiled without it.
$(PROTOCOL)/%.o: $(PROTOCOL)/%.c
	$(CC) $(PROJECT_CPPFLAGS) $(FORTIFY) $(CPPFLAGS) \
	      -std=c11 -O2 -w $(CFLAGS) $(DEPFLAGS) -c $< -o $@

###############################################################################
# Wayland Scanner Rules
###############################################################################

$(PROTOCOL):
	$(MKDIR) $@

$(PROTOCOL)/wlr-layer-shell-unstable-v1-client-protocol.h: $(PROTO_LAYER_XML) | $(PROTOCOL)
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTOCOL)/wlr-layer-shell-unstable-v1-protocol.c: $(PROTO_LAYER_XML) | $(PROTOCOL)
	$(WAYLAND_SCANNER) private-code $< $@

$(PROTOCOL)/xdg-shell-protocol.c: $(PROTO_XDG_XML) | $(PROTOCOL)
	$(WAYLAND_SCANNER) private-code $< $@

###############################################################################
# Utility Targets
###############################################################################

# Reports what this build will actually use, which is the first thing to
# check when a build behaves differently on two machines.
info:
	@echo "target           : $(TARGET) $(VERSION)"
	@echo "profile          : $(BUILD)  (warnings are errors: $(WERROR))"
	@echo "compiler         : $(CC) -- $$($(CC) --version 2>/dev/null | head -1)"
	@echo "wayland-scanner  : $(WAYLAND_SCANNER)"
	@echo "layer-shell xml  : $(or $(PROTO_LAYER_XML),(NOT FOUND))"
	@echo "xdg-shell xml    : $(or $(PROTO_XDG_XML),(NOT FOUND))"
	@echo "fortify level    : $(FORTIFY_LEVEL)"
	@echo "link flags       : $(PROJECT_LDFLAGS)"
	@echo "probed flags     : $(PROBED_CFLAGS)"

help:
	@echo "make                  release build (default)"
	@echo "make BUILD=debug      -Og -g3"
	@echo "make BUILD=asan       AddressSanitizer + UndefinedBehaviorSanitizer"
	@echo "make BUILD=lto        link time optimization"
	@echo "make WERROR=0         downgrade errors back to warnings"
	@echo "make analyze          run GCC's static analyzer"
	@echo "make info             report what this build will use"
	@echo "make vendor           copy the system protocol XML into $(VENDOR_XML)/"
	@echo "make install PREFIX=/usr"
	@echo "make clean"

# Copies the system protocol descriptions into the vendored directory, so
# the tree can afterwards be built on a distribution that does not package
# wlr-protocols.
vendor: | $(VENDOR_XML)
	$(CP) $(PROTO_LAYER_XML) $(VENDOR_XML)/wlr-layer-shell-unstable-v1.xml
	$(CP) $(PROTO_XDG_XML) $(VENDOR_XML)/xdg-shell.xml
	@echo "vendored into $(VENDOR_XML)/"

$(VENDOR_XML):
	$(MKDIR) $@

# The static analyzer is a separate target rather than part of the build:
# it is slow, and its interprocedural reasoning about setjmp/longjmp is not
# reliable enough to gate a release on.
analyze: $(PROTO_HDRS)
	@if [ -z "$(call cc-filter,-fanalyzer)" ]; then \
		echo "$(CC) has no -fanalyzer; try: make analyze CC=gcc"; \
		exit 1; \
	fi
	@for f in $(SRCS); do \
		echo "  ANALYZE $$f"; \
		$(CC) $(PROJECT_CPPFLAGS) $(FORTIFY) $(CPPFLAGS) -std=c11 -O2 \
		      -fanalyzer -fsyntax-only $$f || exit 1; \
	done

run: $(TARGET)
	./$(TARGET) $(ARGS)

# Every path is quoted.  PREFIX and DESTDIR come from the environment or
# the command line, they reach the shell through these recipes, and a
# packager who has a space in a staging directory should get an install and
# not a surprise.  Quoting also means a value containing a semicolon is a
# bad path rather than a second command.
install: $(TARGET)
	$(MKDIR) "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	$(MKDIR) "$(DESTDIR)$(MANPREFIX)/man1"
	$(INSTALL) -m 644 $(TARGET).1 "$(DESTDIR)$(MANPREFIX)/man1/$(TARGET).1"
	$(MKDIR) "$(DESTDIR)$(LICENSEDIR)"
	$(INSTALL) -m 644 LICENSE "$(DESTDIR)$(LICENSEDIR)/LICENSE"
	$(MKDIR) "$(DESTDIR)$(DOCDIR)"
	$(INSTALL) -m 644 README.md "$(DESTDIR)$(DOCDIR)/README.md"

uninstall:
	$(RM) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	$(RM) "$(DESTDIR)$(MANPREFIX)/man1/$(TARGET).1"
	$(RM) "$(DESTDIR)$(LICENSEDIR)/LICENSE"
	$(RM) "$(DESTDIR)$(DOCDIR)/README.md"

clean:
	$(RMDIR) $(PROTOCOL)
	$(RM) $(OBJS) $(OBJS:.o=.d) $(TARGET) compile_commands.json

distclean: clean

###############################################################################
# Dependency Includes
###############################################################################

-include $(DEPS)

###############################################################################
# Phony Targets
###############################################################################

.PHONY: all analyze clean distclean help info install run uninstall vendor
