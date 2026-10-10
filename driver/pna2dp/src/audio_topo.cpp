/*
 * audio_topo.cpp - topology miniport: master volume + mute nodes and a headphone jack.
 *
 * Volume changes made by the user (or by the AVRCP service when the headset reports
 * EVENT_VOLUME_CHANGED) land here. By default the sink applies the gain through AVRCP
 * absolute volume; the pump applies digital gain only when PNA2DP_CFG_SOFTWARE_VOLUME is set.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"

/* ---- filter descriptor ------------------------------------------------------ */

static KSDATARANGE g_TopoBridgeRange[] = {
    { sizeof(KSDATARANGE), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE) }
};
static PKSDATARANGE g_TopoBridgeRanges[] = { &g_TopoBridgeRange[0] };

static PCPIN_DESCRIPTOR g_TopoPins[] = {
    /* PN_TOPO_PIN_WAVEIN: from the wave filter */
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, SIZEOF_ARRAY(g_TopoBridgeRanges), g_TopoBridgeRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, NULL, 0 } },
    /* PN_TOPO_PIN_LINEOUT: the headset */
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, SIZEOF_ARRAY(g_TopoBridgeRanges), g_TopoBridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_HEADPHONES, NULL, 0 } },
};

static PCPROPERTY_ITEM g_TopoVolumeProps[] = {
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_VOLUMELEVEL, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, CPnMiniportTopology::PropertyHandlerTopoNode },
};
static PCPROPERTY_ITEM g_TopoMuteProps[] = {
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_MUTE, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, CPnMiniportTopology::PropertyHandlerTopoNode },
};
DEFINE_PCAUTOMATION_TABLE_PROP(g_TopoVolumeAutomation, g_TopoVolumeProps);
DEFINE_PCAUTOMATION_TABLE_PROP(g_TopoMuteAutomation, g_TopoMuteProps);

static PCNODE_DESCRIPTOR g_TopoNodes[] = {
    { 0, &g_TopoVolumeAutomation, &KSNODETYPE_VOLUME, &KSAUDFNAME_MASTER_VOLUME },
    { 0, &g_TopoMuteAutomation, &KSNODETYPE_MUTE, &KSAUDFNAME_MASTER_MUTE },
};

static PCCONNECTION_DESCRIPTOR g_TopoConnections[] = {
    { PCFILTER_NODE, PN_TOPO_PIN_WAVEIN, PN_TOPO_NODE_VOLUME, 1 },
    { PN_TOPO_NODE_VOLUME, 0, PN_TOPO_NODE_MUTE, 1 },
    { PN_TOPO_NODE_MUTE, 0, PCFILTER_NODE, PN_TOPO_PIN_LINEOUT },
};

static PCPROPERTY_ITEM g_TopoFilterProps[] = {
    { &KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, CPnMiniportTopology::PropertyHandlerTopoFilter },
    { &KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION2, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, CPnMiniportTopology::PropertyHandlerTopoFilter },
};
DEFINE_PCAUTOMATION_TABLE_PROP(g_TopoFilterAutomation, g_TopoFilterProps);

PCFILTER_DESCRIPTOR g_PnTopoFilterDescriptor = {
    0, &g_TopoFilterAutomation,
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(g_TopoPins), g_TopoPins,
    sizeof(PCNODE_DESCRIPTOR), SIZEOF_ARRAY(g_TopoNodes), g_TopoNodes,
    SIZEOF_ARRAY(g_TopoConnections), g_TopoConnections,
    0, NULL
};

static KSJACK_DESCRIPTION g_JackDesc = {
    KSAUDIO_SPEAKER_STEREO, 0x0000FF00 /* green */, eConnTypeOtherDigital, eGeoLocNotApplicable, eGenLocOther, ePortConnJack, TRUE
};

/* ---- shared property helpers -------------------------------------------- */

NTSTATUS PnPropertyBasicSupport(_In_ PPCPROPERTY_REQUEST Req, _In_ ULONG Flags, _In_ DWORD PropTypeSetId)
{
    PAGED_CODE();
    if (Req->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
        PKSPROPERTY_DESCRIPTION d = (PKSPROPERTY_DESCRIPTION)Req->Value;
        d->AccessFlags = Flags;
        d->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
        d->PropTypeSet.Set = KSPROPTYPESETID_General;
        d->PropTypeSet.Id = PropTypeSetId;
        d->PropTypeSet.Flags = 0;
        d->MembersListCount = 0;
        d->Reserved = 0;
        Req->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        return STATUS_SUCCESS;
    }
    if (Req->ValueSize >= sizeof(ULONG)) {
        *(PULONG)Req->Value = Flags;
        Req->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    Req->ValueSize = 0;
    return STATUS_BUFFER_TOO_SMALL;
}

NTSTATUS PnPropertyBasicSupportVolume(_In_ PPCPROPERTY_REQUEST Req)
{
    PAGED_CODE();
    ULONG need = sizeof(KSPROPERTY_DESCRIPTION) + sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_STEPPING_LONG);
    if (Req->ValueSize == 0) { Req->ValueSize = need; return STATUS_BUFFER_OVERFLOW; }
    if (Req->ValueSize < sizeof(KSPROPERTY_DESCRIPTION)) {
        if (Req->ValueSize >= sizeof(ULONG)) { *(PULONG)Req->Value = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT; Req->ValueSize = sizeof(ULONG); return STATUS_SUCCESS; }
        return STATUS_BUFFER_TOO_SMALL;
    }
    PKSPROPERTY_DESCRIPTION d = (PKSPROPERTY_DESCRIPTION)Req->Value;
    d->AccessFlags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT;
    d->DescriptionSize = need;
    d->PropTypeSet.Set = KSPROPTYPESETID_General;
    d->PropTypeSet.Id = VT_I4;
    d->PropTypeSet.Flags = 0;
    d->MembersListCount = 0;
    d->Reserved = 0;
    if (Req->ValueSize >= need) {
        d->MembersListCount = 1;
        PKSPROPERTY_MEMBERSHEADER mh = (PKSPROPERTY_MEMBERSHEADER)(d + 1);
        mh->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
        mh->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
        mh->MembersCount = 1;
        mh->Flags = KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL;
        PKSPROPERTY_STEPPING_LONG s = (PKSPROPERTY_STEPPING_LONG)(mh + 1);
        s->Bounds.SignedMinimum = PN_VOLUME_MIN_DB16;
        s->Bounds.SignedMaximum = PN_VOLUME_MAX_DB16;
        s->SteppingDelta = PN_VOLUME_STEP_DB16;
        s->Reserved = 0;
        Req->ValueSize = need;
    } else {
        Req->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
    }
    return STATUS_SUCCESS;
}

/* ---- miniport ---------------------------------------------------------------- */

#pragma code_seg("PAGE")
NTSTATUS NewPnMiniportTopology(_Out_ PUNKNOWN *Unknown, _In_ PPNADAPTERCOMMON Adapter, _In_ POOL_FLAGS PoolFlags)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Adapter);
    CPnMiniportTopology *p = new (PoolFlags, PNA2DP_TAG_AUDIO) CPnMiniportTopology(NULL);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;
    *Unknown = PUNKNOWN((PMINIPORTTOPOLOGY)p);
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
CPnMiniportTopology::CPnMiniportTopology(_In_opt_ PUNKNOWN UnknownOuter) : CUnknown(UnknownOuter), m_Adapter(NULL), m_Port(NULL), m_PortEvents(NULL)
{
    PAGED_CODE();
}

#pragma code_seg("PAGE")
CPnMiniportTopology::~CPnMiniportTopology()
{
    PAGED_CODE();
    if (m_PortEvents) { m_PortEvents->Release(); m_PortEvents = NULL; }
    if (m_Adapter) { m_Adapter->Release(); m_Adapter = NULL; }
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportTopology::NonDelegatingQueryInterface(_In_ REFIID Interface, _COM_Outptr_ PVOID *Object)
{
    PAGED_CODE();
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) *Object = PVOID(PUNKNOWN(this));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport)) *Object = PVOID(PMINIPORT(this));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology)) *Object = PVOID(PMINIPORTTOPOLOGY(this));
    else *Object = NULL;
    if (*Object) { PUNKNOWN(*Object)->AddRef(); return STATUS_SUCCESS; }
    return STATUS_INVALID_PARAMETER;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportTopology::Init(_In_ PUNKNOWN UnknownAdapter, _In_ PRESOURCELIST ResourceList, _In_ PPORTTOPOLOGY Port)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourceList);
    NTSTATUS status = UnknownAdapter->QueryInterface(IID_IPnAdapterCommon, (PVOID *)&m_Adapter);
    if (!NT_SUCCESS(status)) return status;
    m_Port = Port;
    Port->QueryInterface(IID_IPortEvents, (PVOID *)&m_PortEvents);
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportTopology::GetDescription(_Out_ PPCFILTER_DESCRIPTOR *Description)
{
    PAGED_CODE();
    *Description = &g_PnTopoFilterDescriptor;
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportTopology::DataRangeIntersection(_In_ ULONG PinId, _In_ PKSDATARANGE DataRange, _In_ PKSDATARANGE MatchingDataRange,
                                                                   _In_ ULONG OutputBufferLength, _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
                                                                   _Out_ PULONG ResultantFormatLength)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(PinId); UNREFERENCED_PARAMETER(DataRange); UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength); UNREFERENCED_PARAMETER(ResultantFormat);
    *ResultantFormatLength = 0;
    return STATUS_NOT_IMPLEMENTED;
}

#pragma code_seg("PAGE")
NTSTATUS CPnMiniportTopology::HandleVolume(_In_ PPCPROPERTY_REQUEST Req)
{
    PAGED_CODE();
    if (Req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) return PnPropertyBasicSupportVolume(Req);
    if (Req->InstanceSize < sizeof(ULONG)) return STATUS_INVALID_PARAMETER;
    ULONG channel = *(PULONG)Req->Instance;
    if (Req->ValueSize < sizeof(LONG)) { Req->ValueSize = sizeof(LONG); return STATUS_BUFFER_TOO_SMALL; }
    PLONG v = (PLONG)Req->Value;
    if (Req->Verb & KSPROPERTY_TYPE_GET) {
        *v = m_Adapter->GetVolume(channel == (ULONG)-1 ? 0 : channel);
        Req->ValueSize = sizeof(LONG);
        return STATUS_SUCCESS;
    }
    if (Req->Verb & KSPROPERTY_TYPE_SET) {
        LONG db = *v;
        if (db > PN_VOLUME_MAX_DB16) db = PN_VOLUME_MAX_DB16;
        if (db < PN_VOLUME_MIN_DB16) db = PN_VOLUME_MIN_DB16;
        m_Adapter->SetVolume(channel, db);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

#pragma code_seg("PAGE")
NTSTATUS CPnMiniportTopology::HandleMute(_In_ PPCPROPERTY_REQUEST Req)
{
    PAGED_CODE();
    if (Req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) return PnPropertyBasicSupport(Req, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, VT_BOOL);
    if (Req->InstanceSize < sizeof(ULONG)) return STATUS_INVALID_PARAMETER;
    ULONG channel = *(PULONG)Req->Instance;
    if (Req->ValueSize < sizeof(BOOL)) { Req->ValueSize = sizeof(BOOL); return STATUS_BUFFER_TOO_SMALL; }
    PBOOL v = (PBOOL)Req->Value;
    if (Req->Verb & KSPROPERTY_TYPE_GET) {
        *v = m_Adapter->GetMute(channel == (ULONG)-1 ? 0 : channel);
        Req->ValueSize = sizeof(BOOL);
        return STATUS_SUCCESS;
    }
    if (Req->Verb & KSPROPERTY_TYPE_SET) {
        m_Adapter->SetMute(channel, *v);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

#pragma code_seg("PAGE")
NTSTATUS CPnMiniportTopology::HandleJack(_In_ PPCPROPERTY_REQUEST Req)
{
    PAGED_CODE();
    if (Req->InstanceSize < sizeof(ULONG)) return STATUS_INVALID_PARAMETER;
    ULONG pin = *(PULONG)Req->Instance;
    if (pin != PN_TOPO_PIN_LINEOUT) return STATUS_INVALID_PARAMETER;
    if (Req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) return PnPropertyBasicSupport(Req, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, VT_ILLEGAL);
    if (Req->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION) {
        ULONG need = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION);
        if (Req->ValueSize == 0) { Req->ValueSize = need; return STATUS_BUFFER_OVERFLOW; }
        if (Req->ValueSize < need) return STATUS_BUFFER_TOO_SMALL;
        PKSMULTIPLE_ITEM mi = (PKSMULTIPLE_ITEM)Req->Value;
        mi->Size = need;
        mi->Count = 1;
        RtlCopyMemory(mi + 1, &g_JackDesc, sizeof(g_JackDesc));
        Req->ValueSize = need;
        return STATUS_SUCCESS;
    }
    if (Req->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2) {
        ULONG need = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION2);
        if (Req->ValueSize == 0) { Req->ValueSize = need; return STATUS_BUFFER_OVERFLOW; }
        if (Req->ValueSize < need) return STATUS_BUFFER_TOO_SMALL;
        PKSMULTIPLE_ITEM mi = (PKSMULTIPLE_ITEM)Req->Value;
        mi->Size = need;
        mi->Count = 1;
        PKSJACK_DESCRIPTION2 d = (PKSJACK_DESCRIPTION2)(mi + 1);
        d->DeviceStateInfo = 0;
        d->JackCapabilities = JACKDESC2_PRESENCE_DETECT_CAPABILITY;
        Req->ValueSize = need;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

#pragma code_seg("PAGE")
NTSTATUS CPnMiniportTopology::PropertyHandlerTopoNode(_In_ PPCPROPERTY_REQUEST Req)
{
    PAGED_CODE();
    CPnMiniportTopology *t = (CPnMiniportTopology *)Req->MajorTarget;
    if (!t) return STATUS_INVALID_PARAMETER;
    if (Req->Node == PN_TOPO_NODE_VOLUME && Req->PropertyItem->Id == KSPROPERTY_AUDIO_VOLUMELEVEL) return t->HandleVolume(Req);
    if (Req->Node == PN_TOPO_NODE_MUTE && Req->PropertyItem->Id == KSPROPERTY_AUDIO_MUTE) return t->HandleMute(Req);
    return STATUS_INVALID_DEVICE_REQUEST;
}

#pragma code_seg("PAGE")
NTSTATUS CPnMiniportTopology::PropertyHandlerTopoFilter(_In_ PPCPROPERTY_REQUEST Req)
{
    PAGED_CODE();
    CPnMiniportTopology *t = (CPnMiniportTopology *)Req->MajorTarget;
    if (!t) return STATUS_INVALID_PARAMETER;
    return t->HandleJack(Req);
}

#pragma code_seg("PAGE")
VOID CPnMiniportTopology::GenerateVolumeEvent()
{
    PAGED_CODE();
    if (m_PortEvents) m_PortEvents->GenerateEventList((GUID *)&KSEVENTSETID_AudioControlChange, KSEVENT_CONTROL_CHANGE, FALSE, ULONG(-1), TRUE, PN_TOPO_NODE_VOLUME);
}
#pragma code_seg()
