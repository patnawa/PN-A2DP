/*
 * audio_adapter.cpp - PortCls adapter object: owns the device context, installs the
 * Wave (WaveRT) and Topology subdevices and bridges them.
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"

#pragma code_seg("PAGE")
NTSTATUS NewPnAdapterCommon(_Out_ PUNKNOWN *Unknown, _In_ REFCLSID, _In_opt_ PUNKNOWN UnknownOuter, _In_ POOL_FLAGS PoolFlags)
{
    PAGED_CODE();
    ASSERT(Unknown);
    CPnAdapterCommon *p = new (PoolFlags, PNA2DP_TAG_AUDIO) CPnAdapterCommon(UnknownOuter);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;
    *Unknown = PUNKNOWN((PPNADAPTERCOMMON)p);
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
CPnAdapterCommon::~CPnAdapterCommon()
{
    PAGED_CODE();
    Cleanup();
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnAdapterCommon::NonDelegatingQueryInterface(_In_ REFIID Interface, _COM_Outptr_ PVOID *Object)
{
    PAGED_CODE();
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) *Object = PVOID(PUNKNOWN((PPNADAPTERCOMMON)this));
    else if (IsEqualGUIDAligned(Interface, IID_IPnAdapterCommon)) *Object = PVOID((PPNADAPTERCOMMON)this);
    else if (IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement3)) *Object = PVOID((PADAPTERPOWERMANAGEMENT3)this);
    else if (IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement2)) *Object = PVOID((PADAPTERPOWERMANAGEMENT2)this);
    else if (IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement)) *Object = PVOID((PADAPTERPOWERMANAGEMENT)this);
    else *Object = NULL;
    if (*Object) { PUNKNOWN(*Object)->AddRef(); return STATUS_SUCCESS; }
    return STATUS_INVALID_PARAMETER;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnAdapterCommon::Init(_In_ PDEVICE_OBJECT DeviceObject, _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
    m_DeviceObject = DeviceObject;
    m_DriverObject = DriverObject;
    m_PowerState = PowerDeviceD0;
    m_Volume[0] = m_Volume[1] = 0;
    m_Mute[0] = m_Mute[1] = FALSE;
    NTSTATUS status = PcGetPhysicalDeviceObject(DeviceObject, &m_Pdo);
    if (!NT_SUCCESS(status)) return status;
    /* WDF miniport device over the PortCls FDO; the default I/O target is the PDO (BthEnum). */
    status = WdfDeviceMiniportCreate(WdfGetDriver(), WDF_NO_OBJECT_ATTRIBUTES, DeviceObject, m_Pdo, m_Pdo, &m_WdfDevice);
    if (!NT_SUCCESS(status)) { PN_ERR("WdfDeviceMiniportCreate failed 0x%08x", status); return status; }
    status = DeviceCreate(DeviceObject, m_Pdo, DriverObject, this, &m_Device);
    if (!NT_SUCCESS(status)) return status;
    status = DeviceStart(m_Device);
    if (!NT_SUCCESS(status)) { PN_ERR("DeviceStart failed 0x%08x", status); return status; }
    m_Volume[0] = m_Volume[1] = m_Device->Config.VolumeDb16;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(VOID) CPnAdapterCommon::Cleanup()
{
    PAGED_CODE();
    RemoveFilters();
    if (m_Device) {
        DeviceStop(m_Device);
        DeviceDestroy(m_Device);
        m_Device = NULL;
    }
    if (m_WdfDevice) {
        WdfObjectDelete(m_WdfDevice);
        m_WdfDevice = NULL;
    }
}

STDMETHODIMP_(PDEVICE_OBJECT) CPnAdapterCommon::GetDeviceObject() { return m_DeviceObject; }
STDMETHODIMP_(PPNA2DP_DEVICE) CPnAdapterCommon::GetDevice() { return m_Device; }
STDMETHODIMP_(VOID) CPnAdapterCommon::SetWaveMiniport(_In_opt_ CPnMiniportWaveRT *Wave) { m_Wave = Wave; }

STDMETHODIMP_(LONG) CPnAdapterCommon::GetVolume(_In_ ULONG Channel) { return m_Volume[Channel & 1]; }
STDMETHODIMP_(VOID) CPnAdapterCommon::SetVolume(_In_ ULONG Channel, _In_ LONG Db16)
{
    if (Channel == (ULONG)-1) { m_Volume[0] = m_Volume[1] = Db16; } else m_Volume[Channel & 1] = Db16;
    if (m_Device) DeviceAudioVolumeChanged(m_Device, PN_MAX(m_Volume[0], m_Volume[1]), m_Mute[0] && m_Mute[1]);
}
STDMETHODIMP_(BOOL) CPnAdapterCommon::GetMute(_In_ ULONG Channel) { return m_Mute[Channel & 1]; }
STDMETHODIMP_(VOID) CPnAdapterCommon::SetMute(_In_ ULONG Channel, _In_ BOOL Mute)
{
    if (Channel == (ULONG)-1) { m_Mute[0] = m_Mute[1] = Mute; } else m_Mute[Channel & 1] = Mute;
    if (m_Device) DeviceAudioVolumeChanged(m_Device, PN_MAX(m_Volume[0], m_Volume[1]), m_Mute[0] && m_Mute[1]);
}

/* ---- subdevice installation --------------------------------------------- */

#pragma code_seg("PAGE")
NTSTATUS CPnAdapterCommon::InstallSubdevice(_In_ PIRP Irp, _In_ PCWSTR Name, _In_ REFGUID PortClassId, _In_ PUNKNOWN Miniport, _Out_ PUNKNOWN *OutPort)
{
    PAGED_CODE();
    PPORT port = NULL;
    *OutPort = NULL;
    NTSTATUS status = PcNewPort(&port, PortClassId);
    if (!NT_SUCCESS(status)) return status;
    status = port->Init(m_DeviceObject, Irp, Miniport, PUNKNOWN((PPNADAPTERCOMMON)this), NULL);
    if (NT_SUCCESS(status)) {
        status = PcRegisterSubdevice(m_DeviceObject, (PWSTR)Name, port);
    }
    if (NT_SUCCESS(status)) {
        status = port->QueryInterface(IID_IUnknown, (PVOID *)OutPort);
    }
    port->Release();
    return status;
}

#pragma code_seg("PAGE")
NTSTATUS CPnAdapterCommon::UnregisterSubdevice(_In_opt_ PUNKNOWN Port)
{
    PAGED_CODE();
    if (!Port) return STATUS_SUCCESS;
    PUNREGISTERSUBDEVICE unreg = NULL;
    NTSTATUS status = Port->QueryInterface(IID_IUnregisterSubdevice, (PVOID *)&unreg);
    if (NT_SUCCESS(status)) {
        status = unreg->UnregisterSubdevice(m_DeviceObject, Port);
        unreg->Release();
    }
    return status;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnAdapterCommon::InstallFilters(_In_ PIRP Irp)
{
    PAGED_CODE();
    NTSTATUS status;
    PUNKNOWN topoMini = NULL, waveMini = NULL;

    status = NewPnMiniportTopology(&topoMini, (PPNADAPTERCOMMON)this, POOL_FLAG_NON_PAGED);
    if (!NT_SUCCESS(status)) return status;
    status = InstallSubdevice(Irp, PN_TOPO_NAME, CLSID_PortTopology, topoMini, &m_TopoPort);
    topoMini->Release();
    if (!NT_SUCCESS(status)) { PN_ERR("topology subdevice failed 0x%08x", status); return status; }

    status = NewPnMiniportWaveRT(&waveMini, (PPNADAPTERCOMMON)this, POOL_FLAG_NON_PAGED);
    if (!NT_SUCCESS(status)) return status;
    status = InstallSubdevice(Irp, PN_WAVE_NAME, CLSID_PortWaveRT, waveMini, &m_WavePort);
    waveMini->Release();
    if (!NT_SUCCESS(status)) { PN_ERR("wave subdevice failed 0x%08x", status); return status; }

    /* wave bridge pin -> topology input pin */
    status = PcRegisterPhysicalConnection(m_DeviceObject, m_WavePort, PN_WAVE_PIN_BRIDGE, m_TopoPort, PN_TOPO_PIN_WAVEIN);
    if (!NT_SUCCESS(status)) { PN_ERR("PcRegisterPhysicalConnection failed 0x%08x", status); return status; }
    PN_INFO("audio filters installed");
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(VOID) CPnAdapterCommon::RemoveFilters()
{
    PAGED_CODE();
    if (m_WavePort) { UnregisterSubdevice(m_WavePort); m_WavePort->Release(); m_WavePort = NULL; }
    if (m_TopoPort) { UnregisterSubdevice(m_TopoPort); m_TopoPort->Release(); m_TopoPort = NULL; }
}

/* ---- power ------------------------------------------------------------------ */

#pragma code_seg("PAGE")
STDMETHODIMP_(void) CPnAdapterCommon::PowerChangeState(_In_ POWER_STATE NewState)
{
    PAGED_CODE();
    m_PowerState = NewState.DeviceState;
    PN_INFO("power state D%u", (ULONG)m_PowerState - 1);
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnAdapterCommon::QueryPowerChangeState(_In_ POWER_STATE NewStateQuery)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(NewStateQuery);
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnAdapterCommon::QueryDeviceCapabilities(_Inout_updates_bytes_(sizeof(DEVICE_CAPABILITIES)) PDEVICE_CAPABILITIES PowerDeviceCaps)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(PowerDeviceCaps);
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(void) CPnAdapterCommon::PowerChangeState2(_In_ DEVICE_POWER_STATE NewDeviceState, _In_ SYSTEM_POWER_STATE NewSystemState)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(NewSystemState);
    m_PowerState = NewDeviceState;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(void) CPnAdapterCommon::PowerChangeState3(_In_ DEVICE_POWER_STATE NewDeviceState, _In_ SYSTEM_POWER_STATE NewSystemState, _In_ PC_EXIT_LATENCY D3ExitLatency)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(NewSystemState);
    UNREFERENCED_PARAMETER(D3ExitLatency);
    m_PowerState = NewDeviceState;
    PN_INFO("power state D%u", (ULONG)NewDeviceState - 1);
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnAdapterCommon::D3ExitLatencyChanged(_In_ PC_EXIT_LATENCY NewD3ExitLatency)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(NewD3ExitLatency);
    return STATUS_SUCCESS;
}
#pragma code_seg()
