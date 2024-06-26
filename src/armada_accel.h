/*
 * Marvell Armada DRM-based driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifndef ARMADA_ACCEL_H
#define ARMADA_ACCEL_H

#include "xf86.h"
#include "xf86xv.h"
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "common_drm.h"

struct drm_armada_bufmgr;

enum {
	/* xv_init capabilities */
	XVBO_CAP_KMS_DRM = 1,
	XVBO_CAP_GPU_DRM = 2,
};

struct armada_accel_ops {
	Bool (*pre_init)(ScrnInfoPtr, int);
	int (*screen_init)(ScreenPtr, struct drm_armada_bufmgr *);
	void (*align_bo_size)(ScreenPtr, int *, int *, int);
	Bool (*import_dmabuf)(ScreenPtr, PixmapPtr, int);
	void (*attach_name)(ScreenPtr, PixmapPtr, uint32_t);
	void (*free_pixmap)(PixmapPtr);
	void (*vblank_handler)(int fd, unsigned int sequence,
			       unsigned int tv_sec, unsigned int tv_usec,
			       void *user_data);
	XF86VideoAdaptorPtr (*xv_init)(ScreenPtr, unsigned int *);
	void (*flush_queue)(ScreenPtr);
	int (*export_name)(ScreenPtr, uint32_t);
};

Bool accel_module_init(const struct armada_accel_ops **);
void armada_register_accel(const struct armada_accel_ops *, pointer,
	const char *name);

#endif
