/*
 * control.h - private control interface.
 *
 * The FDO belongs to PortCls. We hook IRP_MJ_CREATE / CLEANUP / CLOSE / DEVICE_CONTROL
 * (and fail every other PortCls major on a control handle) in the driver object: opens
 * whose file name is the reference string
 * L"\\pna2dpctl" are ours; everything else (KS filters/pins) goes to PcDispatchIrp.
 * The device interface GUID_DEVINTERFACE_PNA2DP is registered on the PDO with that
 * reference string, so user mode opens "\\?\BTHENUM#...#{guid}\pna2dpctl".
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#define PNA2DP_CONTROL_REFSTRING  L"pna2dpctl"

NTSTATUS ControlRegisterInterface(_Inout_ PPNA2DP_DEVICE D);
VOID     ControlUnregisterInterface(_Inout_ PPNA2DP_DEVICE D);

_Dispatch_type_(IRP_MJ_CREATE)         DRIVER_DISPATCH ControlDispatchCreate;
_Dispatch_type_(IRP_MJ_CLEANUP)        DRIVER_DISPATCH ControlDispatchCleanup;
_Dispatch_type_(IRP_MJ_CLOSE)          DRIVER_DISPATCH ControlDispatchClose;
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL) DRIVER_DISPATCH ControlDispatchDeviceControl;
/* every other major PortCls installed, except PNP / POWER / SYSTEM_CONTROL / SHUTDOWN */
_Dispatch_type_(IRP_MJ_READ) _Dispatch_type_(IRP_MJ_WRITE) _Dispatch_type_(IRP_MJ_FLUSH_BUFFERS)
_Dispatch_type_(IRP_MJ_QUERY_SECURITY) _Dispatch_type_(IRP_MJ_SET_SECURITY) DRIVER_DISPATCH ControlDispatchOther;

BOOLEAN  ControlIsOurFile(_In_ PFILE_OBJECT File);
