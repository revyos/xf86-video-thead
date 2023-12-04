#  Copyright 2005 Adam Jackson.
#
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  on the rights to use, copy, modify, merge, publish, distribute, sub
#  license, and/or sell copies of the Software, and to permit persons to whom
#  the Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice (including the next
#  paragraph) shall be included in all copies or substantial portions of the
#  Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
#  ADAM JACKSON BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
#  IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
#  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
# Process this file with autoconf to produce a configure script

AC_PREREQ(2.57)
AC_INIT([xf86-video-armada],
	[0.0.0],
	[],
	[xf86-video-armada])

DRIVER_NAME=armada

AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_AUX_DIR(build-aux)
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([foreign dist-bzip2])
AM_MAINTAINER_MODE

# Require X.Org macros 1.8 or later for MAN_SUBSTS set by XORG_MANPAGE_SECTIONS
m4_ifndef([XORG_MACROS_VERSION],
          [m4_fatal([must install xorg-macros 1.8 or later before running autoconf/autogen.
  Hint: either install from source, git://anongit.freedesktop.org/xorg/util/macros or,
  depending on you distribution, try package 'xutils-dev' or 'xorg-x11-util-macros'])])

XORG_MACROS_VERSION(1.8)
XORG_DEFAULT_OPTIONS

# Require X.Org server macros (i.e. XORG_DRIVER_CHECK_EXT) to check fori
# required modules 
m4_ifndef([XORG_DRIVER_CHECK_EXT],
          [m4_fatal([must install xorg-server macros before running autoconf/autogen.
  Hint: either install from source, git://anongit.freedesktop.org/xorg/xserver or,
  depending on your distribution, try package 'xserver-xorg-dev' or 'xorg-x11-server-devel'])])

# Make sure to not include fallback X.Org strndup if libc provides it
AC_CHECK_FUNCS([strndup], [HAVE_STRNDUP=yes], [HAVE_STRNDUP=no])
AM_CONDITIONAL(NEED_STRNDUP, [test x$HAVE_STRNDUP = xno])

# Checks for programs.
AC_DISABLE_STATIC
AC_PROG_LIBTOOL
AC_PROG_CC

PKG_CHECK_MODULES(UDEV, [libudev], [udev=yes], [udev=no])
if test x"$udev" = xyes; then
   AC_DEFINE(HAVE_UDEV,1,[Enable udev-based monitor hotplug detection])
fi


AH_TOP([#include "xorg-server.h"])

AC_ARG_WITH(xorg-module-dir,
            AC_HELP_STRING([--with-xorg-module-dir=DIR],
                           [Default xorg module directory [[default=$libdir/xorg/modules]]]),
            [moduledir="$withval"],
            [moduledir="$libdir/xorg/modules"])

# Etnaviv GPU acceleration
AC_ARG_ENABLE(etnadrm,
	      AC_HELP_STRING([--disable-etnadrm],
			     [Disable Etnaviv (DRM kernel driver) acceleration support [[default=enabled]]]),
	      [ACCEL_ETNADRM="$enableval"],
	      [ACCEL_ETNADRM=auto])

AS_IF([test x$ACCEL_ETNADRM != xno],
      [
   CFLAGS="$CFLAGS -I$srcdir/etna_viv/src/etnaviv"
])

AC_MSG_CHECKING([whether to build Etnaviv (DRM kernel driver) acceleration support])
AS_IF([test x$ACCEL_ETNADRM = xyes],
      [AC_DEFINE(HAVE_ACCEL_ETNADRM,1,[Enable Etnaviv (DRM kernel driver) acceleration support])
       DRIVERS="etnadrm_gpu $DRIVERS"])
AM_CONDITIONAL(HAVE_ACCEL_ETNADRM, test x$ACCEL_ETNADRM = xyes)
AC_MSG_RESULT([$ACCEL_ETNADRM])

AC_ARG_ENABLE(dri2, AC_HELP_STRING([--disable-dri2],
		[Disable DRI support [[default=auto]]]),
		[DRI2="$enableval"],
		[DRI2=auto])

AC_ARG_ENABLE(dri3, AC_HELP_STRING([--disable-dri3],
		[Disable DRI3 support [[default=auto]]]),
		[DRI3="$enableval"],
		[DRI3=auto])

AC_ARG_ENABLE(present, AC_HELP_STRING([--disable-present],
		[Disable PRESENT support [[default=auto]]]),
		[PRESENT="$enableval"],
		[PRESENT=auto])

# Checks for pkg-config packages
PKG_CHECK_MODULES(XORG, [xorg-server >= 1.9.99.1 xproto fontsproto pixman-1])
sdkdir=$(pkg-config --variable=sdkdir xorg-server)

# Checks for required extensions
XORG_DRIVER_CHECK_EXT(RANDR, randrproto)
XORG_DRIVER_CHECK_EXT(RENDER, renderproto)
XORG_DRIVER_CHECK_EXT(XV, videoproto)

# Obtain compilter/linker options for the driver dependencies
PKG_CHECK_MODULES(DRM, [libdrm >= 2.4.47])

PKG_CHECK_MODULES(DRI2, [dri2proto >= 2.6], , DRI2=no)

# Check those options requiring DRM support
if test x$DRI2 != xno; then
   save_CFLAGS=$CFLAGS
   CFLAGS="$XORG_CFLAGS $DRM_CFLAGS $DRI_CFLAGS $DRI2_CFLAGS"
   AC_CHECK_HEADERS([dri2.h], DRI2=yes, DRI2=no, [#include <dixstruct.h>])
   CFLAGS=$save_CFLAGS
fi
AC_MSG_CHECKING([whether to include DRI2 support])
AM_CONDITIONAL(HAVE_DRI2, test x$DRI2 = xyes)
AC_MSG_RESULT([$DRI2])
if test x$DRI2 = xyes; then
   AC_DEFINE(HAVE_DRI2,1,[Enable DRI2 driver support])
fi

PKG_CHECK_MODULES(DRI3, [dri3proto >= 1.0], , DRI3=no)
if test x$DRI3 != xno; then
   save_CFLAGS=$CFLAGS
   CFLAGS="$XORG_CFLAGS $DRM_CFLAGS $DRI_CFLAGS $DRI2_CFLAGS $DRI3_CFLAGS"
   AC_CHECK_HEADERS([dri3.h], DRI3=yes, DRI3=no, [#include <dixstruct.h>])
   CFLAGS=$save_CFLAGS
fi
AC_MSG_CHECKING([whether to include DRI3 support])
AM_CONDITIONAL(HAVE_DRI3, test x$DRI3 = xyes)
AC_MSG_RESULT([$DRI3])
if test x$DRI3 = xyes; then
   AC_DEFINE(HAVE_DRI3,1,[Enable DRI3 driver support])
fi

PKG_CHECK_MODULES(PRESENT, [presentproto >= 1.0], , PRESENT=no)
if test x$PRESENT != xno; then
   save_CFLAGS=$CFLAGS
   CFLAGS="$XORG_CFLAGS $PRESENT_CFLAGS"
   AC_CHECK_HEADERS([present.h], PRESENT=yes, PRESENT=no,
	[#include <xorg-server.h>]
	[#include <dixstruct.h>])
   CFLAGS=$save_CFLAGS
fi
AC_MSG_CHECKING([whether to include PRESENT support])
AM_CONDITIONAL(HAVE_PRESENT, test x$PRESENT = xyes)
AC_MSG_RESULT([$PRESENT])
if test x$PRESENT = xyes; then
   AC_DEFINE(HAVE_PRESENT,1,[Enable PRESENT driver support])
fi


# Checks for header files.
AC_HEADER_STDC

AC_SUBST([XORG_CFLAGS])
AC_SUBST([LIBGAL_CFLAGS])
AC_SUBST([LIBGAL_LIBS])
AC_SUBST([DRIVER_NAME])
AC_SUBST([ETNAVIV_CFLAGS])
AC_SUBST([ETNAVIV_LIBS])
AC_SUBST([moduledir])
AC_SUBST([DRIVERS])

AC_OUTPUT([
	Makefile
	common/Makefile
	etnaviv/Makefile
	man/Makefile
	src/Makefile
])
