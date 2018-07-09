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
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/mhi.h>
#include "mhi_internal.h"

int __must_check mhi_read_reg(struct mhi_controller *mhi_cntrl,
			      void __iomem *base,
			      u32 offset,
			      u32 *out)
{
	u32 tmp = readl_relaxed(base + offset);

	/* unexpected value, query the link status */
	if (PCI_INVALID_READ(tmp) &&
	    mhi_cntrl->link_status(mhi_cntrl, mhi_cntrl->priv_data))
		return -EIO;

	*out = tmp;

	return 0;
}

int __must_check mhi_read_reg_field(struct mhi_controller *mhi_cntrl,
				    void __iomem *base,
				    u32 offset,
				    u32 mask,
				    u32 shift,
				    u32 *out)
{
	u32 tmp;
	int ret;

	ret = mhi_read_reg(mhi_cntrl, base, offset, &tmp);
	if (ret)
		return ret;

	*out = (tmp & mask) >> shift;

	return 0;
}

void mhi_write_reg(struct mhi_controller *mhi_cntrl,
		   void __iomem *base,
		   u32 offset,
		   u32 val)
{
	writel_relaxed(val, base + offset);
}

void mhi_write_reg_field(struct mhi_controller *mhi_cntrl,
			 void __iomem *base,
			 u32 offset,
			 u32 mask,
			 u32 shift,
			 u32 val)
{
	int ret;
	u32 tmp;

	ret = mhi_read_reg(mhi_cntrl, base, offset, &tmp);
	if (ret)
		return;

	tmp &= ~mask;
	tmp |= (val << shift);
	mhi_write_reg(mhi_cntrl, base, offset, tmp);
}

void mhi_write_db(struct mhi_controller *mhi_cntrl,
		  void __iomem *db_addr,
		  dma_addr_t wp)
{
	mhi_write_reg(mhi_cntrl, db_addr, 4, upper_32_bits(wp));
	mhi_write_reg(mhi_cntrl, db_addr, 0, lower_32_bits(wp));
}

void mhi_db_brstmode(struct mhi_controller *mhi_cntrl,
		     struct db_cfg *db_cfg,
		     void __iomem *db_addr,
		     dma_addr_t wp)
{
	if (db_cfg->db_mode) {
		db_cfg->db_val = wp;
		mhi_write_db(mhi_cntrl, db_addr, wp);
		db_cfg->db_mode = 0;
	}
}

void mhi_db_brstmode_disable(struct mhi_controller *mhi_cntrl,
			     struct db_cfg *db_cfg,
			     void __iomem *db_addr,
			     dma_addr_t wp)
{
	db_cfg->db_val = wp;
	mhi_write_db(mhi_cntrl, db_addr, wp);
}

void mhi_ring_er_db(struct mhi_event *mhi_event)
{
	struct mhi_ring *ring = &mhi_event->ring;

	mhi_event->db_cfg.process_db(mhi_event->mhi_cntrl, &mhi_event->db_cfg,
				     ring->db_addr, *ring->ctxt_wp);
}

void mhi_ring_cmd_db(struct mhi_controller *mhi_cntrl, struct mhi_cmd *mhi_cmd)
{
	dma_addr_t db;
	struct mhi_ring *ring = &mhi_cmd->ring;

	db = ring->iommu_base + (ring->wp - ring->base);
	*ring->ctxt_wp = db;
	mhi_write_db(mhi_cntrl, ring->db_addr, db);
}

void mhi_ring_chan_db(struct mhi_controller *mhi_cntrl,
		      struct mhi_chan *mhi_chan)
{
	struct mhi_ring *ring = &mhi_chan->tre_ring;
	dma_addr_t db;

	db = ring->iommu_base + (ring->wp - ring->base);
	*ring->ctxt_wp = db;
	mhi_chan->db_cfg.process_db(mhi_cntrl, &mhi_chan->db_cfg, ring->db_addr,
				    db);
}

enum MHI_EE mhi_get_exec_env(struct mhi_controller *mhi_cntrl)
{
	u32 exec;
	int ret = mhi_read_reg(mhi_cntrl, mhi_cntrl->bhi, BHI_EXECENV, &exec);

	return (ret) ? MHI_EE_MAX : exec;
}

enum MHI_STATE mhi_get_m_state(struct mhi_controller *mhi_cntrl)
{
	u32 state;
	int ret = mhi_read_reg_field(mhi_cntrl, mhi_cntrl->regs, MHISTATUS,
				     MHISTATUS_MHISTATE_MASK,
				     MHISTATUS_MHISTATE_SHIFT, &state);
	return ret ? MHI_STATE_MAX : state;
}

int mhi_map_single_no_bb(struct mhi_controller *mhi_cntrl,
			 struct mhi_buf_info *buf_info)
{
	return -ENOMEM;
}

int mhi_map_single_use_bb(struct mhi_controller *mhi_cntrl,
			  struct mhi_buf_info *buf_info)
{
	return -ENOMEM;
}

void mhi_unmap_single_no_bb(struct mhi_controller *mhi_cntrl,
			    struct mhi_buf_info *buf_info)
{
}

void mhi_unmap_single_use_bb(struct mhi_controller *mhi_cntrl,
			    struct mhi_buf_info *buf_info)
{
}

int mhi_queue_sclist(struct mhi_device *mhi_dev,
		     struct mhi_chan *mhi_chan,
		     void *buf,
		     size_t len,
		     enum MHI_FLAGS mflags)
{
	return -EINVAL;
}

int mhi_queue_nop(struct mhi_device *mhi_dev,
		  struct mhi_chan *mhi_chan,
		  void *buf,
		  size_t len,
		  enum MHI_FLAGS mflags)
{
	return -EINVAL;
}

static void *mhi_to_virtual(struct mhi_ring *ring, dma_addr_t addr)
{
	return (addr - ring->iommu_base) + ring->base;
}

dma_addr_t mhi_to_physical(struct mhi_ring *ring, void *addr)
{
	return (addr - ring->base) + ring->iommu_base;
}

static void mhi_recycle_ev_ring_element(struct mhi_controller *mhi_cntrl,
					struct mhi_ring *ring)
{
	dma_addr_t ctxt_wp;

	/* update the WP */
	ring->wp += ring->el_size;
	ctxt_wp = *ring->ctxt_wp + ring->el_size;

	if (ring->wp >= (ring->base + ring->len)) {
		ring->wp = ring->base;
		ctxt_wp = ring->iommu_base;
	}

	*ring->ctxt_wp = ctxt_wp;

	/* update the RP */
	ring->rp += ring->el_size;
	if (ring->rp >= (ring->base + ring->len))
		ring->rp = ring->base;

	/* visible to other cores */
	smp_wmb();
}

int mhi_queue_skb(struct mhi_device *mhi_dev,
		  struct mhi_chan *mhi_chan,
		  void *buf,
		  size_t len,
		  enum MHI_FLAGS mflags)
{
	return -EINVAL;
}

int mhi_gen_tre(struct mhi_controller *mhi_cntrl,
		struct mhi_chan *mhi_chan,
		void *buf,
		void *cb,
		size_t buf_len,
		enum MHI_FLAGS flags)
{
	return -EINVAL;
}

int mhi_queue_buf(struct mhi_device *mhi_dev,
		  struct mhi_chan *mhi_chan,
		  void *buf,
		  size_t len,
		  enum MHI_FLAGS mflags)
{
	return -EINVAL;
}

/* destroy specific device */
int mhi_destroy_device(struct device *dev, void *data)
{
	struct mhi_device *mhi_dev;
	struct mhi_controller *mhi_cntrl;

	if (dev->bus != &mhi_bus_type)
		return 0;

	mhi_dev = to_mhi_device(dev);
	mhi_cntrl = mhi_dev->mhi_cntrl;

	/* only destroying virtual devices thats attached to bus */
	if (mhi_dev->dev_type ==  MHI_CONTROLLER_TYPE)
		return 0;

	dev_info(mhi_cntrl->dev, "destroy device for chan:%s\n",
		 mhi_dev->chan_name);

	/* notify the client and remove the device from mhi bus */
	device_del(dev);
	put_device(dev);

	return 0;
}

void mhi_notify(struct mhi_device *mhi_dev, enum MHI_CB cb_reason)
{
	struct mhi_driver *mhi_drv;

	if (!mhi_dev->dev.driver)
		return;

	mhi_drv = to_mhi_driver(mhi_dev->dev.driver);

	if (mhi_drv->status_cb)
		mhi_drv->status_cb(mhi_dev, cb_reason);
}

static void mhi_assign_of_node(struct mhi_controller *mhi_cntrl,
			       struct mhi_device *mhi_dev)
{
	struct device_node *controller, *node;
	const char *dt_name;
	int ret;

	controller = mhi_cntrl->of_node;
	for_each_available_child_of_node(controller, node) {
		ret = of_property_read_string(node, "mhi,chan", &dt_name);
		if (ret)
			continue;
		if (!strcmp(mhi_dev->chan_name, dt_name)) {
			mhi_dev->dev.of_node = node;
			break;
		}
	}
}

/* bind mhi channels into mhi devices */
void mhi_create_devices(struct mhi_controller *mhi_cntrl)
{
	int i;
	struct mhi_chan *mhi_chan;
	struct mhi_device *mhi_dev;
	int ret;

	mhi_chan = mhi_cntrl->mhi_chan;
	for (i = 0; i < mhi_cntrl->max_chan; i++, mhi_chan++) {
		if (!mhi_chan->configured || mhi_chan->ee != mhi_cntrl->ee)
			continue;
		mhi_dev = mhi_alloc_device(mhi_cntrl);
		if (!mhi_dev)
			return;

		mhi_dev->dev_type = MHI_XFER_TYPE;
		switch (mhi_chan->dir) {
		case DMA_TO_DEVICE:
			mhi_dev->ul_chan = mhi_chan;
			mhi_dev->ul_chan_id = mhi_chan->chan;
			mhi_dev->ul_xfer = mhi_chan->queue_xfer;
			mhi_dev->ul_event_id = mhi_chan->er_index;
			break;
		case DMA_NONE:
		case DMA_BIDIRECTIONAL:
			mhi_dev->ul_chan_id = mhi_chan->chan;
		case DMA_FROM_DEVICE:
			/* we use dl_chan for offload channels */
			mhi_dev->dl_chan = mhi_chan;
			mhi_dev->dl_chan_id = mhi_chan->chan;
			mhi_dev->dl_xfer = mhi_chan->queue_xfer;
			mhi_dev->dl_event_id = mhi_chan->er_index;
		}

		mhi_chan->mhi_dev = mhi_dev;

		/* check next channel if it matches */
		if ((i + 1) < mhi_cntrl->max_chan && mhi_chan[1].configured) {
			if (!strcmp(mhi_chan[1].name, mhi_chan->name)) {
				i++;
				mhi_chan++;
				if (mhi_chan->dir == DMA_TO_DEVICE) {
					mhi_dev->ul_chan = mhi_chan;
					mhi_dev->ul_chan_id = mhi_chan->chan;
					mhi_dev->ul_xfer = mhi_chan->queue_xfer;
					mhi_dev->ul_event_id =
						mhi_chan->er_index;
				} else {
					mhi_dev->dl_chan = mhi_chan;
					mhi_dev->dl_chan_id = mhi_chan->chan;
					mhi_dev->dl_xfer = mhi_chan->queue_xfer;
					mhi_dev->dl_event_id =
						mhi_chan->er_index;
				}
				mhi_chan->mhi_dev = mhi_dev;
			}
		}

		mhi_dev->chan_name = mhi_chan->name;
		dev_set_name(&mhi_dev->dev, "%04x_%02u.%02u.%02u_%s",
			     mhi_dev->dev_id, mhi_dev->domain, mhi_dev->bus,
			     mhi_dev->slot, mhi_dev->chan_name);

		/* add if there is a matching DT node */
		mhi_assign_of_node(mhi_cntrl, mhi_dev);

		ret = device_add(&mhi_dev->dev);
		if (ret)
			mhi_dealloc_device(mhi_cntrl, mhi_dev);
	}
}
int mhi_process_ctrl_ev_ring(struct mhi_controller *mhi_cntrl,
			     struct mhi_event *mhi_event,
			     u32 event_quota)
{
	struct mhi_tre *dev_rp, *local_rp;
	struct mhi_ring *ev_ring = &mhi_event->ring;
	struct mhi_event_ctxt *er_ctxt =
		&mhi_cntrl->mhi_ctxt->er_ctxt[mhi_event->er_index];
	int count = 0;

	/*
	 * this is a quick check to avoid unnecessary event processing
	 * in case we already in error state, but it's still possible
	 * to transition to error state while processing events
	 */
	if (unlikely(MHI_EVENT_ACCESS_INVALID(mhi_cntrl->pm_state)))
		return -EIO;

	dev_rp = mhi_to_virtual(ev_ring, er_ctxt->rp);
	local_rp = ev_ring->rp;

	while (dev_rp != local_rp) {
		enum MHI_PKT_TYPE type = MHI_TRE_GET_EV_TYPE(local_rp);

		switch (type) {
		case MHI_PKT_TYPE_STATE_CHANGE_EVENT:
		{
			enum MHI_STATE new_state;

			new_state = MHI_TRE_GET_EV_STATE(local_rp);

			dev_info(mhi_cntrl->dev,
				 "MHI state change event to state:%s\n",
				 TO_MHI_STATE_STR(new_state));

			switch (new_state) {
			case MHI_STATE_M0:
				mhi_pm_m0_transition(mhi_cntrl);
				break;
			case MHI_STATE_M1:
				mhi_pm_m1_transition(mhi_cntrl);
				break;
			case MHI_STATE_M3:
				mhi_pm_m3_transition(mhi_cntrl);
				break;
			case MHI_STATE_SYS_ERR:
			{
				enum MHI_PM_STATE new_state;

				dev_info(mhi_cntrl->dev,
					 "MHI system error detected\n");
				write_lock_irq(&mhi_cntrl->pm_lock);
				new_state = mhi_tryset_pm_state(mhi_cntrl,
							MHI_PM_SYS_ERR_DETECT);
				write_unlock_irq(&mhi_cntrl->pm_lock);
				if (new_state == MHI_PM_SYS_ERR_DETECT)
					schedule_work(
						&mhi_cntrl->syserr_worker);
				break;
			}
			default:
				dev_err(mhi_cntrl->dev, "Unsupported STE:%s\n",
					TO_MHI_STATE_STR(new_state));
			}

			break;
		}
		case MHI_PKT_TYPE_CMD_COMPLETION_EVENT:
			break;
		case MHI_PKT_TYPE_EE_EVENT:
		{
			enum MHI_ST_TRANSITION st = MHI_ST_TRANSITION_MAX;
			enum MHI_EE event = MHI_TRE_GET_EV_EXECENV(local_rp);

			dev_info(mhi_cntrl->dev, "MHI EE received event:%s\n",
				 TO_MHI_EXEC_STR(event));
			switch (event) {
			case MHI_EE_SBL:
				st = MHI_ST_TRANSITION_SBL;
				break;
			case MHI_EE_AMSS:
				st = MHI_ST_TRANSITION_AMSS;
				break;
			case MHI_EE_RDDM:
				mhi_cntrl->status_cb(mhi_cntrl,
						     mhi_cntrl->priv_data,
						     MHI_CB_EE_RDDM);
				/* fall thru to wake up the event */
			case MHI_EE_BHIE:
				write_lock_irq(&mhi_cntrl->pm_lock);
				mhi_cntrl->ee = event;
				write_unlock_irq(&mhi_cntrl->pm_lock);
				wake_up(&mhi_cntrl->state_event);
				break;
			default:
				dev_err(mhi_cntrl->dev, "Unhandled EE event\n");
			}
			if (st != MHI_ST_TRANSITION_MAX)
				mhi_queue_state_transition(mhi_cntrl, st);

			break;
		}
		default:
			break;
		}

		mhi_recycle_ev_ring_element(mhi_cntrl, ev_ring);
		local_rp = ev_ring->rp;
		dev_rp = mhi_to_virtual(ev_ring, er_ctxt->rp);
		count++;
	}

	read_lock_bh(&mhi_cntrl->pm_lock);
	if (likely(MHI_DB_ACCESS_VALID(mhi_cntrl->pm_state)))
		mhi_ring_er_db(mhi_event);
	read_unlock_bh(&mhi_cntrl->pm_lock);

	return count;
}

int mhi_process_data_event_ring(struct mhi_controller *mhi_cntrl,
				struct mhi_event *mhi_event,
				u32 event_quota)
{
	return -EIO;
}

void mhi_ev_task(unsigned long data)
{
}

void mhi_ctrl_ev_task(unsigned long data)
{
	struct mhi_event *mhi_event = (struct mhi_event *)data;
	struct mhi_controller *mhi_cntrl = mhi_event->mhi_cntrl;
	enum MHI_STATE state = MHI_STATE_MAX;
	enum MHI_PM_STATE pm_state = 0;
	int ret;

	/* process ctrl events events */
	ret = mhi_event->process_event(mhi_cntrl, mhi_event, U32_MAX);

	/*
	 * we received a MSI but no events to process maybe device went to
	 * SYS_ERR state, check the state
	 */
	if (!ret) {
		write_lock_irq(&mhi_cntrl->pm_lock);
		if (MHI_REG_ACCESS_VALID(mhi_cntrl->pm_state))
			state = mhi_get_m_state(mhi_cntrl);
		if (state == MHI_STATE_SYS_ERR) {
			dev_info(mhi_cntrl->dev, "MHI system error detected\n");
			pm_state = mhi_tryset_pm_state(mhi_cntrl,
						       MHI_PM_SYS_ERR_DETECT);
		}
		write_unlock_irq(&mhi_cntrl->pm_lock);
		if (pm_state == MHI_PM_SYS_ERR_DETECT)
			schedule_work(&mhi_cntrl->syserr_worker);
	}
}

irqreturn_t mhi_msi_handlr(int irq_number, void *dev)
{
	struct mhi_event *mhi_event = dev;
	struct mhi_controller *mhi_cntrl = mhi_event->mhi_cntrl;
	struct mhi_event_ctxt *er_ctxt =
		&mhi_cntrl->mhi_ctxt->er_ctxt[mhi_event->er_index];
	struct mhi_ring *ev_ring = &mhi_event->ring;
	void *dev_rp = mhi_to_virtual(ev_ring, er_ctxt->rp);

	/* confirm ER has pending events to process before scheduling work */
	if (ev_ring->rp == dev_rp)
		return IRQ_HANDLED;

	/* client managed event ring, notify pending data */
	if (mhi_event->cl_manage) {
		struct mhi_chan *mhi_chan = mhi_event->mhi_chan;
		struct mhi_device *mhi_dev = mhi_chan->mhi_dev;

		if (mhi_dev)
			mhi_dev->status_cb(mhi_dev, MHI_CB_PENDING_DATA);
	} else
		tasklet_schedule(&mhi_event->task);

	return IRQ_HANDLED;
}

/* this is the threaded fn */
irqreturn_t mhi_intvec_threaded_handlr(int irq_number, void *dev)
{
	struct mhi_controller *mhi_cntrl = dev;
	enum MHI_STATE state = MHI_STATE_MAX;
	enum MHI_PM_STATE pm_state = 0;

	write_lock_irq(&mhi_cntrl->pm_lock);
	if (MHI_REG_ACCESS_VALID(mhi_cntrl->pm_state))
		state = mhi_get_m_state(mhi_cntrl);
	if (state == MHI_STATE_SYS_ERR) {
		dev_info(mhi_cntrl->dev, "MHI system error detected\n");
		pm_state = mhi_tryset_pm_state(mhi_cntrl,
					       MHI_PM_SYS_ERR_DETECT);
	}
	write_unlock_irq(&mhi_cntrl->pm_lock);
	if (pm_state == MHI_PM_SYS_ERR_DETECT)
		schedule_work(&mhi_cntrl->syserr_worker);

	return IRQ_HANDLED;
}

irqreturn_t mhi_intvec_handlr(int irq_number, void *dev)
{

	struct mhi_controller *mhi_cntrl = dev;

	/* wake up any events waiting for state change */
	wake_up(&mhi_cntrl->state_event);

	return IRQ_WAKE_THREAD;
}

static int __mhi_bdf_to_controller(struct device *dev, const void *tmp)
{
	struct mhi_device *mhi_dev = to_mhi_device(dev);
	const struct mhi_device *match = tmp;

	/* return any none-zero value if match */
	if (mhi_dev->dev_type == MHI_CONTROLLER_TYPE &&
	    mhi_dev->domain == match->domain && mhi_dev->bus == match->bus &&
	    mhi_dev->slot == match->slot && mhi_dev->dev_id == match->dev_id)
		return 1;

	return 0;
}

struct mhi_controller *mhi_bdf_to_controller(u32 domain,
					     u32 bus,
					     u32 slot,
					     u32 dev_id)
{
	struct mhi_device tmp, *mhi_dev;
	struct device *dev;

	tmp.domain = domain;
	tmp.bus = bus;
	tmp.slot = slot;
	tmp.dev_id = dev_id;

	dev = bus_find_device(&mhi_bus_type, NULL, &tmp,
			      __mhi_bdf_to_controller);
	if (!dev)
		return NULL;

	mhi_dev = to_mhi_device(dev);

	return mhi_dev->mhi_cntrl;
}
EXPORT_SYMBOL(mhi_bdf_to_controller);
