// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved. */

/*
 * __CHECKER__ preprocessor is defined by sparse and sparse doesn't like
 * tracepoint macros such as __align()
 */
#ifndef __CHECKER__
#define CREATE_TRACE_POINTS
#include "qaic_trace.h"
#endif
