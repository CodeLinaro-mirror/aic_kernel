# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for Qualcomm Cloud AI accelerators driver
#

obj-$(CONFIG_DRM_ACCEL_QAIC)	:= qaic.o

qaic-y := \
	mhi_controller.o \
	mhi_qaic_ctrl.o \
	qaic_control.o \
	qaic_data.o \
	qaic_drv.o \
	qaic_ras.o \
	qaic_ssr.o \
	qaic_sysfs.o \
	qaic_timesync.o \
	qaic_trace_points.o

qaic-$(CONFIG_DEBUG_FS) += qaic_debugfs.o
qaic-$(CONFIG_DRM_QAIC_HWMON) += qaic_telemetry.o

CFLAGS_qaic_trace_points.o := -I$(src)
