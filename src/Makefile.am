#
# Marvell Armada DRM-based driver
#
# Written by Russell King, 2012, derived in part from the
# Intel xorg X server driver.
#

# Turn off -Wnested-externs - these are a good thing because it allows
# information hiding and helps prevent misuse of private externs.
# Turn off -Wcast-qual - this makes stuff like string assignment
# too noisy.
# Turn off -Wredundant-decls - Xorg headers seem to contain a lot
# of this, so why it's in xorg-macros.m4... maybe more of a wish?
# Turn off -Wshadow - Xorg headers seem to declare a lot of globals
# which can conflict - index, range, etc.
AM_CFLAGS = $(filter-out -Wnested-externs -Wcast-qual -Wredundant-decls \
	-Werror=write-strings -Wshadow,$(CWARNFLAGS)) \
	$(XORG_CFLAGS) $(DRM_CFLAGS) $(UDEV_CFLAGS) \
	-iquote $(top_srcdir)/common

thead_drv_la_LTLIBRARIES = thead_drv.la
thead_drv_la_LDFLAGS = -module -avoid-version
thead_drv_la_LIBADD = $(DRM_LIBS) $(UDEV_LIBS) \
			$(top_builddir)/common/libcommon.la
thead_drv_ladir = @moduledir@/drivers
thead_drv_la_SOURCES = armada_accel.h \
			armada_bufmgr.c \
			armada_bufmgr.h \
			armada_module.c \
			armada_drm.c \
			armada_drm.h \
			armada_ioctl.h \
			common_drm.c \
			common_drm.h \
			common_drm_conn.c \
			common_drm_conn.h \
			common_drm_helper.h \
			common_drm_plane.c
if HAVE_DRI2
thead_drv_la_SOURCES += \
			common_drm_dri2.c \
			common_drm_dri2.h
endif

if HAVE_PRESENT
thead_drv_la_SOURCES += common_present.c
endif
