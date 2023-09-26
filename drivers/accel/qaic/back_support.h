/* SPDX-License-Identifier: GPL-2.0-only */

/* Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved. */

#ifndef _QAIC_BACKSUPPORT_H_
#define _QAIC_BACKSUPPORT_H_

#include <generated/utsrelease.h>
#include <linux/compiler.h>
#include <linux/sched/mm.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 18, 0))
#include <linux/overflow.h>
#endif
#include <linux/version.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 119))
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR == 7) && (RHEL_MINOR >=9)))
#include <asm/types.h>
#define BIT(nr)			(1UL << (nr))
#define GENMASK(h, l)		(((U32_C(1) << ((h) - (l) + 1)) - 1) << (l))
#define GENMASK_ULL(h, l)	(((U64_C(1) << ((h) - (l) + 1)) - 1) << (l))
#else
#include <linux/bitops.h>
#endif /* end <3.13 and RHEL7.9 check */
#endif

/*
 * Parse UTS_UBUNTU_RELEASE_ABI
 */
#ifdef UTS_UBUNTU_RELEASE_ABI
#ifndef UBUNTU_ABI
#define CONCAT_HLP(x, y) x##y
#define HEXIFY(z) CONCAT_HLP(0x, z)
#define UBUNTU_ABI HEXIFY(UTS_UBUNTU_RELEASE_ABI)
#endif //UBUNTU_ABI defined
#define PCI_ERR_ABI_CHECK HEXIFY(56)
#define DMA_SGTABLE_ABI_CHECK HEXIFY(149)
#endif //UTS_UBUNTU_RELEASE_ABI defined

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0) && \
	LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0))
#define from_timer(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof(*var), timer_fieldname)

#define TIMER_DATA_TYPE unsigned long
#define TIMER_FUNC_TYPE void (*)(TIMER_DATA_TYPE)

static inline void timer_setup(struct timer_list *timer,
			       void (*callback)(struct timer_list *),
			       unsigned int flags)
{
	__setup_timer(timer, (TIMER_FUNC_TYPE)callback,
		      (TIMER_DATA_TYPE)timer, flags);
}
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0))
#define devm_get_free_pages(dev, flag, idk)	__get_free_page(flag)
#define devm_free_pages(dev, ptr)		free_page(ptr)
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR == 7) && (RHEL_MINOR >= 6)))
#define drm_dev_get(dev)	drm_dev_ref(dev)
#define drm_dev_put(dev)	drm_dev_unref(dev)
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0))
#define drm_printf_indent(printer, indent, fmt, ...) \
	drm_printf((printer), "%.*s" fmt, (indent), "\t\t\t\t\tX", ##__VA_ARGS__)
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0) && \
		!(defined(CONFIG_SUSE_VERSION)) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR >= 8) && (RHEL_MINOR >= 4)))
#define drm_gem_object_put(x) drm_gem_object_put_unlocked(x)
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0) && \
		!(defined(CONFIG_SUSE_VERSION) && \
			CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) && \
		!(defined(RHEL_MAJOR) && \
			RHEL_MAJOR == 8 && RHEL_MINOR >= 4))
#define devm_drm_dev_alloc(dev, driver, struc, drm) qaic_accel_drm_dev_alloc((driver), (dev))
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0))
#define ida_free(ida, id) ida_simple_remove((ida), (id))
/*
 * ida_simple_get's 'end' value is exclusive, so this isn't exactly the same
 * behavior as ida_alloc which maps to ida_alloc_range(ida,0,0,gfp), with an
 * inclusive max. ida_simple_get has a maximum end value of 0x80000000 (exclusive)
 * which is set by using end=0
 */
#define ida_alloc(ida,gfp) ida_simple_get(ida, 0, 0 , gfp)
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0))
#define is_signed_type(type)       (((type)(-1)) < (type)1)
#define __type_half_max(type) ((type)1 << (8*sizeof(type) - 1 - is_signed_type(type)))
#define type_max(T) ((T)((__type_half_max(T) - 1) + __type_half_max(T)))
#define type_min(T) ((T)((T)-type_max(T)-(T)1))
#ifdef COMPILER_HAS_GENERIC_BUILTIN_OVERFLOW
/*
 * For simplicity and code hygiene, the fallback code below insists on
 * a, b and *d having the same type (similar to the min() and max()
 * macros), whereas gcc's type-generic overflow checkers accept
 * different types. Hence we don't just make check_add_overflow an
 * alias for __builtin_add_overflow, but add type checks similar to
 * below.
 */
#define check_add_overflow(a, b, d) ({		\
	typeof(a) __a = (a);			\
	typeof(b) __b = (b);			\
	typeof(d) __d = (d);			\
	(void) (&__a == &__b);			\
	(void) (&__a == __d);			\
	__builtin_add_overflow(__a, __b, __d);	\
})

#define check_sub_overflow(a, b, d) ({		\
	typeof(a) __a = (a);			\
	typeof(b) __b = (b);			\
	typeof(d) __d = (d);			\
	(void) (&__a == &__b);			\
	(void) (&__a == __d);			\
	__builtin_sub_overflow(__a, __b, __d);	\
})

#define check_mul_overflow(a, b, d) ({		\
	typeof(a) __a = (a);			\
	typeof(b) __b = (b);			\
	typeof(d) __d = (d);			\
	(void) (&__a == &__b);			\
	(void) (&__a == __d);			\
	__builtin_mul_overflow(__a, __b, __d);	\
})

#else

/* Checking for unsigned overflow is relatively easy without causing UB. */
#define __unsigned_add_overflow(a, b, d) ({	\
	typeof(a) __a = (a);			\
	typeof(b) __b = (b);			\
	typeof(d) __d = (d);			\
	(void) (&__a == &__b);			\
	(void) (&__a == __d);			\
	*__d = __a + __b;			\
	*__d < __a;				\
})
#define __unsigned_sub_overflow(a, b, d) ({	\
	typeof(a) __a = (a);			\
	typeof(b) __b = (b);			\
	typeof(d) __d = (d);			\
	(void) (&__a == &__b);			\
	(void) (&__a == __d);			\
	*__d = __a - __b;			\
	__a < __b;				\
})
/*
 * If one of a or b is a compile-time constant, this avoids a division.
 */
#define __unsigned_mul_overflow(a, b, d) ({		\
	typeof(a) __a = (a);				\
	typeof(b) __b = (b);				\
	typeof(d) __d = (d);				\
	(void) (&__a == &__b);				\
	(void) (&__a == __d);				\
	*__d = __a * __b;				\
	__builtin_constant_p(__b) ?			\
	  __b > 0 && __a > type_max(typeof(__a)) / __b : \
	  __a > 0 && __b > type_max(typeof(__b)) / __a;	 \
})

/*
 * For signed types, detecting overflow is much harder, especially if
 * we want to avoid UB. But the interface of these macros is such that
 * we must provide a result in *d, and in fact we must produce the
 * result promised by gcc's builtins, which is simply the possibly
 * wrapped-around value. Fortunately, we can just formally do the
 * operations in the widest relevant unsigned type (u64) and then
 * truncate the result - gcc is smart enough to generate the same code
 * with and without the (u64) casts.
 */

/*
 * Adding two signed integers can overflow only if they have the same
 * sign, and overflow has happened iff the result has the opposite
 * sign.
 */
#define __signed_add_overflow(a, b, d) ({	\
	typeof(a) __a = (a);			\
	typeof(b) __b = (b);			\
	typeof(d) __d = (d);			\
	(void) (&__a == &__b);			\
	(void) (&__a == __d);			\
	*__d = (u64)__a + (u64)__b;		\
	(((~(__a ^ __b)) & (*__d ^ __a))	\
		& type_min(typeof(__a))) != 0;	\
})

/*
 * Subtraction is similar, except that overflow can now happen only
 * when the signs are opposite. In this case, overflow has happened if
 * the result has the opposite sign of a.
 */
#define __signed_sub_overflow(a, b, d) ({	\
	typeof(a) __a = (a);			\
	typeof(b) __b = (b);			\
	typeof(d) __d = (d);			\
	(void) (&__a == &__b);			\
	(void) (&__a == __d);			\
	*__d = (u64)__a - (u64)__b;		\
	((((__a ^ __b)) & (*__d ^ __a))		\
		& type_min(typeof(__a))) != 0;	\
})

/*
 * Signed multiplication is rather hard. gcc always follows C99, so
 * division is truncated towards 0. This means that we can write the
 * overflow check like this:
 *
 * (a > 0 && (b > MAX/a || b < MIN/a)) ||
 * (a < -1 && (b > MIN/a || b < MAX/a) ||
 * (a == -1 && b == MIN)
 *
 * The redundant casts of -1 are to silence an annoying -Wtype-limits
 * (included in -Wextra) warning: When the type is u8 or u16, the
 * __b_c_e in check_mul_overflow obviously selects
 * __unsigned_mul_overflow, but unfortunately gcc still parses this
 * code and warns about the limited range of __b.
 */

#define __signed_mul_overflow(a, b, d) ({				\
	typeof(a) __a = (a);						\
	typeof(b) __b = (b);						\
	typeof(d) __d = (d);						\
	typeof(a) __tmax = type_max(typeof(a));				\
	typeof(a) __tmin = type_min(typeof(a));				\
	(void) (&__a == &__b);						\
	(void) (&__a == __d);						\
	*__d = (u64)__a * (u64)__b;					\
	(__b > 0   && (__a > __tmax/__b || __a < __tmin/__b)) ||	\
	(__b < (typeof(__b))-1  && (__a > __tmin/__b || __a < __tmax/__b)) || \
	(__b == (typeof(__b))-1 && __a == __tmin);			\
})


#define check_add_overflow(a, b, d)					\
	__builtin_choose_expr(is_signed_type(typeof(a)),		\
			__signed_add_overflow(a, b, d),			\
			__unsigned_add_overflow(a, b, d))

#define check_sub_overflow(a, b, d)					\
	__builtin_choose_expr(is_signed_type(typeof(a)),		\
			__signed_sub_overflow(a, b, d),			\
			__unsigned_sub_overflow(a, b, d))

#define check_mul_overflow(a, b, d)					\
	__builtin_choose_expr(is_signed_type(typeof(a)),		\
			__signed_mul_overflow(a, b, d),			\
			__unsigned_mul_overflow(a, b, d))

#endif /* COMPILER_HAS_GENERIC_BUILTIN_OVERFLOW */
#endif /* 4.18.0 Overflow*/

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)) && \
	!(LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0) && \
		LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 86))
static inline size_t __must_check size_add(size_t addend1, size_t addend2)
{
	size_t bytes;

	if (check_add_overflow(addend1, addend2, &bytes))
		return SIZE_MAX;

	return bytes;
}
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)) && \
		(LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 233) || \
			LINUX_VERSION_CODE >= KERNEL_VERSION(5, 5, 0)) && \
		!(defined(CONFIG_SUSE_VERSION) && \
			CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) && \
		!(defined(RHEL_MAJOR) && (RHEL_MAJOR == 8) && (RHEL_MINOR >= 4)) && \
		!(defined(UBUNTU_ABI) && UBUNTU_ABI >= DMA_SGTABLE_ABI_CHECK && \
			LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 233) && \
			LINUX_VERSION_CODE < KERNEL_VERSION(5, 5, 0))
static inline int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
				  enum dma_data_direction dir,
				  unsigned long attrs)
{
	int nents = dma_map_sg(dev, sgt->sgl, sgt->orig_nents, dir);

	if (nents < 0)
		return nents;

	sgt->nents = nents;
	return 0;
}

static inline void dma_sync_sgtable_for_cpu(struct device *dev,
					    struct sg_table *sgt,
					    enum dma_data_direction dir)
{
	dma_sync_sg_for_cpu(dev, sgt->sgl, sgt->orig_nents, dir);
}

static inline void dma_unmap_sgtable(struct device *dev,
				     struct sg_table *sgt,
				     enum dma_data_direction dir,
				     unsigned long attrs)
{
	dma_unmap_sg(dev, sgt->sgl, sgt->orig_nents, dir);
}

static inline void dma_sync_sgtable_for_device(struct device *dev,
					       struct sg_table *sgt,
					       enum dma_data_direction dir)
{
	dma_sync_sg_for_device(dev, sgt->sgl, sgt->orig_nents, dir);
}

#define for_each_sgtable_sg(sgt, sg, i)         \
	for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)
#endif //LINUX_VERSION_CODE < 5.8.0 && SUSE_VERSION < 15.3 && RHEL_VERSION < 8.4

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0))
#if defined __has_attribute
#if __has_attribute(__fallthrough__)
#define fallthrough                     __attribute__((__fallthrough__))
#else
#define fallthrough                     do {} while (0)  /* fallthrough */
#endif //__has_attribute(__fallthrough__)
#else
#define fallthrough                     do {} while (0)  /* fallthrough */
#endif //defined __has_attribute
#endif //LINUX_VERSION_CODE < 5.4.0

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 1, 0)) && \
	!(defined(RHEL_MAJOR) && (RHEL_MAJOR == 8) && (RHEL_MINOR >= 1))
static inline int list_is_first(const struct list_head *list,
		const struct list_head *head)
{
	return list->prev == head;
}
#endif //LINUX_VERSION_CODE < 5.1.0 || RHEL < 8.1

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0))
#define SENSOR_DEVICE_ATTR_RO(_name, _func, _index)             \
	SENSOR_DEVICE_ATTR(_name, 0444, _func##_show, NULL, _index)
#define SENSOR_DEVICE_ATTR_RW(_name, _func, _index)             \
	SENSOR_DEVICE_ATTR(_name, 0644, _func##_show, _func##_store, _index)
#define SENSOR_DEVICE_ATTR_WO(_name, _func, _index)             \
	SENSOR_DEVICE_ATTR(_name, 0200, NULL, _func##_store, _index)
#endif //LINUX_VERSION_CODE < 5.0.0

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0))
#define PCI_VENDOR_ID_QCOM              0x17cb
#define PHYS_ADDR_MAX (~(phys_addr_t)0)
#endif //LINUX_VERSION_CODE < 4.18.0

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0))
typedef unsigned __bitwise __poll_t;
#endif //LINUX_VERSION_CODE < 4.16.0

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)) && !defined(RHEL_MAJOR)
static inline int __must_check device_add_group(struct device *dev,
					const struct attribute_group *grp)
{
	const struct attribute_group *groups[] = { grp, NULL };

	return sysfs_create_groups(&dev->kobj, groups);
}

static inline void device_remove_group(struct device *dev,
				       const struct attribute_group *grp)
{
	const struct attribute_group *groups[] = { grp, NULL };

	sysfs_remove_groups(&dev->kobj, groups);
}
#endif //LINUX_VERSION_CODE < 4.14.0 && !defined(RHEL_MAJOR)

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 0))
#define __GFP_RETRY_MAYFAIL 0
#endif //LINUX_VERSION_CODE < 4.13.0

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0))
/* Needed for mhi_qaic_ctrl */
#define EPOLLIN         0x00000001
#define EPOLLPRI        0x00000002
#define EPOLLOUT        0x00000004
#define EPOLLERR        0x00000008
#define EPOLLHUP        0x00000010
#define EPOLLRDNORM     0x00000040
#define EPOLLRDBAND     0x00000080
#define EPOLLWRNORM     0x00000100
#define EPOLLWRBAND     0x00000200
#define EPOLLMSG        0x00000400
#define EPOLLRDHUP      0x00002000
#endif //LINUX_VERSION_CODE < 4.12.0

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)) && !defined(RHEL_MAJOR)
static inline void *kvmalloc_array(size_t n, size_t size, gfp_t flags)
{
	return kmalloc_array(n, size, flags);
}
#endif //LINUX_VERSION_CODE < 4.12.0 && !defined(RHEL_MAJOR)

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0))
#include <linux/sched.h>
#else
#include <linux/sched/signal.h>
#endif //LINUX_VERSION_CODE < 4.11.0

/*
 * pci_err/dbg/warn have a messy history, each line here aligns with the gating
 * logic blocks below (indicated by tabing).
 *
 * -They were added in 4.9.126...
 * -Removed from 4.10-4.13, but added back in 4.14.69...
 * -But not present in 4.15 (unless Ubuntu)...
 * -Where they were added on 4.15.0-56 (which is really 4.15.18)
 *  first Ubuntu 4.15.0 kernel is really 4.15.2
 *  see: https://people.canonical.com/~kernel/info/kernel-version-map.html
 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 9, 126)) || \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0) && \
		LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 69)) || \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0) && \
		LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0) && \
		!defined(UBUNTU_ABI)) || \
	(defined(UBUNTU_ABI) && UBUNTU_ABI < PCI_ERR_ABI_CHECK && \
		LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 2) && \
		LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0))
#define pci_dbg(pdev, fmt, arg...)     dev_dbg(&(pdev)->dev, fmt, ##arg)
#define pci_err(pdev, fmt, arg...)     dev_err(&(pdev)->dev, fmt, ##arg)
#define pci_warn(pdev, fmt, arg...)    dev_warn(&(pdev)->dev, fmt, ##arg)
#define pci_info(pdev, fmt, arg...)    dev_info(&(pdev)->dev, fmt, ##arg)
#define pci_printk(level, pdev, fmt, arg...) \
	dev_printk(level, &(pdev)->dev, fmt, ##arg)
#endif //kernels without pci_err/dbg/warn defined

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 8, 0)) && !defined(RHEL_MAJOR)
#define PCI_IRQ_MSI 0
static inline int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
		unsigned int max_vecs, unsigned int flags)
{
	return pci_enable_msi_range(dev, min_vecs, max_vecs);
}
static inline void pci_free_irq_vectors(struct pci_dev *dev)
{
	pci_disable_msix(dev);
	pci_disable_msi(dev);
}
static inline int pci_irq_vector(struct pci_dev *dev, unsigned int nr)
{
	return dev->irq + nr;
}
#endif //LINUX_VERSION_CODE < 4.8.0 && !defined(RHEL_MAJOR)

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0)) && !defined(RHEL_MAJOR)
static inline void kvfree(const void *addr)
{
	kfree(addr);
}
#endif //LINUX_VERSION_CODE < 3.15.0 && !defined(RHEL_MAJOR)

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0))
static inline void irq_wake_thread(unsigned int irq, void *dev_id)
{
}
#endif //LINUX_VERSION_CODE < 3.15.0

#endif //_QAIC_BACKSUPPORT_H_
