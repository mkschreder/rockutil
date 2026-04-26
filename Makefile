# Makefile for rockutil — a libusb-1.0 Rockchip Rockusb command-line tool.
#
# Requires libusb-1.0 development headers on the build host:
#   Debian/Ubuntu:  sudo apt install libusb-1.0-0-dev
#   Fedora/RHEL:    sudo dnf install libusbx-devel
#   Arch:           sudo pacman -S libusb
#
# Build:   make
# Install: sudo make install PREFIX=/usr/local
# Clean:   make clean

TARGET  := rockutil

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

PKG_CONFIG ?= pkg-config

LIBUSB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS   := $(shell $(PKG_CONFIG) --libs   libusb-1.0 2>/dev/null)

ifeq ($(strip $(LIBUSB_LIBS)),)
LIBUSB_LIBS := -lusb-1.0
endif

CC       ?= cc
CSTD     ?= -std=c11
WARNINGS := -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
            -Wmissing-prototypes -Wformat=2 -Wno-unused-parameter
OPT      ?= -O2
CFLAGS   := $(CSTD) $(WARNINGS) $(OPT) -D_FILE_OFFSET_BITS=64 \
            $(LIBUSB_CFLAGS) $(CFLAGS)
LDFLAGS  := $(LDFLAGS)
LDLIBS   := $(LIBUSB_LIBS)

SRCS := rockutil.c rkusb.c rkcrc.c rkrc4.c rkimage.c rkparam.c rksparse.c
OBJS := $(SRCS:.c=.o)
DEPS := $(SRCS:.c=.d)

.PHONY: all clean install uninstall check

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

clean:
	$(RM) $(OBJS) $(DEPS) $(TARGET)

check: $(TARGET)
	./$(TARGET) -v

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/$(TARGET)

include tests/Makefile.inc
