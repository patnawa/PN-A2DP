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
    /* The miniports and streams hold references on this object, so nothing that keeps a
     * raw pointer to the device context is left when the destructor runs. */
    if (m_Device) {
        DeviceRelease(m_Device);
        m_Device = NULL;
    }
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnAdapterCommon::NonDelegatingQueryInterface(_In_ REFIID Interface, _COM_Outptr_ PVOID *Object)
{
    PAGED_CODE();
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) *Object = PVOID(PUNKNOWN((PPNADAPTERCOMMON)this));
    else if (IsEqualGUIDAligned(Interface, IID_IPnAdapterCommon)) *Object = PVOID((PPNADAPTERCOMMON)this);
    /* IAdapterPowerManagement, ...2 and ...3 all derive from IUnknown in portcls.h; this class
     * only implements ...3, whose vtable starts with the v1 and v2 methods. A cast to the
     * v1/v2 types would be a reinterpret_cast to the IPnAdapterCommon subobject. */
    else if (IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement3) ||
             IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement2) ||
             IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement)) *Object = PVOID((PADAPTERPOWERMANAGEMENT3)this);
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
    m_Topology = NULL;
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
        /* Quiesce and free the Bluetooth/pump side now (this runs on surprise removal too,
         * while audio streams may still be open). The device structure itself is kept until
         * the destructor, because streams and control handles may still reference it. */
        DeviceStop(m_Device);
        DeviceTeardown(m_Device);
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
/* The volume and mute nodes advertise a single (master) channel, so every channel index
 * sets both entries. */
STDMETHODIMP_(VOID) CPnAdapterCommon::SetVolume(_In_ ULONG Channel, _In_ LONG Db16)
{
    UNREFERENCED_PARAMETER(Channel);
    m_Volume[0] = m_Volume[1] = Db16;
    if (m_Device) DeviceAudioVolumeChanged(m_Device, Db16, m_Mute[0] != FALSE);
    if (m_Topology) { m_Topology->GenerateVolumeEvent(); m_Topology->GenerateRemoteVolumeEvent(); }
}
STDMETHODIMP_(BOOL) CPnAdapterCommon::GetMute(_In_ ULONG Channel) { return m_Mute[Channel & 1]; }
STDMETHODIMP_(VOID) CPnAdapterCommon::SetMute(_In_ ULONG Channel, _In_ BOOL Mute)
{
    UNREFERENCED_PARAMETER(Channel);
    m_Mute[0] = m_Mute[1] = Mute;
    if (m_Device) DeviceAudioVolumeChanged(m_Device, m_Volume[0], Mute != FALSE);
    if (m_Topology) { m_Topology->GenerateVolumeEvent(); m_Topology->GenerateRemoteVolumeEvent(); }
}

VOID CPnAdapterCommon::SetRemoteVolume(_In_ LONG Db16)
{
    /* The receiver already applies this attenuation. Applying it again in the
     * encoder would make playback too quiet. */
    if (!m_Device) return;
    BOOLEAN first = !m_Device->RemoteVolumeActive;
    m_Device->RemoteVolumeActive = TRUE;
    BOOL muted = Db16 <= PN_REMOTE_VOLUME_MIN_DB16;
    if (!muted) m_Volume[0] = m_Volume[1] = Db16;
    m_Mute[0] = m_Mute[1] = muted;
    DeviceAudioVolumeChanged(m_Device, m_Volume[0], muted != FALSE);
    if (m_Topology) m_Topology->GenerateVolumeEvent(); // no echo back to AVRCP
    PN_INFO("remote volume %ld/65536 dB, mute %u%s", Db16, muted, first ? " (absolute volume active)" : "");
}

VOID CPnAdapterCommon::DisableRemoteVolume()
{
    if (!m_Device || !m_Device->RemoteVolumeActive) return;
    m_Device->RemoteVolumeActive = FALSE;
    DeviceAudioVolumeChanged(m_Device, m_Volume[0], m_Mute[0] != FALSE);
    PN_INFO("absolute volume detached; software volume active");
}

/* ---- subdevice installation --------------------------------------------- */

#pragma code_seg("PAGE")
NTSTATUS CPnAdapterCommon::InstallSubdevice(_In_ PIRP Irp, _In_ PRESOURCELIST Resources, _In_ PCWSTR Name, _In_ REFGUID PortClassId, _In_ PUNKNOWN Miniport, _Out_ PUNKNOWN *OutPort)
{
    PAGED_CODE();
    PPORT port = NULL;
    *OutPort = NULL;
    NTSTATUS status = PcNewPort(&port, PortClassId);
    if (!NT_SUCCESS(status)) return status;
    /* Set the per-headset adapter name before EndpointBuilder sees this interface.
     * The INF's static name cannot distinguish multiple Bluetooth receivers. */
    UNICODE_STRING reference, link;
    RtlInitUnicodeString(&reference, Name);
    NTSTATUS nameStatus = IoRegisterDeviceInterface(m_Pdo, &KSCATEGORY_AUDIO, &reference, &link);
    if (NT_SUCCESS(nameStatus)) {
        size_t nameBytes = 0;
        RtlStringCbLengthW(m_Device->Link.RemoteNameW, sizeof(m_Device->Link.RemoteNameW), &nameBytes);
        nameStatus = IoSetDeviceInterfacePropertyData(&link, &DEVPKEY_DeviceInterface_FriendlyName,
            LOCALE_NEUTRAL, PLUGPLAY_PROPERTY_PERSISTENT, DEVPROP_TYPE_STRING,
            (ULONG)(nameBytes + sizeof(WCHAR)), m_Device->Link.RemoteNameW);
        RtlFreeUnicodeString(&link);
    }
    if (!NT_SUCCESS(nameStatus)) PN_WARN("%ws interface name failed 0x%08x", Name, nameStatus);
    status = port->Init(m_DeviceObject, Irp, Miniport, PUNKNOWN((PPNADAPTERCOMMON)this), Resources);
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
STDMETHODIMP_(NTSTATUS) CPnAdapterCommon::InstallFilters(_In_ PIRP Irp, _In_ PRESOURCELIST Resources)
{
    PAGED_CODE();
    NTSTATUS status;
    PUNKNOWN topoMini = NULL, waveMini = NULL;

    status = NewPnMiniportTopology(&topoMini, (PPNADAPTERCOMMON)this, POOL_FLAG_NON_PAGED);
    if (!NT_SUCCESS(status)) return status;
    status = InstallSubdevice(Irp, Resources, PN_TOPO_NAME, CLSID_PortTopology, topoMini, &m_TopoPort);
    topoMini->Release();
    if (!NT_SUCCESS(status)) { PN_ERR("topology subdevice failed 0x%08x", status); return status; }

    status = NewPnMiniportWaveRT(&waveMini, (PPNADAPTERCOMMON)this, POOL_FLAG_NON_PAGED);
    if (!NT_SUCCESS(status)) return status;
    status = InstallSubdevice(Irp, Resources, PN_WAVE_NAME, CLSID_PortWaveRT, waveMini, &m_WavePort);
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
    DevicePowerChanged(m_Device, m_PowerState);
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
    DevicePowerChanged(m_Device, m_PowerState);
}

#pragma code_seg("PAGE")
STDMETHODIMP_(void) CPnAdapterCommon::PowerChangeState3(_In_ DEVICE_POWER_STATE NewDeviceState, _In_ SYSTEM_POWER_STATE NewSystemState, _In_ PC_EXIT_LATENCY D3ExitLatency)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(NewSystemState);
    UNREFERENCED_PARAMETER(D3ExitLatency);
    m_PowerState = NewDeviceState;
    DevicePowerChanged(m_Device, m_PowerState);
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
