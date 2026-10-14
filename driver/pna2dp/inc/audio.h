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
#include "wave_math.h"
#include "wave_formats.h"
#include "audio_notify.h"

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

/* Windows Bluetooth service's KS absolute-volume bridge (property/event ID 1).
 * Also implemented by the inbox btha2dp driver. Remote gain becomes active only
 * after the service reports a receiver volume through SET. */
DEFINE_GUID(PN_KSPROPSETID_BluetoothVolume, 0x401e566b, 0xb47e, 0x4e34, 0xb0, 0xfc, 0x94, 0x82, 0x6e, 0x9e, 0x2a, 0x1a);
#define PN_REMOTE_VOLUME_MIN_DB16 (-63 * 65536 - 32768)

/* ------------------------------------------------------------------------- */
/* Adapter common object                                                      */
/* ------------------------------------------------------------------------- */
DEFINE_GUID(IID_IPnAdapterCommon, 0x3a4b9b60, 0x8c4a, 0x4a9f, 0xb0, 0x5b, 0x22, 0x1d, 0x7e, 0x53, 0x9c, 0x11);

DECLARE_INTERFACE_(IPnAdapterCommon, IUnknown)
{
    STDMETHOD_(NTSTATUS, Init)(THIS_ _In_ PDEVICE_OBJECT DeviceObject, _In_ PDRIVER_OBJECT DriverObject) PURE;
    STDMETHOD_(PDEVICE_OBJECT, GetDeviceObject)(THIS) PURE;
    STDMETHOD_(PPNA2DP_DEVICE, GetDevice)(THIS) PURE;
    STDMETHOD_(NTSTATUS, InstallFilters)(THIS_ _In_ PIRP Irp, _In_ PRESOURCELIST Resources) PURE;
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
    /* Each attached miniport holds a reference from the adapter until RemoveFilters, so a
     * pointer taken under m_MiniportLock (ReferenceWave/ReferenceTopology) stays valid
     * while it is used, even if the filters are removed concurrently. */
    KSPIN_LOCK          m_MiniportLock;
    CPnMiniportWaveRT  *m_Wave;
    CPnMiniportTopology *m_Topology;
    LONG                m_Volume[2];
    BOOL                m_Mute[2];
    DEVICE_POWER_STATE  m_PowerState;
    /* Contract C6 (audio_notify.h): device notifications are handled by one PASSIVE work
     * item, which holds a reference on this object while it is queued or running. */
    PIO_WORKITEM        m_NotifyWorkItem;
    PN_AUDIO_NOTIFY     m_Notify;
    volatile LONG       m_JackConnected;    /* KSJACK_DESCRIPTION.IsConnected */
    volatile LONG       m_Detached;         /* RemoveFilters ran: no more notification work */

    NTSTATUS InstallSubdevice(_In_ PIRP Irp, _In_ PRESOURCELIST Resources, _In_ PCWSTR Name, _In_ REFGUID PortClassId,
                              _In_ PUNKNOWN Miniport, _Out_ PUNKNOWN *OutPort);
    NTSTATUS UnregisterSubdevice(_In_opt_ PUNKNOWN Port);
    VOID DetachMiniports();
    VOID RunNotify();
    static IO_WORKITEM_ROUTINE NotifyWorker;

public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CPnAdapterCommon);
    ~CPnAdapterCommon();

    STDMETHODIMP_(NTSTATUS) Init(_In_ PDEVICE_OBJECT DeviceObject, _In_ PDRIVER_OBJECT DriverObject);
    STDMETHODIMP_(PDEVICE_OBJECT) GetDeviceObject();
    STDMETHODIMP_(PPNA2DP_DEVICE) GetDevice();
    STDMETHODIMP_(NTSTATUS) InstallFilters(_In_ PIRP Irp, _In_ PRESOURCELIST Resources);
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
    VOID SetTopologyMiniport(_In_opt_ CPnMiniportTopology *Topology);   /* referenced until RemoveFilters */
    CPnMiniportTopology *ReferenceTopology();   /* NULL or a referenced miniport: Release it */
    CPnMiniportWaveRT *ReferenceWave();         /* NULL or a referenced miniport: Release it */
    VOID SetRemoteVolume(_In_ LONG Db16);
    VOID DisableRemoteVolume();
    VOID RequestNotify(_In_ LONG Bits);                 /* <= DISPATCH, PN_AUDIO_NOTIFY_* */
    VOID SetJackConnected(_In_ BOOLEAN Connected);      /* <= DISPATCH */
    BOOLEAN JackConnected() { return m_JackConnected != 0; }
};

NTSTATUS NewPnAdapterCommon(_Out_ PUNKNOWN *Unknown, _In_ REFCLSID, _In_opt_ PUNKNOWN UnknownOuter, _In_ POOL_FLAGS PoolFlags);

/* ------------------------------------------------------------------------- */
/* WaveRT miniport                                                             */
/* ------------------------------------------------------------------------- */
#define PN_MAX_FORMATS   PN_WAVE_MAX_FORMATS

class CPnMiniportWaveRT : public IMiniportWaveRT, public IMiniportAudioSignalProcessing, public CUnknown
{
    friend class CPnMiniportWaveRTStream;

    PPNADAPTERCOMMON            m_Adapter;
    PPORTWAVERT                 m_Port;
    PPORTEVENTS                 m_PortEvents;
    PPNA2DP_DEVICE              m_Device;
    CPnMiniportWaveRTStream    *m_Stream;        /* single render stream */
    KSPIN_LOCK                  m_Lock;
    /* The format table is rebuilt at PASSIVE (Init, PnAudioFormatsChanged). m_FormatMutex
     * covers the rebuild and every reader of the table (all PASSIVE). */
    FAST_MUTEX                  m_FormatMutex;
    ULONG                       m_FormatCount;
    PN_WAVE_FORMAT              m_FormatList[PN_MAX_FORMATS];
    KSDATAFORMAT_WAVEFORMATEXTENSIBLE m_Formats[PN_MAX_FORMATS];
    PNA2DP_CAPABILITIES         m_CapsScratch;   /* rebuild input, under m_FormatMutex */
    /* PortCls copies the pin descriptor (DataRangesCount included) when the filter is
     * registered but reads the ranges through m_DataRangePointers without our lock. The
     * envelopes are double buffered: a rebuild writes the idle copy, then swaps pointers. */
    KSDATARANGE_AUDIO           m_Ranges[2][2];  /* [copy][PCM, IEEE float] */
    ULONG                       m_RangeCopy;     /* copy the pointers reference */
    PKSDATARANGE                m_DataRangePointers[4];   /* PCM, modes[, float, modes] */
    ULONG                       m_DataRangeCount;         /* 2, or 4 with the float range; fixed at Init */
    PCPIN_DESCRIPTOR            m_Pins[2];       /* per-instance copy so each device has its own ranges */
    PCFILTER_DESCRIPTOR         m_Filter;

    VOID     LoadFormatInput(_Out_ PN_WAVE_FORMAT_INPUT *In);
    BOOLEAN  BuildFormatTable();     /* TRUE if the table changed */
    NTSTATUS PropertyProposedFormat(_In_ PPCPROPERTY_REQUEST Req);
    NTSTATUS PropertyModes(_In_ PPCPROPERTY_REQUEST Req);
    NTSTATUS PropertyPinLatency(_In_ PPCPROPERTY_REQUEST Req);
    BOOLEAN  IsFormatSupported(_In_ PKSDATAFORMAT Format);

public:
    _Success_(return) BOOLEAN MatchFormat(_In_ PKSDATAFORMAT Format, _Out_opt_ WAVEFORMATEXTENSIBLE *Match);

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
    VOID FormatsChanged();           /* PASSIVE: rebuild, and raise FORMATCHANGE if it changed */
};

NTSTATUS NewPnMiniportWaveRT(_Out_ PUNKNOWN *Unknown, _In_ PPNADAPTERCOMMON Adapter, _In_ POOL_FLAGS PoolFlags);

/* ------------------------------------------------------------------------- */
/* WaveRT stream                                                               */
/* ------------------------------------------------------------------------- */
#define PN_WAVE_FADE_SCRATCH   4096    /* one fade: 2 ms at 192 kHz, 8-byte frames = 3072 */

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
    ULONGLONG               m_FrameRemainder;     /* fractional frame, hns*Hz units (PnWaveFramesDue) */
    ULONGLONG               m_LastTickQpc;        /* QPC of the last tick, for presentation interpolation */
    ULONG                   m_TickPeriodHns;
    LONGLONG                m_NotifyAccumHns;
    LONGLONG                m_PacketCounter;
    ULONG                   m_LastOsWritePacket;
    ULONG                   m_CurrentWritePosition;
    PN_WAVE_STARVATION      m_Starvation;         /* packets played as silence (missed engine deadline) */
    ULONG                   m_StarvedReported;
    PN_WAVE_PRESENTATION    m_Presentation;
    BOOLEAN                 m_WritePacketUsed;    /* engine reports written packets this run */
    BOOLEAN                 m_Eos;
    BOOLEAN                 m_Registered;
    BOOLEAN                 m_Holding;            /* the clock holds for a pending stream start (wave_math.h) */
    LARGE_INTEGER           m_QpcFrequency;
    PN_WAVE_LATE_HOLD       m_LateHold;           /* a late tick stopped at a packet the engine was not asked for */
    ULONG                   m_OwedBytes;          /* bytes due but not played while held there */
    /* fades at the edges of substituted silence and on resume (finding #4); under m_Lock */
    ULONG                   m_FadeFrames;         /* frames per fade, 0 = off */
    ULONG                   m_FadeInPos;          /* frames of the fade-in written */
    BOOLEAN                 m_FadeInActive;
    BOOLEAN                 m_LastSegmentSilent;
    UCHAR                   m_FadeScratch[PN_WAVE_FADE_SCRATCH];
    /* the newest m_FadeFrames frames are held back from the pump, so a gap can always fade
     * the audio before it out (2 ms more latency, counted in the presentation position) */
    UCHAR                   m_Tail[PN_WAVE_FADE_SCRATCH];
    ULONG                   m_TailFrames;

    VOID Tick();
    VOID WriteSegment(_In_ ULONG Offset, _In_ ULONG Bytes, _In_ BOOLEAN Silent);
    VOID EmitPcm(_In_reads_bytes_(Bytes) const UCHAR *Data, _In_ ULONG Bytes);
    VOID FlushTail(_In_ BOOLEAN FadeOut);
    VOID PublishBuffer(_In_opt_ PMDL Mdl, _In_opt_ PUCHAR Buffer, _In_ ULONG Size, _In_ ULONG Notifications);

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
    LONG                m_RemoteListeners;
    PCFILTER_DESCRIPTOR m_Filter;
    PCPIN_DESCRIPTOR    m_Pins[2];

public:
    DECLARE_STD_UNKNOWN();
    CPnMiniportTopology(_In_opt_ PUNKNOWN UnknownOuter);
    ~CPnMiniportTopology();

    IMP_IMiniportTopology;

    static NTSTATUS PropertyHandlerTopoNode(_In_ PPCPROPERTY_REQUEST Req);
    static NTSTATUS PropertyHandlerTopoFilter(_In_ PPCPROPERTY_REQUEST Req);
    static NTSTATUS EventHandlerControlChange(_In_ PPCEVENT_REQUEST Req);
    NTSTATUS HandleRemoteVolume(_In_ PPCPROPERTY_REQUEST Req);
    NTSTATUS HandleVolume(_In_ PPCPROPERTY_REQUEST Req);
    NTSTATUS HandleMute(_In_ PPCPROPERTY_REQUEST Req);
    NTSTATUS HandleJack(_In_ PPCPROPERTY_REQUEST Req);
    NTSTATUS HandleBtAudio(_In_ PPCPROPERTY_REQUEST Req);
    static NTSTATUS EventHandlerJack(_In_ PPCEVENT_REQUEST Req);
    VOID GenerateVolumeEvent();
    VOID GenerateRemoteVolumeEvent();
    VOID GenerateJackEvent();
};

NTSTATUS NewPnMiniportTopology(_Out_ PUNKNOWN *Unknown, _In_ PPNADAPTERCOMMON Adapter, _In_ POOL_FLAGS PoolFlags);

/* Shared KS property helpers */
NTSTATUS PnPropertyBasicSupport(_In_ PPCPROPERTY_REQUEST Req, _In_ ULONG Flags, _In_ DWORD PropTypeSetId);
NTSTATUS PnPropertyBasicSupportVolume(_In_ PPCPROPERTY_REQUEST Req);

extern PCFILTER_DESCRIPTOR g_PnWaveFilterDescriptor;
extern PCFILTER_DESCRIPTOR g_PnTopoFilterDescriptor;
