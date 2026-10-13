/*
 * codechost.h - bridge to the user-mode codec host (inverted call model).
 *
 * The host service opens the device's control interface, registers its codec mask and
 * keeps several IOCTL_PNA2DP_CODEC_GET_WORK requests pended. When the pump has PCM to
 * encode it completes one of those requests with a PNA2DP_CODEC_WORK block. The host
 * encodes and sends IOCTL_PNA2DP_CODEC_PUT_RESULT. Cancel-safe queues (IoCsq) hold the
 * pended requests so that process death or cancellation is always handled.
 *
 * Failures never disable the host permanently: a run of consecutive failures puts it in
 * a quarantine (PN_HOST_HEALTH, audio_policy.h) that expires on a timer, after which the
 * device re-evaluates its codecs.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

typedef struct _PN_CODEC_HOST {
    PPNA2DP_DEVICE      Device;
    KSPIN_LOCK          Lock;
    BOOLEAN             Registered;
    BOOLEAN             Stopped;          /* CodecHostUninitialize ran: no more timers */
    PN_HOST_HEALTH      Health;           /* failure run / quarantine, under Lock */
    KTIMER              QuarantineTimer;  /* fires when a quarantine ends */
    KDPC                QuarantineDpc;
    PFILE_OBJECT        OwnerFile;        /* handle that registered; unregister on close */
    PNA2DP_CODEC_REGISTER Info;
    IO_CSQ              WorkCsq;          /* pended GET_WORK IRPs */
    KSPIN_LOCK          WorkCsqLock;
    LIST_ENTRY          WorkQueue;
    LONG                PendedCount;
    LONG                Generation;
} PN_CODEC_HOST, *PPN_CODEC_HOST;

NTSTATUS CodecHostInitialize(_Out_ PPN_CODEC_HOST H, _In_ PPNA2DP_DEVICE Device);
VOID     CodecHostUninitialize(_Inout_ PPN_CODEC_HOST H);

NTSTATUS CodecHostRegister(_Inout_ PPN_CODEC_HOST H, _In_ PFILE_OBJECT File, _In_ const PNA2DP_CODEC_REGISTER *Info);
VOID     CodecHostUnregister(_Inout_ PPN_CODEC_HOST H, _In_opt_ PFILE_OBJECT File);  /* NULL = force */
BOOLEAN  CodecHostAvailable(_In_ PPN_CODEC_HOST H, _In_ ULONG Codec);
ULONG    CodecHostCodecMask(_In_ PPN_CODEC_HOST H);
/* One failed block (bad/invalid result or unanswered request). A run of them quarantines
 * the host and calls DeviceHostChanged. <= DISPATCH. */
VOID     CodecHostReportFailure(_Inout_ PPN_CODEC_HOST H);
/* A good result: ends a failure run. <= DISPATCH. */
VOID     CodecHostReportSuccess(_Inout_ PPN_CODEC_HOST H);

/* IRP plumbing (called from control.cpp) */
NTSTATUS CodecHostQueueGetWork(_Inout_ PPN_CODEC_HOST H, _In_ PIRP Irp);   /* returns STATUS_PENDING */
VOID     CodecHostFlushFile(_Inout_ PPN_CODEC_HOST H, _In_ PFILE_OBJECT File);

/* Hand a work item to the host. Copies Header + Pcm into a pended IRP's buffer.
 * Returns STATUS_DEVICE_NOT_READY if no request is pended. DISPATCH ok. */
NTSTATUS CodecHostDispatchWork(_Inout_ PPN_CODEC_HOST H, _In_ const PNA2DP_CODEC_WORK *Header,
                               _In_reads_bytes_opt_(PcmBytes) const UCHAR *Pcm, _In_ ULONG PcmBytes);

/* Zero-copy variant: take the oldest pended request first (NULL if none; nothing is
 * consumed), write PCM straight into *Pcm (up to *PcmCapacity bytes), then always
 * finish it with CodecHostCompleteWork. DISPATCH ok. */
PIRP     CodecHostAcquireWork(_Inout_ PPN_CODEC_HOST H, _Outptr_result_maybenull_ PUCHAR *Pcm, _Out_ ULONG *PcmCapacity);
VOID     CodecHostCompleteWork(_Inout_ PPN_CODEC_HOST H, _In_ PIRP Irp, _In_ const PNA2DP_CODEC_WORK *Header, _In_ ULONG PcmBytes);
