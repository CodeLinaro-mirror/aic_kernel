// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/mhi.h>
#include "mhi_internal.h"

static int of_parse_ev_cfg(struct mhi_controller *mhi_cntrl,
			   struct device_node *of_node)
{
	int i, ret, num = 0;
	struct mhi_event *mhi_event;
	struct device_node *child;

	for_each_available_child_of_node(of_node, child) {
		if (!strcmp(child->name, "mhi_event"))
			num++;
	}

	if (!num)
		return -EINVAL;

	mhi_cntrl->total_ev_rings = num;
	mhi_cntrl->mhi_event = kcalloc(num, sizeof(*mhi_cntrl->mhi_event),
				       GFP_KERNEL);
	if (!mhi_cntrl->mhi_event)
		return -ENOMEM;

	/* populate ev ring */
	mhi_event = mhi_cntrl->mhi_event;
	i = 0;
	for_each_available_child_of_node(of_node, child) {
		if (strcmp(child->name, "mhi_event"))
			continue;

		mhi_event->er_index = i++;
		ret = of_property_read_u32(child, "mhi,num-elements",
					   (u32 *)&mhi_event->ring.elements);
		if (ret)
			goto error_ev_cfg;

		ret = of_property_read_u32(child, "mhi,intmod",
					   &mhi_event->intmod);
		if (ret)
			goto error_ev_cfg;

		ret = of_property_read_u32(child, "mhi,msi",
					   &mhi_event->msi);
		if (ret)
			goto error_ev_cfg;

		ret = of_property_read_u32(child, "mhi,chan",
					   &mhi_event->chan);
		if (!ret) {
			if (mhi_event->chan >= mhi_cntrl->max_chan)
				goto error_ev_cfg;
			/* this event ring has a dedicated channel */
			mhi_event->mhi_chan =
				&mhi_cntrl->mhi_chan[mhi_event->chan];
		}

		ret = of_property_read_u32(child, "mhi,priority",
					   &mhi_event->priority);
		if (ret)
			goto error_ev_cfg;

		ret = of_property_read_u32(child, "mhi,brstmode",
					   &mhi_event->db_cfg.brstmode);
		if (ret || MHI_INVALID_BRSTMODE(mhi_event->db_cfg.brstmode))
			goto error_ev_cfg;

		mhi_event->db_cfg.process_db =
			(mhi_event->db_cfg.brstmode == MHI_BRSTMODE_ENABLE) ?
			mhi_db_brstmode : mhi_db_brstmode_disable;

		ret = of_property_read_u32(child, "mhi,data-type",
					   &mhi_event->data_type);
		if (ret)
			mhi_event->data_type = MHI_ER_DATA_ELEMENT_TYPE;

		if (mhi_event->data_type > MHI_ER_DATA_TYPE_MAX)
			goto error_ev_cfg;

		switch (mhi_event->data_type) {
		case MHI_ER_DATA_ELEMENT_TYPE:
			mhi_event->process_event = mhi_process_data_event_ring;
			break;
		case MHI_ER_CTRL_ELEMENT_TYPE:
			mhi_event->process_event = mhi_process_ctrl_ev_ring;
			break;
		}

		mhi_event->hw_ring = of_property_read_bool(child, "mhi,hw-ev");
		if (mhi_event->hw_ring)
			mhi_cntrl->hw_ev_rings++;
		else
			mhi_cntrl->sw_ev_rings++;
		mhi_event->cl_manage = of_property_read_bool(child,
							"mhi,client-manage");
		mhi_event->offload_ev = of_property_read_bool(child,
							      "mhi,offload");
		mhi_event++;
	}

	/* we need msi for each event ring + additional one for BHI */
	mhi_cntrl->msi_required = mhi_cntrl->total_ev_rings + 1;

	return 0;

error_ev_cfg:

	kfree(mhi_cntrl->mhi_event);
	return -EINVAL;
}
static int of_parse_ch_cfg(struct mhi_controller *mhi_cntrl,
			   struct device_node *of_node)
{
	int ret;
	struct device_node *child;
	u32 chan;

	ret = of_property_read_u32(of_node, "mhi,max-channels",
				   &mhi_cntrl->max_chan);
	if (ret)
		return ret;

	mhi_cntrl->mhi_chan = kcalloc(mhi_cntrl->max_chan,
				      sizeof(*mhi_cntrl->mhi_chan), GFP_KERNEL);
	if (!mhi_cntrl->mhi_chan)
		return -ENOMEM;

	INIT_LIST_HEAD(&mhi_cntrl->lpm_chans);

	/* populate channel configurations */
	for_each_available_child_of_node(of_node, child) {
		struct mhi_chan *mhi_chan;

		if (strcmp(child->name, "mhi_chan"))
			continue;

		ret = of_property_read_u32(child, "reg", &chan);
		if (ret || chan >= mhi_cntrl->max_chan)
			goto error_chan_cfg;

		mhi_chan = &mhi_cntrl->mhi_chan[chan];

		ret = of_property_read_string(child, "label",
					      &mhi_chan->name);
		if (ret)
			goto error_chan_cfg;

		mhi_chan->chan = chan;

		ret = of_property_read_u32(child, "mhi,num-elements",
					   (u32 *)&mhi_chan->buf_ring.elements);
		if (!ret && !mhi_chan->buf_ring.elements)
			goto error_chan_cfg;

		mhi_chan->tre_ring.elements = mhi_chan->buf_ring.elements;

		ret = of_property_read_u32(child, "mhi,event-ring",
					   &mhi_chan->er_index);
		if (ret)
			goto error_chan_cfg;

		ret = of_property_read_u32(child, "mhi,chan-dir",
					   &mhi_chan->dir);
		if (ret)
			goto error_chan_cfg;

		ret = of_property_read_u32(child, "mhi,ee", &mhi_chan->ee);
		if (ret || mhi_chan->ee >= MHI_EE_MAX_SUPPORTED)
			goto error_chan_cfg;

		of_property_read_u32(child, "mhi,pollcfg",
				     &mhi_chan->db_cfg.pollcfg);

		ret = of_property_read_u32(child, "mhi,data-type",
					   &mhi_chan->xfer_type);
		if (ret)
			goto error_chan_cfg;

		switch (mhi_chan->xfer_type) {
		case MHI_XFER_BUFFER:
			mhi_chan->gen_tre = mhi_gen_tre;
			mhi_chan->queue_xfer = mhi_queue_buf;
			break;
		case MHI_XFER_SKB:
			mhi_chan->queue_xfer = mhi_queue_skb;
			break;
		case MHI_XFER_SCLIST:
			mhi_chan->gen_tre = mhi_gen_tre;
			mhi_chan->queue_xfer = mhi_queue_sclist;
			break;
		case MHI_XFER_NOP:
			mhi_chan->queue_xfer = mhi_queue_nop;
			break;
		default:
			goto error_chan_cfg;
		}

		mhi_chan->lpm_notify = of_property_read_bool(child,
							     "mhi,lpm-notify");
		mhi_chan->offload_ch = of_property_read_bool(child,
							"mhi,offload-chan");
		mhi_chan->db_cfg.reset_req = of_property_read_bool(child,
							"mhi,db-mode-switch");
		mhi_chan->pre_alloc = of_property_read_bool(child,
							    "mhi,auto-queue");
		mhi_chan->auto_start = of_property_read_bool(child,
							     "mhi,auto-start");

		if (mhi_chan->pre_alloc &&
		    (mhi_chan->dir != DMA_FROM_DEVICE ||
		     mhi_chan->xfer_type != MHI_XFER_BUFFER))
			goto error_chan_cfg;

		/* bi-dir and dirctionless channels must be a offload chan */
		if ((mhi_chan->dir == DMA_BIDIRECTIONAL ||
		     mhi_chan->dir == DMA_NONE) && !mhi_chan->offload_ch)
			goto error_chan_cfg;

		/* if mhi host allocate the buffers then client cannot queue */
		if (mhi_chan->pre_alloc)
			mhi_chan->queue_xfer = mhi_queue_nop;

		if (!mhi_chan->offload_ch) {
			ret = of_property_read_u32(child, "mhi,doorbell-mode",
						   &mhi_chan->db_cfg.brstmode);
			if (ret ||
			    MHI_INVALID_BRSTMODE(mhi_chan->db_cfg.brstmode))
				goto error_chan_cfg;

			mhi_chan->db_cfg.process_db =
				(mhi_chan->db_cfg.brstmode ==
				 MHI_BRSTMODE_ENABLE) ?
				mhi_db_brstmode : mhi_db_brstmode_disable;
		}

		mhi_chan->configured = true;

		if (mhi_chan->lpm_notify)
			list_add_tail(&mhi_chan->node, &mhi_cntrl->lpm_chans);
	}

	return 0;

error_chan_cfg:
	kfree(mhi_cntrl->mhi_chan);

	return -EINVAL;
}

static int of_parse_dt(struct mhi_controller *mhi_cntrl,
		       struct device_node *of_node)
{
	int ret;

	/* parse MHI channel configuration */
	ret = of_parse_ch_cfg(mhi_cntrl, of_node);
	if (ret)
		return ret;

	/* parse MHI event configuration */
	ret = of_parse_ev_cfg(mhi_cntrl, of_node);
	if (ret)
		goto error_ev_cfg;

	ret = of_property_read_u32(of_node, "mhi,timeout",
				   &mhi_cntrl->timeout_ms);
	if (ret)
		mhi_cntrl->timeout_ms = MHI_TIMEOUT_MS;

	mhi_cntrl->bounce_buf = of_property_read_bool(of_node, "mhi,use-bb");
	ret = of_property_read_u32(of_node, "mhi,buffer-len",
				   (u32 *)&mhi_cntrl->buffer_len);
	if (ret)
		mhi_cntrl->buffer_len = MHI_MAX_MTU;

	return 0;

error_ev_cfg:
	kfree(mhi_cntrl->mhi_chan);

	return ret;
}

int of_register_mhi_controller(struct mhi_controller *mhi_cntrl)
{
	int ret;
	int i;
	struct mhi_event *mhi_event;
	struct mhi_chan *mhi_chan;
	struct mhi_cmd *mhi_cmd;
	struct mhi_device *mhi_dev;

	if (!mhi_cntrl->of_node)
		return -EINVAL;

	if (!mhi_cntrl->runtime_get || !mhi_cntrl->runtime_put)
		return -EINVAL;

	if (!mhi_cntrl->status_cb || !mhi_cntrl->link_status)
		return -EINVAL;

	ret = of_parse_dt(mhi_cntrl, mhi_cntrl->of_node);
	if (ret)
		return -EINVAL;

	mhi_cntrl->mhi_cmd = kcalloc(NR_OF_CMD_RINGS,
				     sizeof(*mhi_cntrl->mhi_cmd), GFP_KERNEL);
	if (!mhi_cntrl->mhi_cmd) {
		ret = -ENOMEM;
		goto error_alloc_cmd;
	}

	INIT_LIST_HEAD(&mhi_cntrl->transition_list);
	mutex_init(&mhi_cntrl->pm_mutex);
	rwlock_init(&mhi_cntrl->pm_lock);
	spin_lock_init(&mhi_cntrl->transition_lock);
	spin_lock_init(&mhi_cntrl->wlock);
	init_waitqueue_head(&mhi_cntrl->state_event);

	mhi_cmd = mhi_cntrl->mhi_cmd;
	for (i = 0; i < NR_OF_CMD_RINGS; i++, mhi_cmd++)
		spin_lock_init(&mhi_cmd->lock);

	mhi_event = mhi_cntrl->mhi_event;
	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, mhi_event++) {
		if (mhi_event->offload_ev)
			continue;

		mhi_event->mhi_cntrl = mhi_cntrl;
		spin_lock_init(&mhi_event->lock);
		if (mhi_event->data_type == MHI_ER_CTRL_ELEMENT_TYPE)
			tasklet_init(&mhi_event->task, mhi_ctrl_ev_task,
				     (ulong)mhi_event);
		else
			tasklet_init(&mhi_event->task, mhi_ev_task,
				     (ulong)mhi_event);
	}

	mhi_chan = mhi_cntrl->mhi_chan;
	for (i = 0; i < mhi_cntrl->max_chan; i++, mhi_chan++) {
		mutex_init(&mhi_chan->mutex);
		init_completion(&mhi_chan->completion);
		rwlock_init(&mhi_chan->lock);
	}

	if (mhi_cntrl->bounce_buf) {
		mhi_cntrl->map_single = mhi_map_single_use_bb;
		mhi_cntrl->unmap_single = mhi_unmap_single_use_bb;
	} else {
		mhi_cntrl->map_single = mhi_map_single_no_bb;
		mhi_cntrl->unmap_single = mhi_unmap_single_no_bb;
	}

	/* register controller with mhi_bus */
	mhi_dev = mhi_alloc_device(mhi_cntrl);
	if (!mhi_dev) {
		ret = -ENOMEM;
		goto error_alloc_dev;
	}

	mhi_dev->dev_type = MHI_CONTROLLER_TYPE;
	mhi_dev->mhi_cntrl = mhi_cntrl;
	dev_set_name(&mhi_dev->dev, "%04x_%02u.%02u.%02u", mhi_dev->dev_id,
		     mhi_dev->domain, mhi_dev->bus, mhi_dev->slot);
	ret = device_add(&mhi_dev->dev);
	if (ret)
		goto error_add_dev;

	mhi_cntrl->mhi_dev = mhi_dev;

	mhi_cntrl->parent = debugfs_lookup(mhi_bus_type.name, NULL);

	return 0;

error_add_dev:
	mhi_dealloc_device(mhi_cntrl, mhi_dev);

error_alloc_dev:
	kfree(mhi_cntrl->mhi_cmd);

error_alloc_cmd:
	kfree(mhi_cntrl->mhi_chan);
	kfree(mhi_cntrl->mhi_event);

	return ret;
};
EXPORT_SYMBOL(of_register_mhi_controller);

void mhi_unregister_mhi_controller(struct mhi_controller *mhi_cntrl)
{
	struct mhi_device *mhi_dev = mhi_cntrl->mhi_dev;

	kfree(mhi_cntrl->mhi_cmd);
	kfree(mhi_cntrl->mhi_event);
	kfree(mhi_cntrl->mhi_chan);

	device_del(&mhi_dev->dev);
	put_device(&mhi_dev->dev);
}

/* set ptr to control private data */
static inline void mhi_controller_set_devdata(struct mhi_controller *mhi_cntrl,
					 void *priv)
{
	mhi_cntrl->priv_data = priv;
}


/* allocate mhi controller to register */
struct mhi_controller *mhi_alloc_controller(size_t size)
{
	struct mhi_controller *mhi_cntrl;

	mhi_cntrl = kzalloc(size + sizeof(*mhi_cntrl), GFP_KERNEL);

	if (mhi_cntrl && size)
		mhi_controller_set_devdata(mhi_cntrl, mhi_cntrl + 1);

	return mhi_cntrl;
}
EXPORT_SYMBOL(mhi_alloc_controller);

/* match dev to drv */
static int mhi_match(struct device *dev, struct device_driver *drv)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);
	struct mhi_driver *mhi_drv = to_mhi_driver(drv);
	const struct mhi_device_id *id;

	/* if controller type there is no client driver associated with it */
	if (mhi_dev->dev_type == MHI_CONTROLLER_TYPE)
		return 0;

	for (id = mhi_drv->id_table; id->chan[0]; id++)
		if (!strcmp(mhi_dev->chan_name, id->chan)) {
			mhi_dev->id = id;
			return 1;
		}

	return 0;
};

static void mhi_release_device(struct device *dev)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);

	kfree(mhi_dev);
}

struct bus_type mhi_bus_type = {
	.name = "mhi",
	.dev_name = "mhi",
	.match = mhi_match,
};

static int mhi_driver_probe(struct device *dev)
{
	return -EINVAL;
}

static int mhi_driver_remove(struct device *dev)
{
	return 0;
}

int mhi_driver_register(struct mhi_driver *mhi_drv)
{
	struct device_driver *driver = &mhi_drv->driver;

	if (!mhi_drv->probe || !mhi_drv->remove)
		return -EINVAL;

	driver->bus = &mhi_bus_type;
	driver->probe = mhi_driver_probe;
	driver->remove = mhi_driver_remove;

	return driver_register(driver);
}
EXPORT_SYMBOL(mhi_driver_register);

void mhi_driver_unregister(struct mhi_driver *mhi_drv)
{
	driver_unregister(&mhi_drv->driver);
}
EXPORT_SYMBOL(mhi_driver_unregister);

struct mhi_device *mhi_alloc_device(struct mhi_controller *mhi_cntrl)
{
	struct mhi_device *mhi_dev = kzalloc(sizeof(*mhi_dev), GFP_KERNEL);
	struct device *dev;

	if (!mhi_dev)
		return NULL;

	dev = &mhi_dev->dev;
	device_initialize(dev);
	dev->bus = &mhi_bus_type;
	dev->release = mhi_release_device;
	dev->parent = mhi_cntrl->dev;
	mhi_dev->mhi_cntrl = mhi_cntrl;
	mhi_dev->dev_id = mhi_cntrl->dev_id;
	mhi_dev->domain = mhi_cntrl->domain;
	mhi_dev->bus = mhi_cntrl->bus;
	mhi_dev->slot = mhi_cntrl->slot;
	mhi_dev->mtu = MHI_MAX_MTU;
	atomic_set(&mhi_dev->dev_wake, 0);

	return mhi_dev;
}

static int __init mhi_init(void)
{
	/* parent directory */
	debugfs_create_dir(mhi_bus_type.name, NULL);

	return bus_register(&mhi_bus_type);
}
postcore_initcall(mhi_init);

MODULE_LICENSE("GPL v2");
MODULE_ALIAS("MHI_CORE");
MODULE_DESCRIPTION("MHI Host Interface");
