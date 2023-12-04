#ifndef COMMON_DRM_HELPER_H
#define COMMON_DRM_HELPER_H

#include "xf86.h"
#include "xf86Crtc.h"
#include <xf86drm.h>

xf86CrtcPtr common_drm_covering_crtc(ScrnInfoPtr pScrn, BoxPtr box,
	xf86CrtcPtr desired, BoxPtr box_ret);

xf86CrtcPtr common_drm_drawable_covering_crtc(DrawablePtr pDraw);

int common_drm_vblank_get(ScrnInfoPtr pScrn, xf86CrtcPtr crtc,
	drmVBlank *vbl, const char *func);

int common_drm_get_msc(xf86CrtcPtr crtc, uint64_t *ust, uint64_t *msc);
int common_drm_get_drawable_msc(xf86CrtcPtr crtc, DrawablePtr pDraw,
	uint64_t *ust, uint64_t *msc);

int common_drm_queue_msc_event(ScrnInfoPtr pScrn, xf86CrtcPtr crtc,
	uint64_t *msc, const char *func, Bool nextonmiss,
	struct common_drm_event *event);
int common_drm_queue_drawable_msc_event(ScrnInfoPtr pScrn, xf86CrtcPtr crtc,
	DrawablePtr pDraw, uint64_t *msc, const char *func, Bool nextonmiss,
	struct common_drm_event *event);

int common_drm_vblank_wait(ScrnInfoPtr pScrn, xf86CrtcPtr crtc,
	drmVBlank *vbl, const char *func, Bool nextonmiss);

#endif
