/*
 * pump.cpp - streaming engine: PCM ring -> encoder (kernel SBC or codec host) -> RTP -> L2CAP.
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"
#include "pcm.h"
#include "audio_policy.h"

/* ntifs.h declares it; this driver builds against wdm.h only */
extern "C" NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(_In_ HANDLE Handle, _In_ BOOLEAN Alertable, _In_opt_ PLARGE_INTEGER Timeout);

/* ------------------------------------------------------------------------- */
/* PCM ring                                                                     */
/* ------------------------------------------------------------------------- */

/* Caller holds R->Lock. */
static ULONG RingAvailableLocked(_In_ PPN_PCM_RING R)
{
    return (ULONG)(R->WritePos - R->ReadPos);
}

static ULONG RingAvailable(_In_ PPN_PCM_RING R)
{
    KIRQL irql;
    KeAcquireSpinLock(&R->Lock, &irql);
    ULONG available = RingAvailableLocked(R);
    KeReleaseSpinLock(&R->Lock, irql);
    return available;
}

static ULONG RingRead(_Inout_ PPN_PCM_RING R, _Out_writes_bytes_(Len) PUCHAR Out, _In_ ULONG Len)
{
    KIRQL irql;
    KeAcquireSpinLock(&R->Lock, &irql);
    if (!R->Buffer || !R->Capacity) { KeReleaseSpinLock(&R->Lock, irql); return 0; }
    ULONG avail = RingAvailableLocked(R);
    if (Len > avail) Len = avail;
    ULONG off = (ULONG)(R->ReadPos % R->Capacity);
    ULONG first = PN_MIN(Len, R->Capacity - off);
    RtlCopyMemory(Out, R->Buffer + off, first);
    if (Len > first) RtlCopyMemory(Out + first, R->Buffer, Len - first);
    R->ReadPos += Len;
    KeReleaseSpinLock(&R->Lock, irql);
    return Len;
}

VOID PumpWritePcm(_Inout_ PPN_PUMP P, _In_reads_bytes_(Length) const UCHAR *Data, _In_ ULONG Length)
{
    PPN_PCM_RING R = &P->Ring;
    if (!R->Buffer || !Length) return;
    KIRQL irql;
    KeAcquireSpinLock(&R->Lock, &irql);
    /* re-check under the lock: PumpReleaseInput may have run since the unlocked test */
    if (!R->Buffer || !R->Capacity || !R->BytesPerFrame || R->Capacity <= R->BytesPerFrame) { KeReleaseSpinLock(&R->Lock, irql); return; }
    if (Length >= R->Capacity) {
        Data += Length - R->Capacity + R->BytesPerFrame;
        Length = R->Capacity - R->BytesPerFrame;
    }
    ULONG avail = RingAvailableLocked(R);
    if (avail + Length > R->Capacity) {
        /* overrun: drop the oldest PCM, keep frame alignment */
        ULONG drop = avail + Length - R->Capacity;
        drop = ((drop + R->BytesPerFrame - 1) / R->BytesPerFrame) * R->BytesPerFrame;
        R->ReadPos += drop;
        R->DroppedBytes += drop;
    }
    ULONG off = (ULONG)(R->WritePos % R->Capacity);
    ULONG first = PN_MIN(Length, R->Capacity - off);
    RtlCopyMemory(R->Buffer + off, Data, first);
    if (Length > first) RtlCopyMemory(R->Buffer, Data + first, Length - first);
    R->WritePos += Length;
    KeReleaseSpinLock(&R->Lock, irql);
    if (P->StreamActive) KeSetEvent(&P->WakeEvent, IO_SOUND_INCREMENT, FALSE);
}

VOID PumpFlush(_Inout_ PPN_PUMP P)
{
    KIRQL irql;
    KeAcquireSpinLock(&P->Ring.Lock, &irql);
    P->Ring.ReadPos = P->Ring.WritePos;
    KeReleaseSpinLock(&P->Ring.Lock, irql);
}

static ULONG RingLatencyUs(_In_ PPN_PUMP P)
{
    PPN_PCM_RING R = &P->Ring;
    KIRQL irql;
    KeAcquireSpinLock(&R->Lock, &irql);
    ULONG latency = 0;
    if (R->Buffer && R->BytesPerFrame && R->SampleRate) {
        ULONGLONG frames = RingAvailableLocked(R) / R->BytesPerFrame;
        latency = (ULONG)(frames * 1000000ull / R->SampleRate);
    }
    KeReleaseSpinLock(&R->Lock, irql);
    return latency;
}

NTSTATUS PumpConfigureInput(_Inout_ PPN_PUMP P, _In_ ULONG SampleRate, _In_ ULONG Channels, _In_ ULONG ContainerBits, _In_ ULONG ValidBits, _In_ BOOLEAN IsFloat)
{
    PAGED_CODE();
    if (!SampleRate || SampleRate > 192000 || !Channels || Channels > 2 ||
        (ContainerBits != 16 && ContainerBits != 24 && ContainerBits != 32) || !ValidBits || ValidBits > ContainerBits ||
        (IsFloat && (ContainerBits != 32 || ValidBits != 32))) return STATUS_INVALID_PARAMETER;
    PumpStopStream(P);   /* the thread must not be converting with the old format */
    PumpReleaseInput(P);
    PPN_PCM_RING R = &P->Ring;
    R->BytesPerFrame = Channels * (ContainerBits / 8);
    R->SampleRate = SampleRate;
    R->Channels = Channels;
    R->BitsPerSample = ContainerBits;
    R->ValidBits = ValidBits;
    R->IsFloat = IsFloat;
    ULONG cap = (ULONG)((ULONGLONG)SampleRate * PN_PUMP_RING_MS / 1000) * R->BytesPerFrame;
    cap = ((cap + 4095) / 4096) * 4096;
    cap -= cap % R->BytesPerFrame;
    R->Buffer = (PUCHAR)PnAllocNp(cap, PNA2DP_TAG_PCM);
    if (!R->Buffer) return STATUS_INSUFFICIENT_RESOURCES;
    R->Capacity = cap;
    R->ReadPos = R->WritePos = 0;
    R->DroppedBytes = 0;
    PN_INFO("pump input %u Hz %u ch %u/%u bit%s, ring %u bytes", SampleRate, Channels, ValidBits, ContainerBits, IsFloat ? " float" : "", cap);
    return STATUS_SUCCESS;
}

VOID PumpReleaseInput(_Inout_ PPN_PUMP P)
{
    PAGED_CODE();
    KIRQL irql;
    PUCHAR old;
    KeAcquireSpinLock(&P->Ring.Lock, &irql);
    old = P->Ring.Buffer;
    P->Ring.Buffer = NULL;
    P->Ring.Capacity = 0;
    P->Ring.ReadPos = P->Ring.WritePos = 0;
    KeReleaseSpinLock(&P->Ring.Lock, irql);
    PnFree(old, PNA2DP_TAG_PCM);
}

/* ------------------------------------------------------------------------- */
/* PCM conversion to encoder input                                              */
/* ------------------------------------------------------------------------- */

/* Converts `Frames` interleaved frames from the ring format to Target format with gain. */
static ULONG ConvertPcm(_In_ PPN_PUMP P, _In_reads_bytes_(InBytes) const UCHAR *In, _In_ ULONG InBytes, _Out_writes_bytes_(OutCap) PUCHAR Out, _In_ ULONG OutCap,
                        _In_ ULONG Target, _In_ ULONG TargetChannels)
{
    PPN_PCM_RING R = &P->Ring;
    /* Digital mute is immediate. Gain is unity while Windows controls the
     * receiver's absolute volume, or the local fallback gain otherwise. */
    ULONG gain = P->Muted ? 0 : P->VolumeGainQ15;
    return PnConvertPcm(In, InBytes, R->Channels, R->BitsPerSample, R->ValidBits, R->IsFloat,
                        Out, OutCap, Target, TargetChannels, gain);
}
/* ------------------------------------------------------------------------- */
/* Transport                                                                    */
/* ------------------------------------------------------------------------- */

static BOOLEAN CodecUsesRtp(ULONG Codec)
{
    switch (Codec) {
    case PNA2DP_CODEC_APTX:
    case PNA2DP_CODEC_APTX_LL:
    case PNA2DP_CODEC_FASTSTREAM:
        return FALSE;
    default:
        return TRUE;
    }
}

static BOOLEAN CodecUsesPayloadHeader(ULONG Codec)
{
    switch (Codec) {
    case PNA2DP_CODEC_SBC:
    case PNA2DP_CODEC_LDAC:
        return TRUE;
    default:
        return FALSE;
    }
}

/* Send one media packet made of `FrameCount` codec frames in Payload. */
static NTSTATUS SendMediaPacket(_Inout_ PPN_PUMP P, _In_reads_bytes_(PayloadBytes) const UCHAR *Payload, _In_ ULONG PayloadBytes,
                                _In_ ULONG FrameCount, _In_ ULONG PcmFrames, _In_ ULONG PayloadHeaderByte)
{
    PPN_L2CAP_CHANNEL ch = P->Media;
    if (!ch || !PnChannelIsConnected(ch)) return STATUS_CONNECTION_DISCONNECTED;
    ULONG timestamp = P->Rtp.Timestamp;
    ULONG header = (P->UseRtpHeader ? RTP_HEADER_SIZE : 0) + (P->UsePayloadHeader ? 1 : 0);
    ULONG capacity = PN_MIN(P->MediaMtu, ch->WriteCapacity);
    if (capacity <= header || !PayloadBytes || PayloadBytes > PN_PUMP_MAX_PACKET) return STATUS_INVALID_PARAMETER;
    ULONG room = capacity - header;
    ULONG count = (PayloadBytes + room - 1) / room;
    if ((count > 1 && P->Cfg.Codec != PNA2DP_CODEC_AAC) || count > PN_L2CAP_MAX_WRITES) {
        P->Rtp.Timestamp += PcmFrames;
        P->Stats.PacketsDropped++;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (!PnMediaQueueHasCapacity((ULONG)PnChannelWritesInFlight(ch), count, P->MaxWritesInFlight)) {
        P->Rtp.Timestamp += PcmFrames;
        P->Stats.PacketsDropped++;
        InterlockedIncrement(&P->Abr.Pressure);
        return STATUS_DEVICE_BUSY;
    }
    // Reserve all writes before sending the first fragment. AAC LATM fragments share
    // a timestamp; the final fragment has the RTP marker bit (RFC 3016 section 4.3).
    PPN_L2CAP_WRITE writes[PN_L2CAP_MAX_WRITES] = {};
    for (ULONG i = 0; i < count; ++i) {
        writes[i] = PnChannelAcquireWrite(ch);
        if (!writes[i]) {
            for (ULONG j = 0; j < i; ++j) PnChannelReleaseWrite(ch, writes[j]);
            P->Rtp.Timestamp += PcmFrames;
            P->Stats.PacketsDropped++;
            return STATUS_DEVICE_BUSY;
        }
    }
    NTSTATUS status = STATUS_SUCCESS;
    ULONG offset = 0;
    for (ULONG i = 0; i < count; ++i) {
        PPN_L2CAP_WRITE w = writes[i];
        ULONG bytes = PN_MIN(room, PayloadBytes - offset);
        BOOLEAN last = i + 1 == count;
        if (P->UseRtpHeader) RtpWriteHeader(&P->Rtp, w->Buffer, last ? PcmFrames : 0,
                                           P->Cfg.Codec == PNA2DP_CODEC_AAC && last);
        if (P->UsePayloadHeader) w->Buffer[header - 1] = (UCHAR)(PayloadHeaderByte ? PayloadHeaderByte : (FrameCount & 0x0f));
        RtlCopyMemory(w->Buffer + header, Payload + offset, bytes);
        ULONG length = w->Length = header + bytes;
        status = PnChannelSubmitWrite(ch, w);
        // The completion may already have recycled w, so use the saved length.
        if (NT_SUCCESS(status)) {
            P->Stats.PacketsSent++;
            P->Stats.BytesSent += length;
        } else {
            P->Stats.WriteFailures++;
            InterlockedIncrement(&P->Abr.Failed);
            for (ULONG j = i + 1; j < count; ++j) PnChannelReleaseWrite(ch, writes[j]);
            break;
        }
        offset += bytes;
    }
    P->Rtp.Timestamp = timestamp + PcmFrames; // preserve media time even after a dropped fragment
    if (NT_SUCCESS(status)) P->Stats.EncodedFrames += FrameCount;
    ULONG inflight = (ULONG)PnChannelWritesInFlight(ch);
    P->Stats.QueueDepthPackets = inflight;
    P->Stats.QueueDepthMaxPackets = PN_MAX(P->Stats.QueueDepthMaxPackets, inflight);
    return status;
}

VOID PumpOnWriteDone(_Inout_ PPN_PUMP P, _In_ NTSTATUS Status, _In_ ULONG Bytes, _In_ LONGLONG SubmitTimeHns)
{
    LONGLONG dt = PnNowHns() - SubmitTimeHns;
    ULONG us = (ULONG)(dt / 10);
    P->TxAvgUs = (P->TxAvgUs * 15 + us) / 16;
    if (us > P->TxMaxUs) P->TxMaxUs = us;
    P->Stats.TxCompletionAvgUs = P->TxAvgUs;
    P->Stats.TxCompletionMaxUs = P->TxMaxUs;
    if (!NT_SUCCESS(Status)) {
        P->Stats.WriteFailures++;
        InterlockedIncrement(&P->Abr.Failed);
    }
    InterlockedIncrement(&P->Abr.Completed);
    /* Use the actual write size: LDAC packets and fragmented AAC do not have a
     * constant number of PCM frames. Completion time is host transport latency,
     * not RSSI or an over-the-air retry count. */
    ULONG packetUs = P->Cfg.BitrateBps ? (ULONG)((ULONGLONG)Bytes * 8000000ull / P->Cfg.BitrateBps) : 20000;
    if (us > packetUs * 2 + 10000) {
        InterlockedIncrement(&P->Abr.Slow);
    }
    if (P->StreamActive) KeSetEvent(&P->WakeEvent, IO_SOUND_INCREMENT, FALSE);
}

/* ------------------------------------------------------------------------- */
/* ABR                                                                          */
/* ------------------------------------------------------------------------- */

static ULONG AbrBitrateAtLevel(_In_ PPN_PUMP P, ULONG Level, _Out_opt_ A2DP_CODEC_CONFIG *Result)
{
    A2DP_CODEC_CONFIG base = P->Cfg;
    base.LdacEqmid = (UCHAR)P->LdacBaseQuality;
    return A2dpQualityAtLevel(&base, Level, P->Abr.FloorBps, Result);
}

static VOID AbrApplyLevel(_Inout_ PPN_PUMP P)
{
    AbrBitrateAtLevel(P, P->Abr.Level, &P->Cfg);
    if (P->Cfg.Codec == PNA2DP_CODEC_SBC) {
        sbc_encoder_set_bitpool(&P->Sbc, P->Cfg.SbcBitpool);
        P->CodecFrameBytes = P->Sbc.frame_length;
    }
    P->Stats.AbrLevel = P->Abr.Level;
    PN_INFO("ABR level %u -> %u bps", P->Abr.Level, P->Cfg.BitrateBps);
}

static VOID AbrTick(_Inout_ PPN_PUMP P)
{
    LONGLONG now = PnNowHns();
    ULONG elapsed = (ULONG)PN_MIN((now - P->Abr.LastObservationHns) / PN_HNS_PER_MS, 60000ll);
    P->Abr.LastObservationHns = now;
    ULONG completed = (ULONG)InterlockedExchange(&P->Abr.Completed, 0);
    ULONG slow = (ULONG)InterlockedExchange(&P->Abr.Slow, 0);
    ULONG failed = (ULONG)InterlockedExchange(&P->Abr.Failed, 0);
    ULONG pressure = (ULONG)InterlockedExchange(&P->Abr.Pressure, 0);
    ULONG backlog = P->Abr.BacklogPeakUs;
    P->Abr.BacklogPeakUs = 0;
    BOOLEAN bad = failed || pressure || backlog > P->TargetLatencyMs * 1000 ||
        (slow >= 2 && (ULONGLONG)slow * 10 >= completed);
    int change = PnQualityObserve(&P->Abr.Window, elapsed, completed != 0, bad,
        P->Abr.Enabled, P->Abr.Level, P->Abr.MaxLevel);
    if (change > 0) {
        P->Abr.Level++;
        P->Abr.Downgrades++;
        P->Stats.AbrDowngrades++;
        AbrApplyLevel(P);
        DeviceNotifyEvent(P->Device, PNA2DP_EVENT_STATE_CHANGED);
    } else if (change < 0) {
        P->Abr.Level--;
        P->Abr.Upgrades++;
        P->Stats.AbrUpgrades++;
        AbrApplyLevel(P);
        DeviceNotifyEvent(P->Device, PNA2DP_EVENT_STATE_CHANGED);
    }
    PNA2DP_QUALITY q = {};
    q.Size = sizeof(q);
    q.Flags = P->Abr.Enabled ? PNA2DP_QUALITY_ABR : 0;
    q.Health = bad ? PNA2DP_QUALITY_CONGESTED : !completed ? PNA2DP_QUALITY_IDLE :
        P->Abr.Level ? PNA2DP_QUALITY_RECOVERING : P->Abr.Window.GoodMs >= 3000 ? PNA2DP_QUALITY_STABLE : PNA2DP_QUALITY_SAMPLING;
    q.Codec = P->Cfg.Codec; q.BitrateBps = P->Cfg.BitrateBps;
    q.CeilingBps = AbrBitrateAtLevel(P, 0, NULL); q.FloorBps = AbrBitrateAtLevel(P, P->Abr.MaxLevel, NULL);
    q.Level = P->Abr.Level; q.MaxLevel = P->Abr.MaxLevel;
    q.HealthyMs = P->Abr.Window.GoodMs; q.CongestedMs = P->Abr.Window.BadMs;
    q.CompletedPackets = completed; q.SlowPackets = slow; q.FailedPackets = failed;
    q.BacklogPeakUs = backlog; q.ObservationMs = elapsed;
    KIRQL irql;
    KeAcquireSpinLock(&P->Lock, &irql);
    P->Quality = q;
    KeReleaseSpinLock(&P->Lock, irql);
}

/* ------------------------------------------------------------------------- */
/* Encoding paths                                                               */
/* ------------------------------------------------------------------------- */

/* Kernel SBC: pull PCM, encode N frames, send one packet. Returns TRUE if a packet was sent. */
static BOOLEAN PumpStepSbc(_Inout_ PPN_PUMP P, _Inout_ PUCHAR PcmBuf, _In_ ULONG PcmCap, _Inout_ PUCHAR PktBuf, _In_ ULONG PktCap)
{
    /* Retain PCM during a temporary controller stall. Consuming and encoding it
     * before discovering a full write queue creates avoidable holes in music.
     * The existing latency guard still bounds a prolonged stall. */
    if (!P->Media || !PnChannelIsConnected(P->Media)) return FALSE;
    if (!PnMediaQueueHasCapacity((ULONG)PnChannelWritesInFlight(P->Media), 1, P->MaxWritesInFlight)) {
        P->Stats.TransportWaits++;
        return FALSE;
    }
    ULONG framesPerPacket = P->CodecFramesPerPacket;
    ULONG pcmFrames = framesPerPacket * P->PcmFramesPerCodecFrame;
    ULONG needBytes = pcmFrames * P->Ring.BytesPerFrame;
    if (needBytes > PcmCap) return FALSE;
    if (RingAvailable(&P->Ring) < needBytes) return FALSE;

    ULONG got = RingRead(&P->Ring, PcmBuf, needBytes);
    if (got < needBytes) return FALSE;
    P->Stats.PcmFramesConsumed += pcmFrames;

    ULONG channels = P->Sbc.channels;
    ULONG pcmPerFrame = P->PcmFramesPerCodecFrame;
    ULONG convBytes = ConvertPcm(P, PcmBuf, got, P->ConvBuffer, P->ConvCapacity, PNA2DP_FMT_S16, channels);
    if (convBytes < pcmFrames * channels * sizeof(SHORT)) return FALSE;
    const SHORT *s16 = (const SHORT *)P->ConvBuffer;
    ULONG n = 0, count = 0;
    for (ULONG f = 0; f < framesPerPacket; f++) {
        ULONG len = sbc_encode_frame(&P->Sbc, s16 + f * pcmPerFrame * channels, PktBuf + n, PktCap - n);
        if (!len) break;
        n += len;
        count++;
    }
    if (!count) return FALSE;
    if (P->SampleState == 1 && n <= sizeof(P->Sample.Sbc) && convBytes <= sizeof(P->Sample.Pcm)) {
        KIRQL irql;
        KeAcquireSpinLock(&P->Lock, &irql);
        if (P->SampleState == 1) {
            RtlZeroMemory(&P->Sample, sizeof(P->Sample));
            P->Sample.Size = sizeof(P->Sample);
            P->Sample.SampleRate = P->Cfg.SampleRate; P->Sample.Channels = channels;
            P->Sample.GainQ15 = P->VolumeGainQ15; P->Sample.Muted = P->Muted;
            P->Sample.PcmBytes = count * pcmPerFrame * channels * sizeof(SHORT); P->Sample.SbcBytes = n;
            RtlCopyMemory(P->Sample.Pcm, s16, P->Sample.PcmBytes);
            RtlCopyMemory(P->Sample.Sbc, PktBuf, n);
            P->SampleState = 2;
        }
        KeReleaseSpinLock(&P->Lock, irql);
    }
    SendMediaPacket(P, PktBuf, n, count, count * P->PcmFramesPerCodecFrame, count & 0x0F);
    return TRUE;
}

/* Host path: ship a PCM block to the host (encode request). */
static BOOLEAN PumpStepHostSubmit(_Inout_ PPN_PUMP P, _Inout_ PUCHAR PcmBuf, _In_ ULONG PcmCap)
{
    if (P->HostOutstanding >= PN_PUMP_HOST_WINDOW) return FALSE;
    if (P->HostResultCount >= PN_PUMP_HOST_WINDOW) return FALSE;
    ULONG pcmFrames = P->CodecFramesPerPacket * P->PcmFramesPerCodecFrame;
    ULONG needBytes = pcmFrames * P->Ring.BytesPerFrame;
    if (needBytes > PcmCap) return FALSE;
    if (RingAvailable(&P->Ring) < needBytes) return FALSE;
    ULONG got = RingRead(&P->Ring, PcmBuf, needBytes);
    if (got < needBytes) return FALSE;
    P->Stats.PcmFramesConsumed += pcmFrames;
    ULONG convBytes = ConvertPcm(P, PcmBuf, got, P->ConvBuffer, P->ConvCapacity, P->EncoderPcmFormat, P->Cfg.Channels);

    PNA2DP_CODEC_WORK hdr;
    RtlZeroMemory(&hdr, sizeof(hdr));
    hdr.Kind = PNA2DP_WORK_ENCODE;
    hdr.Sequence = P->HostNextSequence++;
    hdr.StreamGeneration = (ULONG)P->Generation;
    hdr.Params.Codec = P->Cfg.Codec;
    hdr.Params.SampleRate = P->Cfg.SampleRate;
    hdr.Params.Channels = P->Cfg.Channels;
    hdr.Params.PcmFormat = P->EncoderPcmFormat;
    hdr.Params.ChannelMode = P->Cfg.ChannelMode;
    hdr.Params.BitrateBps = P->Cfg.BitrateBps;
    hdr.Params.MediaMtu = P->MediaMtu - (P->UseRtpHeader ? RTP_HEADER_SIZE : 0) - (P->UsePayloadHeader ? 1 : 0);
    hdr.Params.QualityIndex = (P->Cfg.Codec == PNA2DP_CODEC_LDAC) ? P->Cfg.LdacEqmid : (P->Cfg.Codec == PNA2DP_CODEC_SBC) ? P->Cfg.SbcBitpool : P->Abr.Level;
    hdr.Params.CodecInfoLength = P->Cfg.IesLength;
    RtlCopyMemory(hdr.Params.CodecInfo, P->Cfg.Ies, P->Cfg.IesLength);
    hdr.PcmFrames = pcmFrames;
    /* count it before the host can see it: its result may arrive before Dispatch returns */
    InterlockedIncrement(&P->HostOutstanding);
    P->HostLastSubmitHns = PnNowHns();
    NTSTATUS status = CodecHostDispatchWork(&P->Device->Host, &hdr, P->ConvBuffer, convBytes);
    if (!NT_SUCCESS(status)) {
        if (InterlockedDecrement(&P->HostOutstanding) < 0) InterlockedExchange(&P->HostOutstanding, 0);
        P->HostTimeouts++;
        P->Stats.HostTimeouts++;
        return FALSE;
    }
    return TRUE;
}

/* Host path: drain results in order. */
static BOOLEAN PumpStepHostDrain(_Inout_ PPN_PUMP P)
{
    KIRQL irql;
    PPN_ENCODED_BLOCK b = NULL;
    KeAcquireSpinLock(&P->HostLock, &irql);
    if (!IsListEmpty(&P->HostResults)) {
        PPN_ENCODED_BLOCK first = CONTAINING_RECORD(P->HostResults.Flink, PN_ENCODED_BLOCK, Link);
        ULONG header = (P->UseRtpHeader ? RTP_HEADER_SIZE : 0) + (P->UsePayloadHeader ? 1 : 0);
        ULONG capacity = P->Media ? PN_MIN(P->MediaMtu, P->Media->WriteCapacity) : 0;
        ULONG room = capacity > header ? capacity - header : 0;
        ULONG count = room ? (first->PayloadBytes + room - 1) / room : 0;
        /* Malformed/oversize output must reach SendMediaPacket's rejection path,
         * rather than waiting forever for an impossible number of write slots. */
        if (first->PayloadBytes && count && count <= P->MaxWritesInFlight &&
            (!P->Media || !PnChannelIsConnected(P->Media) ||
            !PnMediaQueueHasCapacity((ULONG)PnChannelWritesInFlight(P->Media), count, P->MaxWritesInFlight))) {
            KeReleaseSpinLock(&P->HostLock, irql);
            P->Stats.TransportWaits++;
            return FALSE;
        }
        b = CONTAINING_RECORD(RemoveHeadList(&P->HostResults), PN_ENCODED_BLOCK, Link);
        P->HostResultCount--;
    }
    KeReleaseSpinLock(&P->HostLock, irql);
    if (!b) return FALSE;
    if (b->PayloadBytes) {
        SendMediaPacket(P, b->Payload, b->PayloadBytes, b->FrameCount, b->PcmFrames, b->PayloadHeaderByte);
    }
    PnFree(b, PNA2DP_TAG_CODEC);
    return TRUE;
}

NTSTATUS PumpHostResult(_Inout_ PPN_PUMP P, _In_ const PNA2DP_CODEC_RESULT *R, _In_reads_bytes_(R->PayloadBytes) const UCHAR *Payload)
{
    if (!P->StreamActive || P->Owner != PnEncoderHost || R->StreamGeneration != (ULONG)P->Generation) return STATUS_SUCCESS;   /* stale */
    if (R->Status != 0) {
        P->Stats.HostTimeouts++;
        CodecHostReportFailure(&P->Device->Host);
        return STATUS_SUCCESS;
    }
    if (R->PayloadBytes > PN_PUMP_MAX_PACKET) return STATUS_BUFFER_OVERFLOW;
    if (P->HostResultCount >= 2 * PN_PUMP_HOST_WINDOW) return STATUS_DEVICE_BUSY;
    PPN_ENCODED_BLOCK b = (PPN_ENCODED_BLOCK)PnAllocNp(sizeof(*b), PNA2DP_TAG_CODEC);
    if (!b) return STATUS_INSUFFICIENT_RESOURCES;
    b->Sequence = R->Sequence;
    b->PayloadBytes = R->PayloadBytes;
    b->FrameCount = R->FrameCount;
    b->PcmFrames = R->PcmFramesConsumed;
    b->PayloadHeaderByte = R->PayloadHeaderByte;
    b->ActualBitrate = R->ActualBitrateBps;
    if (R->PayloadBytes) RtlCopyMemory(b->Payload, Payload, R->PayloadBytes);
    KIRQL irql;
    BOOLEAN queued = FALSE;
    KeAcquireSpinLock(&P->HostLock, &irql);
    /* PumpStopStream bumps Generation before it drains under this lock */
    if (P->StreamActive && R->StreamGeneration == (ULONG)P->Generation && P->HostResultCount < 2 * PN_PUMP_HOST_WINDOW) {
        InsertTailList(&P->HostResults, &b->Link);
        P->HostResultCount++;
        queued = TRUE;
    }
    KeReleaseSpinLock(&P->HostLock, irql);
    if (!queued) { PnFree(b, PNA2DP_TAG_CODEC); return STATUS_SUCCESS; }
    if (InterlockedDecrement(&P->HostOutstanding) < 0) InterlockedExchange(&P->HostOutstanding, 0);
    LONGLONG rt = PnNowHns() - P->HostLastSubmitHns;
    P->HostRoundTripAvgUs = (P->HostRoundTripAvgUs * 7 + (ULONG)(rt / 10)) / 8;
    P->Stats.HostRoundTripAvgUs = P->HostRoundTripAvgUs;
    /* Results may have been encoded before an ABR change. Never feed their old
     * bitrate back into the desired configuration: it would undo AAC reductions. */
    KeSetEvent(&P->WakeEvent, IO_SOUND_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

VOID PumpHostLost(_Inout_ PPN_PUMP P)
{
    if (P->Owner == PnEncoderHost && P->StreamActive) {
        PN_WARN("codec host lost while streaming; stream will be re-negotiated");
        P->Stats.HostTimeouts++;
        DeviceNotifyEvent(P->Device, PNA2DP_EVENT_HOST_CHANGED);
    }
}

/* ------------------------------------------------------------------------- */
/* Thread                                                                       */
/* ------------------------------------------------------------------------- */

static VOID PumpThread(_In_ PVOID Context)
{
    PPN_PUMP P = (PPN_PUMP)Context;
    PUCHAR pcmBuf = (PUCHAR)PnAllocNp(PN_PUMP_MAX_PCM_BLOCK, PNA2DP_TAG_PCM);
    PUCHAR pktBuf = (PUCHAR)PnAllocNp(PN_PUMP_MAX_PACKET, PNA2DP_TAG_PCM);
    LARGE_INTEGER timeout;
    LONGLONG lastAbr = PnNowHns();

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY + 2);   /* 18: above normal, below audio engine's 24 */

    while (P->Running) {
        /* 4 ms tick while streaming; otherwise sleep until PumpStartStream/Uninitialize */
        timeout.QuadPart = -PN_HNS_PER_MS * 4;
        KeWaitForSingleObject(&P->WakeEvent, Executive, KernelMode, FALSE, P->StreamActive ? &timeout : NULL);
        if (!P->Running) break;
        /* Busy from here: PumpStopStream clears StreamActive and then waits for Idle, so it
         * never changes the stream state while a step is in progress. */
        KeClearEvent(&P->Idle);
        KeMemoryBarrier();
        if (!P->StreamActive || !pcmBuf || !pktBuf) { KeSetEvent(&P->Idle, 0, FALSE); continue; }

        LONG bp = InterlockedExchange(&P->PendingBitpool, 0);
        if (bp && P->Cfg.Codec == PNA2DP_CODEC_SBC) {
            P->Cfg.SbcMaxBitpool = (UCHAR)PN_MIN(PN_MAX((ULONG)bp, (ULONG)P->Cfg.SbcMinBitpool), P->SbcBitpoolCeiling);
            P->Abr.MaxLevel = 24;
            while (P->Abr.MaxLevel && AbrBitrateAtLevel(P, P->Abr.MaxLevel, NULL) == AbrBitrateAtLevel(P, P->Abr.MaxLevel - 1, NULL)) P->Abr.MaxLevel--;
            P->Abr.Level = PN_MIN(P->Abr.Level, P->Abr.MaxLevel);
            AbrApplyLevel(P); // keep the user's ceiling across subsequent ABR changes
        }
        if (InterlockedExchange(&P->PendingAbrReset, 0) && P->Abr.Level) {
            P->Abr.Level = 0;
            AbrApplyLevel(P);
        }

        /* latency guard: drop PCM beyond MaxLatencyMs so we never lag behind */
        ULONG latUs = RingLatencyUs(P);
        P->Abr.BacklogPeakUs = PN_MAX(P->Abr.BacklogPeakUs, latUs);
        if (latUs > P->MaxLatencyMs * 1000) {
            ULONG keepBytes = (ULONG)((ULONGLONG)P->Ring.SampleRate * P->TargetLatencyMs / 1000) * P->Ring.BytesPerFrame;
            KIRQL irql;
            KeAcquireSpinLock(&P->Ring.Lock, &irql);
            ULONG avail = RingAvailableLocked(&P->Ring);
            if (avail > keepBytes) {
                ULONG drop = avail - keepBytes;
                drop = (drop / P->Ring.BytesPerFrame) * P->Ring.BytesPerFrame;
                P->Ring.ReadPos += drop;
                P->Ring.DroppedBytes += drop;
            }
            KeReleaseSpinLock(&P->Ring.Lock, irql);
            P->Stats.Underruns++;   /* reported as latency-trim events */
            InterlockedIncrement(&P->Abr.Pressure);
        }

        ULONG guard = 0;
        if (P->Owner == PnEncoderKernelSbc) {
            while (guard++ < 64 && P->StreamActive && PumpStepSbc(P, pcmBuf, PN_PUMP_MAX_PCM_BLOCK, pktBuf, PN_PUMP_MAX_PACKET)) {}
        } else if (P->Owner == PnEncoderHost) {
            while (guard++ < 64 && P->StreamActive && (PumpStepHostDrain(P) | PumpStepHostSubmit(P, pcmBuf, PN_PUMP_MAX_PCM_BLOCK))) {}
            /* host watchdog */
            if (P->HostOutstanding > 0 && PnNowHns() - P->HostLastSubmitHns > PN_HNS_PER_MS * PN_PUMP_HOST_TIMEOUT_MS) {
                P->HostTimeouts++;
                P->Stats.HostTimeouts++;
                P->HostLastSubmitHns = PnNowHns();
                InterlockedExchange(&P->HostOutstanding, 0);
                if (P->HostTimeouts >= 3) {
                    PN_WARN("codec host unresponsive; requesting fallback");
                    P->HostTimeouts = 0;
                    CodecHostReportFailure(&P->Device->Host);
                }
            }
        }
        LONGLONG now = PnNowHns();
        if (now - lastAbr > PN_HNS_PER_MS * 250) { lastAbr = now; AbrTick(P); }
        KeSetEvent(&P->Idle, 0, FALSE);
    }
    PnFree(pcmBuf, PNA2DP_TAG_PCM);
    PnFree(pktBuf, PNA2DP_TAG_PCM);
    KeSetEvent(&P->Idle, 0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                    */
/* ------------------------------------------------------------------------- */

NTSTATUS PumpInitialize(_Out_ PPN_PUMP P, _In_ PPNA2DP_DEVICE Device)
{
    PAGED_CODE();
    RtlZeroMemory(P, sizeof(*P));
    KeInitializeSpinLock(&P->Lock);
    KeInitializeSpinLock(&P->Ring.Lock);
    KeInitializeSpinLock(&P->HostLock);
    InitializeListHead(&P->HostResults);
    KeInitializeEvent(&P->WakeEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&P->Idle, NotificationEvent, TRUE);
    P->Device = Device;   /* marks the pump initialized for PumpUninitialize */
    P->VolumeGainQ15 = 32768;
    P->TargetLatencyMs = PN_PUMP_DEFAULT_TARGET_MS;
    P->MaxLatencyMs = PN_PUMP_DEFAULT_MAX_MS;
    P->MaxWritesInFlight = 4;
    P->ConvCapacity = PN_PUMP_MAX_PCM_BLOCK * 2;
    P->ConvBuffer = (PUCHAR)PnAllocNp(P->ConvCapacity, PNA2DP_TAG_PCM);
    if (!P->ConvBuffer) return STATUS_INSUFFICIENT_RESOURCES;
    P->Running = 1;
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    NTSTATUS status = PsCreateSystemThread(&P->ThreadHandle, THREAD_ALL_ACCESS, &oa, NULL, NULL, PumpThread, P);
    if (!NT_SUCCESS(status)) { P->Running = 0; P->ThreadHandle = NULL; return status; }
    status = ObReferenceObjectByHandle(P->ThreadHandle, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&P->Thread, NULL);
    if (!NT_SUCCESS(status)) {
        /* cannot wait on the object later: stop the thread now through its handle */
        P->Thread = NULL;
        P->Running = 0;
        KeSetEvent(&P->WakeEvent, 0, FALSE);
        ZwWaitForSingleObject(P->ThreadHandle, FALSE, NULL);
        ZwClose(P->ThreadHandle);
        P->ThreadHandle = NULL;
        return status;
    }
    return STATUS_SUCCESS;
}

static VOID PumpDrainHostResults(_Inout_ PPN_PUMP P)
{
    KIRQL irql;
    LIST_ENTRY drained;
    InitializeListHead(&drained);
    KeAcquireSpinLock(&P->HostLock, &irql);
    while (!IsListEmpty(&P->HostResults)) InsertTailList(&drained, RemoveHeadList(&P->HostResults));
    P->HostResultCount = 0;
    KeReleaseSpinLock(&P->HostLock, irql);
    while (!IsListEmpty(&drained)) PnFree(CONTAINING_RECORD(RemoveHeadList(&drained), PN_ENCODED_BLOCK, Link), PNA2DP_TAG_CODEC);
    InterlockedExchange(&P->HostOutstanding, 0);
}

VOID PumpUninitialize(_Inout_ PPN_PUMP P)
{
    PAGED_CODE();
    if (!P->Device) return;
    PumpStopStream(P);
    if (P->Running) {
        P->Running = 0;
        KeSetEvent(&P->WakeEvent, 0, FALSE);
        if (P->Thread) {
            KeWaitForSingleObject(P->Thread, Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject(P->Thread);
            P->Thread = NULL;
        }
        if (P->ThreadHandle) { ZwClose(P->ThreadHandle); P->ThreadHandle = NULL; }
    }
    PumpDrainHostResults(P);
    PumpReleaseInput(P);
    PnFree(P->ConvBuffer, PNA2DP_TAG_PCM);
    P->ConvBuffer = NULL;
}

NTSTATUS PumpStartStream(_Inout_ PPN_PUMP P, _In_ const A2DP_CODEC_CONFIG *Cfg, _In_ PPN_L2CAP_CHANNEL Media, _In_ ULONG MediaMtu,
                         _In_ PN_ENCODER_OWNER Owner, _In_ const PNA2DP_CONFIG *User)
{
    PAGED_CODE();
    PumpStopStream(P);
    if (!P->Running || !P->ConvBuffer || !P->Ring.Buffer || !P->Ring.BytesPerFrame) return STATUS_INVALID_DEVICE_STATE;
    if (!Cfg->SampleRate || !Cfg->Channels || !Media) return STATUS_INVALID_PARAMETER;
    // No sample-rate converter exists in this path. Never play 48 kHz PCM at 44.1 kHz.
    if (Cfg->SampleRate != P->Ring.SampleRate) return STATUS_NOT_SUPPORTED;

    P->Cfg = *Cfg;
    P->Media = Media;
    if (MediaMtu < 48) return STATUS_INVALID_PARAMETER;
    P->MediaMtu = PN_MIN(MediaMtu, PN_PUMP_MAX_PACKET);
    P->Owner = Owner;
    P->TargetLatencyMs = User->TargetLatencyMs;
    P->MaxLatencyMs = User->MaxLatencyMs;
    P->UseRtpHeader = CodecUsesRtp(Cfg->Codec);
    P->UsePayloadHeader = CodecUsesPayloadHeader(Cfg->Codec);
    P->Abr.Enabled = (User->Flags & PNA2DP_CFG_ABR_ENABLE) != 0 &&
        (Cfg->Codec != PNA2DP_CODEC_LDAC || User->Ldac.AbrEnable != 0);
    P->Abr.Level = 0;
    RtlZeroMemory(&P->Abr.Window, sizeof(P->Abr.Window));
    InterlockedExchange(&P->Abr.Completed, 0);
    InterlockedExchange(&P->Abr.Slow, 0);
    InterlockedExchange(&P->Abr.Failed, 0);
    InterlockedExchange(&P->Abr.Pressure, 0);
    P->Abr.BacklogPeakUs = 0;
    P->Abr.FloorBps = User->AbrFloorBps;
    P->Abr.LastObservationHns = PnNowHns();
    InterlockedIncrement(&P->Generation);
    InterlockedExchange(&P->HostOutstanding, 0);
    P->HostNextSequence = 1;
    P->HostTimeouts = 0;
    RtpInit(&P->Rtp, 96);
    PumpFlush(P);

    ULONG payloadRoom = P->MediaMtu - (P->UseRtpHeader ? RTP_HEADER_SIZE : 0) - (P->UsePayloadHeader ? 1 : 0);

    switch (Cfg->Codec) {
    case PNA2DP_CODEC_SBC: {
        sbc_params sp;
        sp.sample_rate = Cfg->SampleRate;
        sp.channel_mode = (sbc_u8)(Cfg->ChannelMode == PNA2DP_CHMODE_MONO ? SBC_MODE_MONO : Cfg->ChannelMode == PNA2DP_CHMODE_DUAL ? SBC_MODE_DUAL :
                                   Cfg->ChannelMode == PNA2DP_CHMODE_STEREO ? SBC_MODE_STEREO : SBC_MODE_JOINT);
        sp.allocation = Cfg->SbcAlloc;
        sp.subbands = Cfg->SbcSubbands;
        sp.blocks = Cfg->SbcBlocks;
        sp.bitpool = Cfg->SbcBitpool ? Cfg->SbcBitpool : Cfg->SbcMaxBitpool;
        /* keep one frame within the MTU */
        sbc_u8 maxbp = sbc_max_bitpool_for(&sp, 0, payloadRoom);
        if (maxbp < Cfg->SbcMinBitpool) return STATUS_NOT_SUPPORTED;
        if (sp.bitpool > maxbp) sp.bitpool = maxbp;
        if (sbc_encoder_init(&P->Sbc, &sp) != 0) return STATUS_INVALID_PARAMETER;
        P->Cfg.SbcBitpool = sp.bitpool;
        P->Cfg.SbcMaxBitpool = PN_MIN(P->Cfg.SbcMaxBitpool, maxbp);
        P->SbcBitpoolCeiling = P->Cfg.SbcMaxBitpool;
        P->PcmFramesPerCodecFrame = P->Sbc.pcm_per_frame;
        P->CodecFrameBytes = P->Sbc.frame_length;
        P->EncoderPcmFormat = PNA2DP_FMT_S16;
        P->Abr.MaxLevel = 24;
        /* frames per packet: fill the MTU but stay under ~target/3 ms per packet and 15 frames */
        ULONG fitMtu = payloadRoom / PN_MAX(P->CodecFrameBytes, 1u);
        ULONG frameUs = P->PcmFramesPerCodecFrame * 1000000u / Cfg->SampleRate;
        ULONG fitTime = PN_MAX((P->TargetLatencyMs * 1000u / 3u) / PN_MAX(frameUs, 1u), 1u);
        P->CodecFramesPerPacket = PN_MAX(1u, PN_MIN(PN_MIN(fitMtu, fitTime), 15u));
        if (Owner != PnEncoderKernelSbc) Owner = P->Owner = PnEncoderKernelSbc;
        break;
    }
    case PNA2DP_CODEC_AAC:
        P->PcmFramesPerCodecFrame = 1024;
        P->CodecFramesPerPacket = 1;
        P->EncoderPcmFormat = PNA2DP_FMT_S16;
        P->Abr.MaxLevel = 4;
        break;
    case PNA2DP_CODEC_APTX:
    case PNA2DP_CODEC_APTX_LL:
        P->PcmFramesPerCodecFrame = 4;      /* one aptX sample block = 4 PCM frames -> 8 bytes */
        P->CodecFramesPerPacket = PN_MIN(payloadRoom / 8, 160u);   /* 160 blocks = 640 frames = ~13.3 ms at 48 k */
        P->EncoderPcmFormat = PNA2DP_FMT_S24;
        P->Abr.MaxLevel = 0;
        break;
    case PNA2DP_CODEC_APTX_HD:
        P->PcmFramesPerCodecFrame = 4;      /* 12 bytes per block */
        P->CodecFramesPerPacket = PN_MIN(payloadRoom / 12, 80u);
        P->EncoderPcmFormat = PNA2DP_FMT_S24;
        P->Abr.MaxLevel = 0;
        break;
    case PNA2DP_CODEC_LDAC:
        P->PcmFramesPerCodecFrame = (Cfg->SampleRate >= 88200) ? 256 : 128;
        P->CodecFramesPerPacket = 2;   /* blocks of 2 LDAC frames: the plugin packs per MTU and never queues more than one packet per call */
        P->EncoderPcmFormat = Cfg->PcmFormat ? Cfg->PcmFormat : PNA2DP_FMT_S32;
        P->LdacBaseQuality = PnLdacAbrQuality(Cfg->LdacEqmid, 0);
        P->Abr.MaxLevel = 2 - P->LdacBaseQuality;
        break;
    case PNA2DP_CODEC_OPUS:
        P->PcmFramesPerCodecFrame = 480;    /* 10 ms at 48 k */
        P->CodecFramesPerPacket = 2;
        P->EncoderPcmFormat = PNA2DP_FMT_S16;
        P->Abr.MaxLevel = 3;
        break;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    P->Cfg.BitrateBps = A2dpNominalBitrate(&P->Cfg);
    /* Do not count reductions which produce the same bitrate (floor reached or
     * a receiver ceiling below the next AAC rung). Compact the bottom plateau. */
    while (P->Abr.MaxLevel && AbrBitrateAtLevel(P, P->Abr.MaxLevel, NULL) == AbrBitrateAtLevel(P, P->Abr.MaxLevel - 1, NULL)) P->Abr.MaxLevel--;
    if (P->Owner == PnEncoderHost) {
        /* OPEN request to the host */
        PNA2DP_CODEC_WORK hdr;
        RtlZeroMemory(&hdr, sizeof(hdr));
        hdr.Kind = PNA2DP_WORK_OPEN;
        hdr.Sequence = 0;
        hdr.StreamGeneration = (ULONG)P->Generation;
        hdr.Params.Codec = P->Cfg.Codec;
        hdr.Params.SampleRate = P->Cfg.SampleRate;
        hdr.Params.Channels = P->Cfg.Channels;
        hdr.Params.PcmFormat = P->EncoderPcmFormat;
        hdr.Params.ChannelMode = P->Cfg.ChannelMode;
        hdr.Params.BitrateBps = P->Cfg.BitrateBps;
        hdr.Params.MediaMtu = payloadRoom;
        hdr.Params.QualityIndex = (P->Cfg.Codec == PNA2DP_CODEC_LDAC) ? P->Cfg.LdacEqmid : 0;
        hdr.Params.CodecInfoLength = P->Cfg.IesLength;
        RtlCopyMemory(hdr.Params.CodecInfo, P->Cfg.Ies, P->Cfg.IesLength);
        NTSTATUS status = CodecHostDispatchWork(&P->Device->Host, &hdr, NULL, 0);
        if (!NT_SUCCESS(status)) {
            PN_WARN("codec host not ready for OPEN (0x%08x)", status);
            return status;
        }
    }
    PN_INFO("pump start: %s %u Hz ch %u bitrate %u, %u frames/packet, mtu %u, owner %u", A2dpCodecName(Cfg->Codec), Cfg->SampleRate,
            Cfg->Channels, P->Cfg.BitrateBps, P->CodecFramesPerPacket, P->MediaMtu, P->Owner);
    AbrTick(P); // publish this stream's codec/range before callers observe it
    InterlockedExchange(&P->StreamActive, 1);
    KeSetEvent(&P->WakeEvent, 0, FALSE);
    return STATUS_SUCCESS;
}

VOID PumpStopStream(_Inout_ PPN_PUMP P)
{
    PAGED_CODE();
    if (InterlockedExchange(&P->StreamActive, 0) == 0) {
        if (P->Device) PumpDrainHostResults(P);   /* results that raced an earlier stop */
        return;
    }
    /* Wait until the thread has left its step batch; after this nothing reads the encoder,
     * the ring or the media channel until the next PumpStartStream. */
    KeSetEvent(&P->WakeEvent, 0, FALSE);
    if (P->Thread && KeGetCurrentThread() != P->Thread) KeWaitForSingleObject(&P->Idle, Executive, KernelMode, FALSE, NULL);
    InterlockedIncrement(&P->Generation);
    InterlockedExchange(&P->PendingBitpool, 0);
    InterlockedExchange(&P->PendingAbrReset, 0);
    if (P->Owner == PnEncoderHost) {
        PNA2DP_CODEC_WORK hdr;
        RtlZeroMemory(&hdr, sizeof(hdr));
        hdr.Kind = PNA2DP_WORK_CLOSE;
        hdr.StreamGeneration = (ULONG)P->Generation;
        CodecHostDispatchWork(&P->Device->Host, &hdr, NULL, 0);
    }
    PumpDrainHostResults(P);
    KIRQL sampleIrql;
    KeAcquireSpinLock(&P->Lock, &sampleIrql);
    P->SampleState = 0;
    RtlZeroMemory(&P->Sample, sizeof(P->Sample));
    KeReleaseSpinLock(&P->Lock, sampleIrql);
    P->Media = NULL;
    P->Owner = PnEncoderNone;
    PumpFlush(P);
}

BOOLEAN PumpIsStreaming(_In_ PPN_PUMP P) { return P->StreamActive != 0; }

VOID PumpSetVolume(_Inout_ PPN_PUMP P, _In_ LONG VolumeDb16, _In_ BOOLEAN Muted)
{
    P->VolumeDb16 = VolumeDb16;
    P->Muted = Muted;
    P->VolumeGainQ15 = PnVolumeGainQ15(VolumeDb16);
}

/* The encoder belongs to the pump thread while streaming: hand the change over. */
VOID PumpSetSbcBitpool(_Inout_ PPN_PUMP P, _In_ UCHAR Bitpool)
{
    if (P->Cfg.Codec != PNA2DP_CODEC_SBC || !P->StreamActive || !Bitpool) return;
    InterlockedExchange(&P->PendingBitpool, Bitpool);
    KeSetEvent(&P->WakeEvent, 0, FALSE);
}

VOID PumpSetAbr(_Inout_ PPN_PUMP P, _In_ BOOLEAN Enable)
{
    P->Abr.Enabled = Enable;
    if (Enable || !P->Abr.Level) return;
    if (P->StreamActive) {
        InterlockedExchange(&P->PendingAbrReset, 1);
        KeSetEvent(&P->WakeEvent, 0, FALSE);
    } else {
        P->Abr.Level = 0;   /* PumpStartStream sets up the encoder from scratch */
    }
}

VOID PumpGetStats(_In_ PPN_PUMP P, _Out_ PPNA2DP_STATS Stats)
{
    *Stats = P->Stats;
    Stats->Size = sizeof(*Stats);
    Stats->QueueDepthPackets = P->Media ? (ULONG)PnChannelWritesInFlight(P->Media) : 0;
    Stats->AbrLevel = P->Abr.Level;
}

VOID PumpGetQuality(_In_ PPN_PUMP P, _Out_ PNA2DP_QUALITY *Quality)
{
    KIRQL irql;
    KeAcquireSpinLock(&P->Lock, &irql);
    *Quality = P->Quality;
    if (!P->StreamActive) {
        Quality->Health = PNA2DP_QUALITY_IDLE;
        Quality->HealthyMs = Quality->CongestedMs = 0;
        Quality->CompletedPackets = Quality->SlowPackets = Quality->FailedPackets = 0;
        Quality->BacklogPeakUs = Quality->ObservationMs = 0;
        Quality->BitrateBps = 0;
    }
    Quality->Size = sizeof(*Quality);
    KeReleaseSpinLock(&P->Lock, irql);
}

VOID PumpResetStats(_Inout_ PPN_PUMP P)
{
    RtlZeroMemory(&P->Stats, sizeof(P->Stats));
    P->TxMaxUs = 0;
}

NTSTATUS PumpSbcSample(_Inout_ PPN_PUMP P, _In_ ULONG Action, _Out_opt_ PNA2DP_SBC_SAMPLE *Sample)
{
    KIRQL irql;
    KeAcquireSpinLock(&P->Lock, &irql);
    NTSTATUS status = STATUS_SUCCESS;
    if (Action > 2 || (!Action && !Sample)) status = STATUS_INVALID_PARAMETER;
    else if (Action == 2) { RtlZeroMemory(&P->Sample, sizeof(P->Sample)); P->SampleState = 0; }
    else if (Action == 1 && (!P->StreamActive || P->Owner != PnEncoderKernelSbc)) status = STATUS_INVALID_DEVICE_STATE;
    else if (Action == 1) { RtlZeroMemory(&P->Sample, sizeof(P->Sample)); P->SampleState = 1; }
    else if (P->SampleState != 2) status = STATUS_DEVICE_NOT_READY;
    else { *Sample = P->Sample; RtlZeroMemory(&P->Sample, sizeof(P->Sample)); P->SampleState = 0; }
    KeReleaseSpinLock(&P->Lock, irql);
    return status;
}

ULONG PumpCurrentBitrate(_In_ PPN_PUMP P) { return P->StreamActive ? P->Cfg.BitrateBps : 0; }

ULONG PumpEstimatedLatencyUs(_In_ PPN_PUMP P)
{
    ULONG us = RingLatencyUs(P);
    /* in-flight packets */
    if (P->Media && P->Cfg.SampleRate) {
        ULONG packetUs = P->CodecFramesPerPacket * P->PcmFramesPerCodecFrame * 1000000u / P->Cfg.SampleRate;
        us += (ULONG)PnChannelWritesInFlight(P->Media) * packetUs;
    }
    us += P->TxAvgUs;
    return us;
}
