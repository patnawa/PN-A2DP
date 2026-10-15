/*
 * audio_wave.cpp - WaveRT render miniport and stream.
 *
 * The miniport advertises PCM formats derived from the receiver's capabilities (cached
 * from the last connection until this boot's first one), the configuration and the
 * negotiated rate, rebuilds them on PnAudioFormatsChanged and fires
 * KSEVENT_PINCAPS_FORMATCHANGE when the list changes (rules: wave_formats.h). The stream owns the cyclic buffer the audio engine writes into and a
 * 1-2 ms high-resolution timer that moves the consumed PCM into the pump at the nominal
 * sample rate; notification events fire at packet boundaries (event-driven mode).
 * Clock, presentation and starvation arithmetic lives in wave_math.h (unit tested).
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

/* No KSPROPERTY_PIN_PROPOSEDATAFORMAT2: Windows uses a driver's answer to it as the engine
 * format and ignores the user's Default Format, so answering it pinned every PN endpoint to
 * the first table entry (48 kHz). Without it Windows uses the format the user selects; the
 * stream follows that rate natively when the sink supports it and converts otherwise. */
static PCPROPERTY_ITEM g_WaveFilterProps[] = {
    { &KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT, KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, CPnMiniportWaveRT::PropertyHandlerWaveFilter },
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

static VOID FillFormat(_Out_ KSDATAFORMAT_WAVEFORMATEXTENSIBLE *F, _In_ ULONG Rate, _In_ ULONG Channels, _In_ ULONG ContainerBits, _In_ ULONG ValidBits,
                       _In_ BOOLEAN IsFloat)
{
    RtlZeroMemory(F, sizeof(*F));
    F->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    F->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    F->DataFormat.SubFormat = IsFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
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
    F->WaveFormatExt.SubFormat = IsFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
}

/* Envelope of the table entries of one representation, for the pin's data range. With no
 * entry of that kind (the float range while no hi-res codec is known) it still describes
 * the table's rates, so it stays a valid range; proposals for it simply find no match. */
static VOID FillRange(_Out_ KSDATARANGE_AUDIO *R, _In_ BOOLEAN IsFloat, _In_reads_(Count) const PN_WAVE_FORMAT *T, _In_ ULONG Count)
{
    ULONG minRate = 0xFFFFFFFF, maxRate = 0, minBits = 32, maxBits = 0, rateLo = 0xFFFFFFFF, rateHi = 0;
    for (ULONG i = 0; i < Count; i++) {
        rateLo = PN_MIN(rateLo, T[i].Rate); rateHi = PN_MAX(rateHi, T[i].Rate);
        if ((T[i].IsFloat != 0) != (IsFloat != 0)) continue;
        minRate = PN_MIN(minRate, T[i].Rate); maxRate = PN_MAX(maxRate, T[i].Rate);
        minBits = PN_MIN(minBits, (ULONG)T[i].ContainerBits); maxBits = PN_MAX(maxBits, (ULONG)T[i].ContainerBits);
    }
    if (!maxRate) { minRate = rateLo; maxRate = rateHi; minBits = maxBits = IsFloat ? 32 : 16; }
    if (!maxRate) { minRate = 44100; maxRate = 48000; }
    RtlZeroMemory(R, sizeof(*R));
    R->DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
    R->DataRange.Flags = KSDATARANGE_ATTRIBUTES;   /* the next DataRanges entry is the mode list */
    R->DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    R->DataRange.SubFormat = IsFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    R->DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    R->MaximumChannels = 2;
    R->MinimumBitsPerSample = minBits;
    R->MaximumBitsPerSample = maxBits;
    R->MinimumSampleFrequency = minRate;
    R->MaximumSampleFrequency = maxRate;
}

static PWAVEFORMATEX GetWaveFormatEx(_In_ PKSDATAFORMAT F)
{
    if (!F || F->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX)) return NULL;
    if (!IsEqualGUIDAligned(F->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) || !IsEqualGUIDAligned(F->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) return NULL;
    return (PWAVEFORMATEX)(F + 1);
}

/* Inputs of the table (wave_formats.h). Caller holds m_FormatMutex (PASSIVE). The live
 * capabilities are copied under the device's status lock; before this boot's first
 * connection they are empty and the copy saved at the last connection is used. The
 * negotiated codec and the configuration words are read without the device lock: a torn
 * read during a renegotiation only builds a stale table, and the device reports the
 * change again (PnAudioFormatsChanged) once it is done. */
#pragma code_seg("PAGE")
VOID CPnMiniportWaveRT::LoadFormatInput(_Out_ PN_WAVE_FORMAT_INPUT *In)
{
    PAGED_CODE();
    PPNA2DP_DEVICE d = m_Device;
    RtlZeroMemory(In, sizeof(*In));
    In->AllowFloat = m_DataRangeCount == 4;
    if (!d) return;
    BOOLEAN haveCaps = FALSE;
    DeviceGetCapabilities(d, &m_CapsScratch);
    haveCaps = PnWaveSanitizeCachedCaps(&m_CapsScratch);
    if (!haveCaps && NT_SUCCESS(RegCfgLoadCapabilities(d->DriverObject, d->Link.RemoteAddress, &m_CapsScratch)))
        haveCaps = PnWaveSanitizeCachedCaps(&m_CapsScratch);
    In->Caps = haveCaps ? &m_CapsScratch : NULL;
    In->EnabledCodecMask = d->Config.EnabledCodecMask;
    In->ExposeAllRates = (d->Config.Flags & PNA2DP_CFG_EXPOSE_ALL_RATES) != 0;
    In->ActiveValid = d->ActiveValid;
    if (In->ActiveValid) {
        In->ActiveCodec = d->Active.Codec;
        In->ActiveRate = d->Active.SampleRate;
    }
}

/* PASSIVE. Rebuilds the table; returns TRUE if it changed. Readers (property handlers,
 * NewStream) take m_FormatMutex; PortCls reads the data ranges without it, so they are
 * written to the idle copy and published by swapping the pointers. A reader that already
 * holds the old pointer keeps reading an unchanged copy: it is only rewritten by the next
 * rebuild that changes the table, which takes a new connection or configuration change. */
#pragma code_seg("PAGE")
BOOLEAN CPnMiniportWaveRT::BuildFormatTable()
{
    PAGED_CODE();
    PN_WAVE_FORMAT_INPUT in;
    PN_WAVE_FORMAT list[PN_MAX_FORMATS];
    PnMutexAcquire(&m_FormatMutex);
    LoadFormatInput(&in);
    ULONG n = PnWaveBuildFormats(&in, list, PN_MAX_FORMATS);
    BOOLEAN changed = n != m_FormatCount || RtlCompareMemory(list, m_FormatList, n * sizeof(list[0])) != n * sizeof(list[0]);
    if (changed) {
        RtlCopyMemory(m_FormatList, list, n * sizeof(list[0]));
        for (ULONG i = 0; i < n; i++) FillFormat(&m_Formats[i], list[i].Rate, list[i].Channels, list[i].ContainerBits, list[i].ValidBits, list[i].IsFloat);
        m_FormatCount = n;
        ULONG next = m_DataRangePointers[0] ? (m_RangeCopy ^ 1) : 0;
        FillRange(&m_Ranges[next][0], FALSE, list, n);
        FillRange(&m_Ranges[next][1], TRUE, list, n);
        InterlockedExchangePointer((PVOID volatile *)&m_DataRangePointers[0], &m_Ranges[next][0]);
        if (m_DataRangeCount == 4) InterlockedExchangePointer((PVOID volatile *)&m_DataRangePointers[2], &m_Ranges[next][1]);
        m_RangeCopy = next;
    }
    PnMutexRelease(&m_FormatMutex);
    if (changed) {
        PN_INFO("wave formats: %u entries, first %u Hz %u/%u bit%s%s", n, n ? list[0].Rate : 0, n ? list[0].ValidBits : 0, n ? list[0].ContainerBits : 0,
                in.Caps ? "" : " (receiver capabilities unknown)", in.AllowFloat ? ", float range" : "");
    }
    return changed;
}

#pragma code_seg("PAGE")
VOID CPnMiniportWaveRT::GeneratePinCapsChange()
{
    PAGED_CODE();
    if (m_PortEvents) m_PortEvents->GenerateEventList((GUID *)&KSEVENTSETID_PinCapsChange, KSEVENT_PINCAPS_FORMATCHANGE, TRUE, PN_WAVE_PIN_SYSTEM, FALSE, ULONG(-1));
}

/* Contract C6 worker (PASSIVE, adapter work item). */
#pragma code_seg("PAGE")
VOID CPnMiniportWaveRT::FormatsChanged()
{
    PAGED_CODE();
    if (BuildFormatTable()) GeneratePinCapsChange();
}

/* TRUE if Format matches a table entry; the entry is copied to Match. Every field that is
 * later used for arithmetic (block align, byte rate) must match the table, an extensible
 * format must really be as large as it claims, and the KS subformat must agree with the
 * WAVEFORMATEX tag (wave_formats.h). */
#pragma code_seg("PAGE")
_Success_(return) BOOLEAN CPnMiniportWaveRT::MatchFormat(_In_ PKSDATAFORMAT Format, _Out_opt_ WAVEFORMATEXTENSIBLE *Match)
{
    PAGED_CODE();
    PWAVEFORMATEX w = GetWaveFormatEx(Format);
    if (!w) return FALSE;
    ULONG ksSub = IsEqualGUIDAligned(Format->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ? PN_WAVE_SUB_PCM :
                  IsEqualGUIDAligned(Format->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? PN_WAVE_SUB_FLOAT :
                  IsEqualGUIDAligned(Format->SubFormat, KSDATAFORMAT_SUBTYPE_WAVEFORMATEX) ? PN_WAVE_SUB_WAVEFORMATEX : PN_WAVE_SUB_OTHER;
    ULONG extSub = PN_WAVE_SUB_OTHER;
    ULONG valid = w->wBitsPerSample;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (Format->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE) || w->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return FALSE;
        PWAVEFORMATEXTENSIBLE x = (PWAVEFORMATEXTENSIBLE)w;
        extSub = IsEqualGUIDAligned(x->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ? PN_WAVE_SUB_PCM :
                 IsEqualGUIDAligned(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? PN_WAVE_SUB_FLOAT : PN_WAVE_SUB_OTHER;
        valid = x->Samples.wValidBitsPerSample;
    }
    int kind = PnWaveFormatKind(ksSub, w->wFormatTag, extSub);
    if (kind < 0) return FALSE;
    PnMutexAcquire(&m_FormatMutex);
    LONG i = PnWaveFindFormat(m_FormatList, m_FormatCount, w->nSamplesPerSec, w->nChannels, w->wBitsPerSample, valid, w->nBlockAlign,
                              w->nAvgBytesPerSec, kind == 1);
    if (i >= 0 && Match) *Match = m_Formats[i].WaveFormatExt;
    PnMutexRelease(&m_FormatMutex);
    return i >= 0;
}

#pragma code_seg("PAGE")
BOOLEAN CPnMiniportWaveRT::IsFormatSupported(_In_ PKSDATAFORMAT Format)
{
    PAGED_CODE();
    return MatchFormat(Format, NULL);
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
    : CUnknown(UnknownOuter), m_Adapter(NULL), m_Port(NULL), m_PortEvents(NULL), m_Device(NULL), m_Stream(NULL), m_FormatCount(0),
      m_RangeCopy(0), m_DataRangeCount(2)
{
    PAGED_CODE();
    KeInitializeSpinLock(&m_Lock);
    ExInitializeFastMutex(&m_FormatMutex);
    RtlZeroMemory(m_FormatList, sizeof(m_FormatList));
    RtlZeroMemory(m_Formats, sizeof(m_Formats));
    RtlZeroMemory(&m_CapsScratch, sizeof(m_CapsScratch));
    RtlZeroMemory(m_Ranges, sizeof(m_Ranges));
    RtlZeroMemory(m_DataRangePointers, sizeof(m_DataRangePointers));
    RtlZeroMemory(m_Pins, sizeof(m_Pins));
    RtlZeroMemory(&m_Filter, sizeof(m_Filter));
}

#pragma code_seg("PAGE")
CPnMiniportWaveRT::~CPnMiniportWaveRT()
{
    PAGED_CODE();
    /* the adapter held a reference until RemoveFilters detached this miniport */
    if (m_Adapter) { m_Adapter->Release(); m_Adapter = NULL; }
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

    /* PortCls copies DataRangesCount when the filter is registered, so whether the pin has
     * an IEEE float range is decided once, here: only for a receiver already known (live
     * or cached capabilities) to have a codec that encodes more than 16 bits. A receiver
     * seen for the first time gets 24-in-32 after it connects, and float from the next
     * start. An SBC/AAC receiver keeps the original two-entry layout. */
    PN_WAVE_FORMAT_INPUT in;
    PnMutexAcquire(&m_FormatMutex);
    LoadFormatInput(&in);
    m_DataRangeCount = PnWaveFormatsWantHiRes(&in) ? 4 : 2;
    PnMutexRelease(&m_FormatMutex);
    m_DataRangePointers[1] = (PKSDATARANGE)&g_ModeAttributeList;
    m_DataRangePointers[3] = (PKSDATARANGE)&g_ModeAttributeList;
    BuildFormatTable();

    /* per-instance filter descriptor */
    m_Filter = g_PnWaveFilterDescriptor;
    RtlZeroMemory(m_Pins, sizeof(m_Pins));
    m_Pins[PN_WAVE_PIN_SYSTEM].MaxGlobalInstanceCount = 1;
    m_Pins[PN_WAVE_PIN_SYSTEM].MaxFilterInstanceCount = 1;
    m_Pins[PN_WAVE_PIN_SYSTEM].MinFilterInstanceCount = 0;
    m_Pins[PN_WAVE_PIN_SYSTEM].AutomationTable = NULL;
    m_Pins[PN_WAVE_PIN_SYSTEM].KsPinDescriptor.DataRangesCount = m_DataRangeCount;
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
    /* last: from here on PnAudioFormatsChanged can rebuild the table (serialized by m_FormatMutex) */
    m_Adapter->SetWaveMiniport(this);
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
    PN_VERB("intersection pin %u size %u output %u sub %08x", PinId, DataRange->FormatSize, OutputBufferLength, DataRange->SubFormat.Data1);
    *ResultantFormatLength = 0;
    if (PinId != PN_WAVE_PIN_SYSTEM) return STATUS_INVALID_PARAMETER;
    if (!IsEqualGUIDAligned(DataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) && !IsEqualGUIDAligned(DataRange->MajorFormat, KSDATAFORMAT_TYPE_WILDCARD)) return STATUS_NO_MATCH;
    BOOLEAN clientFloat = IsEqualGUIDAligned(DataRange->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    BOOLEAN clientWild = IsEqualGUIDAligned(DataRange->SubFormat, KSDATAFORMAT_SUBTYPE_WILDCARD) != 0;
    if (!clientFloat && !clientWild && !IsEqualGUIDAligned(DataRange->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) return STATUS_NO_MATCH;
    /* a wildcard client range takes the representation of our range it was matched with */
    BOOLEAN wantFloat = clientFloat || (clientWild && MatchingDataRange && IsEqualGUIDAligned(MatchingDataRange->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    if (!IsEqualGUIDAligned(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) && !IsEqualGUIDAligned(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WILDCARD)) return STATUS_NO_MATCH;

    const KSDATARANGE_AUDIO *cr = (DataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO)) ? (const KSDATARANGE_AUDIO *)DataRange : NULL;
    KSDATAFORMAT_WAVEFORMATEXTENSIBLE picked;
    PnMutexAcquire(&m_FormatMutex);
    LONG i = PnWavePickFormat(m_FormatList, m_FormatCount, wantFloat, cr != NULL, cr ? cr->MinimumSampleFrequency : 0, cr ? cr->MaximumSampleFrequency : 0,
                              cr ? cr->MinimumBitsPerSample : 0, cr ? cr->MaximumBitsPerSample : 0, cr ? cr->MaximumChannels : 0);
    if (i >= 0) picked = m_Formats[i];
    PnMutexRelease(&m_FormatMutex);
    if (i < 0) return STATUS_NO_MATCH;
    const KSDATAFORMAT_WAVEFORMATEXTENSIBLE *pick = &picked;

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
NTSTATUS CPnMiniportWaveRT::PropertyHandlerWaveFilter(_In_ PPCPROPERTY_REQUEST Req)
{
    PAGED_CODE();
    PN_VERB("wave property %u verb %08x instance %u value %u", Req->PropertyItem->Id, Req->Verb, Req->InstanceSize, Req->ValueSize);
    CPnMiniportWaveRT *w = reinterpret_cast<CPnMiniportWaveRT *>(Req->MajorTarget);
    if (!w) return STATUS_INVALID_PARAMETER;
    if (IsEqualGUIDAligned(*Req->PropertyItem->Set, KSPROPSETID_Pin)) {
        if (Req->PropertyItem->Id == KSPROPERTY_PIN_PROPOSEDATAFORMAT) return w->PropertyProposedFormat(Req);
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
      m_LastTickHns(0), m_RateNumerator(0), m_FrameRemainder(0), m_LastTickQpc(0), m_TickPeriodHns(0), m_NotifyAccumHns(0), m_PacketCounter(0),
      m_LastOsWritePacket(ULONG_MAX), m_CurrentWritePosition(0), m_StarvedReported(0), m_WritePacketUsed(FALSE), m_Eos(FALSE), m_Registered(FALSE),
      m_Holding(FALSE), m_OwedBytes(0), m_FadeFrames(0), m_FadeInPos(0), m_FadeInActive(FALSE),
      m_LastSegmentSilent(FALSE), m_TailFrames(0)
{
    PAGED_CODE();
    KeInitializeSpinLock(&m_Lock);
    InitializeListHead(&m_NotifyList);
    RtlZeroMemory(&m_Format, sizeof(m_Format));
    RtlZeroMemory(&m_Starvation, sizeof(m_Starvation));
    RtlZeroMemory(&m_Presentation, sizeof(m_Presentation));
    RtlZeroMemory(&m_LateHold, sizeof(m_LateHold));
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
    /* our own table entry: block align and byte rate are known to be consistent and non-zero */
    if (!Miniport->MatchFormat(DataFormat, &m_Format)) return STATUS_INVALID_PARAMETER;
    m_Miniport = Miniport;
    m_Miniport->AddRef();
    m_PortStream = PortStream;
    m_Pin = Pin;
    m_Device = Miniport->Device();
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
    /* CPU-only buffer (no DMA): any physical page will do */
    PHYSICAL_ADDRESS high; high.QuadPart = MAXLONGLONG;
    PMDL mdl = m_PortStream->AllocatePagesForMdl(high, RequestedSize);
    if (!mdl) return STATUS_UNSUCCESSFUL;
    /* the allocation may be partial; never report (or touch) more than the MDL describes */
    if (MmGetMdlByteCount(mdl) < RequestedSize) {
        PN_WARN("wave buffer: %u of %u bytes allocated", MmGetMdlByteCount(mdl), RequestedSize);
        m_PortStream->FreePagesFromMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
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
    if (Mdl) FreeBufferWithNotification(Mdl, Size);
    else PublishBuffer(NULL, NULL, 0, 0);
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
        if (m_Device) FlushTail(FALSE);   /* the end of the sound reaches the pump */
        m_State = State;   /* a tick that is already queued sees this and does nothing */
        m_LinearPosition = 0; m_PlayPosition = 0; m_PacketCounter = 0; m_LastOsWritePacket = ULONG_MAX; m_CurrentWritePosition = 0; m_Eos = FALSE;
        m_FrameRemainder = 0; m_NotifyAccumHns = 0; m_WritePacketUsed = FALSE;
        RtlZeroMemory(&m_Starvation, sizeof(m_Starvation)); m_StarvedReported = 0;
        RtlZeroMemory(&m_LateHold, sizeof(m_LateHold)); m_OwedBytes = 0; m_Holding = FALSE;
        m_FadeInPos = 0; m_FadeInActive = m_LastSegmentSilent = FALSE;
        RtlZeroMemory(&m_Presentation, sizeof(m_Presentation));   /* a new run starts a new monotonic clock */
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
        if (m_Device && wasRunning) {
            /* the end of the sound: the held-back frames join the ring, and what waits there
             * fades out, is padded and drains, in one step (#15/#26). Handed over on their own
             * first, they woke the pump thread, which could encode them unfaded (#1/#7). */
            PumpFinishStream(&m_Device->Pump, m_Tail, m_TailFrames * m_Format.Format.nBlockAlign);
            m_TailFrames = 0;
        } else if (m_Device) FlushTail(FALSE);
        m_State = State;
        KeReleaseSpinLock(&m_Lock, irql);
        if (wasRunning && m_Timer) ExCancelTimer(m_Timer, NULL);
        if (m_Device) DeviceAudioStateChanged(m_Device, PnAudioPause);
        break;
    }
    case KSSTATE_RUN: {
        /* the pump faded the stream out at a pause or stop and still carries it: the new
         * audio fades in rather than stepping up from that silence (findings #15/#26); a
         * stream the pump starts is handled by the pump */
        BOOLEAN continuing = m_Device && PumpIsStreaming(&m_Device->Pump) && PumpTakeResumeFade(&m_Device->Pump);
        if (m_Device) DeviceAudioStateChanged(m_Device, PnAudioRun);   /* before the first tick: it may hold the clock */
        KeAcquireSpinLock(&m_Lock, &irql);
        m_StartTimeHns = m_LastTickHns = PnNowHns();
        m_LastTickQpc = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
        m_FrameRemainder = 0;
        m_OwedBytes = 0;
        RtlZeroMemory(&m_LateHold, sizeof(m_LateHold));
        m_TickPeriodHns = PnWaveTickPeriodHns(m_Notifications ? m_NotifyIntervalHns : 0);   /* 1 or 2 ms: see wave_math.h */
        m_FadeFrames = m_Format.Format.nBlockAlign ?
            PnWaveFadeFrames(m_Format.Format.nSamplesPerSec, m_Notifications ? m_BufferSize / m_Notifications / m_Format.Format.nBlockAlign : 0,
                             PN_WAVE_FADE_SCRATCH / m_Format.Format.nBlockAlign) : 0;
        m_FadeInActive = continuing && m_FadeFrames;
        m_FadeInPos = 0;
        m_LastSegmentSilent = FALSE;
        m_TailFrames = 0;
        m_State = State;
        KeReleaseSpinLock(&m_Lock, irql);
        if (m_Timer) ExSetTimer(m_Timer, -(LONGLONG)m_TickPeriodHns, m_TickPeriodHns, NULL);
        break;
    }
    default:
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

/* Zero PCM (16-bit, 24-in-32 and float silence) for packets the engine did not write. */
static const UCHAR g_PnWaveSilence[1024] = { 0 };

static VOID PnWaveWriteSilence(_Inout_ PPN_PUMP Pump, _In_ ULONG Bytes)
{
    while (Bytes) {   /* bounded: Bytes <= one cyclic buffer */
        ULONG n = PN_MIN(Bytes, (ULONG)sizeof(g_PnWaveSilence));
        PumpWritePcm(Pump, g_PnWaveSilence, n);
        Bytes -= n;
    }
}

/* Starved packets counted by the stream since the last call (<= DISPATCH, no lock held).
 * Contract C5: PNA2DP_STATS.WaveStarvedPackets. */
static VOID PnWaveNoteStarvation(_In_ PPNA2DP_DEVICE D, _In_ ULONG Packets)
{
    PumpNoteWaveStarvation(&D->Pump, Packets);
}

/* PCM to the pump through the hold-back FIFO of the newest m_FadeFrames frames (m_Lock held). */
VOID CPnMiniportWaveRTStream::EmitPcm(_In_reads_bytes_(Bytes) const UCHAR *Data, _In_ ULONG Bytes)
{
    PPN_PUMP pump = &m_Device->Pump;
    ULONG bpf = m_Format.Format.nBlockAlign;
    ULONG keep = m_FadeFrames * bpf, tail = m_TailFrames * bpf;
    if (!keep || !bpf) { PumpWritePcm(pump, Data, Bytes); return; }
    if (tail + Bytes <= keep) {
        RtlCopyMemory(m_Tail + tail, Data, Bytes);
        m_TailFrames += Bytes / bpf;
        return;
    }
    ULONG out = tail + Bytes - keep;            /* leaves the FIFO, oldest first */
    ULONG fromTail = PN_MIN(out, tail), fromData = out - fromTail;
    if (fromTail) PumpWritePcm(pump, m_Tail, fromTail);
    if (fromData) PumpWritePcm(pump, Data, fromData);
    ULONG tailLeft = tail - fromTail;
    if (tailLeft) RtlMoveMemory(m_Tail, m_Tail + fromTail, tailLeft);
    RtlCopyMemory(m_Tail + tailLeft, Data + fromData, Bytes - fromData);
    m_TailFrames = keep / bpf;
}

/* The held-back frames to the pump (m_Lock held); FadeOut: fading to silence at their end, over
 * all of them (a FIFO not yet full after a start or a gap gets a whole, shorter fade, not the
 * tail of a full-length one). */
VOID CPnMiniportWaveRTStream::FlushTail(_In_ BOOLEAN FadeOut)
{
    if (!m_TailFrames) return;
    ULONG bpf = m_Format.Format.nBlockAlign, bits = m_Format.Format.wBitsPerSample;
    if (FadeOut && m_TailFrames <= m_FadeFrames)
        PnPcmRamp(m_Tail, m_TailFrames, m_Format.Format.nChannels, bits,
                  m_Format.Samples.wValidBitsPerSample ? m_Format.Samples.wValidBitsPerSample : bits,
                  IsEqualGUIDAligned(m_Format.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? TRUE : FALSE,
                  0, m_TailFrames, FALSE);
    PumpWritePcm(&m_Device->Pump, m_Tail, m_TailFrames * bpf);
    m_TailFrames = 0;
}

/* One segment of the cyclic buffer (within one packet) to the pump, m_Lock held. Silent: the
 * packet was not written; the held-back audio before the gap fades out. Audio after a gap (or
 * a resume) fades in. Fades work in the engine format (finding #4). */
VOID CPnMiniportWaveRTStream::WriteSegment(_In_ ULONG Offset, _In_ ULONG Bytes, _In_ BOOLEAN Silent)
{
    PPN_PUMP pump = &m_Device->Pump;
    ULONG bpf = m_Format.Format.nBlockAlign;
    ULONG channels = m_Format.Format.nChannels, bits = m_Format.Format.wBitsPerSample;
    ULONG valid = m_Format.Samples.wValidBitsPerSample ? m_Format.Samples.wValidBitsPerSample : bits;
    BOOLEAN isFloat = IsEqualGUIDAligned(m_Format.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? TRUE : FALSE;
    ULONG frames = bpf ? Bytes / bpf : 0;
    if (Silent) {
        FlushTail(!m_LastSegmentSilent);
        PnWaveWriteSilence(pump, Bytes);
        m_FadeInActive = m_FadeFrames != 0;
        m_FadeInPos = 0;
    } else {
        ULONG n = (m_FadeInActive && m_FadeInPos < m_FadeFrames) ? PN_MIN(frames, m_FadeFrames - m_FadeInPos) : 0;
        if (n) {
            RtlCopyMemory(m_FadeScratch, m_Buffer + Offset, n * bpf);
            PnPcmRamp(m_FadeScratch, n, channels, bits, valid, isFloat, m_FadeInPos, m_FadeFrames, TRUE);
            EmitPcm(m_FadeScratch, n * bpf);
            m_FadeInPos += n;
            if (m_FadeInPos >= m_FadeFrames) m_FadeInActive = FALSE;
        }
        if (Bytes > n * bpf) EmitPcm(m_Buffer + Offset + n * bpf, Bytes - n * bpf);
    }
    m_LastSegmentSilent = Silent;
}

/* Advances the play position by the bytes due since the last tick and hands them to the pump. */
VOID CPnMiniportWaveRTStream::Tick()
{
    KIRQL irql;
    BOOLEAN notify = FALSE;
    ULONG starved = 0;
    BOOLEAN gaveUp = FALSE;
    KeAcquireSpinLock(&m_Lock, &irql);
    if (m_State != KSSTATE_RUN || !m_Buffer || !m_BufferSize) { KeReleaseSpinLock(&m_Lock, irql); return; }
    LONGLONG now = PnNowHns();
    /* a stream start is pending: hold the clock instead of rendering audio the start would
     * discard (wave_math.h, findings #23/#24) */
    if (m_Device && DeviceRenderHold(m_Device, now)) {
        m_LastTickHns = now;
        m_LastTickQpc = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
        m_FrameRemainder = 0;
        m_Holding = TRUE;
        KeReleaseSpinLock(&m_Lock, irql);
        return;
    }
    m_Holding = FALSE;
    if (m_Device && now > m_LastTickHns) {
        ULONGLONG tickHns = (ULONGLONG)(now - m_LastTickHns);
        ULONG tickUs = (ULONG)PN_MIN(tickHns / 10, (ULONGLONG)MAXULONG);
        m_Device->Pump.Stats.WaveMaxTickUs = PN_MAX(m_Device->Pump.Stats.WaveMaxTickUs, tickUs);
        if (m_NotifyIntervalHns && tickHns > m_NotifyIntervalHns) m_Device->Pump.Stats.WaveLateTicks++;
    }
    ULONGLONG elapsed = now > m_LastTickHns ? (ULONGLONG)(now - m_LastTickHns) : 0;
    m_LastTickHns = now;
    m_LastTickQpc = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
    /* whole frames due since the last tick; the exact fractional frame is carried */
    ULONGLONG bytes = PnWaveFramesDue(elapsed, m_Format.Format.nSamplesPerSec, &m_FrameRemainder) * m_Format.Format.nBlockAlign + m_OwedBytes;
    m_OwedBytes = 0;
    if (m_Eos) {
        /* do not run past the last written position */
        ULONG avail = (ULONG)((m_CurrentWritePosition + m_BufferSize - (ULONG)m_PlayPosition) % m_BufferSize);
        if (bytes > avail) bytes = avail;
    }
    if (bytes > m_BufferSize) bytes = m_BufferSize;
    /* Event-driven packet mode: a packet the engine has not written by the time playback
     * enters it holds the previous buffer cycle and is played as silence (wave_math.h).
     * The EOS path is unchanged: it already stops at the last written position. */
    ULONG packetSize = m_Notifications ? m_BufferSize / m_Notifications : 0;
    /* the starvation check also runs after giving up: it re-arms on time (wave_math.h) */
    BOOLEAN checkPackets = packetSize && m_WritePacketUsed && !m_Eos;
    ULONG off = (ULONG)m_PlayPosition;
    ULONG remaining = (ULONG)bytes;
    BOOLEAN held = FALSE;
    /* T2: while the receiver keeps our stream suspended, whether audio resumed (review round 4 #7) */
    BOOLEAN watch = m_Device && DeviceAwaitsAudioResume(m_Device), audible = FALSE;
    while (remaining) {   /* bounded: each pass ends at a packet or buffer boundary within one buffer */
        ULONG seg = PN_MIN(remaining, m_BufferSize - off);
        BOOLEAN silent = FALSE;
        if (checkPackets) {
            ULONGLONG packet = m_LinearPosition / packetSize;
            /* a late tick: do not enter a packet the engine was never asked to write (#20) */
            if (m_LinearPosition % packetSize == 0 &&
                PnWaveLateHold(&m_LateHold, packet, (ULONGLONG)m_PacketCounter, m_LastOsWritePacket, (ULONGLONG)now, remaining, packetSize)) {
                held = TRUE;
                break;
            }
            seg = PN_MIN(seg, packetSize - (ULONG)(m_LinearPosition % packetSize));
            silent = PnWavePacketSilent(&m_Starvation, packet, TRUE, m_LastOsWritePacket);
        }
        if (m_Device) {
            WriteSegment(off, seg, silent);
            if (watch && !silent && !audible) audible = PnPcmHasSignal(m_Buffer + off, seg);
        }
        off += seg;
        if (off >= m_BufferSize) off = 0;
        m_LinearPosition += seg;
        remaining -= seg;
    }
    /* a tick that ends exactly where such a packet starts holds there as well: deciding it now
     * would silence it before the engine is asked for it */
    if (checkPackets && !held && m_LinearPosition % packetSize == 0 &&
        PnWaveLateHold(&m_LateHold, m_LinearPosition / packetSize, (ULONGLONG)m_PacketCounter, m_LastOsWritePacket, (ULONGLONG)now, 0, packetSize))
        held = TRUE;
    if (held) m_OwedBytes = remaining;
    m_PlayPosition = off;
    /* the engine's end of stream reached: nothing follows the held-back frames, so the sound
     * ends as at a pause (fade, pad, drain); the pause that follows adds nothing (#1/#7) */
    if (m_Eos && m_Device && m_TailFrames && (ULONG)m_PlayPosition == m_CurrentWritePosition) {
        PumpFinishStream(&m_Device->Pump, m_Tail, m_TailFrames * m_Format.Format.nBlockAlign);
        m_TailFrames = 0;
    }
    if (m_Device) DeviceNoteRenderedAudio(m_Device, watch, audible, (ULONG)PN_MIN(elapsed / 10, (ULONGLONG)MAXULONG));
    if (checkPackets) {
        /* decide the packet now in progress before the engine is told about it; a packet
         * held at its first frame is decided when playback enters it */
        if (!held) PnWavePacketSilent(&m_Starvation, m_LinearPosition / packetSize, TRUE, m_LastOsWritePacket);
        starved = m_Starvation.Starved - m_StarvedReported;
        m_StarvedReported = m_Starvation.Starved;
        gaveUp = starved && m_Starvation.Disabled;   /* once per give-up: nothing is counted after it */
    }
    if (m_Notifications) {
        LONGLONG pc = (LONGLONG)(m_LinearPosition / packetSize);
        if (held && pc) pc--;   /* the held packet has not started: the engine is asked to write it */
        if (pc != m_PacketCounter) { m_PacketCounter = pc; notify = TRUE; }
    }
    KeReleaseSpinLock(&m_Lock, irql);
    if (starved && m_Device) PnWaveNoteStarvation(m_Device, starved);
    if (gaveUp) PN_WARN("wave: %u consecutive packets not written by the engine; starvation check off until it writes on time again", (ULONG)PN_WAVE_STARVE_GIVE_UP);
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

_IRQL_requires_max_(PASSIVE_LEVEL)   /* as IMP_IMiniportWaveRTOutputStream declares it */
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
    /* only an accepted write while running proves the engine follows this protocol; a
     * stream whose writes are all rejected keeps the old behaviour (never silenced) */
    if (m_State == KSSTATE_RUN) m_WritePacketUsed = TRUE;
    m_CurrentWritePosition = idx * packetSize;
    if (Flags & KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM) {
        m_CurrentWritePosition = (m_CurrentWritePosition + PN_MIN(EosPacketLength, packetSize)) % m_BufferSize;
        m_Eos = TRUE;
    }
    KeReleaseSpinLock(&m_Lock, irql);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)   /* as IMP_IMiniportWaveRTOutputStream declares it */
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::GetOutputStreamPresentationPosition(_Out_ KSAUDIO_PRESENTATION_POSITION *Pos)
{
    if (m_Notifications == 0) return STATUS_NOT_SUPPORTED;
    /* Bluetooth path latency between the consumed position and what the sink presents
     * (C2/C3: pipeline + the applied sink delay - the set speaker delay, a plausible report
     * or the per-codec default - the sources the status reports). Read without the stream
     * lock (the pump has its own). A changed sink delay is followed at the 1 %/s slew. */
    ULONG latencyUs = 0, epoch = 0;
    if (m_Device) {
        epoch = PumpLatencyEpoch(&m_Device->Pump);   /* first: a restart in between snaps on the next call */
        latencyUs = PumpPipelineLatencyUs(&m_Device->Pump) + DeviceSinkDelayUs(m_Device);
    }
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    /* plus the frames the stream holds back for fades */
    if (m_Format.Format.nSamplesPerSec) latencyUs += (ULONG)((ULONGLONG)m_TailFrames * 1000000u / m_Format.Format.nSamplesPerSec);
    /* the position is interpolated to exactly this QPC (wave_math.h) */
    LARGE_INTEGER qpc = KeQueryPerformanceCounter(NULL);
    PnWavePresentationSetEpoch(&m_Presentation, epoch);   /* review #7 */
    ULONGLONG frames = PnWavePresentationFrames(&m_Presentation, m_LinearPosition / PN_MAX(m_Format.Format.nBlockAlign, 1u), m_LastTickQpc,
                                                m_State == KSSTATE_RUN && !m_Eos && !m_Holding, (ULONGLONG)qpc.QuadPart, (ULONGLONG)m_QpcFrequency.QuadPart,
                                                m_Format.Format.nSamplesPerSec, latencyUs);
    KeReleaseSpinLock(&m_Lock, irql);
    Pos->u64PositionInBlocks = frames;
    Pos->u64QPCPosition = (ULONGLONG)qpc.QuadPart;
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(PASSIVE_LEVEL)   /* as IMP_IMiniportWaveRTOutputStream declares it */
STDMETHODIMP_(NTSTATUS) CPnMiniportWaveRTStream::GetPacketCount(_Out_ ULONG *PacketCount)
{
    if (m_Notifications == 0) return STATUS_NOT_SUPPORTED;
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    *PacketCount = (ULONG)m_PacketCounter;
    KeReleaseSpinLock(&m_Lock, irql);
    return STATUS_SUCCESS;
}
