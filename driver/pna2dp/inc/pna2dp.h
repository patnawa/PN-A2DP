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

#define PNA2DP_DRIVER_VERSION   0x00010019u   /* 0.1.0.25 */

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
 * by _NEW_DELETE_OPERATORS_). POOL_FLAGS is a 64-bit integer like size_t, so a
 * (void *, POOL_FLAGS) delete would silently be the C++14 sized delete: the sized delete
 * is declared explicitly instead (the unsized operator delete(void *) is in stdunk.lib). */
void *__cdecl operator new(size_t Size, POOL_FLAGS PoolFlags, ULONG Tag);
void *__cdecl operator new(size_t Size, POOL_FLAGS PoolFlags);
void __cdecl operator delete(void *p, POOL_FLAGS, ULONG);   /* placement form of new(size, flags, tag) */
void __cdecl operator delete(void *p, size_t Size) noexcept;

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
/* Tracing (trace.cpp). Each message goes to up to three sinks:                */
/*  - the bounded ring read by IOCTL_PNA2DP_GET_TRACE: errors, warnings and    */
/*    information always, verbose when DebugLevel >= 4;                        */
/*  - DbgPrintEx(DPFLTR_IHVAUDIO_ID) at the matching DPFLTR_ERROR/WARNING/      */
/*    INFO/TRACE level, up to registry "DebugLevel" (INF default 2, warnings); */
/*  - the TraceLogging provider "PnA2dp.Driver" at the matching ETW level.     */
/* ------------------------------------------------------------------------- */
extern ULONG g_PnDebugLevel;    /* 0 none, 1 error, 2 warning, 3 info, 4 verbose */
VOID PnTraceInitialize(_In_ PDRIVER_OBJECT DriverObject);
VOID PnTraceUninitialize(void);
VOID PnTraceWrite(ULONG Level, PCSTR Format, ...);
VOID PnTraceRead(PNA2DP_TRACE *Trace);
BOOLEAN PnTraceEtwEnabled(_In_ ULONG Level);
NTSTATUS PnPortDispatchIrp(PDEVICE_OBJECT DeviceObject, PIRP Irp);

#define PN_LEVEL_ERROR   1
#define PN_LEVEL_WARN    2
#define PN_LEVEL_INFO    3
#define PN_LEVEL_VERBOSE 4

#define PN_TRACE(level, fmt, ...) \
    do { if ((level) <= PN_LEVEL_INFO || g_PnDebugLevel >= (level) || PnTraceEtwEnabled(level)) \
        PnTraceWrite((level), fmt, __VA_ARGS__); } while (0)

#define PN_ERR(fmt, ...)   PN_TRACE(PN_LEVEL_ERROR,   "[E] " fmt, __VA_ARGS__)
#define PN_WARN(fmt, ...)  PN_TRACE(PN_LEVEL_WARN,    "[W] " fmt, __VA_ARGS__)
#define PN_INFO(fmt, ...)  PN_TRACE(PN_LEVEL_INFO,    "[I] " fmt, __VA_ARGS__)
#define PN_VERB(fmt, ...)  PN_TRACE(PN_LEVEL_VERBOSE, "[V] " fmt, __VA_ARGS__)

/* System event log (contract C9; <= DISPATCH_LEVEL). Codes are the standard ntiologc.h
 * IO_ERR_* values whose messages are in IoLogMsg.dll (registered by the INF); %1 in the
 * message is the device or driver name. UniqueValue is stored as the entry's
 * UniqueErrorValue and first dump word. A NULL DeviceObject logs against the driver. */
VOID PnLogDeviceEvent(_In_opt_ PDEVICE_OBJECT DeviceObject, _In_ NTSTATUS Code, _In_ ULONG UniqueValue);
#define PN_EVENT_CRASH_GUARD    IO_ERR_CONFIGURATION_ERROR  /* load refused; UniqueValue = unclean starts */
#define PN_EVENT_LOAD_FAILED    IO_ERR_INTERNAL_ERROR       /* DriverEntry failed; UniqueValue = NTSTATUS */
#define PN_EVENT_START_FAILED   IO_ERR_INTERNAL_ERROR       /* device start failed; UniqueValue = NTSTATUS */
#define PN_EVENT_CODEC_HOST     IO_ERR_LAYERED_FAILURE      /* codec host fallback or quarantine; UniqueValue below */
#define PN_EVENT_CODEC_HOST_QUARANTINE  0x00010000u   /* | quarantine number: the host failed repeatedly */
#define PN_EVENT_CODEC_HOST_FALLBACK    0x00020000u   /* | PNA2DP_CODEC_*: its encoder vanished, renegotiating */

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

/* Mutex for PASSIVE_LEVEL sections. ExAcquireFastMutex would raise to APC_LEVEL, but the
 * sections hold KMDF synchronous sends, Zw* registry calls and IRPs that complete through
 * a special kernel APC, all of which need PASSIVE_LEVEL. A critical region blocks normal
 * kernel APCs (thread suspension) while keeping IRQL at PASSIVE. */
__forceinline VOID PnMutexAcquire(_Inout_ PFAST_MUTEX m)
{
    KeEnterCriticalRegion();
    ExAcquireFastMutexUnsafe(m);
}
__forceinline VOID PnMutexRelease(_Inout_ PFAST_MUTEX m)
{
    ExReleaseFastMutexUnsafe(m);
    KeLeaveCriticalRegion();
}

/* Monotonic time in 100 ns units (interrupt time, precise). */
__forceinline LONGLONG PnNowHns(void)
{
    ULONG64 qpc;
    return (LONGLONG)KeQueryInterruptTimePrecise(&qpc);
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
