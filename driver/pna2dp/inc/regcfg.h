/*
 * regcfg.h - persistence of global and per-device settings.
 *
 *   HKLM\SYSTEM\CurrentControlSet\Services\pna2dp\Parameters
 *       DebugLevel            REG_DWORD   debugger output (1 error, 2 warning, 3 info, 4 verbose)
 *                                         and verbose ring capture; the trace ring always keeps
 *                                         error, warning and info. Default 2 (code and INF).
 *   HKLM\SYSTEM\CurrentControlSet\Services\pna2dp\State
 *       UncleanStarts         REG_DWORD   crash guard, see below
 *       Devices\<bdaddr>\Config        REG_BINARY  PNA2DP_CONFIG
 *       Devices\<bdaddr>\Capabilities  REG_BINARY  PNA2DP_CAPABILITIES
 *       Devices\<bdaddr>\Name          REG_SZ
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

VOID     RegCfgDefaults(_Out_ PPNA2DP_CONFIG Cfg);
NTSTATUS RegCfgInitialize(_In_ PDRIVER_OBJECT DriverObject);
VOID     RegCfgShutdown();
NTSTATUS RegCfgLoadGlobal(_In_ PDRIVER_OBJECT DriverObject);
NTSTATUS RegCfgLoadDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ BTH_ADDR Address, _Inout_ PPNA2DP_CONFIG Cfg);
NTSTATUS RegCfgSaveDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ BTH_ADDR Address, _In_ const PNA2DP_CONFIG *Cfg,
                          _In_opt_ PCSTR Name);
NTSTATUS RegCfgSaveCapabilities(_In_ PDRIVER_OBJECT DriverObject, _In_ BTH_ADDR Address,
                                _In_ const PNA2DP_CAPABILITIES *Caps);
/* Capabilities saved at the last connection (zeroed on failure). The caller must still
 * check them (PnWaveSanitizeCachedCaps): the value may come from another build. */
NTSTATUS RegCfgLoadCapabilities(_In_ PDRIVER_OBJECT DriverObject, _In_ BTH_ADDR Address,
                                _Out_ PPNA2DP_CAPABILITIES Caps);
BOOLEAN  RegCfgValidate(_Inout_ PPNA2DP_CONFIG Cfg);

/* Crash guard (Services\pna2dp\State\UncleanStarts, REG_DWORD). DriverEntry counts every
 * load; a clean shutdown, a driver unload or 10 minutes of uptime reset the count. After
 * PN_CRASH_GUARD_LIMIT loads in a row that ended without one of those (a crash loop), the
 * driver refuses to load so Windows can boot. Delete the value to try again. */
#define PN_CRASH_GUARD_LIMIT 2   /* WinRE opens after two failed boots; the third boot then succeeds */
BOOLEAN  RegCfgCrashGuardEnter(_In_ PDRIVER_OBJECT DriverObject, _Out_ PULONG Count);
VOID     RegCfgCrashGuardClear(_In_ PDRIVER_OBJECT DriverObject);
