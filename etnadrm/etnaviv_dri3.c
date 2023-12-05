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

static int etnaviv_dri3_fd_from_pixmap(ScreenPtr pScreen, PixmapPtr pixmap,
	CARD16 *stride, CARD32 *size)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vPix = etnaviv_get_pixmap_priv(pixmap);

	/* Only support pixmaps backed by an etnadrm bo */
	if (!vPix || !vPix->etna_bo)
		return BadMatch;

	*stride = pixmap->devKind;
	*size = etna_bo_size(vPix->etna_bo);

	return etna_bo_to_dmabuf(etnaviv->conn, vPix->etna_bo);
}

static dri3_screen_info_rec etnaviv_dri3_info = {
	.version = 0,
	.open = etnaviv_dri3_open,
	.pixmap_from_fd = etnaviv_dri3_pixmap_from_fd,
	.fd_from_pixmap = etnaviv_dri3_fd_from_pixmap,
};

Bool etnaviv_dri3_ScreenInit(ScreenPtr pScreen)
{
	if (!miSyncShmScreenInit(pScreen))
		return FALSE;

	return dri3_screen_init(pScreen, &etnaviv_dri3_info);
}
