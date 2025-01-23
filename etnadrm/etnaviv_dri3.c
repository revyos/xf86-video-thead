/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2015
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* drm includes */
#include <xf86drm.h>
#include <drm_fourcc.h>

#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"
#include "dri3.h"
#include "misyncshm.h"
#include "compat-api.h"

#include "common_drm.h"

#include "etnaviv_accel.h"
#include "etnaviv_dri3.h"

#include <etnaviv/etna_bo.h>
#include "etnaviv_compat.h"

static int etnaviv_dri3_open(ScreenPtr pScreen, RRProviderPtr provider, int *o)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	char *name;
	int fd;

	name = drmGetDeviceNameFromFd(drm->fd);
	if (!name)
		return BadAlloc;

	fd = open(name, O_RDWR | O_CLOEXEC);
	drmFree(name);
	if (fd < 0)
		return BadAlloc;

	*o = fd;

	return Success;
}

static PixmapPtr etnaviv_dri3_pixmap_from_fd(ScreenPtr pScreen, int fd,
	CARD16 width, CARD16 height, CARD16 stride, CARD8 depth, CARD8 bpp)
{
	return etnaviv_pixmap_from_dmabuf(pScreen, fd, width, height,
					  stride, depth, bpp);
}

static PixmapPtr etnaviv_dri3_pixmap_from_fds(ScreenPtr pScreen, CARD8 num_fds,
	const int *fds, CARD16 width, CARD16 height, const CARD32 *strides,
	const CARD32 *offsets, CARD8 depth, CARD8 bpp, CARD64 modifier)
{
	if (num_fds != 1)
		return NULL;
	if (*offsets != 0)
		return NULL;
	switch (modifier) {
	case DRM_FORMAT_MOD_LINEAR:
	case DRM_FORMAT_MOD_INVALID:
		break;
	default:
		return NULL;
	}

	return etnaviv_dri3_pixmap_from_fd(pScreen, *fds, width, height, (CARD16) *strides, depth, bpp);
}

static int etnaviv_dri3_fd_from_pixmap(ScreenPtr pScreen, PixmapPtr pixmap,
	CARD16 *stride, CARD32 *size)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vPix = etnaviv_get_pixmap_priv(pixmap);

	/* Only support pixmaps backed by an etnadrm bo */
	if (!vPix || !vPix->etna_bo)
		return -1;

	*stride = pixmap->devKind;
	*size = etna_bo_size(vPix->etna_bo);

	return etna_bo_to_dmabuf(etnaviv->conn, vPix->etna_bo);
}

static int etnaviv_dri3_fds_from_pixmap(ScreenPtr pScreen, PixmapPtr pixmap,
	int *fds, CARD32 *strides, CARD32 *offsets, CARD64 *modifier)
{
	CARD32 size;
	CARD16 stride;
	int fd = etnaviv_dri3_fd_from_pixmap(pScreen, pixmap, &stride, &size);

	if (fd < 0)
		return 0;

	*fds = 1;
	*offsets = 0;
	*strides = stride;
	*modifier = DRM_FORMAT_MOD_INVALID;
	return 1;
}

static int etnaviv_dri3_get_formats(ScreenPtr screen, CARD32 *num_formats,
	CARD32 **formats)
{
	*num_formats = 0;
	return TRUE;
}

static int etnaviv_dri3_get_modifiers(ScreenPtr screen, uint32_t format,
	uint32_t *num_modifiers, uint64_t **modifiers)
{
	*num_modifiers = 0;
	return TRUE;
}

static int etnaviv_dri3_get_drawable_modifiers(DrawablePtr drawable,
	uint32_t format, uint32_t *num_modifiers, uint64_t **modifiers)
{
	*num_modifiers = 0;
	return TRUE;
}

static dri3_screen_info_rec etnaviv_dri3_info = {
	.version = 2,
	.open = etnaviv_dri3_open,
	.pixmap_from_fd = etnaviv_dri3_pixmap_from_fd,
	.fd_from_pixmap = etnaviv_dri3_fd_from_pixmap,
	.pixmap_from_fds = etnaviv_dri3_pixmap_from_fds,
	.fds_from_pixmap = etnaviv_dri3_fds_from_pixmap,
	.get_formats = etnaviv_dri3_get_formats,
	.get_modifiers = etnaviv_dri3_get_modifiers,
	.get_drawable_modifiers = etnaviv_dri3_get_drawable_modifiers,
};

Bool etnaviv_dri3_ScreenInit(ScreenPtr pScreen)
{
	if (!miSyncShmScreenInit(pScreen))
		return FALSE;

	return dri3_screen_init(pScreen, &etnaviv_dri3_info);
}
