/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 */

#ifndef __QAIC_TELEMETRY_H__
#define __QAIC_TELEMETRY_H__

#include "qaic.h"

#ifdef CONFIG_DRM_QAIC_HWMON
int qaic_telemetry_register(void);
void qaic_telemetry_unregister(void);
void wake_all_telemetry(struct qaic_device *qdev);
#else
int qaic_telemetry_register(void) { return 0; }
void qaic_telemetry_unregister(void) {}
void wake_all_telemetry(struct qaic_device *qdev) {}
#endif /* CONFIG_DRM_QAIC_HWMON */
#endif /* __QAIC_TELEMETRY_H__ */
