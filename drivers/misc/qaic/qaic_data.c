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

struct dma_buf_handle {
	int			buf_fd;
	struct dma_buf		*buf;
	struct dma_buf_attachment *attach;
	struct sg_table		*sgt;
	struct idr		*handles;
	int			dir;
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
	struct dma_buf_handle	*dma_buf;
	u32			dbc_id;
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

static int alloc_handle(struct qaic_device *qdev,
			struct qaic_mem_req_entry *req, u32 dbc_id)
{
	struct mem_handle *mem;
	struct scatterlist *sg;
	struct sg_table *sgt;
	struct page *page;
	int buf_extra;
	int max_order;
	int nr_pages;
	int order;
	int nents;
	int ret;

	if (!(req->dir == DMA_TO_DEVICE || req->dir == DMA_FROM_DEVICE)) {
		ret = -EINVAL;
		goto out;
	}

	mem = kmalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem) {
		ret = -ENOMEM;
		goto out;
	}

	if (req->size) {
		nr_pages = DIV_ROUND_UP(req->size, PAGE_SIZE);
		/*
		 * calculate how much extra we are going to allocate, to remove
		 * later
		 */
		buf_extra = (PAGE_SIZE - req->size % PAGE_SIZE) % PAGE_SIZE;
		max_order = min(MAX_ORDER, get_order(req->size));
		mem->no_xfer = false;
	} else {
		/* allocate a single page for book keeping */
		nr_pages = 1;
		buf_extra = 0;
		max_order = 0;
		mem->no_xfer = true;
	}

	sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto free_mem;
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

	nents = dma_map_sg(&qdev->pdev->dev, sgt->sgl, sgt->nents, req->dir);
	if (!nents) {
		ret = -EFAULT;
		goto free_partial_alloc;
	}

	if (req->dir == DMA_TO_DEVICE)
		dma_sync_sg_for_cpu(&qdev->pdev->dev, sgt->sgl, sgt->nents,
				    req->dir);

	mem->reqs = kcalloc(nents, sizeof(*mem->reqs), GFP_KERNEL);
	if (!mem->reqs) {
		ret = -ENOMEM;
		goto req_alloc_fail;
	}

	mem->sgt = sgt;
	mem->nents = nents;
	mem->dir = req->dir;
	mem->qdev = qdev;
	mem->queued = false;
	mem->dma_buf = NULL;
	mem->dbc_id = dbc_id;
	init_completion(&mem->xfer_done);
	complete_all(&mem->xfer_done);

	ret = encode_reqs(qdev, mem, req);
	if (ret)
		goto encode_req_fail;

	ret = mutex_lock_interruptible(&qdev->dbc[dbc_id].mem_lock);
	if (ret)
		goto encode_req_fail;
	ret = idr_alloc(&qdev->dbc[dbc_id].mem_handles, mem, 1, 0,
		       GFP_KERNEL);
	mutex_unlock(&qdev->dbc[dbc_id].mem_lock);
	if (ret < 0)
		goto encode_req_fail;

	req->handle = ret | (u64)dbc_id << PGOFF_DBC_SHIFT;
	/*
	 * When userspace uses the handle as the offset parameter to mmap,
	 * it needs to be in multiples of PAGE_SIZE.
	 */
	req->handle <<= PAGE_SHIFT;

	kref_init(&mem->ref_count);

	return 0;

encode_req_fail:
	kfree(mem->reqs);
req_alloc_fail:
	dma_unmap_sg(&qdev->pdev->dev, sgt->sgl, sgt->nents, req->dir);
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
free_mem:
	kfree(mem);
out:
	return ret;
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

static int alloc_dma_handle(struct qaic_device *qdev,
			    struct qaic_mem_req_entry *req,
			    struct dma_buf_handle **handle, u32 dbc_id)
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
		goto free_handle;
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

	*handle = dma_handle;

	return 0;

buf_unmap:
	dma_buf_unmap_attachment(dma_handle->attach, dma_handle->sgt, req->dir);
buf_detach:
	dma_buf_detach(dma_handle->buf, dma_handle->attach);
buf_put:
	dma_buf_put(dma_handle->buf);
free_handle:
	kfree(dma_handle);
out:
	*handle = NULL;

	return ret;
}

static int map_handle(struct qaic_device *qdev, struct qaic_mem_req_entry *req,
		      u32 dbc_id)
{
	struct dma_bridge_chan *dbc = &qdev->dbc[dbc_id];
	struct scatterlist *sg, *sgn, *sgf, *sgl;
	int total_len, len, nents, offf = 0, offl = 0;
	struct dma_buf_handle *dma_handle;
	struct mem_handle *mem;
	struct sg_table *sgt;
	int ret, j;

	if (!(req->dir == DMA_TO_DEVICE || req->dir == DMA_FROM_DEVICE ||
	      req->dir == DMA_BIDIRECTIONAL)) {
		ret = -EINVAL;
		goto out;
	}

	ret = mutex_lock_interruptible(&dbc->dma_lock);
	if (ret)
		goto out;

	dma_handle = idr_find(&dbc->dma_handles, req->buf_fd);

	if (dma_handle) {
		kref_get(&dma_handle->ref_count);
	} else {
		ret = alloc_dma_handle(qdev, req, &dma_handle, dbc_id);
		if (ret) {
			mutex_unlock(&dbc->dma_lock);
			goto out;
		}
	}
	mutex_unlock(&dbc->dma_lock);

	/* find out number of relevant nents needed for this mem */
	total_len = 0;
	sgf = NULL;
	sgl = NULL;
	nents = 0;
	for (sg = dma_handle->sgt->sgl; sg; sg = sg_next(sg)) {
		len = sg_dma_len(sg);
		if (req->offset >= total_len &&
		    req->offset < total_len + len) {
			sgf = sg;
			offf = req->offset - total_len;
		}
		if (sgf)
			nents++;
		if (req->offset + req->size >= total_len &&
		    req->offset + req->size <= total_len + len) {
			sgl = sg;
			offl = req->offset + req->size - total_len;
			break;
		}
		total_len += len;
	}

	if (!sgf || !sgl) {
		ret = -EINVAL;
		goto put_dma_handle_ref;
	}

	mem = kmalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem) {
		ret = -ENOMEM;
		goto put_dma_handle_ref;
	}

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto free_mem;
	}

	ret = sg_alloc_table(sgt, nents, GFP_KERNEL);
	if (ret)
		goto free_sgt;

	/* copy relevant sg node and fix dma address, length if needed */
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

	if (req->dir == DMA_TO_DEVICE || req->dir == DMA_BIDIRECTIONAL)
		dma_sync_sg_for_cpu(&qdev->pdev->dev, sgt->sgl, sgt->nents,
				    req->dir);

	mem->reqs = kcalloc(nents, sizeof(*mem->reqs), GFP_KERNEL);
	if (!mem->reqs) {
		ret = -ENOMEM;
		goto free_table;
	}

	mem->no_xfer = false;
	mem->sgt = sgt;
	mem->nents = sgt->nents;
	mem->dir = req->dir;
	mem->qdev = qdev;
	mem->queued = false;
	mem->dma_buf = dma_handle;
	mem->dbc_id = dbc_id;
	init_completion(&mem->xfer_done);
	complete_all(&mem->xfer_done);

	ret = encode_reqs(qdev, mem, req);
	if (ret)
		goto free_req;

	ret = mutex_lock_interruptible(&dbc->mem_lock);
	if (ret)
		goto free_req;
	ret = idr_alloc(&dbc->mem_handles, mem, 1, 0,
		       GFP_KERNEL);
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

	return 0;

free_req:
	kfree(mem->reqs);
free_table:
	sg_free_table(sgt);
free_sgt:
	kfree(sgt);
free_mem:
	kfree(mem);
put_dma_handle_ref:
	mutex_lock(&dbc->dma_lock);
	kref_put(&dma_handle->ref_count, free_dma_handle);
	mutex_unlock(&dbc->dma_lock);
out:
	return ret;
}

static void free_handle_mem(struct kref *kref)
{
	struct mem_handle *mem = container_of(kref, struct mem_handle,
					      ref_count);
	struct scatterlist *sg;
	struct sg_table *sgt;

	sgt = mem->sgt;
	if (mem->dma_buf) {
		mutex_lock(&mem->qdev->dbc[mem->dbc_id].dma_lock);
		kref_put(&mem->dma_buf->ref_count, free_dma_handle);
		mutex_unlock(&mem->qdev->dbc[mem->dbc_id].dma_lock);
	} else {
		dma_unmap_sg(&mem->qdev->pdev->dev, sgt->sgl, sgt->nents, mem->dir);
		for (sg = sgt->sgl; sg; sg = sg_next(sg))
			if (sg_page(sg)) {
				reserve_pages(page_to_pfn(sg_page(sg)),
					      DIV_ROUND_UP(sg->length,
					      PAGE_SIZE), false);
				__free_pages(sg_page(sg),
					     get_order(sg->length));
			}
	}
	sg_free_table(sgt);
	kfree(sgt);
	kfree(mem->reqs);
	kfree(mem);
}

static int free_handle(struct qaic_device *qdev, struct qaic_mem_req_entry *req,
		       u32 req_dbc_id)
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

static bool invalid_sem(struct qaic_sem *sem)
{
	if (sem->val & ~SEM_VAL_MASK || sem->index & ~SEM_INDEX_MASK ||
	    !(sem->presync == 0 || sem->presync == 1) || sem->resv ||
	    sem->flags & ~(SEM_INSYNCFENCE | SEM_OUTSYNCFENCE) ||
	    sem->cmd > SEM_WAIT_GT_0)
		return true;
	return false;
}

int qaic_mem_ioctl(struct qaic_device *qdev, struct qaic_user *usr,
		   unsigned long arg)
{
	struct qaic_mem_req_entry *req;
	struct qaic_mem_req_hdr *hdr;
	bool alloc_path = false;
	bool free_path = false;
	int ret = 0, ret_free;
	u32 dbc_id;
	int rcu_id;
	int count;
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

	for (i = 0; i < count; i++) {
		if (req[i].resv || !(req[i].db_len == 32 ||
		    req[i].db_len == 16 || req[i].db_len == 8 ||
		    req[i].db_len == 0) || invalid_sem(&req[i].sem0) ||
		    invalid_sem(&req[i].sem1) || invalid_sem(&req[i].sem2) ||
		    invalid_sem(&req[i].sem3)) {
			ret = -EINVAL;
			goto free_handles;
		}

		if (!req[i].handle) {
			alloc_path = true;
			if (free_path) {
				ret = -EINVAL;
				i--;
				goto free_handles;
			}
			if (req[i].buf_fd != -1ULL)
				ret = map_handle(qdev, &req[i], dbc_id);
			else
				ret = alloc_handle(qdev, &req[i], dbc_id);
			if (ret)
				goto free_handles;
		} else {
			free_path = true;
			if (alloc_path) {
				ret = -EINVAL;
				i--;
				goto free_handles;
			}
			ret_free = free_handle(qdev, &req[i], dbc_id);
			if (ret_free)
				ret = ret_free;
		}
	}

	if (alloc_path) {
		ret = copy_to_user((void __user *)(arg + sizeof(*hdr)), &req[0],
			     sizeof(*req) * count);
		if (!ret)
			goto release_rcu;
		ret = -EFAULT;
	} else {
		goto release_rcu;
	}

free_handles:
	if (alloc_path) {
		for (j = 0; j <= i; j++)
			free_handle(qdev, &req[j], dbc_id);
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

	if (mem->no_xfer || mem->dma_buf) {
		ret = -EINVAL;
		goto release_rcu;
	}

	for (sg = mem->sgt->sgl; sg; sg = sg_next(sg)) {
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

static int commit_execute(struct qaic_device *qdev, struct mem_handle *mem,
			  u32 dbc_id)
{
	struct dma_bridge_chan *dbc = &qdev->dbc[dbc_id];
	u32 head = le32_to_cpu(__raw_readl(dbc->dbc_base + REQHP_OFF));
	u32 tail = le32_to_cpu(__raw_readl(dbc->dbc_base + REQTP_OFF));
	u32 avail = head - tail;
	struct dbc_req *reqs = mem->reqs;
	bool two_copy = tail + mem->nents > dbc->nelem;

	if (head == U32_MAX || tail == U32_MAX)
		/* PCI link error */
		return -ENODEV;

	if (head <= tail)
		avail += dbc->nelem;

	--avail;

	if (avail < mem->nents)
		return -EAGAIN;

	if (two_copy) {
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
	tail = (tail + mem->nents) % dbc->nelem;
	__raw_writel(cpu_to_le32(tail), dbc->dbc_base + REQTP_OFF);
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
			goto release_rcu;
		}

		ret = mutex_lock_interruptible(&qdev->dbc[dbc_id].mem_lock);
		if (ret)
			goto release_rcu;
		mem = idr_find(&qdev->dbc[dbc_id].mem_handles, handle);
		if (!mem) {
			ret = -ENODEV;
			mutex_unlock(&qdev->dbc[dbc_id].mem_lock);
			goto release_rcu;
		}
		/* prevent free_handle from taking the memory from under us */
		kref_get(&mem->ref_count);
		mutex_unlock(&qdev->dbc[dbc_id].mem_lock);

		if (mem->dir != exec[i].dir) {
			ret = -EINVAL;
			kref_put(&mem->ref_count, free_handle_mem);
			goto release_rcu;
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
			goto release_rcu;
		}

		dma_sync_sg_for_device(&qdev->pdev->dev, mem->sgt->sgl,
				       mem->sgt->nents, mem->dir);

		spin_lock_irqsave(&qdev->dbc[dbc_id].xfer_lock, flags);
		ret = commit_execute(qdev, mem, dbc_id);
		spin_unlock_irqrestore(&qdev->dbc[dbc_id].xfer_lock, flags);
		if (ret) {
			mem->queued = false;
			kref_put(&mem->ref_count, free_handle_mem);
			goto sync_to_cpu;
		}
	}

	goto release_rcu;

sync_to_cpu:
	dma_sync_sg_for_cpu(&qdev->pdev->dev, mem->sgt->sgl, mem->sgt->nents,
			    mem->dir);
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
