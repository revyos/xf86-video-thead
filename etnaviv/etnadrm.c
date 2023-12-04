/*
 * This is a shim layer between etnaviv APIs and etnaviv DRM
 */
#include "config.h"
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <xf86.h>
#include <xf86drm.h>

#include "bo-cache.h"
#include "etnadrm.h"
#include "etnaviv_drm.h"
#include "compat-list.h"
#include "utils.h"

#include <etnaviv/viv.h>
#include <etnaviv/etna.h>
#include <etnaviv/etna_bo.h>
#include <etnaviv/state.xml.h>
#include "etnaviv_compat.h"

struct etna_viv_conn {
	struct viv_conn conn;
	struct bo_cache cache;
	unsigned int etnadrm_pipe;
	unsigned int api_date;
};

static struct etna_viv_conn *to_etna_viv_conn(struct viv_conn *conn)
{
	return container_of(conn, struct etna_viv_conn, conn);
}

static void etna_bo_cache_free(struct bo_cache *bc, struct bo_entry *be);

struct chip_specs {
	uint32_t param;
	uint32_t offset;
};

static const struct chip_specs specs[] = {
	{
		.param = ETNAVIV_PARAM_GPU_MODEL,
		.offset = offsetof(struct viv_specs, chip_model),
	}, {
		.param = ETNAVIV_PARAM_GPU_REVISION,
		.offset = offsetof(struct viv_specs, chip_revision),
	}, {
		.param = ETNAVIV_PARAM_GPU_FEATURES_0,
		.offset = offsetof(struct viv_specs, chip_features[0]),
	}, {
		.param = ETNAVIV_PARAM_GPU_FEATURES_1,
		.offset = offsetof(struct viv_specs, chip_features[1]),
	}, {
		.param = ETNAVIV_PARAM_GPU_FEATURES_2,
		.offset = offsetof(struct viv_specs, chip_features[2]),
	}, {
		.param = ETNAVIV_PARAM_GPU_FEATURES_3,
		.offset = offsetof(struct viv_specs, chip_features[3]),
	}, {
		.param = ETNAVIV_PARAM_GPU_FEATURES_4,
		.offset = offsetof(struct viv_specs, chip_features[4]),
	}, {
		.param = ETNAVIV_PARAM_GPU_STREAM_COUNT,
		.offset = offsetof(struct viv_specs, stream_count),
	}, {
		.param = ETNAVIV_PARAM_GPU_REGISTER_MAX,
		.offset = offsetof(struct viv_specs, register_max),
	}, {
		.param = ETNAVIV_PARAM_GPU_THREAD_COUNT,
		.offset = offsetof(struct viv_specs, thread_count),
	}, {
		.param = ETNAVIV_PARAM_GPU_VERTEX_CACHE_SIZE,
		.offset = offsetof(struct viv_specs, vertex_cache_size),
	}, {
		.param = ETNAVIV_PARAM_GPU_SHADER_CORE_COUNT,
		.offset = offsetof(struct viv_specs, shader_core_count),
	}, {
		.param = ETNAVIV_PARAM_GPU_PIXEL_PIPES,
		.offset = offsetof(struct viv_specs, pixel_pipes),
	}, {
		.param = ETNAVIV_PARAM_GPU_VERTEX_OUTPUT_BUFFER_SIZE,
		.offset = offsetof(struct viv_specs, vertex_output_buffer_size),
	}, {
		.param = ETNAVIV_PARAM_GPU_BUFFER_SIZE,
		.offset = offsetof(struct viv_specs, buffer_size),
	}, {
		.param = ETNAVIV_PARAM_GPU_INSTRUCTION_COUNT,
		.offset = offsetof(struct viv_specs, instruction_count),
	},
};

static int chip_specs(struct viv_conn *conn, struct viv_specs *out, int pipe)
{
	struct drm_etnaviv_param req;
	uint8_t *p = (uint8_t *)out;
	int i;

	memset(&req, 0, sizeof(req));
	req.pipe = pipe;

	for (i = 0; i < ARRAY_SIZE(specs); i++) {
		req.param = specs[i].param;
		if (drmCommandWriteRead(conn->fd, DRM_ETNAVIV_GET_PARAM,
					&req, sizeof(req)))
			return -1;
		*(uint32_t *)(p + specs[i].offset) = req.value;
	}
	return 0;
}

int etnadrm_open_render(const char *name)
{
	drmVersionPtr version;
	char buf[64];
	int minor, fd, rc;

	for (minor = 0; minor < 64; minor++) {
		snprintf(buf, sizeof(buf), "%s/card%d", DRM_DIR_NAME,
			 minor);

		fd = open(buf, O_RDWR);
		if (fd == -1)
			continue;

		version = drmGetVersion(fd);
		if (version) {
			rc = strcmp(version->name, name);
			drmFreeVersion(version);

			if (rc == 0)
				return fd;
		}

		close(fd);
	}

	return -1;
}

int viv_open(enum viv_hw_type hw_type, struct viv_conn **out)
{
	struct etna_viv_conn *ec;
	struct viv_conn *conn;
	drmVersionPtr version;
	Bool found = FALSE;
	int pipe, err = -1;

	ec = calloc(1, sizeof *ec);
	if (!ec)
		return -1;

	bo_cache_init(&ec->cache, etna_bo_cache_free);

	conn = &ec->conn;

	conn->fd = etnadrm_open_render("etnaviv");
	if (conn->fd == -1)
		goto error;

	version = drmGetVersion(conn->fd);
	if (!version)
		goto error;

	conn->hw_type = hw_type;
	conn->kernel_driver.major = 2;
	conn->kernel_driver.minor = 0;
	conn->kernel_driver.patch = 0;
	conn->kernel_driver.build = 0;

	snprintf(conn->kernel_driver.name, sizeof(conn->kernel_driver.name),
		 "%s DRM kernel driver %u.%u.%u, date %s",
		 version->name,
		 version->version_major,
		 version->version_minor,
		 version->version_patchlevel,
		 version->date);

	/*
	 * Read the driver date code, which will tell us which API to use.
	 * We have four APIs at present, which can be identified via the
	 * date code:
	 *   20130625 and earlier are the original APIs
	 *   20150302 is revision 1 of Pengutronix's API
	 *   20150910 is revision 2 of Pengutronix's API
	 *   20151126 is revision 3 of Pengutronix's API
	 */
	ec->api_date = atoi(version->date);

	conn->base_address = 0;

	/*
	 * Current etnadrm is slightly broken in that it deals with
	 * pipes rather than cores.  A core can be 2D, 2D+3D, 3D or
	 * VG, and conceivably we could have multiple cores of the
	 * same type (though unlikely.)  To allow etnadrm to evolve,
	 * scan the available pipes looking for the first core of the
	 * appropriate GPU type.
	 */
	for (pipe = 0; ; pipe++) {
		if (chip_specs(conn, &conn->chip, pipe)) {
			if (pipe < ETNA_MAX_PIPES)
				continue;
			else
				break;
		}

		switch (hw_type) {
		case VIV_HW_2D:
			if (VIV_FEATURE(conn, chipFeatures, PIPE_2D))
				found = TRUE;
			break;

		case VIV_HW_3D:
			if (VIV_FEATURE(conn, chipFeatures, PIPE_3D))
				found = TRUE;
			break;

		case VIV_HW_2D3D:
			if (VIV_FEATURE(conn, chipFeatures, PIPE_2D) &&
			    VIV_FEATURE(conn, chipFeatures, PIPE_3D))
				found = TRUE;
			break;

		case VIV_HW_VG:
			if (VIV_FEATURE(conn, chipFeatures, PIPE_VG))
				found = TRUE;
			break;
		}

		if (found) {
			ec->etnadrm_pipe = pipe;
			break;
		}
	}

	if (!found)
		goto error;

	*out = conn;
	return VIV_STATUS_OK;

error:
	if (conn->fd >= 0)
		close(conn->fd);
	free(conn);
	return err;
}

int viv_close(struct viv_conn *conn)
{
	struct etna_viv_conn *ec = to_etna_viv_conn(conn);

	if (conn->fd < 0)
		return -1;

	bo_cache_fini(&ec->cache);

	close(conn->fd);
	free(conn);
	return 0;
}

static void etnadrm_convert_timeout(struct drm_etnaviv_timespec *ts,
	uint32_t timeout)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);

	ts->tv_sec = now.tv_sec + timeout / 1000;
	ts->tv_nsec = now.tv_nsec + (timeout % 1000) * 1000 * 1000;
	if (ts->tv_nsec > 1000 * 1000 * 1000) {
		ts->tv_nsec -= 1000 * 1000 * 1000;
		ts->tv_sec += 1;
	}
}

int viv_fence_finish(struct viv_conn *conn, uint32_t fence, uint32_t timeout)
{
	unsigned int api_date = to_etna_viv_conn(conn)->api_date;
	union req {
		struct drm_etnaviv_wait_fence_r20151126 r20151126;
		struct drm_etnaviv_wait_fence_r20130625 r20130625;
	} req;
	int ret;

	if (api_date < ETNAVIV_DATE_PENGUTRONIX3) {
		memset(&req, 0, sizeof(req.r20130625));
		req.r20130625.pipe = to_etna_viv_conn(conn)->etnadrm_pipe;
		req.r20130625.fence = fence;
		etnadrm_convert_timeout(&req.r20130625.timeout, timeout);
		ret = drmCommandWrite(conn->fd, DRM_ETNAVIV_WAIT_FENCE,
				      &req.r20130625, sizeof(req.r20130625));
	} else {
		memset(&req, 0, sizeof(req.r20151126));
		req.r20151126.pipe = to_etna_viv_conn(conn)->etnadrm_pipe;
		req.r20151126.fence = fence;
		if (timeout == 0)
			req.r20151126.flags |= ETNA_WAIT_NONBLOCK;
		etnadrm_convert_timeout(&req.r20151126.timeout, timeout);
		ret = drmCommandWrite(conn->fd, DRM_ETNAVIV_WAIT_FENCE,
				      &req.r20151126, sizeof(req.r20151126));
	}

	if (ret == 0)
		conn->last_fence_id = fence;

	return ret;
}

struct etna_bo {
	struct viv_conn *conn;
	void *logical;
	uint32_t handle;
	size_t size;
	int ref;
	int bo_idx;
	struct xorg_list node;
	struct bo_entry cache;
	uint8_t is_usermem;
};

static int etna_bo_gem_wait(struct etna_bo *bo, uint32_t timeout)
{
	struct viv_conn *conn = bo->conn;
	unsigned int api_date = to_etna_viv_conn(conn)->api_date;

	union req {
		struct drm_etnaviv_gem_wait_r20151126 r20151126;
		struct drm_etnaviv_gem_wait_r20130625 r20130625;
	} req;

	if (api_date < ETNAVIV_DATE_PENGUTRONIX3) {
		memset(&req, 0, sizeof(req.r20130625));
		req.r20130625.pipe = to_etna_viv_conn(conn)->etnadrm_pipe;
		req.r20130625.handle = bo->handle;
		etnadrm_convert_timeout(&req.r20130625.timeout, timeout);
		return drmCommandWrite(conn->fd, DRM_ETNAVIV_GEM_WAIT,
				       &req.r20130625, sizeof(req.r20130625));
	} else {
		memset(&req, 0, sizeof(req.r20151126));
		req.r20151126.pipe = to_etna_viv_conn(conn)->etnadrm_pipe;
		req.r20151126.handle = bo->handle;
		if (timeout == 0)
			req.r20151126.flags |= ETNA_WAIT_NONBLOCK;
		etnadrm_convert_timeout(&req.r20151126.timeout, timeout);
		return drmCommandWrite(conn->fd, DRM_ETNAVIV_GEM_WAIT,
				       &req.r20151126, sizeof(req.r20151126));
	}
}

static void etna_bo_free(struct etna_bo *bo)
{
	struct viv_conn *conn = bo->conn;
	struct drm_gem_close req = {
		.handle = bo->handle,
	};

	if (bo->logical)
		munmap(bo->logical, bo->size);

	if (bo->is_usermem)
		etna_bo_gem_wait(bo, VIV_WAIT_INDEFINITE);

	drmIoctl(conn->fd, DRM_IOCTL_GEM_CLOSE, &req);
	free(bo);
}

static void etna_bo_cache_free(struct bo_cache *bc, struct bo_entry *be)
{
	etna_bo_free(container_of(be, struct etna_bo, cache));
}

static struct etna_bo *etna_bo_bucket_get(struct bo_bucket *bucket)
{
	struct bo_entry *be = bo_cache_bucket_get(bucket);
	struct etna_bo *bo = NULL;

	if (be) {
		bo = container_of(be, struct etna_bo, cache);
		bo->ref = 1;
		bo->bo_idx = -1;
	}

	return bo;
}

int etna_bo_del(struct viv_conn *conn, struct etna_bo *mem, struct etna_queue *queue)
{
	struct etna_viv_conn *ec = to_etna_viv_conn(conn);

	if (--mem->ref == 0) {
		if (mem->cache.bucket)
			bo_cache_put(&ec->cache, &mem->cache);
		else
			etna_bo_free(mem);
		return 0;
	}
	return -1;
}

static struct etna_bo *etna_bo_alloc(struct viv_conn *conn)
{
	struct etna_bo *mem;

	mem = calloc(1, sizeof *mem);
	if (mem) {
		mem->conn = conn;
		mem->ref = 1;
		mem->bo_idx = -1;
	}
	return mem;
}

static struct etna_bo *etna_bo_get(struct viv_conn *conn, size_t bytes,
	uint32_t flags)
{
	struct etna_bo *mem;
	struct drm_etnaviv_gem_new req = {
		.size = bytes,
		.flags = ETNA_BO_WC,
	};
	int ret;

	if ((flags & DRM_ETNA_GEM_TYPE_MASK) == DRM_ETNA_GEM_TYPE_CMD)
		req.flags = ETNA_BO_CMDSTREAM;

	mem = etna_bo_alloc(conn);
	if (!mem)
		return NULL;

	ret = drmCommandWriteRead(conn->fd, DRM_ETNAVIV_GEM_NEW,
				  &req, sizeof(req));
	if (ret) {
		free(mem);
		return NULL;
	}

	mem->size = bytes;
	mem->handle = req.handle;

	return mem;
}

struct etna_bo *etna_bo_new(struct viv_conn *conn, size_t bytes, uint32_t flags)
{
	struct etna_viv_conn *ec = to_etna_viv_conn(conn);
	struct bo_bucket *bucket = NULL;
	struct etna_bo *bo;

	do {
		if ((flags & DRM_ETNA_GEM_TYPE_MASK) == DRM_ETNA_GEM_TYPE_CMD)
			break;

		bucket = bo_cache_bucket_find(&ec->cache, bytes);
		if (!bucket)
			break;

		/* We must allocate the bucket size for it to be re-usable */
		bytes = bucket->size;

		bo = etna_bo_bucket_get(bucket);
		if (bo)
			return bo;
	} while (0);

	bo = etna_bo_get(conn, bytes, flags);
	if (bo)
		bo->cache.bucket = bucket;

	return bo;
}

struct etna_bo *etna_bo_from_dmabuf(struct viv_conn *conn, int fd, int prot)
{
	struct etna_bo *mem;
	off_t size;
	int err;

	mem = etna_bo_alloc(conn);
	if (!mem)
		return NULL;

	size = lseek(fd, 0, SEEK_END);
	if (size == (off_t)-1) {
		free(mem);
		return NULL;
	}

	mem->size = size;

	err = drmPrimeFDToHandle(conn->fd, fd, &mem->handle);
	if (err) {
		free(mem);
		mem = NULL;
	}
	return mem;
}

int etna_bo_to_dmabuf(struct viv_conn *conn, struct etna_bo *mem)
{
	int err, fd;

	err = drmPrimeHandleToFD(conn->fd, mem->handle, 0, &fd);
	if (err < 0)
		return -1;

	return fd;
}

int etna_bo_flink(struct etna_bo *bo, uint32_t *name)
{
	struct drm_gem_flink flink = {
		.handle = etna_bo_handle(bo),
	};

	if (drmIoctl(bo->conn->fd, DRM_IOCTL_GEM_FLINK, &flink))
		return -1;

	*name = flink.name;
	return 0;
}

struct etna_bo *etna_bo_from_name(struct viv_conn *conn, uint32_t name)
{
	struct etna_bo *mem;
	struct drm_gem_open req;
	int err;

	mem = etna_bo_alloc(conn);
	if (!mem)
		return NULL;

	memset(&req, 0, sizeof(req));
	req.name = name;
	err = drmIoctl(conn->fd, DRM_IOCTL_GEM_OPEN, &req);
	if (err < 0) {
		free(mem);
		mem = NULL;
	} else {
		mem->handle = req.handle;
		mem->size = req.size;
	}
	return mem;
}

void *etna_bo_map(struct etna_bo *mem)
{
	if (!mem->size)
		return NULL;

	if (!mem->logical) {
		struct drm_etnaviv_gem_info req = {
			.handle = mem->handle,
		};

		if (drmCommandWriteRead(mem->conn->fd, DRM_ETNAVIV_GEM_INFO,
					&req, sizeof(req)))
			return NULL;

		mem->logical = mmap(0, mem->size, PROT_READ | PROT_WRITE,
				    MAP_SHARED, mem->conn->fd, req.offset);
	}
	return mem->logical;
}

struct etna_bo *etna_bo_from_usermem_prot(struct viv_conn *conn, void *memory, size_t size, int prot)
{
	struct etna_bo *mem;
	struct drm_etnaviv_gem_userptr req = {
		.user_ptr = (uintptr_t)memory,
		.user_size = size,
		.flags = (prot & PROT_READ ? ETNA_USERPTR_READ : 0) |
			 (prot & PROT_WRITE ? ETNA_USERPTR_WRITE : 0),
	};
	int err;

	mem = etna_bo_alloc(conn);
	if (!mem)
		return NULL;

	err = drmCommandWriteRead(conn->fd, DRM_ETNAVIV_GEM_USERPTR, &req,
				  sizeof(req));
	if (err) {
		free(mem);
		mem = NULL;
	} else {
		mem->size = size;
		mem->handle = req.handle;
		mem->is_usermem = TRUE;
	}

	return mem;
}

struct etna_bo *etna_bo_from_usermem(struct viv_conn *conn, void *memory, size_t size)
{
	return etna_bo_from_usermem_prot(conn, memory, size,
					 PROT_READ | PROT_WRITE);
}

int etna_bo_cpu_prep(struct etna_bo *bo, struct etna_ctx *pipe, uint32_t op)
{
	return ETNA_OK;
}

void etna_bo_cpu_fini(struct etna_bo *bo)
{
}

uint32_t etna_bo_gpu_address(struct etna_bo *bo)
{
	return 0x40000000;
}

uint32_t etna_bo_handle(struct etna_bo *bo)
{
	/*
	 * If we're wanting the handle, we're more than likely
	 * exporting it, which means we must not re-use this bo.
	 */
	bo->cache.bucket = NULL;
	return bo->handle;
}

uint32_t etna_bo_size(struct etna_bo *bo)
{
	return bo->size;
}


struct _gcoCMDBUF {
	void *logical;
	unsigned start;
	unsigned offset;
	unsigned num_relocs;
	unsigned max_relocs;
	void *relocs;
	unsigned num_bos;
	unsigned max_bos;
	struct drm_etnaviv_gem_submit_bo *bos;
	struct xorg_list bo_head;
};

int etna_free(struct etna_ctx *ctx)
{
	int i;

	if (!ctx)
		return ETNA_INVALID_ADDR;

	for (i = 0; i < NUM_COMMAND_BUFFERS; i++) {
		if (ctx->cmdbufi[i].bo)
			etna_bo_del(ctx->conn, ctx->cmdbufi[i].bo, NULL);
		if (ctx->cmdbuf[i])
			free(ctx->cmdbuf[i]);
	}

	free(ctx);

	return 0;
}

int etna_create(struct viv_conn *conn, struct etna_ctx **out)
{
	struct etna_ctx *ctx;
	int i;

	ctx = calloc(1, sizeof *ctx);
	if (!ctx)
		return ETNA_OUT_OF_MEMORY;

	ctx->conn = conn;
	ctx->cur_buf = ETNA_NO_BUFFER;

	for (i = 0; i < NUM_COMMAND_BUFFERS; i++) {
		ctx->cmdbuf[i] = calloc(1, sizeof *ctx->cmdbuf[i]);
		if (!ctx->cmdbuf[i])
			goto error;
		xorg_list_init(&ctx->cmdbuf[i]->bo_head);
	}

	if (to_etna_viv_conn(ctx->conn)->api_date < ETNAVIV_DATE_PENGUTRONIX2) {
		void *buf;

		for (i = 0; i < NUM_COMMAND_BUFFERS; i++) {
			ctx->cmdbufi[i].bo = etna_bo_new(conn, COMMAND_BUFFER_SIZE,
							 DRM_ETNA_GEM_TYPE_CMD);
			if (!ctx->cmdbufi[i].bo)
				goto error;

			buf = etna_bo_map(ctx->cmdbufi[i].bo);
			if (!buf)
				goto error;

			ctx->cmdbuf[i]->logical = buf;
		}
	} else {
		void *buf;

		for (i = 0; i < NUM_COMMAND_BUFFERS; i++) {
			buf = malloc(COMMAND_BUFFER_SIZE);
			if (!buf)
				goto error;

			ctx->cmdbuf[i]->logical = buf;
		}
	}

	*out = ctx;

	return ETNA_OK;

 error:
	etna_free(ctx);
	return ETNA_OUT_OF_MEMORY;
}

int etna_set_pipe(struct etna_ctx *ctx, enum etna_pipe pipe)
{
	int ret;

	if (!ctx)
		return ETNA_INVALID_ADDR;

	ret = etna_reserve(ctx, 8);
	if (ret != ETNA_OK)
		return ret;

	ETNA_EMIT_LOAD_STATE(ctx, VIVS_GL_FLUSH_CACHE>>2, 1, 0);
	switch (pipe) {
	case ETNA_PIPE_2D:
		ETNA_EMIT(ctx, VIVS_GL_FLUSH_CACHE_PE2D);
		break;
	case ETNA_PIPE_3D:
		ETNA_EMIT(ctx, VIVS_GL_FLUSH_CACHE_DEPTH | VIVS_GL_FLUSH_CACHE_COLOR);
		break;
	default:
		return ETNA_INVALID_VALUE;
	}

	ETNA_EMIT_LOAD_STATE(ctx, VIVS_GL_SEMAPHORE_TOKEN>>2, 1, 0);
	ETNA_EMIT(ctx, VIVS_GL_SEMAPHORE_TOKEN_FROM(SYNC_RECIPIENT_FE) |
		       VIVS_GL_SEMAPHORE_TOKEN_TO(SYNC_RECIPIENT_PE));
	ETNA_EMIT_STALL(ctx, SYNC_RECIPIENT_FE, SYNC_RECIPIENT_PE);
	ETNA_EMIT_LOAD_STATE(ctx, VIVS_GL_PIPE_SELECT>>2, 1, 0);
	ETNA_EMIT(ctx, pipe);

	return 0;
}

static int etna_reloc_bo_index(struct etna_ctx *ctx, struct etna_bo *mem,
	uint32_t flags)
{
	struct drm_etnaviv_gem_submit_bo *b;
	struct _gcoCMDBUF *buf;
	unsigned idx;

	buf = ctx->cmdbuf[ctx->cur_buf];

	if (mem->bo_idx >= 0) {
		b = &buf->bos[mem->bo_idx];
		b->flags |= flags;
		return mem->bo_idx;
	}

	idx = buf->num_bos;
	if (++buf->num_bos > buf->max_bos) {
		if (buf->max_bos)
			buf->max_bos += 16;
		else
			buf->max_bos = 8;
		b = realloc(buf->bos, buf->max_bos * sizeof *b);
		if (!b)
			return -1;
		buf->bos = b;
	}

	b = &buf->bos[idx];
	b->flags = flags;
	b->handle = mem->handle;
	b->presumed = 0;

	mem->bo_idx = idx;
	mem->ref++;
	xorg_list_append(&mem->node, &buf->bo_head);

	return mem->bo_idx;
}

static int etna_do_flush_r20130625(struct etna_ctx *ctx, uint32_t *fence_out)
{
	struct drm_etnaviv_gem_submit_cmd_r20130625 cmd;
	struct drm_etnaviv_gem_submit_r20130625 req;
	struct _gcoCMDBUF *buf;
	int index, ret;

	index = etna_reloc_bo_index(ctx, ctx->cmdbufi[ctx->cur_buf].bo,
				    ETNA_SUBMIT_BO_READ);
	if (index < 0)
		return ETNA_INTERNAL_ERROR;

	buf = ctx->cmdbuf[ctx->cur_buf];

	memset(&cmd, 0, sizeof(cmd));
	cmd.type = ETNA_SUBMIT_CMD_BUF;
	cmd.submit_idx = index;
	cmd.submit_offset = buf->offset;
	cmd.size = ctx->offset * 4 - buf->offset;
	cmd.relocs = (uintptr_t)buf->relocs;
	cmd.nr_relocs = buf->num_relocs;

	memset(&req, 0, sizeof(req));
	req.pipe = to_etna_viv_conn(ctx->conn)->etnadrm_pipe;
	req.cmds = (uintptr_t)&cmd;
	req.nr_cmds = 1;
	req.bos = (uintptr_t)buf->bos;
	req.nr_bos = buf->num_bos;

	ret = drmCommandWriteRead(ctx->conn->fd, DRM_ETNAVIV_GEM_SUBMIT,
				  &req, sizeof(req));

	if (ret == 0 && fence_out)
		*fence_out = req.fence;

	return ret;
}

static int etna_do_flush_r20150302(struct etna_ctx *ctx, uint32_t *fence_out)
{
	struct drm_etnaviv_gem_submit_cmd_r20150302 cmd;
	struct drm_etnaviv_gem_submit_r20150302 req;
	struct _gcoCMDBUF *buf;
	int ret, index;

	index = etna_reloc_bo_index(ctx, ctx->cmdbufi[ctx->cur_buf].bo,
				    ETNA_SUBMIT_BO_READ);
	if (index < 0)
		return ETNA_INTERNAL_ERROR;

	buf = ctx->cmdbuf[ctx->cur_buf];

	memset(&cmd, 0, sizeof(cmd));
	cmd.type = ETNA_SUBMIT_CMD_BUF;
	cmd.submit_idx = index;
	cmd.submit_offset = buf->offset;
	cmd.size = ctx->offset * 4 - buf->offset;
	cmd.relocs = (uintptr_t)buf->relocs;
	cmd.nr_relocs = buf->num_relocs;

	memset(&req, 0, sizeof(req));
	req.pipe = to_etna_viv_conn(ctx->conn)->etnadrm_pipe;
	req.exec_state = ETNADRM_PIPE_2D;
	req.cmds = (uintptr_t)&cmd;
	req.nr_cmds = 1;
	req.bos = (uintptr_t)buf->bos;
	req.nr_bos = buf->num_bos;

	ret = drmCommandWriteRead(ctx->conn->fd, DRM_ETNAVIV_GEM_SUBMIT,
				  &req, sizeof(req));
	if (ret == 0 && fence_out)
		*fence_out = req.fence;

	return ret;
}

static int etna_do_flush_r20150910(struct etna_ctx *ctx, uint32_t *fence_out)
{
	struct drm_etnaviv_gem_submit_r20150910 req;
	struct _gcoCMDBUF *buf;
	int ret;

	buf = ctx->cmdbuf[ctx->cur_buf];

	memset(&req, 0, sizeof(req));
	req.pipe = to_etna_viv_conn(ctx->conn)->etnadrm_pipe;
	req.exec_state = ETNADRM_PIPE_2D;
	req.nr_bos = buf->num_bos;
	req.nr_relocs = buf->num_relocs;
	req.stream_size = ctx->offset * 4 - buf->offset;
	req.bos = (uintptr_t)buf->bos;
	req.relocs = (uintptr_t)buf->relocs;
	req.stream = (uintptr_t)buf->logical + buf->offset;

	ret = drmCommandWriteRead(ctx->conn->fd, DRM_ETNAVIV_GEM_SUBMIT,
				  &req, sizeof(req));
	if (ret == 0 && fence_out)
		*fence_out = req.fence;

	return ret;
}

int etna_flush(struct etna_ctx *ctx, uint32_t *fence_out)
{
	struct _gcoCMDBUF *buf;
	struct etna_bo *i, *n;
	unsigned int api_date;
	int ret;

	if (!ctx)
		return ETNA_INVALID_ADDR;

	if (ctx->cur_buf == ETNA_CTX_BUFFER)
		return ETNA_INTERNAL_ERROR;
	if (ctx->cur_buf == ETNA_NO_BUFFER)
		return 0;

	api_date = to_etna_viv_conn(ctx->conn)->api_date;
	if (api_date < ETNAVIV_DATE_PENGUTRONIX)
		ret = etna_do_flush_r20130625(ctx, fence_out);
	else if (api_date < ETNAVIV_DATE_PENGUTRONIX2)
		ret = etna_do_flush_r20150302(ctx, fence_out);
	else
		ret = etna_do_flush_r20150910(ctx, fence_out);

	if (ret) {
		fprintf(stderr, "drmCommandWriteRead failed: %s\n",
			strerror(errno));
		return ETNA_INTERNAL_ERROR;
	}

	buf = ctx->cmdbuf[ctx->cur_buf];
	xorg_list_for_each_entry_safe(i, n, &buf->bo_head, node) {
		xorg_list_del(&i->node);
		i->bo_idx = -1;
		etna_bo_del(ctx->conn, i, NULL);
	}

	buf->offset = ctx->offset * 4;
	buf->start = buf->offset + END_COMMIT_CLEARANCE;
	buf->offset = buf->start + BEGIN_COMMIT_CLEARANCE;
	buf->num_bos = 0;
	buf->num_relocs = 0;

	if (buf->offset + END_COMMIT_CLEARANCE >= COMMAND_BUFFER_SIZE)
		buf->start = buf->offset = COMMAND_BUFFER_SIZE - END_COMMIT_CLEARANCE;

	ctx->offset = buf->offset / 4;

	return ETNA_OK;
}

int etna_finish(struct etna_ctx *ctx)
{
	uint32_t fence;
	int ret;

	if (!ctx)
		return ETNA_INVALID_ADDR;

	ret = etna_flush(ctx, &fence);
	if (ret != ETNA_OK)
		return ret;

	ret = viv_fence_finish(ctx->conn, fence, VIV_WAIT_INDEFINITE);
	if (ret != VIV_STATUS_OK)
		return ETNA_INTERNAL_ERROR;

	return ETNA_OK;
}

int _etna_reserve_internal(struct etna_ctx *ctx, size_t n)
{
	uint32_t next_fence;
	int next, ret;

	assert((ctx->offset * 4 + END_COMMIT_CLEARANCE) <= COMMAND_BUFFER_SIZE);
	assert(ctx->cur_buf != ETNA_CTX_BUFFER);

	if (ctx->cur_buf != ETNA_NO_BUFFER) {
		uint32_t fence;

		ret = etna_flush(ctx, &fence);
		assert(ret == ETNA_OK);

		ctx->cmdbufi[ctx->cur_buf].sig_id = fence;
	}

	next = (ctx->cur_buf + 1) % NUM_COMMAND_BUFFERS;

	next_fence = ctx->cmdbufi[next].sig_id;
	if (VIV_FENCE_BEFORE(ctx->conn->last_fence_id, next_fence)) {
		ret = viv_fence_finish(ctx->conn, next_fence,
				       VIV_WAIT_INDEFINITE);
		if (ret)
			return ETNA_INTERNAL_ERROR;
	}

	ctx->cmdbuf[next]->start = 0;
	ctx->cmdbuf[next]->offset = BEGIN_COMMIT_CLEARANCE;

	ctx->cur_buf = next;
	ctx->buf = ctx->cmdbuf[next]->logical;
	ctx->offset = ctx->cmdbuf[next]->offset / 4;

	return 0;
}

void etna_emit_reloc(struct etna_ctx *ctx, uint32_t buf_offset,
	struct etna_bo *mem, uint32_t offset, Bool write)
{
	unsigned int api_date = to_etna_viv_conn(ctx->conn)->api_date;
	struct _gcoCMDBUF *buf = ctx->cmdbuf[ctx->cur_buf];
	union reloc {
		struct drm_etnaviv_gem_submit_reloc_r20151214 r20151214;
		struct drm_etnaviv_gem_submit_reloc_r20150302 r20150302;
		struct drm_etnaviv_gem_submit_reloc_r20130625 r20130625;
	} reloc;
	uint32_t flags;
	size_t size;
	int index, n;

	flags = write ? ETNA_SUBMIT_BO_WRITE : ETNA_SUBMIT_BO_READ;

	index = etna_reloc_bo_index(ctx, mem, flags);
	assert(index >= 0);

	if (api_date < ETNAVIV_DATE_PENGUTRONIX) {
		size = sizeof(reloc.r20130625);
		memset(&reloc, 0, size);
		reloc.r20130625.reloc_idx = index;
		reloc.r20130625.reloc_offset = offset;
		reloc.r20130625.submit_offset = buf_offset * 4;
	} else  if (api_date < ETNAVIV_DATE_PENGUTRONIX2) {
		size = sizeof(reloc.r20150302);
		memset(&reloc, 0, size);
		reloc.r20150302.reloc_idx = index;
		reloc.r20150302.reloc_offset = offset;
		reloc.r20150302.submit_offset = buf_offset * 4;
	} else if (api_date < ETNAVIV_DATE_PENGUTRONIX4) {
		size = sizeof(reloc.r20150302);
		memset(&reloc, 0, size);
		reloc.r20150302.reloc_idx = index;
		reloc.r20150302.reloc_offset = offset;
		reloc.r20150302.submit_offset = buf_offset * 4 - buf->offset;
	} else {
		size = sizeof(reloc.r20151214);
		memset(&reloc, 0, size);
		reloc.r20151214.reloc_idx = index;
		reloc.r20151214.reloc_offset = offset;
		reloc.r20151214.submit_offset = buf_offset * 4 - buf->offset;
	}

	n = buf->num_relocs++;
	if (buf->num_relocs > buf->max_relocs) {
		void *r;

		if (buf->max_relocs)
			buf->max_relocs += 16;
		else
			buf->max_relocs = 8;

		r = realloc(buf->relocs, buf->max_relocs * size);
		assert(r != NULL);
		buf->relocs = r;
	}

	memcpy((char *)buf->relocs + n * size, &reloc, size);
}
