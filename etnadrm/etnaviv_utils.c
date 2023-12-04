/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif
#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"

#include <armada_bufmgr.h>
#include "cpu_access.h"
#include "gal_extension.h"
#include "pamdump.h"
#include "pixmaputil.h"

#include "etnaviv_accel.h"
#include "etnaviv_utils.h"

#include <etnaviv/etna_bo.h>
#include <etnaviv/common.xml.h>
#include <etnaviv/state_2d.xml.h>
#include "etnaviv_compat.h"

static const char *etnaviv_errors[] = {
	"invalid argument",
	"invalid object",
	"out of memory",
	"memory locked",
	"memory unlocked",
	"heap corrupted",
	"generic IO",
	"invalid address",
	"context loss",
	"too complex",
	"buffer too small",
	"interface error",
	"not supported",
	"more data",
	"timeout",
	"out of resources",
	"invalid data",
	"invalid mipmap",
	"not found",
	"not aligned",
	"invalid request",
	"GPU unresponsive",
};

const char *etnaviv_strerror(int err)
{
	static char buf[80];
	const char *str = NULL;

	sprintf(buf, "code=%d:errno=%d", err, errno);
	str = buf;

	if (err < 0) {
		if (err >= VIV_STATUS_GPU_NOT_RESPONDING)
			str = etnaviv_errors[-1 - err];
	}
	return str;
}

void __etnaviv_error(struct etnaviv *etnaviv, const char *fn, const char *w, int err)
{
	xf86DrvMsg(etnaviv->scrnIndex, X_ERROR,
		   "[etnaviv] %s: %s failed: %s\n", fn, w,
		   etnaviv_strerror(err));
}


/*
 * Unmap a pixmap from the GPU.  Note that we must wait for any outstanding
 * GPU operations to complete before unmapping the pixmap from the GPU.
 */
static void etnaviv_unmap_gpu(struct etnaviv *etnaviv, struct etnaviv_pixmap *vPix)
{
#ifdef DEBUG_MAP
	dbg("Unmapping vPix %p bo %p\n", vPix, vPix->bo);
#endif
	etna_bo_del(etnaviv->conn, vPix->etna_bo, NULL);
	vPix->etna_bo = NULL;
	vPix->info = 0;
}

/*
 * Map a pixmap to the GPU, and mark the GPU as owning this BO.
 */
Bool etnaviv_map_gpu(struct etnaviv *etnaviv, struct etnaviv_pixmap *vPix,
	enum gpu_access access)
{
	unsigned state, mask;
	uint32_t handle;

#ifdef DEBUG_CHECK_DRAWABLE_USE
	if (vPix->in_use) {
		fprintf(stderr, "Trying to accelerate: %p %p %u\n",
				vPix,
				vPix->etna_bo ? (void *)vPix->etna_bo :
						(void *)vPix->bo,
				vPix->in_use);
		return FALSE;
	}
#endif

	if (access == GPU_ACCESS_RO) {
		state = ST_GPU_R;
		mask = ST_CPU_W | ST_GPU_R;
	} else {
		state = ST_GPU_R | ST_GPU_W;
		mask = ST_CPU_R | ST_CPU_W | ST_GPU_R | ST_GPU_W;
	}

	/* If the pixmap is already appropriately mapped, just return */
	if ((vPix->state & mask) == state)
		return TRUE;

	if (vPix->state & ST_DMABUF) {
		vPix->state = (vPix->state & ~mask) | state;
		return TRUE;
	}

	/*
	 * If there is an etna bo, and there's a CPU use against this
	 * pixmap, finish that first.
	 */
	if (vPix->state & ST_CPU_RW && vPix->etna_bo && !vPix->bo)
		etna_bo_cpu_fini(vPix->etna_bo);

	/*
	 * If we have a shmem bo from KMS, map it to an etna_bo.  This
	 * gives us etna_bo's for everything except the dumb KMS buffers.
	 */
	if (vPix->bo && !vPix->etna_bo) {
		struct drm_armada_bo *bo = vPix->bo;
		struct etna_bo *etna_bo;

		etna_bo = etna_bo_from_usermem(etnaviv->conn, bo->ptr,
					       bo->size);
		if (!etna_bo) {
			xf86DrvMsg(etnaviv->scrnIndex, X_ERROR,
				   "etnaviv: etna_bo_from_usermem(ptr=%p, size=%zu) failed\n", bo->ptr, bo->size);
			return FALSE;
		}

		vPix->etna_bo = etna_bo;
	}

	vPix->state = (vPix->state & ~ST_CPU_RW) | state;

#ifdef DEBUG_MAP
	dbg("Mapped vPix %p etna bo %p to 0x%08x\n",
	    vPix, vPix->etna_bo, etna_bo_gpu_address(vPix->etna_bo));
#endif

	/*
	 * This should never happen - if it does, and we proceeed, we will
	 * take the machine out, so assert and kill ourselves instead.
	 */
	handle = etna_bo_gpu_address(vPix->etna_bo);
	assert(handle != 0 && handle != -1);

	return TRUE;
}

/*
 * Finish a bo for CPU access.  NULL out the fb layer's pixmap data
 * pointer to ensure any further unprotected accesses get caught.
 */
void finish_cpu_drawable(DrawablePtr pDrawable, int access)
{
	PixmapPtr pixmap = drawable_pixmap(pDrawable);
	struct etnaviv_pixmap *vPix = etnaviv_get_pixmap_priv(pixmap);

	if (vPix) {
#ifdef DEBUG_CHECK_DRAWABLE_USE
		vPix->in_use--;
#endif
		if (!(vPix->state & ST_DMABUF))
			pixmap->devPrivate.ptr = NULL;
	}
}

/*
 * Prepare a bo for CPU access.  If the GPU has been accessing the
 * pixmap data, we need to unmap the buffer from the GPU to ensure
 * that our view is up to date.
 */
void prepare_cpu_drawable(DrawablePtr pDrawable, int access)
{
	PixmapPtr pixmap = drawable_pixmap(pDrawable);
	struct etnaviv_pixmap *vPix = etnaviv_get_pixmap_priv(pixmap);

	if (vPix) {
		struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDrawable->pScreen);

		/*
		 * If the CPU is going to write to the pixmap, then we must
		 * ensure that the GPU is not using it.  Otherwise, tolerate
		 * both the GPU and CPU reading the pixmap.
		 */
		if (vPix->state &
		    (access == CPU_ACCESS_RW ? ST_GPU_RW : ST_GPU_W)) {
			etnaviv_batch_wait_commit(etnaviv, vPix);

			/* The GPU is no longer using this pixmap. */
			vPix->state &= ~ST_GPU_RW;

			/* Unmap this bo from the GPU */
			if (vPix->bo && vPix->etna_bo)
				etnaviv_unmap_gpu(etnaviv, vPix);
		}

		if (!(vPix->state & ST_DMABUF)) {
			if (vPix->bo) {
				pixmap->devPrivate.ptr = vPix->bo->ptr;
#ifdef DEBUG_MAP
				dbg("Pixmap %p bo %p to %p\n", pixmap, vPix->bo,
				    pixmap->devPrivate.ptr);
#endif
			} else if (vPix->etna_bo) {
				struct etna_bo *etna_bo = vPix->etna_bo;

				if (!(vPix->state & ST_CPU_RW))
					etna_bo_cpu_prep(etna_bo, NULL, DRM_ETNA_PREP_WRITE);

				pixmap->devPrivate.ptr = etna_bo_map(etna_bo);
#ifdef DEBUG_MAP
				dbg("Pixmap %p etnabo %p to %p\n", pixmap,
				    etna_bo, pixmap->devPrivate.ptr);
#endif
			}
		}
#ifdef DEBUG_CHECK_DRAWABLE_USE
		vPix->in_use++;
#endif
		vPix->state |= access == CPU_ACCESS_RW ? ST_CPU_RW : ST_CPU_R;
	}
}

Bool etnaviv_src_format_valid(struct etnaviv *etnaviv,
	struct etnaviv_format fmt)
{
	if (fmt.format == DE_FORMAT_YV12 &&
	    !VIV_FEATURE(etnaviv->conn, chipFeatures, YUV420_SCALER))
		return FALSE;
	if ((fmt.format >= 16 || fmt.swizzle) &&
	    !VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20))
		return FALSE;
	return fmt.format != UNKNOWN_FORMAT;
}

Bool etnaviv_dst_format_valid(struct etnaviv *etnaviv,
	struct etnaviv_format fmt)
{
	/* Don't permit BGRA or RGBA formats on PE1.0 */
	if (fmt.swizzle &&
	    !VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20))
		return FALSE;
	if (fmt.format == DE_FORMAT_A8 &&
	    !VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2D_A8_TARGET))
		return FALSE;
	return fmt.format != UNKNOWN_FORMAT;
}

#if 1 //def DEBUG
static void dump_pix(struct etnaviv *etnaviv, struct etnaviv_pixmap *vPix,
	bool alpha, int x1, int y1, int x2, int y2,
	const char *fmt, va_list ap)
	__attribute__((__format__(__printf__, 8, 0)));
static void dump_pix(struct etnaviv *etnaviv, struct etnaviv_pixmap *vPix,
	bool alpha, int x1, int y1, int x2, int y2,
	const char *fmt, va_list ap)
{
	static int idx;
	unsigned state = vPix->state;
	const uint32_t *ptr;
	char n[80];

	if (state & ST_DMABUF) {
		/* Can't dump ST_DMABUF pixmaps */
		return;
	} else if (vPix->bo) {
		ptr = vPix->bo->ptr;
	} else {
		ptr = etna_bo_map(vPix->etna_bo);
		state = ST_CPU_RW;
	}

	if (state & ST_GPU_W)
		etnaviv_unmap_gpu(etnaviv, vPix);

	vsprintf(n, fmt, ap);

	dump_pam(ptr, vPix->pitch, alpha, x1, y1, x2, y2,
		 "/tmp/X.%04u.%s-%u.%u.%u.%u.pam",
		 idx++, n, x1, y1, x2, y2);

	if (state & ST_GPU_W) {
		vPix->state &= ~ST_GPU_RW;
		etnaviv_map_gpu(etnaviv, vPix, GPU_ACCESS_RW);
	}
}

void dump_Drawable(DrawablePtr pDraw, const char *fmt, ...)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDraw->pScreen);
	xPoint offset;
	struct etnaviv_pixmap *vPix = etnaviv_drawable_offset(pDraw, &offset);
	va_list ap;

	if (!vPix)
		return;

	va_start(ap, fmt);
	dump_pix(etnaviv, vPix, 0,
		 pDraw->x + offset.x, pDraw->y + offset.y,
		 pDraw->width, pDraw->height, fmt, ap);
	va_end(ap);
}

void dump_Picture(PicturePtr pDst, const char *fmt, ...)
{
	DrawablePtr pDraw = pDst->pDrawable;
	struct etnaviv *etnaviv;
	struct etnaviv_pixmap *vPix;
	xPoint offset;
	bool alpha;
	va_list ap;

	if (!pDraw)
		return;

	etnaviv = etnaviv_get_screen_priv(pDraw->pScreen);
	vPix = etnaviv_drawable_offset(pDraw, &offset);
	if (!vPix)
		return;

	alpha = PICT_FORMAT_A(pDst->format) != 0;

	va_start(ap, fmt);
	dump_pix(etnaviv, vPix, alpha,
		 pDraw->x + offset.x, pDraw->y + offset.y,
		 pDraw->width, pDraw->height, fmt, ap);
	va_end(ap);
}

void dump_vPix(struct etnaviv *etnaviv, struct etnaviv_pixmap *vPix,
	      int alpha, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	dump_pix(etnaviv, vPix, !!alpha,
		 0, 0, vPix->width, vPix->height,
		 fmt, ap);
	va_end(ap);
}
#endif
