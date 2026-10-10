/*
 * audio.h - PortCls adapter, WaveRT render miniport and topology miniport.
 *
 * One render endpoint per device: "Wave" (KSCATEGORY_RENDER) bridged to "Topology"
 * (volume + mute nodes, speaker/headphone jack). Formats advertised depend on the
 * negotiated codec; the engine's chosen PCM format is forwarded to the pump.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#define PN_WAVE_NAME        L"Wave"
#define PN_TOPO_NAME        L"Topology"

enum {
    PN_WAVE_PIN_SYSTEM = 0,     /* host render pin (KSPIN_DATAFLOW_IN) */
    PN_WAVE_PIN_BRIDGE          /* to topology */
};
enum {
    PN_TOPO_PIN_WAVEIN = 0,     /* from wave bridge */
    PN_TOPO_PIN_LINEOUT         /* speaker */
};
enum {
    PN_TOPO_NODE_VOLUME = 0,
    PN_TOPO_NODE_MUTE
};

#define PN_VOLUME_MIN_DB16      (-96 * 0x10000)
#define PN_VOLUME_MAX_DB16      0
#define PN_VOLUME_STEP_DB16     0x8000

/* ------------------------------------------------------------------------- */
/* Adapter common object                                                      */
/* ------------------------------------------------------------------------- */
DEFINE_GUID(IID_IPnAdapterCommon, 0x3a4b9b60, 0x8c4a, 0x4a9f, 0xb0, 0x5b, 0x22, 0x1d, 0x7e, 0x53, 0x9c, 0x11);

DECLARE_INTERFACE_(IPnAdapterCommon, IUnknown)
{
    STDMETHOD_(NTSTATUS, Init)(THIS_ _In_ PDEVICE_OBJECT DeviceObject, _In_ PDRIVER_OBJECT DriverObject) PURE;
    STDMETHOD_(PDEVICE_OBJECT, GetDeviceObject)(THIS) PURE;
    STDMETHOD_(PPNA2DP_DEVICE, GetDevice)(THIS) PURE;
    STDMETHOD_(NTSTATUS, InstallFilters)(THIS_ _In_ PIRP Irp) PURE;
    STDMETHOD_(VOID, RemoveFilters)(THIS) PURE;
    STDMETHOD_(LONG, GetVolume)(THIS_ _In_ ULONG Channel) PURE;
    STDMETHOD_(VOID, SetVolume)(THIS_ _In_ ULONG Channel, _In_ LONG Db16) PURE;
    STDMETHOD_(BOOL, GetMute)(THIS_ _In_ ULONG Channel) PURE;
    STDMETHOD_(VOID, SetMute)(THIS_ _In_ ULONG Channel, _In_ BOOL Mute) PURE;
    STDMETHOD_(VOID, SetWaveMiniport)(THIS_ _In_opt_ CPnMiniportWaveRT *Wave) PURE;
    STDMETHOD_(VOID, Cleanup)(THIS) PURE;
};
typedef IPnAdapterCommon *PPNADAPTERCOMMON;

class CPnAdapterCommon : public IPnAdapterCommon, public IAdapterPowerManagement3, public CUnknown
{
    PDEVICE_OBJECT      m_DeviceObject;
    PDEVICE_OBJECT      m_Pdo;
    PDRIVER_OBJECT      m_DriverObject;
    WDFDEVICE           m_WdfDevice;
    PPNA2DP_DEVICE      m_Device;
    PUNKNOWN            m_WavePort;
    PUNKNOWN            m_TopoPort;
    CPnMiniportWaveRT  *m_Wave;
    LONG                m_Volume[2];
    BOOL                m_Mute[2];
    DEVICE_POWER_STATE  m_PowerState;

    NTSTATUS InstallSubdevice(_In_ PIRP Irp, _In_ PCWSTR Name, _In_ REFGUID PortClassId,
                              _In_ PUNKNOWN Miniport, _Out_ PUNKNOWN *OutPort);
    NTSTATUS UnregisterSubdevice(_In_opt_ PUNKNOWN Port);

public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CPnAdapterCommon);
    ~CPnAdapterCommon();

    STDMETHODIMP_(NTSTATUS) Init(_In_ PDEVICE_OBJECT DeviceObject, _In_ PDRIVER_OBJECT DriverObject);
    STDMETHODIMP_(PDEVICE_OBJECT) GetDeviceObject();
    STDMETHODIMP_(PPNA2DP_DEVICE) GetDevice();
    STDMETHODIMP_(NTSTATUS) InstallFilters(_In_ PIRP Irp);
    STDMETHODIMP_(VOID) RemoveFilters();
    STDMETHODIMP_(LONG) GetVolume(_In_ ULONG Channel);
    STDMETHODIMP_(VOID) SetVolume(_In_ ULONG Channel, _In_ LONG Db16);
    STDMETHODIMP_(BOOL) GetMute(_In_ ULONG Channel);
    STDMETHODIMP_(VOID) SetMute(_In_ ULONG Channel, _In_ BOOL Mute);
    STDMETHODIMP_(VOID) SetWaveMiniport(_In_opt_ CPnMiniportWaveRT *Wave);
    STDMETHODIMP_(VOID) Cleanup();

    IMP_IAdapterPowerManagement3;

    WDFDEVICE GetWdfDevice() { return m_WdfDevice; }
    PDEVICE_OBJECT GetPdo() { return m_Pdo; }
};

NTSTATUS NewPnAdapterCommon(_Out_ PUNKNOWN *Unknown, _In_ REFCLSID, _In_opt_ PUNKNOWN UnknownOuter, _In_ POOL_FLAGS PoolFlags);

/* ------------------------------------------------------------------------- */
/* WaveRT miniport                                                             */
/* ------------------------------------------------------------------------- */
#define PN_MAX_FORMATS   12

class CPnMiniportWaveRT : public IMiniportWaveRT, public IMiniportAudioSignalProcessing, public CUnknown
{
    friend class CPnMiniportWaveRTStream;

    PPNADAPTERCOMMON            m_Adapter;
    PPORTWAVERT                 m_Port;
    PPORTEVENTS                 m_PortEvents;
    PPNA2DP_DEVICE              m_Device;
    CPnMiniportWaveRTStream    *m_Stream;        /* single render stream */
    KSPIN_LOCK                  m_Lock;
    ULONG                       m_FormatCount;
    KSDATAFORMAT_WAVEFORMATEXTENSIBLE m_Formats[PN_MAX_FORMATS];
    KSDATARANGE_AUDIO           m_DataRange;     /* min/max envelope of m_Formats */
    PKSDATARANGE                m_DataRangePointers[2];
    PCPIN_DESCRIPTOR            m_Pins[2];       /* per-instance copy so each device has its own ranges */
    PCFILTER_DESCRIPTOR         m_Filter;

    NTSTATUS BuildFormatTable();
    NTSTATUS PropertyProposedFormat(_In_ PPCPROPERTY_REQUEST Req);
    NTSTATUS PropertyProposedFormat2(_In_ PPCPROPERTY_REQUEST Req);
    NTSTATUS PropertyModes(_In_ PPCPROPERTY_REQUEST Req);
    NTSTATUS PropertyPinLatency(_In_ PPCPROPERTY_REQUEST Req);
    BOOLEAN  IsFormatSupported(_In_ PKSDATAFORMAT Format);

public:
    DECLARE_STD_UNKNOWN();
    CPnMiniportWaveRT(_In_opt_ PUNKNOWN UnknownOuter);
    ~CPnMiniportWaveRT();

    IMP_IMiniportWaveRT;
    STDMETHODIMP_(NTSTATUS) GetModes(_In_ ULONG Pin, _Out_writes_opt_(*NumSignalProcessingModes) GUID *SignalProcessingModes, _Inout_ ULONG *NumSignalProcessingModes);

    static NTSTATUS PropertyHandlerWaveFilter(_In_ PPCPROPERTY_REQUEST Req);
    static NTSTATUS PropertyHandlerWavePin(_In_ PPCPROPERTY_REQUEST Req);
    static NTSTATUS EventHandlerPinCapsChange(_In_ PPCEVENT_REQUEST Req);

    VOID StreamClosed(_In_ CPnMiniportWaveRTStream *Stream);
    PPNA2DP_DEVICE Device() { return m_Device; }
    PPNADAPTERCOMMON Adapter() { return m_Adapter; }
    VOID GeneratePinCapsChange();
    VOID RebuildFormats();
};

NTSTATUS NewPnMiniportWaveRT(_Out_ PUNKNOWN *Unknown, _In_ PPNADAPTERCOMMON Adapter, _In_ POOL_FLAGS PoolFlags);

/* ------------------------------------------------------------------------- */
/* WaveRT stream                                                               */
/* ------------------------------------------------------------------------- */
typedef struct _PN_NOTIFY_ENTRY {
    LIST_ENTRY  Link;
    PKEVENT     Event;
} PN_NOTIFY_ENTRY;

class CPnMiniportWaveRTStream : public IMiniportWaveRTStreamNotification,
                                public IMiniportWaveRTOutputStream,
                                public CUnknown
{
    friend EXT_CALLBACK PnWaveTimerCallback;

    CPnMiniportWaveRT      *m_Miniport;
    PPORTWAVERTSTREAM       m_PortStream;
    PPNA2DP_DEVICE          m_Device;
    ULONG                   m_Pin;
    WAVEFORMATEXTENSIBLE    m_Format;
    KSSTATE                 m_State;
    KSPIN_LOCK              m_Lock;

    PMDL                    m_Mdl;
    PUCHAR                  m_Buffer;
    ULONG                   m_BufferSize;
    ULONG                   m_Notifications;      /* per buffer */
    ULONG                   m_NotifyIntervalHns;
    PEX_TIMER               m_Timer;
    LIST_ENTRY              m_NotifyList;

    ULONGLONG               m_LinearPosition;     /* bytes consumed */
    ULONGLONG               m_PlayPosition;       /* within buffer */
    LONGLONG                m_StartTimeHns;
    LONGLONG                m_LastTickHns;
    ULONGLONG               m_RateNumerator;      /* bytes/sec */
    ULONGLONG               m_ByteRemainderHns;   /* accumulated fractional */
    LONGLONG                m_NotifyAccumHns;
    LONGLONG                m_PacketCounter;
    ULONG                   m_LastOsWritePacket;
    ULONG                   m_CurrentWritePosition;
    BOOLEAN                 m_Eos;
    BOOLEAN                 m_Registered;
    LARGE_INTEGER           m_QpcFrequency;

    VOID Tick();

public:
    DECLARE_STD_UNKNOWN();
    CPnMiniportWaveRTStream(_In_opt_ PUNKNOWN UnknownOuter);
    ~CPnMiniportWaveRTStream();

    NTSTATUS Init(_In_ CPnMiniportWaveRT *Miniport, _In_ PPORTWAVERTSTREAM PortStream, _In_ ULONG Pin,
                  _In_ PKSDATAFORMAT DataFormat);

    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;
    IMP_IMiniportWaveRTOutputStream;
};

/* ------------------------------------------------------------------------- */
/* Topology miniport                                                           */
/* ------------------------------------------------------------------------- */
class CPnMiniportTopology : public IMiniportTopology, public CUnknown
{
    PPNADAPTERCOMMON    m_Adapter;
    PPORTTOPOLOGY       m_Port;
    PPORTEVENTS         m_PortEvents;

public:
    DECLARE_STD_UNKNOWN();
    CPnMiniportTopology(_In_opt_ PUNKNOWN UnknownOuter);
    ~CPnMiniportTopology();

    IMP_IMiniportTopology;

    static NTSTATUS PropertyHandlerTopoNode(_In_ PPCPROPERTY_REQUEST Req);
    static NTSTATUS PropertyHandlerTopoFilter(_In_ PPCPROPERTY_REQUEST Req);
    NTSTATUS HandleVolume(_In_ PPCPROPERTY_REQUEST Req);
    NTSTATUS HandleMute(_In_ PPCPROPERTY_REQUEST Req);
    NTSTATUS HandleJack(_In_ PPCPROPERTY_REQUEST Req);
    VOID GenerateVolumeEvent();
};

NTSTATUS NewPnMiniportTopology(_Out_ PUNKNOWN *Unknown, _In_ PPNADAPTERCOMMON Adapter, _In_ POOL_FLAGS PoolFlags);

/* Shared KS property helpers */
NTSTATUS PnPropertyBasicSupport(_In_ PPCPROPERTY_REQUEST Req, _In_ ULONG Flags, _In_ DWORD PropTypeSetId);
NTSTATUS PnPropertyBasicSupportVolume(_In_ PPCPROPERTY_REQUEST Req);

extern PCFILTER_DESCRIPTOR g_PnWaveFilterDescriptor;
extern PCFILTER_DESCRIPTOR g_PnTopoFilterDescriptor;
