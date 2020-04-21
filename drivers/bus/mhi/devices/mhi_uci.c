// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
 *
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/mhi.h>

#define DEVICE_NAME "mhi"
#define MHI_UCI_DRIVER_NAME "mhi_uci"

struct uci_chan {
	wait_queue_head_t wq;
	spinlock_t lock;
	struct list_head pending; /* user space waiting to read */
	struct uci_buf *cur_buf; /* current buffer user space reading */
	size_t rx_size;
};

struct uci_buf {
	void *data;
	size_t len;
	struct list_head node;
};

struct uci_dev {
	struct list_head node;
	dev_t devt;
	struct device *dev;
	struct mhi_device *mhi_dev;
	const char *chan;
	struct mutex mutex; /* sync open and close */
	struct uci_chan ul_chan;
	struct uci_chan dl_chan;
	size_t mtu;
	int ref_count;
	bool enabled;
};

struct mhi_uci_drv {
	struct list_head head;
	struct mutex lock;
	struct class *class;
	int major;
	dev_t dev_t;
};

#define MAX_UCI_DEVICES (64)

static DECLARE_BITMAP(uci_minors, MAX_UCI_DEVICES);
static struct mhi_uci_drv mhi_uci_drv;

static int mhi_queue_inbound(struct uci_dev *uci_dev)
{
	struct mhi_device *mhi_dev = uci_dev->mhi_dev;
	int nr_trbs = mhi_get_no_free_descriptors(mhi_dev, DMA_FROM_DEVICE);
	size_t mtu = uci_dev->mtu;
	void *buf;
	struct uci_buf *uci_buf;
	int ret = -EIO, i;

	for (i = 0; i < nr_trbs; i++) {
		buf = kmalloc(mtu + sizeof(*uci_buf), GFP_KERNEL);
		if (!buf)
			return -ENOMEM;

		uci_buf = buf + mtu;
		uci_buf->data = buf;

		ret = mhi_queue_transfer(mhi_dev, DMA_FROM_DEVICE, buf, mtu,
					 MHI_EOT);
		if (ret) {
			kfree(buf);
			return ret;
		}
	}

	return ret;
}

static long mhi_uci_ioctl(struct file *file,
			  unsigned int cmd,
			  unsigned long arg)
{
	struct uci_dev *uci_dev = file->private_data;
	struct mhi_device *mhi_dev = uci_dev->mhi_dev;
	long ret = -ERESTARTSYS;

	mutex_lock(&uci_dev->mutex);
	if (uci_dev->enabled)
		ret = mhi_ioctl(mhi_dev, cmd, arg);
	mutex_unlock(&uci_dev->mutex);

	return ret;
}

static int mhi_uci_release(struct inode *inode, struct file *file)
{
	struct uci_dev *uci_dev = file->private_data;

	mutex_lock(&uci_dev->mutex);
	uci_dev->ref_count--;
	if (!uci_dev->ref_count) {
		struct uci_buf *itr, *tmp;
		struct uci_chan *uci_chan;

		if (uci_dev->enabled)
			mhi_unprepare_from_transfer(uci_dev->mhi_dev);

		/* clean inbound channel */
		uci_chan = &uci_dev->dl_chan;
		list_for_each_entry_safe(itr, tmp, &uci_chan->pending, node) {
			list_del(&itr->node);
			kfree(itr->data);
		}
		if (uci_chan->cur_buf)
			kfree(uci_chan->cur_buf->data);

		uci_chan->cur_buf = NULL;

		if (!uci_dev->enabled) {
			mutex_unlock(&uci_dev->mutex);
			mutex_destroy(&uci_dev->mutex);
			clear_bit(MINOR(uci_dev->devt), uci_minors);
			kfree(uci_dev);
			return 0;
		}
	}

	mutex_unlock(&uci_dev->mutex);

	return 0;
}

static unsigned int mhi_uci_poll(struct file *file, poll_table *wait)
{
	struct uci_dev *uci_dev = file->private_data;
	struct mhi_device *mhi_dev = uci_dev->mhi_dev;
	struct uci_chan *uci_chan;
	unsigned int mask = 0;

	poll_wait(file, &uci_dev->dl_chan.wq, wait);
	poll_wait(file, &uci_dev->ul_chan.wq, wait);

	uci_chan = &uci_dev->dl_chan;
	spin_lock_bh(&uci_chan->lock);
	if (!uci_dev->enabled)
		mask = POLLERR;
	else if (!list_empty(&uci_chan->pending) || uci_chan->cur_buf)
		mask |= POLLIN | POLLRDNORM;

	spin_unlock_bh(&uci_chan->lock);

	uci_chan = &uci_dev->ul_chan;
	spin_lock_bh(&uci_chan->lock);
	if (!uci_dev->enabled)
		mask |= POLLERR;
	else if (mhi_get_no_free_descriptors(mhi_dev, DMA_TO_DEVICE) > 0)
		mask |= POLLOUT | POLLWRNORM;

	spin_unlock_bh(&uci_chan->lock);

	return mask;
}

static ssize_t mhi_uci_write(struct file *file,
			     const char __user *buf,
			     size_t count,
			     loff_t *offp)
{
	struct uci_dev *uci_dev = file->private_data;
	struct mhi_device *mhi_dev = uci_dev->mhi_dev;
	struct uci_chan *uci_chan = &uci_dev->ul_chan;
	size_t bytes_xfered = 0;
	int ret, nr_avail;

	if (!buf || !count)
		return -EINVAL;

	/* confirm channel is active */
	spin_lock_bh(&uci_chan->lock);
	if (!uci_dev->enabled) {
		spin_unlock_bh(&uci_chan->lock);
		return -ERESTARTSYS;
	}

	while (count) {
		size_t xfer_size;
		void *kbuf;
		enum MHI_FLAGS flags;

		spin_unlock_bh(&uci_chan->lock);

		/* wait for free descriptors */
		ret = wait_event_interruptible(uci_chan->wq,
			(!uci_dev->enabled) ||
			(nr_avail = mhi_get_no_free_descriptors(mhi_dev,
							DMA_TO_DEVICE)) > 0);

		if (ret == -ERESTARTSYS || !uci_dev->enabled)
			return -ERESTARTSYS;

		xfer_size = min_t(size_t, count, uci_dev->mtu);
		kbuf = kmalloc(xfer_size, GFP_KERNEL);
		if (!kbuf)
			return -ENOMEM;

		ret = copy_from_user(kbuf, buf, xfer_size);
		if (unlikely(ret)) {
			kfree(kbuf);
			return ret;
		}

		spin_lock_bh(&uci_chan->lock);

		/* if ring is full after this force EOT */
		if (nr_avail > 1 && (count - xfer_size))
			flags = MHI_CHAIN;
		else
			flags = MHI_EOT;

		if (uci_dev->enabled)
			ret = mhi_queue_transfer(mhi_dev, DMA_TO_DEVICE, kbuf,
						 xfer_size, flags);
		else
			ret = -ERESTARTSYS;

		if (ret) {
			kfree(kbuf);
			goto sys_interrupt;
		}

		bytes_xfered += xfer_size;
		count -= xfer_size;
		buf += xfer_size;
	}

	spin_unlock_bh(&uci_chan->lock);

	return bytes_xfered;

sys_interrupt:
	spin_unlock_bh(&uci_chan->lock);

	return ret;
}

static ssize_t mhi_uci_read(struct file *file,
			    char __user *buf,
			    size_t count,
			    loff_t *ppos)
{
	struct uci_dev *uci_dev = file->private_data;
	struct mhi_device *mhi_dev = uci_dev->mhi_dev;
	struct uci_chan *uci_chan = &uci_dev->dl_chan;
	struct uci_buf *uci_buf;
	char *ptr;
	size_t to_copy;
	int ret = 0;

	if (!buf)
		return -EINVAL;

	/* confirm channel is active */
	spin_lock_bh(&uci_chan->lock);
	if (!uci_dev->enabled) {
		spin_unlock_bh(&uci_chan->lock);
		return -ERESTARTSYS;
	}

	/* No data available to read, wait */
	if (!uci_chan->cur_buf && list_empty(&uci_chan->pending)) {

		spin_unlock_bh(&uci_chan->lock);
		ret = wait_event_interruptible(uci_chan->wq,
				(!uci_dev->enabled ||
				 !list_empty(&uci_chan->pending)));
		if (ret == -ERESTARTSYS)
			return -ERESTARTSYS;

		spin_lock_bh(&uci_chan->lock);
		if (!uci_dev->enabled) {
			ret = -ERESTARTSYS;
			goto read_error;
		}
	}

	/* new read, get the next descriptor from the list */
	if (!uci_chan->cur_buf) {
		uci_buf = list_first_entry_or_null(&uci_chan->pending,
						   struct uci_buf, node);
		if (unlikely(!uci_buf)) {
			ret = -EIO;
			goto read_error;
		}

		list_del(&uci_buf->node);
		uci_chan->cur_buf = uci_buf;
		uci_chan->rx_size = uci_buf->len;
	}

	uci_buf = uci_chan->cur_buf;
	spin_unlock_bh(&uci_chan->lock);

	/* Copy the buffer to user space */
	to_copy = min_t(size_t, count, uci_chan->rx_size);
	ptr = uci_buf->data + (uci_buf->len - uci_chan->rx_size);
	ret = copy_to_user(buf, ptr, to_copy);
	if (ret)
		return ret;

	uci_chan->rx_size -= to_copy;

	/* we finished with this buffer, queue it back to hardware */
	if (!uci_chan->rx_size) {
		spin_lock_bh(&uci_chan->lock);
		uci_chan->cur_buf = NULL;

		if (uci_dev->enabled)
			ret = mhi_queue_transfer(mhi_dev, DMA_FROM_DEVICE,
						 uci_buf->data, uci_dev->mtu,
						 MHI_EOT);
		else
			ret = -ERESTARTSYS;

		if (ret) {
			kfree(uci_buf->data);
			goto read_error;
		}

		spin_unlock_bh(&uci_chan->lock);
	}

	return to_copy;

read_error:
	spin_unlock_bh(&uci_chan->lock);

	return ret;
}

static int mhi_uci_open(struct inode *inode, struct file *filp)
{
	struct uci_dev *uci_dev;
	int ret = -EIO;
	struct uci_buf *buf_itr, *tmp;
	struct uci_chan *dl_chan;

	mutex_lock(&mhi_uci_drv.lock);
	list_for_each_entry(uci_dev, &mhi_uci_drv.head, node) {
		if (uci_dev->devt == inode->i_rdev) {
			ret = 0;
			break;
		}
	}
	mutex_unlock(&mhi_uci_drv.lock);

	/* could not find a minor node */
	if (ret)
		return ret;

	mutex_lock(&uci_dev->mutex);
	if (!uci_dev->enabled) {
		ret = -ENODEV;
		goto error_open_chan;
	}

	uci_dev->ref_count++;

	if (uci_dev->ref_count == 1) {
		ret = mhi_prepare_for_transfer(uci_dev->mhi_dev);
		if (ret) {
			uci_dev->ref_count--;
			goto error_open_chan;
		}

		ret = mhi_queue_inbound(uci_dev);
		if (ret)
			goto error_rx_queue;
	}

	filp->private_data = uci_dev;
	mutex_unlock(&uci_dev->mutex);

	return 0;

 error_rx_queue:
	dl_chan = &uci_dev->dl_chan;
	mhi_unprepare_from_transfer(uci_dev->mhi_dev);
	list_for_each_entry_safe(buf_itr, tmp, &dl_chan->pending, node) {
		list_del(&buf_itr->node);
		kfree(buf_itr->data);
	}

 error_open_chan:
	mutex_unlock(&uci_dev->mutex);

	return ret;
}

static const struct file_operations mhidev_fops = {
	.open = mhi_uci_open,
	.release = mhi_uci_release,
	.read = mhi_uci_read,
	.write = mhi_uci_write,
	.poll = mhi_uci_poll,
	.unlocked_ioctl = mhi_uci_ioctl,
};

static void mhi_uci_remove(struct mhi_device *mhi_dev)
{
	struct uci_dev *uci_dev = mhi_device_get_devdata(mhi_dev);

	/* disable the node */
	mutex_lock(&uci_dev->mutex);
	spin_lock_irq(&uci_dev->dl_chan.lock);
	spin_lock_irq(&uci_dev->ul_chan.lock);
	uci_dev->enabled = false;
	spin_unlock_irq(&uci_dev->ul_chan.lock);
	spin_unlock_irq(&uci_dev->dl_chan.lock);
	wake_up(&uci_dev->dl_chan.wq);
	wake_up(&uci_dev->ul_chan.wq);

	/* delete the node to prevent new opens */
	device_destroy(mhi_uci_drv.class, uci_dev->devt);
	uci_dev->dev = NULL;
	mutex_lock(&mhi_uci_drv.lock);
	list_del(&uci_dev->node);
	mutex_unlock(&mhi_uci_drv.lock);

	/* safe to free memory only if all file nodes are closed */
	if (!uci_dev->ref_count) {
		mutex_unlock(&uci_dev->mutex);
		mutex_destroy(&uci_dev->mutex);
		clear_bit(MINOR(uci_dev->devt), uci_minors);
		kfree(uci_dev);
		return;
	}

	mutex_unlock(&uci_dev->mutex);
}

static int mhi_uci_probe(struct mhi_device *mhi_dev,
			 const struct mhi_device_id *id)
{
	struct uci_dev *uci_dev;
	int minor;
	int dir;

	uci_dev = kzalloc(sizeof(*uci_dev), GFP_KERNEL);
	if (!uci_dev)
		return -ENOMEM;

	mutex_init(&uci_dev->mutex);
	uci_dev->mhi_dev = mhi_dev;

	mutex_lock(&uci_dev->mutex);
	mutex_lock(&mhi_uci_drv.lock);

	minor = find_first_zero_bit(uci_minors, MAX_UCI_DEVICES);
	if (minor >= MAX_UCI_DEVICES) {
		mutex_unlock(&mhi_uci_drv.lock);
		mutex_unlock(&uci_dev->mutex);
		kfree(uci_dev);
		return -ENOSPC;
	}

	uci_dev->devt = MKDEV(mhi_uci_drv.major, minor);
	uci_dev->dev = device_create(mhi_uci_drv.class, &mhi_dev->dev,
				     uci_dev->devt, uci_dev,
				     DEVICE_NAME "_%04x_%02u.%02u.%02u%s%s",
				     mhi_dev->dev_id, mhi_dev->domain,
				     mhi_dev->bus, mhi_dev->slot, "_",
				     mhi_dev->chan_name);
	set_bit(minor, uci_minors);

	for (dir = 0; dir < 2; dir++) {
		struct uci_chan *uci_chan = (dir) ?
			&uci_dev->ul_chan : &uci_dev->dl_chan;
		spin_lock_init(&uci_chan->lock);
		init_waitqueue_head(&uci_chan->wq);
		INIT_LIST_HEAD(&uci_chan->pending);
	}

	uci_dev->mtu = min_t(size_t, id->driver_data, mhi_dev->mtu);
	mhi_device_set_devdata(mhi_dev, uci_dev);
	uci_dev->enabled = true;

	list_add(&uci_dev->node, &mhi_uci_drv.head);
	mutex_unlock(&mhi_uci_drv.lock);
	mutex_unlock(&uci_dev->mutex);

	return 0;
};

static void mhi_ul_xfer_cb(struct mhi_device *mhi_dev,
			   struct mhi_result *mhi_result)
{
	struct uci_dev *uci_dev = mhi_device_get_devdata(mhi_dev);
	struct uci_chan *uci_chan = &uci_dev->ul_chan;

	kfree(mhi_result->buf_addr);
	if (!mhi_result->transaction_status)
		wake_up(&uci_chan->wq);
}

static void mhi_dl_xfer_cb(struct mhi_device *mhi_dev,
			   struct mhi_result *mhi_result)
{
	struct uci_dev *uci_dev = mhi_device_get_devdata(mhi_dev);
	struct uci_chan *uci_chan = &uci_dev->dl_chan;
	unsigned long flags;
	struct uci_buf *buf;

	if (mhi_result->transaction_status == -ENOTCONN) {
		kfree(mhi_result->buf_addr);
		return;
	}

	spin_lock_irqsave(&uci_chan->lock, flags);
	buf = mhi_result->buf_addr + uci_dev->mtu;
	buf->data = mhi_result->buf_addr;
	buf->len = mhi_result->bytes_xferd;
	list_add_tail(&buf->node, &uci_chan->pending);
	spin_unlock_irqrestore(&uci_chan->lock, flags);

	wake_up(&uci_chan->wq);
}

/* .driver_data stores max mtu */
static const struct mhi_device_id mhi_uci_match_table[] = {
	{ .chan = "LOOPBACK", .driver_data = 0x1000 },
	{ .chan = "SAHARA", .driver_data = 0x8000 },
	{ .chan = "EFS", .driver_data = 0x1000 },
	{ .chan = "QMI0", .driver_data = 0x1000 },
	{ .chan = "QMI1", .driver_data = 0x1000 },
	{ .chan = "TF", .driver_data = 0x1000 },
	{ .chan = "BL", .driver_data = 0x1000 },
	{ .chan = "DUN", .driver_data = 0x1000 },
	{ .chan = "QAIC_DIAG", .driver_data = 0x1000 },
	{ .chan = "QAIC_SAHARA", .driver_data = 0x8000 },
	{ .chan = "QAIC_QDSS", .driver_data = 0x1000 },
	{},
};

static struct mhi_driver mhi_uci_driver = {
	.id_table = mhi_uci_match_table,
	.remove = mhi_uci_remove,
	.probe = mhi_uci_probe,
	.ul_xfer_cb = mhi_ul_xfer_cb,
	.dl_xfer_cb = mhi_dl_xfer_cb,
	.driver = {
		.name = MHI_UCI_DRIVER_NAME,
		.owner = THIS_MODULE,
	},
};

static int mhi_uci_init(void)
{
	int ret;

	ret = register_chrdev(0, MHI_UCI_DRIVER_NAME, &mhidev_fops);
	if (ret < 0)
		return ret;

	mhi_uci_drv.major = ret;
	mhi_uci_drv.class = class_create(THIS_MODULE, MHI_UCI_DRIVER_NAME);
	if (IS_ERR(mhi_uci_drv.class)) {
		unregister_chrdev(mhi_uci_drv.major, MHI_UCI_DRIVER_NAME);
		return -ENODEV;
	}

	mutex_init(&mhi_uci_drv.lock);
	INIT_LIST_HEAD(&mhi_uci_drv.head);

	ret = mhi_driver_register(&mhi_uci_driver);
	if (ret) {
		class_destroy(mhi_uci_drv.class);
		unregister_chrdev(mhi_uci_drv.major, MHI_UCI_DRIVER_NAME);
	}

	return ret;
}

static void __exit mhi_uci_exit(void)
{
	mhi_driver_unregister(&mhi_uci_driver);
	class_destroy(mhi_uci_drv.class);
	unregister_chrdev(mhi_uci_drv.major, MHI_UCI_DRIVER_NAME);
}

module_init(mhi_uci_init);
module_exit(mhi_uci_exit);

MODULE_LICENSE("GPL v2");
MODULE_ALIAS("pci:v000017CBd0000A100sv*sd*bc*sc*i*");
MODULE_DESCRIPTION("MHI UCI Driver");
