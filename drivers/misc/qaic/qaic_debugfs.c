// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2020, The Linux Foundation. All rights reserved. */

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/mhi.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "qaic.h"
#include "qaic_debugfs.h"

#define BOOTLOG_POOL_SIZE 16
#define BOOTLOG_MSG_SIZE  512

struct bootlog_msg {
	char str[BOOTLOG_MSG_SIZE];
	struct qaic_device *qdev;
	struct work_struct work;
};

struct bootlog_page {
	struct list_head node;
	unsigned int size;
	unsigned int offset;
};

struct dentry *qaic_debugfs_dir;

static int bootlog_show(struct seq_file *s, void *data)
{
	struct qaic_device *qdev = s->private;
	struct bootlog_page *page;
	void *log;
	void *page_end;

	mutex_lock(&qdev->bootlog_mutex);
	list_for_each_entry(page, &qdev->bootlog, node) {
		log = page + 1;
		page_end = (void *)page + page->offset;
		while (log < page_end) {
			seq_printf(s, "%s", (char *)log);
			log += strlen(log) + 1;
		}
	}
	mutex_unlock(&qdev->bootlog_mutex);

	return 0;
}

static int bootlog_open(struct inode *inode, struct file *file)
{
	struct qaic_device *qdev = inode->i_private;

	return single_open(file, bootlog_show, qdev);
}

static const struct file_operations bootlog_fops = {
	.owner = THIS_MODULE,
	.open = bootlog_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int read_dbc_fifo_size(void *data, u64 *value)
{
	struct dma_bridge_chan *dbc = (struct dma_bridge_chan *) data;

	*value = dbc->nelem;
	return 0;
}

static int read_dbc_queued(void *data, u64 *value)
{
	struct dma_bridge_chan *dbc = (struct dma_bridge_chan *) data;
	u32 tail, head;

	qaic_data_get_fifo_info(dbc, &head, &tail);

	if (head == U32_MAX || tail == U32_MAX)
		*value = 0;
	else if (head > tail)
		*value = dbc->nelem - head + tail;
	else
		*value = tail - head;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(dbc_fifo_size_fops, read_dbc_fifo_size, NULL, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(dbc_queued_fops, read_dbc_queued, NULL, "%llu\n");

static void qaic_debugfs_add_dbc_entry(struct pci_dev *pdev, uint16_t dbc_id,
				       struct dentry *parent)
{
	char name[16];
	struct qaic_device *qdev = pci_get_drvdata(pdev);
	struct dma_bridge_chan *dbc = &qdev->dbc[dbc_id];

	snprintf(name, 16, "%s%03u", QAIC_DEBUGFS_DBC_PREFIX, dbc_id);

	dbc->debugfs_root = debugfs_create_dir(name, parent);

	debugfs_create_file(QAIC_DEBUGFS_DBC_FIFO_SIZE, 0444, dbc->debugfs_root,
						dbc, &dbc_fifo_size_fops);

	debugfs_create_file(QAIC_DEBUGFS_DBC_QUEUED, 0444, dbc->debugfs_root,
						dbc, &dbc_queued_fops);
}

void qaic_debugfs_remove_pci_device(struct pci_dev *pdev)
{
	struct qaic_device *qdev = pci_get_drvdata(pdev);

	debugfs_remove_recursive(qdev->debugfs_root);
}

int qaic_debugfs_add_pci_device(struct pci_dev *pdev)
{
	struct pci_bus *bus = pdev->bus;
	struct qaic_device *qdev = pci_get_drvdata(pdev);
	char name[16];
	uint16_t i;

	if (qaic_debugfs_dir == NULL) {
		pci_dbg(pdev, "Qaic debugfs root not preset\n");
		return -ENOENT;
	}

	snprintf(name, 16, "%04x:%02x:%02x.%x", pci_domain_nr(bus),
		 bus->number, PCI_SLOT(pdev->devfn),
		 PCI_FUNC(pdev->devfn));

	qdev->debugfs_root = debugfs_create_dir(name, qaic_debugfs_dir);

	for (i = 0; i < QAIC_NUM_DBC; ++i)
		qaic_debugfs_add_dbc_entry(pdev, i, qdev->debugfs_root);

	debugfs_create_file("bootlog", 0444, qdev->debugfs_root, qdev,
			    &bootlog_fops);
	return 0;
}

static struct bootlog_page *alloc_bootlog_page(struct qaic_device *qdev)
{
	struct bootlog_page *page;

	page = (struct bootlog_page *)__get_free_page(GFP_KERNEL);
	if (!page)
		return page;

	page->size = PAGE_SIZE;
	page->offset = sizeof(*page);
	list_add_tail(&page->node, &qdev->bootlog);

	return page;
}

static int reset_bootlog(struct qaic_device *qdev)
{
	struct bootlog_page *page;
	struct bootlog_page *i;

	list_for_each_entry_safe(page, i, &qdev->bootlog, node) {
		list_del(&page->node);
		free_page((unsigned long)page);
	}

	page = alloc_bootlog_page(qdev);
	if (!page)
		return -ENOMEM;

	return 0;
}

static void *bootlog_get_space(struct qaic_device *qdev, unsigned int size)
{
	struct bootlog_page *page;

	page = list_last_entry(&qdev->bootlog, struct bootlog_page, node);

	if (size > page->size - sizeof(*page))
		return NULL;

	if (page->offset + size >= page->size) {
		page = alloc_bootlog_page(qdev);
		if (!page)
			return NULL;
	}

	return (void *)page + page->offset;
}

static void bootlog_commit(struct qaic_device *qdev, unsigned int size)
{
	struct bootlog_page *page;

	page = list_last_entry(&qdev->bootlog, struct bootlog_page, node);

	page->offset += size;
}

static void bootlog_log(struct work_struct *work)
{
	struct bootlog_msg *msg = container_of(work, struct bootlog_msg, work);
	struct qaic_device *qdev = msg->qdev;
	unsigned int len = strlen(msg->str) + 1;
	void *log;

	mutex_lock(&qdev->bootlog_mutex);
	log = bootlog_get_space(qdev, len);
	if (log) {
		memcpy(log, msg, len);
		bootlog_commit(qdev, len);
	}
	mutex_unlock(&qdev->bootlog_mutex);
	mhi_queue_transfer(qdev->bootlog_ch, DMA_FROM_DEVICE,
			   msg, BOOTLOG_MSG_SIZE, MHI_EOT);
}

static int qaic_bootlog_mhi_probe(struct mhi_device *mhi_dev,
				  const struct mhi_device_id *id)
{
	struct qaic_device *qdev;
	struct bootlog_msg *msg;
	int ret;
	int i;

	qdev = (struct qaic_device *)pci_get_drvdata(
					to_pci_dev(mhi_dev->mhi_cntrl->dev));

	mhi_device_set_devdata(mhi_dev, qdev);
	qdev->bootlog_ch = mhi_dev;

	qdev->bootlog_wq = alloc_ordered_workqueue("qaic_bootlog", 0);
        if (!qdev->bootlog_wq) {
                ret = -ENOMEM;
		goto fail;
	}

	mutex_lock(&qdev->bootlog_mutex);
	ret = reset_bootlog(qdev);
	mutex_unlock(&qdev->bootlog_mutex);
	if (ret)
		goto reset_fail;

	ret = mhi_prepare_for_transfer(qdev->bootlog_ch);

	if (ret)
		goto prepare_fail;

	for (i = 0; i < BOOTLOG_POOL_SIZE; i++) {
		msg = kmalloc(sizeof(*msg), GFP_KERNEL);
		if (!msg) {
			ret = -ENOMEM;
			goto alloc_fail;
		}

		msg->qdev = qdev;
		INIT_WORK(&msg->work, bootlog_log);

		ret = mhi_queue_transfer(qdev->bootlog_ch, DMA_FROM_DEVICE,
					 msg, BOOTLOG_MSG_SIZE, MHI_EOT);
		if (ret)
			goto queue_fail;

	}

	return 0;

queue_fail:
alloc_fail:
	mhi_unprepare_from_transfer(qdev->bootlog_ch);
prepare_fail:
reset_fail:
	flush_workqueue(qdev->bootlog_wq);
	destroy_workqueue(qdev->bootlog_wq);
fail:
	return ret;
}

static void qaic_bootlog_mhi_remove(struct mhi_device *mhi_dev)
{
	struct qaic_device *qdev;

	qdev = mhi_device_get_devdata(mhi_dev);

	mhi_unprepare_from_transfer(qdev->bootlog_ch);
	flush_workqueue(qdev->bootlog_wq);
	destroy_workqueue(qdev->bootlog_wq);
}

static void qaic_bootlog_mhi_ul_xfer_cb(struct mhi_device *mhi_dev,
					struct mhi_result *mhi_result)
{
}

static void qaic_bootlog_mhi_dl_xfer_cb(struct mhi_device *mhi_dev,
					struct mhi_result *mhi_result)
{
	struct qaic_device *qdev = mhi_device_get_devdata(mhi_dev);
	struct bootlog_msg *msg = mhi_result->buf_addr;

	if (mhi_result->transaction_status) {
		kfree(msg);
		return;
	}

	/* force a null at the end of the transfered string */
	msg->str[mhi_result->bytes_xferd - 1] = 0;

	queue_work(qdev->bootlog_wq, &msg->work);
}

static const struct mhi_device_id qaic_bootlog_mhi_match_table[] = {
        { .chan = "QAIC_LOGGING", },
        {},
};

static struct mhi_driver qaic_bootlog_mhi_driver = {
	.id_table = qaic_bootlog_mhi_match_table,
	.remove = qaic_bootlog_mhi_remove,
	.probe = qaic_bootlog_mhi_probe,
	.ul_xfer_cb = qaic_bootlog_mhi_ul_xfer_cb,
	.dl_xfer_cb = qaic_bootlog_mhi_dl_xfer_cb,
	.driver = {
		.name = "qaic_bootlog",
		.owner = THIS_MODULE,
	},
};

void qaic_debugfs_init(void)
{
	if (qaic_debugfs_dir != NULL)
		return;

	qaic_debugfs_dir = debugfs_create_dir("qaic", NULL);
	mhi_driver_register(&qaic_bootlog_mhi_driver);
}

void qaic_debugfs_exit(void)
{
	mhi_driver_unregister(&qaic_bootlog_mhi_driver);
	debugfs_remove_recursive(qaic_debugfs_dir);
}

