/*
 * pump.cpp - streaming engine: PCM ring -> encoder (kernel SBC or codec host) -> RTP -> L2CAP.
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"

/* ------------------------------------------------------------------------- */
/* PCM ring                                                                     */
/* ------------------------------------------------------------------------- */

static ULONG RingAvailable(_In_ PPN_PCM_RING R)
{
    return (ULONG)(R->WritePos - R->ReadPos);
}

static ULONG RingRead(_Inout_ PPN_PCM_RING R, _Out_writes_bytes_(Len) PUCHAR Out, _In_ ULONG Len)
{
    KIRQL irql;
    KeAcquireSpinLock(&R->Lock, &irql);
    ULONG avail = RingAvailable(R);
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
    if (Length >= R->Capacity) {
        Data += Length - R->Capacity + R->BytesPerFrame;
        Length = R->Capacity - R->BytesPerFrame;
    }
    ULONG avail = RingAvailable(R);
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
    if (!R->Buffer || !R->BytesPerFrame || !R->SampleRate) return 0;
    ULONGLONG frames = RingAvailable(R) / R->BytesPerFrame;
    return (ULONG)(frames * 1000000ull / R->SampleRate);
}

NTSTATUS PumpConfigureInput(_Inout_ PPN_PUMP P, _In_ ULONG SampleRate, _In_ ULONG Channels, _In_ ULONG ContainerBits, _In_ ULONG ValidBits, _In_ BOOLEAN IsFloat)
{
    PAGED_CODE();
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
    ULONG inBps = R->BitsPerSample / 8;
    ULONG frames = InBytes / R->BytesPerFrame;
    ULONG outBps = (Target == PNA2DP_FMT_S16) ? 2 : (Target == PNA2DP_FMT_S24) ? 3 : 4;
    if (frames * TargetChannels * outBps > OutCap) frames = OutCap / (TargetChannels * outBps);
    LONG gain = (LONG)P->VolumeGainQ15;
    BOOLEAN applyGain = P->SoftwareVolume && (gain != 32768 || P->Muted);
    if (P->Muted && P->SoftwareVolume) gain = 0;

    for (ULONG f = 0; f < frames; f++) {
        for (ULONG c = 0; c < TargetChannels; c++) {
            ULONG src = PN_MIN(c, R->Channels - 1);
            const UCHAR *s = In + (f * R->Channels + src) * inBps;
            LONG v32;   /* sample normalised to S32 */
            if (R->IsFloat) {
                float fv = *(const float *)s;
                if (fv > 1.0f) fv = 1.0f; else if (fv < -1.0f) fv = -1.0f;
                v32 = (LONG)(fv * 2147483647.0f);
            } else if (inBps == 2) {
                v32 = (LONG)(*(const SHORT *)s) << 16;
            } else if (inBps == 3) {
                v32 = (LONG)(((ULONG)s[0] << 8) | ((ULONG)s[1] << 16) | ((ULONG)s[2] << 24));
            } else {
                v32 = *(const LONG *)s;
                if (R->ValidBits && R->ValidBits < 32) v32 = (LONG)((ULONG)v32 & ~((1u << (32 - R->ValidBits)) - 1));
            }
            if (applyGain) v32 = (LONG)(((LONGLONG)v32 * gain) >> 15);
            UCHAR *d = Out + (f * TargetChannels + c) * outBps;
            if (Target == PNA2DP_FMT_S16) {
                /* round to 16 bits */
                LONGLONG r = (LONGLONG)v32 + 0x8000; if (r > 0x7FFFFFFF) r = 0x7FFFFFFF;
                *(SHORT *)d = (SHORT)(r >> 16);
            } else if (Target == PNA2DP_FMT_S24) {
                ULONG u = (ULONG)v32 >> 8;
                d[0] = (UCHAR)u; d[1] = (UCHAR)(u >> 8); d[2] = (UCHAR)(u >> 16);
            } else if (Target == PNA2DP_FMT_F32) {
                *(float *)d = (float)v32 / 2147483648.0f;
            } else {
                *(LONG *)d = v32;
            }
        }
    }
    return frames * TargetChannels * outBps;
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

    if (PnChannelWritesInFlight(ch) >= (LONG)P->MaxWritesInFlight) {
        P->Stats.PacketsDropped++;
        P->Abr.CongestionScore += 2;
        P->Abr.LastCongestionHns = PnNowHns();
        return STATUS_DEVICE_BUSY;
    }
    PPN_L2CAP_WRITE w = PnChannelAcquireWrite(ch);
    if (!w) { P->Stats.PacketsDropped++; return STATUS_DEVICE_BUSY; }
    ULONG n = 0;
    if (P->UseRtpHeader) {
        if (w->Capacity < RTP_HEADER_SIZE) { PnChannelReleaseWrite(ch, w); return STATUS_BUFFER_OVERFLOW; }
        RtpWriteHeader(&P->Rtp, w->Buffer, PcmFrames, FALSE);
        n = RTP_HEADER_SIZE;
    }
    if (P->UsePayloadHeader) {
        w->Buffer[n++] = (UCHAR)(PayloadHeaderByte ? PayloadHeaderByte : (FrameCount & 0x0F));
    }
    if (n + PayloadBytes > w->Capacity || n + PayloadBytes > P->MediaMtu) {
        PnChannelReleaseWrite(ch, w);
        P->Stats.PacketsDropped++;
        return STATUS_BUFFER_OVERFLOW;
    }
    RtlCopyMemory(w->Buffer + n, Payload, PayloadBytes);
    w->Length = n + PayloadBytes;
    NTSTATUS status = PnChannelSubmitWrite(ch, w);
    if (NT_SUCCESS(status)) {
        P->Stats.PacketsSent++;
        P->Stats.BytesSent += w->Length;
        P->Stats.EncodedFrames += FrameCount;
        LONG inflight = PnChannelWritesInFlight(ch);
        P->Stats.QueueDepthPackets = (ULONG)inflight;
        if ((ULONG)inflight > P->Stats.QueueDepthMaxPackets) P->Stats.QueueDepthMaxPackets = (ULONG)inflight;
    } else {
        P->Stats.WriteFailures++;
    }
    return status;
}

VOID PumpOnWriteDone(_Inout_ PPN_PUMP P, _In_ NTSTATUS Status, _In_ ULONG Bytes, _In_ LONGLONG SubmitTimeHns)
{
    UNREFERENCED_PARAMETER(Bytes);
    LONGLONG dt = PnNowHns() - SubmitTimeHns;
    ULONG us = (ULONG)(dt / 10);
    P->TxAvgUs = (P->TxAvgUs * 15 + us) / 16;
    if (us > P->TxMaxUs) P->TxMaxUs = us;
    P->Stats.TxCompletionAvgUs = P->TxAvgUs;
    P->Stats.TxCompletionMaxUs = P->TxMaxUs;
    if (!NT_SUCCESS(Status)) {
        P->Stats.WriteFailures++;
    }
    /* Congestion heuristic: a completion that takes longer than ~2 packet durations means the
     * controller's ACL buffers are backing up. */
    ULONG packetUs = P->Cfg.SampleRate ? (P->CodecFramesPerPacket * P->PcmFramesPerCodecFrame * 1000000u / P->Cfg.SampleRate) : 20000;
    if (us > packetUs * 2 + 10000) {
        P->Abr.CongestionScore += 1;
        P->Abr.LastCongestionHns = PnNowHns();
    }
    if (P->StreamActive) KeSetEvent(&P->WakeEvent, IO_SOUND_INCREMENT, FALSE);
}

/* ------------------------------------------------------------------------- */
/* ABR                                                                          */
/* ------------------------------------------------------------------------- */

static VOID AbrApplyLevel(_Inout_ PPN_PUMP P)
{
    ULONG level = P->Abr.Level;
    switch (P->Cfg.Codec) {
    case PNA2DP_CODEC_SBC: {
        /* ladder: max bitpool down to min in steps of ~15% */
        ULONG maxbp = P->Cfg.SbcMaxBitpool, minbp = PN_MAX(P->Cfg.SbcMinBitpool, 2u);
        ULONG bp = maxbp;
        for (ULONG i = 0; i < level; i++) bp = bp - PN_MAX(bp / 7, 2u);
        if (bp < minbp) bp = minbp;
        P->Cfg.SbcBitpool = (UCHAR)bp;
        sbc_encoder_set_bitpool(&P->Sbc, (sbc_u8)bp);
        P->CodecFrameBytes = P->Sbc.frame_length;
        break;
    }
    case PNA2DP_CODEC_LDAC:
        P->Cfg.LdacEqmid = (UCHAR)PN_MIN(level, 2u);
        break;
    case PNA2DP_CODEC_AAC: {
        static const ULONG ladder[] = { 0, 256000, 192000, 128000, 96000 };
        ULONG b = P->Cfg.MaxBitrateBps;
        if (level && level < PN_ARRAYSIZE(ladder)) b = PN_MIN(b, ladder[level]);
        P->Cfg.BitrateBps = b;
        break;
    }
    default:
        break;
    }
    P->Stats.AbrLevel = level;
    P->Cfg.BitrateBps = A2dpNominalBitrate(&P->Cfg);
    PN_INFO("ABR level %u -> %u bps", level, P->Cfg.BitrateBps);
}

static VOID AbrTick(_Inout_ PPN_PUMP P)
{
    if (!P->Abr.Enabled) return;
    LONGLONG now = PnNowHns();
    /* leak */
    if (P->Abr.CongestionScore) P->Abr.CongestionScore--;
    if (P->Abr.CongestionScore > 6 && P->Abr.Level < P->Abr.MaxLevel && now - P->Abr.LastChangeHns > 2 * PN_HNS_PER_SEC) {
        P->Abr.Level++;
        P->Abr.Downgrades++;
        P->Stats.AbrDowngrades++;
        P->Abr.CongestionScore = 0;
        P->Abr.LastChangeHns = now;
        AbrApplyLevel(P);
        DeviceNotifyEvent(P->Device, PNA2DP_EVENT_STATE_CHANGED);
    } else if (P->Abr.Level > 0 && now - P->Abr.LastCongestionHns > 15 * PN_HNS_PER_SEC && now - P->Abr.LastChangeHns > 15 * PN_HNS_PER_SEC) {
        P->Abr.Level--;
        P->Abr.Upgrades++;
        P->Stats.AbrUpgrades++;
        P->Abr.LastChangeHns = now;
        AbrApplyLevel(P);
        DeviceNotifyEvent(P->Device, PNA2DP_EVENT_STATE_CHANGED);
    }
}

/* ------------------------------------------------------------------------- */
/* Encoding paths                                                               */
/* ------------------------------------------------------------------------- */

/* Kernel SBC: pull PCM, encode N frames, send one packet. Returns TRUE if a packet was sent. */
static BOOLEAN PumpStepSbc(_Inout_ PPN_PUMP P, _Inout_ PUCHAR PcmBuf, _In_ ULONG PcmCap, _Inout_ PUCHAR PktBuf, _In_ ULONG PktCap)
{
    ULONG framesPerPacket = P->CodecFramesPerPacket;
    ULONG pcmFrames = framesPerPacket * P->PcmFramesPerCodecFrame;
    ULONG needBytes = pcmFrames * P->Ring.BytesPerFrame;
    if (needBytes > PcmCap) return FALSE;
    if (RingAvailable(&P->Ring) < needBytes) return FALSE;

    ULONG got = RingRead(&P->Ring, PcmBuf, needBytes);
    if (got < needBytes) return FALSE;
    P->Stats.PcmFramesConsumed += pcmFrames;

    ULONG convBytes = ConvertPcm(P, PcmBuf, got, P->ConvBuffer, P->ConvCapacity, PNA2DP_FMT_S16, P->Sbc.channels);
    UNREFERENCED_PARAMETER(convBytes);
    const SHORT *s16 = (const SHORT *)P->ConvBuffer;
    ULONG n = 0, count = 0;
    for (ULONG f = 0; f < framesPerPacket; f++) {
        ULONG len = sbc_encode_frame(&P->Sbc, s16 + f * P->PcmFramesPerCodecFrame * P->Sbc.channels, PktBuf + n, PktCap - n);
        if (!len) break;
        n += len;
        count++;
    }
    if (!count) return FALSE;
    SendMediaPacket(P, PktBuf, n, count, count * P->PcmFramesPerCodecFrame, count & 0x0F);
    return TRUE;
}

/* Host path: ship a PCM block to the host (encode request). */
static BOOLEAN PumpStepHostSubmit(_Inout_ PPN_PUMP P, _Inout_ PUCHAR PcmBuf, _In_ ULONG PcmCap)
{
    if (P->HostOutstanding >= PN_PUMP_HOST_WINDOW) return FALSE;
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
    NTSTATUS status = CodecHostDispatchWork(&P->Device->Host, &hdr, P->ConvBuffer, convBytes);
    if (!NT_SUCCESS(status)) {
        P->HostTimeouts++;
        P->Stats.HostTimeouts++;
        return FALSE;
    }
    InterlockedIncrement(&P->HostOutstanding);
    P->HostLastSubmitHns = PnNowHns();
    return TRUE;
}

/* Host path: drain results in order. */
static BOOLEAN PumpStepHostDrain(_Inout_ PPN_PUMP P)
{
    KIRQL irql;
    PPN_ENCODED_BLOCK b = NULL;
    KeAcquireSpinLock(&P->HostLock, &irql);
    if (!IsListEmpty(&P->HostResults)) b = CONTAINING_RECORD(RemoveHeadList(&P->HostResults), PN_ENCODED_BLOCK, Link);
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
    if (R->StreamGeneration != (ULONG)P->Generation) return STATUS_SUCCESS;   /* stale */
    if (R->PayloadBytes > PN_PUMP_MAX_PACKET) return STATUS_BUFFER_OVERFLOW;
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
    KeAcquireSpinLock(&P->HostLock, &irql);
    InsertTailList(&P->HostResults, &b->Link);
    KeReleaseSpinLock(&P->HostLock, irql);
    InterlockedDecrement(&P->HostOutstanding);
    LONGLONG rt = PnNowHns() - P->HostLastSubmitHns;
    P->HostRoundTripAvgUs = (P->HostRoundTripAvgUs * 7 + (ULONG)(rt / 10)) / 8;
    P->Stats.HostRoundTripAvgUs = P->HostRoundTripAvgUs;
    if (R->ActualBitrateBps) P->Cfg.BitrateBps = R->ActualBitrateBps;
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
        timeout.QuadPart = -PN_HNS_PER_MS * 4;
        KeWaitForSingleObject(&P->WakeEvent, Executive, KernelMode, FALSE, &timeout);
        if (!P->Running) break;
        if (!P->StreamActive || !pcmBuf || !pktBuf) continue;

        /* latency guard: drop PCM beyond MaxLatencyMs so we never lag behind */
        ULONG latUs = RingLatencyUs(P);
        if (latUs > P->MaxLatencyMs * 1000) {
            ULONG keepBytes = (ULONG)((ULONGLONG)P->Ring.SampleRate * P->TargetLatencyMs / 1000) * P->Ring.BytesPerFrame;
            KIRQL irql;
            KeAcquireSpinLock(&P->Ring.Lock, &irql);
            ULONG avail = RingAvailable(&P->Ring);
            if (avail > keepBytes) {
                ULONG drop = avail - keepBytes;
                drop = (drop / P->Ring.BytesPerFrame) * P->Ring.BytesPerFrame;
                P->Ring.ReadPos += drop;
                P->Ring.DroppedBytes += drop;
            }
            KeReleaseSpinLock(&P->Ring.Lock, irql);
            P->Stats.Underruns++;   /* reported as latency-trim events */
            P->Abr.CongestionScore += 3;
            P->Abr.LastCongestionHns = PnNowHns();
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
                if (P->HostTimeouts > 25) {
                    PN_WARN("codec host unresponsive; requesting fallback");
                    P->HostTimeouts = 0;
                    DeviceNotifyEvent(P->Device, PNA2DP_EVENT_HOST_CHANGED);
                }
            }
        }
        LONGLONG now = PnNowHns();
        if (now - lastAbr > PN_HNS_PER_MS * 250) { lastAbr = now; AbrTick(P); }
    }
    PnFree(pcmBuf, PNA2DP_TAG_PCM);
    PnFree(pktBuf, PNA2DP_TAG_PCM);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                    */
/* ------------------------------------------------------------------------- */

NTSTATUS PumpInitialize(_Out_ PPN_PUMP P, _In_ PPNA2DP_DEVICE Device)
{
    PAGED_CODE();
    RtlZeroMemory(P, sizeof(*P));
    P->Device = Device;
    KeInitializeSpinLock(&P->Lock);
    KeInitializeSpinLock(&P->Ring.Lock);
    KeInitializeSpinLock(&P->HostLock);
    InitializeListHead(&P->HostResults);
    KeInitializeEvent(&P->WakeEvent, SynchronizationEvent, FALSE);
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
    if (!NT_SUCCESS(status)) { P->Running = 0; return status; }
    ObReferenceObjectByHandle(P->ThreadHandle, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&P->Thread, NULL);
    return STATUS_SUCCESS;
}

VOID PumpUninitialize(_Inout_ PPN_PUMP P)
{
    PAGED_CODE();
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
    PumpReleaseInput(P);
    PnFree(P->ConvBuffer, PNA2DP_TAG_PCM);
    P->ConvBuffer = NULL;
}

NTSTATUS PumpStartStream(_Inout_ PPN_PUMP P, _In_ const A2DP_CODEC_CONFIG *Cfg, _In_ PPN_L2CAP_CHANNEL Media, _In_ ULONG MediaMtu,
                         _In_ PN_ENCODER_OWNER Owner, _In_ const PNA2DP_CONFIG *User)
{
    PAGED_CODE();
    PumpStopStream(P);
    if (!P->Ring.Buffer) return STATUS_INVALID_DEVICE_STATE;

    P->Cfg = *Cfg;
    P->Media = Media;
    P->MediaMtu = PN_MAX(MediaMtu, 128u);
    P->Owner = Owner;
    P->TargetLatencyMs = User->TargetLatencyMs;
    P->MaxLatencyMs = User->MaxLatencyMs;
    P->SoftwareVolume = (User->Flags & PNA2DP_CFG_SOFTWARE_VOLUME) != 0;
    P->UseRtpHeader = CodecUsesRtp(Cfg->Codec);
    P->UsePayloadHeader = CodecUsesPayloadHeader(Cfg->Codec);
    P->Abr.Enabled = (User->Flags & PNA2DP_CFG_ABR_ENABLE) != 0;
    P->Abr.Level = 0;
    P->Abr.CongestionScore = 0;
    P->Abr.LastChangeHns = PnNowHns();
    P->Abr.LastCongestionHns = 0;
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
        if (sp.bitpool > maxbp) sp.bitpool = maxbp;
        if (sbc_encoder_init(&P->Sbc, &sp) != 0) return STATUS_INVALID_PARAMETER;
        P->Cfg.SbcBitpool = sp.bitpool;
        P->Cfg.SbcMaxBitpool = PN_MIN(P->Cfg.SbcMaxBitpool, maxbp);
        P->PcmFramesPerCodecFrame = P->Sbc.pcm_per_frame;
        P->CodecFrameBytes = P->Sbc.frame_length;
        P->EncoderPcmFormat = PNA2DP_FMT_S16;
        P->Abr.MaxLevel = 6;
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
        P->Abr.MaxLevel = 3;
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
        P->Abr.MaxLevel = 2;
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
    InterlockedExchange(&P->StreamActive, 1);
    KeSetEvent(&P->WakeEvent, 0, FALSE);
    return STATUS_SUCCESS;
}

VOID PumpStopStream(_Inout_ PPN_PUMP P)
{
    PAGED_CODE();
    if (InterlockedExchange(&P->StreamActive, 0) == 0) return;
    /* let the thread observe the flag */
    LARGE_INTEGER wait;
    wait.QuadPart = -PN_HNS_PER_MS * 10;
    KeDelayExecutionThread(KernelMode, FALSE, &wait);
    if (P->Owner == PnEncoderHost) {
        PNA2DP_CODEC_WORK hdr;
        RtlZeroMemory(&hdr, sizeof(hdr));
        hdr.Kind = PNA2DP_WORK_CLOSE;
        hdr.StreamGeneration = (ULONG)P->Generation;
        CodecHostDispatchWork(&P->Device->Host, &hdr, NULL, 0);
    }
    KIRQL irql;
    KeAcquireSpinLock(&P->HostLock, &irql);
    while (!IsListEmpty(&P->HostResults)) {
        PPN_ENCODED_BLOCK b = CONTAINING_RECORD(RemoveHeadList(&P->HostResults), PN_ENCODED_BLOCK, Link);
        PnFree(b, PNA2DP_TAG_CODEC);
    }
    KeReleaseSpinLock(&P->HostLock, irql);
    InterlockedExchange(&P->HostOutstanding, 0);
    P->Media = NULL;
    P->Owner = PnEncoderNone;
    PumpFlush(P);
}

BOOLEAN PumpIsStreaming(_In_ PPN_PUMP P) { return P->StreamActive != 0; }

VOID PumpSetVolume(_Inout_ PPN_PUMP P, _In_ LONG VolumeDb16, _In_ BOOLEAN Muted)
{
    P->VolumeDb16 = VolumeDb16;
    P->Muted = Muted;
    /* linear gain = 10^(dB/20), computed with integer math: 2^(dB * log2(10)/20) = 2^(dB * 0.16610) */
    LONG db = VolumeDb16 >> 16;   /* whole dB (negative) */
    if (db >= 0) { P->VolumeGainQ15 = 32768; return; }
    if (db <= -96) { P->VolumeGainQ15 = 0; return; }
    /* table for 0..-95 dB in Q15 */
    static const USHORT table[96] = {
        32768,29205,26029,23198,20675,18427,16423,14637,13045,11627,10362, 9235, 8231, 7336, 6538, 5827,
         5193, 4628, 4125, 3677, 3277, 2920, 2603, 2320, 2068, 1843, 1642, 1464, 1305, 1163, 1036,  924,
          823,  734,  654,  583,  519,  463,  413,  368,  328,  292,  260,  232,  207,  184,  164,  146,
          131,  116,  104,   92,   82,   73,   65,   58,   52,   46,   41,   37,   33,   29,   26,   23,
           21,   18,   16,   15,   13,   12,   10,    9,    8,    7,    7,    6,    5,    5,    4,    4,
            3,    3,    3,    2,    2,    2,    2,    1,    1,    1,    1,    1,    1,    1,    1,    0 };
    P->VolumeGainQ15 = table[-db];
}

VOID PumpSetSbcBitpool(_Inout_ PPN_PUMP P, _In_ UCHAR Bitpool)
{
    if (P->Cfg.Codec != PNA2DP_CODEC_SBC || !P->StreamActive) return;
    if (Bitpool < P->Cfg.SbcMinBitpool) Bitpool = P->Cfg.SbcMinBitpool;
    if (Bitpool > P->Cfg.SbcMaxBitpool) Bitpool = P->Cfg.SbcMaxBitpool;
    P->Cfg.SbcBitpool = Bitpool;
    sbc_encoder_set_bitpool(&P->Sbc, (sbc_u8)Bitpool);
    P->CodecFrameBytes = P->Sbc.frame_length;
    P->Cfg.BitrateBps = A2dpNominalBitrate(&P->Cfg);
}

VOID PumpSetAbr(_Inout_ PPN_PUMP P, _In_ BOOLEAN Enable)
{
    P->Abr.Enabled = Enable;
    if (!Enable && P->Abr.Level) { P->Abr.Level = 0; AbrApplyLevel(P); }
}

VOID PumpGetStats(_In_ PPN_PUMP P, _Out_ PPNA2DP_STATS Stats)
{
    *Stats = P->Stats;
    Stats->Size = sizeof(*Stats);
    Stats->QueueDepthPackets = P->Media ? (ULONG)PnChannelWritesInFlight(P->Media) : 0;
    Stats->AbrLevel = P->Abr.Level;
}

VOID PumpResetStats(_Inout_ PPN_PUMP P)
{
    RtlZeroMemory(&P->Stats, sizeof(P->Stats));
    P->TxMaxUs = 0;
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
