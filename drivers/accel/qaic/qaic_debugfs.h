/* SPDX-License-Identifier: GPL-2.0-only */

/* Copyright (c) 2020, The Linux Foundation. All rights reserved. */

#ifndef __QAIC_DEBUGFS_H__
#define __QAIC_DEBUGFS_H__

#include <drm/drm_file.h>

#define DBC_DEBUGFS_ENTRIES			2

#ifdef CONFIG_DEBUG_FS
int qaic_bootlog_register(void);
void qaic_bootlog_unregister(void);
void qaic_debugfs_init(struct drm_minor *minor);
#else
int qaic_bootlog_register(void) { return 0; }
void qaic_bootlog_unregister(void) {}
void qaic_debugfs_init(struct drm_minor *minor) {}
#endif /* CONFIG_DEBUG_FS */
#endif /* __QAIC_DEBUGFS_H__ */
