/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#if !defined(_QAIC_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _QAIC_TRACE_H_

#include <drm/drm_file.h>
#include <linux/stringify.h>
#include <linux/types.h>
#include <linux/tracepoint.h>
#include <uapi/drm/qaic_accel.h>

#include "qaic.h"

#undef TRACE_SYSTEM
#define TRACE_SYSTEM qaic
#define TRACE_INCLUDE_FILE qaic_trace

#define MAX_BUF_LEN			256
#define DEV_NAME_LEN			16
#define trace_qaic_devname(qddev)	(qddev) ? dev_name(to_accel_kdev(qddev)) : "NODEV"

DECLARE_EVENT_CLASS(qaic_trace1,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret),
	TP_ARGS(qddev, msg, ret),
	TP_STRUCT__entry(
		__array(char, devname, DEV_NAME_LEN)
		__string(msg, msg)
		__field(int, ret)
	),
	TP_fast_assign(
		snprintf(__entry->devname, DEV_NAME_LEN, "%s", trace_qaic_devname(qddev));
		__assign_str(msg, msg);
		__entry->ret = ret;
	),
	TP_printk("%s: %s %d", __entry->devname, __get_str(msg), __entry->ret)
);

DEFINE_EVENT(qaic_trace1, qaic_mhi_queue,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret),
	TP_ARGS(qddev, msg, ret)
);

DEFINE_EVENT(qaic_trace1, qaic_manage,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret),
	TP_ARGS(qddev, msg, ret)
);

DEFINE_EVENT(qaic_trace1, qaic_encode,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret),
	TP_ARGS(qddev, msg, ret)
);

DEFINE_EVENT(qaic_trace1, qaic_decode,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret),
	TP_ARGS(qddev, msg, ret)
);

DEFINE_EVENT(qaic_trace1, qaic_create_bo,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret),
	TP_ARGS(qddev, msg, ret)
);

DEFINE_EVENT(qaic_trace1, qaic_mmap,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret),
	TP_ARGS(qddev, msg, ret)
);

DEFINE_EVENT(qaic_trace1, qaic_attach_slice_bo,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret),
	TP_ARGS(qddev, msg, ret)
);

DEFINE_EVENT(qaic_trace1, qaic_execute,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret),
	TP_ARGS(qddev, msg, ret)
);

DEFINE_EVENT(qaic_trace1, qaic_wait,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret),
	TP_ARGS(qddev, msg, ret)
);

DEFINE_EVENT(qaic_trace1, qaic_stats,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret),
	TP_ARGS(qddev, msg, ret)
);

DEFINE_EVENT(qaic_trace1, qaic_detach,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret),
	TP_ARGS(qddev, msg, ret)
);

DEFINE_EVENT(qaic_trace1, qaic_ssr,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret),
	TP_ARGS(qddev, msg, ret)
);

DECLARE_EVENT_CLASS(qaic_trace2,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret, struct qaic_user *usr),
	TP_ARGS(qddev, msg, ret, usr),
	TP_STRUCT__entry(
		__array(char, devname, DEV_NAME_LEN)
		__string(msg, msg)
		__field(int, ret)
		__field(int, handle)
	),
	TP_fast_assign(
		snprintf(__entry->devname, DEV_NAME_LEN, "%s", trace_qaic_devname(qddev));
		__assign_str(msg, msg);
		__entry->ret = ret;
		__entry->handle = usr->handle;
	),
	TP_printk("%s: %d: %s %d", __entry->devname, __entry->handle, __get_str(msg), __entry->ret)
);

DEFINE_EVENT(qaic_trace2, qaic_manage_usr,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, int ret, struct qaic_user *usr),
	TP_ARGS(qddev, msg, ret, usr)
);

DECLARE_EVENT_CLASS(qaic_trace3,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1),
	TP_ARGS(qddev, msg, data1),
	TP_STRUCT__entry(
		__array(char, devname, DEV_NAME_LEN)
		__array(char, buf, MAX_BUF_LEN)
	),
	TP_fast_assign(
		snprintf(__entry->devname, DEV_NAME_LEN, "%s", trace_qaic_devname(qddev));
		snprintf(__entry->buf, MAX_BUF_LEN, msg, data1);
	),
	TP_printk("%s: %s", __entry->devname, __entry->buf)
);

DEFINE_EVENT(qaic_trace3, qaic_manage_1,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1),
	TP_ARGS(qddev, msg, data1)
);

DEFINE_EVENT(qaic_trace3, qaic_manage_dbg,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1),
	TP_ARGS(qddev, msg, data1)
);

DEFINE_EVENT(qaic_trace3, qaic_encode_1,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1),
	TP_ARGS(qddev, msg, data1)
);

DEFINE_EVENT(qaic_trace3, qaic_decode_1,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1),
	TP_ARGS(qddev, msg, data1)
);

DEFINE_EVENT(qaic_trace3, qaic_mmap_1,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1),
	TP_ARGS(qddev, msg, data1)
);

DEFINE_EVENT(qaic_trace3, qaic_attach_slice_bo_1,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1),
	TP_ARGS(qddev, msg, data1)
);

DEFINE_EVENT(qaic_trace3, qaic_execute_1,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1),
	TP_ARGS(qddev, msg, data1)
);

DEFINE_EVENT(qaic_trace3, qaic_wait_1,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1),
	TP_ARGS(qddev, msg, data1)
);

DEFINE_EVENT(qaic_trace3, qaic_stats_1,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1),
	TP_ARGS(qddev, msg, data1)
);

DEFINE_EVENT(qaic_trace3, qaic_detach_1,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1),
	TP_ARGS(qddev, msg, data1)
);

DEFINE_EVENT(qaic_trace3, qaic_ssr_1,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1),
	TP_ARGS(qddev, msg, data1)
);

DECLARE_EVENT_CLASS(qaic_trace4,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2),
	TP_ARGS(qddev, msg, data1, data2),
	TP_STRUCT__entry(
		__array(char, devname, DEV_NAME_LEN)
		__array(char, buf, MAX_BUF_LEN)
	),
	TP_fast_assign(
		snprintf(__entry->devname, DEV_NAME_LEN, "%s", trace_qaic_devname(qddev));
		snprintf(__entry->buf, MAX_BUF_LEN, msg, data1, data2);
	),
	TP_printk("%s: %s", __entry->devname, __entry->buf)
);

DEFINE_EVENT(qaic_trace4, qaic_manage_2,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2),
	TP_ARGS(qddev, msg, data1, data2)
);

DEFINE_EVENT(qaic_trace4, qaic_encode_2,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2),
	TP_ARGS(qddev, msg, data1, data2)
);

DEFINE_EVENT(qaic_trace4, qaic_decode_2,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2),
	TP_ARGS(qddev, msg, data1, data2)
);

DEFINE_EVENT(qaic_trace4, qaic_attach_slice_bo_2,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2),
	TP_ARGS(qddev, msg, data1, data2)
);

DEFINE_EVENT(qaic_trace4, qaic_execute_2,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2),
	TP_ARGS(qddev, msg, data1, data2)
);

DEFINE_EVENT(qaic_trace4, qaic_stats_2,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2),
	TP_ARGS(qddev, msg, data1, data2)
);

DEFINE_EVENT(qaic_trace4, qaic_ssr_2,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2),
	TP_ARGS(qddev, msg, data1, data2)
);

DECLARE_EVENT_CLASS(qaic_trace5,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2, u64 data3),
	TP_ARGS(qddev, msg, data1, data2, data3),
	TP_STRUCT__entry(
		__array(char, devname, DEV_NAME_LEN)
		__array(char, buf, MAX_BUF_LEN)
	),
	TP_fast_assign(
		snprintf(__entry->devname, DEV_NAME_LEN, "%s", trace_qaic_devname(qddev));
		snprintf(__entry->buf, MAX_BUF_LEN, msg, data1, data2, data3);
	),
	TP_printk("%s: %s", __entry->devname, __entry->buf)
);

DEFINE_EVENT(qaic_trace5, qaic_manage_3,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2, u64 data3),
	TP_ARGS(qddev, msg, data1, data2, data3)
);

DEFINE_EVENT(qaic_trace5, qaic_encode_3,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2, u64 data3),
	TP_ARGS(qddev, msg, data1, data2, data3)
);

DEFINE_EVENT(qaic_trace5, qaic_decode_3,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2, u64 data3),
	TP_ARGS(qddev, msg, data1, data2, data3)
);

DEFINE_EVENT(qaic_trace5, qaic_attach_slice_bo_3,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2, u64 data3),
	TP_ARGS(qddev, msg, data1, data2, data3)
);

DEFINE_EVENT(qaic_trace5, qaic_ssr_3,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2, u64 data3),
	TP_ARGS(qddev, msg, data1, data2, data3)
);

DECLARE_EVENT_CLASS(qaic_trace6,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2, u64 data3,
		 u64 data4),
	TP_ARGS(qddev, msg, data1, data2, data3, data4),
	TP_STRUCT__entry(
		__array(char, devname, DEV_NAME_LEN)
		__array(char, buf, MAX_BUF_LEN)
	),
	TP_fast_assign(
		snprintf(__entry->devname, DEV_NAME_LEN, "%s", trace_qaic_devname(qddev));
		snprintf(__entry->buf, MAX_BUF_LEN, msg, data1, data2, data3, data4);
	),
	TP_printk("%s: %s", __entry->devname, __entry->buf)
);

DEFINE_EVENT(qaic_trace6, qaic_manage_4,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2, u64 data3,
		 u64 data4),
	TP_ARGS(qddev, msg, data1, data2, data3, data4)
);

DEFINE_EVENT(qaic_trace6, qaic_attach_slice_bo_4,
	TP_PROTO(struct qaic_drm_device *qddev, const char *msg, u64 data1, u64 data2, u64 data3,
		 u64 data4),
	TP_ARGS(qddev, msg, data1, data2, data3, data4)
);

TRACE_EVENT(qaic_encode_passthrough,
	TP_PROTO(struct qaic_drm_device *qddev,
		 struct qaic_manage_trans_passthrough *in_trans),
	TP_ARGS(qddev, in_trans),
	TP_STRUCT__entry(
		__string(devname, trace_qaic_devname(qddev))
		__field(__u32, len)
	),
	TP_fast_assign(
		__assign_str(devname, trace_qaic_devname(qddev));
		__entry->len = in_trans->hdr.len;
	),
	TP_printk("%s: len %u", __get_str(devname), __entry->len)
);

TRACE_EVENT(qaic_encode_dma,
	TP_PROTO(struct qaic_drm_device *qddev,
		 struct qaic_manage_trans_dma_xfer *in_trans),
	TP_ARGS(qddev, in_trans),
	TP_STRUCT__entry(
		__string(devname, trace_qaic_devname(qddev))
		__field(__u32, len)
		__field(__u32, tag)
		__field(__u32, pad)
		__field(__u64, addr)
		__field(__u64, size)
	),
	TP_fast_assign(
		__assign_str(devname, trace_qaic_devname(qddev));
		__entry->len = in_trans->hdr.len;
		__entry->tag = in_trans->tag;
		__entry->pad = in_trans->pad;
		__entry->addr = in_trans->addr;
		__entry->size = in_trans->size;
	),
	TP_printk("%s: len %u tag %u pad %u address 0x%llx size %llu",
		__get_str(devname), __entry->len, __entry->tag, __entry->pad,
		__entry->addr, __entry->size)
);

TRACE_EVENT(qaic_encode_activate,
	TP_PROTO(struct qaic_drm_device *qddev,
		 struct qaic_manage_trans_activate_to_dev *in_trans),
	TP_ARGS(qddev, in_trans),
	TP_STRUCT__entry(
		__string(devname, trace_qaic_devname(qddev))
		__field(__u32, len)
		__field(__u32, queue_size)
		__field(__u32, eventfd)
		__field(__u32, options)
		__field(__u32, pad)
	),
	TP_fast_assign(
		__assign_str(devname, trace_qaic_devname(qddev));
		__entry->len = in_trans->hdr.len;
		__entry->queue_size = in_trans->queue_size;
		__entry->eventfd = in_trans->eventfd;
		__entry->options = in_trans->options;
		__entry->pad = in_trans->pad;
	),
	TP_printk("%s: len %u queue_size %u eventfd %u options %u pad %u",
		__get_str(devname), __entry->len, __entry->queue_size,
		__entry->eventfd, __entry->options, __entry->pad)
);

TRACE_EVENT(qaic_encode_deactivate,
	TP_PROTO(struct qaic_drm_device *qddev,
		 struct qaic_manage_trans_deactivate *in_trans),
	TP_ARGS(qddev, in_trans),
	TP_STRUCT__entry(
		__string(devname, trace_qaic_devname(qddev))
		__field(__u32, len)
		__field(__u32, dbc_id)
	),
	TP_fast_assign(
		__assign_str(devname, trace_qaic_devname(qddev));
		__entry->len = in_trans->hdr.len;
		__entry->dbc_id = in_trans->dbc_id;
	),
	TP_printk("%s: len %u dbc_id %u",
		__get_str(devname), __entry->len, __entry->dbc_id)
);

TRACE_EVENT(qaic_encode_status,
	TP_PROTO(struct qaic_drm_device *qddev,
		 struct qaic_manage_trans_status_to_dev *in_trans),
	TP_ARGS(qddev, in_trans),
	TP_STRUCT__entry(
		__string(devname, trace_qaic_devname(qddev))
		__field(__u32, len)
	),
	TP_fast_assign(
		__assign_str(devname, trace_qaic_devname(qddev));
		__entry->len = in_trans->hdr.len;
	),
	TP_printk("%s: len %u", __get_str(devname), __entry->len)
);

TRACE_EVENT(qaic_decode_passthrough,
	TP_PROTO(struct qaic_drm_device *qddev,
		 struct qaic_manage_trans_passthrough *out_trans),
	TP_ARGS(qddev, out_trans),
	TP_STRUCT__entry(
		__string(devname, trace_qaic_devname(qddev))
		__field(__u32, len)
	),
	TP_fast_assign(
		__assign_str(devname, trace_qaic_devname(qddev));
		__entry->len = out_trans->hdr.len;
	),
	TP_printk("%s: len %u",
		__get_str(devname), __entry->len)
);

TRACE_EVENT(qaic_decode_activate,
	TP_PROTO(struct qaic_drm_device *qddev,
		 struct qaic_manage_trans_activate_from_dev *out_trans),
	TP_ARGS(qddev, out_trans),
	TP_STRUCT__entry(
		__string(devname, trace_qaic_devname(qddev))
		__field(__u32, len)
		__field(__u32, status)
		__field(__u32, dbc_id)
		__field(__u64, options)
	),
	TP_fast_assign(
		__assign_str(devname, trace_qaic_devname(qddev));
		__entry->len = out_trans->hdr.len;
		__entry->status = out_trans->status;
		__entry->dbc_id = out_trans->dbc_id;
		__entry->options = out_trans->options;
	),
	TP_printk("%s: len %u status %u dbc_id %u options %llu",
		__get_str(devname), __entry->len, __entry->status,
		__entry->dbc_id, __entry->options)
);

TRACE_EVENT(qaic_decode_deactivate,
	TP_PROTO(struct qaic_drm_device *qddev, u32 dbc_id, u32 status),
	TP_ARGS(qddev, dbc_id, status),
	TP_STRUCT__entry(
		__string(devname, trace_qaic_devname(qddev))
		__field(u32, dbc_id)
		__field(u32, status)
	),
	TP_fast_assign(
		__assign_str(devname, trace_qaic_devname(qddev));
		__entry->dbc_id = dbc_id;
		__entry->status = status;
	),
	TP_printk("%s: dbc_id %u status %u",
		__get_str(devname), __entry->dbc_id, __entry->status)
);

TRACE_EVENT(qaic_decode_status,
	TP_PROTO(struct qaic_drm_device *qddev,
		 struct qaic_manage_trans_status_from_dev *out_trans),
	TP_ARGS(qddev, out_trans),
	TP_STRUCT__entry(
		__string(devname, trace_qaic_devname(qddev))
		__field(__u32, len)
		__field(__u16, major)
		__field(__u16, minor)
		__field(__u32, status)
		__field(__u64, status_flags)
	),
	TP_fast_assign(
		__assign_str(devname, trace_qaic_devname(qddev));
		__entry->len = out_trans->hdr.len;
		__entry->major = out_trans->major;
		__entry->minor = out_trans->minor;
		__entry->status = out_trans->status;
		__entry->status_flags = out_trans->status_flags;
	),
	TP_printk("%s: len %u major %u minor %u status %u flags 0x%llx",
		__get_str(devname), __entry->len, __entry->major, __entry->minor,
		__entry->status, __entry->status_flags)
);

TRACE_EVENT(qaic_dbc_req,
	TP_PROTO(struct qaic_bo *bo, struct dbc_req *req),
	TP_ARGS(bo, req),
	TP_STRUCT__entry(
		__field(u32, handle)
		__field(u32, dbc_id)
		__field(u16, req_id)
		__field(u8, seq_id)
		__field(u8, cmd)
		__field(u64, src_addr)
		__field(u64, dest_addr)
		__field(u32, len)
		__field(u64, db_addr)
		__field(u8, db_len)
		__field(u32, db_data)
		__field(u32, sem_cmd0)
		__field(u32, sem_cmd1)
		__field(u32, sem_cmd2)
		__field(u32, sem_cmd3)
	),
	TP_fast_assign(
		__entry->handle = bo->handle;
		__entry->dbc_id = bo->dbc->id;
		__entry->req_id = le16_to_cpu(req->req_id);
		__entry->seq_id = req->seq_id;
		__entry->cmd = req->cmd;
		__entry->src_addr = le64_to_cpu(req->src_addr);
		__entry->dest_addr = le64_to_cpu(req->dest_addr);
		__entry->len = le32_to_cpu(req->len);
		__entry->db_addr = le64_to_cpu(req->db_addr);
		__entry->db_len = req->db_len;
		__entry->db_data = le32_to_cpu(req->db_data);
		__entry->sem_cmd0 = le32_to_cpu(req->sem_cmd0);
		__entry->sem_cmd1 = le32_to_cpu(req->sem_cmd1);
		__entry->sem_cmd2 = le32_to_cpu(req->sem_cmd2);
		__entry->sem_cmd3 = le32_to_cpu(req->sem_cmd3);
	),
	TP_printk("\n"
		  "buf handle=%d\n"
		  "dbc_id=%u\n"
		  "req_id=%hu\n"
		  "seq_id=%hhu\n"
		  "cmd=%#04x\n"
		  "\t[7]%#03lx %s\n"
		  "\t[6:5] Reserved\n"
		  "\t[4] %#03lx %s\n"
		  "\t[3]%#03lx %s\n"
		  "\t[2] Reserved\n"
		  "\t[1:0] %#03lx %s\n"
		  "src_addr=%#018llx\n"
		  "dest_addr=%#018llx\n"
		  "len=%u\n"
		  "db_addr=%#018llx\n"
		  "db_len=%hhu\n"
		  "db_data=%u\n"
		  "sem_cmd0=%#010x\n"
		  "\t[11:0] %#06lx Semaphore value\n"
		  "\t[15:12] Reserved\n"
		  "\t[20:16] %#04lx Semaphore index\n"
		  "\t[21] Reserved\n"
		  "\t[22] %#03lx Semaphore Sync\n"
		  "\t[23] Reserved\n"
		  "\t[26:24] %#03lx Semaphore command\n"
		  "\t[28:27] Reserved\n"
		  "\t[29] %#03lx Semaphore DMA out bound sync fence\n"
		  "\t[30] %#03lx Semaphore DMA in bound sync fence\n"
		  "\t[31] %#03lx Enable semaphore command\n"
		  "sem_cmd1=%#010x\n"
		  "\t[11:0] %#06lx Semaphore value\n"
		  "\t[15:12] Reserved\n"
		  "\t[20:16] %#04lx Semaphore index\n"
		  "\t[21] Reserved\n"
		  "\t[22] %#03lx Semaphore Sync\n"
		  "\t[23] Reserved\n"
		  "\t[26:24] %#03lx Semaphore command\n"
		  "\t[28:27] Reserved\n"
		  "\t[29] %#03lx Semaphore DMA out bound sync fence\n"
		  "\t[30] %#03lx Semaphore DMA in bound sync fence\n"
		  "\t[31] %#03lx Enable semaphore command\n"
		  "sem_cmd2=%#010x\n"
		  "\t[11:0] %#06lx Semaphore value\n"
		  "\t[15:12] Reserved\n"
		  "\t[20:16] %#04lx Semaphore index\n"
		  "\t[21] Reserved\n"
		  "\t[22] %#03lx Semaphore Sync\n"
		  "\t[23] Reserved\n"
		  "\t[26:24] %#03lx Semaphore command\n"
		  "\t[28:27] Reserved\n"
		  "\t[29] %#03lx Semaphore DMA out bound sync fence\n"
		  "\t[30] %#03lx Semaphore DMA in bound sync fence\n"
		  "\t[31] %#03lx Enable semaphore command\n"
		  "sem_cmd3=%#010x\n"
		  "\t[11:0] %#06lx Semaphore value\n"
		  "\t[15:12] Reserved\n"
		  "\t[20:16] %#04lx Semaphore index\n"
		  "\t[21] Reserved\n"
		  "\t[22] %#03lx Semaphore Sync\n"
		  "\t[23] Reserved\n"
		  "\t[26:24] %#03lx Semaphore command\n"
		  "\t[28:27] Reserved\n"
		  "\t[29] %#03lx Semaphore DMA out bound sync fence\n"
		  "\t[30] %#03lx Semaphore DMA in bound sync fence\n"
		  "\t[31] %#03lx Enable semaphore command",
		  __entry->handle,
		  __entry->dbc_id,
		  __entry->req_id,
		  __entry->seq_id,
		  __entry->cmd,
		  __entry->cmd & BIT(7),
		  __entry->cmd & BIT(7) ? "Force to generate MSI after DMA is completed" : "Do not force to generate MSI after DMA is completed",
		  (__entry->cmd & BIT(4)) >> 4,
		  (__entry->cmd & BIT(4)) >> 4 ? "Generate completion element in the response queue" : "No Completion Code",
		  (__entry->cmd & BIT(3)) >> 3,
		  (__entry->cmd & BIT(3)) >> 3 ? "DMA request is a Bulk transfer" : "DMA request is a Link list transfer",
		  __entry->cmd & GENMASK(1, 0),
		  __entry->cmd & BIT(0) ?
		  (__entry->cmd & BIT(1)) >> 1 ? "NA" : "DMA transfer inbound"
		  : (__entry->cmd & BIT(1)) >> 1 ? "DMA transfer outbound" : "No DMA transfer involved",
		  __entry->src_addr,
		  __entry->dest_addr,
		  __entry->len,
		  __entry->db_addr,
		  __entry->db_len,
		  __entry->db_data,
		  __entry->sem_cmd0,
		  __entry->sem_cmd0 & GENMASK(11, 0),
		  (__entry->sem_cmd0 & GENMASK(20, 16)) >> 16,
		  (__entry->sem_cmd0 & BIT(22)) >> 22,
		  (__entry->sem_cmd0 & GENMASK(26, 24)) >> 24,
		  (__entry->sem_cmd0 & BIT(29)) >> 29,
		  (__entry->sem_cmd0 & BIT(30)) >> 30,
		  (__entry->sem_cmd0 & BIT(31)) >> 31,
		  __entry->sem_cmd1,
		  __entry->sem_cmd1 & GENMASK(11, 0),
		  (__entry->sem_cmd1 & GENMASK(20, 16)) >> 16,
		  (__entry->sem_cmd1 & BIT(22)) >> 22,
		  (__entry->sem_cmd1 & GENMASK(26, 24)) >> 24,
		  (__entry->sem_cmd1 & BIT(29)) >> 29,
		  (__entry->sem_cmd1 & BIT(30)) >> 30,
		  (__entry->sem_cmd1 & BIT(31)) >> 31,
		  __entry->sem_cmd2,
		  __entry->sem_cmd2 & GENMASK(11, 0),
		  (__entry->sem_cmd2 & GENMASK(20, 16)) >> 16,
		  (__entry->sem_cmd2 & BIT(22)) >> 22,
		  (__entry->sem_cmd2 & GENMASK(26, 24)) >> 24,
		  (__entry->sem_cmd2 & BIT(29)) >> 29,
		  (__entry->sem_cmd2 & BIT(30)) >> 30,
		  (__entry->sem_cmd2 & BIT(31)) >> 31,
		  __entry->sem_cmd3,
		  __entry->sem_cmd3 & GENMASK(11, 0),
		  (__entry->sem_cmd3 & GENMASK(20, 16)) >> 16,
		  (__entry->sem_cmd3 & BIT(22)) >> 22,
		  (__entry->sem_cmd3 & GENMASK(26, 24)) >> 24,
		  (__entry->sem_cmd3 & BIT(29)) >> 29,
		  (__entry->sem_cmd3 & BIT(30)) >> 30,
		  (__entry->sem_cmd3 & BIT(31)) >> 31
		  )
);

#endif /* _QAIC_TRACE_H_ */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#include <trace/define_trace.h>
