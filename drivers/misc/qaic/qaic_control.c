// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2019-2020, The Linux Foundation. All rights reserved. */

#include <asm/byteorder.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mhi.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include <linux/sched/signal.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <uapi/misc/qaic.h>

#include "qaic.h"
#include "qaic_trace.h"

#define MANAGE_MAGIC_NUMBER	   0x43494151 /* "QAIC" in little endian */
#define QAIC_DBC_Q_GAP		   0x100
#define QAIC_DBC_Q_BUF_ALIGN	   0x1000
#define RESP_TIMEOUT		   60 * HZ
#define QAIC_MANAGE_EXT_MSG_LENGTH SZ_64K /* Max DMA message length */
#define QAIC_WRAPPER_MAX_SIZE      SZ_4K
#define QAIC_MHI_RETRY_WAIT_MS	   100
#define QAIC_MHI_RETRY_MAX	   20

/*
 * wire encoding structures for the manage protocol.
 * All fields are little endian on the wire
 */
struct _msg_hdr {
	u32 magic_number;
	u32 sequence_number;
	u32 len; /* length of this message */
	u32 count; /* number of transactions in this message */
	u32 handle; /* unique id to track the resources consumed */
} __packed;

struct _msg {
	struct _msg_hdr hdr;
	u8 data[];
} __packed;

struct _trans_hdr {
	u32 type;
	u32 len;
} __packed;

/* Each message sent from driver to device are organized in a list of wrapper_msg */
struct wrapper_msg {
	struct list_head list;
	struct kref ref_count;
	u32 len; /* length of data to transfer */
	struct wrapper_list *head;
	union {
		struct _msg msg;
		struct _trans_hdr trans;
	};
};

struct wrapper_list {
	struct list_head list;
	spinlock_t lock;
};

struct _trans_passthrough {
	struct _trans_hdr hdr;
	u8 data[];
} __packed;

struct _addr_size_pair {
	u64 addr;
	u64 size;
} __packed;

struct _trans_dma_xfer {
	struct _trans_hdr hdr;
	u32 tag;
	u32 count;
	u32 dma_chunk_id;
	struct _addr_size_pair data[];
} __packed;

/* Initiated by device to continue the DMA xfer of a large piece of data */
struct _trans_dma_xfer_cont {
	struct _trans_hdr hdr;
	u32 dma_chunk_id;
	u64 xferred_size;
} __packed;

struct _trans_activate_to_dev {
	struct _trans_hdr hdr;
	u32 buf_len;
	u64 req_q_addr;
	u32 req_q_size;
	u64 rsp_q_addr;
	u32 rsp_q_size;
	u32 reserved;
} __packed;

struct _trans_activate_from_dev {
	struct _trans_hdr hdr;
	u32 status;
	u32 dbc_id;
} __packed;

struct _trans_deactivate_from_dev {
	struct _trans_hdr hdr;
	u32 status;
	u32 dbc_id;
} __packed;

struct _trans_terminate_to_dev {
	struct _trans_hdr hdr;
	u32 handle;
} __packed;

struct _trans_terminate_from_dev {
	struct _trans_hdr hdr;
	u32 status;
} __packed;

struct _trans_status_to_dev {
	struct _trans_hdr hdr;
} __packed;

struct _trans_status_from_dev {
	struct _trans_hdr hdr;
	u16 major;
	u16 minor;
	u32 status;
} __packed;

struct xfer_queue_elem {
	struct list_head list;
	u32 seq_num;
	struct completion xfer_done;
	void *buf;
};

struct dma_xfer {
	struct list_head list;
	struct sg_table *sgt;
	struct page **page_list;
	unsigned long nr_pages;
};

struct ioctl_resources {
	struct list_head dma_xfers;
	void *buf;
	dma_addr_t dma_addr;
	u32 total_size;
	u32 nelem;
	void *rsp_q_base;
	u32 status;
	u32 dbc_id;
	u32 dma_chunk_id;
	u64 xferred_dma_size;
	void *trans_hdr;
};

struct resp_work {
	struct work_struct work;
	struct qaic_device *qdev;
	void *buf;
};

static void free_wrapper(struct kref *ref)
{
	struct wrapper_msg *wrapper = container_of(ref, struct wrapper_msg,
						   ref_count);

	list_del(&wrapper->list);
	kfree(wrapper);
}

static void save_dbc_buf(struct qaic_device *qdev,
			 struct ioctl_resources *resources,
			 struct qaic_user *usr)
{
	u32 dbc_id = resources->dbc_id;

	if (resources->buf) {
		wait_event_interruptible(qdev->dbc[dbc_id].dbc_release,
					 !qdev->dbc[dbc_id].in_use);
		qdev->dbc[dbc_id].req_q_base = resources->buf;
		qdev->dbc[dbc_id].rsp_q_base = resources->rsp_q_base;
		qdev->dbc[dbc_id].dma_addr = resources->dma_addr;
		qdev->dbc[dbc_id].total_size = resources->total_size;
		qdev->dbc[dbc_id].nelem = resources->nelem;
		qdev->dbc[dbc_id].usr = usr;
		qdev->dbc[dbc_id].in_use = true;
		resources->buf = 0;
	}
}

static void free_dbc_buf(struct qaic_device *qdev,
			 struct ioctl_resources *resources)
{
	if (resources->buf)
		dma_free_coherent(&qdev->pdev->dev, resources->total_size,
				  resources->buf, resources->dma_addr);
	resources->buf = 0;
}

static void free_dma_xfers(struct qaic_device *qdev,
			   struct ioctl_resources *resources)
{
	struct dma_xfer *xfer;
	struct dma_xfer *x;
	int i;

	list_for_each_entry_safe(xfer, x, &resources->dma_xfers, list) {
		dma_unmap_sg(&qdev->pdev->dev, xfer->sgt->sgl, xfer->sgt->nents,
			     DMA_TO_DEVICE);
		sg_free_table(xfer->sgt);
		kfree(xfer->sgt);
		for (i = 0; i < xfer->nr_pages; ++i)
			put_page(xfer->page_list[i]);
		kfree(xfer->page_list);
		list_del(&xfer->list);
		kfree(xfer);
	}
}

static struct wrapper_msg *add_wrapper(struct wrapper_list *wrappers, u32 size)
{
	struct wrapper_msg *w = kzalloc(size, GFP_KERNEL);
	if (!w)
		return NULL;
	list_add_tail(&w->list, &wrappers->list);
	kref_init(&w->ref_count);
	w->head = wrappers;
	return w;
}

static int encode_passthrough(struct qaic_device *qdev, void *trans,
			      struct wrapper_list *wrappers, u32 *user_len)
{
	struct qaic_manage_trans_passthrough *in_trans = trans;
	struct _trans_passthrough *out_trans;
	struct wrapper_msg *trans_wrapper;
	struct wrapper_msg *wrapper;
	struct _msg *msg;

	wrapper = list_first_entry(&wrappers->list, struct wrapper_msg, list);
	msg = &wrapper->msg;

	if (msg->hdr.len + in_trans->hdr.len > QAIC_MANAGE_EXT_MSG_LENGTH) {
		trace_encode_error(qdev, "passthrough trans exceeds msg len");
		return -ENOSPC;
	}

	trans_wrapper = add_wrapper(wrappers,
			            offsetof(struct wrapper_msg, trans) +
			            in_trans->hdr.len);
	if (!trans_wrapper) {
		trace_encode_error(qdev, "encode passthrough alloc fail");
		return -ENOMEM;
	}
	trans_wrapper->len = in_trans->hdr.len;
	out_trans = (struct _trans_passthrough *)&trans_wrapper->trans;

	memcpy(out_trans, in_trans, in_trans->hdr.len);
	msg->hdr.len += in_trans->hdr.len;
	msg->hdr.count++;
	*user_len += in_trans->hdr.len;
	out_trans->hdr.type = cpu_to_le32(TRANS_PASSTHROUGH_TO_DEV);
	out_trans->hdr.len = cpu_to_le32(out_trans->hdr.len);

	return 0;
}

static int encode_dma(struct qaic_device *qdev, void *trans,
		      struct wrapper_list *wrappers, u32 *user_len,
		      struct ioctl_resources *resources,
		      struct qaic_user *usr)
{
	struct qaic_manage_trans_dma_xfer *in_trans = trans;
	struct _trans_dma_xfer *out_trans;
	struct wrapper_msg *trans_wrapper;
	struct wrapper_msg *wrapper;
	struct _addr_size_pair *asp;
	unsigned long need_pages;
	struct scatterlist *last;
	struct page **page_list;
	unsigned long nr_pages;
	struct scatterlist *sg;
	struct wrapper_msg *w;
	struct dma_xfer *xfer;
	struct sg_table *sgt;
	unsigned int dma_len;
	u64 dma_chunk_len;
	struct _msg *msg;
	void *boundary;
	int nents_dma;
	int nents;
	u32 size;
	int ret;
	int i;

	wrapper = list_first_entry(&wrappers->list, struct wrapper_msg, list);
	msg = &wrapper->msg;

	if (msg->hdr.len > (UINT_MAX - QAIC_MANAGE_EXT_MSG_LENGTH)) {
		trace_encode_error(qdev, "msg hdr length too large");
		ret = -EINVAL;
		goto out;
	}

	/* There should be enough space to hold at least one ASP entry. */
	if (msg->hdr.len + sizeof(*out_trans) + sizeof(*asp) >
	    QAIC_MANAGE_EXT_MSG_LENGTH) {
		trace_encode_error(qdev, "no space left in msg");
		ret = -ENOMEM;
		goto out;
	}

	if (in_trans->addr + in_trans->size < in_trans->addr ||
	    !in_trans->size) {
		trace_encode_error(qdev, "dma trans addr range overflow or no size");
		ret = -EINVAL;
		goto out;
	}

	xfer = kmalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer) {
		trace_encode_error(qdev, "dma no mem for xfer");
		ret = -ENOMEM;
		goto out;
	}

	need_pages = PAGE_ALIGN(in_trans->size + offset_in_page(in_trans->addr) -
				resources->xferred_dma_size) >> PAGE_SHIFT;

	nr_pages = need_pages;

	while(1) {
		page_list = kmalloc_array(nr_pages, sizeof(*page_list),
				GFP_KERNEL | __GFP_NOWARN);
		if (!page_list) {
			nr_pages = nr_pages/2;
			if (!nr_pages) {
				trace_encode_error(qdev, "dma page list alloc fail");
				ret = -ENOMEM;
				goto free_resource;
			}
		} else {
			break;
		}
	}

	ret = get_user_pages_fast(in_trans->addr + resources->xferred_dma_size,
				  nr_pages, 0, page_list);
	if (ret < 0 || ret != nr_pages) {
		trace_encode_error(qdev, "dma get user pages fail");
		ret = -EFAULT;
		goto free_page_list;
	}

	sgt = kmalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		trace_encode_error(qdev, "dma sgt alloc fail");
		ret = -ENOMEM;
		goto put_pages;
	}

	ret = sg_alloc_table_from_pages(sgt, page_list, nr_pages,
					offset_in_page(in_trans->addr + resources->xferred_dma_size),
					in_trans->size - resources->xferred_dma_size, GFP_KERNEL);
	if (ret) {
		trace_encode_error(qdev, "dma alloc table from pages fail");
		ret = -ENOMEM;
		goto free_sgt;
	}

	nents = dma_map_sg(&qdev->pdev->dev, sgt->sgl, sgt->nents,
			   DMA_TO_DEVICE);
	if (!nents) {
		trace_encode_error(qdev, "dma mapping failed");
		ret = -EFAULT;
		goto free_table;
	}

	/*
	 * It turns out several of the iommu drivers don't combine adjacent
	 * regions, which is really what we expect based on the description of
	 * dma_map_sg(), so lets see if that can be done.  It makes our message
	 * more efficent.
	 */
	last = sgt->sgl;
	nents_dma = nents;
	size = QAIC_MANAGE_EXT_MSG_LENGTH - msg->hdr.len - sizeof(*out_trans);
	for_each_sg(sgt->sgl, sg, nents, i) {
		if (sg_dma_address(last) + sg_dma_len(last) !=
		    sg_dma_address(sg)) {
			size -= sizeof(*asp);
			/* Save 1K for possible follow-up transactions. */
			if (size < SZ_1K) {
				nents_dma = i;
				break;
			}
		}
		last = sg;
	}

	trans_wrapper = add_wrapper(wrappers, QAIC_WRAPPER_MAX_SIZE);
	if (!trans_wrapper) {
		trace_encode_error(qdev, "encode dma alloc wrapper fail");
		ret = -ENOMEM;
		goto dma_unmap;
	}
	out_trans = (struct _trans_dma_xfer *)&trans_wrapper->trans;

	asp = out_trans->data;
	boundary = (void *)trans_wrapper + QAIC_WRAPPER_MAX_SIZE;
	size = 0;

	last = sgt->sgl;
	dma_len = 0;
	w = trans_wrapper;
	dma_chunk_len = 0;
	/* Adjecent DMA entries could be stitched together. */
	for_each_sg(sgt->sgl, sg, nents_dma, i) {
		/* hit a discontinuity, finalize segment and start new one */
		if (sg_dma_address(last) + sg_dma_len(last) !=
		    sg_dma_address(sg)) {
			asp->size = cpu_to_le64(dma_len);
			dma_chunk_len += dma_len;
			if (dma_len) {
				asp++;
				if ((void *)asp + sizeof(*asp) > boundary) {
					w->len =(void *)asp - (void *)&w->msg;
					size += w->len;
					w = add_wrapper(wrappers,
							QAIC_WRAPPER_MAX_SIZE);
				        if (!w) {
						trace_encode_error(qdev, "encode dma wrapper alloc fail");
						ret = -ENOMEM;
						goto dma_unmap;
					}
					boundary = (void *)w +
						   QAIC_WRAPPER_MAX_SIZE;
					asp = (struct _addr_size_pair *)&w->msg;
				}
			}
			dma_len = 0;
			asp->addr = cpu_to_le64(sg_dma_address(sg));
		}
		dma_len += sg_dma_len(sg);
		last = sg;
	}
	/* finalize the last segment */
	asp->size = cpu_to_le64(dma_len);
	w->len = (void *)asp + sizeof(*asp) - (void *)&w->msg;
	size += w->len;

	msg->hdr.len += size;
	msg->hdr.count++;

	out_trans->hdr.type = cpu_to_le32(TRANS_DMA_XFER_TO_DEV);
	out_trans->hdr.len = cpu_to_le32(size);
	out_trans->tag = cpu_to_le32(in_trans->tag);
	out_trans->count = cpu_to_le32((size - sizeof(*out_trans))/
			               sizeof(*asp));
	dma_chunk_len += dma_len;

	*user_len += in_trans->hdr.len;

	if (resources->dma_chunk_id)
		out_trans->dma_chunk_id = cpu_to_le32(resources->dma_chunk_id);
	else if (need_pages > nr_pages || nents_dma < nents) {
		while (resources->dma_chunk_id == 0)
			resources->dma_chunk_id =
				atomic_inc_return(&usr->chunk_id);

		out_trans->dma_chunk_id = cpu_to_le32(resources->dma_chunk_id);
	}
	resources->xferred_dma_size += dma_chunk_len;
	resources->trans_hdr = trans;

	xfer->sgt = sgt;
	xfer->page_list = page_list;
	xfer->nr_pages = nr_pages;
	list_add(&xfer->list, &resources->dma_xfers);
	return 0;

dma_unmap:
	dma_unmap_sg(&qdev->pdev->dev, sgt->sgl, sgt->nents, DMA_TO_DEVICE);
free_table:
	sg_free_table(sgt);
free_sgt:
	kfree(sgt);
put_pages:
	for (i = 0; i < nr_pages; ++i)
		put_page(page_list[i]);
free_page_list:
	kfree(page_list);
free_resource:
	kfree(xfer);
out:
	return ret;
}

static int encode_activate(struct qaic_device *qdev, void *trans,
			   struct wrapper_list *wrappers,
			   u32 *user_len,
			   struct ioctl_resources *resources)
{
	struct qaic_manage_trans_activate_to_dev *in_trans = trans;
	struct _trans_activate_to_dev *out_trans;
	struct wrapper_msg *trans_wrapper;
	struct wrapper_msg *wrapper;
	dma_addr_t dma_addr;
	struct _msg *msg;
	void *buf;
	u32 nelem;
	u32 size;
	int ret;

	wrapper = list_first_entry(&wrappers->list, struct wrapper_msg, list);
	msg = &wrapper->msg;

	if (msg->hdr.len + sizeof(*out_trans) > QAIC_MANAGE_MAX_MSG_LENGTH) {
		trace_encode_error(qdev, "activate trans exceeds msg len");
		return -ENOSPC;
	}

	if (!in_trans->queue_size) {
		trace_encode_error(qdev, "activate unspecified queue size");
		return -EINVAL;
	}

	if (in_trans->resv) {
		trace_encode_error(qdev, "activate non-zero resv");
		return -EINVAL;
	}

	nelem = in_trans->queue_size;
	size = (get_dbc_req_elem_size() + get_dbc_rsp_elem_size()) * nelem;
	if (size / nelem != get_dbc_req_elem_size() + get_dbc_rsp_elem_size()) {
		trace_encode_error(qdev, "activate queue size overflow");
		return -EINVAL;
	}

	if (size + QAIC_DBC_Q_GAP + QAIC_DBC_Q_BUF_ALIGN < size) {
		trace_encode_error(qdev, "activate queue size align overflow");
		return -EINVAL;
	}

	size = ALIGN((size + QAIC_DBC_Q_GAP), QAIC_DBC_Q_BUF_ALIGN);

	buf = dma_alloc_coherent(&qdev->pdev->dev, size, &dma_addr, GFP_KERNEL);
	if (!buf) {
		trace_encode_error(qdev, "activate queue alloc fail");
		return -ENOMEM;
	}

	trans_wrapper = add_wrapper(wrappers,
				    offsetof(struct wrapper_msg, trans) +
			            sizeof(*out_trans));
	if (!trans_wrapper) {
		trace_encode_error(qdev, "encode activate alloc fail");
		ret = -ENOMEM;
		goto free_dma;
	}
	trans_wrapper->len = sizeof(*out_trans);
	out_trans = (struct _trans_activate_to_dev *)&trans_wrapper->trans;

	out_trans->hdr.type = cpu_to_le32(TRANS_ACTIVATE_TO_DEV);
	out_trans->hdr.len = cpu_to_le32(sizeof(*out_trans));
	out_trans->buf_len = cpu_to_le32(size);
	out_trans->req_q_addr = cpu_to_le64(dma_addr);
	out_trans->req_q_size = cpu_to_le32(nelem);
	out_trans->rsp_q_addr = cpu_to_le64(dma_addr + size - nelem *
							get_dbc_rsp_elem_size());
	out_trans->rsp_q_size = cpu_to_le32(nelem);

	*user_len += in_trans->hdr.len;
	msg->hdr.len += sizeof(*out_trans);
	msg->hdr.count++;

	resources->buf = buf;
	resources->dma_addr = dma_addr;
	resources->total_size = size;
	resources->nelem = nelem;
	resources->rsp_q_base = buf + size - nelem * get_dbc_rsp_elem_size();
	return 0;

free_dma:
	buf = dma_alloc_coherent(&qdev->pdev->dev, size, buf, dma_addr);
	return ret;
}

static int encode_deactivate(struct qaic_device *qdev, void *trans,
			     u32 *user_len, struct qaic_user *usr)
{
	struct qaic_manage_trans_deactivate *in_trans = trans;

	if (in_trans->dbc_id >= QAIC_NUM_DBC || in_trans->resv) {
		trace_encode_error(qdev, "deactivate invalid dbc id or resv not zero");
		return -EINVAL;
	}

	*user_len += in_trans->hdr.len;

	return disable_dbc(qdev, in_trans->dbc_id, usr);
}

static int encode_status(struct qaic_device *qdev, void *trans,
			 struct wrapper_list *wrappers,
			 u32 *user_len)
{
	struct qaic_manage_trans_status_to_dev *in_trans = trans;
	struct _trans_status_to_dev *out_trans;
	struct wrapper_msg *trans_wrapper;
	struct wrapper_msg *wrapper;
	struct _msg *msg;

	wrapper = list_first_entry(&wrappers->list, struct wrapper_msg, list);
	msg = &wrapper->msg;

	if (msg->hdr.len + in_trans->hdr.len > QAIC_MANAGE_MAX_MSG_LENGTH) {
		trace_encode_error(qdev, "status trans exceeds msg len");
		return -ENOSPC;
	}

	trans_wrapper = add_wrapper(wrappers, sizeof(*trans_wrapper));
	if (!trans_wrapper) {
		trace_encode_error(qdev, "encode status alloc fail");
		return -ENOMEM;
	}
	trans_wrapper->len = sizeof(*out_trans);
	out_trans = (struct _trans_status_to_dev *)&trans_wrapper->trans;

	out_trans->hdr.type = cpu_to_le32(TRANS_STATUS_TO_DEV);
	out_trans->hdr.len = cpu_to_le32(in_trans->hdr.len);
	msg->hdr.len += in_trans->hdr.len;
	msg->hdr.count++;
	*user_len += in_trans->hdr.len;

	return 0;
}

static int encode_message(struct qaic_device *qdev,
			  struct qaic_manage_msg *user_msg,
			  struct wrapper_list *wrappers,
			  struct ioctl_resources *resources,
			  struct qaic_user *usr)
{
	struct qaic_manage_trans_hdr *trans_hdr;
	struct wrapper_msg *wrapper;
	struct _msg *msg;
	u32 user_len = 0;
	int ret;
	int i;

	wrapper = list_first_entry(&wrappers->list, struct wrapper_msg, list);
	msg = &wrapper->msg;

	msg->hdr.len = sizeof(msg->hdr);

	if (resources->dma_chunk_id) {
		ret = encode_dma(qdev, resources->trans_hdr, wrappers,
				 &user_len, resources, usr);
		msg->hdr.count = 1;
		goto out;
	}

	for (i = 0; i < user_msg->count; ++i) {
		if (user_len >= user_msg->len) {
			trace_encode_error(qdev, "msg exceeds len");
			ret = -EINVAL;
			break;
		}
		trans_hdr = (struct qaic_manage_trans_hdr *)
						(user_msg->data + user_len);
		if (user_len + trans_hdr->len > user_msg->len) {
			trace_encode_error(qdev, "trans exceeds msg len");
			ret = -EINVAL;
			break;
		}

		switch (trans_hdr->type) {
		case TRANS_PASSTHROUGH_FROM_USR:
			ret = encode_passthrough(qdev, trans_hdr, wrappers,
					         &user_len);
			break;
		case TRANS_DMA_XFER_FROM_USR:
			ret = encode_dma(qdev, trans_hdr, wrappers, &user_len,
					 resources, usr);
			break;
		case TRANS_ACTIVATE_FROM_USR:
			ret = encode_activate(qdev, trans_hdr, wrappers,
					      &user_len, resources);
			break;
		case TRANS_DEACTIVATE_FROM_USR:
			ret = encode_deactivate(qdev, trans_hdr, &user_len,
					        usr);
			break;
		case TRANS_STATUS_FROM_USR:
			ret = encode_status(qdev, trans_hdr, wrappers,
					    &user_len);
			break;
		default:
			trace_encode_error(qdev, "unknown trans");
			ret = -EINVAL;
			break;
		}

		if (ret)
			break;
	}

	if (user_len != user_msg->len) {
		trace_encode_error(qdev, "msg processed exceeds len");
		ret = -EINVAL;
	}
out:
	if (ret) {
		free_dma_xfers(qdev, resources);
		free_dbc_buf(qdev, resources);
		return ret;
	}

	return 0;
}

static int decode_passthrough(struct qaic_device *qdev, void *trans,
			      struct qaic_manage_msg *user_msg, u32 *msg_len)
{
	struct _trans_passthrough *in_trans = trans;
	struct qaic_manage_trans_passthrough *out_trans;
	u32 len;

	out_trans = (void *)user_msg->data + user_msg->len;

	len = le32_to_cpu(in_trans->hdr.len);
	if (user_msg->len + len > QAIC_MANAGE_MAX_MSG_LENGTH) {
		trace_decode_error(qdev, "passthrough trans exceeds msg len");
		return -ENOSPC;
	}

	memcpy(out_trans, in_trans, len);
	user_msg->len += len;
	*msg_len += len;
	out_trans->hdr.type = le32_to_cpu(out_trans->hdr.type);
	return 0;
}

static int decode_activate(struct qaic_device *qdev, void *trans,
			   struct qaic_manage_msg *user_msg, u32 *msg_len,
			   struct ioctl_resources *resources,
			   struct qaic_user *usr)
{
	struct _trans_activate_from_dev *in_trans = trans;
	struct qaic_manage_trans_activate_from_dev *out_trans;
	u32 len;

	out_trans = (void *)user_msg->data + user_msg->len;

	len = le32_to_cpu(in_trans->hdr.len);
	if (user_msg->len + len > QAIC_MANAGE_MAX_MSG_LENGTH) {
		trace_decode_error(qdev, "activate trans exceeds msg len");
		return -ENOSPC;
	}

	user_msg->len += len;
	*msg_len += len;
	out_trans->hdr.type = le32_to_cpu(in_trans->hdr.type);
	out_trans->hdr.len = len;
	out_trans->status = le32_to_cpu(in_trans->status);
	out_trans->dbc_id = le32_to_cpu(in_trans->dbc_id);

	if (!resources->buf) {
		trace_decode_error(qdev, "activate with no assigned resources");
		/* how did we get an activate response with a request? */
		return -EINVAL;
	}

	if (out_trans->dbc_id >= QAIC_NUM_DBC) {
		trace_decode_error(qdev, "activate invalid dbc id");
		/*
		 * The device assigned an invalid resource, which should never
		 * happen.  Inject an error so the user can try to recover.
		 */
		out_trans->status = -ENODEV;
	}

	resources->status = out_trans->status;
	resources->dbc_id = out_trans->dbc_id;
	if (!resources->status)
		save_dbc_buf(qdev, resources, usr);
	return 0;
}

static int decode_deactivate(struct qaic_device *qdev, void *trans,
			     u32 *msg_len)
{
	struct _trans_deactivate_from_dev *in_trans = trans;
	u32 dbc_id = le32_to_cpu(in_trans->dbc_id);
	u32 status = le32_to_cpu(in_trans->status);

	if (dbc_id >= QAIC_NUM_DBC) {
		trace_decode_error(qdev, "deactivate invalid dbc id");
		/*
		 * The device assigned an invalid resource, which should never
		 * happen.  Inject an error so the user can try to recover.
		 */
		return -ENODEV;
	}
	if (status) {
		trace_decode_error(qdev, "deactivate device failed");
		/*
		 * Releasing resources failed on the device side, which puts
		 * us in a bind since they may still be in use, so be safe and
		 * do nothing.
		 */
		return -ENODEV;
	}

	release_dbc(qdev, dbc_id);
	*msg_len += sizeof(*in_trans);
	return 0;
}

static int decode_status(struct qaic_device *qdev, void *trans,
			 struct qaic_manage_msg *user_msg, u32 *user_len)
{
	struct _trans_status_from_dev *in_trans = trans;
	struct qaic_manage_trans_status_from_dev *out_trans;
	u32 len;

	out_trans = (void *)user_msg->data + user_msg->len;

	len = le32_to_cpu(in_trans->hdr.len);
	if (user_msg->len + len > QAIC_MANAGE_MAX_MSG_LENGTH) {
		trace_decode_error(qdev, "status trans exceeds msg len");
		return -ENOSPC;
	}

	out_trans->hdr.type = le32_to_cpu(TRANS_STATUS_FROM_DEV);
	out_trans->hdr.len = len;
	out_trans->major = le32_to_cpu(in_trans->major);
	out_trans->minor = le32_to_cpu(in_trans->minor);
	*user_len += in_trans->hdr.len;
	user_msg->len += len;

	return 0;
}

static int decode_message(struct qaic_device *qdev,
			  struct qaic_manage_msg *user_msg, struct _msg *msg,
			  struct ioctl_resources *resources,
			  struct qaic_user *usr)
{
	struct _trans_hdr *trans_hdr;
	u32 msg_len = 0;
	int ret;
	int i;

	if (msg->hdr.len > QAIC_MANAGE_MAX_MSG_LENGTH) {
		trace_decode_error(qdev, "msg to decode len greater than size");
		return -EINVAL;
	}

	user_msg->len = 0;
	user_msg->count = le32_to_cpu(msg->hdr.count);

	for (i = 0; i < user_msg->count; ++i) {
		trans_hdr = (struct _trans_hdr *)(msg->data + msg_len);
		if (msg_len + trans_hdr->len > msg->hdr.len) {
			trace_decode_error(qdev, "trans len exceeds msg len");
			return -EINVAL;
		}

		switch (trans_hdr->type) {
		case TRANS_PASSTHROUGH_FROM_DEV:
			ret = decode_passthrough(qdev, trans_hdr, user_msg,
						 &msg_len);
			break;
		case TRANS_ACTIVATE_FROM_DEV:
			ret = decode_activate(qdev, trans_hdr, user_msg,
					      &msg_len, resources, usr);
			break;
		case TRANS_DEACTIVATE_FROM_DEV:
			ret = decode_deactivate(qdev, trans_hdr, &msg_len);
			break;
		case TRANS_STATUS_FROM_DEV:
			ret = decode_status(qdev, trans_hdr, user_msg,
					    &msg_len);
			break;
		default:
			trace_decode_error(qdev, "unknown trans type");
			return -EINVAL;
		}

		if (ret)
			return ret;
	}

	if (msg_len != (msg->hdr.len - sizeof(msg->hdr))) {
		trace_decode_error(qdev, "decoded msg ended up longer than final trans");
		return -EINVAL;
	}

	return 0;
}

static void *msg_xfer(struct qaic_device *qdev, struct wrapper_list *wrappers,
		      u32 seq_num, bool ignore_signal)
{
	struct xfer_queue_elem elem;
	struct wrapper_msg *w;
	struct _msg *out_buf;
	int retry_count;
	long ret;

	if (qdev->in_reset) {
		mutex_unlock(&qdev->cntl_mutex);
		return ERR_PTR(-ENODEV);
	}

	elem.seq_num = seq_num;
	elem.buf = NULL;
	init_completion(&elem.xfer_done);
	if (likely(!qdev->cntl_lost_buf)) {
		/*
		 * The max size of request to device is QAIC_MANAGE_EXT_MSG_LENGTH.
		 * The max size of response from device is QAIC_MANAGE_MAX_MSG_LENGTH.
		 */
		out_buf = kmalloc(QAIC_MANAGE_MAX_MSG_LENGTH, GFP_KERNEL);
		if (!out_buf) {
			mutex_unlock(&qdev->cntl_mutex);
			return ERR_PTR(-ENOMEM);
		}

		ret = mhi_queue_transfer(qdev->cntl_ch, DMA_FROM_DEVICE,
					 out_buf, QAIC_MANAGE_MAX_MSG_LENGTH,
					 MHI_EOT);
		if (ret) {
			mutex_unlock(&qdev->cntl_mutex);
			return ERR_PTR(ret);
		}
	} else {
		/*
		 * we lost a buffer because we queued a recv buf, but then
		 * queuing the corresponding tx buf failed.  To try to avoid
		 * a memory leak, lets reclaim it and use it for this
		 * transaction.
		 */
		qdev->cntl_lost_buf = false;
	}

	list_for_each_entry(w, &wrappers->list, list) {
		kref_get(&w->ref_count);
		retry_count = 0;
retry:
		ret = mhi_queue_transfer(qdev->cntl_ch, DMA_TO_DEVICE, &w->msg,
					 w->len,
					 list_is_last(&w->list,
						      &wrappers->list) ?
						MHI_EOT : MHI_CHAIN);
		if (ret) {
			if (ret == -EBUSY &&
			    retry_count++ < QAIC_MHI_RETRY_MAX) {
				msleep_interruptible(QAIC_MHI_RETRY_WAIT_MS);
				if (!signal_pending(current))
					goto retry;
			}

			qdev->cntl_lost_buf = true;
			kref_put(&w->ref_count, free_wrapper);
			mutex_unlock(&qdev->cntl_mutex);
			return ERR_PTR(ret);
		}
	}

	list_add_tail(&elem.list, &qdev->cntl_xfer_list);
	mutex_unlock(&qdev->cntl_mutex);

	if (ignore_signal)
		ret = wait_for_completion_timeout(&elem.xfer_done,
						  RESP_TIMEOUT);
	else
		ret = wait_for_completion_interruptible_timeout(&elem.xfer_done,
								RESP_TIMEOUT);
	/*
	 * not using _interruptable because we have to cleanup or we'll
	 * likely cause memory corruption
	 */
	mutex_lock(&qdev->cntl_mutex);
	if (!list_empty(&elem.list))
		list_del(&elem.list);
	if (!ret && !elem.buf)
		ret = -ETIMEDOUT;
	else if (ret > 0 && !elem.buf)
		ret = -EIO;
	mutex_unlock(&qdev->cntl_mutex);

	if (ret < 0) {
		kfree(elem.buf);
		return ERR_PTR(ret);
	}

	return elem.buf;
}

/* Add a transaction to abort the outstanding DMA continuation */
static int abort_dma_cont(struct qaic_device *qdev,
		          struct wrapper_list *wrappers, u32 dma_chunk_id)
{
	struct _trans_dma_xfer *out_trans;
	u32 size = sizeof(*out_trans);
	struct wrapper_msg *wrapper;
	struct wrapper_msg *w;
	struct _msg *msg;

	wrapper = list_first_entry(&wrappers->list, struct wrapper_msg, list);
	msg = &wrapper->msg;

	wrapper = add_wrapper(wrappers,
			      offsetof(struct wrapper_msg, trans) + sizeof(*out_trans));

	if (!wrapper) {
		trace_encode_error(qdev, "abort dma cont alloc fail");
		return -ENOMEM;
	}

	/* Remove all but the first wrapper which has the msg header */
	list_for_each_entry_safe(wrapper, w, &wrappers->list, list)
		if (!list_is_first(&wrapper->list, &wrappers->list))
			kref_put(&wrapper->ref_count, free_wrapper);

	out_trans = (struct _trans_dma_xfer *)&wrapper->trans;
	out_trans->hdr.type = cpu_to_le32(TRANS_DMA_XFER_TO_DEV);
	out_trans->hdr.len = cpu_to_le32(size);
	out_trans->tag = cpu_to_le32(0);
	out_trans->count = cpu_to_le32(0);
	out_trans->dma_chunk_id = cpu_to_le32(dma_chunk_id);

	msg->hdr.len = size + sizeof(*msg);
	msg->hdr.count = 1;
	wrapper->len = size;

	return 0;
}

static struct wrapper_list *alloc_wrapper_list(void)
{
	struct wrapper_list *wrappers;

	wrappers = kmalloc(sizeof(*wrappers), GFP_KERNEL);
	if (!wrappers)
		return NULL;
	INIT_LIST_HEAD(&wrappers->list);
	spin_lock_init(&wrappers->lock);

	return wrappers;
}

static int __qaic_manage(struct qaic_device *qdev, struct qaic_user *usr,
		         struct qaic_manage_msg *user_msg,
			 struct ioctl_resources *resources,
			 struct _msg **rsp)
{
	struct wrapper_list *wrappers;
	struct wrapper_msg *wrapper;
	struct wrapper_msg *w;
	bool all_done = false;
	struct _msg *msg;
	int ret;

	wrappers = alloc_wrapper_list();
	if (!wrappers) {
		trace_manage_error(qdev, usr, "unable to alloc wrappers");
		return -ENOMEM;
	}

	wrapper = add_wrapper(wrappers, sizeof(*wrapper));
	if (!wrapper) {
		trace_manage_error(qdev, usr, "__qaic_manage failed to add wrapper");
		kfree(wrappers);
		return -ENOMEM;
	}

	msg = &wrapper->msg;
	wrapper->len = sizeof(*msg);

	ret = encode_message(qdev, user_msg, wrappers, resources, usr);
	if (ret && resources->dma_chunk_id)
		ret = abort_dma_cont(qdev, wrappers, resources->dma_chunk_id);
	if (ret)
		goto encode_failed;

	ret = mutex_lock_interruptible(&qdev->cntl_mutex);
	if (ret)
		goto lock_failed;

	msg->hdr.magic_number = MANAGE_MAGIC_NUMBER;
	msg->hdr.sequence_number = cpu_to_le32(qdev->next_seq_num++);
	msg->hdr.len = cpu_to_le32(msg->hdr.len);
	msg->hdr.count = cpu_to_le32(msg->hdr.count);

	if (usr)
		msg->hdr.handle = cpu_to_le32(usr->handle);
	else
		msg->hdr.handle = 0;

	/* msg_xfer releases the mutex */
	*rsp = msg_xfer(qdev, wrappers, qdev->next_seq_num - 1, false);
	if (IS_ERR(*rsp)) {
		trace_manage_error(qdev, usr, "failed to xmit to device");
		ret = PTR_ERR(*rsp);
	}

lock_failed:
	free_dma_xfers(qdev, resources);
encode_failed:
	spin_lock(&wrappers->lock);
	list_for_each_entry_safe(wrapper, w, &wrappers->list, list)
		kref_put(&wrapper->ref_count, free_wrapper);
	all_done = list_empty(&wrappers->list);
	spin_unlock(&wrappers->lock);
	if (all_done)
		kfree(wrappers);

	return ret;
}

static int qaic_manage(struct qaic_device *qdev, struct qaic_user *usr,
		       struct qaic_manage_msg *user_msg)
{
	struct _trans_dma_xfer_cont *dma_cont = NULL;
	struct ioctl_resources resources;
	struct _msg *rsp;
	int ret;

	memset(&resources, 0, sizeof(struct ioctl_resources));

	INIT_LIST_HEAD(&resources.dma_xfers);

	if (user_msg->len > QAIC_MANAGE_MAX_MSG_LENGTH ||
	    user_msg->count > QAIC_MANAGE_MAX_MSG_LENGTH / sizeof(struct qaic_manage_trans_hdr)) {
		trace_manage_error(qdev, usr, "msg from userspace too long or too many transactions");
		return -EINVAL;
	}

dma_xfer_continue:
	ret = __qaic_manage(qdev, usr, user_msg, &resources, &rsp);
	if (ret)
		return ret;
	/* dma_cont should be the only transaction if present */
	if (rsp->hdr.count == 1) {
		dma_cont = (struct _trans_dma_xfer_cont *)rsp->data;
		if (dma_cont->hdr.type != TRANS_DMA_XFER_CONT)
			dma_cont = NULL;
	}
	if (dma_cont) {
		if (dma_cont->dma_chunk_id == resources.dma_chunk_id &&
		    dma_cont->xferred_size == resources.xferred_dma_size) {
			kfree(rsp);
			goto dma_xfer_continue;
		}

		trace_manage_error(qdev, usr, "wrong size/id for DMA continuation");
		ret = -EINVAL;
		goto dma_cont_failed;
	}

	ret = decode_message(qdev, user_msg, rsp, &resources, usr);

dma_cont_failed:
	free_dbc_buf(qdev, &resources);
	kfree(rsp);
	return ret;
}

int qaic_manage_ioctl(struct qaic_device *qdev, struct qaic_user *usr,
		      unsigned long arg)
{
	struct qaic_manage_msg *user_msg;
	int ret;

	user_msg = kzalloc(QAIC_MANAGE_MAX_MSG_LENGTH, GFP_KERNEL);
	if (!user_msg) {
		trace_manage_error(qdev, usr, "no mem for userspace message");
		ret = -ENOMEM;
		goto out;
	}

	if (copy_from_user(user_msg, (void __user *)arg, sizeof(*user_msg))) {
		trace_manage_error(qdev, usr, "failed to copy message header from userspace");
		ret = -EFAULT;
		goto copy_from_user_failed;
	}
	if (user_msg->len > QAIC_MANAGE_MAX_MSG_LENGTH - sizeof(*user_msg)) {
		trace_manage_error(qdev, usr, "user message too long");
		ret = -EINVAL;
		goto copy_from_user_failed;
	}
	if (copy_from_user(user_msg->data, (void __user *)(arg + sizeof(*user_msg)),
				user_msg->len)) {
		trace_manage_error(qdev, usr, "failed to copy message body from userspace");
		ret = -EFAULT;
		goto copy_from_user_failed;
	}

	ret = qaic_manage(qdev, usr, user_msg);
	if (ret)
		goto copy_from_user_failed;

	if (copy_to_user((void __user *)arg, user_msg,
			 sizeof(*user_msg) + user_msg->len)) {
		trace_manage_error(qdev, usr, "failed to copy to userspace");
		ret = -EFAULT;
	}

copy_from_user_failed:
	kfree(user_msg);
out:
	return ret;
}

int get_cntl_version(struct qaic_device *qdev, struct qaic_user *usr,
		     u16 *major, u16 *minor)
{
	int ret;
	struct qaic_manage_msg *user_msg;
	struct qaic_manage_trans_status_to_dev *status_query;
	struct qaic_manage_trans_status_from_dev *status_result;

	user_msg = kmalloc(sizeof(*user_msg) + sizeof(*status_result), GFP_KERNEL);
	if (!user_msg) {
		ret = -ENOMEM;
		goto out;
	}
	user_msg->len = sizeof(*status_query);
	user_msg->count = 1;

	status_query = (struct qaic_manage_trans_status_to_dev *)user_msg->data;
	status_query->hdr.type = TRANS_STATUS_FROM_USR;
	status_query->hdr.len = sizeof(status_query->hdr);

	ret = qaic_manage(qdev, usr, user_msg);
	if (ret)
		goto kfree_user_msg;
	status_result =
		(struct qaic_manage_trans_status_from_dev *)user_msg->data;
	*major = status_result->major;
	*minor = status_result->minor;

kfree_user_msg:
	kfree(user_msg);
out:
	return ret;
}

static void resp_worker(struct work_struct *work)
{
	struct resp_work *resp = container_of(work, struct resp_work, work);
	struct qaic_device *qdev = resp->qdev;
	struct _msg *msg = resp->buf;
	struct xfer_queue_elem *elem;
	struct xfer_queue_elem *i;
	bool found = false;

	if (msg->hdr.magic_number != MANAGE_MAGIC_NUMBER) {
		kfree(msg);
		kfree(resp);
		return;
	}

	mutex_lock(&qdev->cntl_mutex);
	list_for_each_entry_safe(elem, i, &qdev->cntl_xfer_list, list) {
		if (elem->seq_num == le32_to_cpu(msg->hdr.sequence_number)) {
			found = true;
			list_del_init(&elem->list);
			elem->buf = msg;
			complete_all(&elem->xfer_done);
			break;
		}
	}
	mutex_unlock(&qdev->cntl_mutex);

	if (!found)
		/* request must have timed out, drop packet */
		kfree(msg);

	kfree(resp);
}

static void free_wrapper_from_list(struct wrapper_list *wrappers,
				   struct wrapper_msg *wrapper)
{
	bool all_done = false;

	spin_lock(&wrappers->lock);
	kref_put(&wrapper->ref_count, free_wrapper);
	all_done = list_empty(&wrappers->list);
	spin_unlock(&wrappers->lock);

	if (all_done)
		kfree(wrappers);
}

void qaic_mhi_ul_xfer_cb(struct mhi_device *mhi_dev,
			 struct mhi_result *mhi_result)
{
	struct _msg *msg = mhi_result->buf_addr;
	struct wrapper_msg *wrapper = container_of(msg, struct wrapper_msg,
						   msg);

	free_wrapper_from_list(wrapper->head, wrapper);
}

void qaic_mhi_dl_xfer_cb(struct mhi_device *mhi_dev,
			 struct mhi_result *mhi_result)
{
	struct qaic_device *qdev = mhi_device_get_devdata(mhi_dev);
	struct _msg *msg = mhi_result->buf_addr;
	struct resp_work *resp;

	if (mhi_result->transaction_status) {
		kfree(msg);
		return;
	}

	resp = kmalloc(sizeof(*resp), GFP_ATOMIC);
	if (!resp) {
		pci_err(qdev->pdev, "dl_xfer_cb alloc fail, dropping message\n");
		kfree(msg);
		return;
	}

	INIT_WORK(&resp->work, resp_worker);
	resp->qdev = qdev;
	resp->buf = msg;
	queue_work(qdev->cntl_wq, &resp->work);
}

int qaic_control_open(struct qaic_device *qdev)
{
	if (!qdev->cntl_ch)
		return -ENODEV;

	return mhi_prepare_for_transfer(qdev->cntl_ch);
}

void qaic_control_close(struct qaic_device *qdev)
{
	mhi_unprepare_from_transfer(qdev->cntl_ch);
}

void qaic_release_usr(struct qaic_device *qdev, struct qaic_user *usr)
{
	struct _trans_terminate_to_dev *trans;
	struct wrapper_list *wrappers;
	struct wrapper_msg *wrapper;
	struct _msg *msg;
	struct _msg *rsp;

	wrappers = alloc_wrapper_list();
	if (!wrappers) {
		trace_manage_error(qdev, usr, "unable to alloc wrappers");
		return;
	}

	wrapper = add_wrapper(wrappers, sizeof(*wrapper) + sizeof(*msg) +
			      sizeof(*trans));
	if (!wrapper)
		return;

	msg = &wrapper->msg;

	trans = (struct _trans_terminate_to_dev *)msg->data;

	trans->hdr.type = cpu_to_le32(TRANS_TERMINATE_TO_DEV);
	trans->hdr.len = cpu_to_le32(sizeof(*trans));
	trans->handle = cpu_to_le32(usr->handle);

	mutex_lock(&qdev->cntl_mutex);
	msg->hdr.magic_number = MANAGE_MAGIC_NUMBER;
	msg->hdr.sequence_number = cpu_to_le32(qdev->next_seq_num++);
	msg->hdr.len = cpu_to_le32(sizeof(msg->hdr) + sizeof(*trans));
	msg->hdr.count = cpu_to_le32(1);
	msg->hdr.handle = cpu_to_le32(usr->handle);
	wrapper->len = msg->hdr.len;

	/*
	 * msg_xfer releases the mutex
	 * We don't care about the return of msg_xfer since we will not do
	 * anything different based on what happens.
	 * We ignore pending signals since one will be set if the user is
	 * killed, and we need give the device a chance to cleanup, otherwise
	 * DMA may still be in progress when we return.
	 */
	rsp = msg_xfer(qdev, wrappers, qdev->next_seq_num - 1, true);
	if (!IS_ERR(rsp))
		kfree(rsp);
	free_wrapper_from_list(wrappers, wrapper);
}

void wake_all_cntl(struct qaic_device *qdev)
{
	struct xfer_queue_elem *elem;
	struct xfer_queue_elem *i;

	mutex_lock(&qdev->cntl_mutex);
	list_for_each_entry_safe(elem, i, &qdev->cntl_xfer_list, list) {
		list_del_init(&elem->list);
		complete_all(&elem->xfer_done);
	}
	mutex_unlock(&qdev->cntl_mutex);
}
