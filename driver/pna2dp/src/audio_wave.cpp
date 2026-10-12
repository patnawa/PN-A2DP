/*
 * audio_wave.cpp - WaveRT render miniport and stream.
 *
 * The miniport advertises PCM formats derived from the negotiated codec (or sensible
 * defaults before the first connection) and fires KSEVENT_PINCAPS_FORMATCHANGE when the
 * list changes. The stream owns the cyclic buffer the audio engine writes into and a
 * 1 ms high-resolution timer that moves the consumed PCM into the pump at the nominal
 * sample rate; notification events fire at packet boundaries (event-driven mode).
 *
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"

/* ---- static descriptor template -------------------------------------------- */

static KSATTRIBUTE g_ModeAttribute = { sizeof(KSATTRIBUTE), 0, STATICGUIDOF(KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE) };
static PKSATTRIBUTE g_ModeAttributes[] = { &g_ModeAttribute };
static KSATTRIBUTE_LIST g_ModeAttributeList = { ARRAYSIZE(g_ModeAttributes), g_ModeAttributes };

static KSDATARANGE g_WaveBridgeRange[] = {
    { sizeof(KSDATARANGE), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE) }
};
static PKSDATARANGE g_WaveBridgeRanges[] = { &g_WaveBridgeRange[0] };

static PCPROPERTY_ITEM g_WaveFilterProps[] = {
    { &KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT, KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, CPnMiniportWaveRT::PropertyHandlerWaveFilter },
    { &KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT2, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, CPnMiniportWaveRT::PropertyHandlerWaveFilter },
};
static PCEVENT_ITEM g_WaveFilterEvents[] = {
    { &KSEVENTSETID_PinCapsChange, KSEVENT_PINCAPS_FORMATCHANGE, KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT, CPnMiniportWaveRT::EventHandlerPinCapsChange },
};
DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(g_WaveFilterAutomation, g_WaveFilterProps, g_WaveFilterEvents);

/* The bridge advertises analog audio. Describe the PCM-to-output conversion so
 * AudioEndpointBuilder can trace a render path through the wave filter. The
 * physical DAC is in the Bluetooth receiver, not on this computer. */
static PCNODE_DESCRIPTOR g_WaveNodes[] = {
    { 0, NULL, &KSNODETYPE_DAC, NULL },
};
static PCCONNECTION_DESCRIPTOR g_WaveConnections[] = {
    { PCFILTER_NODE, PN_WAVE_PIN_SYSTEM, 0, 1 },
    { 0, 0, PCFILTER_NODE, PN_WAVE_PIN_BRIDGE },
};

PCFILTER_DESCRIPTOR g_PnWaveFilterDescriptor = {
    0, &g_WaveFilterAutomation, sizeof(PCPIN_DESCRIPTOR), 2, NULL,
    sizeof(PCNODE_DESCRIPTOR), SIZEOF_ARRAY(g_WaveNodes), g_WaveNodes,
    SIZEOF_ARRAY(g_WaveConnections), g_WaveConnections, 0, NULL
};

/* ---- format helpers --------------------------------------------------------- */

static VOID FillFormat(_Out_ KSDATAFORMAT_WAVEFORMATEXTENSIBLE *F, _In_ ULONG Rate, _In_ ULONG Channels, _In_ ULONG ContainerBits, _In_ ULONG ValidBits)
{
    RtlZeroMemory(F, sizeof(*F));
    F->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    F->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    F->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    F->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    F->WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    F->WaveFormatExt.Format.nChannels = (WORD)Channels;
    F->WaveFormatExt.Format.nSamplesPerSec = Rate;
    F->WaveFormatExt.Format.wBitsPerSample = (WORD)ContainerBits;
    F->WaveFormatExt.Format.nBlockAlign = (WORD)(Channels * ContainerBits / 8);
    F->WaveFormatExt.Format.nAvgBytesPerSec = Rate * F->WaveFormatExt.Format.nBlockAlign;
    F->WaveFormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    F->WaveFormatExt.Samples.wValidBitsPerSample = (WORD)ValidBits;
    F->WaveFormatExt.dwChannelMask = (Channels == 1) ? KSAUDIO_SPEAKER_MONO : KSAUDIO_SPEAKER_STEREO;
    F->WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
}

static PWAVEFORMATEX GetWaveFormatEx(_In_ PKSDATAFORMAT F)
{
    if (!F || F->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX)) return NULL;
    if (!IsEqualGUIDAligned(F->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) || !IsEqualGUIDAligned(F->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) return NULL;
    return (PWAVEFORMATEX)(F + 1);
}

#pragma code_seg("PAGE")
NTSTATUS CPnMiniportWaveRT::BuildFormatTable()
{
    PAGED_CODE();
    static const ULONG rateOrder[] = { 48000, 44100, 96000, 88200, 32000, 16000 };
    ULONG allowed = 0;
    ULONG formatMask = PNA2DP_FMT_S16;
    PPNA2DP_DEVICE d = m_Device;
    BOOLEAN exposeAll = d && (d->Config.Flags & PNA2DP_CFG_EXPOSE_ALL_RATES);

    if (d && d->ActiveValid) {
        /* rates the sink supports for the negotiated codec */
        for (ULONG i = 0; i < d->Caps.SepCount; i++) {
            if (d->Caps.Seps[i].Codec != d->Active.Codec) continue;
            A2DP_CODEC_CAPS caps;
            UCHAR mct = (d->Active.Codec == PNA2DP_CODEC_SBC) ? A2DP_CODEC_SBC : (d->Active.Codec == PNA2DP_CODEC_AAC) ? A2DP_CODEC_MPEG24_AAC : A2DP_CODEC_VENDOR;
            if (A2dpParseCaps(mct, d->Caps.Seps[i].CodecInfo, d->Caps.Seps[i].CodecInfoLength, &caps, NULL, NULL)) {
                allowed |= caps.RateMask;
                formatMask |= caps.FormatMask;
            }
        }
        allowed |= A2dpHzToRateMask(d->Active.SampleRate);
    } else if (d && d->Caps.SepCount) {
        for (ULONG i = 0; i < d->Caps.SepCount; i++) {
            if (!d->Caps.Seps[i].Codec || !(d->Config.EnabledCodecMask & PNA2DP_CODEC_BIT(d->Caps.Seps[i].Codec))) continue;
            A2DP_CODEC_CAPS caps;
            UCHAR mct = (d->Caps.Seps[i].Codec == PNA2DP_CODEC_SBC) ? A2DP_CODEC_SBC : (d->Caps.Seps[i].Codec == PNA2DP_CODEC_AAC) ? A2DP_CODEC_MPEG24_AAC : A2DP_CODEC_VENDOR;
            if (A2dpParseCaps(mct, d->Caps.Seps[i].CodecInfo, d->Caps.Seps[i].CodecInfoLength, &caps, NULL, NULL)) {
                allowed |= caps.RateMask;
                formatMask |= caps.FormatMask;
            }
        }
    }
    if (!allowed) allowed = PNA2DP_RATE_48000 | PNA2DP_RATE_44100;
    if (!exposeAll) {
        ULONG keep = PNA2DP_RATE_48000 | PNA2DP_RATE_44100;
        if (d && d->ActiveValid) keep |= A2dpHzToRateMask(d->Active.SampleRate);
        allowed &= keep;
        if (!allowed) allowed = PNA2DP_RATE_48000 | PNA2DP_RATE_44100;
    }
    BOOLEAN hiRes = (formatMask & (PNA2DP_FMT_S24 | PNA2DP_FMT_S32)) != 0 && d && d->ActiveValid && d->Active.Codec != PNA2DP_CODEC_SBC && d->Active.Codec != PNA2DP_CODEC_AAC;

    /* Runs from Init only, before the filter is registered: nothing else reads the table yet
     * (and a spin lock must not be held in pageable code). */
    m_FormatCount = 0;
    /* put the negotiated rate first so Windows 11's single-format endpoint picks it */
    ULONG firstRate = (d && d->ActiveValid) ? d->Active.SampleRate : 48000;
    for (ULONG pass = 0; pass < 2 && m_FormatCount < PN_MAX_FORMATS; pass++) {
        for (ULONG i = 0; i < PN_ARRAYSIZE(rateOrder) && m_FormatCount < PN_MAX_FORMATS; i++) {
            ULONG r = rateOrder[i];
            if (!(allowed & A2dpHzToRateMask(r))) continue;
            if ((pass == 0) != (r == firstRate)) continue;
            if (hiRes && m_FormatCount < PN_MAX_FORMATS) FillFormat(&m_Formats[m_FormatCount++], r, 2, 32, 24);
            if (m_FormatCount < PN_MAX_FORMATS) FillFormat(&m_Formats[m_FormatCount++], r, 2, 16, 16);
        }
    }
    /* envelope */
    ULONG minRate = 0xFFFFFFFF, maxRate = 0, minBits = 32, maxBits = 16;
    for (ULONG i = 0; i < m_FormatCount; i++) {
        ULONG r = m_Formats[i].WaveFormatExt.Format.nSamplesPerSec, b = m_Formats[i].WaveFormatExt.Format.wBitsPerSample;
        minRate = PN_MIN(minRate, r); maxRate = PN_MAX(maxRate, r); minBits = PN_MIN(minBits, b); maxBits = PN_MAX(maxBits, b);
    }
    RtlZeroMemory(&m_DataRange, sizeof(m_DataRange));
    m_DataRange.DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
    m_DataRange.DataRange.Flags = KSDATARANGE_ATTRIBUTES;
    m_DataRange.DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    m_DataRange.DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    m_DataRange.DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    m_DataRange.MaximumChannels = 2;
    m_DataRange.MinimumBitsPerSample = minBits;
    m_DataRange.MaximumBitsPerSample = maxBits;
    m_DataRange.MinimumSampleFrequency = minRate;
    m_DataRange.MaximumSampleFrequency = maxRate;
    m_DataRangePointers[0] = (PKSDATARANGE)&m_DataRange;
    m_DataRangePointers[1] = (PKSDATARANGE)&g_ModeAttributeList;
    PN_INFO("wave formats: %u entries, %u..%u Hz, %u..%u bit", m_FormatCount, minRate, maxRate, minBits, maxBits);
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
VOID CPnMiniportWaveRT::GeneratePinCapsChange()
{
    PAGED_CODE();
    if (m_PortEvents) m_PortEvents->GenerateEventList((GUID *)&KSEVENTSETID_PinCapsChange, KSEVENT_PINCAPS_FORMATCHANGE, TRUE, PN_WAVE_PIN_SYSTEM, FALSE, ULONG(-1));
}

/* Returns the table entry that matches Format, or NULL. Every field that is later used for
 * arithmetic (block align, byte rate) must match the table, and an extensible format must
 * really be as large as it claims. */
#pragma code_seg("PAGE")
const KSDATAFORMAT_WAVEFORMATEXTENSIBLE *CPnMiniportWaveRT::MatchFormat(_In_ PKSDATAFORMAT Format)
{
    PAGED_CODE();
    PWAVEFORMATEX w = GetWaveFormatEx(Format);
    if (!w) return NULL;
    if (!IsEqualGUIDAligned(Format->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && !IsEqualGUIDAligned(Format->SubFormat, KSDATAFORMAT_SUBTYPE_WAVEFORMATEX)) return NULL;
    ULONG valid = w->wBitsPerSample;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (Format->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE) || w->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return NULL;
        PWAVEFORMATEXTENSIBLE x = (PWAVEFORMATEXTENSIBLE)w;
        if (!IsEqualGUIDAligned(x->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) return NULL;
        valid = x->Samples.wValidBitsPerSample;
    } else if (w->wFormatTag != WAVE_FORMAT_PCM) {
        return NULL;
    }
    for (ULONG i = 0; i < m_FormatCount; i++) {
        const WAVEFORMATEXTENSIBLE *f = &m_Formats[i].WaveFormatExt;
        if (f->Format.nSamplesPerSec == w->nSamplesPerSec && f->Format.nChannels == w->nChannels && f->Format.wBitsPerSample == w->wBitsPerSample &&
            f->Format.nBlockAlign == w->nBlockAlign && f->Format.nAvgBytesPerSec == w->nAvgBytesPerSec &&
            f->Samples.wValidBitsPerSample == valid) return &m_Formats[i];
    }
    return NULL;
}

#pragma code_seg("PAGE")
BOOLEAN CPnMiniportWaveRT::IsFormatSupported(_In_ PKSDATAFORMAT Format)
{
    PAGED_CODE();
    return MatchFormat(Format) != NULL;
}

/* ---- miniport ---------------------------------------------------------------- */

#pragma code_seg("PAGE")
NTSTATUS NewPnMiniportWaveRT(_Out_ PUNKNOWN *Unknown, _In_ PPNADAPTERCOMMON Adapter, _In_ POOL_FLAGS PoolFlags)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Adapter);
    CPnMiniportWaveRT *p = new (PoolFlags, PNA2DP_TAG_AUDIO) CPnMiniportWaveRT(NULL);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;
    *Unknown = PUNKNOWN((PMINIPORTWAVERT)p);
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
CPnMiniportWaveRT::CPnMiniportWaveRT(_In_opt_ PUNKNOWN UnknownOuter)
    : CUnknown(UnknownOuter), m_Adapter(NULL), m_Port(NULL), m_PortEvents(NULL), m_Device(NULL), m_Stream(NULL), m_FormatCount(0)
{
    PAGED_CODE();
    KeInitializeSpinLock(&m_Lock);
    RtlZeroMemory(m_Formats, sizeof(m_Formats));
    RtlZeroMemory(&m_DataRange, sizeof(m_DataRange));
    RtlZeroMemory(m_Pins, sizeof(m_Pins));
    RtlZeroMemory(&m_Filter, sizeof(m_Filter));
}

#pragma code_seg("PAGE")
CPnMiniportWaveRT::~CPnMiniportWaveRT()
{
    PAGED_CODE();
    if (m_Adapter) { m_Adapter->SetWaveMiniport(NULL); m_Adapter->Release(); m_Adapter = NULL; }
    if (m_PortEvents) { m_PortEvents->Release(); m_PortEvents = NULL; }
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRT::NonDelegatingQueryInterface(_In_ REFIID Interface, _COM_Outptr_ PVOID *Object)
{
    PAGED_CODE();
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) *Object = PVOID(PUNKNOWN(PMINIPORTWAVERT(this)));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport)) *Object = PVOID(PMINIPORT(this));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRT)) *Object = PVOID(PMINIPORTWAVERT(this));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportAudioSignalProcessing)) *Object = PVOID(PMINIPORTAudioSignalProcessing(this));
    else *Object = NULL;
    if (*Object) { PUNKNOWN(*Object)->AddRef(); return STATUS_SUCCESS; }
    return STATUS_INVALID_PARAMETER;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRT::Init(_In_ PUNKNOWN UnknownAdapter, _In_ PRESOURCELIST ResourceList, _In_ PPORTWAVERT Port)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourceList);
    NTSTATUS status = UnknownAdapter->QueryInterface(IID_IPnAdapterCommon, (PVOID *)&m_Adapter);
    if (!NT_SUCCESS(status)) return status;
    m_Port = Port;
    m_Device = m_Adapter->GetDevice();
    Port->QueryInterface(IID_IPortEvents, (PVOID *)&m_PortEvents);
    m_Adapter->SetWaveMiniport(this);

    BuildFormatTable();

    /* per-instance filter descriptor */
    m_Filter = g_PnWaveFilterDescriptor;
    RtlZeroMemory(m_Pins, sizeof(m_Pins));
    m_Pins[PN_WAVE_PIN_SYSTEM].MaxGlobalInstanceCount = 1;
    m_Pins[PN_WAVE_PIN_SYSTEM].MaxFilterInstanceCount = 1;
    m_Pins[PN_WAVE_PIN_SYSTEM].MinFilterInstanceCount = 0;
    m_Pins[PN_WAVE_PIN_SYSTEM].AutomationTable = NULL;
    m_Pins[PN_WAVE_PIN_SYSTEM].KsPinDescriptor.DataRangesCount = 2;
    m_Pins[PN_WAVE_PIN_SYSTEM].KsPinDescriptor.DataRanges = m_DataRangePointers;
    m_Pins[PN_WAVE_PIN_SYSTEM].KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_IN;
    m_Pins[PN_WAVE_PIN_SYSTEM].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_SINK;
    m_Pins[PN_WAVE_PIN_SYSTEM].KsPinDescriptor.Category = &KSCATEGORY_AUDIO;
    m_Pins[PN_WAVE_PIN_BRIDGE].KsPinDescriptor.DataRangesCount = SIZEOF_ARRAY(g_WaveBridgeRanges);
    m_Pins[PN_WAVE_PIN_BRIDGE].KsPinDescriptor.DataRanges = g_WaveBridgeRanges;
    m_Pins[PN_WAVE_PIN_BRIDGE].KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_OUT;
    m_Pins[PN_WAVE_PIN_BRIDGE].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
    m_Pins[PN_WAVE_PIN_BRIDGE].KsPinDescriptor.Category = &KSCATEGORY_AUDIO;
    m_Filter.Pins = m_Pins;
    m_Filter.PinCount = 2;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRT::GetDescription(_Out_ PPCFILTER_DESCRIPTOR *Description)
{
    PAGED_CODE();
    *Description = &m_Filter;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRT::GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION DeviceDescription)
{
    PAGED_CODE();
    RtlZeroMemory(DeviceDescription, sizeof(*DeviceDescription));
    DeviceDescription->Master = TRUE;
    DeviceDescription->ScatterGather = TRUE;
    DeviceDescription->Dma32BitAddresses = TRUE;
    DeviceDescription->InterfaceType = PCIBus;
    DeviceDescription->MaximumLength = 0xFFFFFFFF;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRT::GetModes(_In_ ULONG Pin, _Out_writes_opt_(*NumSignalProcessingModes) GUID *SignalProcessingModes, _Inout_ ULONG *NumSignalProcessingModes)
{
    PAGED_CODE();
    PN_VERB("GetModes pin %u capacity %u", Pin, *NumSignalProcessingModes);
    if (Pin != PN_WAVE_PIN_SYSTEM) { *NumSignalProcessingModes = 0; return STATUS_NOT_SUPPORTED; }
    if (SignalProcessingModes == NULL || *NumSignalProcessingModes < 1) { *NumSignalProcessingModes = 1; return SignalProcessingModes ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS; }
    SignalProcessingModes[0] = AUDIO_SIGNALPROCESSINGMODE_DEFAULT;
    *NumSignalProcessingModes = 1;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRT::DataRangeIntersection(_In_ ULONG PinId, _In_ PKSDATARANGE DataRange, _In_ PKSDATARANGE MatchingDataRange,
                                                                 _In_ ULONG OutputBufferLength, _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
                                                                 _Out_ PULONG ResultantFormatLength)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(MatchingDataRange);
    PN_VERB("intersection pin %u size %u output %u sub %08x", PinId, DataRange->FormatSize, OutputBufferLength, DataRange->SubFormat.Data1);
    *ResultantFormatLength = 0;
    if (PinId != PN_WAVE_PIN_SYSTEM) return STATUS_INVALID_PARAMETER;
    if (!IsEqualGUIDAligned(DataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) && !IsEqualGUIDAligned(DataRange->MajorFormat, KSDATAFORMAT_TYPE_WILDCARD)) return STATUS_NO_MATCH;
    if (!IsEqualGUIDAligned(DataRange->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && !IsEqualGUIDAligned(DataRange->SubFormat, KSDATAFORMAT_SUBTYPE_WILDCARD)) return STATUS_NO_MATCH;
    if (!IsEqualGUIDAligned(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) && !IsEqualGUIDAligned(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WILDCARD)) return STATUS_NO_MATCH;

    const KSDATARANGE_AUDIO *cr = (DataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO)) ? (const KSDATARANGE_AUDIO *)DataRange : NULL;
    const KSDATAFORMAT_WAVEFORMATEXTENSIBLE *pick = NULL;
    for (ULONG i = 0; i < m_FormatCount; i++) {
        const WAVEFORMATEXTENSIBLE *f = &m_Formats[i].WaveFormatExt;
        if (cr) {
            if (f->Format.nSamplesPerSec < cr->MinimumSampleFrequency || f->Format.nSamplesPerSec > cr->MaximumSampleFrequency) continue;
            if (f->Format.wBitsPerSample < cr->MinimumBitsPerSample || f->Format.wBitsPerSample > cr->MaximumBitsPerSample) continue;
            if (f->Format.nChannels > cr->MaximumChannels) continue;
        }
        pick = &m_Formats[i];
        break;
    }
    if (!pick) return STATUS_NO_MATCH;

    if (OutputBufferLength == 0) { *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE); return STATUS_BUFFER_OVERFLOW; }
    if (!ResultantFormat) return STATUS_INVALID_PARAMETER;
    if (OutputBufferLength == sizeof(KSDATAFORMAT)) {
        RtlCopyMemory(ResultantFormat, &pick->DataFormat, sizeof(KSDATAFORMAT));
        ((PKSDATAFORMAT)ResultantFormat)->FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
        *ResultantFormatLength = sizeof(KSDATAFORMAT);
        return STATUS_SUCCESS;
    }
    if (OutputBufferLength < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(ResultantFormat, pick, sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE));
    *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRT::NewStream(_Out_ PMINIPORTWAVERTSTREAM *Stream, _In_ PPORTWAVERTSTREAM PortStream, _In_ ULONG Pin, _In_ BOOLEAN Capture, _In_ PKSDATAFORMAT DataFormat)
{
    PAGED_CODE();
    PN_VERB("NewStream pin %u capture %u size %u", Pin, Capture, DataFormat ? DataFormat->FormatSize : 0);
    *Stream = NULL;
    if (Pin != PN_WAVE_PIN_SYSTEM || Capture) return STATUS_INVALID_PARAMETER;
    if (!IsFormatSupported(DataFormat)) {
        PWAVEFORMATEX w = GetWaveFormatEx(DataFormat);
        PN_WARN("NewStream: unsupported format %u Hz %u ch %u bit", w ? w->nSamplesPerSec : 0, w ? w->nChannels : 0, w ? w->wBitsPerSample : 0);
        return STATUS_INVALID_PARAMETER;
    }
    if (m_Stream) return STATUS_INSUFFICIENT_RESOURCES;   /* one render stream */
    CPnMiniportWaveRTStream *s = new (POOL_FLAG_NON_PAGED, PNA2DP_TAG_AUDIO) CPnMiniportWaveRTStream(NULL);
    if (!s) return STATUS_INSUFFICIENT_RESOURCES;
    s->AddRef();
    NTSTATUS status = s->Init(this, PortStream, Pin, DataFormat);
    PN_VERB("stream Init returned 0x%08x", status);
    if (!NT_SUCCESS(status)) { s->Release(); return status; }
    m_Stream = s;
    *Stream = PMINIPORTWAVERTSTREAM(s);
    (*Stream)->AddRef();
    s->Release();
    return STATUS_SUCCESS;
}

#pragma code_seg()
DECLSPEC_NOINLINE VOID CPnMiniportWaveRT::StreamClosed(_In_ CPnMiniportWaveRTStream *Stream)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    if (m_Stream == Stream) m_Stream = NULL;
    KeReleaseSpinLock(&m_Lock, irql);
}

/* ---- filter properties ------------------------------------------------------ */

#pragma code_seg("PAGE")
NTSTATUS CPnMiniportWaveRT::PropertyProposedFormat(_In_ PPCPROPERTY_REQUEST Req)
{
    PAGED_CODE();
    if (Req->InstanceSize < sizeof(KSP_PIN) - RTL_SIZEOF_THROUGH_FIELD(KSP_PIN, Property)) return STATUS_INVALID_PARAMETER;
    PKSP_PIN kspPin = CONTAINING_RECORD(Req->Instance, KSP_PIN, PinId);
    if (kspPin->PinId == PN_WAVE_PIN_BRIDGE) return STATUS_NOT_SUPPORTED;
    if (kspPin->PinId != PN_WAVE_PIN_SYSTEM) return STATUS_INVALID_PARAMETER;
    if (Req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) return PnPropertyBasicSupport(Req, KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, VT_ILLEGAL);
    if (Req->Verb & KSPROPERTY_TYPE_SET) {
        if (Req->ValueSize >= sizeof(KSDATAFORMAT)) {
            PKSDATAFORMAT df = (PKSDATAFORMAT)Req->Value;
            PN_VERB("propose pin %u size %u major %08x sub %08x spec %08x flags %x", kspPin->PinId, df->FormatSize, df->MajorFormat.Data1, df->SubFormat.Data1, df->Specifier.Data1, df->Flags);
        }
        if (Req->ValueSize == 0) { Req->ValueSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE); return STATUS_BUFFER_OVERFLOW; }
        if (Req->ValueSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX)) return STATUS_BUFFER_TOO_SMALL;
        if (Req->ValueSize < ((PKSDATAFORMAT)Req->Value)->FormatSize) return STATUS_BUFFER_TOO_SMALL;
        return IsFormatSupported((PKSDATAFORMAT)Req->Value) ? STATUS_SUCCESS : STATUS_NO_MATCH;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

#pragma code_seg("PAGE")
NTSTATUS CPnMiniportWaveRT::PropertyProposedFormat2(_In_ PPCPROPERTY_REQUEST Req)
{
    PAGED_CODE();
    if (Req->InstanceSize < sizeof(KSP_PIN) - RTL_SIZEOF_THROUGH_FIELD(KSP_PIN, Property)) return STATUS_INVALID_PARAMETER;
    PKSP_PIN kspPin = CONTAINING_RECORD(Req->Instance, KSP_PIN, PinId);
    if (kspPin->PinId != PN_WAVE_PIN_SYSTEM || m_FormatCount == 0) return STATUS_NOT_SUPPORTED;
    if (Req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) return PnPropertyBasicSupport(Req, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, VT_ILLEGAL);
    /* attributes (mode) follow the KSP_PIN; we have one mode so the default format is the same for any mode */
    PKSMULTIPLE_ITEM items = (PKSMULTIPLE_ITEM)(kspPin + 1);
    ULONG cbItems = (ULONG)((((PUCHAR)Req->Instance) + Req->InstanceSize) - (PUCHAR)items);
    if (cbItems < sizeof(KSMULTIPLE_ITEM) || items->Size > cbItems) cbItems = 0;
    else cbItems = items->Size;
    ULONG cbFormat = (sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) + 7) & ~7u;
    ULONG need = cbFormat + cbItems;
    if (Req->ValueSize == 0) { Req->ValueSize = need; return STATUS_BUFFER_OVERFLOW; }
    if (Req->ValueSize < need) return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(Req->Value, need);
    RtlCopyMemory(Req->Value, &m_Formats[0], sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE));
    PKSDATAFORMAT df = (PKSDATAFORMAT)Req->Value;
    if (cbItems) {
        df->Flags = KSDATAFORMAT_ATTRIBUTES;
        RtlCopyMemory((PUCHAR)Req->Value + cbFormat, items, cbItems);
    }
    Req->ValueSize = need;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
NTSTATUS CPnMiniportWaveRT::PropertyHandlerWaveFilter(_In_ PPCPROPERTY_REQUEST Req)
{
    PAGED_CODE();
    PN_VERB("wave property %u verb %08x instance %u value %u", Req->PropertyItem->Id, Req->Verb, Req->InstanceSize, Req->ValueSize);
    CPnMiniportWaveRT *w = reinterpret_cast<CPnMiniportWaveRT *>(Req->MajorTarget);
    if (!w) return STATUS_INVALID_PARAMETER;
    if (IsEqualGUIDAligned(*Req->PropertyItem->Set, KSPROPSETID_Pin)) {
        if (Req->PropertyItem->Id == KSPROPERTY_PIN_PROPOSEDATAFORMAT) return w->PropertyProposedFormat(Req);
        if (Req->PropertyItem->Id == KSPROPERTY_PIN_PROPOSEDATAFORMAT2) return w->PropertyProposedFormat2(Req);
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

#pragma code_seg("PAGE")
NTSTATUS CPnMiniportWaveRT::PropertyHandlerWavePin(_In_ PPCPROPERTY_REQUEST Req)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Req);
    return STATUS_INVALID_DEVICE_REQUEST;
}

#pragma code_seg("PAGE")
NTSTATUS CPnMiniportWaveRT::EventHandlerPinCapsChange(_In_ PPCEVENT_REQUEST Req)
{
    PAGED_CODE();
    CPnMiniportWaveRT *w = reinterpret_cast<CPnMiniportWaveRT *>(Req->MajorTarget);
    if (!w || !IsEqualGUIDAligned(*Req->EventItem->Set, KSEVENTSETID_PinCapsChange)) return STATUS_INVALID_PARAMETER;
    switch (Req->Verb) {
    case PCEVENT_VERB_SUPPORT: return STATUS_SUCCESS;
    case PCEVENT_VERB_ADD:
        if (!Req->EventEntry) return STATUS_UNSUCCESSFUL;
        if (w->m_PortEvents) w->m_PortEvents->AddEventToEventList(Req->EventEntry);
        return STATUS_SUCCESS;
    case PCEVENT_VERB_REMOVE: return STATUS_SUCCESS;
    default: return STATUS_INVALID_PARAMETER;
    }
}

/* ---- stream ------------------------------------------------------------------ */

#pragma code_seg("PAGE")
CPnMiniportWaveRTStream::CPnMiniportWaveRTStream(_In_opt_ PUNKNOWN UnknownOuter)
    : CUnknown(UnknownOuter), m_Miniport(NULL), m_PortStream(NULL), m_Device(NULL), m_Pin(0), m_State(KSSTATE_STOP), m_Mdl(NULL), m_Buffer(NULL),
      m_BufferSize(0), m_Notifications(0), m_NotifyIntervalHns(0), m_Timer(NULL), m_LinearPosition(0), m_PlayPosition(0), m_StartTimeHns(0),
      m_LastTickHns(0), m_RateNumerator(0), m_ByteRemainderHns(0), m_NotifyAccumHns(0), m_PacketCounter(0), m_LastOsWritePacket(ULONG_MAX),
      m_CurrentWritePosition(0), m_Eos(FALSE), m_Registered(FALSE)
{
    PAGED_CODE();
    KeInitializeSpinLock(&m_Lock);
    InitializeListHead(&m_NotifyList);
    RtlZeroMemory(&m_Format, sizeof(m_Format));
    m_QpcFrequency.QuadPart = 0;
}

#pragma code_seg("PAGE")
CPnMiniportWaveRTStream::~CPnMiniportWaveRTStream()
{
    PAGED_CODE();
    if (m_Timer) { ExDeleteTimer(m_Timer, TRUE, TRUE, NULL); m_Timer = NULL; }
    KeFlushQueuedDpcs();
    while (!IsListEmpty(&m_NotifyList)) {
        PN_NOTIFY_ENTRY *e = CONTAINING_RECORD(RemoveHeadList(&m_NotifyList), PN_NOTIFY_ENTRY, Link);
        PnFree(e, PNA2DP_TAG_AUDIO);
    }
    if (m_Device && m_Registered) DeviceAudioStreamClosed(m_Device);
    if (m_Miniport) { m_Miniport->StreamClosed(this); m_Miniport->Release(); m_Miniport = NULL; }
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::NonDelegatingQueryInterface(_In_ REFIID Interface, _COM_Outptr_ PVOID *Object)
{
    PAGED_CODE();
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) *Object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream)) *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStreamNotification)) *Object = PVOID(PMINIPORTWAVERTSTREAMNOTIFICATION(this));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTOutputStream)) *Object = PVOID(PMINIPORTWAVERTOUTPUTSTREAM(this));
    else *Object = NULL;
    if (*Object) { PUNKNOWN(*Object)->AddRef(); return STATUS_SUCCESS; }
    return STATUS_INVALID_PARAMETER;
}

#pragma code_seg()
EXT_CALLBACK PnWaveTimerCallback;
VOID PnWaveTimerCallback(_In_ PEX_TIMER Timer, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Timer);
    CPnMiniportWaveRTStream *s = (CPnMiniportWaveRTStream *)Context;
    if (s) s->Tick();
}

#pragma code_seg("PAGE")
NTSTATUS CPnMiniportWaveRTStream::Init(_In_ CPnMiniportWaveRT *Miniport, _In_ PPORTWAVERTSTREAM PortStream, _In_ ULONG Pin, _In_ PKSDATAFORMAT DataFormat)
{
    PAGED_CODE();
    const KSDATAFORMAT_WAVEFORMATEXTENSIBLE *match = Miniport->MatchFormat(DataFormat);
    if (!match) return STATUS_INVALID_PARAMETER;
    m_Miniport = Miniport;
    m_Miniport->AddRef();
    m_PortStream = PortStream;
    m_Pin = Pin;
    m_Device = Miniport->Device();
    /* our own table entry: block align and byte rate are known to be consistent and non-zero */
    m_Format = match->WaveFormatExt;
    m_RateNumerator = m_Format.Format.nAvgBytesPerSec;
    KeQueryPerformanceCounter(&m_QpcFrequency);
    m_Timer = ExAllocateTimer(PnWaveTimerCallback, this, EX_TIMER_HIGH_RESOLUTION);
    if (!m_Timer) return STATUS_INSUFFICIENT_RESOURCES;
    BOOLEAN isFloat = IsEqualGUIDAligned(m_Format.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? TRUE : FALSE;
    NTSTATUS status = DeviceAudioStreamOpened(m_Device, m_Format.Format.nSamplesPerSec, m_Format.Format.nChannels, m_Format.Format.wBitsPerSample,
                            m_Format.Samples.wValidBitsPerSample ? m_Format.Samples.wValidBitsPerSample : m_Format.Format.wBitsPerSample, isFloat);
    if (!NT_SUCCESS(status)) return status;
    m_Registered = TRUE;
    PN_VERB("wave stream opened: %u Hz %u ch %u/%u bit", m_Format.Format.nSamplesPerSec, m_Format.Format.nChannels,
            m_Format.Samples.wValidBitsPerSample, m_Format.Format.wBitsPerSample);
    return STATUS_SUCCESS;
}

/* -- buffers -- */

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::AllocateBufferWithNotification(_In_ ULONG NotificationCount, _In_ ULONG RequestedSize, _Out_ PMDL *AudioBufferMdl,
                                                                                _Out_ ULONG *ActualSize, _Out_ ULONG *OffsetFromFirstPage, _Out_ MEMORY_CACHING_TYPE *CacheType)
{
    PAGED_CODE();
    if (!m_Format.Format.nBlockAlign || !m_RateNumerator) return STATUS_INVALID_DEVICE_STATE;
    if (m_Mdl) return STATUS_INVALID_DEVICE_STATE;   /* one buffer per stream */
    if (RequestedSize == 0 || RequestedSize < m_Format.Format.nBlockAlign) return STATUS_UNSUCCESSFUL;
    if (NotificationCount == 0 || (RequestedSize % NotificationCount) != 0) return STATUS_INVALID_PARAMETER;
    RequestedSize -= RequestedSize % m_Format.Format.nBlockAlign;
    if (RequestedSize / NotificationCount == 0) return STATUS_INVALID_PARAMETER;   /* Tick divides by the packet size */
    PHYSICAL_ADDRESS high; high.QuadPart = MAXULONG;
    PMDL mdl = m_PortStream->AllocatePagesForMdl(high, RequestedSize);
    if (!mdl) return STATUS_UNSUCCESSFUL;
    PUCHAR buffer = (PUCHAR)m_PortStream->MapAllocatedPages(mdl, MmCached);
    if (!buffer) { m_PortStream->FreePagesFromMdl(mdl); return STATUS_INSUFFICIENT_RESOURCES; }
    PublishBuffer(mdl, buffer, RequestedSize, NotificationCount);
    ULONGLONG bufferHns = (ULONGLONG)RequestedSize * PN_HNS_PER_SEC / m_RateNumerator;
    m_NotifyIntervalHns = (ULONG)(bufferHns / NotificationCount);
    *AudioBufferMdl = mdl;
    *ActualSize = RequestedSize;
    *OffsetFromFirstPage = 0;
    *CacheType = MmCached;
    PN_VERB("wave buffer %u bytes, %u notifications (%u.%u ms each)", RequestedSize, NotificationCount, m_NotifyIntervalHns / 10000, (m_NotifyIntervalHns / 1000) % 10);
    return STATUS_SUCCESS;
}

/* The timer callback reads the buffer fields under m_Lock, so they are published and
 * withdrawn under it too; a tick that is already copying finishes before the unmap. */
#pragma code_seg()
DECLSPEC_NOINLINE VOID CPnMiniportWaveRTStream::PublishBuffer(_In_opt_ PMDL Mdl, _In_opt_ PUCHAR Buffer, _In_ ULONG Size, _In_ ULONG Notifications)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    m_Mdl = Mdl;
    m_Buffer = Buffer;
    m_BufferSize = Size;
    m_Notifications = Notifications;
    m_PlayPosition = 0;
    KeReleaseSpinLock(&m_Lock, irql);
}

#pragma code_seg("PAGE")
STDMETHODIMP_(VOID) CPnMiniportWaveRTStream::FreeBufferWithNotification(_In_ PMDL Mdl, _In_ ULONG Size)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Size);
    PUCHAR buffer = m_Buffer;
    PublishBuffer(NULL, NULL, 0, 0);
    if (Mdl) {
        if (buffer) m_PortStream->UnmapAllocatedPages(buffer, Mdl);
        m_PortStream->FreePagesFromMdl(Mdl);
    }
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::AllocateAudioBuffer(_In_ ULONG RequestedSize, _Out_ PMDL *AudioBufferMdl, _Out_ ULONG *ActualSize,
                                                                     _Out_ ULONG *OffsetFromFirstPage, _Out_ MEMORY_CACHING_TYPE *CacheType)
{
    PAGED_CODE();
    NTSTATUS status = AllocateBufferWithNotification(1, RequestedSize - (RequestedSize % PN_MAX(m_Format.Format.nBlockAlign, 1u)), AudioBufferMdl, ActualSize, OffsetFromFirstPage, CacheType);
    if (NT_SUCCESS(status)) PublishBuffer(m_Mdl, m_Buffer, m_BufferSize, 0);   /* no event-driven mode */
    return status;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(VOID) CPnMiniportWaveRTStream::FreeAudioBuffer(_In_opt_ PMDL Mdl, _In_ ULONG Size)
{
    PAGED_CODE();
    FreeBufferWithNotification(Mdl, Size);
}

#pragma code_seg()
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::RegisterNotificationEvent(_In_ PKEVENT NotificationEvent)
{
    PN_NOTIFY_ENTRY *e = (PN_NOTIFY_ENTRY *)PnAllocNp(sizeof(*e), PNA2DP_TAG_AUDIO);
    if (!e) return STATUS_INSUFFICIENT_RESOURCES;
    e->Event = NotificationEvent;
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    for (PLIST_ENTRY l = m_NotifyList.Flink; l != &m_NotifyList; l = l->Flink) {
        if (CONTAINING_RECORD(l, PN_NOTIFY_ENTRY, Link)->Event == NotificationEvent) { KeReleaseSpinLock(&m_Lock, irql); PnFree(e, PNA2DP_TAG_AUDIO); return STATUS_UNSUCCESSFUL; }
    }
    InsertTailList(&m_NotifyList, &e->Link);
    KeReleaseSpinLock(&m_Lock, irql);
    return STATUS_SUCCESS;
}

#pragma code_seg()
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::UnregisterNotificationEvent(_In_ PKEVENT NotificationEvent)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    for (PLIST_ENTRY l = m_NotifyList.Flink; l != &m_NotifyList; l = l->Flink) {
        PN_NOTIFY_ENTRY *e = CONTAINING_RECORD(l, PN_NOTIFY_ENTRY, Link);
        if (e->Event == NotificationEvent) { RemoveEntryList(l); KeReleaseSpinLock(&m_Lock, irql); PnFree(e, PNA2DP_TAG_AUDIO); return STATUS_SUCCESS; }
    }
    KeReleaseSpinLock(&m_Lock, irql);
    return STATUS_NOT_FOUND;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::GetClockRegister(_Out_ KSRTAUDIO_HWREGISTER *Register) { PAGED_CODE(); UNREFERENCED_PARAMETER(Register); return STATUS_NOT_IMPLEMENTED; }
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::GetPositionRegister(_Out_ KSRTAUDIO_HWREGISTER *Register) { PAGED_CODE(); UNREFERENCED_PARAMETER(Register); return STATUS_NOT_IMPLEMENTED; }

#pragma code_seg("PAGE")
STDMETHODIMP_(VOID) CPnMiniportWaveRTStream::GetHWLatency(_Out_ KSRTAUDIO_HWLATENCY *hwLatency)
{
    PAGED_CODE();
    /* This software WaveRT endpoint has no DMA FIFO or chipset latency. Windows
     * requires their combined lead to fit inside its cyclic buffer. Reporting
     * the Bluetooth queue here made every 20 ms format probe fail. Downstream
     * transport latency is tracked by the pump and presentation position. */
    RtlZeroMemory(hwLatency, sizeof(*hwLatency));
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::SetFormat(_In_ PKSDATAFORMAT DataFormat) { PAGED_CODE(); UNREFERENCED_PARAMETER(DataFormat); return STATUS_NOT_SUPPORTED; }

/* -- state / position -- */

#pragma code_seg()
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::SetState(_In_ KSSTATE State)
{
    KIRQL irql;
    switch (State) {
    case KSSTATE_STOP:
        KeAcquireSpinLock(&m_Lock, &irql);
        m_State = State;   /* a tick that is already queued sees this and does nothing */
        m_LinearPosition = 0; m_PlayPosition = 0; m_PacketCounter = 0; m_LastOsWritePacket = ULONG_MAX; m_CurrentWritePosition = 0; m_Eos = FALSE;
        m_ByteRemainderHns = 0; m_NotifyAccumHns = 0;
        KeReleaseSpinLock(&m_Lock, irql);
        if (m_Timer) ExCancelTimer(m_Timer, NULL);
        if (m_Device) DeviceAudioStateChanged(m_Device, PnAudioStop);
        break;
    case KSSTATE_ACQUIRE:
        KeAcquireSpinLock(&m_Lock, &irql);
        m_State = State;
        KeReleaseSpinLock(&m_Lock, irql);
        break;
    case KSSTATE_PAUSE: {
        KeAcquireSpinLock(&m_Lock, &irql);
        BOOLEAN wasRunning = (m_State == KSSTATE_RUN);
        m_State = State;
        KeReleaseSpinLock(&m_Lock, irql);
        if (wasRunning && m_Timer) ExCancelTimer(m_Timer, NULL);
        if (m_Device) DeviceAudioStateChanged(m_Device, PnAudioPause);
        break;
    }
    case KSSTATE_RUN:
        KeAcquireSpinLock(&m_Lock, &irql);
        m_StartTimeHns = m_LastTickHns = PnNowHns();
        m_ByteRemainderHns = 0;
        m_State = State;
        KeReleaseSpinLock(&m_Lock, irql);
        if (m_Device) DeviceAudioStateChanged(m_Device, PnAudioRun);
        if (m_Timer) ExSetTimer(m_Timer, -PN_HNS_PER_MS, PN_HNS_PER_MS, NULL);
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

/* Advances the play position by the bytes due since the last tick and hands them to the pump. */
VOID CPnMiniportWaveRTStream::Tick()
{
    KIRQL irql;
    BOOLEAN notify = FALSE;
    KeAcquireSpinLock(&m_Lock, &irql);
    if (m_State != KSSTATE_RUN || !m_Buffer || !m_BufferSize) { KeReleaseSpinLock(&m_Lock, irql); return; }
    LONGLONG now = PnNowHns();
    if (m_Device && now > m_LastTickHns) {
        ULONGLONG tickHns = (ULONGLONG)(now - m_LastTickHns);
        ULONG tickUs = (ULONG)PN_MIN(tickHns / 10, (ULONGLONG)MAXULONG);
        m_Device->Pump.Stats.WaveMaxTickUs = PN_MAX(m_Device->Pump.Stats.WaveMaxTickUs, tickUs);
        if (m_NotifyIntervalHns && tickHns > m_NotifyIntervalHns) m_Device->Pump.Stats.WaveLateTicks++;
    }
    LONGLONG elapsed = now - m_LastTickHns + (LONGLONG)m_ByteRemainderHns;
    m_LastTickHns = now;
    if (elapsed < 0) elapsed = 0;
    /* whole frames due since the last tick; the time of the partial frame is carried */
    ULONGLONG rate = m_Format.Format.nSamplesPerSec;
    ULONGLONG frames = (ULONGLONG)elapsed * rate / PN_HNS_PER_SEC;
    m_ByteRemainderHns = (ULONGLONG)elapsed - (frames * PN_HNS_PER_SEC / rate);
    ULONGLONG bytes = frames * m_Format.Format.nBlockAlign;
    if (m_Eos) {
        /* do not run past the last written position */
        ULONG avail = (ULONG)((m_CurrentWritePosition + m_BufferSize - (ULONG)m_PlayPosition) % m_BufferSize);
        if (bytes > avail) bytes = avail;
    }
    if (bytes > m_BufferSize) bytes = m_BufferSize;
    ULONG off = (ULONG)m_PlayPosition;
    ULONG first = (ULONG)PN_MIN(bytes, (ULONGLONG)(m_BufferSize - off));
    if (m_Device && bytes) {
        PumpWritePcm(&m_Device->Pump, m_Buffer + off, first);
        if (bytes > first) PumpWritePcm(&m_Device->Pump, m_Buffer, (ULONG)(bytes - first));
    }
    m_PlayPosition = (m_PlayPosition + bytes) % m_BufferSize;
    m_LinearPosition += bytes;
    if (m_Notifications) {
        ULONG packetSize = m_BufferSize / m_Notifications;
        LONGLONG pc = (LONGLONG)(m_LinearPosition / packetSize);
        if (pc != m_PacketCounter) { m_PacketCounter = pc; notify = TRUE; }
    }
    KeReleaseSpinLock(&m_Lock, irql);
    if (notify) {
        KeAcquireSpinLock(&m_Lock, &irql);
        for (PLIST_ENTRY l = m_NotifyList.Flink; l != &m_NotifyList; l = l->Flink) KeSetEvent(CONTAINING_RECORD(l, PN_NOTIFY_ENTRY, Link)->Event, 0, FALSE);
        KeReleaseSpinLock(&m_Lock, irql);
    }
}

STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::GetPosition(_Out_ PKSAUDIO_POSITION Position)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    Position->PlayOffset = m_PlayPosition;
    Position->WriteOffset = m_PlayPosition;
    KeReleaseSpinLock(&m_Lock, irql);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::SetWritePacket(_In_ ULONG PacketNumber, _In_ DWORD Flags, _In_ ULONG EosPacketLength)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    if (m_Notifications == 0 || m_BufferSize < m_Notifications) { KeReleaseSpinLock(&m_Lock, irql); return STATUS_NOT_SUPPORTED; }
    if (m_Eos) { KeReleaseSpinLock(&m_Lock, irql); return STATUS_INVALID_DEVICE_STATE; }
    ULONG packetSize = m_BufferSize / m_Notifications;
    ULONG expected = (ULONG)m_PacketCounter + (m_State == KSSTATE_RUN ? 1 : 0);
    LONG delta = (LONG)(PacketNumber - expected);
    if (delta < 0) { KeReleaseSpinLock(&m_Lock, irql); return STATUS_DATA_LATE_ERROR; }
    if (delta > 0) { KeReleaseSpinLock(&m_Lock, irql); return STATUS_DATA_OVERRUN; }
    ULONG idx = PacketNumber % m_Notifications;
    m_LastOsWritePacket = PacketNumber;
    m_CurrentWritePosition = idx * packetSize;
    if (Flags & KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM) {
        m_CurrentWritePosition = (m_CurrentWritePosition + PN_MIN(EosPacketLength, packetSize)) % m_BufferSize;
        m_Eos = TRUE;
    }
    KeReleaseSpinLock(&m_Lock, irql);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::GetOutputStreamPresentationPosition(_Out_ KSAUDIO_PRESENTATION_POSITION *Pos)
{
    if (m_Notifications == 0) return STATUS_NOT_SUPPORTED;
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    ULONGLONG linear = m_LinearPosition;
    KeReleaseSpinLock(&m_Lock, irql);
    /* what the sink has presented: consumed position minus the Bluetooth path latency */
    ULONGLONG latencyBytes = 0;
    if (m_Device) latencyBytes = (ULONGLONG)(PumpEstimatedLatencyUs(&m_Device->Pump) + m_Device->Avdtp.SinkDelayTenthsMs * 100) * m_RateNumerator / 1000000ull;
    ULONGLONG presented = linear > latencyBytes ? linear - latencyBytes : 0;
    LARGE_INTEGER qpc = KeQueryPerformanceCounter(NULL);
    Pos->u64PositionInBlocks = presented / PN_MAX(m_Format.Format.nBlockAlign, 1u);
    Pos->u64QPCPosition = (ULONGLONG)qpc.QuadPart;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::GetPacketCount(_Out_ ULONG *PacketCount)
{
    if (m_Notifications == 0) return STATUS_NOT_SUPPORTED;
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    *PacketCount = (ULONG)m_PacketCounter;
    KeReleaseSpinLock(&m_Lock, irql);
    return STATUS_SUCCESS;
}
