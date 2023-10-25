/* SPDX-License-Identifier: GPL-2.0-only */

/* Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved. */

#ifndef _QAIC_BACKSUPPORT_H_
#define _QAIC_BACKSUPPORT_H_

#include "backport_flags.h"

#include <linux/compiler.h>
#ifdef _QBP_INCLUDE_SCHED_MM
#include <linux/sched/mm.h>
#endif
#ifdef _QBP_HAS_CHECK_OVERFLOW
#include <linux/overflow.h>
#endif
#include <linux/version.h>
#ifdef _QBP_INCLUDE_BITOPS
#ifdef _QBP_INCLUDE_BITOPS_MASK
#include <asm/types.h>
#define BIT(nr)			(1UL << (nr))
#define GENMASK(h, l)		(((U32_C(1) << ((h) - (l) + 1)) - 1) << (l))
#define GENMASK_ULL(h, l)	(((U64_C(1) << ((h) - (l) + 1)) - 1) << (l))
#else
#include <linux/bitops.h>
#endif /* end _QBP_INCLUDE_BITOPS_MASK */
#endif /* end _QBP_INCLUDE_BITOPS */

#ifdef _QBP_NEED_TIMER_SETUP
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
#endif /* end _QBP_NEED_TIMER_SETUP */

#ifdef _QBP_REDEF_DEVM_FREE_PAGE
#define devm_get_free_pages(dev, flag, idk)	__get_free_page(flag)
#define devm_free_pages(dev, ptr)		free_page(ptr)
#endif /* end _QBP_REDEF_DEVM_FREE_PAGE */

#ifdef _QBP_REDEF_DRM_DEV_UNPLUG
#define drm_dev_is_unplugged(x) drm_device_is_unplugged(x)
#endif

#ifdef _QBP_REDEF_DRM_DEV_GET_PUT
#define drm_dev_get(dev)	drm_dev_ref(dev)
#define drm_dev_put(dev)	drm_dev_unref(dev)
#endif

#ifdef _QBP_REDEF_DRM_PRINT_INDENT
#define drm_printf_indent(printer, indent, fmt, ...) \
	drm_printf((printer), "%.*s" fmt, (indent), "\t\t\t\t\tX", ##__VA_ARGS__)
#endif

#ifdef _QBP_REDEF_GEM_OBJ_PUT
#ifdef _QBP_REDEF_GEM_OBJ_GETPUT_OLDER
#define drm_gem_object_put(x) drm_gem_object_unreference_unlocked(x)
#define drm_gem_object_get(x) drm_gem_object_reference(x)
#else
#define drm_gem_object_put(x) drm_gem_object_put_unlocked(x)
#endif
#endif

#ifdef _QBP_REDEF_DEVM_DRM_ALLOC
#define devm_drm_dev_alloc(dev, driver, struc, drm) qaic_accel_drm_dev_alloc((driver), (dev))
#endif

#ifdef _QBP_REDEF_IDA_FREE
#define ida_free(ida, id) ida_simple_remove((ida), (id))
/*
 * ida_simple_get's 'end' value is exclusive, so this isn't exactly the same
 * behavior as ida_alloc which maps to ida_alloc_range(ida,0,0,gfp), with an
 * inclusive max. ida_simple_get has a maximum end value of 0x80000000 (exclusive)
 * which is set by using end=0
 */
#define ida_alloc(ida,gfp) ida_simple_get(ida, 0, 0 , gfp)
#endif

#ifndef _QBP_HAS_CHECK_OVERFLOW
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
#endif /* end _QBP_HAS_CHECK_OVERFLOW */

#ifdef _QBP_NEED_OVERFLOW_SIZE_ADD
static inline size_t __must_check size_add(size_t addend1, size_t addend2)
{
	size_t bytes;

	if (check_add_overflow(addend1, addend2, &bytes))
		return SIZE_MAX;

	return bytes;
}
#endif

#ifdef _QBP_NEED_DMA_SYNC_SGTABLE
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
#endif /* end _QBP_NEED_DMA_SYNC_SGTABLE */

#ifdef _QBP_NEED_ATTR_FALLTHROUGH
#if defined __has_attribute
#if __has_attribute(__fallthrough__)
#define fallthrough                     __attribute__((__fallthrough__))
#else
#define fallthrough                     do {} while (0)  /* fallthrough */
#endif //__has_attribute(__fallthrough__)
#else
#define fallthrough                     do {} while (0)  /* fallthrough */
#endif //defined __has_attribute
#endif /* end _QBP_NEED_ATTR_FALLTHROUGH */

#ifdef _QBP_NEED_LIST_IS_FIRST
static inline int list_is_first(const struct list_head *list,
		const struct list_head *head)
{
	return list->prev == head;
}
#endif /* end _QBP_NEED_LIST_IS_FIRST */

#ifdef _QBP_NEED_SENSOR_DEV_ATTR
#define SENSOR_DEVICE_ATTR_RO(_name, _func, _index)             \
	SENSOR_DEVICE_ATTR(_name, 0444, _func##_show, NULL, _index)
#define SENSOR_DEVICE_ATTR_RW(_name, _func, _index)             \
	SENSOR_DEVICE_ATTR(_name, 0644, _func##_show, _func##_store, _index)
#define SENSOR_DEVICE_ATTR_WO(_name, _func, _index)             \
	SENSOR_DEVICE_ATTR(_name, 0200, NULL, _func##_store, _index)
#endif /* end _QBP_NEED_SENSOR_DEV_ATTR */

#ifdef _QBP_NEED_PCI_VENDOR_PHYS_ADDR_MAX
#define PCI_VENDOR_ID_QCOM              0x17cb
#define PHYS_ADDR_MAX (~(phys_addr_t)0)
#endif /* end _QBP_NEED_PCI_VENDOR_PHYS_ADDR_MAX */

#ifdef _QBP_NEED_POLL_T
typedef unsigned __bitwise __poll_t;
#endif /* _QBP_NEED_POLL_T */

#ifdef _QBP_REDEF_DEV_GROUP_ADD_REMOVE
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
#endif /* end _QBP_REDEF_DEV_GROUP_ADD_REMOVE */

#ifdef _QBP_NEED_GFP_RETRY_MAYFAIL
#define __GFP_RETRY_MAYFAIL 0
#endif /* end _QBP_NEED_GFP_RETRY_MAYFAIL */

#ifdef _QBP_NEED_EPOLL
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
#endif /* end _QBP_NEED_EPOLL */

#ifdef _QBP_REDEF_KVMALLOC_ARRAY
static inline void *kvmalloc_array(size_t n, size_t size, gfp_t flags)
{
	return kmalloc_array(n, size, flags);
}
#endif /* end _QBP_REDEF_KVMALLOC_ARRAY */

#ifdef _QBP_INCLUDE_SCHED_SIGNAL
#include <linux/sched.h>
#else
#include <linux/sched/signal.h>
#endif /* end _QBP_INCLUDE_SCHED_SIGNAL */

#ifdef _QBP_REDEF_PCI_PRINTK
#define pci_dbg(pdev, fmt, arg...)     dev_dbg(&(pdev)->dev, fmt, ##arg)
#define pci_err(pdev, fmt, arg...)     dev_err(&(pdev)->dev, fmt, ##arg)
#define pci_warn(pdev, fmt, arg...)    dev_warn(&(pdev)->dev, fmt, ##arg)
#define pci_info(pdev, fmt, arg...)    dev_info(&(pdev)->dev, fmt, ##arg)
#define pci_printk(level, pdev, fmt, arg...) \
	dev_printk(level, &(pdev)->dev, fmt, ##arg)
#endif /* end _QBP_REDEF_PCI_PRINTK */

#ifdef _QBP_NEED_PCI_IRQ_VEC
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
#endif /* end _QBP_NEED_PCI_IRQ_VEC */

#ifdef _QBP_REDEF_KVFREE
static inline void kvfree(const void *addr)
{
	kfree(addr);
}
#endif /* end _QBP_REDEF_KVFREE */

#ifdef _QBP_NEED_IRQ_WAKE_THREAD
static inline void irq_wake_thread(unsigned int irq, void *dev_id)
{
}
#endif /* _QBP_NEED_IRQ_WAKE_THREAD */

#endif //_QAIC_BACKSUPPORT_H_
