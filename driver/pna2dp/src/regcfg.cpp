/*
 * regcfg.cpp - registry persistence (global + per device).
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"

ULONG g_PnDebugLevel = PN_LEVEL_INFO;

VOID RegCfgDefaults(_Out_ PPNA2DP_CONFIG Cfg)
{
    RtlZeroMemory(Cfg, sizeof(*Cfg));
    Cfg->Size = sizeof(*Cfg);
    Cfg->Version = PNA2DP_ABI_VERSION;
    /* Quality first: LDAC > aptX HD > AAC > aptX > aptX LL > SBC. A codec that no host offers is skipped. */
    Cfg->CodecPriority[0] = PNA2DP_CODEC_LDAC;
    Cfg->CodecPriority[1] = PNA2DP_CODEC_APTX_HD;
    Cfg->CodecPriority[2] = PNA2DP_CODEC_AAC;
    Cfg->CodecPriority[3] = PNA2DP_CODEC_APTX;
    Cfg->CodecPriority[4] = PNA2DP_CODEC_APTX_LL;
    Cfg->CodecPriority[5] = PNA2DP_CODEC_SBC;
    Cfg->EnabledCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX) |
                            PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_HD) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_LL) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC);
    Cfg->Flags = PNA2DP_CFG_ABR_ENABLE | PNA2DP_CFG_APPLY_LIVE | PNA2DP_CFG_SUSPEND_ON_SCO | PNA2DP_CFG_DELAY_REPORTING;
    Cfg->TargetLatencyMs = PN_PUMP_DEFAULT_TARGET_MS;
    Cfg->MaxLatencyMs = PN_PUMP_DEFAULT_MAX_MS;
    Cfg->AbrFloorBps = 128000;
    Cfg->VolumeDb16 = 0;
    Cfg->ReconnectDelayMs = 2000;
    Cfg->Sbc.MinBitpool = 2;
    Cfg->Sbc.MaxBitpool = 0;            /* 0 = whatever the sink allows */
    Cfg->Aac.Vbr = 1;
    Cfg->Aac.BitrateBps = 0;
    Cfg->Ldac.Eqmid = PNA2DP_LDAC_EQMID_SQ;
    Cfg->Ldac.AbrEnable = 1;
}

BOOLEAN RegCfgValidate(_Inout_ PPNA2DP_CONFIG Cfg)
{
    if (Cfg->Size != sizeof(*Cfg) || Cfg->Version != PNA2DP_ABI_VERSION) return FALSE;
    if (Cfg->TargetLatencyMs < 20) Cfg->TargetLatencyMs = 20;
    if (Cfg->TargetLatencyMs > 400) Cfg->TargetLatencyMs = 400;
    if (Cfg->MaxLatencyMs < Cfg->TargetLatencyMs + 20) Cfg->MaxLatencyMs = Cfg->TargetLatencyMs + 20;
    if (Cfg->MaxLatencyMs > 1000) Cfg->MaxLatencyMs = 1000;
    if (Cfg->Sbc.MinBitpool && Cfg->Sbc.MinBitpool < 2) Cfg->Sbc.MinBitpool = 2;
    if (Cfg->Sbc.MaxBitpool > 250) Cfg->Sbc.MaxBitpool = 250;
    if (Cfg->Sbc.MaxBitpool && Cfg->Sbc.MinBitpool > Cfg->Sbc.MaxBitpool) Cfg->Sbc.MinBitpool = Cfg->Sbc.MaxBitpool;
    if (Cfg->Ldac.Eqmid > PNA2DP_LDAC_EQMID_MQ && Cfg->Ldac.Eqmid != PNA2DP_LDAC_EQMID_ABR) Cfg->Ldac.Eqmid = PNA2DP_LDAC_EQMID_SQ;
    if (Cfg->VolumeDb16 > 0) Cfg->VolumeDb16 = 0;
    if (Cfg->VolumeDb16 < -96 * 0x10000) Cfg->VolumeDb16 = -96 * 0x10000;
    BOOLEAN any = FALSE;
    for (ULONG i = 0; i < PN_ARRAYSIZE(Cfg->CodecPriority); i++) {
        if (Cfg->CodecPriority[i] >= PNA2DP_CODEC_MAX) Cfg->CodecPriority[i] = 0;
        if (Cfg->CodecPriority[i]) any = TRUE;
    }
    if (!any) Cfg->CodecPriority[0] = PNA2DP_CODEC_SBC;
    Cfg->EnabledCodecMask |= PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC);   /* SBC is mandatory */
    return TRUE;
}

static NTSTATUS OpenDeviceKey(_In_ PDRIVER_OBJECT DriverObject, _In_ BTH_ADDR Address, _In_ BOOLEAN Create, _In_ ACCESS_MASK Access, _Out_ PHANDLE Key)
{
    PAGED_CODE();
    HANDLE params = NULL;
    *Key = NULL;
    NTSTATUS status = IoOpenDriverRegistryKey(DriverObject, DriverRegKeyParameters, Create ? KEY_ALL_ACCESS : KEY_READ,
                                              Create ? 0 : 0, &params);
    if (!NT_SUCCESS(status)) return status;
    WCHAR sub[64];
    RtlStringCchPrintfW(sub, PN_ARRAYSIZE(sub), L"Devices\\%012llX", Address);
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, sub);
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, params, NULL);
    if (Create) {
        /* ZwCreateKey does not create intermediate keys; create "Devices" first */
        UNICODE_STRING devices;
        RtlInitUnicodeString(&devices, L"Devices");
        OBJECT_ATTRIBUTES oad;
        HANDLE hd = NULL;
        InitializeObjectAttributes(&oad, &devices, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, params, NULL);
        status = ZwCreateKey(&hd, KEY_ALL_ACCESS, &oad, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
        if (NT_SUCCESS(status)) ZwClose(hd);
        status = ZwCreateKey(Key, Access, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    } else {
        status = ZwOpenKey(Key, Access, &oa);
    }
    ZwClose(params);
    return status;
}

static NTSTATUS ReadBinary(_In_ HANDLE Key, _In_ PCWSTR Name, _Out_writes_bytes_(Size) PVOID Buffer, _In_ ULONG Size)
{
    PAGED_CODE();
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, Name);
    ULONG len = sizeof(KEY_VALUE_PARTIAL_INFORMATION) + Size;
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)PnAllocPaged(len, PNA2DP_TAG_GENERIC);
    if (!info) return STATUS_INSUFFICIENT_RESOURCES;
    ULONG result = 0;
    NTSTATUS status = ZwQueryValueKey(Key, &n, KeyValuePartialInformation, info, len, &result);
    if (NT_SUCCESS(status)) {
        if (info->DataLength == Size) RtlCopyMemory(Buffer, info->Data, Size);
        else status = STATUS_INFO_LENGTH_MISMATCH;
    }
    PnFree(info, PNA2DP_TAG_GENERIC);
    return status;
}

static NTSTATUS WriteValue(_In_ HANDLE Key, _In_ PCWSTR Name, _In_ ULONG Type, _In_reads_bytes_(Size) const VOID *Data, _In_ ULONG Size)
{
    PAGED_CODE();
    UNICODE_STRING n;
    RtlInitUnicodeString(&n, Name);
    return ZwSetValueKey(Key, &n, 0, Type, (PVOID)Data, Size);
}

NTSTATUS RegCfgLoadGlobal(_In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
    HANDLE params = NULL;
    NTSTATUS status = IoOpenDriverRegistryKey(DriverObject, DriverRegKeyParameters, KEY_READ, 0, &params);
    if (!NT_SUCCESS(status)) return status;
    ULONG level = g_PnDebugLevel;
    if (NT_SUCCESS(ReadBinary(params, L"DebugLevel", &level, sizeof(level)))) g_PnDebugLevel = level;
    ZwClose(params);
    return STATUS_SUCCESS;
}

NTSTATUS RegCfgLoadDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ BTH_ADDR Address, _Inout_ PPNA2DP_CONFIG Cfg)
{
    PAGED_CODE();
    HANDLE key = NULL;
    NTSTATUS status = OpenDeviceKey(DriverObject, Address, FALSE, KEY_READ, &key);
    if (!NT_SUCCESS(status)) return status;
    PNA2DP_CONFIG tmp;
    status = ReadBinary(key, L"Config", &tmp, sizeof(tmp));
    ZwClose(key);
    if (NT_SUCCESS(status) && RegCfgValidate(&tmp)) {
        *Cfg = tmp;
        return STATUS_SUCCESS;
    }
    return NT_SUCCESS(status) ? STATUS_INVALID_PARAMETER : status;
}

NTSTATUS RegCfgSaveDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ BTH_ADDR Address, _In_ const PNA2DP_CONFIG *Cfg, _In_opt_ PCSTR Name)
{
    PAGED_CODE();
    HANDLE key = NULL;
    NTSTATUS status = OpenDeviceKey(DriverObject, Address, TRUE, KEY_ALL_ACCESS, &key);
    if (!NT_SUCCESS(status)) return status;
    status = WriteValue(key, L"Config", REG_BINARY, Cfg, sizeof(*Cfg));
    if (Name) {
        WCHAR w[BTH_MAX_NAME_SIZE + 1];
        ULONG i = 0;
        for (; i < BTH_MAX_NAME_SIZE && Name[i]; i++) w[i] = (WCHAR)(UCHAR)Name[i];
        w[i] = 0;
        WriteValue(key, L"Name", REG_SZ, w, (i + 1) * sizeof(WCHAR));
    }
    ZwClose(key);
    return status;
}

NTSTATUS RegCfgSaveCapabilities(_In_ PDRIVER_OBJECT DriverObject, _In_ BTH_ADDR Address, _In_ const PNA2DP_CAPABILITIES *Caps)
{
    PAGED_CODE();
    HANDLE key = NULL;
    NTSTATUS status = OpenDeviceKey(DriverObject, Address, TRUE, KEY_ALL_ACCESS, &key);
    if (!NT_SUCCESS(status)) return status;
    status = WriteValue(key, L"Capabilities", REG_BINARY, Caps, sizeof(*Caps));
    ULONG mask = Caps->CodecMask;
    WriteValue(key, L"CodecMask", REG_DWORD, &mask, sizeof(mask));
    ZwClose(key);
    return status;
}
