/*
 * regcfg.h - persistence of global and per-device settings.
 *
 *   HKLM\SYSTEM\CurrentControlSet\Services\pna2dp\Parameters
 *       DebugLevel            REG_DWORD
 *       Devices\<bdaddr>\Config        REG_BINARY  PNA2DP_CONFIG
 *       Devices\<bdaddr>\Capabilities  REG_BINARY  PNA2DP_CAPABILITIES
 *       Devices\<bdaddr>\Name          REG_SZ
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

VOID     RegCfgDefaults(_Out_ PPNA2DP_CONFIG Cfg);
NTSTATUS RegCfgLoadGlobal(_In_ PDRIVER_OBJECT DriverObject);
NTSTATUS RegCfgLoadDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ BTH_ADDR Address, _Inout_ PPNA2DP_CONFIG Cfg);
NTSTATUS RegCfgSaveDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ BTH_ADDR Address, _In_ const PNA2DP_CONFIG *Cfg,
                          _In_opt_ PCSTR Name);
NTSTATUS RegCfgSaveCapabilities(_In_ PDRIVER_OBJECT DriverObject, _In_ BTH_ADDR Address,
                                _In_ const PNA2DP_CAPABILITIES *Caps);
BOOLEAN  RegCfgValidate(_Inout_ PPNA2DP_CONFIG Cfg);
