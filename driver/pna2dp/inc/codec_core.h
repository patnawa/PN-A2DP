/* Platform includes for the pure codec/packet helpers, also built by core_test.
 * No kernel scheduling or Bluetooth APIs are emulated by the tests.
 * SPDX-License-Identifier: MIT */
#pragma once
#ifdef _KERNEL_MODE
#include "pna2dp.h"
#else
#include "pna2dp_public.h"
#include "../../../common/sbc/sbc.h"
#include "a2dp_codec.h"
#include "rtp.h"
#define PN_MIN(a, b) ((a) < (b) ? (a) : (b))
#define PN_MAX(a, b) ((a) > (b) ? (a) : (b))
#define PN_ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
