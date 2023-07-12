/* SPDX-License-Identifier: GPL-2.0-only */

/* Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved. */

#ifndef MHI_BACKSUPPORT_H_
#define MHI_BACKSUPPORT_H_

#include <linux/version.h>
#include <generated/utsrelease.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)) && \
	!(defined(CONFIG_SUSE_VERSION) && \
		CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) && \
	!(defined(RHEL_MAJOR) && (RHEL_MAJOR == 8) && (RHEL_MINOR >= 4))
#include <linux/delay.h>
#endif //LINUX_VERSION_CODE < 5.8.0 && SUSE_VERSION<15.3 && RHEL_VERSION < 8.4

/*
 * UTS_UBUNTU_RELEASE_ABI can be an invalid octal number, we can't force it to
 * be read as a decimal, but we can force it to be hexidecimal. It doesn't stop
 * there, the C preprocessor doesn't like concatenating integers, so a helper
 * macro is needed.
 */
#ifdef UTS_UBUNTU_RELEASE_ABI
#ifndef UBUNTU_ABI
#define CONCAT_HLP(x, y) x##y
#define HEXIFY(z) CONCAT_HLP(0x, z)
#define UBUNTU_ABI HEXIFY(UTS_UBUNTU_RELEASE_ABI)
#endif //UBUNTU_ABI defined
#define SYSFS_ABI_CHECK HEXIFY(139)
#endif //UTS_UBUNTU_RELEASE_ABI defined

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

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)  && \
	(LINUX_VERSION_CODE < KERNEL_VERSION(4, 9, 260) || \
		LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)) && \
	(LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 224) || \
		LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)) && \
	(LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 179) || \
		LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)) && \
	(LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 103) || \
		LINUX_VERSION_CODE >= KERNEL_VERSION(5, 5, 0)) && \
	!(defined(CONFIG_SUSE_VERSION) && \
		CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) &&\
	!(defined(RHEL_MAJOR) && (RHEL_MAJOR == 8 && (RHEL_MINOR >= 5))) && \
	!(defined(UBUNTU_ABI) && UBUNTU_ABI >= SYSFS_ABI_CHECK && \
		LINUX_VERSION_CODE <= KERNEL_VERSION(4, 15, 18) && \
		LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
static inline int sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list args;
	int len;

	if (WARN(!buf || offset_in_page(buf),
		 "invalid sysfs_emit: buf:%p\n", buf))
		return 0;

	va_start(args, fmt);
	len = vscnprintf(buf, PAGE_SIZE, fmt, args);
	va_end(args);

	return len;
}

static inline int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
	va_list args;
	int len;

	if (WARN(!buf || offset_in_page(buf) || at < 0 || at >= PAGE_SIZE,
		 "invalid sysfs_emit_at: buf:%p at:%d\n", buf, at))
		return 0;

	va_start(args, fmt);
	len = vscnprintf(buf + at, PAGE_SIZE - at, fmt, args);
	va_end(args);

	return len;
}
#endif /*
	* LINUX_VERSION_CODE < 5.10 && (<5.4.103 || >5.4.214) &&
	*			RHEL < 8.5 && < Ubuntu18.04-4.15.0-139
	*/

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)) && \
	!(defined(CONFIG_SUSE_VERSION) && \
		CONFIG_SUSE_VERSION == 15 && CONFIG_SUSE_PATCHLEVEL >= 3) && \
	!(defined(RHEL_MAJOR) && (RHEL_MAJOR == 8) && (RHEL_MINOR >= 4))
static inline void fsleep(unsigned long usecs)
{
	if (usecs <= 10)
		udelay(usecs);
	else if (usecs <= 20000)
		usleep_range(usecs, 2 * usecs);
	else
		msleep(DIV_ROUND_UP(usecs, 1000));
}
#endif //LINUX_VERSION_CODE < 5.8.0 && SUSE_VERSION<15.3 && RHEL_VERSION < 8.4

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 9, 0) && \
		!(defined(RHEL_MAJOR) && (RHEL_MAJOR == 7) && (RHEL_MINOR >= 9)))
#define BUILD_BUG_ON_MSG(cond, msg) compiletime_assert(!(cond), msg)

#define __BUILD_BUG_ON_NOT_POWER_OF_2(n)        \
	BUILD_BUG_ON(((n) & ((n) - 1)) != 0)

#define __bf_shf(x) (__builtin_ffsll(x) - 1)

#define __BF_FIELD_CHECK(_mask, _reg, _val, _pfx)			\
	({								\
		 BUILD_BUG_ON_MSG(!__builtin_constant_p(_mask),		\
			 _pfx "mask is not constant");			\
		 BUILD_BUG_ON_MSG((_mask) == 0, _pfx "mask is zero");	\
		 BUILD_BUG_ON_MSG(__builtin_constant_p(_val) ?		\
			~((_mask) >> __bf_shf(_mask)) & (_val) : 0,	\
			_pfx "value too large for the field");		\
		 BUILD_BUG_ON_MSG((_mask) > (typeof(_reg))~0ull,	\
			_pfx "type of reg too small for mask");		\
		__BUILD_BUG_ON_NOT_POWER_OF_2((_mask) +			\
			(1ULL << __bf_shf(_mask)));			\
	})
#define FIELD_PREP(_mask, _val)						\
	({								\
	  __BF_FIELD_CHECK(_mask, 0ULL, _val, "FIELD_PREP: ");		\
	  ((typeof(_mask))(_val) << __bf_shf(_mask)) & (_mask);		\
	})
#define FIELD_GET(_mask, _reg)						\
	({								\
	 __BF_FIELD_CHECK(_mask, _reg, 0U, "FIELD_GET: ");		\
	 (typeof(_mask))(((_reg) & (_mask)) >> __bf_shf(_mask));	\
	})
#endif //LINUX_VERSION_CODE < 4.9.0

#endif //MHI_BACKSUPPORT_H_
