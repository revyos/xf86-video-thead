/*
 * Marvell Armada DRM-based Xvideo driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <armada_bufmgr.h>

#include "armada_accel.h"
#include "armada_drm.h"
#include "common_drm.h"
#include "xf86Crtc.h"
#include "xf86xv.h"
#include "utils.h"
#include <X11/extensions/Xv.h>
#include <X11/Xatom.h>

#include "armada_ioctl.h"
#include "boxutil.h"
#include "fourcc.h"
#include "xv_attribute.h"
#include "xv_image_format.h"
#include "xvbo.h"

/* Size of physical addresses via BMM */
typedef uint32_t phys_t;
#define INVALID_PHYS	(~(phys_t)0)

#define NR_BUFS	3

enum armada_drm_properties {
	PROP_DRM_SATURATION,
	PROP_DRM_BRIGHTNESS,
	PROP_DRM_CONTRAST,
	PROP_DRM_ITURBT_709,
	PROP_DRM_COLORKEY,
	NR_DRM_PROPS
};

static const char *armada_drm_property_names[NR_DRM_PROPS] = {
	[PROP_DRM_SATURATION] = "saturation",
	[PROP_DRM_BRIGHTNESS] = "brightness",
	[PROP_DRM_CONTRAST] = "contrast",
	[PROP_DRM_ITURBT_709] = "iturbt_709",
	[PROP_DRM_COLORKEY] = "colorkey",
};

struct drm_xv_prop {
	uint32_t prop_id;
	uint64_t value;
};

struct drm_xv {
	int fd;
	struct drm_armada_bufmgr *bufmgr;

	/* Common information */
	xf86CrtcPtr desired_crtc;
	Bool has_xvbo;
	Bool is_xvbo;
	Bool autopaint_colorkey;
	Bool has_primary;
	Bool primary_obscured;

	/* Cached image information */
	RegionRec clipBoxes;
	int fourcc;
	short width;
	short height;
	uint32_t image_size;
	uint32_t pitches[3];
	uint32_t offsets[3];

	unsigned bo_idx;
	struct {
		struct drm_armada_bo *bo;
		uint32_t fb_id;
	} bufs[NR_BUFS];

	struct drm_armada_bo *last_bo;

	int (*get_fb)(ScrnInfoPtr, struct drm_xv *, unsigned char *,
		uint32_t *);
	struct drm_armada_bo *(*import_name)(ScrnInfoPtr, struct drm_xv *,
		uint32_t);

	/* Plane information */
	const struct xv_image_format *plane_format;
	uint32_t plane_fb_id;
	xf86CrtcPtr primary_crtc;
	drmModePlanePtr overlay_plane;
	unsigned int num_planes;
	drmModePlanePtr mode_planes[4];
	struct drm_xv_prop props[NR_DRM_PROPS];
};

enum {
	attr_encoding,
	attr_saturation,
	attr_brightness,
	attr_contrast,
	attr_iturbt_709,
	attr_autopaint_colorkey,
	attr_colorkey,
	attr_pipe,
	attr_deinterlace,
};

static struct xv_attr_data armada_drm_xv_attributes[];

/*
 * Attribute support code
 */
static int armada_drm_prop_set(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 value, pointer data)
{
	struct drm_xv *drmxv = data;
	struct drm_xv_prop *prop = &drmxv->props[attr->id];
	uint32_t prop_id;
	unsigned i;

	if (prop->prop_id == 0)
		return Success; /* Actually BadMatch... */

	prop->value = value;
	prop_id = prop->prop_id;

	for (i = 0; i < drmxv->num_planes; i++)
		drmModeObjectSetProperty(drmxv->fd,
					 drmxv->mode_planes[i]->plane_id,
					 DRM_MODE_OBJECT_PLANE, prop_id,
					 value);

	return Success;
}

static int armada_drm_prop_get(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 *value, pointer data)
{
	struct drm_xv *drmxv = data;
	*value = drmxv->props[attr->id].value;
	return Success;
}

static int armada_drm_set_colorkey(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 value, pointer data)
{
	struct drm_xv *drmxv = data;

	RegionEmpty(&drmxv->clipBoxes);

	return armada_drm_prop_set(pScrn, attr, value, data);
}

static int armada_drm_set_autopaint(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 value, pointer data)
{
	struct drm_xv *drmxv = data;

	drmxv->autopaint_colorkey = !!value;
	if (value != 0) {
		RegionEmpty(&drmxv->clipBoxes);
		return Success;
	}

	attr = &armada_drm_xv_attributes[attr_colorkey];

	/*
	 * If autopainting of the colorkey is disabled, should we
	 * zero the colorkey?  For the time being, we do.
	 */
	return attr->set(pScrn, attr, 0, data);
}

static int armada_drm_get_autopaint(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 *value, pointer data)
{
	struct drm_xv *drmxv = data;
	*value = drmxv->autopaint_colorkey;
	return Success;
}

static int armada_drm_set_pipe(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 value, pointer data)
{
	struct drm_xv *drmxv = data;

	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);

	if (value < -1 || value >= config->num_crtc)
		return BadValue;
	if (value == -1)
		drmxv->desired_crtc = NULL;
	else
		drmxv->desired_crtc = config->crtc[value];
	return Success;
}

static int armada_drm_get_pipe(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 *value, pointer data)
{
	struct drm_xv *drmxv = data;
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	unsigned i;

	*value = -1;

	for (i = 0; i < config->num_crtc; i++)
		if (config->crtc[i] == drmxv->desired_crtc) {
			*value = i;
			break;
		}

	return Success;
}

static int armada_drm_set_ignore(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 value, pointer data)
{
	return Success;
}

static int armada_drm_get_ignore(ScrnInfoPtr pScrn,
	const struct xv_attr_data *attr, INT32 *value, pointer data)
{
	*value = attr->id;
	return Success;
}

/*
 * This must match the strings and order in the table above
 *
 * XvSetPortAttribute(3) suggests that XV_BRIGHTNESS, XV_CONTRAST, XV_HUE
 * and XV_SATURATION should all be in the range of -1000 ... 1000.  It
 * seems not many drivers follow that requirement.
 */
static XF86AttributeRec OverlayAttributes[] = {
	{ XvSettable | XvGettable, 0,      0,          "XV_ENCODING" },
	{ XvSettable | XvGettable, -16384, 16383,      "XV_SATURATION" },
	{ XvSettable | XvGettable, -256,   255,        "XV_BRIGHTNESS" },
	{ XvSettable | XvGettable, -16384, 16383,      "XV_CONTRAST" },
	{ XvSettable | XvGettable, 0,      1,          "XV_ITURBT_709" },
	{ XvSettable | XvGettable, 0,      1,          "XV_AUTOPAINT_COLORKEY"},
	{ XvSettable | XvGettable, 0,      0x00ffffff, "XV_COLORKEY" },
	{ XvSettable | XvGettable, -1,     2,          "XV_PIPE" },
/*	{ XvSettable | XvGettable, 0,      0,          "XV_DEINTERLACE" }, */
};

static struct xv_attr_data armada_drm_xv_attributes[] = {
	[attr_encoding] = {
		.name = "XV_ENCODING",
		.set = armada_drm_set_ignore,
		.get = armada_drm_get_ignore,
		.attr = &OverlayAttributes[attr_encoding],
	},
	[attr_saturation] = {
		.name = "XV_SATURATION",
		.id = PROP_DRM_SATURATION,
		.offset = 16384,
		.set = armada_drm_prop_set,
		.get = armada_drm_prop_get,
		.attr = &OverlayAttributes[attr_saturation],
	},
	[attr_brightness] = {
		.name = "XV_BRIGHTNESS",
		.id = PROP_DRM_BRIGHTNESS,
		.offset = 256,
		.set = armada_drm_prop_set,
		.get = armada_drm_prop_get,
		.attr = &OverlayAttributes[attr_brightness],
	},
	[attr_contrast] = {
		.name = "XV_CONTRAST",
		.id = PROP_DRM_CONTRAST,
		.offset = 16384,
		.set = armada_drm_prop_set,
		.get = armada_drm_prop_get,
		.attr = &OverlayAttributes[attr_contrast],
	},
	[attr_iturbt_709] = {
		.name = "XV_ITURBT_709",
		.id = PROP_DRM_ITURBT_709,
		.set = armada_drm_prop_set,
		.get = armada_drm_prop_get,
		.attr = &OverlayAttributes[attr_iturbt_709],
	},
	[attr_autopaint_colorkey] = {
		.name = "XV_AUTOPAINT_COLORKEY",
		.set = armada_drm_set_autopaint,
		.get = armada_drm_get_autopaint,
		.attr = &OverlayAttributes[attr_autopaint_colorkey],
	},
	[attr_colorkey] = {
		.name = "XV_COLORKEY",
		.id = PROP_DRM_COLORKEY,
		.set = armada_drm_set_colorkey,
		.get = armada_drm_prop_get,
		.attr = &OverlayAttributes[attr_colorkey],
	},
	[attr_pipe] = {
		.name = "XV_PIPE",
		.set = armada_drm_set_pipe,
		.get = armada_drm_get_pipe,
		.attr = &OverlayAttributes[attr_pipe],
	},
	/*
	 * We could stop gst-plugins-bmmxv complaining, but arguably
	 * it is a bug in that code which _assumes_ that this atom
	 * exists.  Hence, this code is commented out.
	[attr_deinterlace] = {
		.name = "XV_DEINTERLACE",
		.set = armada_drm_set_ignore,
		.get = armada_drm_get_ignore,
		.attr = &OverlayAttributes[attr_deinterlace],
	},
	 */
};

static XF86VideoEncodingRec OverlayEncodings[] = {
	{ 0, "XV_IMAGE", 2048, 2048, { 1, 1 }, },
};

/* The list of visuals that which we can render against - anything really */
static XF86VideoFormatRec OverlayFormats[] = {
	{ 8,  PseudoColor },
	{ 16, TrueColor },
	{ 24, TrueColor },
	{ 32, TrueColor },
};


/*
 * These are in order of preference.  The I420/YV12 formats require
 * conversion within the X server rather than the application, that's
 * relatively easy to do, and moreover involves reading less data than
 * I422/YV16.  YV16 and VYUY are not common formats (vlc at least does
 * not have any support for it but does have I422) so these comes at
 * the very end, to try to avoid vlc complaining about them.
 */
static const struct xv_image_format armada_drm_formats[] = {
	/* Standard Xv formats */
	{
		.u.drm_format = DRM_FORMAT_UYVY,
		.xv_image = XVIMAGE_UYVY,
	}, {
		.u.drm_format = DRM_FORMAT_YUYV,
		.xv_image = XVIMAGE_YUY2,
	}, {
		.u.drm_format = DRM_FORMAT_YUV420,
		.xv_image = XVIMAGE_I420,
	}, {
		.u.drm_format = DRM_FORMAT_YVU420,
		.xv_image = XVIMAGE_YV12,
	}, {
	/* Our own formats */
		.u.drm_format = DRM_FORMAT_YUV422,
		.xv_image = XVIMAGE_I422,
	}, {
		.u.drm_format = DRM_FORMAT_YVU422,
		.xv_image = XVIMAGE_YV16,
	}, {
		.u.drm_format = DRM_FORMAT_VYUY,
		.xv_image = XVIMAGE_VYUY,
	}, {
		.u.drm_format = DRM_FORMAT_ARGB8888,
		.xv_image = XVIMAGE_ARGB8888,
	}, {
		.u.drm_format = DRM_FORMAT_ABGR8888,
		.xv_image = XVIMAGE_ABGR8888,
	}, {
		.u.drm_format = DRM_FORMAT_XRGB8888,
		.xv_image = XVIMAGE_XRGB8888,
	}, {
		.u.drm_format = DRM_FORMAT_XBGR8888,
		.xv_image = XVIMAGE_XBGR8888,
	}, {
		.u.drm_format = DRM_FORMAT_RGB888,
		.xv_image = XVIMAGE_RGB888,
	}, {
		.u.drm_format = DRM_FORMAT_BGR888,
		.xv_image = XVIMAGE_BGR888,
	}, {
		.u.drm_format = DRM_FORMAT_ARGB1555,
		.xv_image = XVIMAGE_ARGB1555,
	}, {
		.u.drm_format = DRM_FORMAT_ABGR1555,
		.xv_image = XVIMAGE_ABGR1555,
	}, {
		.u.drm_format = DRM_FORMAT_RGB565,
		.xv_image = XVIMAGE_RGB565
	}, {
		.u.drm_format = DRM_FORMAT_BGR565,
		.xv_image = XVIMAGE_BGR565
	}, {
		/* This must be the last */
		.u.drm_format = 0,
		.xv_image = XVIMAGE_XVBO
	},
};

/* It would be nice to be given the image pointer... */
static const struct xv_image_format *armada_drm_lookup_xvfourcc(int fmt)
{
	return xv_image_xvfourcc(armada_drm_formats,
				 ARRAY_SIZE(armada_drm_formats), fmt);
}

static const struct xv_image_format *armada_drm_lookup_drmfourcc(uint32_t fmt)
{
	return xv_image_drm(armada_drm_formats,
			    ARRAY_SIZE(armada_drm_formats), fmt);
}

static int
armada_drm_get_fmt_info(const struct xv_image_format *fmt,
	uint32_t *pitch, uint32_t *offset, short width, short height)
{
	const XF86ImageRec *img = &fmt->xv_image;
	int ret = 0;

	if (img->id == FOURCC_XVBO) {
		/* Our special XVBO format is only two uint32_t */
		pitch[0] = 2 * sizeof(uint32_t);
		offset[0] = 0;
		ret = pitch[0];
	} else if (img->format == XvPlanar) {
		uint32_t size[3];

		pitch[0] = width / img->horz_y_period;
		pitch[1] = width / img->horz_u_period;
		pitch[2] = width / img->horz_v_period;
		size[0] = (pitch[0] * (height / img->vert_y_period) + 7) & ~7;
		size[1] = (pitch[1] * (height / img->vert_u_period) + 7) & ~7;
		size[2] = (pitch[2] * (height / img->vert_v_period) + 7) & ~7;
		offset[0] = 0;
		offset[1] = offset[0] + size[0];
		offset[2] = offset[1] + size[1];

		ret = size[0] + size[1] + size[2];
	} else if (img->format == XvPacked) {
		offset[0] = 0;
		pitch[0] = width * ((img->bits_per_pixel + 7) / 8);
		ret = offset[0] + pitch[0] * height;
	}

	return ret;
}

static void armada_drm_bufs_free(struct drm_xv *drmxv)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(drmxv->bufs); i++) {
		if (drmxv->bufs[i].fb_id) {
			if (drmxv->bufs[i].fb_id == drmxv->plane_fb_id)
				drmxv->plane_fb_id = 0;
			drmModeRmFB(drmxv->fd, drmxv->bufs[i].fb_id);
			drmxv->bufs[i].fb_id = 0;
		}
		if (drmxv->bufs[i].bo) {
			drm_armada_bo_put(drmxv->bufs[i].bo);
			drmxv->bufs[i].bo = NULL;
		}
	}

	if (drmxv->plane_fb_id) {
		drmModeRmFB(drmxv->fd, drmxv->plane_fb_id);
		drmxv->plane_fb_id = 0;
	}

	if (drmxv->last_bo) {
		drm_armada_bo_put(drmxv->last_bo);
		drmxv->last_bo = NULL;
	}
}

static Bool
armada_drm_create_fbid(struct drm_xv *drmxv, struct drm_armada_bo *bo,
	uint32_t *id)
{
	uint32_t handles[3];

	/* Just set the three plane handles to be the same */
	handles[0] =
	handles[1] =
	handles[2] = bo->handle;

	/* Create the framebuffer object for this buffer */
	if (drmModeAddFB2(drmxv->fd, drmxv->width, drmxv->height,
			  drmxv->plane_format->u.drm_format, handles,
			  drmxv->pitches, drmxv->offsets, id, 0))
		return FALSE;

	return TRUE;
}

static int armada_drm_bufs_alloc(struct drm_xv *drmxv)
{
	struct drm_armada_bufmgr *bufmgr = drmxv->bufmgr;
	uint32_t width = drmxv->width;
	uint32_t height = drmxv->image_size / width / 2;
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(drmxv->bufs); i++) {
		struct drm_armada_bo *bo;

		bo = drm_armada_bo_dumb_create(bufmgr, width, height, 16);
		if (!bo) {
			armada_drm_bufs_free(drmxv);
			return BadAlloc;
		}

		drmxv->bufs[i].bo = bo;
		if (drm_armada_bo_map(bo) ||
		    !armada_drm_create_fbid(drmxv, bo, &drmxv->bufs[i].fb_id)) {
			armada_drm_bufs_free(drmxv);
			return BadAlloc;
		}
	}

	return Success;
}

/*
 * The Marvell Xv protocol hack.
 *
 * This is pretty disgusting - it passes a magic number, a count, the
 * physical address of the BMM buffer, and a checksum via the Xv image
 * interface.
 *
 * The X server is then expected to queue the frame for display, and
 * then overwrite the SHM buffer with its own magic number, a count,
 * the physical address of a used BMM buffer, and a checksum back to
 * the application.
 *
 * Looking at other gstreamer implementations (such as fsl) this kind
 * of thing seems to be rather common, though normally only in one
 * direction.
 */
#define BMM_SHM_MAGIC1  0x13572468
#define BMM_SHM_MAGIC2  0x24681357

static uint32_t armada_drm_bmm_chk(unsigned char *buf, uint32_t len)
{
	uint32_t i, chk, *ptr = (uint32_t *)buf;

	for (chk = i = 0; i < len; i++)
		 chk ^= ptr[i];

	return chk;
}

static Bool armada_drm_is_bmm(unsigned char *buf)
{
	uint32_t *ptr, len;

	if ((uintptr_t)buf & (sizeof(*ptr) - 1))
		return FALSE;

	ptr = (uint32_t *)buf;
	if (*ptr != BMM_SHM_MAGIC1)
		return FALSE;

	len = 2 + ptr[1];
	return armada_drm_bmm_chk(buf, len) == ptr[len];
}

static int
armada_drm_get_xvbo(ScrnInfoPtr pScrn, struct drm_xv *drmxv, unsigned char *buf,
	uint32_t *id)
{
	struct drm_armada_bo *bo;
	uint32_t name = ((uint32_t *)buf)[1];

	/* Lookup the bo for the global name on the DRI2 device */
	bo = drmxv->import_name(pScrn, drmxv, name);
	if (!bo) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] xvbo: import of name 0x%08x failed: %s\n",
			   name, strerror(errno));
		return BadAlloc;
	}

	/* Is this a re-display of the previous frame? */
	if (drmxv->last_bo == bo) {
		drm_armada_bo_put(bo);
		*id = drmxv->plane_fb_id;
		return Success;
	}

	if (!armada_drm_create_fbid(drmxv, bo, id)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			"[drm] XVBO: drmModeAddFB2 failed: %s\n",
			strerror(errno));
		return BadAlloc;
	}

	/* Now replace the last bo with the current bo */
	if (drmxv->last_bo)
		drm_armada_bo_put(drmxv->last_bo);

	drmxv->last_bo = bo;

	return Success;
}

static int
armada_drm_get_std(ScrnInfoPtr pScrn, struct drm_xv *drmxv, unsigned char *src,
	uint32_t *id)
{
	struct drm_armada_bo *bo = drmxv->bufs[drmxv->bo_idx].bo;

	if (bo) {
		/* Copy new image data into the buffer */
		memcpy(bo->ptr, src, drmxv->image_size);

		/* Return this buffer's framebuffer id */
		*id = drmxv->bufs[drmxv->bo_idx].fb_id;

		/* Move to the next buffer index now */
		if (++drmxv->bo_idx >= ARRAY_SIZE(drmxv->bufs))
			drmxv->bo_idx = 0;
	}

	return bo ? Success : BadAlloc;
}

/* Common methods */
static int
armada_drm_Xv_SetPortAttribute(ScrnInfoPtr pScrn, Atom attribute,
	INT32 value, pointer data)
{
	return xv_attr_SetPortAttribute(armada_drm_xv_attributes,
		ARRAY_SIZE(armada_drm_xv_attributes),
		pScrn, attribute, value, data);
}

static int
armada_drm_Xv_GetPortAttribute(ScrnInfoPtr pScrn, Atom attribute,
	INT32 *value, pointer data)
{
	return xv_attr_GetPortAttribute(armada_drm_xv_attributes,
		ARRAY_SIZE(armada_drm_xv_attributes),
		pScrn, attribute, value, data);
}

static void armada_drm_Xv_QueryBestSize(ScrnInfoPtr pScrn, Bool motion,
	short vid_w, short vid_h, short drw_w, short drw_h,
	unsigned int *p_w, unsigned int *p_h, pointer data)
{
	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "QueryBestSize: vid %dx%d drw %dx%d\n",
		   vid_w, vid_h, drw_w, drw_h);
	*p_w = maxt(vid_w, drw_w);
	*p_h = maxt(vid_h, drw_h); 
}

static int
armada_drm_Xv_QueryImageAttributes(ScrnInfoPtr pScrn, int image,
	unsigned short *width, unsigned short *height, int *pitches,
	int *offsets)
{
	const struct xv_image_format *fmt;
	unsigned i, ret = 0;
	uint32_t pitch[3], offset[3];

	*width = (*width + 1) & ~1;
	*height = (*height + 1) & ~1;

	fmt = armada_drm_lookup_xvfourcc(image);
	if (!fmt)
		return 0;

	ret = armada_drm_get_fmt_info(fmt, pitch, offset, *width, *height);
	if (ret) {
		for (i = 0; i < fmt->xv_image.num_planes; i++) {
			if (pitches)
				pitches[i] = pitch[i];
			if (offsets)
				offsets[i] = offset[i];
		}
	}

	return ret;
}

static int
armada_drm_Xv_QueryImageAttributes_noxvbo(ScrnInfoPtr pScrn, int image,
	unsigned short *width, unsigned short *height, int *pitches,
	int *offsets)
{
	if (image == FOURCC_XVBO)
		return 0;

	return armada_drm_Xv_QueryImageAttributes(pScrn, image, width, height,
						  pitches, offsets);
}

/* Plane interface support */
static int
armada_drm_plane_fbid(ScrnInfoPtr pScrn, struct drm_xv *drmxv, int image,
	unsigned char *buf, short width, short height, uint32_t *id)
{
	const struct xv_image_format *fmt;
	Bool is_xvbo = image == FOURCC_XVBO;
	int ret;

	if (is_xvbo)
		/*
		 * XVBO support allows applications to prepare the DRM
		 * buffer object themselves, and pass a global name to
		 * the X server to update the hardware with.  This is
		 * similar to Intel XvMC support, except we also allow
		 * the image format to be specified via a fourcc as the
		 * first word.
		 */
		image = ((uint32_t *)buf)[0];
	else if (armada_drm_is_bmm(buf))
		/*
		 * We no longer handle the old Marvell BMM buffer
		 * passing protocol
		 */
		return BadAlloc;

	if (drmxv->width != width || drmxv->height != height ||
	    drmxv->fourcc != image || !drmxv->plane_format ||
	    drmxv->is_xvbo != is_xvbo) {
		uint32_t size;

		/* format or size changed */
		fmt = armada_drm_lookup_xvfourcc(image);
		if (!fmt)
			return BadMatch;

		/* Check whether this is XVBO mapping */
		if (is_xvbo) {
			drmxv->is_xvbo = TRUE;
			drmxv->get_fb = armada_drm_get_xvbo;
		} else {
			drmxv->is_xvbo = FALSE;
			drmxv->get_fb = armada_drm_get_std;
		}

		armada_drm_bufs_free(drmxv);

		size = armada_drm_get_fmt_info(fmt, drmxv->pitches,
					       drmxv->offsets, width, height);

		drmxv->plane_format = fmt;
		drmxv->image_size = size;
		drmxv->width = width;
		drmxv->height = height;
		drmxv->fourcc = image;

//		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
//			   "[drm] xvbo %u fourcc %08x\n",
//			   is_xvbo, image);

		/* Pre-allocate the buffers if we aren't using XVBO or BMM */
		if (!drmxv->is_xvbo) {
			int ret = armada_drm_bufs_alloc(drmxv);
			if (ret != Success) {
				drmxv->plane_format = NULL;
				return ret;
			}
		}

	}

	ret = drmxv->get_fb(pScrn, drmxv, buf, id);
	if (ret != Success) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] Xv: failed to get framebuffer\n");
		return ret;
	}

	return Success;
}

static void armada_drm_primary_plane_restore(xf86CrtcPtr crtc)
{
	struct common_drm_info *drm = GET_DRM_INFO(crtc->scrn);
	struct common_crtc_info *drmc = common_crtc(crtc);
	int ret;

	ret = drmModeSetPlane(drm->fd, drmc->primary_plane_id,
			      drmc->drm_id, drm->fb_id, 0,
			      crtc->x, crtc->y,
			      crtc->mode.HDisplay, crtc->mode.VDisplay,
			      0, 0,
			      crtc->mode.HDisplay << 16,
			      crtc->mode.VDisplay << 16);
	if (ret)
		xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
			   "[drm] unable to restore plane %u: %s\n",
			   drmc->primary_plane_id, strerror(errno));
}

static Bool armada_drm_primary_plane_disable(xf86CrtcPtr crtc)
{
	struct common_drm_info *drm = GET_DRM_INFO(crtc->scrn);
	struct common_crtc_info *drmc = common_crtc(crtc);
	int ret;

	ret = drmModeSetPlane(drm->fd, drmc->primary_plane_id,
			      0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0);
	if (ret)
		xf86DrvMsg(crtc->scrn->scrnIndex, X_WARNING,
			   "[drm] unable to disable plane %u: %s\n",
			   drmc->primary_plane_id, strerror(errno));

	return ret == 0;
}

static void armada_drm_plane_disable(ScrnInfoPtr pScrn, struct drm_xv *drmxv,
	drmModePlanePtr mode_plane)
{
	int ret;

	ret = drmModeSetPlane(drmxv->fd, mode_plane->plane_id,
			      0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0);
	if (ret)
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "[drm] unable to disable plane %u: %s\n",
			   mode_plane->plane_id, strerror(errno));
}

static void
armada_drm_plane_StopVideo(ScrnInfoPtr pScrn, pointer data, Bool cleanup)
{
	struct drm_xv *drmxv = data;

	if (drmxv->primary_crtc) {
		armada_drm_primary_plane_restore(drmxv->primary_crtc);
		drmxv->primary_crtc = NULL;
	}

	if (drmxv->overlay_plane) {
		RegionEmpty(&drmxv->clipBoxes);
		armada_drm_plane_disable(pScrn, drmxv, drmxv->overlay_plane);
		drmxv->overlay_plane = NULL;
	}

	if (cleanup) {
		drmxv->plane_format = NULL;
		armada_drm_bufs_free(drmxv);
	}
}

static Bool armada_drm_check_plane(ScrnInfoPtr pScrn, struct drm_xv *drmxv,
	xf86CrtcPtr crtc)
{
	unsigned int i;
	uint32_t crtc_mask;

	if (!crtc) {
		/* Not being displayed on a CRTC */
		armada_drm_plane_StopVideo(pScrn, drmxv, FALSE);
		return FALSE;
	}

	crtc_mask = 1 << common_crtc(crtc)->num;

	if (drmxv->primary_crtc && drmxv->primary_crtc != crtc) {
		/* Moved to a different CRTC */
		armada_drm_primary_plane_restore(drmxv->primary_crtc);
		drmxv->primary_crtc = NULL;
		drmxv->primary_obscured = FALSE;
	}

	if (drmxv->overlay_plane &&
	    !(drmxv->overlay_plane->possible_crtcs & crtc_mask)) {
		/* Moved on to a different CRTC */
		armada_drm_plane_disable(pScrn, drmxv, drmxv->overlay_plane);
		drmxv->overlay_plane = NULL;
	}

	if (!drmxv->overlay_plane)
		for (i = 0; i < drmxv->num_planes; i++)
			if (drmxv->mode_planes[i]->possible_crtcs & crtc_mask) {
				drmxv->overlay_plane = drmxv->mode_planes[i];
				break;
			}

	return drmxv->overlay_plane != NULL;
}

/*
 * Fill the clip boxes after we've done the ioctl so we don't impact on
 * latency.
 */
static void armada_drm_xv_draw_colorkey(ScrnInfoPtr pScrn, DrawablePtr pDraw,
	struct drm_xv *drmxv, RegionPtr clipBoxes, Bool repaint)
{
	if (drmxv->autopaint_colorkey &&
	    (repaint || !RegionEqual(&drmxv->clipBoxes, clipBoxes))) {
		RegionCopy(&drmxv->clipBoxes, clipBoxes);
		xf86XVFillKeyHelperDrawable(pDraw,
				    drmxv->props[PROP_DRM_COLORKEY].value,
				    clipBoxes);
	}
}

static int
armada_drm_plane_Put(ScrnInfoPtr pScrn, struct drm_xv *drmxv, uint32_t fb_id,
	short src_x, short src_y, short src_w, short src_h,
	short width, short height, BoxPtr dst, RegionPtr clipBoxes)
{
	xf86CrtcPtr crtc = NULL;
	uint32_t crtc_x, crtc_y;
	INT32 x1, x2, y1, y2;

	x1 = src_x;
	x2 = src_x + src_w;
	y1 = src_y;
	y2 = src_y + src_h;

	if (!xf86_crtc_clip_video_helper(pScrn, &crtc, drmxv->desired_crtc,
					 dst, &x1, &x2, &y1, &y2, clipBoxes,
					 width, height))
		return BadAlloc;

	if (!armada_drm_check_plane(pScrn, drmxv, crtc))
		return Success;

	/* Calculate the position on this CRTC */
	crtc_x = dst->x1 - crtc->x;
	crtc_y = dst->y1 - crtc->y;

	drmModeSetPlane(drmxv->fd, drmxv->overlay_plane->plane_id,
			common_crtc(crtc)->drm_id, fb_id, 0,
			crtc_x, crtc_y, dst->x2 - dst->x1, dst->y2 - dst->y1,
			x1, y1, x2 - x1, y2 - y1);

	if (drmxv->has_primary) {
		BoxRec crtcbox;
		Bool obscured;

		box_init(&crtcbox, crtc->x, crtc->y,
			 xf86ModeWidth(&crtc->mode, crtc->rotation),
			 xf86ModeHeight(&crtc->mode, crtc->rotation));

		obscured = RegionContainsRect(clipBoxes, &crtcbox) == rgnIN;

		if (obscured && !drmxv->primary_obscured) {
			if (common_crtc(crtc)->primary_plane_id &&
			    armada_drm_primary_plane_disable(crtc))
				drmxv->primary_crtc = crtc;
		} else if (!obscured && drmxv->primary_crtc) {
			armada_drm_primary_plane_restore(drmxv->primary_crtc);
			drmxv->primary_crtc = NULL;
		}

		drmxv->primary_obscured = obscured;
	}

	return Success;
}

static int armada_drm_plane_PutImage(ScrnInfoPtr pScrn,
        short src_x, short src_y, short drw_x, short drw_y,
        short src_w, short src_h, short drw_w, short drw_h,
        int image, unsigned char *buf, short width, short height,
        Bool sync, RegionPtr clipBoxes, pointer data, DrawablePtr pDraw)
{
	struct drm_xv *drmxv = data;
	BoxRec dst;
	uint32_t fb_id;
	int ret;

	box_init(&dst, drw_x, drw_y, drw_w, drw_h);

	ret = armada_drm_plane_fbid(pScrn, drmxv, image, buf, width, height,
				    &fb_id);
	if (ret != Success)
		return ret;

	ret = armada_drm_plane_Put(pScrn, drmxv, fb_id,
				    src_x, src_y, src_w, src_h,
				    width, height, &dst, clipBoxes);

	armada_drm_xv_draw_colorkey(pScrn, pDraw, drmxv, clipBoxes, FALSE);

	/* If there was a previous fb, release it. */
	if (drmxv->is_xvbo &&
	    drmxv->plane_fb_id && drmxv->plane_fb_id != fb_id) {
		drmModeRmFB(drmxv->fd, drmxv->plane_fb_id);
		drmxv->plane_fb_id = 0;
	}

	drmxv->plane_fb_id = fb_id;

	return ret;
}

static int armada_drm_plane_ReputImage(ScrnInfoPtr pScrn,
	short src_x, short src_y, short drw_x, short drw_y,
	short src_w, short src_h, short drw_w, short drw_h,
	RegionPtr clipBoxes, pointer data, DrawablePtr pDraw)
{
	struct drm_xv *drmxv = data;
	BoxRec dst;
	int ret;

	if (drmxv->plane_fb_id == 0)
		return Success;

	box_init(&dst, drw_x, drw_y, drw_w, drw_h);

	ret = armada_drm_plane_Put(pScrn, drmxv, drmxv->plane_fb_id,
				   src_x, src_y, src_w, src_h,
				   drmxv->width, drmxv->height,
				   &dst, clipBoxes);

	armada_drm_xv_draw_colorkey(pScrn, pDraw, drmxv, clipBoxes, TRUE);

	return ret;
}

static XF86VideoAdaptorPtr
armada_drm_XvInitPlane(ScrnInfoPtr pScrn, DevUnion *priv, struct drm_xv *drmxv,
	drmModePlanePtr mode_plane)
{
	XF86VideoAdaptorPtr p;
	XF86AttributeRec *attrs;
	XF86ImageRec *images;
	unsigned i, num_images, num_attrs;

	p = xf86XVAllocateVideoAdaptorRec(pScrn);
	if (!p)
		return NULL;

	images = calloc(mode_plane->count_formats + 1, sizeof(*images));
	if (!images) {
		free(p);
		return NULL;
	}

	for (num_images = i = 0; i < mode_plane->count_formats; i++) {
		const struct xv_image_format *fmt;
		uint32_t id = mode_plane->formats[i];

		if (id == 0)
			continue;

		fmt = armada_drm_lookup_drmfourcc(id);
		if (fmt)
			images[num_images++] = fmt->xv_image;
	}

	if (drmxv->has_xvbo)
		images[num_images++] = (XF86ImageRec)XVIMAGE_XVBO;

	attrs = calloc(ARRAY_SIZE(armada_drm_xv_attributes), sizeof(*attrs));
	if (!attrs) {
		free(images);
		free(p);
		return NULL;
	}

	for (num_attrs = i = 0; i < ARRAY_SIZE(armada_drm_xv_attributes); i++) {
		struct xv_attr_data *d = &armada_drm_xv_attributes[i];

		if (d->get == armada_drm_prop_get &&
		    drmxv->props[d->id].prop_id == 0)
			continue;

		attrs[num_attrs++] = *d->attr;
	}

	p->type = XvWindowMask | XvInputMask | XvImageMask;
	p->flags = VIDEO_OVERLAID_IMAGES;
	p->name = "Marvell Armada Overlay Video";
	p->nEncodings = sizeof(OverlayEncodings) / sizeof(XF86VideoEncodingRec);
	p->pEncodings = OverlayEncodings;
	p->nFormats = sizeof(OverlayFormats) / sizeof(XF86VideoFormatRec);
	p->pFormats = OverlayFormats;
	p->nPorts = 1;
	p->pPortPrivates = priv;
	p->nAttributes = num_attrs;
	p->pAttributes = attrs;
	p->nImages = num_images;
	p->pImages = images;
	p->StopVideo = armada_drm_plane_StopVideo;
	p->SetPortAttribute = armada_drm_Xv_SetPortAttribute;
	p->GetPortAttribute = armada_drm_Xv_GetPortAttribute;
	p->QueryBestSize = armada_drm_Xv_QueryBestSize;
	p->PutImage = armada_drm_plane_PutImage;
	p->ReputImage = armada_drm_plane_ReputImage;
	p->QueryImageAttributes = armada_drm_Xv_QueryImageAttributes;

	/*
	 * Simple work-around to disable XVBO support, rather
	 * than run-time testing this.
	 */
	if (!drmxv->has_xvbo)
		p->QueryImageAttributes =
			armada_drm_Xv_QueryImageAttributes_noxvbo;

	return p;
}

static Bool armada_drm_init_atoms(ScrnInfoPtr pScrn)
{
	unsigned i;
	Bool mismatch = FALSE;

	if (armada_drm_xv_attributes[0].x_atom)
		return TRUE;

	if (!xv_attr_init(armada_drm_xv_attributes,
			  ARRAY_SIZE(armada_drm_xv_attributes)))
		return FALSE;

	for (i = 0; i < ARRAY_SIZE(armada_drm_xv_attributes); i++) {
		struct xv_attr_data *d = &armada_drm_xv_attributes[i];

		/*
		 * We could generate the overlay attributes from
		 * our own attribute information, which would
		 * eliminate the need for this check.
		 */
		if (strcmp(d->name, OverlayAttributes[i].name)) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Overlay attribute %u mismatch\n", i);
			mismatch = TRUE;
		}

		/*
		 * XV_PIPE needs to be initialized with the number
		 * of CRTCs, which is not known at build time.
		 */
		if (strcmp(d->name, "XV_PIPE") == 0) {
			xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
			OverlayAttributes[i].max_value = config->num_crtc - 1;
		}
	}

	/* If we encounter a mismatch, error out */
	return !mismatch;
}

static struct drm_armada_bo *armada_drm_import_armada_name(ScrnInfoPtr pScrn,
	struct drm_xv *drmxv, uint32_t name)
{
	return drm_armada_bo_create_from_name(drmxv->bufmgr, name);
}

static struct drm_armada_bo *armada_drm_import_accel_name(ScrnInfoPtr pScrn,
	struct drm_xv *drmxv, uint32_t name)
{
	ScreenPtr scrn = screenInfo.screens[pScrn->scrnIndex];
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	struct drm_armada_bo *bo;
	int fd;

	fd = arm->accel_ops->export_name(scrn, name);
	if (fd == -1) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "export_name failed\n");
		return NULL;
	}

	bo = drm_armada_bo_from_fd(drmxv->bufmgr, fd);
	if (!bo)
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "drm_armada_bo_from_fd failed: %s\n",
			   strerror(errno));
	close(fd);

	return bo;
}

static void armada_drm_property_setup(struct drm_xv *drmxv,
	drmModePropertyPtr prop, uint64_t value)
{
	unsigned int i;

	for (i = 0; i < NR_DRM_PROPS; i++) {
		if (drmxv->props[i].prop_id)
			continue;

		if (strcmp(prop->name, armada_drm_property_names[i]) == 0) {
			drmxv->props[i].prop_id = prop->prop_id;
			drmxv->props[i].value = value;
			break;
		}
	}
}

static void armada_drm_parse_properties(ScrnInfoPtr pScrn,
	struct drm_xv *drmxv, drmModeObjectPropertiesPtr props)
{
	unsigned int i;

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop;
		uint32_t prop_id = props->props[i];
		uint64_t prop_val = props->prop_values[i];

		prop = common_drm_plane_get_property(pScrn, prop_id);
		if (!prop)
			continue;

		armada_drm_property_setup(drmxv, prop, prop_val);
	}
}

static Bool armada_drm_gather_planes(ScrnInfoPtr pScrn, struct drm_xv *drmxv)
{
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	unsigned int i;

	if (!common_drm_init_plane_resources(pScrn))
		return FALSE;

	drmxv->has_primary = drm->has_universal_planes;

	for (i = 0; i < drm->num_overlay_planes &&
		    i < ARRAY_SIZE(drmxv->mode_planes); i++) {
		drmxv->mode_planes[drmxv->num_planes++] =
			drm->overlay_planes[i].mode_plane;

		armada_drm_parse_properties(pScrn, drmxv,
					    drm->overlay_planes[i].mode_props);
	}

	return TRUE;
}

Bool armada_drm_XvInit(ScrnInfoPtr pScrn)
{
	ScreenPtr scrn = screenInfo.screens[pScrn->scrnIndex];
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	struct armada_drm_info *arm = GET_ARMADA_DRM_INFO(pScrn);
	XF86VideoAdaptorPtr xv[2], ovl_adap = NULL, gpu_adap = NULL;
	struct drm_xv *drmxv;
	DevUnion priv[1];
	unsigned num, cap = 0;
	Bool ret, prefer_overlay;

	if (!armada_drm_init_atoms(pScrn))
		return FALSE;

	/* Initialise the GPU textured adapter first. */
	if (arm->accel_ops && arm->accel_ops->xv_init)
		gpu_adap = arm->accel_ops->xv_init(scrn, &cap);

	/* FIXME: we leak this */
	drmxv = calloc(1, sizeof *drmxv);
	if (!drmxv)
		return FALSE;

	if (cap & XVBO_CAP_KMS_DRM) {
		drmxv->has_xvbo = TRUE;
		drmxv->import_name = armada_drm_import_armada_name;
	}
	if (cap & XVBO_CAP_GPU_DRM) {
		drmxv->has_xvbo = TRUE;
		drmxv->import_name = armada_drm_import_accel_name;
	}
	drmxv->fd = drm->fd;
	drmxv->bufmgr = arm->bufmgr;
	drmxv->autopaint_colorkey = TRUE;

	if (!armada_drm_gather_planes(pScrn, drmxv))
		goto err_free;

	if (!xf86ReturnOptValBool(arm->Options, OPTION_XV_DISPRIMARY, TRUE))
		drmxv->has_primary = FALSE;

	if (drmxv->mode_planes[0]) {
		priv[0].ptr = drmxv;
		ovl_adap = armada_drm_XvInitPlane(pScrn, priv, drmxv,
						  drmxv->mode_planes[0]);
		if (!ovl_adap)
			goto err_free;
	}

	prefer_overlay = xf86ReturnOptValBool(arm->Options,
					      OPTION_XV_PREFEROVL, TRUE);

	num = 0;
	if (gpu_adap && !prefer_overlay)
		xv[num++] = gpu_adap;

	if (ovl_adap)
		xv[num++] = ovl_adap;

	if (gpu_adap && prefer_overlay)
		xv[num++] = gpu_adap;

	ret = xf86XVScreenInit(scrn, xv, num);

	if (ovl_adap) {
		free(ovl_adap->pImages);
		free(ovl_adap);
	}
	if (gpu_adap) {
		free(gpu_adap->pImages);
		free(gpu_adap->pPortPrivates);
		free(gpu_adap);
	}
	if (!ret)
		goto err_free;
	return TRUE;

 err_free:
	if (ovl_adap) {
		free(ovl_adap->pImages);
		free(ovl_adap);
	}
	if (gpu_adap) {
		free(gpu_adap->pImages);
		free(gpu_adap->pPortPrivates);
		free(gpu_adap);
	}

	free(drmxv);

	common_drm_cleanup_plane_resources(pScrn);

	return FALSE;
}
