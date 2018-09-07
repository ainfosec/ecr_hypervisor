AS         = $(CROSS_COMPILE)gas
LD         = $(CROSS_COMPILE)gld
CC         = $(CROSS_COMPILE)gcc
CPP        = $(CROSS_COMPILE)gcc -E
CXX        = $(CROSS_COMPILE)g++
AR         = $(CROSS_COMPILE)gar
RANLIB     = $(CROSS_COMPILE)granlib
NM         = $(CROSS_COMPILE)gnm
STRIP      = $(CROSS_COMPILE)gstrip
OBJCOPY    = $(CROSS_COMPILE)gobjcopy
OBJDUMP    = $(CROSS_COMPILE)gobjdump
SIZEUTIL   = $(CROSS_COMPILE)gsize

SHELL      = bash

INSTALL      = ginstall
INSTALL_DIR  = $(INSTALL) -d -m0755 -p
INSTALL_DATA = $(INSTALL) -m0644 -p
INSTALL_PROG = $(INSTALL) -m0755 -p

BOOT_DIR ?= /boot
DEBUG_DIR ?= /usr/lib/debug

SunOS_LIBDIR = /usr/sfw/lib
SunOS_LIBDIR_x86_64 = /usr/sfw/lib/amd64

SOCKET_LIBS = -lsocket
PTHREAD_LIBS = -lpthread
UTIL_LIBS =

SONAME_LDFLAG = -h
SHLIB_LDFLAGS = -R $(SunOS_LIBDIR) -shared

CFLAGS += -Wa,--divide -D_POSIX_C_SOURCE=200112L -D__EXTENSIONS__

