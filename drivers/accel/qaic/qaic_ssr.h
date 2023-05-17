/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __QAIC_SSR_H__
#define __QAIC_SSR_H__

int qaic_ssr_register(void);
void qaic_ssr_unregister(void);
void clean_up_ssr(struct qaic_device *qdev, u32 dbc_id);
#endif /* __QAIC_SSR_H__ */
