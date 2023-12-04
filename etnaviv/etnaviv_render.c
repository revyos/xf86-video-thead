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
#include <sys/mman.h>
#include <unistd.h>

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif
#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"

#ifdef RENDER

#include "mipict.h"
#include "fbpict.h"

#include "boxutil.h"
#include "glyph_assemble.h"
#include "glyph_cache.h"
#include "glyph_extents.h"
#include "pictureutil.h"
#include "prefetch.h"
#include "unaccel.h"

#include "etnaviv_accel.h"
#include "etnaviv_render.h"
#include "etnaviv_utils.h"

#include <etnaviv/etna_bo.h>
#include <etnaviv/common.xml.h>
#include <etnaviv/state_2d.xml.h>
#include "etnaviv_compat.h"

struct etnaviv_composite_state {
	struct {
		struct etnaviv_pixmap *pix;
		struct etnaviv_format format;
		xPoint offset;
	} dst;
	struct etnaviv_blend_op final_blend;
	struct etnaviv_de_op final_op;
	PixmapPtr pPixTemp;
	RegionRec region;
#ifdef DEBUG_BLEND
	CARD8 op;
#endif
};

static struct etnaviv_format etnaviv_pict_format(PictFormatShort fmt)
{
	unsigned int format, swizzle;

	switch (fmt) {
#define C(pf, vf, sw) case PICT_##pf:				\
		format = DE_FORMAT_##vf;			\
		swizzle = DE_SWIZZLE_##sw;			\
		break

	C(a8r8g8b8, 	A8R8G8B8,	ARGB);
	C(x8r8g8b8, 	X8R8G8B8,	ARGB);
	C(a8b8g8r8, 	A8R8G8B8,	ABGR);
	C(x8b8g8r8, 	X8R8G8B8,	ABGR);
	C(b8g8r8a8, 	A8R8G8B8,	BGRA);
	C(b8g8r8x8, 	X8R8G8B8,	BGRA);
	C(r5g6b5,	R5G6B5,		ARGB);
	C(b5g6r5,	R5G6B5,		ABGR);
	C(a1r5g5b5, 	A1R5G5B5,	ARGB);
	C(x1r5g5b5, 	X1R5G5B5,	ARGB);
	C(a1b5g5r5, 	A1R5G5B5,	ABGR);
	C(x1b5g5r5, 	X1R5G5B5,	ABGR);
	C(a4r4g4b4, 	A4R4G4B4,	ARGB);
	C(x4r4g4b4, 	X4R4G4B4,	ARGB);
	C(a4b4g4r4, 	A4R4G4B4,	ABGR);
	C(x4b4g4r4, 	X4R4G4B4,	ABGR);
	C(a8,		A8,		ARGB);
	C(c8,		INDEX8,		ARGB);
#undef C
	/* Others are unsupported in hardware */
	default:
		return (struct etnaviv_format){
			.format = UNKNOWN_FORMAT,
		};
	}

	return (struct etnaviv_format){
		.format = format,
		.swizzle = swizzle,
	};
}

#ifdef DEBUG_BLEND
static void etnaviv_debug_blend_op(const char *func,
	CARD8 op, CARD16 width, CARD16 height,
	PicturePtr pSrc, INT16 xSrc, INT16 ySrc,
	PicturePtr pMask, INT16 xMask, INT16 yMask,
	PicturePtr pDst, INT16 xDst, INT16 yDst)
{
	char src_buf[80], mask_buf[80], dst_buf[80];

	fprintf(stderr,
		"%s: op 0x%02x %ux%u\n"
		"  src  %+d,%+d %s\n"
		"  mask %+d,%+d %s\n"
		"  dst  %+d,%+d %s\n",
		func, op, width, height,
		xSrc, ySrc, picture_desc(pSrc, src_buf, sizeof(src_buf)),
		xMask, yMask, picture_desc(pMask, mask_buf, sizeof(mask_buf)),
		xDst, yDst, picture_desc(pDst, dst_buf, sizeof(dst_buf)));
}
#endif

struct transform_properties {
	xPoint translation;
	unsigned rot_mode;
};

static Bool picture_transform(PicturePtr pict, struct transform_properties *p)
{
	PictTransformPtr t = pict->transform;

	if (t->matrix[2][0] != 0 ||
	    t->matrix[2][1] != 0 ||
	    t->matrix[2][2] != pixman_int_to_fixed(1))
		return FALSE;

	if (xFixedFrac(t->matrix[0][2]) != 0 ||
	    xFixedFrac(t->matrix[1][2]) != 0)
		return FALSE;

	p->translation.x = pixman_fixed_to_int(t->matrix[0][2]);
	p->translation.y = pixman_fixed_to_int(t->matrix[1][2]);

	if (t->matrix[1][0] == 0 && t->matrix[0][1] == 0) {
		if (t->matrix[0][0] == pixman_int_to_fixed(1) &&
		    t->matrix[1][1] == pixman_int_to_fixed(1)) {
			/* No rotation */
			p->rot_mode = DE_ROT_MODE_ROT0;
			return TRUE;
		} else if (t->matrix[0][0] == pixman_int_to_fixed(-1) &&
			   t->matrix[1][1] == pixman_int_to_fixed(-1)) {
			/* 180° rotation */
			p->rot_mode = DE_ROT_MODE_ROT180;
			p->translation.x -= pict->pDrawable->width;
			p->translation.y -= pict->pDrawable->height;
			return TRUE;
		}
	} else if (t->matrix[0][0] == 0 && t->matrix[1][1] == 0) {
		if (t->matrix[0][1] == pixman_int_to_fixed(-1) &&
		    t->matrix[1][0] == pixman_int_to_fixed(1)) {
			/* Rotate left (90° anti-clockwise) */
			p->rot_mode = DE_ROT_MODE_ROT90;
			p->translation.x -= pict->pDrawable->width;
			return TRUE;
		} else if (t->matrix[0][1] == pixman_int_to_fixed(1) &&
			   t->matrix[1][0] == pixman_int_to_fixed(-1)) {
			/* Rotate right (90° clockwise) */
			p->rot_mode = DE_ROT_MODE_ROT270;
			p->translation.y -= pict->pDrawable->height;
			return TRUE;
		}
	}

	return FALSE;
}

static Bool picture_has_pixels(PicturePtr pPict, xPoint origin,
	const BoxRec *box)
{
	DrawablePtr pDrawable;
	BoxRec b;

	/* If there is no drawable, there are no pixels that can be fetched */
	pDrawable = pPict->pDrawable;
	if (!pDrawable)
		return FALSE;

	if (pPict->filter == PictFilterConvolution)
		return FALSE;

	box_init(&b, origin.x, origin.y, box->x2, box->y2);

	/* transform to the source coordinates if required */
	if (pPict->transform)
		pixman_transform_bounds(pPict->transform, &b);

	/* Does the drawable contain all the pixels we want? */
	return drawable_contains_box(pDrawable, &b);
}

static const struct etnaviv_blend_op etnaviv_composite_op[] = {
#define OP(op,s,d) \
	[PictOp##op] = { \
		.src_mode = DE_BLENDMODE_##s, \
		.dst_mode = DE_BLENDMODE_##d, \
	}
	OP(Clear,       ZERO,     ZERO),
	OP(Src,         ONE,      ZERO),
	OP(Dst,         ZERO,     ONE),
	OP(Over,        ONE,      INVERSED),
	OP(OverReverse, INVERSED, ONE),
	OP(In,          NORMAL,   ZERO),
	OP(InReverse,   ZERO,     NORMAL),
	OP(Out,         INVERSED, ZERO),
	OP(OutReverse,  ZERO,     INVERSED),
	OP(Atop,        NORMAL,   INVERSED),
	OP(AtopReverse, INVERSED, NORMAL),
	OP(Xor,         INVERSED, INVERSED),
	OP(Add,         ONE,      ONE),
#undef OP
};

/* Source alpha is used when the destination mode is not ZERO or ONE */
static Bool etnaviv_op_uses_source_alpha(struct etnaviv_blend_op *op)
{
	if (op->dst_mode == DE_BLENDMODE_ZERO ||
	    op->dst_mode == DE_BLENDMODE_ONE)
		return FALSE;

	return TRUE;
}

static Bool etnaviv_blend_src_alpha_normal(struct etnaviv_blend_op *op)
{
	return (op->alpha_mode &
		VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE__MASK) ==
		VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_NORMAL;
}

static Bool etnaviv_fill_single(struct etnaviv *etnaviv,
	struct etnaviv_pixmap *vPix, const BoxRec *clip, uint32_t colour)
{
	struct etnaviv_de_op op = {
		.clip = clip,
		.rop = 0xf0,
		.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT,
		.brush = TRUE,
		.fg_colour = colour,
	};

	if (!etnaviv_map_gpu(etnaviv, vPix, GPU_ACCESS_RW))
		return FALSE;

	op.dst = INIT_BLIT_PIX(vPix, vPix->pict_format, ZERO_OFFSET);

	etnaviv_batch_start(etnaviv, &op);
	etnaviv_de_op(etnaviv, &op, clip, 1);
	etnaviv_de_end(etnaviv);

	return TRUE;
}

static Bool etnaviv_blend(struct etnaviv *etnaviv, const BoxRec *clip,
	const struct etnaviv_blend_op *blend,
	struct etnaviv_pixmap *vDst, struct etnaviv_pixmap *vSrc,
	const BoxRec *pBox, unsigned nBox, xPoint src_offset,
	xPoint dst_offset)
{
	struct etnaviv_de_op op = {
		.blend_op = blend,
		.clip = clip,
		.src_origin_mode = SRC_ORIGIN_RELATIVE,
		.rop = 0xcc,
		.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT,
		.brush = FALSE,
	};

	if (!etnaviv_map_gpu(etnaviv, vDst, GPU_ACCESS_RW) ||
	    !etnaviv_map_gpu(etnaviv, vSrc, GPU_ACCESS_RO))
		return FALSE;

	op.src = INIT_BLIT_PIX(vSrc, vSrc->pict_format, src_offset);
	op.dst = INIT_BLIT_PIX(vDst, vDst->pict_format, dst_offset);

	etnaviv_batch_start(etnaviv, &op);
	etnaviv_de_op(etnaviv, &op, pBox, nBox);
	etnaviv_de_end(etnaviv);

	return TRUE;
}

static struct etnaviv_format etnaviv_set_format(struct etnaviv_pixmap *vpix, PicturePtr pict)
{
	vpix->pict_format = etnaviv_pict_format(pict->format);
	vpix->pict_format.tile = vpix->format.tile;
	return vpix->pict_format;
}

static struct etnaviv_pixmap *etnaviv_get_scratch_argb(ScreenPtr pScreen,
	PixmapPtr *ppPixmap, unsigned int width, unsigned int height)
{
	struct etnaviv_pixmap *vpix;
	PixmapPtr pixmap;

	if (*ppPixmap)
		return etnaviv_get_pixmap_priv(*ppPixmap);

	pixmap = pScreen->CreatePixmap(pScreen, width, height, 32,
				       CREATE_PIXMAP_USAGE_GPU);
	if (!pixmap)
		return NULL;

	vpix = etnaviv_get_pixmap_priv(pixmap);
	vpix->pict_format = etnaviv_pict_format(PICT_a8r8g8b8);

	*ppPixmap = pixmap;

	return vpix;
}

static Bool etnaviv_pict_solid_argb(PicturePtr pict, uint32_t *col)
{
	unsigned r, g, b, a, rbits, gbits, bbits, abits;
	PictFormatPtr pFormat;
	xRenderColor colour;
	CARD32 pixel;
	uint32_t argb;

	if (!picture_is_solid(pict, &pixel))
		return FALSE;

	pFormat = pict->pFormat;
	/* If no format (eg, source-only) assume it's the correct format */
	if (!pFormat || pict->format == PICT_a8r8g8b8) {
		*col = pixel;
		return TRUE;
	}

	switch (pFormat->type) {
	case PictTypeDirect:
		r = (pixel >> pFormat->direct.red) & pFormat->direct.redMask;
		g = (pixel >> pFormat->direct.green) & pFormat->direct.greenMask;
		b = (pixel >> pFormat->direct.blue) & pFormat->direct.blueMask;
		a = (pixel >> pFormat->direct.alpha) & pFormat->direct.alphaMask;
		rbits = Ones(pFormat->direct.redMask);
		gbits = Ones(pFormat->direct.greenMask);
		bbits = Ones(pFormat->direct.blueMask);
		abits = Ones(pFormat->direct.alphaMask);
		if (abits)
			argb = scale16(a, abits) << 24;
		else
			argb = 0xff000000;
		if (rbits)
			argb |= scale16(r, rbits) << 16;
		if (gbits)
			argb |= scale16(g, gbits) << 8;
		if (bbits)
			argb |= scale16(b, bbits);
		break;
	case PictTypeIndexed:
		miRenderPixelToColor(pFormat, pixel, &colour);
		argb = (colour.alpha >> 8) << 24;
		argb |= (colour.red >> 8) << 16;
		argb |= (colour.green >> 8) << 8;
		argb |= (colour.blue >> 8);
		break;
	default:
		/* unknown type, just assume pixel value */
		argb = pixel;
		break;
	}

	*col = argb;

	return TRUE;
}

static Bool etnaviv_composite_to_pixmap(CARD8 op, PicturePtr pSrc,
	PicturePtr pMask, PixmapPtr pPix, INT16 xSrc, INT16 ySrc,
	INT16 xMask, INT16 yMask, CARD16 width, CARD16 height)
{
	DrawablePtr pDrawable = &pPix->drawable;
	ScreenPtr pScreen = pPix->drawable.pScreen;
	PictFormatPtr f;
	PicturePtr dest;
	int err;

	f = PictureMatchFormat(pScreen, 32, PICT_a8r8g8b8);
	if (!f)
		return FALSE;

	dest = CreatePicture(0, pDrawable, f, 0, 0, serverClient, &err);
	if (!dest)
		return FALSE;
	ValidatePicture(dest);

	unaccel_Composite(op, pSrc, pMask, dest, xSrc, ySrc, xMask, yMask,
			  0, 0, width, height);

	FreePicture(dest, 0);

	return TRUE;
}

/*
 * There is a bug in the GPU hardware with destinations lacking alpha and
 * swizzles BGRA/RGBA.  Rather than the GPU treating bits 7:0 as alpha, it
 * continues to treat bits 31:24 as alpha.  This results in it replacing
 * the B or R bits on input to the blend operation with 1.0.  However, it
 * continues to accept the non-existent source alpha from bits 31:24.
 *
 * Work around this by switching to the equivalent alpha format, and adjust
 * blend operation or alpha subsitution appropriately at the call site.
 */
static Bool etnaviv_workaround_nonalpha(struct etnaviv_format *fmt)
{
	switch (fmt->format) {
	case DE_FORMAT_X4R4G4B4:
		fmt->format = DE_FORMAT_A4R4G4B4;
		return TRUE;
	case DE_FORMAT_X1R5G5B5:
		fmt->format = DE_FORMAT_A1R5G5B5;
		return TRUE;
	case DE_FORMAT_X8R8G8B8:
		fmt->format = DE_FORMAT_A8R8G8B8;
		return TRUE;
	case DE_FORMAT_R5G6B5:
		return TRUE;
	}
	return FALSE;
}

/*
 * Acquire a drawable picture.  origin refers to the location in untransformed
 * space of the origin, which will be updated for the underlying pixmap origin.
 * rotation, if non-NULL, will take the GPU rotation.  Returns NULL if GPU
 * acceleration of this drawable is not possible.
 */
static struct etnaviv_pixmap *etnaviv_acquire_drawable_picture(
	ScreenPtr pScreen, PicturePtr pict, const BoxRec *clip, xPoint *origin,
	unsigned *rotation)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	DrawablePtr drawable = pict->pDrawable;
	struct etnaviv_pixmap *vpix;
	xPoint offset;

	vpix = etnaviv_drawable_offset(drawable, &offset);
	if (!vpix)
		return NULL;

	offset.x += drawable->x;
	offset.y += drawable->y;

	etnaviv_set_format(vpix, pict);
	if (!etnaviv_src_format_valid(etnaviv, vpix->pict_format))
		return NULL;

	if (!picture_has_pixels(pict, *origin, clip))
		return NULL;

	if (pict->transform) {
		struct transform_properties prop;
		struct pixman_transform inv;
		struct pixman_vector vec;

		if (!picture_transform(pict, &prop))
			return NULL;

		if (rotation) {
			switch (prop.rot_mode) {
			case DE_ROT_MODE_ROT180: /* 180°, aka inverted */
			case DE_ROT_MODE_ROT270: /* 90° clockwise, aka right */
				if (!VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20))
					return NULL;
				/* fallthrough */
			case DE_ROT_MODE_ROT0: /* no rotation, aka normal */
			case DE_ROT_MODE_ROT90: /* 90° anti-clockwise, aka left */
				break;
			default:
				return NULL;
			}
			*rotation = prop.rot_mode;
		} else if (prop.rot_mode != DE_ROT_MODE_ROT0) {
			return NULL;
		}

		/* Map the drawable source offsets to destination coords.
		 * The GPU calculates the source coordinate using:
		 * source coord = rotation(destination coord + source origin)
		 * where rotation() rotates around the center point of the
		 * source.  Hence, for a 90° anti-clockwise:
		 *  rotation(x, y) { return (source_width - y), x; }
		 * We need to do some fiddling here to calculate the source
		 * origin values.
		 */
		vec.vector[0] = pixman_int_to_fixed(offset.x + prop.translation.x);
		vec.vector[1] = pixman_int_to_fixed(offset.y + prop.translation.y);
		vec.vector[2] = pixman_int_to_fixed(0);

		pixman_transform_invert(&inv, pict->transform);
		pixman_transform_point(&inv, &vec);

		origin->x += pixman_fixed_to_int(vec.vector[0]);
		origin->y += pixman_fixed_to_int(vec.vector[1]);
	} else {
		/* No transform, simple case */
		origin->x += offset.x;
		origin->y += offset.y;

		if (rotation)
			*rotation = DE_ROT_MODE_ROT0;
	}

	return vpix;
}

/*
 * Acquire the source. If we're filling a solid surface, force it to have
 * alpha; it may be used in combination with a mask.  Otherwise, we ask
 * for the plain source format, with or without alpha, and convert later
 * when copying.  If force_vtemp is set, we ensure that the source is in
 * our temporary pixmap.
 */
static struct etnaviv_pixmap *etnaviv_acquire_src(ScreenPtr pScreen,
	PicturePtr pict, const BoxRec *clip, PixmapPtr *ppPixTemp,
	xPoint *src_topleft, unsigned *rotation, Bool force_vtemp)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vSrc, *vTemp;
	struct etnaviv_blend_op copy_op;
	uint32_t colour;

	if (etnaviv_pict_solid_argb(pict, &colour)) {
		vTemp = etnaviv_get_scratch_argb(pScreen, ppPixTemp,
						 clip->x2, clip->y2);
		if (!vTemp)
			return NULL;

		if (!etnaviv_fill_single(etnaviv, vTemp, clip, colour))
			return NULL;

		src_topleft->x = 0;
		src_topleft->y = 0;

		if (rotation)
			*rotation = DE_ROT_MODE_ROT0;

		return vTemp;
	}

	vSrc = etnaviv_acquire_drawable_picture(pScreen, pict, clip,
						src_topleft, rotation);
	if (!vSrc)
		goto fallback;

	if (force_vtemp)
		goto copy_to_vtemp;

	return vSrc;

fallback:
	vTemp = etnaviv_get_scratch_argb(pScreen, ppPixTemp,
					 clip->x2, clip->y2);
	if (!vTemp)
		return NULL;

	if (!etnaviv_composite_to_pixmap(PictOpSrc, pict, NULL, *ppPixTemp,
					 src_topleft->x, src_topleft->y,
					 0, 0, clip->x2, clip->y2))
		return NULL;

	src_topleft->x = 0;
	src_topleft->y = 0;

	if (rotation)
		*rotation = DE_ROT_MODE_ROT0;

	return vTemp;

copy_to_vtemp:
	vTemp = etnaviv_get_scratch_argb(pScreen, ppPixTemp,
					 clip->x2, clip->y2);
	if (!vTemp)
		return NULL;

	copy_op = etnaviv_composite_op[PictOpSrc];

	if (etnaviv_workaround_nonalpha(&vSrc->pict_format)) {
		copy_op.alpha_mode |= VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_GLOBAL;
		copy_op.src_alpha = 255;
	}

	if (!etnaviv_blend(etnaviv, clip, &copy_op, vTemp, vSrc, clip, 1,
			   *src_topleft, ZERO_OFFSET))
		return NULL;

	src_topleft->x = 0;
	src_topleft->y = 0;

	if (rotation)
		*rotation = DE_ROT_MODE_ROT0;

	return vTemp;
}

/*
 * Compute the regions (in destination pixmap coordinates) which need to
 * be composited.  Each picture's pCompositeClip includes the drawable
 * position, so each position must be adjusted for its position on the
 * backing pixmap.
 */
static Bool etnaviv_compute_composite_region(RegionPtr region,
	PicturePtr pSrc, PicturePtr pMask, PicturePtr pDst,
	INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
	INT16 xDst, INT16 yDst, CARD16 width, CARD16 height)
{
	if (pSrc->pDrawable) {
		xSrc += pSrc->pDrawable->x;
		ySrc += pSrc->pDrawable->y;
	}

	if (pMask && pMask->pDrawable) {
		xMask += pMask->pDrawable->x;
		yMask += pMask->pDrawable->y;
	}

	xDst += pDst->pDrawable->x;
	yDst += pDst->pDrawable->y;

	return miComputeCompositeRegion(region, pSrc, pMask, pDst,
					xSrc, ySrc, xMask, yMask,
					xDst, yDst, width, height) &&
		/* Xorg 1.17 can return TRUE, despite the region being
		 * empty, which goes against the comment immediately
		 * above miComputeCompositeRegion().  It appears to be
		 * a pixman bug, with pixman_region_intersect() returning
		 * TRUE even though the resulting region is empty.
		 */
		RegionNotEmpty(region);
}

static Bool etnaviv_Composite_Clear(PicturePtr pDst, struct etnaviv_composite_state *state)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vDst = state->dst.pix;

	if (!etnaviv_map_gpu(etnaviv, vDst, GPU_ACCESS_RW))
		return FALSE;

	state->final_op.src = INIT_BLIT_PIX(vDst, state->dst.format, ZERO_OFFSET);

	return TRUE;
}

static int etnaviv_accel_composite_srconly(PicturePtr pSrc, PicturePtr pDst,
	INT16 xSrc, INT16 ySrc, INT16 xDst, INT16 yDst,
	struct etnaviv_composite_state *state)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vSrc;
	BoxRec clip_temp;
	xPoint src_topleft;
	unsigned rotation;

	if (pSrc->alphaMap)
		return FALSE;

	/* If the source has no drawable, and is not solid, fallback */
	if (!pSrc->pDrawable && !picture_is_solid(pSrc, NULL))
		return FALSE;

	src_topleft.x = xSrc;
	src_topleft.y = ySrc;

	/* Include the destination drawable's position on the pixmap */
	xDst += pDst->pDrawable->x;
	yDst += pDst->pDrawable->y;

	/*
	 * Compute the temporary image clipping box, which is the
	 * clipping region extents without the destination offset.
	 */
	clip_temp = *RegionExtents(&state->region);
	clip_temp.x1 -= xDst;
	clip_temp.y1 -= yDst;
	clip_temp.x2 -= xDst;
	clip_temp.y2 -= yDst;

	/*
	 * Get the source.  The source image will be described by vSrc with
	 * origin src_topleft.  This may or may not be the temporary image,
	 * and vSrc->pict_format describes its format, including whether the
	 * alpha channel is valid.
	 */
	vSrc = etnaviv_acquire_src(pScreen, pSrc, &clip_temp, &state->pPixTemp,
				   &src_topleft, &rotation, FALSE);
	if (!vSrc)
		return FALSE;

	/*
	 * Apply the same work-around for a non-alpha source as for a
	 * non-alpha destination.  The test order is important here as
	 * we must always have an alpha format, otherwise the selected
	 * alpha mode (by etnaviv_accel_reduce_mask()) will be ignored.
	 */
	if (etnaviv_workaround_nonalpha(&vSrc->pict_format) &&
	    etnaviv_blend_src_alpha_normal(&state->final_blend)) {
		state->final_blend.alpha_mode |= VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_GLOBAL;
		state->final_blend.src_alpha = 255;
	}

	src_topleft.x -= xDst + state->dst.offset.x;
	src_topleft.y -= yDst + state->dst.offset.y;

	if (!etnaviv_map_gpu(etnaviv, state->dst.pix, GPU_ACCESS_RW) ||
	    !etnaviv_map_gpu(etnaviv, vSrc, GPU_ACCESS_RO))
		return FALSE;

	state->final_op.src = INIT_BLIT_PIX_ROT(vSrc, vSrc->pict_format,
						src_topleft, rotation);

	return TRUE;
}

static int etnaviv_accel_composite_masked(PicturePtr pSrc, PicturePtr pMask,
	PicturePtr pDst, INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
	INT16 xDst, INT16 yDst, struct etnaviv_composite_state *state)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vSrc, *vMask, *vTemp;
	struct etnaviv_blend_op mask_op;
	BoxRec clip_temp;
	xPoint src_topleft, mask_offset;

	src_topleft.x = xSrc;
	src_topleft.y = ySrc;
	mask_offset.x = xMask;
	mask_offset.y = yMask;

	/* Include the destination drawable's position on the pixmap */
	xDst += pDst->pDrawable->x;
	yDst += pDst->pDrawable->y;

	/*
	 * Compute the temporary image clipping box, which is the
	 * clipping region extents without the destination offset.
	 */
	clip_temp = *RegionExtents(&state->region);
	clip_temp.x1 -= xDst;
	clip_temp.y1 -= yDst;
	clip_temp.x2 -= xDst;
	clip_temp.y2 -= yDst;

	/* Get a temporary pixmap. */
	vTemp = etnaviv_get_scratch_argb(pScreen, &state->pPixTemp,
					 clip_temp.x2, clip_temp.y2);
	if (!vTemp)
		return FALSE;

	if (pSrc->alphaMap || pMask->alphaMap)
		goto fallback;

	/* If the source has no drawable, and is not solid, fallback */
	if (!pSrc->pDrawable && !picture_is_solid(pSrc, NULL))
		goto fallback;

	mask_op = etnaviv_composite_op[PictOpInReverse];

	if (pMask->componentAlpha && PICT_FORMAT_RGB(pMask->format)) {
		/* Only PE2.0 can do component alpha blends. */
		if (!VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20))
			goto fallback;

		/* Adjust the mask blend (InReverse) to perform the blend. */
		mask_op.alpha_mode =
			VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_NORMAL |
			VIVS_DE_ALPHA_MODES_GLOBAL_DST_ALPHA_MODE_NORMAL;
		mask_op.src_mode = DE_BLENDMODE_ZERO;
		mask_op.dst_mode = DE_BLENDMODE_COLOR;
	}

	if (!pMask->pDrawable)
		goto fallback;

	vMask = etnaviv_acquire_drawable_picture(pScreen, pMask, &clip_temp,
						 &mask_offset, NULL);
	if (!vMask)
		goto fallback;

	/*
	 * Get the source.  The source image will be described by vSrc with
	 * origin src_topleft.  This will always be the temporary image,
	 * which will always have alpha - which is required for the final
	 * blend.
	 */
	vSrc = etnaviv_acquire_src(pScreen, pSrc, &clip_temp, &state->pPixTemp,
				   &src_topleft, NULL, TRUE);
	if (!vSrc)
		goto fallback;

#ifdef DEBUG_BLEND
	etnaviv_batch_wait_commit(etnaviv, vSrc);
	etnaviv_batch_wait_commit(etnaviv, vMask);
	dump_vPix(etnaviv, vSrc, 1, "A-ISRC%2.2x-%p", state->op, pSrc);
	dump_vPix(etnaviv, vMask, 1, "A-MASK%2.2x-%p", state->op, pMask);
#endif

	/*
	 * Blend the source (in the temporary pixmap) with the mask
	 * via a InReverse op.
	 */
	if (!etnaviv_blend(etnaviv, &clip_temp, &mask_op, vSrc, vMask,
			   &clip_temp, 1, mask_offset, ZERO_OFFSET))
		return FALSE;

finish:
	src_topleft.x = -(xDst + state->dst.offset.x);
	src_topleft.y = -(yDst + state->dst.offset.y);

	if (!etnaviv_map_gpu(etnaviv, state->dst.pix, GPU_ACCESS_RW) ||
	    !etnaviv_map_gpu(etnaviv, vSrc, GPU_ACCESS_RO))
		return FALSE;

	state->final_op.src = INIT_BLIT_PIX(vSrc, vSrc->pict_format, src_topleft);

	return TRUE;

fallback:
	/* Do the (src IN mask) in software instead */
	if (!etnaviv_composite_to_pixmap(PictOpSrc, pSrc, pMask, state->pPixTemp,
					 xSrc, ySrc, xMask, yMask,
					 clip_temp.x2, clip_temp.y2))
		return FALSE;

	vSrc = vTemp;
	goto finish;
}

/*
 * Handle cases where we can reduce a (s IN m) OP d operation to
 * a simpler s OP' d operation, possibly modifying OP' to use the
 * GPU global alpha features.
 */
static Bool etnaviv_accel_reduce_mask(struct etnaviv_composite_state *state,
	PicturePtr pSrc, PicturePtr pMask, PicturePtr pDst)
{
	uint32_t colour, alpha_mode = 0;
	uint8_t src_mode;

	/* Deal with component alphas first */
	if (pMask->componentAlpha && PICT_FORMAT_RGB(pMask->format)) {
		/*
		 * The component alpha operation is (for C in R,G,B,A):
		 *  dst.C = tV.C * Fa(OP) + dst.C * Fb(OP)
		 *   where tV.C = src.C * mask.C, tA.C = src.A * mask.C
		 *   and Fa(OP) is the alpha factor based on dst.A
		 *   and Fb(OP) is the alpha factor based on tA.C
		 */
		/* If the mask is solid white, the IN has no effect. */
		if (etnaviv_pict_solid_argb(pMask, &colour)) {
			if (colour == 0xffffffff)
				return TRUE;
		}

		return FALSE;
	}

	/*
	 * If the mask has no alpha, then the alpha channel is treated
	 * as constant 1.0.  This makes the IN operation redundant.
	 */
	if (!PICT_FORMAT_A(pMask->format))
		return TRUE;

	/*
	 * The mask must be a solid colour for any reducing.  At this
	 * point, the only thing that matters is the value of the alpha
	 * component.
	 */
	if (!etnaviv_pict_solid_argb(pMask, &colour))
		return FALSE;

	/* Convert the colour to A8 */
	colour >>= 24;

	/* If the alpha value is 1.0, the mask has no effect. */
	if (colour == 0xff)
		return TRUE;

	/*
	 * A PictOpOver with a mask looks like this:
	 *
	 *  dst.A = src.A * mask.A + dst.A * (1 - src.A * mask.A)
	 *  dst.C = src.C * mask.A + dst.C * (1 - src.A * mask.A)
	 *
	 * Or, in terms of the generic alpha blend equations:
	 *
	 *  dst.A = src.A * Fa + dst.A * Fb
	 *  dst.C = src.C * Fa + dst.C * Fb
	 *
	 * with Fa = mask.A, Fb = (1 - src.A * mask.A).  With a
	 * solid mask, mask.A is constant.
	 *
	 * Our GPU provides us with the ability to replace or scale
	 * src.A and/or dst.A inputs in the generic alpha blend
	 * equations, and using a PictOpAtop operation, the factors
	 * are Fa = dst.A, Fb = 1 - src.A.
	 *
	 * If we subsitute src.A with src.A * mask.A, and dst.A with
	 * mask.A, then we get pretty close for the colour channels:
	 *
	 *   dst.A = src.A * mask.A + mask.A * (1 - src.A * mask.A)
	 *   dst.C = src.C * mask.A + dst.C  * (1 - src.A * mask.A)
	 *
	 * However, the alpha channel becomes simply:
	 *
	 *  dst.A = mask.A
	 *
	 * and hence will be incorrect.  Therefore, the destination
	 * format must not have an alpha channel.
	 *
	 * We can do similar transformations for other operators as
	 * well.
	 */
	switch (src_mode = state->final_blend.src_mode) {
	case DE_BLENDMODE_ZERO:
		/*
		 * Fa = 0, there is no source component in the output, so
		 * there is no need for Fa to involve mask.A
		 */
		break;

	case DE_BLENDMODE_NORMAL:
		/*
		 * Fa = Ad but we need Fa = mask.A * dst.A, so replace the
		 * destination alpha with a scaled version.  However, we can
		 * only do this on non-alpha destinations, hence Fa = mask.A.
		 * Note: non-alpha destinations should never see NORMAL here.
		 */
	case DE_BLENDMODE_ONE:
		/*
		 * Fa = 1, but we need Fa = mask.A, so replace the destination
		 * alpha with mask.A.  This will make the computed destination
		 * alpha incorrect.
		 */
		if (PICT_FORMAT_A(pDst->format))
			return FALSE;

		/*
		 * To replace Fa = 1 with mask.A, we subsitute global alpha
		 * for dst.A, and switch the source blend mode to "NORMAL".
		 */
		alpha_mode |= VIVS_DE_ALPHA_MODES_GLOBAL_DST_ALPHA_MODE_GLOBAL;
		src_mode = DE_BLENDMODE_NORMAL;
		state->final_blend.dst_alpha = colour;
		break;

	case DE_BLENDMODE_INVERSED:
		/*
		 * Fa = mask.A * (1 - dst.A) supportable for non-alpha
		 * destinations as dst.A is defined as 1.0, making Fa = 0.
		 * Note: non-alpha destinations should never see INVERSED here.
		 */
		if (PICT_FORMAT_A(pDst->format))
			return FALSE;

		src_mode = DE_BLENDMODE_ZERO;
		break;

	default:
		/* Other blend modes unsupported. */
		return FALSE;
	}

	switch (state->final_blend.dst_mode) {
	case DE_BLENDMODE_ZERO:
	case DE_BLENDMODE_ONE:
		/*
		 * Fb = 0 or 1, no action required.
		 */
		break;

	case DE_BLENDMODE_NORMAL:
	case DE_BLENDMODE_INVERSED:
		/*
		 * Fb = mask.A * src.A or
		 * Fb = 1 - mask.A * src.A
		 */
		state->final_blend.src_alpha = colour;
		/*
		 * With global scaled alpha and a non-alpha source,
		 * the GPU appears to buggily read and use the X bits
		 * as source alpha.  Work around this by using global
		 * source alpha instead for this case.
		 */
		if (PICT_FORMAT_A(pSrc->format))
			alpha_mode |= VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_SCALED;
		else
			alpha_mode |= VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_GLOBAL;
		break;

	default:
		/* Other blend modes unsupported. */
		return FALSE;
	}

	state->final_blend.src_mode = src_mode;
	state->final_blend.alpha_mode |= alpha_mode;

	return TRUE;
}

/*
 * A composite operation is: (pSrc IN pMask) OP pDst.  We always try
 * to perform an on-GPU "OP" where possible, which is handled by the
 * function below.  The source for this operation is determined by
 * sub-functions.
 */
static int etnaviv_accel_Composite(CARD8 op, PicturePtr pSrc, PicturePtr pMask,
	PicturePtr pDst, INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask,
	INT16 xDst, INT16 yDst, CARD16 width, CARD16 height)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_composite_state state;
	int rc;

#ifdef DEBUG_BLEND
	etnaviv_debug_blend_op(__FUNCTION__, op, width, height,
			       pSrc, xSrc, ySrc,
			       pMask, xMask, yMask,
			       pDst, xDst, yDst);
	state.op = op;
#endif
	state.pPixTemp = NULL;

	/* If the destination has an alpha map, fallback */
	if (pDst->alphaMap)
		return FALSE;

	/* If we can't do the op, there's no point going any further */
	if (op >= ARRAY_SIZE(etnaviv_composite_op))
		return FALSE;

	/* The destination pixmap must have a bo */
	state.dst.pix = etnaviv_drawable_offset(pDst->pDrawable,
						&state.dst.offset);
	if (!state.dst.pix)
		return FALSE;

	state.dst.format = etnaviv_set_format(state.dst.pix, pDst);

	/* ... and the destination format must be supported */
	if (!etnaviv_dst_format_valid(etnaviv, state.dst.format))
		return FALSE;

	state.final_blend = etnaviv_composite_op[op];

	/*
	 * Apply the workaround for non-alpha destination.  The test order
	 * is important here: we only need the full workaround for non-
	 * PictOpClear operations, but we still need the format adjustment.
	 */
	if (etnaviv_workaround_nonalpha(&state.dst.format) &&
	    op != PictOpClear) {
		/*
		 * When the destination does not have an alpha channel, we
		 * need to provide an alpha value of 1.0, and the computed
		 * alpha is irrelevant.  Replace source modes which depend
		 * on destination alpha with their corresponding constant
		 * value modes, rather than using global alpha subsitution.
		 */
		switch (state.final_blend.src_mode) {
		case DE_BLENDMODE_NORMAL:
			state.final_blend.src_mode = DE_BLENDMODE_ONE;
			break;
		case DE_BLENDMODE_INVERSED:
			state.final_blend.src_mode = DE_BLENDMODE_ZERO;
			break;
		}

		/*
		 * PE1.0 hardware contains a bug with non-A8* destinations.
		 * Even though Fb is sampled from the source, it limits the
		 * number of bits to that of the destination format:
		 * RGB565 forces the src.A to one.
		 * A1R5G5B5 limits src.A to the top bit.
		 * A4R4G4B4 limits src.A to the top four bits.
		 */
		if (!VIV_FEATURE(etnaviv->conn, chipMinorFeatures0, 2DPE20) &&
		    state.dst.format.format != DE_FORMAT_A8R8G8B8 &&
		    etnaviv_op_uses_source_alpha(&state.final_blend))
			return FALSE;
	}

	/*
	 * Compute the composite region from the source, mask and
	 * destination positions on their backing pixmaps.  The
	 * transformation is not applied at this stage.
	 */
	if (!etnaviv_compute_composite_region(&state.region, pSrc, pMask, pDst,
					      xSrc, ySrc, xMask, yMask,
					      xDst, yDst, width, height))
		return TRUE;

	miCompositeSourceValidate(pSrc);
	if (pMask)
		miCompositeSourceValidate(pMask);

	if (op == PictOpClear) {
		/* Short-circuit for PictOpClear */
		rc = etnaviv_Composite_Clear(pDst, &state);
	} else if (!pMask || etnaviv_accel_reduce_mask(&state,
						       pSrc, pMask, pDst)) {
		rc = etnaviv_accel_composite_srconly(pSrc, pDst,
						     xSrc, ySrc,
						     xDst, yDst,
						     &state);
	} else {
		rc = etnaviv_accel_composite_masked(pSrc, pMask, pDst,
						    xSrc, ySrc, xMask, yMask,
						    xDst, yDst,
						    &state);
	}

	/*
	 * If we were successful with the previous step(s), complete
	 * the composite operation with the final accelerated blend op.
	 * The above functions will have done the necessary setup for
	 * this step.
	 */
	if (rc) {
		state.final_op.dst = INIT_BLIT_PIX(state.dst.pix,
						   state.dst.format,
						   state.dst.offset);
		state.final_op.clip = RegionExtents(&state.region);
		state.final_op.blend_op = &state.final_blend;
		state.final_op.src_origin_mode = SRC_ORIGIN_RELATIVE;
		state.final_op.rop = 0xcc;
		state.final_op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;
		state.final_op.brush = FALSE;

#ifdef DEBUG_BLEND
		etnaviv_batch_wait_commit(etnaviv, state.final_op.src.pixmap);
		dump_vPix(etnaviv, state.final_op.src.pixmap, 1,
			  "A-FSRC%2.2x-%p", op, pSrc);
		dump_vPix(etnaviv, state.final_op.dst.pixmap, 1,
			  "A-FDST%2.2x-%p", op, pDst);
#endif

		etnaviv_batch_start(etnaviv, &state.final_op);
		etnaviv_de_op(etnaviv, &state.final_op,
			      RegionRects(&state.region),
			      RegionNumRects(&state.region));
		etnaviv_de_end(etnaviv);

#ifdef DEBUG_BLEND
		etnaviv_batch_wait_commit(etnaviv, state.final_op.dst.pixmap);
		dump_vPix(etnaviv, state.final_op.dst.pixmap,
			  PICT_FORMAT_A(pDst->format) != 0,
			  "A-DEST%2.2x-%p", op, pDst);
#endif
	}

	/* Destroy any temporary pixmap we may have allocated */
	if (state.pPixTemp)
		pScreen->DestroyPixmap(state.pPixTemp);

	RegionUninit(&state.region);

	return rc;
}

static Bool etnaviv_accel_Glyphs(CARD8 final_op, PicturePtr pSrc,
	PicturePtr pDst, PictFormatPtr maskFormat, INT16 xSrc, INT16 ySrc,
	int nlist, GlyphListPtr list, GlyphPtr *glyphs)
{
	ScreenPtr pScreen = pDst->pDrawable->pScreen;
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	struct etnaviv_pixmap *vMask;
	struct etnaviv_format fmt;
	struct etnaviv_de_op op;
	PixmapPtr pMaskPixmap;
	PicturePtr pMask, pCurrent;
	BoxRec extents, box;
	CARD32 alpha;
	int width, height, x, y, n, error;
	struct glyph_render *gr, *grp;

	if (!maskFormat)
		return FALSE;

	n = glyphs_assemble(pScreen, &gr, &extents, nlist, list, glyphs);
	if (n == -1)
		return FALSE;
	if (n == 0)
		return TRUE;

	width = extents.x2 - extents.x1;
	height = extents.y2 - extents.y1;

	pMaskPixmap = pScreen->CreatePixmap(pScreen, width, height,
					    maskFormat->depth,
					    CREATE_PIXMAP_USAGE_GPU);
	if (!pMaskPixmap)
		goto destroy_gr;

	alpha = NeedsComponent(maskFormat->format);
	pMask = CreatePicture(0, &pMaskPixmap->drawable, maskFormat,
			      CPComponentAlpha, &alpha, serverClient, &error);
	if (!pMask)
		goto destroy_pixmap;

	/* Drop our reference to the mask pixmap */
	pScreen->DestroyPixmap(pMaskPixmap);

	vMask = etnaviv_get_pixmap_priv(pMaskPixmap);
	/* Clear the mask to transparent */
	fmt = etnaviv_set_format(vMask, pMask);
	box_init(&box, 0, 0, width, height);
	if (!etnaviv_fill_single(etnaviv, vMask, &box, 0))
		goto destroy_picture;

	op.dst = INIT_BLIT_PIX(vMask, fmt, ZERO_OFFSET);
	op.blend_op = &etnaviv_composite_op[PictOpAdd];
	op.clip = &box;
	op.src_origin_mode = SRC_ORIGIN_NONE;
	op.rop = 0xcc;
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;
	op.brush = FALSE;

	pCurrent = NULL;
	for (grp = gr; grp < gr + n; grp++) {
		if (pCurrent != grp->picture) {
			PixmapPtr pPix = drawable_pixmap(grp->picture->pDrawable);
			struct etnaviv_pixmap *v = etnaviv_get_pixmap_priv(pPix);

			if (!etnaviv_map_gpu(etnaviv, v, GPU_ACCESS_RO))
				goto destroy_picture;

			if (pCurrent)
				etnaviv_de_end(etnaviv);

			prefetch(grp);

			op.src = INIT_BLIT_PIX(v, v->pict_format, ZERO_OFFSET);

			pCurrent = grp->picture;

			etnaviv_batch_start(etnaviv, &op);
		}

		prefetch(grp + 1);

		etnaviv_de_op_src_origin(etnaviv, &op, grp->glyph_pos,
					 &grp->dest_box);
	}
	etnaviv_de_end(etnaviv);

	free(gr);

	x = extents.x1;
	y = extents.y1;

	/*
	 * x,y correspond to the top/left corner of the glyphs.
	 * list->xOff,list->yOff correspond to the baseline.  The passed
	 * xSrc/ySrc also correspond to this point.  So, we need to adjust
	 * the source for the top/left corner of the glyphs to be rendered.
	 */
	xSrc += x - list->xOff;
	ySrc += y - list->yOff;

	CompositePicture(final_op, pSrc, pMask, pDst, xSrc, ySrc, 0, 0, x, y,
			 width, height);

	FreePicture(pMask, 0);
	return TRUE;

destroy_picture:
	FreePicture(pMask, 0);
	free(gr);
	return FALSE;

destroy_pixmap:
	pScreen->DestroyPixmap(pMaskPixmap);
destroy_gr:
	free(gr);
	return FALSE;
}

static void etnaviv_accel_glyph_upload(ScreenPtr pScreen, PicturePtr pDst,
	GlyphPtr pGlyph, PicturePtr pSrc, unsigned x, unsigned y)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	PixmapPtr src_pix = drawable_pixmap(pSrc->pDrawable);
	PixmapPtr dst_pix = drawable_pixmap(pDst->pDrawable);
	struct etnaviv_pixmap *vdst = etnaviv_get_pixmap_priv(dst_pix);
	struct etnaviv_format fmt;
	struct etnaviv_de_op op;
	unsigned width = pGlyph->info.width;
	unsigned height = pGlyph->info.height;
	unsigned old_pitch = src_pix->devKind;
	unsigned i, pitch = ALIGN(old_pitch, 16);
	struct etna_bo *usr = NULL;
	BoxRec box;
	xPoint src_offset, dst_offset = { 0, };
	struct etnaviv_pixmap *vpix;
	void *b = NULL;

	src_offset.x = -x;
	src_offset.y = -y;

	vpix = etnaviv_get_pixmap_priv(src_pix);
	if (vpix) {
		fmt = etnaviv_set_format(vpix, pSrc);
		op.src = INIT_BLIT_PIX(vpix, fmt, src_offset);
	} else {
		struct etnaviv_usermem_node *unode;
		char *buf, *src = src_pix->devPrivate.ptr;
		size_t size, align = maxt(VIVANTE_ALIGN_MASK, getpagesize());

		unode = malloc(sizeof(*unode));
		if (!unode)
			return;

		memset(unode, 0, sizeof(*unode));

		size = pitch * height + align - 1;
		size &= ~(align - 1);

		if (posix_memalign(&b, align, size))
			return;

		for (i = 0, buf = b; i < height; i++, buf += pitch)
			memcpy(buf, src + old_pitch * i, old_pitch);

		usr = etna_bo_from_usermem_prot(etnaviv->conn, b, size, PROT_READ);
		if (!usr) {
			xf86DrvMsg(etnaviv->scrnIndex, X_ERROR,
				   "etnaviv: %s: etna_bo_from_usermem_prot(ptr=%p, size=%zu) failed: %s\n",
				   __FUNCTION__, b, size, strerror(errno));
			free(b);
			return;
		}

		unode->bo = usr;
		unode->mem = b;

		/* Add this to the list of usermem nodes to be freed */
		etnaviv_add_freemem(etnaviv, unode);

		op.src = INIT_BLIT_BO(usr, pitch,
				      etnaviv_pict_format(pSrc->format),
				      src_offset);
	}

	box_init(&box, x, y, width, height);

	fmt = etnaviv_set_format(vdst, pDst);

	if (!etnaviv_map_gpu(etnaviv, vdst, GPU_ACCESS_RW))
		return;

	op.dst = INIT_BLIT_PIX(vdst, fmt, dst_offset);
	op.blend_op = NULL;
	op.clip = &box;
	op.src_origin_mode = SRC_ORIGIN_RELATIVE;
	op.rop = 0xcc;
	op.cmd = VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT;
	op.brush = FALSE;

	etnaviv_batch_start(etnaviv, &op);
	etnaviv_de_op(etnaviv, &op, &box, 1);
	etnaviv_de_end(etnaviv);
}

static void
etnaviv_Composite(CARD8 op, PicturePtr pSrc, PicturePtr pMask, PicturePtr pDst,
	INT16 xSrc, INT16 ySrc, INT16 xMask, INT16 yMask, INT16 xDst, INT16 yDst,
	CARD16 width, CARD16 height)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDst->pDrawable->pScreen);
	Bool ret;

	if (!etnaviv->force_fallback) {
		ret = etnaviv_accel_Composite(op, pSrc, pMask, pDst,
					      xSrc, ySrc, xMask, yMask,
					      xDst, yDst, width, height);
		if (ret)
			return;
	}
	unaccel_Composite(op, pSrc, pMask, pDst, xSrc, ySrc,
			  xMask, yMask, xDst, yDst, width, height);
}

static void etnaviv_Glyphs(CARD8 op, PicturePtr pSrc, PicturePtr pDst,
	PictFormatPtr maskFormat, INT16 xSrc, INT16 ySrc, int nlist,
	GlyphListPtr list, GlyphPtr * glyphs)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pDst->pDrawable->pScreen);

	if (etnaviv->force_fallback ||
	    !etnaviv_accel_Glyphs(op, pSrc, pDst, maskFormat,
				  xSrc, ySrc, nlist, list, glyphs))
		unaccel_Glyphs(op, pSrc, pDst, maskFormat,
			       xSrc, ySrc, nlist, list, glyphs);
}

static void etnaviv_UnrealizeGlyph(ScreenPtr pScreen, GlyphPtr glyph)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);

	glyph_cache_remove(pScreen, glyph);

	etnaviv->UnrealizeGlyph(pScreen, glyph);
}

static const unsigned glyph_formats[] = {
	PICT_a8r8g8b8,
	PICT_a8,
};

static Bool etnaviv_CreateScreenResources(ScreenPtr pScreen)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	Bool ret;

	pScreen->CreateScreenResources = etnaviv->CreateScreenResources;
	ret = pScreen->CreateScreenResources(pScreen);
	if (ret) {
		size_t num = 1;

		/*
		 * If the 2D engine can do A8 targets, then enable
		 * PICT_a8 for glyph cache acceleration.
		 */
		if (VIV_FEATURE(etnaviv->conn, chipMinorFeatures0,
				2D_A8_TARGET)) {
			xf86DrvMsg(etnaviv->scrnIndex, X_INFO,
				   "etnaviv: A8 target supported\n");
			num = 2;
		} else {
			xf86DrvMsg(etnaviv->scrnIndex, X_INFO,
				   "etnaviv: A8 target not supported\n");
		}

		ret = glyph_cache_init(pScreen, etnaviv_accel_glyph_upload,
				       glyph_formats, num,
				       /* CREATE_PIXMAP_USAGE_TILE | */
				       CREATE_PIXMAP_USAGE_GPU);
	}
	return ret;
}

void etnaviv_render_screen_init(ScreenPtr pScreen)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	PictureScreenPtr ps = GetPictureScreenIfSet(pScreen);

	if (!etnaviv->force_fallback) {
		etnaviv->CreateScreenResources = pScreen->CreateScreenResources;
		pScreen->CreateScreenResources = etnaviv_CreateScreenResources;
	}

	etnaviv->Composite = ps->Composite;
	ps->Composite = etnaviv_Composite;
	etnaviv->Glyphs = ps->Glyphs;
	ps->Glyphs = etnaviv_Glyphs;
	etnaviv->UnrealizeGlyph = ps->UnrealizeGlyph;
	ps->UnrealizeGlyph = etnaviv_UnrealizeGlyph;
	etnaviv->Triangles = ps->Triangles;
	ps->Triangles = unaccel_Triangles;
	etnaviv->Trapezoids = ps->Trapezoids;
	ps->Trapezoids = unaccel_Trapezoids;
	etnaviv->AddTriangles = ps->AddTriangles;
	ps->AddTriangles = unaccel_AddTriangles;
	etnaviv->AddTraps = ps->AddTraps;
	ps->AddTraps = unaccel_AddTraps;
}

void etnaviv_render_close_screen(ScreenPtr pScreen)
{
	struct etnaviv *etnaviv = etnaviv_get_screen_priv(pScreen);
	PictureScreenPtr ps = GetPictureScreenIfSet(pScreen);

	/* Restore the Pointers */
	ps->Composite = etnaviv->Composite;
	ps->Glyphs = etnaviv->Glyphs;
	ps->UnrealizeGlyph = etnaviv->UnrealizeGlyph;
	ps->Triangles = etnaviv->Triangles;
	ps->Trapezoids = etnaviv->Trapezoids;
	ps->AddTriangles = etnaviv->AddTriangles;
	ps->AddTraps = etnaviv->AddTraps;
}
#endif
