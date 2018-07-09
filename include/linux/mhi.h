/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 *
 */
#ifndef _MHI_H_
#define _MHI_H_

struct mhi_chan;
struct mhi_event;
struct mhi_ctxt;
struct mhi_cmd;
struct mhi_buf_info;

/**
 * enum MHI_CB - MHI callback
 * @MHI_CB_IDLE: MHI entered idle state
 * @MHI_CB_PENDING_DATA: New data available for client to process
 * @MHI_CB_LPM_ENTER: MHI host entered low power mode
 * @MHI_CB_LPM_EXIT: MHI host about to exit low power mode
 * @MHI_CB_EE_RDDM: MHI device entered RDDM execution enviornment
 */
enum MHI_CB {
	MHI_CB_IDLE,
	MHI_CB_PENDING_DATA,
	MHI_CB_LPM_ENTER,
	MHI_CB_LPM_EXIT,
	MHI_CB_EE_RDDM,
};

/**
 * enum MHI_FLAGS - Transfer flags
 * @MHI_EOB: End of buffer for bulk transfer
 * @MHI_EOT: End of transfer
 * @MHI_CHAIN: Linked transfer
 */
enum MHI_FLAGS {
	MHI_EOB,
	MHI_EOT,
	MHI_CHAIN,
};

/**
 * enum mhi_device_type - Device types
 * @MHI_XFER_TYPE: Handles data transfer
 * @MHI_TIMESYNC_TYPE: Use for timesync feature
 * @MHI_CONTROLLER_TYPE: Control device
 */
enum mhi_device_type {
	MHI_XFER_TYPE,
	MHI_TIMESYNC_TYPE,
	MHI_CONTROLLER_TYPE,
};

/**
 * struct mhi_controller - Master controller structure for external modem
 * @dev: Device associated with this controller
 * @of_node: DT that has MHI configuration information
 * @regs: Points to base of MHI MMIO register space
 * @dev_id: PCIe device id of the external device
 * @domain: PCIe domain the device connected to
 * @bus: PCIe bus the device assigned to
 * @slot: PCIe slot for the modem
 * @iova_start: IOMMU starting address for data
 * @iova_stop: IOMMU stop address for data
 * @fw_image: Firmware image name for normal booting
 * @edl_image: Firmware image name for emergency download mode
 * @fbc_download: MHI host needs to do complete image transfer
 * @rddm_size: RAM dump size that host should allocate for debugging purpose
 * @sbl_size: SBL image size
 * @seg_len: BHIe vector size
 * @fbc_image: Points to firmware image buffer
 * @rddm_image: Points to RAM dump buffer
 * @max_chan: Maximum number of channels controller support
 * @mhi_chan: Points to channel configuration table
 * @lpm_chans: List of channels that require LPM notifications
 * @total_ev_rings: Total # of event rings allocated
 * @hw_ev_rings: Number of hardware event rings
 * @sw_ev_rings: Number of software event rings
 * @msi_required: Number of msi required to operate
 * @msi_allocated: Number of msi allocated by bus master
 * @irq: base irq # to request
 * @mhi_event: MHI event ring configurations table
 * @mhi_cmd: MHI command ring configurations table
 * @mhi_ctxt: MHI device context, shared memory between host and device
 * @timeout_ms: Timeout in ms for state transitions
 * @pm_state: Power management state
 * @ee: MHI device execution environment
 * @dev_state: MHI STATE
 * @status_cb: CB function to notify various power states to but master
 * @link_status: Query link status in case of abnormal value read from device
 * @runtime_get: Async runtime resume function
 * @runtimet_put: Release votes
 * @time_get: Return host time in us
 * @lpm_disable: Request controller to disable link level low power modes
 * @lpm_enable: Controller may enable link level low power modes again
 * @priv_data: Points to bus master's private data
 */
struct mhi_controller {
	struct list_head node;
	struct mhi_device *mhi_dev;

	/* device node for iommu ops */
	struct device *dev;
	struct device_node *of_node;

	/* mmio base */
	void __iomem *regs;

	/* device topology */
	u32 dev_id;
	u32 domain;
	u32 bus;
	u32 slot;

	/* addressing window */
	dma_addr_t iova_start;
	dma_addr_t iova_stop;

	/* fw images */
	const char *fw_image;
	const char *edl_image;

	/* mhi host manages downloading entire fbc images */
	bool fbc_download;
	size_t rddm_size;
	size_t sbl_size;
	size_t seg_len;
	u32 session_id;
	u32 sequence_id;

	/* physical channel config data */
	u32 max_chan;
	struct mhi_chan *mhi_chan;
	struct list_head lpm_chans; /* these chan require lpm notification */

	/* physical event config data */
	u32 total_ev_rings;
	u32 hw_ev_rings;
	u32 sw_ev_rings;
	u32 msi_required;
	u32 msi_allocated;
	int *irq; /* interrupt table */
	struct mhi_event *mhi_event;

	/* cmd rings */
	struct mhi_cmd *mhi_cmd;

	/* mhi context (shared with device) */
	struct mhi_ctxt *mhi_ctxt;

	u32 timeout_ms;

	/* caller should grab pm_mutex for suspend/resume operations */
	struct mutex pm_mutex;
	bool pre_init;
	rwlock_t pm_lock;
	u32 pm_state;
	u32 ee;
	u32 dev_state;
	bool wake_set;
	atomic_t dev_wake;
	atomic_t alloc_size;
	struct list_head transition_list;
	spinlock_t transition_lock;
	spinlock_t wlock;

	/* debug counters */
	u32 M0, M2, M3;

	/* worker for different state transitions */
	struct work_struct st_worker;
	struct work_struct fw_worker;
	struct work_struct syserr_worker;
	wait_queue_head_t state_event;

	/* shadow functions */
	void (*status_cb)(struct mhi_controller *mhi_cntrl, void *priv,
			  enum MHI_CB cb);
	int (*link_status)(struct mhi_controller *mhi_cntrl, void *priv);
	void (*wake_get)(struct mhi_controller *mhi_cntrl, bool override);
	void (*wake_put)(struct mhi_controller *mhi_cntrl, bool override);
	int (*runtime_get)(struct mhi_controller *mhi_cntrl, void *priv);
	void (*runtime_put)(struct mhi_controller *mhi_cntrl, void *priv);
	void (*lpm_disable)(struct mhi_controller *mhi_cntrl, void *priv);
	void (*lpm_enable)(struct mhi_controller *mhi_cntrl, void *priv);
	int (*map_single)(struct mhi_controller *mhi_cntrl,
			  struct mhi_buf_info *buf);
	void (*unmap_single)(struct mhi_controller *mhi_cntrl,
			     struct mhi_buf_info *buf);

	/* channel to control DTR messaging */
	struct mhi_device *dtr_dev;

	/* bounce buffer settings */
	bool bounce_buf;
	size_t buffer_len;

	/* controller specific data */
	void *priv_data;
	void *log_buf;
	struct dentry *dentry;
	struct dentry *parent;
};

/**
 * struct mhi_device - mhi device structure associated bind to channel
 * @dev: Device associated with the channels
 * @mtu: Maximum # of bytes controller support
 * @ul_chan_id: MHI channel id for UL transfer
 * @dl_chan_id: MHI channel id for DL transfer
 * @tiocm: Device current terminal settings
 * @priv: Driver private data
 */
struct mhi_device {
	struct device dev;
	u32 dev_id;
	u32 domain;
	u32 bus;
	u32 slot;
	size_t mtu;
	int ul_chan_id;
	int dl_chan_id;
	int ul_event_id;
	int dl_event_id;
	u32 tiocm;
	const struct mhi_device_id *id;
	const char *chan_name;
	struct mhi_controller *mhi_cntrl;
	struct mhi_chan *ul_chan;
	struct mhi_chan *dl_chan;
	atomic_t dev_wake;
	enum mhi_device_type dev_type;
	void *priv_data;
	int (*ul_xfer)(struct mhi_device *mhi_dev, struct mhi_chan *mhi_chan,
		       void *buf, size_t len, enum MHI_FLAGS flags);
	int (*dl_xfer)(struct mhi_device *mhi_dev, struct mhi_chan *mhi_chan,
		       void *buf, size_t len, enum MHI_FLAGS flags);
	void (*status_cb)(struct mhi_device *mhi_dev, enum MHI_CB cb);
};

/**
 * struct mhi_result - Completed buffer information
 * @buf_addr: Address of data buffer
 * @dir: Channel direction
 * @bytes_xfer: # of bytes transferred
 * @transaction_status: Status of last trasnferred
 */
struct mhi_result {
	void *buf_addr;
	enum dma_data_direction dir;
	size_t bytes_xferd;
	int transaction_status;
};

/**
 * struct mhi_driver - mhi driver information
 * @id_table: NULL terminated channel ID names
 * @ul_xfer_cb: UL data transfer callback
 * @dl_xfer_cb: DL data transfer callback
 * @status_cb: Asynchronous status callback
 */
struct mhi_driver {
	const struct mhi_device_id *id_table;
	int (*probe)(struct mhi_device *mhi_dev,
		     const struct mhi_device_id *id);
	void (*remove)(struct mhi_device *mhi_dev);
	void (*ul_xfer_cb)(struct mhi_device *mhi_dev,
			   struct mhi_result *result);
	void (*dl_xfer_cb)(struct mhi_device *mhi_dev,
			   struct mhi_result *result);
	void (*status_cb)(struct mhi_device *mhi_dev, enum MHI_CB mhi_cb);
	struct device_driver driver;
};

#define to_mhi_driver(drv) container_of(drv, struct mhi_driver, driver)
#define to_mhi_device(dev) container_of(dev, struct mhi_device, dev)

static inline void mhi_device_set_devdata(struct mhi_device *mhi_dev,
					  void *priv)
{
	mhi_dev->priv_data = priv;
}

static inline void *mhi_device_get_devdata(struct mhi_device *mhi_dev)
{
	return mhi_dev->priv_data;
}

static inline void *mhi_controller_get_devdata(struct mhi_controller *mhi_cntrl)
{
	return mhi_cntrl->priv_data;
}

static inline void mhi_free_controller(struct mhi_controller *mhi_cntrl)
{
	kfree(mhi_cntrl);
}

/**
 * mhi_driver_register - Register driver with MHI framework
 * @mhi_drv: mhi_driver structure
 */
int mhi_driver_register(struct mhi_driver *mhi_drv);

/**
 * mhi_driver_unregister - Unregister a driver for mhi_devices
 * @mhi_drv: mhi_driver structure
 */
void mhi_driver_unregister(struct mhi_driver *mhi_drv);

/**
 * mhi_alloc_controller - Allocate mhi_controller structure
 * Allocate controller structure and additional data for controller
 * private data. You may get the private data pointer by calling
 * mhi_controller_get_devdata
 * @size: # of additional bytes to allocate
 */
struct mhi_controller *mhi_alloc_controller(size_t size);

/**
 * of_register_mhi_controller - Register MHI controller
 * Registers MHI controller with MHI bus framework. DT must be supported
 * @mhi_cntrl: MHI controller to register
 */
int of_register_mhi_controller(struct mhi_controller *mhi_cntrl);

void mhi_unregister_mhi_controller(struct mhi_controller *mhi_cntrl);

/**
 * mhi_bdf_to_controller - Look up a registered controller
 * Search for controller based on device identification
 * @domain: RC domain of the device
 * @bus: Bus device connected to
 * @slot: Slot device assigned to
 * @dev_id: Device Identification
 */
struct mhi_controller *mhi_bdf_to_controller(u32 domain, u32 bus, u32 slot,
					     u32 dev_id);

#endif /* _MHI_H_ */
