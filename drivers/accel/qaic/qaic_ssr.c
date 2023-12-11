// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2020-2021, The Linux Foundation. All rights reserved. */
/* Copyright (c) 2021-2023 Qualcomm Innovation Center, Inc. All rights reserved. */

#include <asm/byteorder.h>
#include <drm/drm_file.h>
#include <linux/devcoredump.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mhi.h>
#include <linux/workqueue.h>

#include "qaic.h"
#include "qaic_ssr.h"
#include "qaic_trace.h"

#define MSG_BUF_SZ 32
#define READ_RSP_BUF_PAGE_ORDER 2
#define to_qddev(dump_info) ((dump_info)->dbc->qdev->qddev)

enum ssr_cmds {
	DEBUG_TRANSFER_INFO =		BIT(0),
	DEBUG_TRANSFER_INFO_RSP =	BIT(1),
	MEMORY_READ =			BIT(2),
	MEMORY_READ_RSP =		BIT(3),
	DEBUG_TRANSFER_DONE =		BIT(4),
	DEBUG_TRANSFER_DONE_RSP =	BIT(5),
	SSR_EVENT =			BIT(8),
	SSR_EVENT_RSP =			BIT(9),
};

enum ssr_events {
	SSR_EVENT_NACK =	BIT(0),
	BEFORE_SHUTDOWN =	BIT(1),
	AFTER_SHUTDOWN =	BIT(2),
	BEFORE_POWER_UP =	BIT(3),
	AFTER_POWER_UP =	BIT(4),
};

struct debug_info_table {
	/* Save preferences. Default is mandatory */
	u64 save_perf;
	/* Base address of the debug region */
	u64 mem_base;
	/* Size of debug region in bytes */
	u64 len;
	/* Description */
	char desc[20];
	/* Filename of debug region */
	char filename[20];
};

struct _ssr_hdr {
	__le32 cmd;
	__le32 len;
	__le32 dbc_id;
};

struct ssr_hdr {
	u32 cmd;
	u32 len;
	u32 dbc_id;
};

struct ssr_debug_transfer_info {
	struct ssr_hdr hdr;
	u32 resv;
	u64 tbl_addr;
	u64 tbl_len;
} __packed;

struct ssr_debug_transfer_info_rsp {
	struct _ssr_hdr hdr;
	__le32 ret;
} __packed;

struct ssr_memory_read {
	struct _ssr_hdr hdr;
	__le32 resv;
	__le64 addr;
	__le64 len;
} __packed;

struct ssr_memory_read_rsp {
	struct _ssr_hdr hdr;
	__le32 resv;
	u8 data[];
} __packed;

struct ssr_debug_transfer_done {
	struct _ssr_hdr hdr;
	__le32 resv;
} __packed;

struct ssr_debug_transfer_done_rsp {
	struct _ssr_hdr hdr;
	__le32 ret;
} __packed;

struct ssr_event {
	struct ssr_hdr hdr;
	u32 event;
} __packed;

struct ssr_event_rsp {
	struct _ssr_hdr hdr;
	__le32 event;
} __packed;

struct ssr_resp {
	/* Work struct to schedule work coming on QAIC_SSR channel */
	struct work_struct work;
	/* Root struct of device, used to access device resources */
	struct qaic_device *qdev;
	/* Buffer used by MHI for transfer requests */
	u8 data[] __aligned(8);
};

/* SSR crashdump book keeping structure */
struct ssr_dump_info {
	/* DBC associated with this SSR crashdump */
	struct dma_bridge_chan *dbc;
	/*
	 * It will be used when we complete the crashdump download and switch
	 * to waiting on SSR events
	 */
	struct ssr_resp *resp;
	/* MEMORY READ request MHI buffer.*/
	struct ssr_memory_read *read_buf_req;
	/* TRUE: ->read_buf_req is queued for MHI transaction. FALSE: Otherwise */
	bool read_buf_req_queued;
	/* Buffer recevied in response to MEMORY READ request */
	struct ssr_resp *read_buf_rsp;
	/* Size of the buffer queued in for MHI transfer */
	u64 read_buf_rsp_sz;
	/* TRUE: ->read_buf_rsp is queued for MHI transaction. FALSE: Otherwise */
	bool read_buf_rsp_queued;
	/* Address of table in host */
	void *tbl_addr;
	/* Total size of table */
	u64 tbl_len;
	/* Offset of table(->tbl_addr) where the new chunk will be dumped */
	u64 tbl_off;
	/* Address of table in device/target */
	u64 tbl_addr_dev;
	/* Ptr to the entire dump */
	void *dump_addr;
	/* Entire crashdump size */
	u64 dump_sz;
	/* Offset of crashdump(->dump_addr) where the new chunk will be dumped */
	u64 dump_off;
	/*
	 * Crashdump will be collected chunk by chunk and this is max size of
	 * one chunk
	 */
	u64 chunk_sz;
	/* Points to the table entry we are currently downloading */
	struct debug_info_table *tbl_ent;
	/* Offset in the current table entry(->tbl_ent) for next chuck */
	u64 tbl_ent_off;
};

struct dump_file_meta {
	u64 size;		/* Total size of the entire dump */
	u64 tbl_len;		/* Length of the table in byte */
};

/*
 * Layout of crashdump
 *              +------------------------------------------+
 *              |         Crashdump Meta structure         |
 *              | type: struct dump_file_meta              |
 *              +------------------------------------------+
 *              |             Crashdump Table              |
 *              | type: array of struct debug_info_table   |
 *              |                                          |
 *              |                                          |
 *              |                                          |
 *              +------------------------------------------+
 *              |                Crashdump                 |
 *              |                                          |
 *              |                                          |
 *              |                                          |
 *              |                                          |
 *              |                                          |
 *              +------------------------------------------+
 */

static void free_ssr_dump_info(struct ssr_dump_info *dump_info)
{
	if (!dump_info)
		return;
	if (!dump_info->read_buf_req_queued)
		kfree(dump_info->read_buf_req);
	if (!dump_info->read_buf_rsp_queued)
		kfree(dump_info->read_buf_rsp);
	vfree(dump_info->tbl_addr);
	vfree(dump_info->dump_addr);
	dump_info->dbc->dump_info = NULL;
	kfree(dump_info);
}

void clean_up_ssr(struct qaic_device *qdev, u32 dbc_id)
{
	dbc_exit_ssr(qdev, dbc_id);
	free_ssr_dump_info(qdev->dbc[dbc_id].dump_info);
}

static int alloc_dump(struct ssr_dump_info *dump_info)
{
	struct debug_info_table *tbl_ent = dump_info->tbl_addr;
	struct dump_file_meta *dump_meta;
	u64 tbl_sz_lp = 0;
	u64 dump_size = 0;

	while (tbl_sz_lp < dump_info->tbl_len) {
		le64_to_cpus(&tbl_ent->save_perf);
		le64_to_cpus(&tbl_ent->mem_base);
		le64_to_cpus(&tbl_ent->len);

		if (tbl_ent->len == 0) {
			trace_qaic_ssr_1(to_qddev(dump_info), "Invalid table entry len at index %llu.",
					 tbl_sz_lp / sizeof(*tbl_ent));
			return -EINVAL;
		}

		dump_size += tbl_ent->len;
		tbl_ent++;
		tbl_sz_lp += sizeof(*tbl_ent);
	}

	dump_info->dump_sz = dump_size + dump_info->tbl_len + sizeof(*dump_meta);
	dump_info->dump_addr = vzalloc(dump_info->dump_sz);
	if (!dump_info->dump_addr) {
		trace_qaic_ssr(to_qddev(dump_info), "Failed to allocate memory for entire dump.", -ENOMEM);
		return -ENOMEM;
	}

	/* Copy crashdump meta and table */
	dump_meta = dump_info->dump_addr;
	dump_meta->size = dump_info->dump_sz;
	dump_meta->tbl_len = dump_info->tbl_len;
	memcpy(dump_info->dump_addr + sizeof(*dump_meta), dump_info->tbl_addr, dump_info->tbl_len);
	/* Offset by crashdump meta and table (copied above) */
	dump_info->dump_off = dump_info->tbl_len + sizeof(*dump_meta);

	return 0;
}

static int send_xfer_done(struct qaic_device *qdev, void *resp, u32 dbc_id)
{
	struct ssr_debug_transfer_done *xfer_done;
	int ret;

	xfer_done = kmalloc(sizeof(*xfer_done), GFP_KERNEL);
	if (!xfer_done) {
		ret = -ENOMEM;
		trace_qaic_ssr(qdev->qddev, "Failed to allocate memory for dbg resp buffer.", ret);
		goto out;
	}

	ret = mhi_queue_buf(qdev->ssr_ch, DMA_FROM_DEVICE, resp, MSG_BUF_SZ, MHI_EOT);
	if (ret) {
		trace_qaic_ssr(qdev->qddev, "MHI failed to queue dbg resp buffer.", ret);
		goto free_xfer_done;
	}

	xfer_done->hdr.cmd = cpu_to_le32(DEBUG_TRANSFER_DONE);
	xfer_done->hdr.len = cpu_to_le32(sizeof(*xfer_done));
	xfer_done->hdr.dbc_id = cpu_to_le32(dbc_id);

	ret = mhi_queue_buf(qdev->ssr_ch, DMA_TO_DEVICE, xfer_done, sizeof(*xfer_done), MHI_EOT);
	if (ret) {
		trace_qaic_ssr(qdev->qddev, "MHI failed to queue dbg req buffer.", ret);
		goto free_xfer_done;
	}

	return 0;

free_xfer_done:
	kfree(xfer_done);
out:
	return ret;
}

static int mem_read_req(struct qaic_device *qdev, struct ssr_dump_info *dump_info, u64 dest_addr,
		        u64 dest_len)
{
	struct ssr_memory_read *read_buf_req = dump_info->read_buf_req;
	u32 dbc_id = dump_info->dbc->id;
	int ret;

	ret = mhi_queue_buf(qdev->ssr_ch, DMA_FROM_DEVICE, dump_info->read_buf_rsp->data,
			    dump_info->read_buf_rsp_sz, MHI_EOT);
	if (ret) {
		trace_qaic_ssr(qdev->qddev, "MHI failed to queue read resp buffer.", ret);
		goto out;
	}
	else {
		dump_info->read_buf_rsp_queued = true;
	}


	read_buf_req->hdr.cmd = cpu_to_le32(MEMORY_READ);
	read_buf_req->hdr.len = cpu_to_le32(sizeof(*read_buf_req));
	read_buf_req->hdr.dbc_id = cpu_to_le32(dbc_id);
	read_buf_req->addr = cpu_to_le64(dest_addr);
	read_buf_req->len = cpu_to_le64(dest_len);

	ret = mhi_queue_buf(qdev->ssr_ch, DMA_TO_DEVICE, read_buf_req, sizeof(*read_buf_req),
			    MHI_EOT);
	if (!ret)
		dump_info->read_buf_req_queued = true;
	else
		trace_qaic_ssr(qdev->qddev, "MHI failed to queue read req buffer.", ret);

out:
	return ret;
}

static int ssr_copy_table(struct ssr_dump_info *dump_info, void *data, u64 len)
{
	if (len > dump_info->tbl_len - dump_info->tbl_off) {
		trace_qaic_ssr_3(to_qddev(dump_info), "Table chunk too large. chunk %llu total table size %llu table offset %llu.",
				 len, dump_info->tbl_len, dump_info->tbl_off);
		return -EINVAL;
	}

	memcpy(dump_info->tbl_addr + dump_info->tbl_off, data, len);
	dump_info->tbl_off += len;

	/* Entire table has been downloaded, alloc dump memory */
	if (dump_info->tbl_off == dump_info->tbl_len) {
		dump_info->tbl_ent = dump_info->tbl_addr;
		return alloc_dump(dump_info);
	}

	return 0;
}

static int ssr_copy_dump(struct ssr_dump_info *dump_info, void *data, u64 len)
{
	struct debug_info_table *tbl_ent;

	tbl_ent = dump_info->tbl_ent;

	if (len > tbl_ent->len - dump_info->tbl_ent_off) {
		trace_qaic_ssr_3(to_qddev(dump_info), "Dump chunk too large. chunk %llu total dump size %llu dump offset %llu.",
				 len, tbl_ent->len, dump_info->tbl_ent_off);
		return -EINVAL;
	}

	memcpy(dump_info->dump_addr + dump_info->dump_off, data, len);
	dump_info->dump_off += len;
	dump_info->tbl_ent_off += len;

	/*
	 * Current segment (a entry in table) of the crashdump is complete,
	 * move to next one
	 */
	if (tbl_ent->len == dump_info->tbl_ent_off) {
		dump_info->tbl_ent++;
		dump_info->tbl_ent_off = 0;
	}

	return 0;
}

static void ssr_dump_worker(struct work_struct *work)
{
	struct ssr_resp *read_buf_rsp = container_of(work, struct ssr_resp, work);
	struct qaic_device *qdev = read_buf_rsp->qdev;
	struct ssr_memory_read_rsp *mem_rd_resp;
	struct debug_info_table *tbl_ent;
	struct ssr_dump_info *dump_info;
	u64 dest_addr, dest_len;
	struct _ssr_hdr *_hdr;
	struct ssr_hdr hdr;
	u64 data_len;
	int ret;

	mem_rd_resp = (struct ssr_memory_read_rsp *)read_buf_rsp->data;
	_hdr = &mem_rd_resp->hdr;
	hdr.cmd = le32_to_cpu(_hdr->cmd);
	hdr.len = le32_to_cpu(_hdr->len);
	hdr.dbc_id = le32_to_cpu(_hdr->dbc_id);

	if (hdr.dbc_id >= qdev->num_dbc) {
		trace_qaic_ssr_1(qdev->qddev, "Invalid dbc_id=%llu.", hdr.dbc_id);
		goto reset_device;
	}

	dump_info = qdev->dbc[hdr.dbc_id].dump_info;

	if (!dump_info) {
		trace_qaic_ssr(qdev->qddev, "Invalid state device not in reset.", -EINVAL);
		goto reset_device;
	}

	dump_info->read_buf_rsp_queued = false;

	if (hdr.cmd != MEMORY_READ_RSP) {
		trace_qaic_ssr_2(qdev->qddev, "Invalid cmd=%llu. Expected MEMORY_READ_RSP(%llu).",
				 hdr.cmd, MEMORY_READ_RSP);
		goto free_dump_info;
	}

	if (hdr.len > dump_info->read_buf_rsp_sz) {
		trace_qaic_ssr_2(qdev->qddev, "Invalid memory read resp len=%llu it cannot exceed %llu.",
				 hdr.cmd, dump_info->read_buf_rsp_sz);
		goto free_dump_info;
	}

	data_len = hdr.len - sizeof(*mem_rd_resp);

	if (dump_info->tbl_off < dump_info->tbl_len) /* Chunk belongs to table */
		ret = ssr_copy_table(dump_info, mem_rd_resp->data, data_len);
	else /* Chunk belongs to crashdump */
		ret = ssr_copy_dump(dump_info, mem_rd_resp->data, data_len);

	if (ret) {
		trace_qaic_ssr(qdev->qddev, "Failed to copy data.", ret);
		goto free_dump_info;
	}

	if (dump_info->tbl_off < dump_info->tbl_len) {
		/* Continue downloading table */
		dest_addr = dump_info->tbl_addr_dev + dump_info->tbl_off;
		dest_len = min(dump_info->chunk_sz, dump_info->tbl_len - dump_info->tbl_off);
		ret = mem_read_req(qdev, dump_info, dest_addr, dest_len);
	} else if (dump_info->dump_off < dump_info->dump_sz) {
		/* Continue downloading crashdump */
		tbl_ent = dump_info->tbl_ent;
		dest_addr = tbl_ent->mem_base + dump_info->tbl_ent_off;
		dest_len = min(dump_info->chunk_sz, tbl_ent->len - dump_info->tbl_ent_off);
		ret = mem_read_req(qdev, dump_info, dest_addr, dest_len);
	} else {
		/* Crashdump download complete */
		ret = send_xfer_done(qdev, dump_info->resp->data, hdr.dbc_id);
	}

	if (ret) { /* Most likely a MHI xfer has failed */
		trace_qaic_ssr(qdev->qddev, "MHI transfer failed.", ret);
		goto free_dump_info;
	}

	return;

free_dump_info:
	/* Free the allocated memory */
	free_ssr_dump_info(dump_info);
reset_device:
	/*
	 * After subsystem crashes in device crashdump collection begins but
	 * something went wrong while collecting crashdump, now instead of
	 * handling this error we just reset the device as the best effort has
	 * been made
	 */
	mhi_soc_reset(qdev->mhi_cntrl);
}

static struct ssr_dump_info *alloc_dump_info(struct qaic_device *qdev,
					     struct ssr_debug_transfer_info *debug_info)
{
	struct ssr_dump_info *dump_info;
	unsigned int read_rsp_sz;
	int ret;

	le64_to_cpus(&debug_info->tbl_len);
	le64_to_cpus(&debug_info->tbl_addr);

	if (debug_info->tbl_len == 0 ||
	    debug_info->tbl_len % sizeof(struct debug_info_table) != 0) {
		ret = -EINVAL;
		trace_qaic_ssr_1(qdev->qddev, "Invalid table size %llu.", debug_info->tbl_len);
		goto out;
	}

	/* Allocate SSR crashdump book keeping structure */
	dump_info = kzalloc(sizeof(*dump_info), GFP_KERNEL);
	if (!dump_info) {
		ret = -ENOMEM;
		trace_qaic_ssr(qdev->qddev, "Failed to allocate SSR metadata.", ret);
		goto out;
	}

	/* Buffer used to receive MEMORY READ response from device via MHI */
	read_rsp_sz = PAGE_SIZE << READ_RSP_BUF_PAGE_ORDER;
	while (read_rsp_sz > 0) {
		dump_info->read_buf_rsp = kzalloc(read_rsp_sz, GFP_KERNEL | __GFP_NOWARN);
		if (dump_info->read_buf_rsp)
			break;
		read_rsp_sz >>= 1;
	}
	if (!dump_info->read_buf_rsp) {
		ret = -ENOMEM;
		trace_qaic_ssr(qdev->qddev, "Failed to allocate memory read resp buffer.", ret);
		goto free_dump_info;
	}

	/* Buffer used to send MEMORY READ request to device via MHI */
	dump_info->read_buf_req = kzalloc(sizeof(*dump_info->read_buf_req), GFP_KERNEL);
	if (!dump_info->read_buf_req) {
		ret = -ENOMEM;
		trace_qaic_ssr(qdev->qddev, "Failed to allocate memory read req buffer.", ret);
		goto free_read_buf_rsp;
	}

	/* Crashdump meta table buffer */
	dump_info->tbl_addr = vzalloc(debug_info->tbl_len);
	if (!dump_info->tbl_addr) {
		ret = -ENOMEM;
		trace_qaic_ssr(qdev->qddev, "Failed to allocate table buffer.", ret);
		goto free_read_buf_req;
	}

	INIT_WORK(&dump_info->read_buf_rsp->work, ssr_dump_worker);
	dump_info->read_buf_rsp->qdev = qdev;
	dump_info->tbl_addr_dev = debug_info->tbl_addr;
	dump_info->tbl_len = debug_info->tbl_len;
	dump_info->read_buf_rsp_sz = read_rsp_sz - sizeof(*dump_info->read_buf_rsp);
	dump_info->chunk_sz = dump_info->read_buf_rsp_sz - sizeof(struct ssr_memory_read_rsp);

	return dump_info;

free_read_buf_req:
	kfree(dump_info->read_buf_req);
free_read_buf_rsp:
	kfree(dump_info->read_buf_rsp);
free_dump_info:
	kfree(dump_info);
out:
	return ERR_PTR(ret);
}

static int dbg_xfer_info_rsp(struct qaic_device *qdev, struct dma_bridge_chan *dbc,
			     struct ssr_debug_transfer_info *debug_info,
			     struct ssr_dump_info **dump_info_out)
{

	struct ssr_debug_transfer_info_rsp *debug_rsp;
	struct ssr_dump_info *dump_info;
	int ret = 0, ret2;

	debug_rsp = kmalloc(sizeof(*debug_rsp), GFP_KERNEL);
	if (!debug_rsp) {
		trace_qaic_ssr(qdev->qddev, "Failed to allocate dbg rsp buffer.", -ENOMEM);
		return -ENOMEM;
	}

	if (dbc->state != DBC_STATE_BEFORE_POWER_UP) {
		ret = -EINVAL;
		trace_qaic_ssr_2(qdev->qddev, "Invalid DBC state(%llu) for debug transfer. Expected %llu.",
				 dbc->state, DBC_STATE_BEFORE_POWER_UP);
		dump_info = NULL;
		goto send_rsp;
	}

	dump_info = alloc_dump_info(qdev, debug_info);
	if (IS_ERR(dump_info)) {
		ret = PTR_ERR(dump_info);
		dump_info = NULL;
		trace_qaic_ssr(qdev->qddev, "Failed alloc_dump_info().", ret);
	}

send_rsp:
	debug_rsp->hdr.cmd = cpu_to_le32(DEBUG_TRANSFER_INFO_RSP);
	debug_rsp->hdr.len = cpu_to_le32(sizeof(*debug_rsp));
	debug_rsp->hdr.dbc_id = cpu_to_le32(dbc->id);
	/*
	 * 0 = Return an ACK confirming the host is ready to download crashdump
	 * 1 = Return an NACK confirming the host is not ready to download crashdump
	 */
	debug_rsp->ret = cpu_to_le32(ret ? 1 : 0);

	ret2 = mhi_queue_buf(qdev->ssr_ch, DMA_TO_DEVICE, debug_rsp, sizeof(*debug_rsp), MHI_EOT);
	if (ret2) {
		free_ssr_dump_info(dump_info);
		kfree(debug_rsp);
		trace_qaic_ssr(qdev->qddev, "MHI failed to send debug rsp.", ret2);
		return ret2;
	}

	*dump_info_out = dump_info;

	return ret;
}

static void dbg_xfer_done_rsp(struct qaic_device *qdev, struct dma_bridge_chan *dbc,
			      struct ssr_debug_transfer_done_rsp *xfer_rsp)
{
	struct device *kdev = to_accel_kdev(qdev->qddev);
	struct ssr_dump_info *dump_info = dbc->dump_info;
	u32 status = le32_to_cpu(xfer_rsp->ret);

	if (!dump_info) {
		trace_qaic_ssr(qdev->qddev, "Not in SSR. Invalid dbg transfer done resp.", -EINVAL);
		return;
	}

	if (status) {
		free_ssr_dump_info(dump_info);
		trace_qaic_ssr(qdev->qddev, "SSR transfer done failed.", status);
		return;
	}

	dev_coredumpv(kdev, dump_info->dump_addr, dump_info->dump_sz, GFP_KERNEL);
	/* dev_coredumpv will free dump_info->dump_addr */
	dump_info->dump_addr = NULL;
	free_ssr_dump_info(dump_info);
}

static void ssr_worker(struct work_struct *work)
{
	struct ssr_resp *resp = container_of(work, struct ssr_resp, work);
	struct ssr_hdr *hdr = (struct ssr_hdr *)resp->data;
	struct ssr_dump_info *dump_info = NULL;
	struct qaic_device *qdev = resp->qdev;
	struct ssr_event_rsp *event_rsp;
	struct dma_bridge_chan *dbc;
	struct ssr_event *event;
	u32 ssr_event_ack;
	int ret;

	le32_to_cpus(&hdr->cmd);
	le32_to_cpus(&hdr->len);
	le32_to_cpus(&hdr->dbc_id);

	if (hdr->len > MSG_BUF_SZ) {
		trace_qaic_ssr_2(qdev->qddev, "Response size %llu too large. Expected %llu.",
				 hdr->len, MSG_BUF_SZ);
		goto out;
	}

	if (hdr->dbc_id >= qdev->num_dbc) {
		trace_qaic_ssr_1(qdev->qddev, "Invalid dbc_id=%llu.", hdr->dbc_id);
		goto out;
	}

	dbc = &qdev->dbc[hdr->dbc_id];

	switch (hdr->cmd) {
	case DEBUG_TRANSFER_INFO:
		ret = dbg_xfer_info_rsp(qdev, dbc, (struct ssr_debug_transfer_info *)resp->data,
					&dump_info);
		if (ret) {
			trace_qaic_ssr(qdev->qddev, "Failed dbg_xfer_info_rsp().", ret);
			break;
		}

		dbc->dump_info = dump_info;
		dump_info->dbc = dbc;
		dump_info->resp = resp;

		/* Start by downloading debug table */
		ret = mem_read_req(qdev, dump_info, dump_info->tbl_addr_dev,
				   min(dump_info->tbl_len, dump_info->chunk_sz));
		if (ret) {
			free_ssr_dump_info(dump_info);
			trace_qaic_ssr(qdev->qddev, "Failed mem_read_req().", ret);
			break;
		}

		/*
		 * Till now everything went fine, which means that we will be
		 * collecting crashdump chunk by chunk. Do not queue a response
		 * buffer for SSR cmds till the crashdump is complete.
		 */
		return;
	case SSR_EVENT:
		event = (struct ssr_event *)hdr;
		le32_to_cpus(&event->event);
		ssr_event_ack = event->event;

		switch (event->event) {
		case BEFORE_SHUTDOWN:
			set_dbc_state(qdev, hdr->dbc_id, DBC_STATE_BEFORE_SHUTDOWN);
			dbc_enter_ssr(qdev, hdr->dbc_id);
			break;
		case AFTER_SHUTDOWN:
			set_dbc_state(qdev, hdr->dbc_id, DBC_STATE_AFTER_SHUTDOWN);
			break;
		case BEFORE_POWER_UP:
			set_dbc_state(qdev, hdr->dbc_id, DBC_STATE_BEFORE_POWER_UP);
			break;
		case AFTER_POWER_UP:
			/*
			 * If dump info is a non NULL value it means that we
			 * have received this SSR event while downloading a
			 * crashdump for this DBC is still in progress. NACK
			 * the SSR event
			 */
			if (dbc->dump_info) {
				free_ssr_dump_info(dbc->dump_info);
				ssr_event_ack = SSR_EVENT_NACK;
				trace_qaic_ssr(qdev->qddev, "Unexpected AFTER_POWER_UP event received dump downloading still in progress.",
					       PTR_ERR(dbc->dump_info));
				break;
			}

			set_dbc_state(qdev, hdr->dbc_id, DBC_STATE_AFTER_POWER_UP);
			break;
		default:
			break;
		}

		event_rsp = kmalloc(sizeof(*event_rsp), GFP_KERNEL);
		if (!event_rsp) {
			trace_qaic_ssr(qdev->qddev, "Failed to create event resp buffer.", -ENOMEM);
			break;
		}

		event_rsp->hdr.cmd = cpu_to_le32(SSR_EVENT_RSP);
		event_rsp->hdr.len = cpu_to_le32(sizeof(*event_rsp));
		event_rsp->hdr.dbc_id = cpu_to_le32(hdr->dbc_id);
		event_rsp->event = cpu_to_le32(ssr_event_ack);

		ret = mhi_queue_buf(qdev->ssr_ch, DMA_TO_DEVICE, event_rsp, sizeof(*event_rsp),
				    MHI_EOT);
		if (ret) {
			trace_qaic_ssr(qdev->qddev, "MHI failed to send event resp.", ret);
			kfree(event_rsp);
		}

		if (event->event == AFTER_POWER_UP && ssr_event_ack != SSR_EVENT_NACK) {
			dbc_exit_ssr(qdev, hdr->dbc_id);
			set_dbc_state(qdev, hdr->dbc_id, DBC_STATE_IDLE);
		}

		break;
	case DEBUG_TRANSFER_DONE_RSP:
		dbg_xfer_done_rsp(qdev, dbc, (struct ssr_debug_transfer_done_rsp *)hdr);
		break;
	default:
		break;
	}

out:
	ret = mhi_queue_buf(qdev->ssr_ch, DMA_FROM_DEVICE, resp->data, MSG_BUF_SZ, MHI_EOT);
	if (ret) {
		trace_qaic_ssr(qdev->qddev, "MHI failed to send resp.", ret);
		kfree(resp);
	}
}

static int qaic_ssr_mhi_probe(struct mhi_device *mhi_dev, const struct mhi_device_id *id)
{
	struct qaic_device *qdev = pci_get_drvdata(to_pci_dev(mhi_dev->mhi_cntrl->cntrl_dev));
	struct ssr_resp *resp;
	int ret;

	ret = mhi_prepare_for_transfer(mhi_dev);
	if (ret)
		return ret;

	resp = kzalloc(sizeof(*resp) + MSG_BUF_SZ, GFP_KERNEL);
	if (!resp) {
		mhi_unprepare_from_transfer(mhi_dev);
		return -ENOMEM;
	}

	resp->qdev = qdev;
	INIT_WORK(&resp->work, ssr_worker);

	ret = mhi_queue_buf(mhi_dev, DMA_FROM_DEVICE, resp->data, MSG_BUF_SZ, MHI_EOT);
	if (ret) {
		kfree(resp);
		mhi_unprepare_from_transfer(mhi_dev);
		return ret;
	}

	dev_set_drvdata(&mhi_dev->dev, qdev);
	qdev->ssr_ch = mhi_dev;

	return 0;
}

static void qaic_ssr_mhi_remove(struct mhi_device *mhi_dev)
{
	struct qaic_device *qdev;

	qdev = dev_get_drvdata(&mhi_dev->dev);
	mhi_unprepare_from_transfer(qdev->ssr_ch);
	qdev->ssr_ch = NULL;
}

static void qaic_ssr_mhi_ul_xfer_cb(struct mhi_device *mhi_dev, struct mhi_result *mhi_result)
{
	struct qaic_device *qdev = dev_get_drvdata(&mhi_dev->dev);
	struct _ssr_hdr *hdr = mhi_result->buf_addr;
	struct ssr_dump_info *dump_info;

	if (mhi_result->transaction_status) {
		kfree(mhi_result->buf_addr);
		return;
	}

	/*
	 * MEMORY READ is used to download crashdump. And crashdump is
	 * downloaded chunk by chunk in a series of MEMORY READ SSR commands.
	 * Hence to avoid too many kmalloc() and kfree() of the same MEMORY READ
	 * request buffer, we allocate only one such buffer and free it only
	 * once.
	 */
	dump_info = qdev->dbc[le32_to_cpu(hdr->dbc_id)].dump_info;
	if (le32_to_cpu(hdr->cmd) == MEMORY_READ) {
		dump_info->read_buf_req_queued = false;
		return;
	}

	kfree(mhi_result->buf_addr);
}

static void qaic_ssr_mhi_dl_xfer_cb(struct mhi_device *mhi_dev, struct mhi_result *mhi_result)
{
	struct ssr_resp *resp = container_of(mhi_result->buf_addr, struct ssr_resp, data);

	if (mhi_result->transaction_status) {
		kfree(resp);
		return;
	}

	queue_work(resp->qdev->ssr_wq, &resp->work);
}

static const struct mhi_device_id qaic_ssr_mhi_match_table[] = {
	{ .chan = "QAIC_SSR", },
	{},
};

static struct mhi_driver qaic_ssr_mhi_driver = {
	.id_table = qaic_ssr_mhi_match_table,
	.remove = qaic_ssr_mhi_remove,
	.probe = qaic_ssr_mhi_probe,
	.ul_xfer_cb = qaic_ssr_mhi_ul_xfer_cb,
	.dl_xfer_cb = qaic_ssr_mhi_dl_xfer_cb,
	.driver = {
		.name = "qaic_ssr",
		.owner = THIS_MODULE,
	},
};

int qaic_ssr_register(void)
{
	return mhi_driver_register(&qaic_ssr_mhi_driver);
}

void qaic_ssr_unregister(void)
{
	mhi_driver_unregister(&qaic_ssr_mhi_driver);
}
