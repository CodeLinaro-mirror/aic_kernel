// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2019-2020, The Linux Foundation. All rights reserved. */

#include <linux/completion.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/moduleparam.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <uapi/misc/qaic.h>

#include "qaic.h"

#define PGOFF_DBC_SHIFT 32
#define PGOFF_DBC_MASK	GENMASK_ULL(63, 32)
#define SEM_VAL_MASK	GENMASK_ULL(11, 0)
#define SEM_INDEX_MASK	GENMASK_ULL(4, 0)
#define BULK_XFER	BIT(3)
#define GEN_COMPLETION	BIT(4)
#define INBOUND_XFER	1
#define OUTBOUND_XFER	2
#define REQHP_OFF	0x0 /* we read this */
#define REQTP_OFF	0x4 /* we write this */
#define RSPHP_OFF	0x8 /* we write this */
#define RSPTP_OFF	0xc /* we read this */

#define ENCODE_SEM(val, index, sync, cmd, flags)			\
			((val) |					\
			(index) << 16 |					\
			(sync) << 22 |					\
			(cmd) << 24 |					\
			((cmd) ? BIT(31) : 0) |				\
			(((flags) & SEM_INSYNCFENCE) ? BIT(30) : 0) |	\
			(((flags) & SEM_OUTSYNCFENCE) ? BIT(29) : 0))

static unsigned int wait_exec_default_timeout = 5000; /* 5 sec default */
module_param(wait_exec_default_timeout, uint, 0600);

struct dbc_req { /* everything must be little endian encoded */
	u16	req_id;
	u8	seq_id;
	u8	cmd;
	u32	resv;
	u64	src_addr;
	u64	dest_addr;
	u32	len;
	u32	resv2;
	u64	db_addr; /* doorbell address */
	u8	db_len; /* not a raw value, special encoding */
	u8	resv3;
	u16	resv4;
	u32	db_data;
	u32	sem_cmd0;
	u32	sem_cmd1;
	u32	sem_cmd2;
	u32	sem_cmd3;
} __packed;

struct dbc_rsp { /* everything must be little endian encoded */
	u16	req_id;
	u16	status;
} __packed;

/*
 * dma_buf_handle is used for import path, and alloc_buf_handle is used
 * for export path. Each of them is used by several mem_handle. When a
 * mem_handle uses sg from either of the export or import handle, it
 * need to increase respective reference count.
 * Complete set of mem_handle for one batch is expected to use either
 * export path or import path. Mix of them is not allowed.
 */

struct dma_buf_handle {
	int			buf_fd;
	struct dma_buf		*buf;
	struct dma_buf_attachment *attach;
	struct sg_table		*sgt;
	struct idr		*handles;
	int			dir;
	struct kref		ref_count;
};

struct alloc_buf_handle {
	struct sg_table		*sgt;
	int			handle;
	struct idr		*handles;
	struct kref		ref_count;
};

struct mem_handle {
	struct sg_table		*sgt;  /* Mapped pages */
	int			nents; /* num dma mapped elements in sgt */
	int			dir;   /* see enum dma_data_direction */
	struct dbc_req		*reqs;
	struct list_head	list;
	u16			req_id;/* req_id for the xfer while in flight */
	struct completion	xfer_done;
	struct kref		ref_count;
	struct qaic_device	*qdev;
	bool			queued;
	bool			no_xfer;
	u32			dbc_id;
	bool			export;
	union {
		struct dma_buf_handle	*dma;
		struct alloc_buf_handle	*alloc;
	} handle;
};

inline int get_dbc_req_elem_size(void)
{
	return sizeof(struct dbc_req);
}

inline int get_dbc_rsp_elem_size(void)
{
	return sizeof(struct dbc_rsp);
}

static int reserve_pages(unsigned long start_pfn, unsigned long nr_pages,
			 bool reserve)
{
	unsigned long pfn;
	unsigned long end_pfn = start_pfn + nr_pages;
	struct page *page;

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		if (!pfn_valid(pfn))
			return -EINVAL;
		page =  pfn_to_page(pfn);
		if (reserve)
			SetPageReserved(page);
		else
			ClearPageReserved(page);
	}
	return 0;
}

static void free_alloc_handle(struct kref *kref)
{
	struct alloc_buf_handle *alloc_handle =
				container_of(kref, struct alloc_buf_handle,
					     ref_count);
	struct sg_table *sgt = alloc_handle->sgt;
	struct scatterlist *sg;

	for (sg = sgt->sgl; sg; sg = sg_next(sg))
		if (sg_page(sg)) {
			reserve_pages(page_to_pfn(sg_page(sg)),
				      DIV_ROUND_UP(sg->length,
				      PAGE_SIZE), false);
			__free_pages(sg_page(sg), get_order(sg->length));
		}
	sg_free_table(sgt);
	kfree(sgt);
	idr_remove(alloc_handle->handles, alloc_handle->handle);
	kfree(alloc_handle);
}

static void free_dma_handle(struct kref *kref)
{
	struct dma_buf_handle *dma_handle =
				container_of(kref, struct dma_buf_handle,
					     ref_count);

	dma_buf_unmap_attachment(dma_handle->attach, dma_handle->sgt,
				 dma_handle->dir);
	dma_buf_detach(dma_handle->buf, dma_handle->attach);
	dma_buf_put(dma_handle->buf);
	idr_remove(dma_handle->handles, dma_handle->buf_fd);
	kfree(dma_handle);
}

static void free_handle_mem(struct kref *kref)
{
	struct mem_handle *mem = container_of(kref, struct mem_handle,
					      ref_count);

	if (mem->export)
		dma_unmap_sg(&mem->qdev->pdev->dev, mem->sgt->sgl,
			     mem->sgt->nents, mem->dir);
	mutex_lock(&mem->qdev->dbc[mem->dbc_id].handle_lock);
	if (mem->export)
		kref_put(&mem->handle.alloc->ref_count, free_alloc_handle);
	else
		kref_put(&mem->handle.dma->ref_count, free_dma_handle);
	mutex_unlock(&mem->qdev->dbc[mem->dbc_id].handle_lock);
	sg_free_table(mem->sgt);
	kfree(mem->sgt);
	kfree(mem->reqs);
	kfree(mem);
}

static int free_one_handle(struct qaic_device *qdev,
			   struct qaic_mem_req_entry *req, u32 req_dbc_id)
{
	struct mem_handle *mem;
	unsigned long flags;
	int handle;
	int dbc_id;
	int ret;

	handle = req->handle & ~PGOFF_DBC_MASK;
	dbc_id = (req->handle & PGOFF_DBC_MASK) >> PGOFF_DBC_SHIFT;

	/* we shifted up by PAGE_SHIFT to make mmap happy, need to undo that */
	handle >>= PAGE_SHIFT;
	dbc_id >>= PAGE_SHIFT;

	if (dbc_id != req_dbc_id)
		return -EINVAL;

	ret = mutex_lock_interruptible(&qdev->dbc[dbc_id].mem_lock);
	if (ret)
		goto lock_fail;
	mem = idr_find(&qdev->dbc[dbc_id].mem_handles, handle);
	if (mem) {
		spin_lock_irqsave(&qdev->dbc[dbc_id].xfer_lock, flags);
		if (mem->queued)
			ret = -EINVAL;
		else
			idr_remove(&qdev->dbc[dbc_id].mem_handles, handle);
		spin_unlock_irqrestore(&qdev->dbc[dbc_id].xfer_lock, flags);
	} else {
		ret = -ENODEV;
	}
	mutex_unlock(&qdev->dbc[dbc_id].mem_lock);
	if (ret)
		goto lock_fail;

	kref_put(&mem->ref_count, free_handle_mem);

	ret = 0;

lock_fail:
	return ret;
}

static int free_handles(struct qaic_device *qdev,
			struct qaic_mem_req_entry *req, int count, u32 dbc_id)
{
	int ret = 0, ret_free, i;

	for (i = 0; i < count; i++) {
		ret_free = free_one_handle(qdev, &req[i], dbc_id);
		if (ret_free)
			ret = ret_free;
	}

	return ret;
}

static int map_one_alloc_handle(struct qaic_device *qdev,
				struct sg_table *sgt_h, struct sg_table **sgt_m,
				struct qaic_mem_req_entry *req)
{
	int total_len, len, nents, offf = 0, offl = 0;
	struct scatterlist *sg, *sgn, *sgf, *sgl;
	struct sg_table *sgt;
	int ret, j;
	u64 size;

	/* find out number of relevant nents needed for this mem */
	total_len = 0;
	sgf = NULL;
	sgl = NULL;
	nents = 0;
	size = req->size ? req->size : PAGE_SIZE;

	for (sg = sgt_h->sgl; sg; sg = sg_next(sg)) {
		len = sg->length;
		if (req->offset >= total_len &&
		    req->offset < total_len + len) {
			sgf = sg;
			offf = req->offset - total_len;
		}
		if (sgf)
			nents++;
		if (req->offset + size >= total_len &&
		    req->offset + size <= total_len + len) {
			sgl = sg;
			offl = req->offset + size - total_len;
			break;
		}
		total_len += len;
	}

	if (!sgf || !sgl) {
		ret = -EINVAL;
		goto out;
	}

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto out;
	}

	ret = sg_alloc_table(sgt, nents, GFP_KERNEL);
	if (ret)
		goto free_sgt;

	/* copy relevant sg node and fix page and length */
	sgn = sgf;
	for_each_sg(sgt->sgl, sg, nents, j) {
		memcpy(sg, sgn, sizeof(*sg));
		if (sgn == sgf)
			sg_set_page(sg, sg_page(sgn), sgn->length - offf, offf);
		else
			offf = 0;
		if (sgn == sgl) {
			sg_set_page(sg, sg_page(sgn), offl - offf, offf);
			sg_mark_end(sg);
			break;
		}
		sgn = sg_next(sgn);
	}

	nents = dma_map_sg(&qdev->pdev->dev, sgt->sgl, sgt->nents, req->dir);
	if (!nents) {
		ret = -EFAULT;
		goto free_table;
	}

	*sgt_m = sgt;

	return nents;

free_table:
	sg_free_table(sgt);
free_sgt:
	kfree(sgt);
out:
	return ret;
}

static int map_one_dma_handle(struct qaic_device *qdev,
			      struct sg_table *sgt_h, struct sg_table **sgt_m,
			      struct qaic_mem_req_entry *req)
{
	int total_len, len, nents, offf = 0, offl = 0;
	struct scatterlist *sg, *sgn, *sgf, *sgl;
	struct sg_table *sgt;
	int ret, j;
	u64 size;

	/* find out number of relevant nents needed for this mem */
	total_len = 0;
	sgf = NULL;
	sgl = NULL;
	nents = 0;
	size = req->size ? req->size : PAGE_SIZE;

	for (sg = sgt_h->sgl; sg; sg = sg_next(sg)) {
		len = sg_dma_len(sg);
		if (req->offset >= total_len &&
		    req->offset < total_len + len) {
			sgf = sg;
			offf = req->offset - total_len;
		}
		if (sgf)
			nents++;
		if (req->offset + size >= total_len &&
		    req->offset + size <= total_len + len) {
			sgl = sg;
			offl = req->offset + size - total_len;
			break;
		}
		total_len += len;
	}

	if (!sgf || !sgl) {
		ret = -EINVAL;
		goto out;
	}
	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto out;
	}

	ret = sg_alloc_table(sgt, nents, GFP_KERNEL);
	if (ret)
		goto free_sgt;

	/* copy relevant sg node and fix dma address and length */
	sgn = sgf;
	for_each_sg(sgt->sgl, sg, nents, j) {
		memcpy(sg, sgn, sizeof(*sg));
		if (sgn == sgf) {
			sg_dma_address(sg) += offf;
			sg_dma_len(sg) -= offf;
		} else {
			offf = 0;
		}
		if (sgn == sgl)
			sg_dma_len(sg) = offl - offf;
		sgn = sg_next(sgn);
	}

	*sgt_m = sgt;

	return nents;

free_sgt:
	kfree(sgt);
out:
	return ret;
}

static int encode_reqs(struct qaic_device *qdev, struct mem_handle *mem,
		       struct qaic_mem_req_entry *req)
{
	u8 cmd = BULK_XFER;
	u64 db_addr = cpu_to_le64(req->db_addr);
	u8 db_len;
	u32 db_data = cpu_to_le32(req->db_data);
	struct scatterlist *sg;
	u64 dev_addr;
	int presync_sem;
	int i;

	if (!mem->no_xfer)
		cmd |= (req->dir == DMA_TO_DEVICE ? INBOUND_XFER :
								OUTBOUND_XFER);

	if (req->db_len && !IS_ALIGNED(req->db_addr, req->db_len / 8))
		return -EINVAL;

	presync_sem = req->sem0.presync + req->sem1.presync +
		      req->sem2.presync + req->sem3.presync;
	if (presync_sem > 1)
		return -EINVAL;

	presync_sem = req->sem0.presync << 0 | req->sem1.presync << 1 |
		      req->sem2.presync << 2 | req->sem3.presync << 3;

	switch (req->db_len) {
	case 32:
		db_len = BIT(7);
		break;
	case 16:
		db_len = BIT(7) | 1;
		break;
	case 8:
		db_len = BIT(7) | 2;
		break;
	case 0:
		db_len = 0; /* doorbell is not active for this command */
		break;
	default:
		return -EINVAL; /* should never hit this */
	}

	/*
	 * When we end up splitting up a single request (ie a mem handle) into
	 * multiple DMA requests, we have to manage the sync data carefully.
	 * There can only be one presync sem.  That needs to be on every xfer
	 * so that the DMA engine doesn't transfer data before the receiver is
	 * ready.  We only do the doorbell and postsync sems after the xfer.
	 * To guarantee previous xfers for the request are complete, we use a
	 * fence.
	 */
	dev_addr = req->dev_addr;
	for_each_sg(mem->sgt->sgl, sg, mem->nents, i) {
		mem->reqs[i].cmd = cmd;
		mem->reqs[i].src_addr =
			cpu_to_le64(req->dir == DMA_TO_DEVICE ?
					sg_dma_address(sg) : dev_addr);
		mem->reqs[i].dest_addr =
			cpu_to_le64(req->dir == DMA_TO_DEVICE ?
					dev_addr : sg_dma_address(sg));
		mem->reqs[i].len = cpu_to_le32(sg_dma_len(sg));
		switch (presync_sem) {
		case BIT(0):
			mem->reqs[i].sem_cmd0 = cpu_to_le32(
						ENCODE_SEM(req->sem0.val,
							req->sem0.index,
							req->sem0.presync,
							req->sem0.cmd,
							req->sem0.flags));
			break;
		case BIT(1):
			mem->reqs[i].sem_cmd1 = cpu_to_le32(
						ENCODE_SEM(req->sem1.val,
							req->sem1.index,
							req->sem1.presync,
							req->sem1.cmd,
							req->sem1.flags));
			break;
		case BIT(2):
			mem->reqs[i].sem_cmd2 = cpu_to_le32(
						ENCODE_SEM(req->sem2.val,
							req->sem2.index,
							req->sem2.presync,
							req->sem2.cmd,
							req->sem2.flags));
			break;
		case BIT(3):
			mem->reqs[i].sem_cmd3 = cpu_to_le32(
						ENCODE_SEM(req->sem3.val,
							req->sem3.index,
							req->sem3.presync,
							req->sem3.cmd,
							req->sem3.flags));
			break;
		}
		dev_addr += sg_dma_len(sg);
	}
	/* add post transfer stuff to last segment */
	i--;
	mem->reqs[i].cmd |= GEN_COMPLETION;
	mem->reqs[i].db_addr = db_addr;
	mem->reqs[i].db_len = db_len;
	mem->reqs[i].db_data = db_data;
	req->sem0.flags |= (req->dir == DMA_TO_DEVICE ? SEM_INSYNCFENCE :
							SEM_OUTSYNCFENCE);
	mem->reqs[i].sem_cmd0 = cpu_to_le32(ENCODE_SEM(req->sem0.val,
						       req->sem0.index,
						       req->sem0.presync,
						       req->sem0.cmd,
						       req->sem0.flags));
	mem->reqs[i].sem_cmd1 = cpu_to_le32(ENCODE_SEM(req->sem1.val,
						       req->sem1.index,
						       req->sem1.presync,
						       req->sem1.cmd,
						       req->sem1.flags));
	mem->reqs[i].sem_cmd2 = cpu_to_le32(ENCODE_SEM(req->sem2.val,
						       req->sem2.index,
						       req->sem2.presync,
						       req->sem2.cmd,
						       req->sem2.flags));
	mem->reqs[i].sem_cmd3 = cpu_to_le32(ENCODE_SEM(req->sem3.val,
						       req->sem3.index,
						       req->sem3.presync,
						       req->sem3.cmd,
						       req->sem3.flags));

	return 0;
}

static int map_one_handle(struct qaic_device *qdev,
			  struct qaic_mem_req_entry *req, u32 dbc_id,
			  bool *prev_cont)
{
	struct dma_bridge_chan *dbc = &qdev->dbc[dbc_id];
	struct alloc_buf_handle *alloc_handle = NULL;
	struct dma_buf_handle *dma_handle = NULL;
	struct sg_table *sgt_h, *sgt = NULL;
	struct mem_handle *mem;
	int nents;
	int ret;

	ret = mutex_lock_interruptible(&dbc->handle_lock);
	if (ret)
		goto out;

	if (req->buf_fd != -1UL) {
		dma_handle = idr_find(&dbc->dma_handles, req->handle);
		sgt_h = dma_handle->sgt;
	} else {
		alloc_handle = idr_find(&dbc->alloc_handles, req->handle);
		sgt_h = alloc_handle->sgt;
	}
	mutex_unlock(&dbc->handle_lock);

	if (req->buf_fd != -1UL)
		nents = map_one_dma_handle(qdev, sgt_h, &sgt, req);
	else
		nents = map_one_alloc_handle(qdev, sgt_h, &sgt, req);

	if (nents < 0) {
		ret = nents;
		goto out;
	}

	if (req->dir == DMA_TO_DEVICE)
		dma_sync_sg_for_cpu(&qdev->pdev->dev, sgt->sgl, sgt->nents,
				    req->dir);

	mem = kmalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem) {
		ret = -ENOMEM;
		goto free_sgt;
	}

	mem->reqs = kcalloc(nents, sizeof(*mem->reqs), GFP_KERNEL);
	if (!mem->reqs) {
		ret = -ENOMEM;
		goto free_mem;
	}

	mem->no_xfer = !req->size;
	mem->sgt = sgt;
	mem->nents = nents;
	mem->dir = req->dir;
	mem->qdev = qdev;
	mem->queued = false;
	mem->dbc_id = dbc_id;
	init_completion(&mem->xfer_done);
	complete_all(&mem->xfer_done);

	ret = encode_reqs(qdev, mem, req);
	if (ret)
		goto free_req;

	ret = mutex_lock_interruptible(&dbc->mem_lock);
	if (ret)
		goto free_req;
	ret = idr_alloc(&dbc->mem_handles, mem, 1, 0, GFP_KERNEL);
	mutex_unlock(&dbc->mem_lock);
	if (ret < 0)
		goto free_req;

	req->handle = ret | (u64)dbc_id << PGOFF_DBC_SHIFT;
	/*
	 * When userspace uses the handle as the offset parameter to mmap,
	 * it needs to be in multiples of PAGE_SIZE.
	 */
	req->handle <<= PAGE_SHIFT;

	kref_init(&mem->ref_count);

	if (req->buf_fd != -1UL) {
		mem->export = false;
		mem->handle.dma = dma_handle;
		if (*prev_cont)
			kref_get(&dma_handle->ref_count);
	} else {
		mem->export = true;
		mem->handle.alloc = alloc_handle;
		if (*prev_cont)
			kref_get(&alloc_handle->ref_count);
	}
	*prev_cont = req->cont;

	return 0;

free_req:
	kfree(mem->reqs);
free_mem:
	kfree(mem);
free_sgt:
	sg_free_table(sgt);
	kfree(sgt);
	mutex_lock(&dbc->handle_lock);
	if (req->buf_fd != -1UL)
		kref_put(&dma_handle->ref_count, free_dma_handle);
	else
		kref_put(&alloc_handle->ref_count, free_alloc_handle);
	mutex_unlock(&dbc->handle_lock);
out:
	return ret;
}

static int map_handles(struct qaic_device *qdev, struct qaic_mem_req_entry *req,
		       int count, u32 dbc_id)
{
	int ret_free, ret, i, j;
	bool prev_cont = false;

	for (i = 0; i < count; i++) {
		ret = map_one_handle(qdev, &req[i], dbc_id, &prev_cont);
		if (ret)
			goto free_handle;
	}
	return 0;

free_handle:
	for (j = 0; j < i; j++) {
		ret_free = free_one_handle(qdev, &req[j], dbc_id);
		if (ret_free)
			ret = ret_free;
	}

	return ret;
}

static int alloc_one_sgt_handle(struct qaic_device *qdev,
				u32 dbc_id, u64 size,
				enum dma_data_direction dir)
{
	struct alloc_buf_handle *alloc_handle;
	struct scatterlist *sg;
	struct sg_table *sgt;
	struct page *page;
	int buf_extra;
	int max_order;
	int nr_pages;
	int ret = 0;
	int order;

	alloc_handle = kmalloc(sizeof(*alloc_handle), GFP_KERNEL);
	if (!alloc_handle) {
		ret = -ENOMEM;
		goto out;
	}

	if (size) {
		nr_pages = DIV_ROUND_UP(size, PAGE_SIZE);
		/*
		 * calculate how much extra we are going to allocate, to remove
		 * later
		 */
		buf_extra = (PAGE_SIZE - size % PAGE_SIZE) % PAGE_SIZE;
		max_order = min(MAX_ORDER, get_order(size));
	} else {
		/* allocate a single page for book keeping */
		nr_pages = 1;
		buf_extra = 0;
		max_order = 0;
	}

	sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto free_alloc_handle;
	}

	if (sg_alloc_table(sgt, nr_pages, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto free_sgt;
	}

	sg = sgt->sgl;
	sgt->nents = 0;

	/*
	 * Try to allocate enough pages to cover the request.  High order pages
	 * will be contiguous, which will be conducive to DMA.
	 */
	while (1) {
		order = min(fls(nr_pages) - 1, max_order);
		while (1) {
			page = alloc_pages(GFP_KERNEL | GFP_HIGHUSER |
					   __GFP_NOWARN | __GFP_ZERO |
					   (order ? __GFP_NORETRY :
							__GFP_RETRY_MAYFAIL),
					   order);
			if (page)
				break;
			if (!order--) {
				sg_set_page(sg, NULL, 0, 0);
				sg_mark_end(sg);
				ret = -ENOMEM;
				goto free_partial_alloc;
			}
			max_order = order;
		}

		ret = reserve_pages(page_to_pfn(page), 1 << order, true);
		if (ret)
			goto free_partial_alloc;

		sg_set_page(sg, page, PAGE_SIZE << order, 0);
		sgt->nents++;
		nr_pages -= 1 << order;
		if (!nr_pages) {
			if (buf_extra)
				sg_set_page(sg, page,
					    (PAGE_SIZE << order) - buf_extra,
					    0);
			sg_mark_end(sg);
			break;
		}
		sg = sg_next(sg);
	}

	ret = mutex_lock_interruptible(&qdev->dbc[dbc_id].handle_lock);
	if (ret)
		goto free_partial_alloc;
	ret = idr_alloc(&qdev->dbc[dbc_id].alloc_handles, alloc_handle, 1,
			0, GFP_KERNEL);
	mutex_unlock(&qdev->dbc[dbc_id].handle_lock);
	if (ret < 0)
		goto free_partial_alloc;

	alloc_handle->sgt = sgt;
	alloc_handle->handles = &qdev->dbc[dbc_id].alloc_handles;
	alloc_handle->handle = ret;
	kref_init(&alloc_handle->ref_count);

	return ret;

free_partial_alloc:
	for (sg = sgt->sgl; sg; sg = sg_next(sg))
		if (sg_page(sg)) {
			reserve_pages(page_to_pfn(sg_page(sg)),
				      DIV_ROUND_UP(sg->length, PAGE_SIZE),
				      false);
			__free_pages(sg_page(sg), get_order(sg->length));
		}
	sg_free_table(sgt);
free_sgt:
	kfree(sgt);
free_alloc_handle:
	kfree(alloc_handle);
out:
	return ret;

}

static int alloc_sgt(struct qaic_device *qdev, struct qaic_mem_req_entry *req,
		     int count, u32 dbc_id)
{
	struct alloc_buf_handle *alloc_handle;
	int i = 0, j = 0, k;
	int ret;

again:
	while (i < count && req[i++].cont);

	ret = alloc_one_sgt_handle(qdev, dbc_id, req[i-1].total_size,
				   req[i].dir);
	if (ret < 0)
		goto free_sgt_handle;

	while (j < i) {
		/* This is a temporary handle, will be overwritten */
		req[j].handle = ret;
		j++;
	}

	if (i < count)
		goto again;

	return 0;

free_sgt_handle:
	for (k = 0; k < j; k++) {
		if (!req[k].cont) {
			mutex_lock(&qdev->dbc[dbc_id].handle_lock);
			alloc_handle =
				idr_find(&qdev->dbc[dbc_id].alloc_handles,
					 req[k].handle);
			mutex_unlock(&qdev->dbc[dbc_id].handle_lock);
			kref_put(&alloc_handle->ref_count, free_alloc_handle);
		}
	}
	return ret;
}

static int alloc_dma_handle(struct qaic_device *qdev,
			    struct qaic_mem_req_entry *req,
			    u32 dbc_id)
{
	struct dma_bridge_chan *dbc = &qdev->dbc[dbc_id];
	struct dma_buf_handle *dma_handle;
	int ret;

	dma_handle = kzalloc(sizeof(*dma_handle), GFP_KERNEL);
	if (!dma_handle) {
		ret = -ENOMEM;
		goto out;
	}

	dma_handle->buf = dma_buf_get(req->buf_fd);
	if (IS_ERR(dma_handle->buf)) {
		ret = PTR_ERR(dma_handle->buf);
		goto free_dma_handle;
	}

	dma_handle->attach = dma_buf_attach(dma_handle->buf, &qdev->pdev->dev);
	if (IS_ERR(dma_handle->attach)) {
		ret = PTR_ERR(dma_handle->attach);
		goto buf_put;
	}

	dma_handle->sgt = dma_buf_map_attachment(dma_handle->attach, req->dir);
	if (IS_ERR(dma_handle->sgt)) {
		ret = PTR_ERR(dma_handle->sgt);
		goto buf_detach;
	}

	dma_handle->dir = req->dir;
	dma_handle->buf_fd = req->buf_fd;

	ret = idr_alloc(&dbc->dma_handles, dma_handle, req->buf_fd,
			req->buf_fd + 1, GFP_KERNEL);

	if (ret != req->buf_fd)
		goto buf_unmap;

	dma_handle->handles = &dbc->dma_handles;

	kref_init(&dma_handle->ref_count);

	return 0;

buf_unmap:
	dma_buf_unmap_attachment(dma_handle->attach, dma_handle->sgt, req->dir);
buf_detach:
	dma_buf_detach(dma_handle->buf, dma_handle->attach);
buf_put:
	dma_buf_put(dma_handle->buf);
free_dma_handle:
	kfree(dma_handle);
out:

	return ret;
}

static int find_sgt(struct qaic_device *qdev, struct qaic_mem_req_entry *req,
		    int count, u32 dbc_id)
{
	struct dma_bridge_chan *dbc = &qdev->dbc[dbc_id];
	struct dma_buf_handle *dma_handle;
	int ret, i, j;
	u64 buf_fd;

	ret = mutex_lock_interruptible(&dbc->handle_lock);
	if (ret)
		goto out;

	for (i = 0; i < count; i++) {
		dma_handle = idr_find(&dbc->dma_handles, req[i].buf_fd);

		if (!dma_handle) {
			ret = alloc_dma_handle(qdev, &req[i], dbc_id);
			if (ret)
				goto free_dma_handles;
		}
		/* This is a temporary handle, will be overwritten */
		req[i].handle = req[i].buf_fd;
	}
	mutex_unlock(&dbc->handle_lock);

	return 0;

free_dma_handles:
	buf_fd = -1UL;
	for (j = 0; j < i; j++) {
		if (buf_fd != req[i].buf_fd) {
			mutex_lock(&dbc->handle_lock);
			dma_handle = idr_find(&dbc->dma_handles, buf_fd);
			mutex_unlock(&dbc->handle_lock);
			kref_put(&dma_handle->ref_count, free_dma_handle);
			buf_fd = req[i].buf_fd;
		}
	}
out:
	return ret;
}

static int create_handles(struct qaic_device *qdev,
			  struct qaic_mem_req_entry *req, int count, u32 dbc_id)
{
	int ret = 0;

	if (req[0].buf_fd == -1UL)
		ret = alloc_sgt(qdev, req, count, dbc_id);
	else
		ret = find_sgt(qdev, req, count, dbc_id);

	return ret;
}

static bool invalid_sem(struct qaic_sem *sem)
{
	if (sem->val & ~SEM_VAL_MASK || sem->index & ~SEM_INDEX_MASK ||
	    !(sem->presync == 0 || sem->presync == 1) || sem->resv ||
	    sem->flags & ~(SEM_INSYNCFENCE | SEM_OUTSYNCFENCE) ||
	    sem->cmd > SEM_WAIT_GT_0)
		return true;
	return false;
}

static int validate_req(struct qaic_mem_req_entry *req, int count)
{
	enum dma_data_direction dir = req[0].dir;
	bool alloc_path = (req[0].handle == 0);
	bool export = (req[0].buf_fd == -1UL);
	u64 total_size = req[0].total_size;
	bool last = true;
	int i;

	for (i = 0; i < count; i++) {
		if (!(req[i].db_len == 32 || req[i].db_len == 16 ||
		    req[i].db_len == 8 || req[i].db_len == 0) ||
		    invalid_sem(&req[i].sem0) || invalid_sem(&req[i].sem1) ||
		    invalid_sem(&req[i].sem2) || invalid_sem(&req[i].sem3))
			return -EINVAL;

		if (!(req->dir == DMA_TO_DEVICE || req->dir == DMA_FROM_DEVICE))
			return -EINVAL;

		if (export && req[i].buf_fd != -1UL)
			return -EINVAL;

		if (!export && req[i].buf_fd == -1UL)
			return -EINVAL;

		if (alloc_path && req[i].handle)
			return -EINVAL;

		if (!alloc_path && !req[i].handle)
			return -EINVAL;

		if (!last && total_size != req[i].total_size)
			return -EINVAL;

		if (!last && dir != req[i].dir)
			return -EINVAL;

		if (last) {
			total_size = req[i].total_size;
			dir = req[i].dir;
		}
		last = !req[i].cont;
	}

	if (!last)
		return -EINVAL;

	return 0;
}

int qaic_mem_ioctl(struct qaic_device *qdev, struct qaic_user *usr,
		   unsigned long arg)
{
	struct qaic_mem_req_entry *req;
	struct qaic_mem_req_hdr *hdr;
	int ret = 0;
	u32 dbc_id;
	int rcu_id;
	int count;

	hdr = kmalloc(sizeof(*hdr), GFP_KERNEL);
	if (!hdr) {
		ret = -ENOMEM;
		goto out;
	}

	if (copy_from_user(hdr, (void __user *)arg, sizeof(*hdr))) {
		ret = -EFAULT;
		goto free_hdr;
	}

	count = hdr->count;

	if (count > ((sizeof(struct qaic_mem_req) - sizeof(*hdr)) /
			     sizeof(*req))) {
		ret = -EINVAL;
		goto free_hdr;
	}

	req = kcalloc(count, sizeof(*req), GFP_KERNEL);
	if (!req) {
		ret = -ENOMEM;
		goto free_hdr;
	}

	if (copy_from_user(req, (void __user *)(arg + sizeof(*hdr)),
			   sizeof(*req) * count)) {
		ret = -EFAULT;
		goto free_req;
	}

	dbc_id = hdr->dbc_id;

	if (dbc_id >= QAIC_NUM_DBC) {
		ret = -EINVAL;
		goto free_req;
	}

	rcu_id = srcu_read_lock(&qdev->dbc[dbc_id].ch_lock);

	if (!qdev->dbc[dbc_id].usr ||
			usr->handle != qdev->dbc[dbc_id].usr->handle) {
		ret = -EPERM;
		goto release_rcu;
	}

	ret = validate_req(req, count);
	if (ret)
		goto release_rcu;

	if (req[0].handle == 0) {
		ret = create_handles(qdev, req, count, dbc_id);
		if (ret)
			goto release_rcu;

		ret = map_handles(qdev, req, count, dbc_id);
		if (ret)
			goto release_rcu;

		ret = copy_to_user((void __user *)(arg + sizeof(*hdr)), &req[0],
				   sizeof(*req) * count);
		if (!ret)
			goto release_rcu;
		ret = -EFAULT;
	} else {
		ret = free_handles(qdev, req, count, dbc_id);
	}
release_rcu:
	srcu_read_unlock(&qdev->dbc[dbc_id].ch_lock, rcu_id);
free_req:
	kfree(req);
free_hdr:
	kfree(hdr);
out:
	return ret;
}

int qaic_data_mmap(struct qaic_device *qdev, struct qaic_user *usr,
		   struct vm_area_struct *vma)
{
	unsigned long offset = 0;
	struct mem_handle *mem;
	struct scatterlist *sg;
	int handle;
	int dbc_id;
	int rcu_id;
	int ret;

	dbc_id = (vma->vm_pgoff & PGOFF_DBC_MASK) >> PGOFF_DBC_SHIFT;
	handle = vma->vm_pgoff & ~PGOFF_DBC_MASK;

	if (dbc_id >= QAIC_NUM_DBC) {
		ret = -EINVAL;
		goto out;
	}

	rcu_id = srcu_read_lock(&qdev->dbc[dbc_id].ch_lock);
	if (!qdev->dbc[dbc_id].usr ||
	    usr->handle != qdev->dbc[dbc_id].usr->handle) {
		ret = -EPERM;
		goto release_rcu;
	}

	ret = mutex_lock_interruptible(&qdev->dbc[dbc_id].mem_lock);
	if (ret)
		goto release_rcu;
	mem = idr_find(&qdev->dbc[dbc_id].mem_handles, handle);
	mutex_unlock(&qdev->dbc[dbc_id].mem_lock);
	if (!mem) {
		ret = -ENODEV;
		goto release_rcu;
	}

	if (mem->no_xfer || !mem->export) {
		ret = -EINVAL;
		goto release_rcu;
	}

	for (sg = mem->handle.alloc->sgt->sgl; sg; sg = sg_next(sg)) {
		if (sg_page(sg)) {
			ret = remap_pfn_range(vma, vma->vm_start + offset,
					      page_to_pfn(sg_page(sg)),
					      sg->length, vma->vm_page_prot);
			if (ret)
				goto release_rcu;
			offset += sg->length;
		}
	}

release_rcu:
	srcu_read_unlock(&qdev->dbc[dbc_id].ch_lock, rcu_id);
out:
	return ret;
}

static inline int copy_exec_reqs(struct qaic_device *qdev,
				 struct mem_handle *mem, u32 dbc_id, u32 head,
				 u32 *ptail)
{
	struct dma_bridge_chan *dbc = &qdev->dbc[dbc_id];
	struct dbc_req *reqs = mem->reqs;
	u32 tail = *ptail;
	u32 avail;

	avail = head - tail;
	if (head <= tail)
		avail += dbc->nelem;

	--avail;

	if (avail < mem->nents)
		return -EAGAIN;

	if (tail + mem->nents > dbc->nelem) {
		avail = dbc->nelem - tail;
		avail = min_t(u32, avail, mem->nents);
		memcpy(dbc->req_q_base + tail * get_dbc_req_elem_size(),
		       reqs, sizeof(*reqs) * avail);
		reqs += avail;
		avail = mem->nents - avail;
		if (avail)
			memcpy(dbc->req_q_base, reqs, sizeof(*reqs) * avail);
	} else {
		memcpy(dbc->req_q_base + tail * get_dbc_req_elem_size(),
		       reqs, sizeof(*reqs) * mem->nents);
	}

	init_completion(&mem->xfer_done);
	list_add_tail(&mem->list, &dbc->xfer_list);
	*ptail = (tail + mem->nents) % dbc->nelem;

	return 0;
}

int qaic_execute_ioctl(struct qaic_device *qdev, struct qaic_user *usr,
		       unsigned long arg)
{
	struct qaic_execute_entry *exec;
	struct qaic_execute_hdr *hdr;
	struct mem_handle *mem;
	unsigned long flags;
	bool queued;
	u16 req_id;
	int handle;
	int dbc_id;
	int rcu_id;
	int count;
	u32 head;
	u32 tail;
	int ret;
	int i, j;

	hdr = kmalloc(sizeof(*hdr), GFP_KERNEL);
	if (!hdr) {
		ret = -ENOMEM;
		goto out;
	}

	if (copy_from_user(hdr, (void __user *)arg, sizeof(*hdr))) {
		ret = -EFAULT;
		goto free_hdr;
	}

	if (hdr->dbc_id > QAIC_NUM_DBC) {
		ret = -EINVAL;
		goto free_hdr;
	}

	count = hdr->count;

	if (count > ((sizeof(struct qaic_execute) - sizeof(*hdr)) /
			     sizeof(*exec))) {
		ret = -EINVAL;
		goto free_hdr;
	}

	exec = kcalloc(count, sizeof(*exec), GFP_KERNEL);
	if (!exec) {
		ret = -ENOMEM;
		goto free_hdr;
	}

	if (copy_from_user(exec, (void __user *)(arg + sizeof(*hdr)),
			   sizeof(*exec) * count)) {
		ret = -EFAULT;
		goto free_exec;
	}

	rcu_id = srcu_read_lock(&qdev->dbc[hdr->dbc_id].ch_lock);
	if (!qdev->dbc[hdr->dbc_id].usr ||
	    qdev->dbc[hdr->dbc_id].usr->handle != usr->handle) {
		ret = -EPERM;
		goto release_rcu;
	}

	ret = mutex_lock_interruptible(&qdev->dbc[hdr->dbc_id].mem_lock);
	if (ret)
		goto release_rcu;

	head = le32_to_cpu(__raw_readl(qdev->dbc[hdr->dbc_id].dbc_base +
			   REQHP_OFF));
	tail = le32_to_cpu(__raw_readl(qdev->dbc[hdr->dbc_id].dbc_base +
			   REQTP_OFF));

	if (head == U32_MAX || tail == U32_MAX) {
		/* PCI link error */
		ret = -ENODEV;
		goto unlock_mem_lock;
	}

	for (i = 0; i < count; i++) {
		handle = exec[i].handle & ~PGOFF_DBC_MASK;
		dbc_id = (exec[i].handle & PGOFF_DBC_MASK) >> PGOFF_DBC_SHIFT;

		/*
		 * we shifted up by PAGE_SHIFT to make mmap happy,
		 * need to undo that
		 */
		handle >>= PAGE_SHIFT;
		dbc_id >>= PAGE_SHIFT;

		if (dbc_id != hdr->dbc_id) {
			ret = -EINVAL;
			goto unlock_mem_lock;
		}

		mem = idr_find(&qdev->dbc[dbc_id].mem_handles, handle);
		if (!mem) {
			ret = -ENODEV;
			goto unlock_mem_lock;
		}
		/* prevent free_handle from taking the memory from under us */
		kref_get(&mem->ref_count);

		if (mem->dir != exec[i].dir) {
			ret = -EINVAL;
			kref_put(&mem->ref_count, free_handle_mem);
			goto unlock_mem_lock;
		}

		spin_lock_irqsave(&qdev->dbc[dbc_id].xfer_lock, flags);
		req_id = qdev->dbc[dbc_id].next_req_id++;
		queued = mem->queued;
		mem->queued = true;
		spin_unlock_irqrestore(&qdev->dbc[dbc_id].xfer_lock, flags);
		mem->req_id = req_id;
		for (j = 0; j < mem->nents; j++)
			mem->reqs[j].req_id = cpu_to_le16(req_id);

		if (queued) {
			ret = -EINVAL;
			kref_put(&mem->ref_count, free_handle_mem);
			goto unlock_mem_lock;
		}

		dma_sync_sg_for_device(&qdev->pdev->dev, mem->sgt->sgl,
				       mem->sgt->nents, mem->dir);

		spin_lock_irqsave(&qdev->dbc[dbc_id].xfer_lock, flags);
		ret = copy_exec_reqs(qdev, mem, dbc_id, head, &tail);
		spin_unlock_irqrestore(&qdev->dbc[dbc_id].xfer_lock, flags);
		if (ret) {
			mem->queued = false;
			kref_put(&mem->ref_count, free_handle_mem);
			goto sync_to_cpu;
		}
	}

	__raw_writel(cpu_to_le32(tail), qdev->dbc[hdr->dbc_id].dbc_base +
		     REQTP_OFF);

	goto unlock_mem_lock;

sync_to_cpu:
	dma_sync_sg_for_cpu(&qdev->pdev->dev, mem->sgt->sgl, mem->sgt->nents,
			    mem->dir);
	for (j = i - 1; j >= 0; j--) {
		mem = list_last_entry(&qdev->dbc[hdr->dbc_id].xfer_list,
				      struct mem_handle, list);
		dma_sync_sg_for_cpu(&qdev->pdev->dev, mem->sgt->sgl,
				    mem->sgt->nents, mem->dir);
		list_del(&mem->list);
	}
unlock_mem_lock:
	mutex_unlock(&qdev->dbc[hdr->dbc_id].mem_lock);
release_rcu:
	srcu_read_unlock(&qdev->dbc[hdr->dbc_id].ch_lock, rcu_id);
free_exec:
	kfree(exec);
free_hdr:
	kfree(hdr);
out:
	return ret;
}

irqreturn_t dbc_irq_handler(int irq, void *data)
{
	struct dma_bridge_chan *dbc = data;
	struct qaic_device *qdev = dbc->qdev;
	struct mem_handle *mem;
	struct mem_handle *i;
	struct dbc_rsp *rsp;
	unsigned long flags;
	int rcu_id;
	u16 status;
	u16 req_id;
	u32 head;
	u32 tail;

	rcu_id = srcu_read_lock(&dbc->ch_lock);
read_fifo:
	/*
	 * if this channel isn't assigned or gets unassigned during processing
	 * we have nothing further to do
	 */
	if (!dbc->usr) {
		srcu_read_unlock(&dbc->ch_lock, rcu_id);
		return IRQ_HANDLED;
	}

	head = le32_to_cpu(__raw_readl(dbc->dbc_base + RSPHP_OFF));
	tail = le32_to_cpu(__raw_readl(dbc->dbc_base + RSPTP_OFF));

	if (head == U32_MAX || tail == U32_MAX) {
		/* PCI link error */
		srcu_read_unlock(&dbc->ch_lock, rcu_id);
		return IRQ_HANDLED;
	}

	if (head == tail) { /* queue empty */
		srcu_read_unlock(&dbc->ch_lock, rcu_id);
		return IRQ_HANDLED;
	}

	while (head != tail) {
		rsp = dbc->rsp_q_base + head * sizeof(*rsp);
		req_id = le16_to_cpu(rsp->req_id);
		status = le16_to_cpu(rsp->status);
		if (status)
			pci_dbg(qdev->pdev, "req_id %d failed with status %d\n",
				req_id, status);
		spin_lock_irqsave(&dbc->xfer_lock, flags);
		list_for_each_entry_safe(mem, i, &dbc->xfer_list, list) {
			if (mem->req_id == req_id) {
				list_del(&mem->list);
				dma_sync_sg_for_cpu(&qdev->pdev->dev,
						    mem->sgt->sgl,
						    mem->sgt->nents,
						    mem->dir);
				mem->queued = false;
				complete_all(&mem->xfer_done);
				kref_put(&mem->ref_count, free_handle_mem);
				break;
			}
		}
		spin_unlock_irqrestore(&dbc->xfer_lock, flags);
		head = (head + 1) % dbc->nelem;
		__raw_writel(cpu_to_le32(head), dbc->dbc_base + RSPHP_OFF);
	}

	/* elements might have been put in the queue while we were processing */
	goto read_fifo;
}

int qaic_wait_exec_ioctl(struct qaic_device *qdev, struct qaic_user *usr,
			 unsigned long arg)
{
	struct mem_handle *mem;
	struct qaic_wait_exec *wait;
	unsigned int timeout;
	int handle;
	int dbc_id;
	int rcu_id;
	int ret;

	wait = kmalloc(sizeof(*wait), GFP_KERNEL);
	if (!wait) {
		ret = -ENOMEM;
		goto out;
	}

	if (copy_from_user(wait, (void __user *)arg, sizeof(*wait))) {
		ret = -EFAULT;
		goto free_wait;
	}

	if (wait->resv) {
		ret = -EINVAL;
		goto free_wait;
	}

	handle = wait->handle & ~PGOFF_DBC_MASK;
	dbc_id = (wait->handle & PGOFF_DBC_MASK) >> PGOFF_DBC_SHIFT;

	/* we shifted up by PAGE_SHIFT to make mmap happy, need to undo that */
	handle >>= PAGE_SHIFT;
	dbc_id >>= PAGE_SHIFT;

	if (dbc_id > QAIC_NUM_DBC) {
		ret = -EINVAL;
		goto free_wait;
	}

	rcu_id = srcu_read_lock(&qdev->dbc[dbc_id].ch_lock);
	if (!qdev->dbc[dbc_id].usr ||
	    qdev->dbc[dbc_id].usr->handle != usr->handle) {
		ret = -EPERM;
		goto release_rcu;
	}

	ret = mutex_lock_interruptible(&qdev->dbc[dbc_id].mem_lock);
	if (ret)
		goto release_rcu;
	mem = idr_find(&qdev->dbc[dbc_id].mem_handles, handle);
	mutex_unlock(&qdev->dbc[dbc_id].mem_lock);
	if (!mem) {
		ret = -ENODEV;
		goto release_rcu;
	}

	/* we don't want the mem handle freed under us in case of deactivate */
	kref_get(&mem->ref_count);
	srcu_read_unlock(&qdev->dbc[dbc_id].ch_lock, rcu_id);
	timeout = wait->timeout ? wait->timeout : wait_exec_default_timeout;
	ret = wait_for_completion_interruptible_timeout(&mem->xfer_done,
				msecs_to_jiffies(timeout));
	rcu_id = srcu_read_lock(&qdev->dbc[dbc_id].ch_lock);
	if (!ret)
		ret = -ETIMEDOUT;
	else if (ret > 0)
		ret = 0;
	if (!qdev->dbc[dbc_id].usr) {
		ret = -EPERM;
		goto release_rcu;
	}

	kref_put(&mem->ref_count, free_handle_mem);

release_rcu:
	srcu_read_unlock(&qdev->dbc[dbc_id].ch_lock, rcu_id);
free_wait:
	kfree(wait);
out:
	return ret;
}

int disable_dbc(struct qaic_device *qdev, u32 dbc_id, struct qaic_user *usr)
{
	if (!qdev->dbc[dbc_id].usr ||
	    qdev->dbc[dbc_id].usr->handle != usr->handle)
		return -EPERM;

	qdev->dbc[dbc_id].usr = NULL;
	synchronize_srcu(&qdev->dbc[dbc_id].ch_lock);
	return 0;
}

void wakeup_dbc(struct qaic_device *qdev, u32 dbc_id)
{
	struct mem_handle *mem;
	struct mem_handle *i;

	qdev->dbc[dbc_id].usr = NULL;
	synchronize_srcu(&qdev->dbc[dbc_id].ch_lock);
	list_for_each_entry_safe(mem, i, &qdev->dbc[dbc_id].xfer_list, list) {
		list_del(&mem->list);
		dma_sync_sg_for_cpu(&qdev->pdev->dev,
				    mem->sgt->sgl,
				    mem->sgt->nents,
				    mem->dir);
		complete_all(&mem->xfer_done);
	}
}

void release_dbc(struct qaic_device *qdev, u32 dbc_id)
{
	struct mem_handle *mem;
	int next_id = 0;

	wakeup_dbc(qdev, dbc_id);

	dma_free_coherent(&qdev->pdev->dev, qdev->dbc[dbc_id].total_size,
			  qdev->dbc[dbc_id].req_q_base,
			  qdev->dbc[dbc_id].dma_addr);
	qdev->dbc[dbc_id].total_size = 0;
	qdev->dbc[dbc_id].req_q_base = NULL;
	qdev->dbc[dbc_id].dma_addr = 0;
	qdev->dbc[dbc_id].nelem = 0;
	qdev->dbc[dbc_id].usr = NULL;
	while (1) {
		mem = idr_get_next(&qdev->dbc[dbc_id].mem_handles, &next_id);
		if (!mem)
			break;
		idr_remove(&qdev->dbc[dbc_id].mem_handles, next_id);
		/* account for the missing put from the irq handler */
		if (mem->queued) {
			mem->queued = false;
			kref_put(&mem->ref_count, free_handle_mem);
		}
		kref_put(&mem->ref_count, free_handle_mem);
	}
	qdev->dbc[dbc_id].in_use = false;
	wake_up(&qdev->dbc[dbc_id].dbc_release);
}

void qaic_data_get_fifo_info(struct dma_bridge_chan *dbc, u32 *head, u32 *tail)
{
	if (!dbc || !head || !tail)
		return;

	*head = le32_to_cpu(__raw_readl(dbc->dbc_base + REQHP_OFF));
	*tail = le32_to_cpu(__raw_readl(dbc->dbc_base + REQTP_OFF));
}
