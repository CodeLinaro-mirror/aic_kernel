/* SPDX-License-Identifier: GPL-2.0-only */

/* Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved. */

#ifndef _QAIC_ACCEL_HELPERS
#define _QAIC_ACCEL_HELPERS

#include <drm/drm_auth.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_vma_manager.h>
#include <linux/debugfs.h>
#include <linux/dma-buf.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR == 8) && (RHEL_MINOR >=3)))
#include <drm/drmP.h>
#endif

#include "qaic.h"

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0))
#define ACCEL_MAJOR		261
#define ACCEL_MAX_MINORS	256

#define DRIVER_COMPUTE_ACCEL DRIVER_RENDER

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 5, 0))
#define DRM_ACCEL_FOPS \
	.open		= accel_open,\
	.release	= drm_release,\
	.unlocked_ioctl	= drm_ioctl,\
	.compat_ioctl	= drm_compat_ioctl,\
	.poll		= drm_poll,\
	.read		= drm_read,\
	.llseek		= noop_llseek, \
	.mmap		= accel_gem_mmap
#else
#define DRM_ACCEL_FOPS \
	.open		= accel_open,\
	.release	= drm_release,\
	.unlocked_ioctl	= drm_ioctl,\
	.compat_ioctl	= drm_compat_ioctl,\
	.poll		= drm_poll,\
	.read		= drm_read,\
	.llseek		= noop_llseek, \
	.mmap		= drm_gem_mmap
#endif //end 5.5 mmap

#define DEFINE_DRM_ACCEL_FOPS(name) \
	static const struct file_operations name = {\
		.owner		= THIS_MODULE,\
		DRM_ACCEL_FOPS,\
	}

static DEFINE_SPINLOCK(accel_minor_lock);
static struct idr accel_minors_idr;

static struct dentry *accel_debugfs_root;
static struct class *accel_class;
static struct device_type accel_sysfs_device_minor = {
	.name = "accel_minor"
};

struct drm_prime_member {
	struct dma_buf *dma_buf;
	uint32_t handle;

	struct rb_node dmabuf_rb;
	struct rb_node handle_rb;
};

int accel_init(void);
void accel_exit(void);
void accel_minor_remove(int index);
int accel_minor_alloc(void);
void accel_minor_replace(struct drm_minor *minor, int index);
void accel_set_device_instance_params(struct device *kdev, int index);
int accel_open(struct inode *inode, struct file *filp);
void accel_debugfs_init(struct drm_minor *minor, int minor_id);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 5, 0))
static int accel_gem_mmap(struct file *filp, struct vm_area_struct *vma);
#endif
void drm_prime_remove_buf_handle(struct drm_prime_file_private *prime_fpriv,
				 uint32_t handle);
void drm_gem_open(struct drm_device *dev, struct drm_file *file_private);
void drm_gem_release(struct drm_device *dev, struct drm_file *file_private);
void drm_file_free(struct drm_file *file);
void drm_prime_init_file_private(struct drm_prime_file_private *prime_fpriv);
struct drm_file *drm_file_alloc(struct drm_minor *minor);
int drm_open_helper(struct file *filp, struct drm_minor *minor);
void drm_managed_release(struct drm_device *dev);


#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 5, 0))
int qaic_accel_gem_object_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma);
#endif
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR == 8) && (RHEL_MINOR >= 1)))
void qaic_accel_free_object(struct drm_gem_object *obj);
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16 ,0))
void qaic_accel_gem_print_info(struct drm_printer *p, unsigned int indent,
				const struct drm_gem_object *obj);
#endif /* end >=4.16.0 */
#endif /* end <5.0.0 */

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 5, 0))
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR == 8) && (RHEL_MINOR >= 1)))
static const struct vm_operations_struct qaic_accel_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};
#endif

static int accel_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *priv = filp->private_data;
	struct drm_device *dev = priv->minor->dev;
	struct drm_gem_object *obj = NULL;
	struct drm_vma_offset_node *node;
	unsigned long obj_size;
	int ret;

	if (drm_dev_is_unplugged(dev))
		return -ENODEV;

	drm_vma_offset_lock_lookup(dev->vma_offset_manager);
	node = drm_vma_offset_exact_lookup_locked(dev->vma_offset_manager,
						  vma->vm_pgoff,
						  vma_pages(vma));
	if (likely(node)) {
		obj = container_of(node, struct drm_gem_object, vma_node);
		/*
		 * When the object is being freed, after it hits 0-refcnt it
		 * proceeds to tear down the object. In the process it will
		 * attempt to remove the VMA offset and so acquire this
		 * mgr->vm_lock.  Therefore if we find an object with a 0-refcnt
		 * that matches our range, we know it is in the process of being
		 * destroyed and will be freed as soon as we release the lock -
		 * so we have to check for the 0-refcnted object and treat it as
		 * invalid.
		 */
		if (!kref_get_unless_zero(&obj->refcount))
			obj = NULL;
	}
	drm_vma_offset_unlock_lookup(dev->vma_offset_manager);

	if (!obj)
		return -EINVAL;

	if (!drm_vma_node_is_allowed(node, priv)) {
		drm_gem_object_put(obj);
		return -EACCES;
	}

	/* Check for valid size. */
	obj_size = drm_vma_node_size(node) << PAGE_SHIFT;
	if (obj_size < vma->vm_end - vma->vm_start)
		return -EINVAL;

	/* Take a ref for this mapping of the object, so that the fault
	 * handler can dereference the mmap offset's pointer to the object.
	 * This reference is cleaned up by the corresponding vm_close
	 * (which should happen whether the vma was created by this call, or
	 * by a vm_open due to mremap or partial unmap or whatever).
	 */
	drm_gem_object_get(obj);

	vma->vm_private_data = obj;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR == 8) && (RHEL_MINOR >= 1)))
	vma->vm_ops = &qaic_accel_vm_ops;
#else
	vma->vm_ops = obj->funcs->vm_ops;
#endif

	ret = qaic_accel_gem_object_mmap(obj, vma);

	if (ret)
		drm_gem_object_put(obj);
	else
		WARN_ON(!(vma->vm_flags & VM_DONTEXPAND));

	drm_gem_object_put(obj);
	return ret;
}
#endif //end 5.5 mmap

static void drm_events_release(struct drm_file *file_priv)
{
	struct drm_device *dev = file_priv->minor->dev;
	struct drm_pending_event *e, *et;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);

	/* Unlink pending events */
	list_for_each_entry_safe(e, et, &file_priv->pending_event_list,
				 pending_link) {
		list_del(&e->pending_link);
		e->file_priv = NULL;
	}

	/* Remove unconsumed events */
	list_for_each_entry_safe(e, et, &file_priv->event_list, link) {
		list_del(&e->link);
		kfree(e);
	}

	spin_unlock_irqrestore(&dev->event_lock, flags);
}

void drm_prime_remove_buf_handle(struct drm_prime_file_private *prime_fpriv,
				 uint32_t handle)
{
	struct rb_node *rb;

	mutex_lock(&prime_fpriv->lock);

	rb = prime_fpriv->handles.rb_node;
	while (rb) {
		struct drm_prime_member *member;

		member = rb_entry(rb, struct drm_prime_member, handle_rb);
		if (member->handle == handle) {
			rb_erase(&member->handle_rb, &prime_fpriv->handles);
			rb_erase(&member->dmabuf_rb, &prime_fpriv->dmabufs);

			dma_buf_put(member->dma_buf);
			kfree(member);
			break;
		} else if (member->handle < handle) {
			rb = rb->rb_right;
		} else {
			rb = rb->rb_left;
		}
	}

	mutex_unlock(&prime_fpriv->lock);
}

static void drm_gem_object_handle_free(struct drm_gem_object *obj)
{
	struct drm_device *dev = obj->dev;

	/* Remove any name for this object */
	if (obj->name) {
		idr_remove(&dev->object_name_idr, obj->name);
		obj->name = 0;
	}
}

static void drm_gem_object_exported_dma_buf_free(struct drm_gem_object *obj)
{
	/* Unbreak the reference cycle if we have an exported dma_buf. */
	if (obj->dma_buf) {
		dma_buf_put(obj->dma_buf);
		obj->dma_buf = NULL;
	}
}

static void drm_gem_object_handle_put_unlocked(struct drm_gem_object *obj)
{
	struct drm_device *dev = obj->dev;
	bool final = false;

	if (WARN_ON(READ_ONCE(obj->handle_count) == 0))
		return;

	/*
	* Must bump handle count first as this may be the last
	* ref, in which case the object would disappear before we
	* checked for a name
	*/

	mutex_lock(&dev->object_name_lock);
	if (--obj->handle_count == 0) {
		drm_gem_object_handle_free(obj);
		drm_gem_object_exported_dma_buf_free(obj);
		final = true;
	}
	mutex_unlock(&dev->object_name_lock);

	if (final)
		drm_gem_object_put(obj);
}

static int drm_gem_object_release_handle(int id, void *ptr, void *data)
{
	struct drm_file *file_priv = data;
	struct drm_gem_object *obj = ptr;

	drm_prime_remove_buf_handle(&file_priv->prime, id);
	drm_vma_node_revoke(&obj->vma_node, file_priv);

	drm_gem_object_handle_put_unlocked(obj);

	return 0;
}

void drm_gem_open(struct drm_device *dev, struct drm_file *file_private)
{
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0))
	idr_init(&file_private->object_idr);
#else
	idr_init_base(&file_private->object_idr, 1);
#endif
	spin_lock_init(&file_private->table_lock);
}

void drm_gem_release(struct drm_device *dev, struct drm_file *file_private)
{
	idr_for_each(&file_private->object_idr,
		     &drm_gem_object_release_handle, file_private);
	idr_destroy(&file_private->object_idr);
}

void drm_file_free(struct drm_file *file)
{
	struct drm_device *dev;

	if (!file)
		return;

	dev = file->minor->dev;

	DRM_DEBUG("comm=\"%s\", pid=%d, dev=0x%lx, open_count=%d\n",
		  current->comm, task_pid_nr(current),
		  (long)old_encode_dev(file->minor->kdev->devt),
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0) && \
		!(defined(CONFIG_SUSE_VERSION) && \
			CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) && \
		!(defined(RHEL_MAJOR) && \
			RHEL_MAJOR == 8 && RHEL_MINOR >= 4))
		  dev->open_count
#else
		  atomic_read(&dev->open_count)
#endif
		  );

	drm_events_release(file);

	if (drm_core_check_feature(dev, DRIVER_GEM))
		drm_gem_release(dev, file);

	if (dev->driver->postclose)
		dev->driver->postclose(dev, file);

	WARN_ON(!RB_EMPTY_ROOT(&file->prime.dmabufs));

	WARN_ON(!list_empty(&file->event_list));

	put_pid(file->pid);
	kfree(file);
}

void drm_prime_init_file_private(struct drm_prime_file_private *prime_fpriv)
{
	mutex_init(&prime_fpriv->lock);
	prime_fpriv->dmabufs = RB_ROOT;
	prime_fpriv->handles = RB_ROOT;
}

struct drm_file *drm_file_alloc(struct drm_minor *minor)
{
	struct drm_device *dev = minor->dev;
	struct drm_file *file;
	int ret;

	file = kzalloc(sizeof(*file), GFP_KERNEL);
	if (!file)
		return ERR_PTR(-ENOMEM);

	file->pid = get_pid(task_pid(current));
	file->minor = minor;

	/* for compatibility root is always authenticated */
	file->authenticated = capable(CAP_SYS_ADMIN);

	INIT_LIST_HEAD(&file->lhead);
	INIT_LIST_HEAD(&file->fbs);
	mutex_init(&file->fbs_lock);
	INIT_LIST_HEAD(&file->blobs);
	INIT_LIST_HEAD(&file->pending_event_list);
	INIT_LIST_HEAD(&file->event_list);
	init_waitqueue_head(&file->event_wait);
	file->event_space = 4096; /* set aside 4k for event buffer */

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 67) && \
		(LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0))) || \
		(LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 6))
	spin_lock_init(&file->master_lookup_lock);
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)) || \
	(defined(RHEL_MAJOR) && (RHEL_MAJOR == 7) && (RHEL_MINOR >= 3))
	mutex_init(&file->event_read_lock);
#endif

	if (drm_core_check_feature(dev, DRIVER_GEM))
		drm_gem_open(dev, file);

	drm_prime_init_file_private(&file->prime);

	if (dev->driver->open) {
		ret = dev->driver->open(dev, file);
		if (ret < 0)
			goto out_prime_destroy;
	}

	return file;

out_prime_destroy:
	WARN_ON(!RB_EMPTY_ROOT(&file->prime.dmabufs));
	if (drm_core_check_feature(dev, DRIVER_GEM))
		drm_gem_release(dev, file);
	put_pid(file->pid);
	kfree(file);

	return ERR_PTR(ret);
}

int drm_open_helper(struct file *filp, struct drm_minor *minor)
{
	struct drm_device *dev = minor->dev;
	struct drm_file *priv;
	//int ret;

	if (filp->f_flags & O_EXCL)
		return -EBUSY;
	//There was a check here for SPARC cpu
	if (dev->switch_power_state != DRM_SWITCH_POWER_ON &&
	    dev->switch_power_state != DRM_SWITCH_POWER_DYNAMIC_OFF)
		return -EINVAL;

	priv= drm_file_alloc(minor);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	if (unlikely(drm_is_primary_client(priv))) {
		printk(KERN_ERR "%s : %d - drm is primary ... and I've assumed otherwise", __func__, __LINE__ );
	}

	filp->private_data = priv;
	filp->f_mode |= FMODE_UNSIGNED_OFFSET;
	priv->filp = filp;

	mutex_lock(&dev->filelist_mutex);
	list_add(&priv->lhead, &dev->filelist);
	mutex_unlock(&dev->filelist_mutex);

	return 0;
}

static char *accel_devnode(struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "accel/%s", dev_name(dev));
}

static int accel_name_info(struct seq_file *m, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *) m->private;
	struct drm_minor *minor = node->minor;
	struct drm_device *dev = minor->dev;
	struct drm_master *master;

	mutex_lock(&dev->master_mutex);
	master = dev->master;
	seq_printf(m, "%s", dev->driver->name);
	if (dev->dev)
		seq_printf(m, " dev=%s", dev_name(dev->dev));
	if (master && master->unique)
		seq_printf(m, " master=%s", master->unique);
	if (dev->unique)
		seq_printf(m, " unique=%s", dev->unique);
	seq_puts(m, "\n");
	mutex_unlock(&dev->master_mutex);

	return 0;
}

static const struct drm_info_list accel_debugfs_list[] = {
	{"name", accel_name_info, 0}
};
#define ACCEL_DEBUGFS_ENTRIES ARRAY_SIZE(accel_debugfs_list)


/**
  * accel_debugfs_init() - Initialize debugfs for accel minor
  * @minor: Pointer to the drm_minor instance.
  * @minor_id: The minor's id
  *
  * This function initializes the drm minor's debugfs members and creates
  * a root directory for the minor in debugfs. It also creates common files
  * for accelerators and calls the driver's debugfs init callback.
  */
void accel_debugfs_init(struct drm_minor *minor, int minor_id)
{
	struct drm_device *dev = minor->dev;
	char name[64];

	INIT_LIST_HEAD(&minor->debugfs_list);
	mutex_init(&minor->debugfs_lock);
	sprintf(name, "%d", minor_id);
	minor->debugfs_root = debugfs_create_dir(name, accel_debugfs_root);

	drm_debugfs_create_files(accel_debugfs_list, ACCEL_DEBUGFS_ENTRIES,
		minor->debugfs_root, minor);

	if (dev->driver->debugfs_init)
		dev->driver->debugfs_init(minor);
}

static void accel_debugfs_remove_all_files(struct drm_minor *minor)
{
	struct drm_info_node *node, *tmp;

	mutex_lock(&minor->debugfs_lock);
	list_for_each_entry_safe(node, tmp, &minor->debugfs_list, list) {
		debugfs_remove(node->dent);
		list_del(&node->list);
		kfree(node);
	}
	mutex_unlock(&minor->debugfs_lock);
}

static void accel_debugfs_cleanup(struct drm_minor *minor)
{
	if (!minor->debugfs_root)
		return;

	accel_debugfs_remove_all_files(minor);

	debugfs_remove_recursive(minor->debugfs_root);
	minor->debugfs_root = NULL;
}

static int qaic_accel_minor_register(struct drm_minor *minor)
{
	int ret;

	accel_debugfs_init(minor, minor->index);
	ret = device_add(minor->kdev);
	if (ret) {
		accel_debugfs_cleanup(minor);
		return ret;
	}

	accel_minor_replace(minor, minor->index);
	DRM_DEBUG("new accel minor registered %d\n", minor->index);

	return 0;
}

static void qaic_accel_minor_unregister(struct drm_minor *minor)
{
	unsigned long flags;
	struct drm_minor *tmp;

	if (!minor)
		return;

	/* we know the minor, but need to know if it's been unregistered already */
	spin_lock_irqsave(&accel_minor_lock, flags);
	tmp = idr_find(&accel_minors_idr, minor->index);
	spin_unlock_irqrestore(&accel_minor_lock, flags);

	if (!tmp || !device_is_registered(tmp->kdev))
		return;

	WARN_ON(minor != tmp);

	accel_minor_replace(NULL, minor->index);
	device_del(minor->kdev);
	accel_debugfs_cleanup(minor);
}

/**
 * accel_set_device_instance_params() - Set some device parameters for accel device
 * @kdev: Pointer to the device instance.
 * @index: The minor's index
 *
 * This function creates the dev_t of the device using the accel major and
 * the device's minor number. In addition, it sets the class and type of the
 * device instance to the accel sysfs class and device type, respectively.
 */
void accel_set_device_instance_params(struct device *kdev, int index)
{
	kdev->devt = MKDEV(ACCEL_MAJOR, index);
	kdev->class = accel_class;
	kdev->type = &accel_sysfs_device_minor;
}

int accel_minor_alloc(void)
{
	unsigned long flags;
	int r;

	spin_lock_irqsave(&accel_minor_lock, flags);
	r = idr_alloc(&accel_minors_idr, NULL, 0, ACCEL_MAX_MINORS, GFP_NOWAIT);
	spin_unlock_irqrestore(&accel_minor_lock, flags);

	return r;
}

void accel_minor_remove(int index)
{
	unsigned long flags;

	spin_lock_irqsave(&accel_minor_lock, flags);
	idr_remove(&accel_minors_idr, index);
	spin_unlock_irqrestore(&accel_minor_lock, flags);
}

void accel_minor_replace(struct drm_minor *accel_minor, int index)
{
	unsigned long flags;

	spin_lock_irqsave(&accel_minor_lock, flags);
	idr_replace(&accel_minors_idr, accel_minor, index);
	spin_unlock_irqrestore(&accel_minor_lock, flags);
}

static void accel_minor_alloc_release(struct drm_device *dev, void *data)
{
	struct drm_minor *minor = data;

	WARN_ON(dev != minor->dev);

	put_device(minor->kdev);
	accel_minor_remove(minor->index);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0) || \
		(defined(CONFIG_SUSE_VERSION) && \
			CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) || \
		(defined(RHEL_MAJOR) && \
			RHEL_MAJOR == 8 && RHEL_MINOR >= 4))
static void accel_minor_device_release(unsigned int minor_id) {}
#else
static void accel_minor_device_release(unsigned int minor_id)
{
	struct drm_minor *minor;
	unsigned long flags;

	spin_lock_irqsave(&accel_minor_lock, flags);
	minor = idr_find(&accel_minors_idr, minor_id);
	spin_unlock_irqrestore(&accel_minor_lock, flags);

	if (!minor)
		return;

	accel_minor_alloc_release(minor->dev, minor);
}
#endif

static struct drm_minor *accel_minor_acquire(unsigned int minor_id)
{
	struct drm_minor *minor;
	unsigned long flags;

	spin_lock_irqsave(&accel_minor_lock, flags);
	minor = idr_find(&accel_minors_idr, minor_id);
	if (minor)
		drm_dev_get(minor->dev);
	spin_unlock_irqrestore(&accel_minor_lock, flags);

	if (!minor) {
		return ERR_PTR(-ENODEV);
	} else if (drm_dev_is_unplugged(minor->dev)) {
		drm_dev_put(minor->dev);
		return ERR_PTR(-ENODEV);
	}

	return minor;
}

static void accel_minor_release(struct drm_minor *minor)
{
	drm_dev_put(minor->dev);
}

static void accel_sysfs_release(struct device *dev)
{
	kfree(dev);
}

int accel_open(struct inode *inode, struct file *filp)
{
	struct drm_device *dev;
	struct drm_minor *minor;
	int ret;

	minor = accel_minor_acquire(iminor(inode));
	if (IS_ERR(minor))
		return PTR_ERR(minor);

	dev = minor->dev;
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0) && \
		!(defined(CONFIG_SUSE_VERSION) && \
			CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) && \
		!(defined(RHEL_MAJOR) && \
			RHEL_MAJOR == 8 && RHEL_MINOR >= 4))
	dev->open_count++;
#else
	atomic_fetch_inc(&dev->open_count);
#endif

	filp->f_mapping = dev->anon_inode->i_mapping;

	ret = drm_open_helper(filp, minor);
	if (ret)
		goto err_undo;
	return 0;

err_undo:
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0) && \
		!(defined(CONFIG_SUSE_VERSION) && \
			CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) && \
		!(defined(RHEL_MAJOR) && \
			RHEL_MAJOR == 8 && RHEL_MINOR >= 4))
	dev->open_count--;
#else
	atomic_dec(&dev->open_count);
#endif
	accel_minor_release(minor);
	return ret;
}

static int accel_stub_open(struct inode *inode, struct file *filp)
{
	const struct file_operations *new_fops;
	struct drm_minor *minor;
	int err;

	minor = accel_minor_acquire(iminor(inode));
	if (IS_ERR(minor))
		return PTR_ERR(minor);

	new_fops = fops_get(minor->dev->driver->fops);
	if (!new_fops) {
		err = -ENODEV;
		goto out;
	}

	replace_fops(filp, new_fops);
	if (filp->f_op->open)
		err = filp->f_op->open(inode, filp);
	else
		err = 0;

out:
	accel_minor_release(minor);

	return err;
}

static const struct file_operations accel_stub_fops = {
	.owner = THIS_MODULE,
	.open = accel_stub_open,
	.llseek = noop_llseek,
};

static void qaic_drm_accel_free(struct qaic_drm_device *qddev)
{
	if (!qddev->accel)
		return;

	accel_minor_device_release(qddev->accel->index);

	kfree(qddev->accel);
	qddev->accel = NULL;
}

static void qaic_accel_cleanup(struct qaic_device *qdev)
{
	if (!qdev->qddev)
		return;
	qaic_drm_accel_free(qdev->qddev);
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0) && \
		!(defined(CONFIG_SUSE_VERSION) && \
			CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) && \
		!(defined(RHEL_MAJOR) && \
			RHEL_MAJOR == 8 && RHEL_MINOR >= 4))
struct qaic_drm_device *qaic_accel_drm_dev_alloc(const struct drm_driver *driver,
							struct device *parent) {
	struct qaic_drm_device *qddev;
	struct drm_device *ddev;

	qddev = kzalloc(sizeof(*qddev), GFP_KERNEL);
	if (!qddev) {
		qddev = ERR_PTR(-ENOMEM);
		goto error_exit;
	}

	ddev = drm_dev_alloc(driver, parent);
	if (IS_ERR(ddev)) {
		kfree(qddev);
		qddev = (struct qaic_drm_device *) ddev;
		goto error_exit;
	}
	ddev->dev_private = qddev;
	qddev->ddev = ddev;
error_exit:
	return qddev;
}
#endif

static int accel_alloc(struct qaic_drm_device *qddev)
{
	struct drm_minor *acc_minor;
	int ret;

	/* drm_minor_alloc(accel) stuff */
	acc_minor = kzalloc(sizeof(*acc_minor), GFP_KERNEL);
	if (!acc_minor)
		return -ENOMEM;

	acc_minor->type = 32; /* the DRM_MINOR_ACCEL type (but not defined locally since we're acting as Render too) */

	acc_minor->dev = to_drm_from_qddev(qddev);

	idr_preload(GFP_KERNEL);
	ret = accel_minor_alloc();
	idr_preload_end();
	if (ret < 0)
		goto idr_alloc_fail;

	acc_minor->index = ret;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0) || \
		(defined(CONFIG_SUSE_VERSION) && \
			CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) || \
		(defined(RHEL_MAJOR) && \
			RHEL_MAJOR == 8 && RHEL_MINOR >= 4))
	ret = drmm_add_action_or_reset(&qddev->drm, accel_minor_alloc_release, acc_minor);
	if (ret)
		goto dev_alloc_fail;
#endif
	/* do drm_sysfs_minor_alloc() stuff */
	acc_minor->kdev = kzalloc(sizeof(struct device), GFP_KERNEL);
	if (!acc_minor->kdev) {
		ret = -ENOMEM;
		goto dev_alloc_fail;
	}

	device_initialize(acc_minor->kdev);

	accel_set_device_instance_params(acc_minor->kdev, acc_minor->index);

	acc_minor->kdev->parent = acc_minor->dev->dev;
	acc_minor->kdev->release = accel_sysfs_release;

	dev_set_drvdata(acc_minor->kdev, acc_minor);
	ret = dev_set_name(acc_minor->kdev, "accel%d", acc_minor->index);
	if (ret < 0) {
		goto dev_set_fail;
	}

	/* finished with drm_sysfs_minor_alloc */
	qddev->accel = acc_minor;
	return 0;

dev_set_fail:
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0) && \
		!(defined(CONFIG_SUSE_VERSION) && \
			CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) && \
		!(defined(RHEL_MAJOR) && \
			RHEL_MAJOR == 8 && RHEL_MINOR >= 4))
	put_device(acc_minor->kdev);
#endif
dev_alloc_fail:
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0) && \
		!(defined(CONFIG_SUSE_VERSION) && \
			CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) && \
		!(defined(RHEL_MAJOR) && \
			RHEL_MAJOR == 8 && RHEL_MINOR >= 4))
	accel_minor_remove(acc_minor->index);
#endif
idr_alloc_fail:
	kfree(acc_minor);
	return ret;
}

static int accel_sysfs_init(void)
{
	accel_class = class_create(THIS_MODULE, "accel");
	if (IS_ERR(accel_class))
		return PTR_ERR(accel_class);

	accel_class->devnode = accel_devnode;

	return 0;
}

static void accel_sysfs_destroy(void)
{
	if (IS_ERR_OR_NULL(accel_class))
		return;
	class_destroy(accel_class);
	accel_class = NULL;
}

int accel_init(void)
{
	int ret;

	idr_init(&accel_minors_idr);

	ret = accel_sysfs_init();
	if (ret < 0) {
		DRM_ERROR("Cannot create ACCEL class %d\n", ret);
		goto error;
	}

	accel_debugfs_root = debugfs_create_dir("accel", NULL);

	ret = register_chrdev(ACCEL_MAJOR, "accel", &accel_stub_fops);
	if (ret < 0) {
		DRM_ERROR("Cannot register ACCEL major: %d\n", ret);
		goto error;
	}

	return 0;

error:
	accel_exit();
	return ret;
}

void accel_exit(void)
{
	unregister_chrdev(ACCEL_MAJOR, "accel");
	debugfs_remove(accel_debugfs_root);
	accel_sysfs_destroy();
	idr_destroy(&accel_minors_idr);
}

#endif //end LINUX_VERSION_CODE < 6.2.0

#endif //end _QAIC_ACCEL_HELPERS
