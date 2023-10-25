/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _QAIC_BACKPORT_FLAGS_H_
#define _QAIC_BACKPORT_FLAGS_H_

#include <linux/version.h>
#include <generated/utsrelease.h>

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

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR == 7) && (RHEL_MINOR >=9)))
#define _QBP_INCLUDE_BITOPS_MASK
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0))
#define _QBP_NEED_IRQ_WAKE_THREAD
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0)) && !defined(RHEL_MAJOR)
#define _QBP_REDEF_KVFREE
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0))
#define _QBP_REDEF_DEVM_FREE_PAGE
#else
#define _QBP_HAS_PCI_ERROR_RESET_NOTIFY
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)) || \
	(defined(RHEL_MAJOR) && (RHEL_MAJOR == 7) && (RHEL_MINOR >= 3))
#define _QBP_HAS_DRM_FILE_EVENT_INFO_LOCK
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 8, 0)) && !defined(RHEL_MAJOR)
#define _QBP_NEED_PCI_IRQ_VEC
#endif

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
#define _QBP_REDEF_PCI_PRINTK
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0) && \
		!(defined (RHEL_MAJOR) && \
			(RHEL_MAJOR == 7) && (RHEL_MINOR >= 5)))
#else
#define _QBP_INCLUDE_HAS_DRM_DRV
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0) && \
		!(defined(RHEL_MAJOR) && RHEL_MAJOR == 7 && RHEL_MINOR >= 5))
#define _QBP_ALT_DRM_MANAGED_NO_RELEASE
#else
#define _QBP_INCLUDE_SCHED_MM
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0))
#define _QBP_INCLUDE_SCHED_SIGNAL
#else
#define _QBP_NEED_HWMON_TEMP_ALARM
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0))
#define _QBP_NEED_EPOLL
#define _QAIC_DRM_INCLUDE_DRMP_JIC
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)) && !defined(RHEL_MAJOR)
#define _QBP_REDEF_KVMALLOC_ARRAY
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0) && \
		!(defined (RHEL_MAJOR) && \
			(RHEL_MAJOR == 7) && (RHEL_MINOR >= 5)))
#define _QBP_HAS_DRM_DEBUGFS_CLEANUP
#define _QBP_INCLUDE_FIX_DRM_GEM
#else
#define _QBP_INCLUDE_HAS_DRM_FILE
#define _QBP_INCLUDE_HAS_DRM_DEBUGFS
#define _QBP_INCLUDE_HAS_DRM_PRIME
#define _QBP_INCLUDE_HAS_DRM_IOCTL
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0) && \
		!(defined (RHEL_MAJOR) && \
			(RHEL_MAJOR == 7) && (RHEL_MINOR >= 9)))
#define _QBP_REDEF_GEM_OBJ_GETPUT_OLDER
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 0))
#define _QBP_NEED_GFP_RETRY_MAYFAIL
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)) || \
	(defined(CONFIG_SUSE_VERSION) && \
		CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL <= 1)
#define _QBP_HAS_PCI_ERROR_RESET_PREPDONE
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR == 7) && (RHEL_MINOR >= 5)))
#define _QBP_REDEF_DRM_DEV_UNPLUG
#else
#define _QBP_INCLUDE_HAS_DRM_DEVICE
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)) && !defined(RHEL_MAJOR)
#define _QBP_REDEF_DEV_GROUP_ADD_REMOVE
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0) && \
	LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0))
#define _QBP_NEED_TIMER_SETUP
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 119))
#define _QBP_INCLUDE_BITOPS
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR == 7) && (RHEL_MINOR >= 6)))
#define _QBP_REDEF_DRM_DEV_GET_PUT
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0))
#define _QBP_REDEF_DRM_PRINT_INDENT
#define _QBP_NEED_POLL_T
#define _QBP_NEED_IDR_INIT_BASE
#else
#define _QBP_HAS_DRM_DRV_GEM_PRINT_INFO
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR == 7) && (RHEL_MINOR >= 5)))
#else
#define _QBP_INCLUDE_HAS_DRM_PRINT
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0))
#define _QBP_NEED_PCI_VENDOR_PHYS_ADDR_MAX
#else
#define _QBP_HAS_CHECK_OVERFLOW
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0))
#define _QBP_REDEF_IDA_FREE
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR == 8) && (RHEL_MINOR >=3)))
#define _QBP_INCLUDE_DRMP
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0))
#define _QBP_NEED_SENSOR_DEV_ATTR
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR == 8) && (RHEL_MINOR >= 1)))
#define _QBP_ALT_DRM_GEM_OBJ_FUNCS
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 1, 0))
#define _QBP_INCLUDE_FIX_DRM_FILE
/* xa_alloc introduced in 4.20.0 but has reversed entry/max before 5.1.0 */
#define _QBP_NEED_XARRAY_STABLE_XA_ALLOC
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 1, 0)) && \
	!(defined(RHEL_MAJOR) && (RHEL_MAJOR == 8) && (RHEL_MINOR >= 1))
#define _QBP_NEED_LIST_IS_FIRST
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)) && \
	!(defined(RHEL_MAJOR) && (RHEL_MAJOR == 8) && (RHEL_MINOR >= 3))
#define _QBP_HAS_DRM_GEM_OBJ_DMA_RESV
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 3, 0))
#define _QBP_INCLUDE_FIX_DRM_DEBUGFS
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0))
#define _QBP_NEED_ATTR_FALLTHROUGH
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 5, 0))
#define _QBP_ALT_DRM_GEM_OBJ_MMAP_FUNC
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0) && \
		!(defined(RHEL_MAJOR) && \
			RHEL_MAJOR == 8 && RHEL_MINOR >= 5))
#define _QBP_NEED_MHI_MODALIAS_DEV_ID
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0) && \
		!(defined(CONFIG_SUSE_VERSION) && \
			CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) && \
		!(defined(RHEL_MAJOR) && \
			RHEL_MAJOR == 8 && RHEL_MINOR >= 4))
#define _QBP_NEED_DRM_DEV_ATOMIC_OPEN_COUNT
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0) && \
		!(defined(CONFIG_SUSE_VERSION) && \
			CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) && \
		!(defined(RHEL_MAJOR) && \
			RHEL_MAJOR == 8 && RHEL_MINOR >= 4))
#define _QBP_ALT_DRM_MANAGED
#define _QBP_REDEF_DEVM_DRM_ALLOC
#define _QBP_HAS_INT_DEBUGFS_INIT_RETVAL
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
#define _QBP_NEED_DMA_SYNC_SGTABLE
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0) && \
		!(defined(CONFIG_SUSE_VERSION)) && \
		!(defined(RHEL_MAJOR) && \
			(RHEL_MAJOR >= 8) && (RHEL_MINOR >= 4)))
#define _QBP_REDEF_GEM_OBJ_PUT
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 67) && \
		(LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0))) || \
		(LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 6))
#define _QBP_HAS_DRM_FILE_LOOKUP_LOCK
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0) || defined(RHEL_MAJOR) && (RHEL_MAJOR == 9))
#define _QBP_HAS_MOD_IMPORT_DMA_BUF
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)) && \
	!(LINUX_VERSION_CODE < KERNEL_VERSION(5, 16, 0) && \
		LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 86))
#define _QBP_NEED_OVERFLOW_SIZE_ADD
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0) && !defined(_QBP_ALT_DRM_MANAGED) && \
		!(defined(RHEL_MAJOR) && RHEL_MAJOR == 8 && RHEL_MINOR >= 8) && \
		!(defined(RHEL_MAJOR) && RHEL_MAJOR == 9 && RHEL_MINOR >= 2))
#define _QBP_NEED_DRM_MANAGED_MUTEX
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 4))
#define _QBP_HAS_PRANDOM_U32
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0))
#define _QBP_NEED_DRM_ACCEL_FRAMEWORK
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0))
#define _QBP_CLASS_CREATE_ONE_ARG
#define _QBP_HAS_UEVENT_CONST_DEV
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0))
#define _QBP_NEED_ACCEL_FOP_MMAP
#define _QBP_HAS_INCLUSIVE_MAX_ORDER
#endif

#endif /* end _QAIC_BACKPORT_FLAGS_H_ */
