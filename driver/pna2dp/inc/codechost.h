/*
 * codechost.h - bridge to the user-mode codec host (inverted call model).
 *
 * The host service opens the device's control interface, registers its codec mask and
 * keeps several IOCTL_PNA2DP_CODEC_GET_WORK requests pended. When the pump has PCM to
 * encode it completes one of those requests with a PNA2DP_CODEC_WORK block. The host
 * encodes and sends IOCTL_PNA2DP_CODEC_PUT_RESULT. Cancel-safe queues (IoCsq) hold the
 * pended requests so that process death or cancellation is always handled.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

typedef struct _PN_CODEC_HOST {
    PPNA2DP_DEVICE      Device;
    KSPIN_LOCK          Lock;
    BOOLEAN             Registered;
    LONG                Failed;           /* unusable until the service re-registers */
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
VOID     CodecHostReportFailure(_Inout_ PPN_CODEC_HOST H);

/* IRP plumbing (called from control.cpp) */
NTSTATUS CodecHostQueueGetWork(_Inout_ PPN_CODEC_HOST H, _In_ PIRP Irp);   /* returns STATUS_PENDING */
VOID     CodecHostFlushFile(_Inout_ PPN_CODEC_HOST H, _In_ PFILE_OBJECT File);

/* Hand a work item to the host. Copies Header + Pcm into a pended IRP's buffer.
 * Returns STATUS_DEVICE_NOT_READY if no request is pended. DISPATCH ok. */
NTSTATUS CodecHostDispatchWork(_Inout_ PPN_CODEC_HOST H, _In_ const PNA2DP_CODEC_WORK *Header,
                               _In_reads_bytes_opt_(PcmBytes) const UCHAR *Pcm, _In_ ULONG PcmBytes);
