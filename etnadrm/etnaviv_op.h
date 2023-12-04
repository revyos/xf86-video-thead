#ifndef ETNAVIV_OP_H
#define ETNAVIV_OP_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>

#include "xf86.h"

#define VIVANTE_MAX_2D_RECTS	256

struct etna_bo;
struct etnaviv;

#define UNKNOWN_FORMAT	0x1f

struct etnaviv_format {
	uint32_t
		format:5,
		swizzle:2,
		tile:1,
		planes:2,
		u:2,
		v:2;
};

struct etnaviv_blend_op {
	uint32_t alpha_mode;
	uint8_t src_mode;	/* DE_BLENDMODE_xx */
	uint8_t dst_mode;	/* DE_BLENDMODE_xx */
	uint8_t src_alpha;
	uint8_t dst_alpha;
};

struct etnaviv_blit_buf {
	struct etnaviv_format format;
	struct etnaviv_pixmap *pixmap;
	struct etna_bo *bo;
	unsigned pitch;
	xPoint offset;
	unsigned short width;
	unsigned short height;
	unsigned rotate;
};

#define INIT_BLIT_BUF(_fmt,_pix,_bo,_pitch,_off,_w,_h,_r)	\
	((struct etnaviv_blit_buf){				\
		.format = _fmt,					\
		.pixmap = _pix,					\
		.bo = _bo,					\
		.pitch = _pitch,				\
		.offset	= _off,					\
		.width = _w,					\
		.height = _h,					\
		.rotate = _r,				\
	})

#define INIT_BLIT_PIX_ROT(_pix, _fmt, _off, _rot) \
	INIT_BLIT_BUF((_fmt), (_pix), (_pix)->etna_bo, (_pix)->pitch, (_off), \
		      (_pix)->width, (_pix)->height, _rot)
#define INIT_BLIT_PIX(_pix, _fmt, _off) \
	INIT_BLIT_PIX_ROT(_pix, _fmt, _off, DE_ROT_MODE_ROT0)

#define INIT_BLIT_BO(_bo, _pitch, _fmt, _off) \
	INIT_BLIT_BUF((_fmt), NULL, (_bo), (_pitch), (_off), 0, 0, DE_ROT_MODE_ROT0)

#define INIT_BLIT_NULL	\
	INIT_BLIT_BUF({ }, NULL, NULL, 0, ZERO_OFFSET, 0, 0, DE_ROT_MODE_ROT0)

#define ZERO_OFFSET ((xPoint){ 0, 0 })

#define SRC_ORIGIN_NONE		0
#define SRC_ORIGIN_ABSOLUTE	1
#define SRC_ORIGIN_RELATIVE	2

struct etnaviv_de_op {
	struct etnaviv_blit_buf dst;
	struct etnaviv_blit_buf src;
	const struct etnaviv_blend_op *blend_op;
	const BoxRec *clip;
	uint8_t src_origin_mode;
	uint8_t rop;
	unsigned cmd;
	Bool brush;
	uint32_t fg_colour;
};

struct etnaviv_vr_op {
	struct etnaviv_blit_buf dst;
	struct etnaviv_blit_buf src;
	uint32_t *src_pitches;
	uint32_t *src_offsets;
	BoxRec src_bounds;
	uint32_t h_scale;
	uint32_t v_scale;
	unsigned cmd;
	unsigned vr_op;
};

void etnaviv_de_start(struct etnaviv *etnaviv, const struct etnaviv_de_op *op);
void etnaviv_de_end(struct etnaviv *etnaviv);
void etnaviv_de_op_src_origin(struct etnaviv *etnaviv,
	const struct etnaviv_de_op *op, xPoint src_origin, const BoxRec *dest);
void etnaviv_de_op(struct etnaviv *etnaviv, const struct etnaviv_de_op *op,
	const BoxRec *pBox, size_t nBox);
void etnaviv_vr_op(struct etnaviv *etnaviv, struct etnaviv_vr_op *op,
	const BoxRec *dst, uint32_t x1, uint32_t y1,
	const BoxRec *boxes, size_t n);
void etnaviv_emit(struct etnaviv *etnaviv);
void etnaviv_flush(struct etnaviv *etnaviv);

#endif
