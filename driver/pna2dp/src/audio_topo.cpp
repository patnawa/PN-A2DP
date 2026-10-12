/*
 * audio_topo.cpp - topology miniport: master volume, mute and receiver endpoint.
 *
 * Windows endpoint volume and mute land here. The Bluetooth service can bridge
 * these controls to AVRCP; otherwise the pump applies the gain locally.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"
#include "audio_policy.h"

/* ---- filter descriptor ------------------------------------------------------ */

static KSDATARANGE g_TopoBridgeRange[] = {
    { sizeof(KSDATARANGE), 0, 0, 0, STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG), STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE) }
};
static PKSDATARANGE g_TopoBridgeRanges[] = { &g_TopoBridgeRange[0] };

static PCPIN_DESCRIPTOR g_TopoPins[] = {
    /* PN_TOPO_PIN_WAVEIN: from the wave filter */
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, SIZEOF_ARRAY(g_TopoBridgeRanges), g_TopoBridgeRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, NULL, 0 } },
    /* PN_TOPO_PIN_LINEOUT: category is selected per receiver at Init. */
    { 0, 0, 0, NULL, { 0, NULL, 0, NULL, SIZEOF_ARRAY(g_TopoBridgeRanges), g_TopoBridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_SPEAKER, NULL, 0 } },
};

static PCPROPERTY_ITEM g_TopoVolumeProps[] = {
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_VOLUMELEVEL, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, CPnMiniportTopology::PropertyHandlerTopoNode },
};
static PCPROPERTY_ITEM g_TopoMuteProps[] = {
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_MUTE, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, CPnMiniportTopology::PropertyHandlerTopoNode },
};
static PCEVENT_ITEM g_TopoNodeEvents[] = {
    { &KSEVENTSETID_AudioControlChange, KSEVENT_CONTROL_CHANGE, KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT, CPnMiniportTopology::EventHandlerControlChange },
};
DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(g_TopoVolumeAutomation, g_TopoVolumeProps, g_TopoNodeEvents);
DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(g_TopoMuteAutomation, g_TopoMuteProps, g_TopoNodeEvents);

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
    { &PN_KSPROPSETID_BluetoothVolume, 1, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, CPnMiniportTopology::PropertyHandlerTopoFilter },
    { &KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, CPnMiniportTopology::PropertyHandlerTopoFilter },
    { &KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION2, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, CPnMiniportTopology::PropertyHandlerTopoFilter },
};
static PCEVENT_ITEM g_TopoFilterEvents[] = {
    { &PN_KSPROPSETID_BluetoothVolume, 1, KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT, CPnMiniportTopology::EventHandlerControlChange },
    { &KSEVENTSETID_AudioControlChange, KSEVENT_CONTROL_CHANGE, KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT, CPnMiniportTopology::EventHandlerControlChange },
};
DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(g_TopoFilterAutomation, g_TopoFilterProps, g_TopoFilterEvents);

/* BthAvrcp watches this interface class on the AudioSink device instance. PortCls
 * publishes it with the Topology reference and owns its PnP lifetime. */
static GUID g_TopoCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO), STATICGUIDOF(KSCATEGORY_TOPOLOGY),
    { 0xe630310c, 0x1733, 0x42bb, { 0x98, 0x47, 0x7e, 0x45, 0xd1, 0xa2, 0x74, 0x90 } },
};

PCFILTER_DESCRIPTOR g_PnTopoFilterDescriptor = {
    0, &g_TopoFilterAutomation,
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(g_TopoPins), g_TopoPins,
    sizeof(PCNODE_DESCRIPTOR), SIZEOF_ARRAY(g_TopoNodes), g_TopoNodes,
    SIZEOF_ARRAY(g_TopoConnections), g_TopoConnections,
    SIZEOF_ARRAY(g_TopoCategories), g_TopoCategories
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
CPnMiniportTopology::CPnMiniportTopology(_In_opt_ PUNKNOWN UnknownOuter) : CUnknown(UnknownOuter), m_Adapter(NULL), m_Port(NULL), m_PortEvents(NULL), m_RemoteListeners(0)
{
    PAGED_CODE();
    m_Filter = g_PnTopoFilterDescriptor;
    RtlCopyMemory(m_Pins, g_TopoPins, sizeof(m_Pins));
    m_Filter.Pins = m_Pins;
}

#pragma code_seg("PAGE")
CPnMiniportTopology::~CPnMiniportTopology()
{
    PAGED_CODE();
    if (m_Adapter) m_Adapter->GetDevice()->Adapter->SetTopologyMiniport(NULL);
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
    m_Pins[PN_TOPO_PIN_LINEOUT].KsPinDescriptor.Category =
        PnEndpointIsHeadphones(m_Adapter->GetDevice()->Link.RemoteCod) ? &KSNODETYPE_HEADPHONES : &KSNODETYPE_SPEAKER;
    Port->QueryInterface(IID_IPortEvents, (PVOID *)&m_PortEvents);
    m_Adapter->GetDevice()->Adapter->SetTopologyMiniport(this);
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CPnMiniportTopology::GetDescription(_Out_ PPCFILTER_DESCRIPTOR *Description)
{
    PAGED_CODE();
    *Description = &m_Filter;
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
    if (IsEqualGUIDAligned(*Req->PropertyItem->Set, PN_KSPROPSETID_BluetoothVolume)) return t->HandleRemoteVolume(Req);
    return t->HandleJack(Req);
}

NTSTATUS CPnMiniportTopology::HandleRemoteVolume(_In_ PPCPROPERTY_REQUEST Req)
{
    PAGED_CODE();
    if (Req->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
        return PnPropertyBasicSupport(Req, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, VT_I4);
    if (Req->ValueSize < sizeof(LONG)) { Req->ValueSize = sizeof(LONG); return STATUS_BUFFER_TOO_SMALL; }
    LONG *value = (LONG *)Req->Value;
    if (Req->Verb & KSPROPERTY_TYPE_GET) {
        LONG db = m_Adapter->GetVolume(0);
        *value = m_Adapter->GetMute(0) || db < PN_REMOTE_VOLUME_MIN_DB16 ? PN_REMOTE_VOLUME_MIN_DB16 : db;
        Req->ValueSize = sizeof(LONG);
        return STATUS_SUCCESS;
    }
    if (Req->Verb & KSPROPERTY_TYPE_SET) {
        if (*value < PN_REMOTE_VOLUME_MIN_DB16 || *value > 0) return STATUS_INVALID_PARAMETER;
        m_Adapter->GetDevice()->Adapter->SetRemoteVolume(*value);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS CPnMiniportTopology::EventHandlerControlChange(_In_ PPCEVENT_REQUEST Req)
{
    PAGED_CODE();
    CPnMiniportTopology *t = (CPnMiniportTopology *)Req->MajorTarget;
    if (!t || !t->m_PortEvents) return STATUS_INVALID_PARAMETER;
    BOOLEAN remote = IsEqualGUIDAligned(*Req->EventItem->Set, PN_KSPROPSETID_BluetoothVolume);
    switch (Req->Verb) {
    case PCEVENT_VERB_SUPPORT: return STATUS_SUCCESS;
    case PCEVENT_VERB_ADD:
        if (!Req->EventEntry) return STATUS_INVALID_PARAMETER;
        if (remote) InterlockedIncrement(&t->m_RemoteListeners);
        t->m_PortEvents->AddEventToEventList(Req->EventEntry);
        if (remote) PN_INFO("Windows Bluetooth volume listener attached");
        return STATUS_SUCCESS;
    case PCEVENT_VERB_REMOVE:
        if (remote && InterlockedDecrement(&t->m_RemoteListeners) == 0)
            t->m_Adapter->GetDevice()->Adapter->DisableRemoteVolume();
        return STATUS_SUCCESS;
    default: return STATUS_INVALID_PARAMETER;
    }
}

#pragma code_seg("PAGE")
VOID CPnMiniportTopology::GenerateVolumeEvent()
{
    PAGED_CODE();
    if (m_PortEvents) m_PortEvents->GenerateEventList((GUID *)&KSEVENTSETID_AudioControlChange, KSEVENT_CONTROL_CHANGE, FALSE, ULONG(-1), TRUE, PN_TOPO_NODE_VOLUME);
    if (m_PortEvents) m_PortEvents->GenerateEventList((GUID *)&KSEVENTSETID_AudioControlChange, KSEVENT_CONTROL_CHANGE, FALSE, ULONG(-1), TRUE, PN_TOPO_NODE_MUTE);
}

VOID CPnMiniportTopology::GenerateRemoteVolumeEvent()
{
    PAGED_CODE();
    if (m_PortEvents) m_PortEvents->GenerateEventList((GUID *)&PN_KSPROPSETID_BluetoothVolume, 1, FALSE, ULONG(-1), FALSE, ULONG(-1));
}
#pragma code_seg()
