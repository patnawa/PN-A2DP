/*
 * PN-A2DP kernel driver - master header.
 *
 * Layering (top to bottom):
 *   audio_*.cpp     PortCls WaveRT render miniport + topology (Windows audio endpoint)
 *   pump.cpp        PCM ring -> encoder (kernel SBC or user-mode codec host) -> RTP -> L2CAP
 *   avdtp.cpp       AVDTP 1.3 signalling state machine (initiator and acceptor)
 *   a2dp_codec.cpp  codec capability parsing / selection / configuration building
 *   bthlink.cpp     BRB based L2CAP channels, SDP queries, HCI/L2CAP PnP events
 *   control.cpp     private device interface + IOCTLs (status, config, codec host)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#define PNA2DP_DRIVER_VERSION   0x00010000u   /* 0.1.0.0 */

#ifdef PN_DEFINE_GUIDS
#include <initguid.h>      /* driver.cpp defines every GUID exactly once */
#endif
#include <portcls.h>
#include <stdunk.h>
#include <ksdebug.h>
#include <ntintsafe.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <wdfminiport.h>
#include <bthdef.h>
#include <bthguid.h>
#include <bthioctl.h>
#include <sdpnode.h>
#include <bthddi.h>
#include <bthsdpddi.h>
#include <bthsdpdef.h>
#include <devpkey.h>
#include <wdmguid.h>

#include "pna2dp_public.h"

/* C++ allocation operators (implemented in driver.cpp; stdunk.h's inline versions are disabled
 * by _NEW_DELETE_OPERATORS_). */
void *__cdecl operator new(size_t Size, POOL_FLAGS PoolFlags, ULONG Tag);
void *__cdecl operator new(size_t Size, POOL_FLAGS PoolFlags);
void __cdecl operator delete(void *p, POOL_FLAGS, ULONG);
void __cdecl operator delete(void *p, POOL_FLAGS);

/* ------------------------------------------------------------------------- */
/* Pool tags                                                                  */
/* ------------------------------------------------------------------------- */
#define PNA2DP_TAG_DEVICE   'vDnP'
#define PNA2DP_TAG_BRB      'bRnP'
#define PNA2DP_TAG_L2CAP    '2LnP'
#define PNA2DP_TAG_AVDTP    'vAnP'
#define PNA2DP_TAG_PCM      'cPnP'
#define PNA2DP_TAG_CODEC    'dCnP'
#define PNA2DP_TAG_AUDIO    'uAnP'
#define PNA2DP_TAG_SDP      'dSnP'
#define PNA2DP_TAG_CTL      'tCnP'
#define PNA2DP_TAG_GENERIC  'nGnP'

/* ------------------------------------------------------------------------- */
/* Tracing: DbgPrintEx with a runtime level (registry "DebugLevel")           */
/* ------------------------------------------------------------------------- */
extern ULONG g_PnDebugLevel;    /* 0 none, 1 error, 2 warning, 3 info, 4 verbose */

#define PN_LEVEL_ERROR   1
#define PN_LEVEL_WARN    2
#define PN_LEVEL_INFO    3
#define PN_LEVEL_VERBOSE 4

#define PN_TRACE(level, fmt, ...) \
    do { if (g_PnDebugLevel >= (level)) { \
        DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "pna2dp: " fmt "\n", __VA_ARGS__); } } while (0)

#define PN_ERR(fmt, ...)   PN_TRACE(PN_LEVEL_ERROR,   "[E] " fmt, __VA_ARGS__)
#define PN_WARN(fmt, ...)  PN_TRACE(PN_LEVEL_WARN,    "[W] " fmt, __VA_ARGS__)
#define PN_INFO(fmt, ...)  PN_TRACE(PN_LEVEL_INFO,    "[I] " fmt, __VA_ARGS__)
#define PN_VERB(fmt, ...)  PN_TRACE(PN_LEVEL_VERBOSE, "[V] " fmt, __VA_ARGS__)

#define PN_MIN(a, b) ((a) < (b) ? (a) : (b))
#define PN_MAX(a, b) ((a) > (b) ? (a) : (b))
#define PN_ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))

#define PN_HNS_PER_MS   10000ll
#define PN_HNS_PER_SEC  10000000ll

/* Bluetooth constants */
#define A2DP_UUID_AUDIO_SINK    0x110B
#define A2DP_UUID_AUDIO_SOURCE  0x110A
#define A2DP_UUID_ADVANCED_AUDIO 0x110D

/* Forward declarations */
struct PNA2DP_DEVICE;
typedef struct PNA2DP_DEVICE *PPNA2DP_DEVICE;

struct PN_L2CAP_CHANNEL;
typedef struct PN_L2CAP_CHANNEL *PPN_L2CAP_CHANNEL;

class CPnAdapterCommon;
class CPnMiniportWaveRT;
class CPnMiniportWaveRTStream;
class CPnMiniportTopology;

/* Allocation helpers (non-paged by default; we touch most structures at DISPATCH). */
__forceinline PVOID PnAllocNp(_In_ SIZE_T size, _In_ ULONG tag)
{
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, size, tag);
}
__forceinline PVOID PnAllocPaged(_In_ SIZE_T size, _In_ ULONG tag)
{
    return ExAllocatePool2(POOL_FLAG_PAGED, size, tag);
}
__forceinline VOID PnFree(_In_opt_ PVOID p, _In_ ULONG tag)
{
    if (p) ExFreePoolWithTag(p, tag);
}

/* Monotonic time in 100 ns units (interrupt time, precise). */
__forceinline LONGLONG PnNowHns(void)
{
    return (LONGLONG)KeQueryInterruptTimePrecise(NULL);
}

#include "bthlink.h"
#include "avdtp.h"
#include "a2dp_codec.h"
extern "C" {
#include "../../../common/sbc/sbc.h"
}
#include "rtp.h"
#include "pump.h"
#include "codechost.h"
#include "regcfg.h"
#include "device.h"
#include "control.h"
#include "audio.h"
