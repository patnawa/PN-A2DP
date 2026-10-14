/*
 * pump.cpp - streaming engine: PCM ring -> encoder (kernel SBC or codec host) -> RTP -> L2CAP.
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"
#include "pcm.h"
#include "audio_policy.h"

/* every outstanding host request has a ledger slot, every queued result a block */
C_ASSERT(PN_PUMP_HOST_WINDOW <= PN_HOST_LEDGER_SLOTS);
/* every media write the channel can have outstanding has a TX ledger slot */
C_ASSERT(PN_L2CAP_MAX_WRITES <= PN_TX_LEDGER_SLOTS);

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

/* Caller holds R->Lock and has checked Len <= available. */
static VOID RingPeekLocked(_In_ PPN_PCM_RING R, _Out_writes_bytes_(Len) PUCHAR Out, _In_ ULONG Len)
{
    ULONG off = (ULONG)(R->ReadPos % R->Capacity);
    ULONG first = PN_MIN(Len, R->Capacity - off);
    RtlCopyMemory(Out, R->Buffer + off, first);
    if (Len > first) RtlCopyMemory(Out + first, R->Buffer, Len - first);
}

/* Caller holds R->Lock and has checked the ring is configured (Capacity > BytesPerFrame).
 * Appends Length bytes, dropping the oldest on overrun. *Avail: what the ring held before,
 * less any drop. A write ends a finished stream's silence: PumpFinishStream may act again. */
static VOID RingWriteLocked(_Inout_ PPN_PUMP P, _In_reads_bytes_(Length) const UCHAR *Data, _In_ ULONG Length,
                            _Out_ PULONG Avail, _Out_ PBOOLEAN Overrun)
{
    PPN_PCM_RING R = &P->Ring;
    if (Length >= R->Capacity) {
        Data += Length - R->Capacity + R->BytesPerFrame;
        Length = R->Capacity - R->BytesPerFrame;
    }
    ULONG avail = RingAvailableLocked(R);
    BOOLEAN overrun = avail + Length > R->Capacity;
    if (overrun) {
        /* overrun: drop the oldest PCM, keep frame alignment */
        ULONG drop = avail + Length - R->Capacity;
        drop = ((drop + R->BytesPerFrame - 1) / R->BytesPerFrame) * R->BytesPerFrame;
        R->ReadPos += drop;
        R->DroppedBytes += drop;
        P->Stats.RingOverflowEvents++;
        avail -= drop;
    }
    ULONG off = (ULONG)(R->WritePos % R->Capacity);
    ULONG first = PN_MIN(Length, R->Capacity - off);
    RtlCopyMemory(R->Buffer + off, Data, first);
    if (Length > first) RtlCopyMemory(R->Buffer, Data + first, Length - first);
    R->WritePos += Length;
    P->Finished = FALSE;
    *Avail = avail;
    *Overrun = overrun;
}

VOID PumpWritePcm(_Inout_ PPN_PUMP P, _In_reads_bytes_(Length) const UCHAR *Data, _In_ ULONG Length)
{
    PPN_PCM_RING R = &P->Ring;
    if (!R->Buffer || !Length) return;
    KIRQL irql;
    KeAcquireSpinLock(&R->Lock, &irql);
    /* re-check under the lock: PumpReleaseInput may have run since the unlocked test */
    if (!R->Buffer || !R->Capacity || !R->BytesPerFrame || R->Capacity <= R->BytesPerFrame) { KeReleaseSpinLock(&R->Lock, irql); return; }
    ULONG avail;
    BOOLEAN overrun;
    RingWriteLocked(P, Data, Length, &avail, &overrun);
    KeReleaseSpinLock(&R->Lock, irql);
    /* Wake the thread when this write completes an encoder input block, not on every
     * timer tick: completions, host results, a newly pended host request and the 4 ms
     * tick cover a ring that already holds a block the pump could not take yet. */
    if (P->StreamActive && PnPcmWriteShouldWake(avail, Length, P->WakeBytes, overrun))
        KeSetEvent(&P->WakeEvent, IO_SOUND_INCREMENT, FALSE);
}

VOID PumpFlush(_Inout_ PPN_PUMP P)
{
    KIRQL irql;
    KeAcquireSpinLock(&P->Ring.Lock, &irql);
    P->Ring.ReadPos = P->Ring.WritePos;
    KeReleaseSpinLock(&P->Ring.Lock, irql);
}

/* Caller holds R->Lock. Applies a fade-out to the newest Frames frames, in place. */
static VOID RingFadeTailLocked(_Inout_ PPN_PCM_RING R, _In_ ULONG Frames)
{
    ULONGLONG start = R->WritePos - (ULONGLONG)Frames * R->BytesPerFrame;
    ULONG done = 0;
    while (done < Frames) {   /* at most two pieces: the ring wraps once */
        ULONG off = (ULONG)((start + (ULONGLONG)done * R->BytesPerFrame) % R->Capacity);
        ULONG n = PN_MIN(Frames - done, (R->Capacity - off) / R->BytesPerFrame);
        if (!n) break;
        PnPcmRamp(R->Buffer + off, n, R->Channels, R->BitsPerSample, R->ValidBits, R->IsFloat, done, Frames, FALSE);
        done += n;
    }
}

BOOLEAN PumpTakeResumeFade(_Inout_ PPN_PUMP P)
{
    return InterlockedExchange(&P->ResumeFade, 0) != 0;
}

VOID PumpFinishStream(_Inout_ PPN_PUMP P, _In_reads_bytes_opt_(TailBytes) const UCHAR *Tail, _In_ ULONG TailBytes)
{
    PPN_PCM_RING R = &P->Ring;
    ULONG block = P->WakeBytes;
    BOOLEAN padded = FALSE, wake = FALSE;
    KIRQL irql;
    KeAcquireSpinLock(&R->Lock, &irql);
    if (!R->Buffer || !R->Capacity || !R->BytesPerFrame || R->Capacity <= R->BytesPerFrame) { KeReleaseSpinLock(&R->Lock, irql); return; }
    /* The engine's last frames (the WaveRT stream's hold-back FIFO) join the ring under the
     * same lock hold as the fade below: handed over first, they woke the pump thread, which
     * could encode them at full level before the fade (findings #1/#7). */
    if (Tail && TailBytes) {
        ULONG before;
        BOOLEAN overrun;
        RingWriteLocked(P, Tail, TailBytes, &before, &overrun);
        wake = PnPcmWriteShouldWake(before, TailBytes, P->WakeBytes, overrun);
    }
    if (P->StreamActive && !P->Finished && R->SampleRate && block && block % R->BytesPerFrame == 0) {
        ULONG avail = RingAvailableLocked(R);
        /* an empty ring (the pump took every whole block) still gets its silence: the encoder
         * drains and the resumed audio fades in (findings #1/#7) */
        if (avail) RingFadeTailLocked(R, PN_MIN(avail / R->BytesPerFrame, R->SampleRate * PN_TRIM_FADE_MS / 1000));
        /* blocks of silence that push the encoder's delay out (FDK AAC holds ~1600 frames) and,
         * converting, the rate converter's and a short block's (audio_policy.h PnFinishExtraBlocks) */
        ULONG extra = PnFinishExtraBlocks(P->Cfg.Codec, block / R->BytesPerFrame, P->Rate.DelayEngineFrames);
        ULONG pad = PnFinishPadBytes(avail, block, extra, R->Capacity - avail);
        for (ULONG done = 0; done < pad;) {
            ULONG off = (ULONG)((R->WritePos + done) % R->Capacity);
            ULONG n = PN_MIN(pad - done, R->Capacity - off);
            RtlZeroMemory(R->Buffer + off, n);
            done += n;
        }
        R->WritePos += pad;
        padded = pad != 0;
        P->Finished = TRUE;   /* a second call (the engine's EOS, then its pause) adds nothing */
    }
    KeReleaseSpinLock(&R->Lock, irql);
    if (padded) InterlockedExchange(&P->ResumeFade, 1);
    if (P->StreamActive && (padded || wake)) KeSetEvent(&P->WakeEvent, IO_SOUND_INCREMENT, FALSE);
}

/* The newest Frames frames waiting in the ring hold a signal (finding #4). */
static BOOLEAN RingTailHasSignal(_In_ PPN_PCM_RING R, _In_ ULONG Frames)
{
    KIRQL irql;
    KeAcquireSpinLock(&R->Lock, &irql);
    BOOLEAN signal = R->Buffer && R->Capacity &&
        PnRingTailHasSignal(R->Buffer, R->Capacity, R->WritePos, RingAvailableLocked(R), Frames * R->BytesPerFrame);
    KeReleaseSpinLock(&R->Lock, irql);
    return signal;
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

/* Bytes of one sample in a PNA2DP_FMT_* encoder format. */
static ULONG PcmFormatBytes(ULONG Fmt)
{
    return Fmt == PNA2DP_FMT_S16 ? 2 : Fmt == PNA2DP_FMT_S24 ? 3 : 4;
}

/* Converts `Frames` interleaved frames from the ring format to Target format with gain. */
static ULONG ConvertPcm(_In_ PPN_PUMP P, _In_reads_bytes_(InBytes) const UCHAR *In, _In_ ULONG InBytes, _Out_writes_bytes_(OutCap) PUCHAR Out, _In_ ULONG OutCap,
                        _In_ ULONG Target, _In_ ULONG TargetChannels)
{
    PPN_PCM_RING R = &P->Ring;
    /* Gain is unity while Windows controls the receiver's absolute volume, or the local
     * fallback gain otherwise. Mute and volume changes ramp across one block, and a
     * pending trim crossfade is applied (PcmState belongs to the pump thread). */
    ULONG gain = P->Muted ? 0 : P->VolumeGainQ15;
    return PnConvertPcmEx(&P->PcmState, In, InBytes, R->Channels, R->BitsPerSample, R->ValidBits, R->IsFloat,
                          Out, OutCap, Target, TargetChannels, gain);
}

/* Pump thread: a step found the TX queue full. The ring backlog signals link congestion
 * only while this is so; a ring that grows for other reasons (a slow codec host) does
 * not lower the bitrate. */
static VOID PumpNoteTxFull(_Inout_ PPN_PUMP P)
{
    P->Stats.TransportWaits++;
    InterlockedIncrement(&P->Abr.QueueFull);
    P->Abr.BacklogPeakUs = PN_MAX(P->Abr.BacklogPeakUs, RingLatencyUs(P));
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
        InterlockedIncrement(&P->Abr.QueueFull);
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
    if (count > 1) P->Stats.FragmentedPackets++;
    for (ULONG i = 0; i < count; ++i) {
        PPN_L2CAP_WRITE w = writes[i];
        ULONG bytes = PN_MIN(room, PayloadBytes - offset);
        BOOLEAN last = i + 1 == count;
        if (P->UseRtpHeader) RtpWriteHeader(&P->Rtp, w->Buffer, last ? PcmFrames : 0,
                                           P->Cfg.Codec == PNA2DP_CODEC_AAC && last);
        if (P->UsePayloadHeader) w->Buffer[header - 1] = (UCHAR)(PayloadHeaderByte ? PayloadHeaderByte : (FrameCount & 0x0f));
        RtlCopyMemory(w->Buffer + header, Payload + offset, bytes);
        ULONG length = w->Length = header + bytes;
        /* Account the write before it can complete; the PCM rides on the last fragment. */
        KIRQL irql;
        KeAcquireSpinLock(&P->TxLock, &irql);
        BOOLEAN tracked = PnTxLedgerPush(&P->TxLedger, last ? PcmFrames : 0, PnNowHns());
        KeReleaseSpinLock(&P->TxLock, irql);
        status = PnChannelSubmitWrite(ch, w);
        if (!NT_SUCCESS(status) && tracked) {
            /* never sent, so it never completes; only this thread pushes */
            KeAcquireSpinLock(&P->TxLock, &irql);
            PnTxLedgerCancelNewest(&P->TxLedger);
            KeReleaseSpinLock(&P->TxLock, irql);
        }
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
    LONGLONG now = PnNowHns();
    LONGLONG dt = now - SubmitTimeHns;
    ULONG us = (ULONG)(dt / 10);
    /* completion latency (queueing included) stays the published TxCompletion* figure */
    P->TxAvgUs = (P->TxAvgUs * 15 + us) / 16;
    if (us > P->TxMaxUs) P->TxMaxUs = us;
    P->Stats.TxCompletionAvgUs = P->TxAvgUs;
    P->Stats.TxCompletionMaxUs = P->TxMaxUs;
    if (!NT_SUCCESS(Status)) {
        P->Stats.WriteFailures++;
        InterlockedIncrement(&P->Abr.Failed);
    }
    /* This write's own send time, without the time it queued behind the previous one. */
    ULONG sendUs = 0, packetUs = 0;
    BOOLEAN current = FALSE;
    KIRQL irql;
    KeAcquireSpinLock(&P->TxLock, &irql);
    if (SubmitTimeHns >= P->TxStreamStartHns) {   /* not a write of an earlier stream */
        current = TRUE;
        /* The media time this write carries: its PCM frames (VBR AAC and LDAC writes vary in
         * size, not in media time: review round 4 #4); a fragment without frames from its size. */
        packetUs = PnTxWriteMediaUs(PnTxLedgerOldestFrames(&P->TxLedger), P->Cfg.SampleRate, Bytes, P->Cfg.BitrateBps);
        /* a write held by a stall stays out of the ABR throughput estimate (review #6) */
        PnTxLedgerCompleteEx(&P->TxLedger, SubmitTimeHns, now, Bytes > P->TxHeaderBytes ? Bytes - P->TxHeaderBytes : 0,
                             PnTxStallBoundUs(packetUs), &sendUs);
        P->TxSendAvgUs = (P->TxSendAvgUs * 7 + sendUs) / 8;
    }
    KeReleaseSpinLock(&P->TxLock, irql);
    if (current) {
        InterlockedIncrement(&P->Abr.Completed);
        /* Slow: the link took longer to send this write than the media it carries lasts,
         * beyond the fixed completion latency (review round 4 #3). Host transport time, not RSSI. */
        if (PnTxWriteSlow(sendUs, packetUs)) InterlockedIncrement(&P->Abr.Slow);
    }
    /* a write slot is free again, also after a write of an earlier stream */
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

/* Ring bytes one encoder input block of Frames codec frames can take: at the engine rate, the
 * most the rate converter needs for them from any state (PnPcmMaxInputFrames; exact when the
 * rates are equal). Sizes the PCM block and the wake level. */
static ULONG PumpBlockBytes(_In_ PPN_PUMP P, ULONG Frames)
{
    ULONGLONG bytes = (ULONGLONG)PnPcmMaxInputFrames(&P->Rate, Frames * P->PcmFramesPerCodecFrame) * P->Ring.BytesPerFrame;
    return bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : (ULONG)bytes;
}

/* SBC frames per packet for the encoder's current bitpool (findings #6, #8): a bitpool
 * above the one the stream started at must not overflow the MTU, and a lower one packs more
 * frames into fewer packets so the reduction frees air. Within the pump's PCM block. */
static VOID PumpSbcRepack(_Inout_ PPN_PUMP P)
{
    ULONG n = A2dpSbcFramesPerPacket(&P->Cfg, PnSbcPackingTargetMs(P->TargetLatencyMs), P->PayloadRoom, P->TxHeaderBytes, P->SbcSduBytes);
    if (!n) n = 1;
    while (n > 1 && PumpBlockBytes(P, n) > PN_PUMP_MAX_PCM_BLOCK) n--;
    P->CodecFramesPerPacket = n;
    P->WakeBytes = PN_MIN(PumpBlockBytes(P, n), (ULONG)PN_PUMP_MAX_PCM_BLOCK);
}

static VOID AbrApplyLevel(_Inout_ PPN_PUMP P)
{
    AbrBitrateAtLevel(P, P->Abr.Level, &P->Cfg);
    if (P->Cfg.Codec == PNA2DP_CODEC_SBC) {
        sbc_encoder_set_bitpool(&P->Sbc, P->Cfg.SbcBitpool);
        P->CodecFrameBytes = P->Sbc.frame_length;
        PumpSbcRepack(P);
    }
    P->Stats.AbrLevel = P->Abr.Level;
    PN_INFO("ABR level %u -> %u bps", P->Abr.Level, P->Cfg.BitrateBps);
}

static VOID AbrTick(_Inout_ PPN_PUMP P)
{
    LONGLONG now = PnNowHns();
    ULONG elapsed = (ULONG)PN_MIN((now - P->Abr.LastObservationHns) / PN_HNS_PER_MS, 60000ll);
    P->Abr.LastObservationHns = now;
    PN_ABR_SAMPLE s;
    RtlZeroMemory(&s, sizeof(s));
    s.ElapsedMs = elapsed;
    s.Completed = (ULONG)InterlockedExchange(&P->Abr.Completed, 0);
    s.Slow = (ULONG)InterlockedExchange(&P->Abr.Slow, 0);
    s.Failed = (ULONG)InterlockedExchange(&P->Abr.Failed, 0);
    s.Pressure = (ULONG)InterlockedExchange(&P->Abr.Pressure, 0);
    s.QueueFull = (ULONG)InterlockedExchange(&P->Abr.QueueFull, 0);
    s.Trims = P->Abr.Trims;
    P->Abr.Trims = 0;
    s.BacklogPeakUs = P->Abr.BacklogPeakUs;
    P->Abr.BacklogPeakUs = 0;
    ULONG effTarget, effMax;
    PnEffectiveLatencyMs(P->TargetLatencyMs, P->MaxLatencyMs, PN_PUMP_RING_MS, &effTarget, &effMax);
    s.TargetLatencyUs = effTarget * 1000;
    /* latency profiles: the ring at both ends of the window (draining) and the backlog the
     * profile's buffer absorbs before slow writes count (0 up to a 60 ms target) */
    ULONG ringUs = RingLatencyUs(P);
    s.RingStartUs = P->Abr.LastRingUs;
    s.RingEndUs = ringUs;
    P->Abr.LastRingUs = ringUs;
    s.SoftGateUs = PnAbrSoftGateUs(effTarget);
    ULONGLONG deliveredFrames;
    KIRQL irql;
    KeAcquireSpinLock(&P->TxLock, &irql);
    deliveredFrames = P->TxLedger.DeliveredFrames;
    s.DeliveredBits = P->TxLedger.DeliveredBits;
    s.BusyUs = P->TxLedger.BusyUs;
    s.Stalls = P->TxLedger.Stalls;
    P->TxLedger.DeliveredFrames = P->TxLedger.DeliveredBits = P->TxLedger.BusyUs = 0;
    P->TxLedger.Stalls = 0;
    KeReleaseSpinLock(&P->TxLock, irql);
    s.DeliveredMediaUs = P->Cfg.SampleRate ? deliveredFrames * 1000000ull / P->Cfg.SampleRate : 0;
    /* every rung's bitrate, so a reduction can jump to what the link sustains */
    ULONG ladder[25];
    ULONG maxLevel = PN_MIN(P->Abr.MaxLevel, (ULONG)PN_ARRAYSIZE(ladder) - 1);
    BOOLEAN needLadder = P->Abr.Enabled && P->Abr.Level < maxLevel && (s.Trims || PnAbrReasons(&s));
    if (needLadder) for (ULONG i = 0; i <= maxLevel; i++) ladder[i] = AbrBitrateAtLevel(P, i, NULL);
    ULONG reasons = 0;
    int change = PnAbrDecide(&P->Abr.Window, &s, needLadder ? ladder : NULL,
        P->Abr.Enabled, P->Abr.Level, maxLevel, &reasons);
    BOOLEAN bad = reasons != 0;
    ULONG completed = s.Completed, slow = s.Slow, failed = s.Failed, backlog = s.BacklogPeakUs;
    if (change > 0) {
        P->Abr.LastChangeReason = reasons;
        ++P->Abr.ChangeSequence;
        P->Abr.Level = PN_MIN(P->Abr.Level + (ULONG)change, maxLevel);
        P->Abr.Downgrades++;
        P->Stats.AbrDowngrades++;
        AbrApplyLevel(P);
        DeviceNotifyEvent(P->Device, PNA2DP_EVENT_STATE_CHANGED);
    } else if (change < 0) {
        P->Abr.LastChangeReason = PNA2DP_QUALITY_REASON_RECOVERY;
        ++P->Abr.ChangeSequence;
        P->Abr.Level--;
        P->Abr.Upgrades++;
        P->Stats.AbrUpgrades++;
        AbrApplyLevel(P);
        DeviceNotifyEvent(P->Device, PNA2DP_EVENT_STATE_CHANGED);
    }
    PNA2DP_QUALITY q = {};
    q.Size = sizeof(q);
    q.Flags = PNA2DP_QUALITY_CHANGE_DETAILS | (P->Abr.Enabled ? PNA2DP_QUALITY_ABR : 0);
    q.Health = bad ? PNA2DP_QUALITY_CONGESTED : !completed ? PNA2DP_QUALITY_IDLE :
        P->Abr.Level ? PNA2DP_QUALITY_RECOVERING : P->Abr.Window.GoodMs >= 3000 ? PNA2DP_QUALITY_STABLE : PNA2DP_QUALITY_SAMPLING;
    q.Codec = P->Cfg.Codec; q.BitrateBps = P->Cfg.BitrateBps;
    q.CeilingBps = AbrBitrateAtLevel(P, 0, NULL); q.FloorBps = AbrBitrateAtLevel(P, P->Abr.MaxLevel, NULL);
    q.Level = P->Abr.Level; q.MaxLevel = P->Abr.MaxLevel;
    q.HealthyMs = P->Abr.Window.GoodMs; q.CongestedMs = P->Abr.Window.BadMs;
    q.CompletedPackets = completed; q.SlowPackets = slow; q.FailedPackets = failed;
    q.BacklogPeakUs = backlog; q.ObservationMs = elapsed;
    q.RecoveryTargetMs = PnQualityRecoveryTarget(&P->Abr.Window);
    q.ChangeSequence = P->Abr.ChangeSequence; q.LastChangeReason = P->Abr.LastChangeReason;
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
        PumpNoteTxFull(P);
        return FALSE;
    }
    ULONG framesPerPacket = P->CodecFramesPerPacket;
    /* never a packet the MTU refuses: SendMediaPacket would drop every one (finding #6) */
    if (P->CodecFrameBytes && framesPerPacket * P->CodecFrameBytes > P->PayloadRoom)
        framesPerPacket = PN_MAX(P->PayloadRoom / P->CodecFrameBytes, 1u);
    ULONG pcmFrames = framesPerPacket * P->PcmFramesPerCodecFrame;
    /* engine frames that make exactly pcmFrames codec frames from the converter's state */
    ULONG ringFrames = PnPcmInputFramesFor(&P->PcmState, pcmFrames);
    ULONG needBytes = ringFrames * P->Ring.BytesPerFrame;
    if (needBytes > PcmCap) return FALSE;
    if (RingAvailable(&P->Ring) < needBytes) return FALSE;

    ULONG got = RingRead(&P->Ring, PcmBuf, needBytes);
    if (got < needBytes) return FALSE;
    P->Stats.PcmFramesConsumed += ringFrames;

    /* The encoder takes S32: gain and 24-bit sources reach the analysis filter without
     * being rounded to 16 bits first (16-bit input at unity gain encodes bit-exactly). */
    ULONG channels = P->Sbc.channels;
    ULONG pcmPerFrame = P->PcmFramesPerCodecFrame;
    ULONG convBytes = ConvertPcm(P, PcmBuf, got, P->ConvBuffer, PN_MIN(P->ConvCapacity, pcmFrames * channels * (ULONG)sizeof(LONG)),
                                 PNA2DP_FMT_S32, channels);
    if (convBytes < pcmFrames * channels * sizeof(LONG)) return FALSE;
    const LONG *s32 = (const LONG *)P->ConvBuffer;
    ULONG n = 0, count = 0;
    for (ULONG f = 0; f < framesPerPacket; f++) {
        ULONG len = sbc_encode_frame_s32(&P->Sbc, (const sbc_s32 *)s32 + f * pcmPerFrame * channels, PktBuf + n, PktCap - n);
        if (!len) break;
        n += len;
        count++;
    }
    if (!count) return FALSE;
    ULONG samples = count * pcmPerFrame * channels;
    if (P->SampleState == 1 && n <= sizeof(P->Sample.Sbc) && samples * sizeof(SHORT) <= sizeof(P->Sample.Pcm)) {
        KIRQL irql;
        KeAcquireSpinLock(&P->Lock, &irql);
        if (P->SampleState == 1) {
            RtlZeroMemory(&P->Sample, sizeof(P->Sample));
            P->Sample.Size = sizeof(P->Sample);
            P->Sample.SampleRate = P->Cfg.SampleRate; P->Sample.Channels = channels;
            P->Sample.GainQ15 = P->VolumeGainQ15; P->Sample.Muted = P->Muted;
            P->Sample.PcmBytes = samples * sizeof(SHORT); P->Sample.SbcBytes = n;
            /* the diagnostic sample stays S16: the encoder input rounded to 16 bits */
            SHORT *pcm = (SHORT *)P->Sample.Pcm;
            for (ULONG i = 0; i < samples; i++) pcm[i] = (SHORT)(PN_MIN((LONGLONG)s32[i] + 0x8000, (LONGLONG)2147483647) >> 16);
            RtlCopyMemory(P->Sample.Sbc, PktBuf, n);
            P->SampleState = 2;
        }
        KeReleaseSpinLock(&P->Lock, irql);
    }
    SendMediaPacket(P, PktBuf, n, count, count * P->PcmFramesPerCodecFrame, count & 0x0F);
    return TRUE;
}

/* Host path: ship a PCM block to the host (encode request). A pended request is taken
 * before any PCM leaves the ring: without one the PCM simply waits for the next step. */
static BOOLEAN PumpStepHostSubmit(_Inout_ PPN_PUMP P, _Inout_ PUCHAR PcmBuf, _In_ ULONG PcmCap)
{
    if (P->HostOutstanding >= PN_PUMP_HOST_WINDOW) return FALSE;
    if (P->HostResultCount >= PN_PUMP_HOST_WINDOW) return FALSE;
    ULONG pcmFrames = P->CodecFramesPerPacket * P->PcmFramesPerCodecFrame;
    ULONG ringFrames = PnPcmInputFramesFor(&P->PcmState, pcmFrames);   /* engine frames for exactly pcmFrames */
    ULONG needBytes = ringFrames * P->Ring.BytesPerFrame;
    if (needBytes > PcmCap) return FALSE;
    if (RingAvailable(&P->Ring) < needBytes) return FALSE;
    PUCHAR dst;
    ULONG dstCap;
    InterlockedExchange(&P->HostWaitingForWork, 1);   /* set first: a request pended from now on wakes us */
    PIRP irp = CodecHostAcquireWork(&P->Device->Host, &dst, &dstCap);
    if (!irp) return FALSE;   /* the host has not re-pended yet: not a timeout, PCM stays queued */
    InterlockedExchange(&P->HostWaitingForWork, 0);
    /* From here the request must be completed. Only a concurrent flush can shorten the
     * read; that is a discontinuity anyway, so pad with silence. */
    ULONG got = RingRead(&P->Ring, PcmBuf, needBytes);
    if (got < needBytes) RtlZeroMemory(PcmBuf + got, needBytes - got);
    P->Stats.PcmFramesConsumed += ringFrames;
    ULONG outBytes = pcmFrames * P->Cfg.Channels * PcmFormatBytes(P->EncoderPcmFormat);
    ULONG convBytes = dst ? ConvertPcm(P, PcmBuf, needBytes, dst, PN_MIN(dstCap, outBytes), P->EncoderPcmFormat, P->Cfg.Channels) : 0;

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
    /* record it before the host can see it: its result may arrive before Complete returns */
    LONGLONG now = PnNowHns();
    KIRQL irql;
    KeAcquireSpinLock(&P->HostLock, &irql);
    BOOLEAN recorded = convBytes && PnHostLedgerAdd(&P->HostLedger, hdr.Sequence, pcmFrames, now);
    P->HostOutstanding = (LONG)P->HostLedger.Count;
    KeReleaseSpinLock(&P->HostLock, irql);
    P->HostLastSubmitHns = now;
    if (!recorded) {
        /* conversion failed: hand the request back as a no-op (unknown kinds are ignored) */
        hdr.Kind = 0;
        P->Stats.HostDroppedBlocks++;
        CodecHostCompleteWork(&P->Device->Host, irp, &hdr, 0);
        return FALSE;
    }
    CodecHostCompleteWork(&P->Device->Host, irp, &hdr, convBytes);
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
            if (P->Media && PnChannelIsConnected(P->Media)) PumpNoteTxFull(P);
            else P->Stats.TransportWaits++;
            return FALSE;
        }
        b = CONTAINING_RECORD(RemoveHeadList(&P->HostResults), PN_ENCODED_BLOCK, Link);
        P->HostResultCount--;
        P->HostQueuedPcmFrames -= PN_MIN(P->HostQueuedPcmFrames, b->PcmFrames);
    }
    KeReleaseSpinLock(&P->HostLock, irql);
    if (!b) return FALSE;
    /* PCM of dropped blocks before this one: the receiver sees a gap, not a time shift */
    P->Rtp.Timestamp += b->SkipPcmFrames;
    if (b->PayloadBytes) {
        SendMediaPacket(P, b->Payload, b->PayloadBytes, b->FrameCount, b->PcmFrames, b->PayloadHeaderByte);
    } else {
        P->Rtp.Timestamp += b->PcmFrames;
    }
    KeAcquireSpinLock(&P->HostLock, &irql);
    InsertTailList(&P->HostFreeBlocks, &b->Link);
    KeReleaseSpinLock(&P->HostLock, irql);
    return TRUE;
}

/* Failure events for the health policy; a run of them quarantines the host. Written-off
 * and skipped requests count once per stall episode (PnHostStallCounts, finding #19). */
static VOID PumpHostFailures(_Inout_ PPN_PUMP P, _In_ ULONG Count)
{
    for (ULONG i = 0; i < PN_MIN(Count, PN_HOST_FAILURE_LIMIT); i++) CodecHostReportFailure(&P->Device->Host);
}

NTSTATUS PumpHostResult(_Inout_ PPN_PUMP P, _In_ const PNA2DP_CODEC_RESULT *R, _In_reads_bytes_(R->PayloadBytes) const UCHAR *Payload)
{
    if (!P->StreamActive || P->Owner != PnEncoderHost || R->StreamGeneration != (ULONG)P->Generation) return STATUS_SUCCESS;   /* stale */
    BOOLEAN valid = R->Status == 0 &&
        PnHostResultValid(R->PayloadBytes, PN_PUMP_MAX_PACKET, R->FrameCount, R->PayloadHeaderByte, P->UsePayloadHeader);
    KIRQL irql;
    BOOLEAN matched = FALSE, queued = FALSE;
    ULONG lostFailures = 0;
    PN_HOST_MATCH m = { 0 };
    KeAcquireSpinLock(&P->HostLock, &irql);
    /* PumpStopStream bumps Generation before it drains under this lock */
    if (P->StreamActive && R->StreamGeneration == (ULONG)P->Generation) {
        /* Results come back in submission order: a later sequence writes off the
         * requests before it; a sequence that is not outstanding is a duplicate or
         * was already written off by the watchdog. */
        m = PnHostLedgerMatch(&P->HostLedger, R->Sequence);
        matched = m.Matched;
        if (matched) {
            P->HostPendingSkip += m.LostPcmFrames;
            if (m.Lost && PnHostStallCounts(&P->HostStall, (ULONGLONG)PnNowHns() / PN_HNS_PER_MS)) lostFailures = 1;
            if (valid && !IsListEmpty(&P->HostFreeBlocks)) {
                PPN_ENCODED_BLOCK b = CONTAINING_RECORD(RemoveHeadList(&P->HostFreeBlocks), PN_ENCODED_BLOCK, Link);
                b->Sequence = R->Sequence;
                b->PayloadBytes = R->PayloadBytes;
                b->FrameCount = R->FrameCount;
                /* never more PCM time than the host was actually given */
                b->PcmFrames = PnHostLedgerReturn(&P->HostLedger, R->PcmFramesConsumed);
                b->PayloadHeaderByte = R->PayloadHeaderByte;
                b->ActualBitrate = R->ActualBitrateBps;
                b->SkipPcmFrames = P->HostPendingSkip;
                b->SubmitTimeHns = m.SubmitHns;
                P->HostPendingSkip = 0;
                if (R->PayloadBytes) RtlCopyMemory(b->Payload, Payload, R->PayloadBytes);
                InsertTailList(&P->HostResults, &b->Link);
                P->HostResultCount++;
                P->HostQueuedPcmFrames += b->PcmFrames;
                queued = TRUE;
                PnHostStallEnd(&P->HostStall);   /* the host answers again */
            } else {
                /* this block's audio is lost; its RTP time is kept for the next packet */
                P->HostPendingSkip += PnHostLedgerReturn(&P->HostLedger, m.SubmittedPcmFrames);
            }
            P->HostOutstanding = (LONG)P->HostLedger.Count;
        }
    }
    KeReleaseSpinLock(&P->HostLock, irql);
    if (!matched) return STATUS_SUCCESS;
    if (m.Lost) {
        PN_WARN("codec host skipped %u request(s) before sequence %u", m.Lost, R->Sequence);
        P->Stats.HostTimeouts += m.Lost;
        P->Stats.HostDroppedBlocks += m.Lost;
        PumpHostFailures(P, lostFailures);
    }
    if (!queued) {
        P->Stats.HostDroppedBlocks++;
        if (!valid) {
            /* one bad block is dropped; only a run of them quarantines the host */
            CodecHostReportFailure(&P->Device->Host);
            return R->Status == 0 ? STATUS_INVALID_PARAMETER : STATUS_SUCCESS;
        }
        return STATUS_SUCCESS;   /* no free block (cannot happen inside the window) */
    }
    CodecHostReportSuccess(&P->Device->Host);
    /* round trip of this request, measured from its own submission */
    LONGLONG rt = PnNowHns() - m.SubmitHns;
    P->HostRoundTripAvgUs = (P->HostRoundTripAvgUs * 7 + (ULONG)PN_MIN(rt / 10, 0x7fffffffll)) / 8;
    P->Stats.HostRoundTripAvgUs = P->HostRoundTripAvgUs;
    /* Results may have been encoded before an ABR change. Never feed their old
     * bitrate back into the desired configuration: it would undo AAC reductions. */
    KeSetEvent(&P->WakeEvent, IO_SOUND_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

VOID PumpHostWorkPended(_Inout_ PPN_PUMP P)
{
    if (P->StreamActive && P->Owner == PnEncoderHost && InterlockedExchange(&P->HostWaitingForWork, 0))
        KeSetEvent(&P->WakeEvent, IO_SOUND_INCREMENT, FALSE);
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

/* Pump thread: discard the oldest PCM down to KeepMs. The audio that would have played
 * next is saved so the next block crossfades into the kept audio instead of jumping. */
static VOID PumpTrim(_Inout_ PPN_PUMP P, _In_ ULONG KeepMs)
{
    PPN_PCM_RING R = &P->Ring;
    ULONG fade = 0, drop = 0;
    /* a crossfade not started yet (a stall trims again before anything was converted) keeps
     * its source: that continues the audio played last, the newly dropped audio does not */
    BOOLEAN pending = PnPcmFadePending(&P->PcmState);   /* PcmState belongs to the pump thread */
    KIRQL irql;
    KeAcquireSpinLock(&R->Lock, &irql);
    if (R->Buffer && R->Capacity && R->BytesPerFrame && R->SampleRate) {
        ULONG keepBytes = (ULONG)((ULONGLONG)R->SampleRate * KeepMs / 1000) * R->BytesPerFrame;
        drop = PnTrimPlan(RingAvailableLocked(R), keepBytes, R->BytesPerFrame, R->SampleRate * PN_TRIM_FADE_MS / 1000,
                          P->FadeBuffer && !pending ? PN_PUMP_FADE_BYTES : 0, &fade);
        if (fade && P->FadeBuffer && !pending) RingPeekLocked(R, P->FadeBuffer, fade * R->BytesPerFrame);
        R->ReadPos += drop;
        R->DroppedBytes += drop;
    }
    KeReleaseSpinLock(&R->Lock, irql);
    if (!drop) return;
    /* replaces a crossfade in progress (its source was older audio anyway); a pending one stays */
    if (!pending) {
        P->PcmState.FadeOut = fade ? P->FadeBuffer : NULL;
        P->PcmState.FadeFrames = fade;
        P->PcmState.FadePos = 0;
    }
    P->Stats.Underruns++;   /* ABI name; counts latency-trim events */
    P->Abr.Trims++;
}

static KSTART_ROUTINE PumpThread;
static VOID PumpThread(_In_ PVOID Context)
{
    PPN_PUMP P = (PPN_PUMP)Context;
    PUCHAR pcmBuf = (PUCHAR)PnAllocNp(PN_PUMP_MAX_PCM_BLOCK, PNA2DP_TAG_PCM);
    PUCHAR pktBuf = (PUCHAR)PnAllocNp(PN_PUMP_MAX_PACKET, PNA2DP_TAG_PCM);
    LARGE_INTEGER timeout;
    LONGLONG lastAbr = PnNowHns();

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY + 2);   /* 18: above normal, below audio engine's 24 */

    while (P->Running) {
        /* Event driven: a buffered encoder block, a write completion, a host result or a
         * pended host request wakes the thread. The tick is only a safety net (latency
         * guard, host watchdog, ABR) while streaming; otherwise sleep until
         * PumpStartStream/Uninitialize. */
        timeout.QuadPart = -PN_HNS_PER_MS * PN_PUMP_TICK_MS;
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

        /* latency guard: drop PCM beyond MaxLatencyMs so we never lag behind. The ceiling
         * stays below the ring size, or overruns would discard audio every tick instead. */
        ULONG effTarget, effMax;
        PnEffectiveLatencyMs(P->TargetLatencyMs, P->MaxLatencyMs, PN_PUMP_RING_MS, &effTarget, &effMax);
        if (RingLatencyUs(P) > effMax * 1000) PumpTrim(P, effTarget);

        ULONG guard = 0;
        if (P->Owner == PnEncoderKernelSbc) {
            while (guard++ < 64 && P->StreamActive && PumpStepSbc(P, pcmBuf, PN_PUMP_MAX_PCM_BLOCK, pktBuf, PN_PUMP_MAX_PACKET)) {}
        } else if (P->Owner == PnEncoderHost) {
            while (guard++ < 64 && P->StreamActive && (PumpStepHostDrain(P) | PumpStepHostSubmit(P, pcmBuf, PN_PUMP_MAX_PCM_BLOCK))) {}
            /* host watchdog: write off exactly the requests that are overdue */
            if (P->HostOutstanding > 0) {
                LONGLONG nowHns = PnNowHns();
                ULONG expired = 0, failures = 0;
                KIRQL irql;
                KeAcquireSpinLock(&P->HostLock, &irql);
                while (P->HostLedger.Count && nowHns - PnHostLedgerOldestSubmit(&P->HostLedger) > PN_HNS_PER_MS * PN_PUMP_HOST_TIMEOUT_MS) {
                    P->HostPendingSkip += PnHostLedgerDropOldest(&P->HostLedger);
                    expired++;
                    if (PnHostStallCounts(&P->HostStall, (ULONGLONG)nowHns / PN_HNS_PER_MS)) failures++;
                }
                P->HostOutstanding = (LONG)P->HostLedger.Count;
                KeReleaseSpinLock(&P->HostLock, irql);
                if (expired) {
                    PN_WARN("codec host did not answer %u request(s) within %u ms", expired, PN_PUMP_HOST_TIMEOUT_MS);
                    P->HostTimeouts += expired;
                    P->Stats.HostTimeouts += expired;
                    P->Stats.HostDroppedBlocks += expired;
                    PumpHostFailures(P, failures);
                }
            }
        }
        LONGLONG now = PnNowHns();
        if (now - lastAbr > PN_HNS_PER_MS * 250) {
            lastAbr = now;
            AbrTick(P);
            /* C4: writes outstanding and none completing: the device declares the receiver
             * unreachable (its worker acts; nothing here takes a device mutex) */
            if (PnStallEdge(PumpMediaStalled(P, PN_MEDIA_STALL_MS), &P->StallReported)) DeviceMediaStalled(P->Device);
        }
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
    KeInitializeSpinLock(&P->TxLock);
    InitializeListHead(&P->HostResults);
    InitializeListHead(&P->HostFreeBlocks);
    KeInitializeEvent(&P->WakeEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&P->Idle, NotificationEvent, TRUE);
    P->Device = Device;   /* marks the pump initialized for PumpUninitialize */
    P->VolumeGainQ15 = 32768;
    P->TargetLatencyMs = PN_PUMP_DEFAULT_TARGET_MS;
    P->MaxLatencyMs = PN_PUMP_DEFAULT_MAX_MS;
    P->MaxWritesInFlight = 4;
    P->ConvCapacity = PN_PUMP_MAX_PCM_BLOCK;   /* kernel SBC: at most 15 x 128 frames x 2 ch x S32 = 15360 */
    P->ConvBuffer = (PUCHAR)PnAllocNp(P->ConvCapacity, PNA2DP_TAG_PCM);
    if (!P->ConvBuffer) return STATUS_INSUFFICIENT_RESOURCES;
    P->FadeBuffer = (PUCHAR)PnAllocNp(PN_PUMP_FADE_BYTES, PNA2DP_TAG_PCM);
    if (!P->FadeBuffer) return STATUS_INSUFFICIENT_RESOURCES;   /* PumpUninitialize frees it */
    /* host results never allocate: one block per possible queued result */
    for (ULONG i = 0; i < PN_PUMP_HOST_BLOCKS; i++) {
        PPN_ENCODED_BLOCK b = (PPN_ENCODED_BLOCK)PnAllocNp(sizeof(*b), PNA2DP_TAG_CODEC);
        if (!b) return STATUS_INSUFFICIENT_RESOURCES;   /* PumpUninitialize frees the list */
        InsertTailList(&P->HostFreeBlocks, &b->Link);
    }
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
    KeAcquireSpinLock(&P->HostLock, &irql);
    while (!IsListEmpty(&P->HostResults)) InsertTailList(&P->HostFreeBlocks, RemoveHeadList(&P->HostResults));
    P->HostResultCount = 0;
    P->HostQueuedPcmFrames = 0;
    RtlZeroMemory(&P->HostLedger, sizeof(P->HostLedger));
    RtlZeroMemory(&P->HostStall, sizeof(P->HostStall));
    P->HostPendingSkip = 0;
    P->HostOutstanding = 0;
    KeReleaseSpinLock(&P->HostLock, irql);
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
    while (!IsListEmpty(&P->HostFreeBlocks)) PnFree(CONTAINING_RECORD(RemoveHeadList(&P->HostFreeBlocks), PN_ENCODED_BLOCK, Link), PNA2DP_TAG_CODEC);
    PumpReleaseInput(P);
    PnFree(P->ConvBuffer, PNA2DP_TAG_PCM);
    P->ConvBuffer = NULL;
    PnFree(P->FadeBuffer, PNA2DP_TAG_PCM);
    P->FadeBuffer = NULL;
}

NTSTATUS PumpStartStream(_Inout_ PPN_PUMP P, _In_ const A2DP_CODEC_CONFIG *Cfg, _In_ PPN_L2CAP_CHANNEL Media, _In_ ULONG MediaMtu,
                         _In_ PN_ENCODER_OWNER Owner, _In_ const PNA2DP_CONFIG *User)
{
    PAGED_CODE();
    PumpStopStream(P);
    if (!P->Running || !P->ConvBuffer || !P->Ring.Buffer || !P->Ring.BytesPerFrame) return STATUS_INVALID_DEVICE_STATE;
    if (!Cfg->SampleRate || !Cfg->Channels || !Media) return STATUS_INVALID_PARAMETER;
    /* The engine rate is converted to the codec rate (pcm.h PnPcmRatePlan: equal, exactly 2:1, or
     * 44.1 / 48 / 88.2 / 96 kHz to 44.1 / 48 kHz). Never play PCM at another rate unconverted. */
    PN_PCM_RATE_PLAN rate;
    if (!PnPcmRatePlan(P->Ring.SampleRate, Cfg->SampleRate, &rate)) return STATUS_NOT_SUPPORTED;
    P->Rate = rate;

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
    InterlockedExchange(&P->Abr.QueueFull, 0);
    P->Abr.Trims = 0;
    P->Abr.BacklogPeakUs = 0;
    P->Abr.LastRingUs = 0;
    P->Abr.FloorBps = User->AbrFloorBps;
    P->Abr.LastObservationHns = PnNowHns();
    P->StallReported = FALSE;
    InterlockedIncrement(&P->Generation);
    InterlockedExchange(&P->HostOutstanding, 0);
    P->HostNextSequence = 1;
    P->HostTimeouts = 0;
    RtpInit(&P->Rtp, 96);
    /* PCM rendered before the stream could start (stale after an outage, or what the start
     * hold could not keep): when it ended in a signal the next sample is mid-programme, so the
     * first block fades in from silence instead of stepping to it (findings #5, #25, #23).
     * Rendered silence (T2) is no programme: the onset after it stays untouched (finding #4). */
    BOOLEAN midProgramme = RingTailHasSignal(&P->Ring, P->Ring.SampleRate * PN_TRIM_FADE_MS / 1000);
    InterlockedExchange(&P->ResumeFade, 0);
    PumpFlush(P);
    P->Finished = FALSE;
    PnPcmStateResetRate(&P->PcmState, &P->Rate, midProgramme);   /* a fresh converter per stream */
    InterlockedExchange(&P->HostWaitingForWork, 0);
    P->TxHeaderBytes = (P->UseRtpHeader ? RTP_HEADER_SIZE : 0) + (P->UsePayloadHeader ? 1 : 0);
    P->PayloadRoom = P->MediaMtu - P->TxHeaderBytes;
    {
        /* writes of an earlier stream may still complete: they are recognised by time */
        KIRQL irql;
        KeAcquireSpinLock(&P->TxLock, &irql);
        RtlZeroMemory(&P->TxLedger, sizeof(P->TxLedger));
        P->TxStreamStartHns = PnNowHns();
        P->TxSendAvgUs = 0;
        KeReleaseSpinLock(&P->TxLock, irql);
    }
    /* published once the ring, the host state and the TX ledger hold only the new stream:
     * a position query that sees the new epoch must not see the pre-start ring and lock
     * its latency in (review round 5 #4) */
    InterlockedIncrement(&P->LatencyEpoch);

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
        /* Baseband packet the SDU should fit (0 = radio unknown: MTU only, as before). SBC XQ
         * packs to the packet both ends take: 3-DH5 only with a sink known to do 3 Mbps EDR
         * (review round 4 #5); its bitpool is plain dual channel's (known issue, #1/#7). */
        PPNA2DP_DEVICE D = P->Device;
        ULONG localSdu = PnBasebandSduBytes(D->Link.RadioInfoValid, D->Link.RadioInfo.radioInfo.lmpSupportedFeatures);
        ULONG xqSdu = PnSbcXqSduBytes(D->Link.RadioInfoValid, D->Link.RadioInfo.radioInfo.lmpSupportedFeatures,
                                      D->Link.RemoteFeaturesValid, D->Link.RemoteFeatures);
        A2DP_SBC_STREAM_PLAN plan;
        if (!A2dpSbcPlanStream(Cfg, (User->Flags & PNA2DP_CFG_SBC_XQ) != 0, localSdu, xqSdu, payloadRoom, P->TxHeaderBytes,
                               PnSbcPackingTargetMs(P->TargetLatencyMs), &plan)) return STATUS_NOT_SUPPORTED;
        sp.bitpool = plan.StartBitpool;
        if (sbc_encoder_init(&P->Sbc, &sp) != 0) return STATUS_INVALID_PARAMETER;
        /* a bitpool lowered live stays the ABR's top rung across the restart; a live change
         * may still raise it to the negotiated maximum (finding #6) */
        P->Cfg.SbcBitpool = plan.StartBitpool;
        P->Cfg.SbcMaxBitpool = plan.StartBitpool;
        P->Cfg.SbcFitBitpool = plan.FitBitpool;
        P->SbcBitpoolCeiling = plan.CeilingBitpool;
        P->SbcSduBytes = plan.SduBytes;
        P->PcmFramesPerCodecFrame = P->Sbc.pcm_per_frame;
        P->CodecFrameBytes = P->Sbc.frame_length;
        P->EncoderPcmFormat = PNA2DP_FMT_S32;   /* sbc_encode_frame_s32 */
        P->Abr.MaxLevel = 24;
        P->CodecFramesPerPacket = plan.FramesPerPacket;
        if (Owner != PnEncoderKernelSbc) Owner = P->Owner = PnEncoderKernelSbc;
        break;
    }
    case PNA2DP_CODEC_AAC: {
        P->PcmFramesPerCodecFrame = 1024;
        P->CodecFramesPerPacket = 1;
        P->EncoderPcmFormat = PNA2DP_FMT_S16;
        P->Abr.MaxLevel = 4;
        /* Many sinks do not reassemble fragmented AAC: keep the ladder's top (and so every
         * rung) at a bitrate whose access unit fits one packet. The plugin caps its peak
         * with the same formula. */
        ULONG fit = PnAacMaxBitrateForPayload(payloadRoom, Cfg->SampleRate);
        if (fit) {
            if (!P->Cfg.MaxBitrateBps || P->Cfg.MaxBitrateBps > fit) P->Cfg.MaxBitrateBps = fit;
            if (!P->Cfg.BitrateBps || P->Cfg.BitrateBps > fit) P->Cfg.BitrateBps = fit;
        }
        break;
    }
    case PNA2DP_CODEC_APTX:
    case PNA2DP_CODEC_APTX_LL:
    case PNA2DP_CODEC_APTX_HD:
        /* one codeword = 4 PCM frames -> 4 bytes (aptX, LL) or 6 (HD); packets fill the MTU in
         * multiples of 8 codewords, the parity sync period (findings #16, #17) */
        P->PcmFramesPerCodecFrame = 4;
        P->CodecFramesPerPacket = PnAptxBlocksPerPacket(Cfg->Codec, payloadRoom);
        P->EncoderPcmFormat = PNA2DP_FMT_S24;
        P->Abr.MaxLevel = 0;
        break;
    case PNA2DP_CODEC_LDAC:
        /* libldac needs a 679-byte media MTU; the device renegotiates below it (finding #18) */
        if (!PnLdacMediaMtuUsable(P->MediaMtu)) return STATUS_NOT_SUPPORTED;
        P->PcmFramesPerCodecFrame = (Cfg->SampleRate >= 88200) ? 256 : 128;
        P->CodecFramesPerPacket = 2;   /* blocks of 2 LDAC frames: the plugin packs per MTU and never queues more than one packet per call */
        P->EncoderPcmFormat = Cfg->PcmFormat ? Cfg->PcmFormat : PNA2DP_FMT_S32;
        /* converted audio is not re-quantised to 16 bits: the automatic format takes S32 (a stream
         * set up for a 16-bit engine at its own rate, then bridged to another engine rate) */
        if (P->Rate.EngineRate != P->Rate.CodecRate && !User->Ldac.FormatMask && P->EncoderPcmFormat == PNA2DP_FMT_S16)
            P->EncoderPcmFormat = PNA2DP_FMT_S32;
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
    /* Rounding to S16 is dithered (kernel SBC takes S32 and needs none). */
    P->PcmState.Dither = P->Owner == PnEncoderHost && P->EncoderPcmFormat == PNA2DP_FMT_S16;
    /* one encoder block must fit the pump's PCM buffer (at an engine rate above the codec rate
     * the block takes more ring bytes); aptX keeps whole sync periods */
    BOOLEAN aptx = Cfg->Codec == PNA2DP_CODEC_APTX || Cfg->Codec == PNA2DP_CODEC_APTX_HD || Cfg->Codec == PNA2DP_CODEC_APTX_LL;
    while (P->CodecFramesPerPacket > 1 && PumpBlockBytes(P, P->CodecFramesPerPacket) > PN_PUMP_MAX_PCM_BLOCK)
        P->CodecFramesPerPacket -= (aptx && P->CodecFramesPerPacket > PN_APTX_SYNC_BLOCKS) ? PN_APTX_SYNC_BLOCKS : 1;
    if (PumpBlockBytes(P, P->CodecFramesPerPacket) > PN_PUMP_MAX_PCM_BLOCK) return STATUS_NOT_SUPPORTED;
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
    PN_INFO("pump start: %s %u Hz ch %u bitrate %u, %u frames/packet, mtu %u, owner %u, engine %u Hz (2:1 %u, polyphase %u/%u, +%u frames)",
            A2dpCodecName(Cfg->Codec), Cfg->SampleRate, Cfg->Channels, P->Cfg.BitrateBps, P->CodecFramesPerPacket, P->MediaMtu, P->Owner,
            P->Ring.SampleRate, P->Rate.Decimate == 2, P->Rate.Rs ? P->Rate.Rs->L : 0, P->Rate.Rs ? P->Rate.Rs->M : 0, P->Rate.DelayEngineFrames);
    AbrTick(P); // publish this stream's codec/range before callers observe it
    /* PCM writes wake the thread once a whole encoder input block is buffered */
    P->WakeBytes = PN_MIN(PumpBlockBytes(P, P->CodecFramesPerPacket), (ULONG)PN_PUMP_MAX_PCM_BLOCK);
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
    P->WakeBytes = 0;
    PumpFlush(P);
    InterlockedIncrement(&P->LatencyEpoch);   /* after the flush, as in PumpStartStream */
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
    if (Sample) RtlZeroMemory(Sample, sizeof(*Sample));   /* _Out_opt_: defined on every path */
    KeAcquireSpinLock(&P->Lock, &irql);
    NTSTATUS status = STATUS_SUCCESS;
    if (Action > 2) status = STATUS_INVALID_PARAMETER;
    else if (Action == 2) { RtlZeroMemory(&P->Sample, sizeof(P->Sample)); P->SampleState = 0; }
    else if (Action == 1 && (!P->StreamActive || P->Owner != PnEncoderKernelSbc)) status = STATUS_INVALID_DEVICE_STATE;
    else if (Action == 1) { RtlZeroMemory(&P->Sample, sizeof(P->Sample)); P->SampleState = 1; }
    else if (!Sample) status = STATUS_INVALID_PARAMETER;
    else if (P->SampleState != 2) status = STATUS_DEVICE_NOT_READY;
    else { *Sample = P->Sample; RtlZeroMemory(&P->Sample, sizeof(P->Sample)); P->SampleState = 0; }
    KeReleaseSpinLock(&P->Lock, irql);
    return status;
}

ULONG PumpCurrentBitrate(_In_ PPN_PUMP P) { return P->StreamActive ? P->Cfg.BitrateBps : 0; }

ULONG PumpCeilingBitrate(_In_ PPN_PUMP P)
{
    if (!P->StreamActive) return 0;
    KIRQL irql;
    KeAcquireSpinLock(&P->Lock, &irql);
    ULONG ceiling = P->Quality.CeilingBps;   /* published by AbrTick, first in PumpStartStream */
    KeReleaseSpinLock(&P->Lock, irql);
    return ceiling;
}

VOID PumpNoteWaveStarvation(_Inout_ PPN_PUMP P, _In_ ULONG Packets)
{
    InterlockedExchangeAdd((volatile LONG *)&P->Stats.WaveStarvedPackets, (LONG)Packets);
}

ULONG PumpPipelineLatencyUs(_In_ PPN_PUMP P)
{
    /* Exact frame accounting: a block moving from the ring to the host, to the result
     * queue or into a write leaves the total unchanged. */
    PPN_PCM_RING R = &P->Ring;
    ULONGLONG ringFrames = 0, codecFrames = 0;
    ULONG ringRate = 0, sendUs;
    KIRQL irql;
    KeAcquireSpinLock(&R->Lock, &irql);
    if (R->Buffer && R->BytesPerFrame) {
        ringFrames = RingAvailableLocked(R) / R->BytesPerFrame;
        ringRate = R->SampleRate;
        if (P->StreamActive) ringFrames += P->Rate.DelayEngineFrames;   /* the rate converter's latency (0 at equal rates) */
    }
    KeReleaseSpinLock(&R->Lock, irql);
    KeAcquireSpinLock(&P->HostLock, &irql);
    codecFrames += PnHostLedgerHeldFrames(&P->HostLedger) + P->HostQueuedPcmFrames;
    KeReleaseSpinLock(&P->HostLock, irql);
    KeAcquireSpinLock(&P->TxLock, &irql);
    codecFrames += P->TxLedger.InFlightFrames;
    sendUs = P->TxSendAvgUs;
    KeReleaseSpinLock(&P->TxLock, irql);
    return PnPipelineLatencyUs(ringFrames, ringRate, codecFrames, P->Cfg.SampleRate, sendUs);
}

ULONG PumpEstimatedLatencyUs(_In_ PPN_PUMP P) { return PumpPipelineLatencyUs(P); }

/* Increments at every stream start and stop, after each flushed the ring. Read with a
 * barrier: the caller reads it before the latency (review round 5 #4). */
ULONG PumpLatencyEpoch(_In_ PPN_PUMP P) { return (ULONG)InterlockedCompareExchange(&P->LatencyEpoch, 0, 0); }

BOOLEAN PumpMediaStalled(_In_ PPN_PUMP P, _In_ ULONG ThresholdMs)
{
    if (!P->StreamActive) return FALSE;
    KIRQL irql;
    KeAcquireSpinLock(&P->TxLock, &irql);
    BOOLEAN stalled = PnTxLedgerStalled(&P->TxLedger, PnNowHns(), ThresholdMs);
    KeReleaseSpinLock(&P->TxLock, irql);
    return stalled;
}
